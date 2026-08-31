#include "app/selftest/Support.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "app/OpenAnyFile.hpp"
#include "color/Space.hpp"
#include "core/Mask.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/ImageIO.hpp"
#include "io/PsdImport.hpp"

// io/PsdImport (and the app/OpenAnyFile seam it fills) -- headless, GPU-free.
// Every fixture below is written by a matching PSD *writer* built in this
// file, so a pass proves this reader agrees with this author's reading of
// the published specification. **That is still all it proves**, and it is
// worth being clear about what does the other half of the job: three
// genuine Photoshop-authored documents have since been read by this module
// and by an independent third-party reader and compared layer by layer, and
// io/PsdImport.hpp's own header records that comparison in full. Those
// files are the user's artwork, are not in this repository, and cannot be,
// so they are not reachable from `--selftest` -- the flag that reads them
// is `--psd-report` (app/PsdReport.hpp), run by hand against a file someone
// has. This suite and that flag prove different things and neither replaces
// the other.
//
// The section split mirrors io/PsdImport.hpp's own "Scope" and "What has
// NOT been verified" sections:
//
//   A. A four-layer happy path: raw and RLE compression, 8-bit depth,
//      offset (partly off-canvas, negative-origin) layer bounds, opacity,
//      the hidden-layer flag bit, clipping, a Unicode name preferred over
//      its Pascal fallback, exact and unmapped blend-mode keys.
//   B. 16-bit depth.
//   C. Deliberately malformed input: a length that runs past the actual
//      file, a mid-channel truncation, a zero layer count, an inverted
//      rectangle, ZIP compression, PSB, an unsupported colour mode and an
//      unsupported depth -- each asserted to REFUSE cleanly rather than
//      read out of bounds or produce a document.
//   D. app/OpenAnyFile's seam: a layered PSD opens through io/PsdImport, a
//      flat one (no layer section) falls back to the pre-existing
//      flattened path unchanged, and a PSD with real but unreadable layer
//      content (ZIP) is refused outright rather than silently flattened.
//   E. Transparent regions cost no tiles -- the guard on the empty-tile
//      skip a real 180 MB Photoshop file made necessary.
//   F. Layer groups ('lsct'): a flat group, a nested one (fixture-only --
//      no sample file exercises depth > 0), a non-'pass' group blend key
//      (imports anyway, warns by name), and both directions of an
//      unbalanced divider/header stack (total refusal, neither repaired).
namespace np {
namespace {

// --- A tiny byte writer, in DescFixture's mould (app/selftest/
// DescFixture.hpp) but for PSD's own big-endian primitives rather than
// Action Descriptor ones. Not shared with DescFixture: that header is
// scoped to io/Descriptor's wire format (Keys, osTypes, ...), and PSD's
// framing shares nothing with it below the "big-endian" level. -------------
struct ByteWriter {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v & 0xFFu)); }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
  void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void str4(const char* s) {
    for (int i = 0; i < 4; ++i) u8(static_cast<unsigned char>(s[i]));
  }
  void bytes(const std::vector<uint8_t>& v) { b.insert(b.end(), v.begin(), v.end()); }
};

// Minimal UTF-8 -> UTF-16 (BMP + surrogate pairs), for building a `luni`
// fixture's payload. Test-only: io/PsdImport.cpp decodes the other
// direction, independently, and this function existing on both sides of a
// fixture would be exactly the kind of shared-bug risk this file's own
// header disclaims -- so it is instead a completely separate, much
// simpler routine (no error repair, no U+FFFD path) rather than a reuse of
// anything in io/PsdImport.cpp.
std::u16string utf8ToUtf16(const std::string& s) {
  std::u16string out;
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0;
    int len = 1;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
    else { cp = c & 0x07u; len = 4; }
    for (int k = 1; k < len && i + static_cast<size_t>(k) < s.size(); ++k)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3Fu);
    i += static_cast<size_t>(len);
    if (cp < 0x10000) {
      out.push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
  }
  return out;
}

// A PackBits ENCODER -- the writer's half of the algorithm io/PsdImport.cpp's
// `decodePackBits()` implements the reader's half of. Greedy: a run of 3 or
// more identical bytes is run-length-encoded, everything else is emitted as
// literal packets -- which is what exercises BOTH of PackBits' two packet
// kinds (a solid-fill test channel below produces one large run; a varied
// one produces literals), rather than only the literal path a lazier
// "chunk everything into literals" encoder would leave untested.
std::vector<uint8_t> packBitsEncodeRow(const std::vector<uint8_t>& row) {
  std::vector<uint8_t> out;
  size_t i = 0;
  while (i < row.size()) {
    size_t runLen = 1;
    while (i + runLen < row.size() && row[i + runLen] == row[i] && runLen < 128) ++runLen;
    if (runLen >= 3) {
      out.push_back(static_cast<uint8_t>(static_cast<int8_t>(-static_cast<int>(runLen - 1))));
      out.push_back(row[i]);
      i += runLen;
    } else {
      const size_t litStart = i;
      size_t litLen = 0;
      while (i < row.size() && litLen < 128) {
        size_t rl = 1;
        while (i + rl < row.size() && row[i + rl] == row[i] && rl < 3) ++rl;
        if (rl >= 3) break;
        ++i;
        ++litLen;
      }
      out.push_back(static_cast<uint8_t>(litLen - 1));
      for (size_t k = 0; k < litLen; ++k) out.push_back(row[litStart + k]);
    }
  }
  return out;
}

// One channel's worth of raw samples (one uint32 per pixel, row-major, in
// the sample's own [0, 2^depth) range), encoded exactly as PSD's "Channel
// image data" table describes: a 2-byte compression code, then the bytes.
// `compression` 2 writes a compression code of 2 (ZIP) with a few
// deliberately-arbitrary payload bytes after it -- io/PsdImport.cpp refuses
// on the compression code alone, so what follows it is never read and its
// content does not matter.
std::vector<uint8_t> encodeChannel(const std::vector<uint32_t>& samples, uint32_t width,
                                   uint32_t height, int bytesPerSample, int compression) {
  ByteWriter w;
  w.u16(static_cast<uint32_t>(compression));
  if (compression == 2) {
    for (int i = 0; i < 8; ++i) w.u8(0xAB);
    return w.b;
  }
  std::vector<uint8_t> raw;
  raw.reserve(samples.size() * static_cast<size_t>(bytesPerSample));
  for (uint32_t v : samples) {
    if (bytesPerSample == 2) { raw.push_back(static_cast<uint8_t>(v >> 8)); raw.push_back(static_cast<uint8_t>(v)); }
    else raw.push_back(static_cast<uint8_t>(v));
  }
  if (compression == 0) {
    w.bytes(raw);
    return w.b;
  }
  // RLE: `height` big-endian u16 row-lengths, then the concatenated
  // per-row PackBits streams.
  const size_t rowBytes = static_cast<size_t>(width) * static_cast<size_t>(bytesPerSample);
  std::vector<std::vector<uint8_t>> encRows;
  encRows.reserve(height);
  for (uint32_t y = 0; y < height; ++y) {
    std::vector<uint8_t> row(raw.begin() + static_cast<long>(y * rowBytes),
                             raw.begin() + static_cast<long>((y + 1) * rowBytes));
    encRows.push_back(packBitsEncodeRow(row));
  }
  for (auto& r : encRows) w.u16(static_cast<uint32_t>(r.size()));
  for (auto& r : encRows) w.bytes(r);
  return w.b;
}

struct ChannelSpec {
  int16_t id = 0;
  std::vector<uint32_t> samples;
};

// One `lsct` ("Section divider setting") additional-layer-info block --
// docs/psd-import-gaps.md section 3's wire layout: a divider record
// (`type = 3`, len 4) writes only `type`; a header record (`type` 1 or 2)
// optionally follows it with `8BIM` + a 4-byte blend key (`hasBlendKey`,
// len >= 12) and optionally a u32 sub-type past that (`hasSubType`, len >=
// 16) -- this fixture writer supports the sub-type field for shape-fidelity
// with a real file even though io/PsdImport.cpp never reads it.
struct LsctSpec {
  uint32_t type = 0;
  bool hasBlendKey = false;
  std::string blendKey = "pass";
  bool hasSubType = false;
  uint32_t subType = 0;
};

// The 20-byte layer mask record (docs/psd-import-gaps.md section 1), the
// fixture-builder's mirror of io/PsdImport.cpp's own `ParsedLayer` mask
// fields. `overrideSize`, when set, writes a DIFFERENT size field than the
// natural 20 -- used by the one fixture that checks the "size > 20 is
// refused by name" path, the same `layerCountOverride` trick `buildPsd()`
// already uses for its own "declares a truthful-looking but wrong count"
// fixture.
struct MaskSpec {
  int32_t top = 0, left = 0, bottom = 0, right = 0;
  uint8_t defaultColor = 255;
  uint8_t flags = 0;
  std::optional<uint32_t> overrideSize;
};

struct LayerSpec {
  int32_t top = 0, left = 0, bottom = 0, right = 0;
  std::string blendKey = "norm";
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  bool hidden = false;
  std::string pascalName;
  std::optional<std::string> uniName;
  std::optional<uint32_t> lspfFlags;  // unset = no `lspf` block at all
  int compression = 0;
  std::vector<ChannelSpec> channels;
  std::optional<LsctSpec> lsct;
  std::optional<MaskSpec> mask;
};

// Builds a whole PSD file's bytes from a list of `LayerSpec`s, following
// Adobe's published layout exactly (io/PsdImport.hpp's header cites the
// source): header, empty Color Mode Data, empty Image Resources, then the
// Layer and Mask Information section this function's own comments derive
// field by field. `layerCountOverride`, when set, writes a DIFFERENT count
// than `layers.size()` -- used by section C's "zero layers" fixture, which
// needs a layer info section that is syntactically present but declares no
// layers, a shape `layers.empty()` alone cannot produce (an empty list
// still writes a truthful zero, which is exactly what that fixture wants,
// so in practice this parameter is unused today and kept only because the
// zero-layers fixture is clearer written as "override the count of a
// nonempty spec list to zero" than as a separate near-duplicate builder).
std::vector<uint8_t> buildPsd(uint32_t width, uint32_t height, uint16_t depth,
                              const std::vector<LayerSpec>& layers, uint16_t version = 1,
                              uint16_t colorMode = 3, bool omitLayerSection = false,
                              std::optional<int16_t> layerCountOverride = std::nullopt) {
  ByteWriter w;
  w.str4("8BPS");
  w.u16(version);
  for (int i = 0; i < 6; ++i) w.u8(0);
  w.u16(4);  // header channel count -- informational only, within [1,56]
  w.u32(height);
  w.u32(width);
  w.u16(depth);
  w.u16(colorMode);
  w.u32(0);  // Color Mode Data length
  w.u32(0);  // Image Resources length

  if (omitLayerSection) {
    w.u32(0);  // Layer and Mask Information length: none
    return w.b;
  }

  const int bytesPerSample = depth == 16 ? 2 : 1;

  struct Built {
    const LayerSpec* spec = nullptr;
    std::vector<std::pair<int16_t, std::vector<uint8_t>>> channelBytes;
    std::vector<uint8_t> extraData;
  };
  std::vector<Built> built;
  built.reserve(layers.size());
  for (const LayerSpec& L : layers) {
    Built bl;
    bl.spec = &L;
    const uint32_t lw = static_cast<uint32_t>(L.right - L.left);
    const uint32_t lh = static_cast<uint32_t>(L.bottom - L.top);
    for (const ChannelSpec& ch : L.channels)
      bl.channelBytes.push_back({ch.id, encodeChannel(ch.samples, lw, lh, bytesPerSample,
                                                       L.compression)});

    ByteWriter ew;
    if (L.mask) {
      ByteWriter mw;
      mw.i32(L.mask->top);
      mw.i32(L.mask->left);
      mw.i32(L.mask->bottom);
      mw.i32(L.mask->right);
      mw.u8(L.mask->defaultColor);
      mw.u8(L.mask->flags);
      mw.u16(0);  // padding -- present only in the plain 20-byte shape
      ew.u32(L.mask->overrideSize.value_or(static_cast<uint32_t>(mw.b.size())));
      ew.bytes(mw.b);
      // A non-default `overrideSize` deliberately does not resize `mw.b` to
      // match -- the ">20, refused by name" fixture wants a size field that
      // claims more than actually follows it, to prove the refusal comes
      // from the declared size alone (io/PsdImport.cpp reads that size and
      // refuses before it would ever try to read the extra bytes it claims).
    } else {
      ew.u32(0);  // layer mask data: none
    }
    ew.u32(0);  // layer blending ranges: none
    std::string pname = L.pascalName.substr(0, 255);
    ew.u8(static_cast<uint32_t>(pname.size()));
    for (unsigned char c : pname) ew.u8(c);
    const size_t consumed = 1 + pname.size();
    const size_t padded = (consumed + 3) & ~static_cast<size_t>(3);
    for (size_t i = consumed; i < padded; ++i) ew.u8(0);
    if (L.uniName) {
      std::u16string u16name = utf8ToUtf16(*L.uniName);
      u16name.push_back(0);  // Photoshop's own trailing-NUL convention
      ByteWriter payload;
      payload.u32(static_cast<uint32_t>(u16name.size()));
      for (char16_t c : u16name) payload.u16(static_cast<uint16_t>(c));
      ByteWriter tagged;
      tagged.str4("8BIM");
      tagged.str4("luni");
      tagged.u32(static_cast<uint32_t>(payload.b.size()));
      tagged.bytes(payload.b);
      ew.bytes(tagged.b);
    }
    if (L.lspfFlags) {
      ByteWriter tagged;
      tagged.str4("8BIM");
      tagged.str4("lspf");
      tagged.u32(4);
      tagged.u32(*L.lspfFlags);
      ew.bytes(tagged.b);
    }
    if (L.lsct) {
      ByteWriter payload;
      payload.u32(L.lsct->type);
      if (L.lsct->hasBlendKey) {
        payload.str4("8BIM");
        payload.str4(L.lsct->blendKey.c_str());
      }
      if (L.lsct->hasSubType) payload.u32(L.lsct->subType);
      ByteWriter tagged;
      tagged.str4("8BIM");
      tagged.str4("lsct");
      tagged.u32(static_cast<uint32_t>(payload.b.size()));
      tagged.bytes(payload.b);
      ew.bytes(tagged.b);
    }
    bl.extraData = ew.b;
    built.push_back(std::move(bl));
  }

  ByteWriter records;
  for (const Built& bl : built) {
    const LayerSpec& L = *bl.spec;
    records.i32(L.top);
    records.i32(L.left);
    records.i32(L.bottom);
    records.i32(L.right);
    records.u16(static_cast<uint32_t>(bl.channelBytes.size()));
    for (const auto& [id, bytes] : bl.channelBytes) {
      records.i16(id);
      records.u32(static_cast<uint32_t>(bytes.size()));
    }
    records.str4("8BIM");
    records.str4(L.blendKey.c_str());
    records.u8(L.opacity);
    records.u8(L.clipping);
    records.u8(L.hidden ? 0x02u : 0x00u);
    records.u8(0);  // filler
    records.u32(static_cast<uint32_t>(bl.extraData.size()));
    records.bytes(bl.extraData);
  }
  ByteWriter chdata;
  for (const Built& bl : built)
    for (const auto& [id, bytes] : bl.channelBytes) chdata.bytes(bytes);

  ByteWriter layerInfo;
  layerInfo.i16(layerCountOverride.value_or(static_cast<int16_t>(layers.size())));
  layerInfo.bytes(records.b);
  layerInfo.bytes(chdata.b);

  ByteWriter layerMaskInfo;
  layerMaskInfo.u32(static_cast<uint32_t>(layerInfo.b.size()));
  layerMaskInfo.bytes(layerInfo.b);
  layerMaskInfo.u32(0);  // Global layer mask info: none

  w.u32(static_cast<uint32_t>(layerMaskInfo.b.size()));
  w.bytes(layerMaskInfo.b);
  return w.b;
}

// A flat (layer-less) PSD carrying a real trailing "Image Data Section" --
// what a PSD saved with Maximize Compatibility off actually looks like, and
// the shape `PsdImportResult::noLayerData` exists to route around rather
// than refuse. Deliberately the same 26-byte-header / raw-planar-RGB shape
// app/selftest/FormatSupport.cpp's own flattened-PSD fixture already uses
// (that section's own comment cites the identical layout) -- not shared code
// with it (two small, independent builders, each provably matching the
// published layout on its own, rather than one the other could be silently
// bug-compatible with), but a deliberately IDENTICAL shape so that this
// fixture exercises the exact real-world case that section already proved
// `decodeImageLinear()` reads correctly.
std::vector<uint8_t> buildFlatPsd(uint32_t width, uint32_t height,
                                  const std::array<std::array<uint8_t, 3>, 4>& pixelsRgb) {
  ByteWriter w;
  w.str4("8BPS");
  w.u16(1);
  for (int i = 0; i < 6; ++i) w.u8(0);
  w.u16(3);
  w.u32(height);
  w.u32(width);
  w.u16(8);
  w.u16(3);
  w.u32(0);
  w.u32(0);
  w.u32(0);  // Layer and Mask Information length: none (the flat case)
  w.u16(0);  // Image Data Section compression: raw
  for (int c = 0; c < 3; ++c)
    for (const auto& px : pixelsRgb) w.u8(px[static_cast<size_t>(c)]);
  return w.b;
}

std::array<float, 4> pixelAt(const Layer& layer, int32_t x, int32_t y) {
  if (!layer.rgbTiles.has_value()) return {0.0f, 0.0f, 0.0f, 0.0f};
  const PixelCoord doc{x, y};
  const Tile* t = layer.rgbTiles->find(tileCoordAt(doc));
  if (!t) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(doc));
}

// The mask-store mirror of `pixelAt()` above, at document coordinates --
// `nullopt` mask reads as 1.0 (core/Mask.hpp: absent means reveal), and an
// engaged-but-unallocated tile reads as 1.0 too, through the same
// `maskCoverage()` leaf core/Composite's own walk goes through.
float maskCoverageAtDoc(const Layer& layer, int32_t x, int32_t y) {
  if (!layer.mask.has_value()) return 1.0f;
  const PixelCoord doc{x, y};
  return maskCoverage(layer.mask->find(tileCoordAt(doc)), tileLocalOffset(doc));
}

bool nearf(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}
bool anyContains(const std::vector<std::string>& hay, const std::string& needle) {
  for (const std::string& s : hay)
    if (contains(s, needle)) return true;
  return false;
}

}  // namespace

bool runPsdImportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  constexpr float kTol = 3e-3f;  // half-float storage + sRGB-decode rounding

  // ==========================================================================
  std::printf("  -- A. Four-layer happy path: raw + RLE, offset bounds, opacity, hidden,\n");
  std::printf("        clipping, luni-over-Pascal name, exact + unmapped blend keys --\n");
  // ==========================================================================
  {
    std::vector<LayerSpec> layers;

    LayerSpec bg;
    bg.top = 0; bg.left = 0; bg.bottom = 16; bg.right = 16;
    bg.blendKey = "norm";
    bg.opacity = 255;
    bg.clipping = 1;  // deliberately set on the BOTTOM layer -- must be forced false
    bg.pascalName = "Background";
    bg.compression = 0;  // raw
    bg.channels = {{0, std::vector<uint32_t>(16 * 16, 200)},
                   {1, std::vector<uint32_t>(16 * 16, 100)},
                   {2, std::vector<uint32_t>(16 * 16, 50)}};
    // No alpha channel at all -- exercises the "missing alpha defaults to
    // fully opaque" path, io/ImageDecode.cpp's own convention.
    layers.push_back(bg);

    LayerSpec mid;
    mid.top = 2; mid.left = -3; mid.bottom = 10; mid.right = 9;  // 12x8, partly off-canvas
    mid.blendKey = "mul ";
    mid.opacity = 128;
    mid.clipping = 1;  // NOT the bottom layer -- must come through as true
    mid.pascalName = "Middle";
    mid.compression = 1;  // RLE -- a solid fill run-length-encodes to one run per row
    const uint32_t midPixels = 12u * 8u;
    mid.channels = {{0, std::vector<uint32_t>(midPixels, 10)},
                    {1, std::vector<uint32_t>(midPixels, 10)},
                    {2, std::vector<uint32_t>(midPixels, 10)},
                    {-1, std::vector<uint32_t>(midPixels, 255)}};
    layers.push_back(mid);

    LayerSpec top;
    top.top = 0; top.left = 0; top.bottom = 16; top.right = 16;
    top.blendKey = "dark";
    top.opacity = 255;
    top.hidden = true;
    top.pascalName = "Cafe Layer (ascii fallback)";
    top.uniName = "Caf\xC3\xA9 Layer";  // "Café Layer", UTF-8 in this source file
    top.compression = 0;
    top.channels = {{0, std::vector<uint32_t>(16 * 16, 0)},
                    {1, std::vector<uint32_t>(16 * 16, 0)},
                    {2, std::vector<uint32_t>(16 * 16, 0)},
                    {-1, std::vector<uint32_t>(16 * 16, 128)}};
    layers.push_back(top);

    LayerSpec unmapped;
    unmapped.top = 0; unmapped.left = 0; unmapped.bottom = 2; unmapped.right = 2;
    // "diss" (Dissolve) -- permanently unmapped: a stochastic per-pixel
    // dither is incompatible with blendPixel()'s pure two-pixel-in,
    // one-pixel-out signature (docs/blend-mode-gaps.md's own "out of scope"
    // section), so this key stays the safe "unrecognised blend name" fixture
    // no matter how many more Photoshop modes later land. Was "sLit" (Soft
    // Light), then "diff" (Difference) -- both got real mappings below/on
    // another stage's branch and had to move.
    unmapped.blendKey = "diss";
    unmapped.pascalName = "Unmapped Blend";
    unmapped.compression = 0;
    unmapped.channels = {{0, std::vector<uint32_t>(4, 255)},
                         {1, std::vector<uint32_t>(4, 255)},
                         {2, std::vector<uint32_t>(4, 255)},
                         {-1, std::vector<uint32_t>(4, 255)}};
    layers.push_back(unmapped);

    const std::vector<uint8_t> bytes = buildPsd(16, 16, 8, layers);
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));

    check(r.ok, "A: a 4-layer, mixed-compression PSD parses (ok)");
    check(r.document.layers.size() == 4, "A: exactly 4 layers arrived, matching the file");

    if (r.ok && r.document.layers.size() == 4) {
      const Layer& l0 = r.document.layers[0];
      const Layer& l1 = r.document.layers[1];
      const Layer& l2 = r.document.layers[2];
      const Layer& l3 = r.document.layers[3];

      // Stacking order: file-order 0 is doc.layers[0], the BOTTOM of the
      // stack, per io/PsdImport.hpp's own cited evidence -- checked here by
      // name, not merely by position, so a silent reversal shows up as a
      // wrong layer's name rather than as a coincidentally-still-passing
      // index.
      check(l0.name == "Background" && l1.name == "Middle" &&
                (l2.name == "Caf\xC3\xA9 Layer") && l3.name == "Unmapped Blend",
            "A: layer order is file order (index 0 = bottom), by name, not reversed");

      check(l2.name == "Caf\xC3\xA9 Layer",
            "A: the Unicode `luni` name is preferred over the Pascal fallback name");

      const auto bgPixel = pixelAt(l0, 5, 5);
      check(nearf(bgPixel[0], srgbDecode(200.0f / 255.0f), kTol) &&
                nearf(bgPixel[1], srgbDecode(100.0f / 255.0f), kTol) &&
                nearf(bgPixel[2], srgbDecode(50.0f / 255.0f), kTol) && nearf(bgPixel[3], 1.0f, kTol),
            "A: Background (raw, no alpha channel) decodes sRGB->linear and defaults to opaque");

      // (0,2) in document space is local (3,0) inside Middle's 12x8
      // footprint (left = -3), so this is the offset/negative-origin
      // placement fixture, not merely the RLE decode.
      const auto midPixel = pixelAt(l1, 0, 2);
      const float midChan = srgbDecode(10.0f / 255.0f);
      check(nearf(midPixel[0], midChan, kTol) && nearf(midPixel[1], midChan, kTol) &&
                nearf(midPixel[2], midChan, kTol) && nearf(midPixel[3], 1.0f, kTol),
            "A: Middle (RLE, negative-left offset rect) lands at the right document coordinate");
      check(nearf(l1.opacity, 128.0f / 255.0f, 1e-4f), "A: Middle's opacity byte converts to [0,1]");
      check(l1.clipped, "A: Middle's clipping byte (non-bottom layer) becomes Layer::clipped");
      check(!l0.clipped,
            "A: Background's clipping byte is IGNORED -- the bottom layer can never be clipped "
            "(core/Layer.hpp's own invariant), even though the fixture set the byte");

      const auto topPixel = pixelAt(l2, 5, 5);
      check(nearf(topPixel[3], 128.0f / 255.0f, kTol),
            "A: Cafe layer's own alpha channel (distinct from opacity) is read and premultiplies "
            "its (all-zero) colour");
      check(!l2.visible,
            "A: flags bit 1 SET means HIDDEN (the inverted reading io/PsdImport.hpp's header "
            "argues for), not the naive 'bit 1 = visible' reading of the spec's own table");
      check(l0.visible && l1.visible && l3.visible, "A: every other layer's flags bit was clear, "
                                                     "and all three import visible");

      check(l0.blend == "normal", "A: 'norm' maps to core::BlendMode::Normal");
      check(l1.blend == "multiply", "A: 'mul ' maps to core::BlendMode::Multiply");
      check(l2.blend == "min",
            "A: 'dark' (Darken) maps to core::BlendMode::Min -- an exact per-channel minimum, "
            "not an approximation");
      check(l3.blend == "normal" && anyContains(r.warnings, "diss"),
            "A: 'diss' (Dissolve, permanently out of scope) imports as Normal AND is named in "
            "a warning");
    }

    // Stage 2 (docs/blend-mode-gaps.md): 'sLit' now has a real equivalent --
    // a one-layer fixture, imported and checked on its own, rather than
    // folded into the four-layer one above.
    {
      LayerSpec soft;
      soft.top = 0; soft.left = 0; soft.bottom = 2; soft.right = 2;
      soft.blendKey = "sLit";
      soft.pascalName = "Soft Light Layer";
      soft.compression = 0;
      soft.channels = {{0, std::vector<uint32_t>(4, 255)},
                       {1, std::vector<uint32_t>(4, 255)},
                       {2, std::vector<uint32_t>(4, 255)},
                       {-1, std::vector<uint32_t>(4, 255)}};
      const std::vector<uint8_t> softBytes = buildPsd(2, 2, 8, {soft});
      const PsdImportResult softR =
          importPsd(std::span<const uint8_t>(softBytes.data(), softBytes.size()));
      check(softR.ok && softR.document.layers.size() == 1 &&
                softR.document.layers[0].blend == "soft-light" && softR.warnings.empty(),
            "A: 'sLit' now maps to core::BlendMode::SoftLight (Stage 2) with no warning -- an "
            "approximation in substance (linear- vs gamma-space blending) but not the 'no "
            "equivalent' case, so mapBlendKey() rightly reports it exact");
    }
  }

  // ==========================================================================
  std::printf("  -- B. 16-bit depth --\n");
  // ==========================================================================
  {
    LayerSpec l;
    l.top = 0; l.left = 0; l.bottom = 2; l.right = 2;
    l.pascalName = "Sixteen";
    l.compression = 0;
    l.channels = {{0, std::vector<uint32_t>(4, 0x8000)},
                 {1, std::vector<uint32_t>(4, 0xFFFF)},
                 {2, std::vector<uint32_t>(4, 0x0000)},
                 {-1, std::vector<uint32_t>(4, 0xFFFF)}};
    const std::vector<uint8_t> bytes = buildPsd(2, 2, 16, {l});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(r.ok && r.document.layers.size() == 1, "B: a 16-bit PSD parses to one layer");
    if (r.ok && !r.document.layers.empty()) {
      const auto px = pixelAt(r.document.layers[0], 0, 0);
      check(nearf(px[0], srgbDecode(0x8000 / 65535.0f), kTol) &&
                nearf(px[1], srgbDecode(1.0f), kTol) && nearf(px[2], srgbDecode(0.0f), kTol),
            "B: 16-bit big-endian samples decode against the 65535 full-scale, not 255");
    }
  }

  // ==========================================================================
  std::printf("  -- C. Deliberately malformed input: refuse cleanly, never read out of\n");
  std::printf("        bounds and never crash --\n");
  // ==========================================================================
  {
    LayerSpec good;
    good.top = 0; good.left = 0; good.bottom = 4; good.right = 4;
    good.pascalName = "L";
    good.channels = {{0, std::vector<uint32_t>(16, 1)}};

    // C1: a channel-data length field claiming far more than the file
    // actually holds. Derived, not guessed: for THIS single-layer,
    // single-channel, raw fixture the length field sits at a fixed offset
    // -- 26 (header) + 4 (Color Mode Data length) + 4 (Image Resources
    // length) + 4 (Layer&Mask Info length) + 4 (Layer info length) + 2
    // (layer count) + 16 (rect) + 2 (numChannels) + 2 (this channel's id)
    // = byte 64, four bytes. Verified below by reading the ORIGINAL value
    // back before overwriting it, so a change to this file's own builder
    // layout fails this assertion loudly rather than silently patching the
    // wrong field.
    {
      std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {good});
      constexpr size_t kLenOffset = 64;
      const uint32_t original = (static_cast<uint32_t>(bytes[kLenOffset]) << 24) |
                                (static_cast<uint32_t>(bytes[kLenOffset + 1]) << 16) |
                                (static_cast<uint32_t>(bytes[kLenOffset + 2]) << 8) |
                                static_cast<uint32_t>(bytes[kLenOffset + 3]);
      check(original == 2 + 16, "C1 setup: the channel-length field is where this fixture's own "
                                "layout says it is (guards this whole assertion against drift)");
      bytes[kLenOffset] = 0xFF; bytes[kLenOffset + 1] = 0xFF;
      bytes[kLenOffset + 2] = 0xFF; bytes[kLenOffset + 3] = 0xF0;
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && !r.error.empty(),
            "C1: a channel length claiming far more than the file holds refuses cleanly");
    }

    // C2: truncated mid-channel -- chop bytes off the end of an otherwise
    // valid file, landing inside the last channel's own data.
    {
      std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {good});
      bytes.resize(bytes.size() - 5);
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData, "C2: a file truncated mid-channel refuses cleanly");
    }

    // C3: a layer count of zero -- syntactically valid, semantically "no
    // layers", and distinguished from every OTHER refusal by `noLayerData`
    // so app/OpenAnyFile.cpp can fall back rather than refuse the file.
    {
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && r.noLayerData, "C3: a declared layer count of zero sets noLayerData, not a "
                                    "generic refusal");
    }

    // C4: an inverted rectangle.
    {
      LayerSpec bad = good;
      bad.bottom = bad.top - 5;  // inverted: bottom < top
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {bad});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "inverted"),
            "C4: an inverted layer rectangle (bottom < top) refuses cleanly, named as such");
    }

    // C5: ZIP compression -- refused by name, never decoded into garbage.
    {
      LayerSpec zip = good;
      zip.compression = 2;
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {zip});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "ZIP") && contains(r.error, "zlib"),
            "C5: ZIP-compressed channel data is refused by name, not silently decoded");
    }

    // C6: PSB (version 2) -- refused by name, never misparsed as PSD.
    {
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {good}, /*version=*/2);
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "PSB"),
            "C6: a PSB (version 2) file is refused by name rather than misparsed");
    }

    // C7: an unsupported colour mode (CMYK).
    {
      const std::vector<uint8_t> bytes =
          buildPsd(4, 4, 8, {good}, /*version=*/1, /*colorMode=*/4);
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "CMYK"),
            "C7: CMYK colour mode is refused by name, no conversion guessed");
    }

    // C8: an unsupported depth (32-bit float).
    {
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 32, {good});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "32"),
            "C8: 32-bit depth is refused by name");
    }

    // C9: not a PSD at all.
    {
      const std::vector<uint8_t> bytes = {'G', 'I', 'F', '8', '9', 'a'};
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData, "C9: bytes with no '8BPS' signature refuse cleanly");
    }

    // C10: an empty layer (zero-area rectangle) -- a legitimate PSD state,
    // not a malformed one, checked here because it shares this section's
    // "does this crash or misbehave" concern (an empty pixel buffer, a
    // zero-height PackBits table) even though it is not itself adversarial.
    {
      LayerSpec empty;
      empty.top = 5; empty.left = 5; empty.bottom = 5; empty.right = 5;
      empty.pascalName = "Empty";
      const std::vector<uint8_t> bytes = buildPsd(8, 8, 8, {good, empty});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 2 && r.document.layers[1].name == "Empty",
            "C10: a zero-area layer imports without crashing, alongside an ordinary one");
    }
  }

  // ==========================================================================
  std::printf("  -- D. app/OpenAnyFile's seam: layered PSD, flat-PSD fallback, and a\n");
  std::printf("        genuine refusal that does NOT silently flatten --\n");
  // ==========================================================================
  {
    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "np-selftest-psdimport";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    auto writeFile = [&](const std::string& name, const std::vector<uint8_t>& bytes) {
      const std::filesystem::path p = scratch / name;
      std::ofstream f(p, std::ios::binary);
      f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      f.close();
      return p.string();
    };

    // D1: a layered PSD opens with N layers, through io/PsdImport.
    {
      LayerSpec a, b;
      a.top = 0; a.left = 0; a.bottom = 4; a.right = 4; a.pascalName = "A";
      a.channels = {{0, std::vector<uint32_t>(16, 10)}};
      b.top = 0; b.left = 0; b.bottom = 4; b.right = 4; b.pascalName = "B";
      b.channels = {{0, std::vector<uint32_t>(16, 20)}};
      const std::string path = writeFile("layered.psd", buildPsd(4, 4, 8, {a, b}));
      const OpenAnyResult r = openAnyFileAsDocument(path);
      check(r.ok && r.document.document.layers.size() == 2,
            "D1: File > Open on a layered PSD produces a 2-layer document");
      check(r.ok && contains(r.status, "2 layers"),
            "D1: the status line names the layer count");
    }

    // D2: a flat PSD (no layer section, a real trailing composite) still
    // opens -- the pre-existing flattened path, completely unchanged, for
    // the one case io/PsdImport declines on purpose.
    {
      const std::array<std::array<uint8_t, 3>, 4> px = {{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {10, 20, 30}}};
      const std::string path = writeFile("flat.psd", buildFlatPsd(2, 2, px));
      const OpenAnyResult r = openAnyFileAsDocument(path);
      check(r.ok && r.document.document.layers.size() == 1,
            "D2: a flat PSD (no layer info) still opens, through the pre-existing flattened path");
    }

    // D3: a PSD that DOES carry real layer data, in a shape this build
    // cannot read (ZIP) -- refused outright, NOT silently opened flattened.
    // This is the one behavioural distinction that matters most: D2's
    // success and this refusal must not be confusable from the caller's
    // side.
    {
      LayerSpec zip;
      zip.top = 0; zip.left = 0; zip.bottom = 2; zip.right = 2;
      zip.compression = 2;
      zip.channels = {{0, {1, 2, 3, 4}}};
      const std::string path = writeFile("unreadable.psd", buildPsd(2, 2, 8, {zip}));
      const OpenAnyResult r = openAnyFileAsDocument(path);
      check(!r.ok, "D3: a PSD with real but unreadable (ZIP) layer data is refused outright");
      check(!r.ok && contains(r.status, "ZIP"),
            "D3: the refusal names the reason, not a generic 'damaged file' sentence");
    }

    // D3-control: is D3's `!r.ok` assertion actually sensitive to
    // app/OpenAnyFile.cpp's "refuse outright, don't fall back" branch, or
    // would it stay green even if that branch were deleted? A sabotage run
    // (forcing that branch to fall through to the flattened path instead of
    // refusing) answered this, and the answer has a real nuance worth
    // recording rather than silently accepting or silently "fixing" with a
    // fixture that turned out not to fix anything -- see below.
    //
    // First attempt: give D3's fixture a VALID trailing composite (raw RGB,
    // buildFlatPsd's own proven shape) after its ZIP layer, on the theory
    // that a real Photoshop "Maximize Compatibility" file has exactly this
    // shape, so THAT is the fixture that would expose a silent-flatten
    // regression. Rebuilding with the sabotage in place and this richer
    // fixture, `!r.ok` **still stayed green** -- the sabotage bypasses
    // io/PsdImport's refusal fine, but the flattened fallback (this build's
    // stb_image is compiled STBI_ONLY_PNG/JPEG/BMP/TGA -- paint/Palette.cpp's
    // own comment says why -- so PSD never reaches stb at all; every PSD
    // flatten in this build is OpenImageIO's PSD reader or nothing) ALSO
    // declined the file, independently of the sabotage, for an unrelated
    // reason: confirmed directly below, OpenImageIO's own linked PSD reader
    // refuses a non-empty layer section containing a ZIP-compressed channel
    // outright, even though only the composite subimage was ever requested
    // -- almost certainly the same "no zlib" limitation io/PsdImport.cpp
    // itself has, since OIIO's PSD reader evidently walks the full layer
    // list structurally before any subimage can be read. A RAW-compressed
    // control variant of the identical shape, run through the same
    // fallback call, succeeds -- ruling out "OIIO can't flatten ANY
    // non-empty layer section" as the explanation, and pinning it on ZIP
    // specifically.
    //
    // So: in *this* build, there is no fixture where "io/PsdImport refuses
    // for ZIP" and "the fallback would have silently succeeded" are both
    // true at once -- both readers agree ZIP is unreadable, which is a
    // genuine (if narrow) mitigation, not a gap this file should paper over
    // with a fixture engineered to look like it proves something it does
    // not. D3's `!r.ok` therefore is NOT sabotage-discriminating on its own
    // for this specific refusal reason in this specific build (only the
    // message-content assertion is); it still has real value as a basic
    // "the branch exists at all" check, and as the one place a REGRESSION
    // in OIIO's own ZIP handling (should this project's OpenImageIO ever
    // gain zlib) would surface as a behaviour change worth re-examining.
    // The two checks below make that reasoning falsifiable by running it,
    // rather than trusting this comment: D3-control 1 is the same claim
    // this comment makes about ZIP; D3-control 2 is the RAW-compression
    // control that rules out the alternative explanation.
    {
      LayerSpec zip;
      zip.top = 0; zip.left = 0; zip.bottom = 2; zip.right = 2;
      zip.compression = 2;
      zip.channels = {{0, {1, 2, 3, 4}}};
      std::vector<uint8_t> bytes = buildPsd(2, 2, 8, {zip});
      // Header channel count sits at a fixed offset -- 4 ("8BPS") + 2
      // (version) + 6 (reserved) = byte 12, big-endian u16 -- patched from
      // buildPsd's own hardcoded 4 down to 3 to match the RGB-only (no
      // alpha) composite appended below, the same shape buildFlatPsd uses.
      constexpr size_t kHeaderChannelCountOffset = 12;
      bytes[kHeaderChannelCountOffset] = 0;
      bytes[kHeaderChannelCountOffset + 1] = 3;
      ByteWriter composite;
      composite.u16(0);  // Image Data Section compression: raw
      const std::array<std::array<uint8_t, 3>, 4> compositePx = {
          {{9, 8, 7}, {6, 5, 4}, {3, 2, 1}, {0, 1, 2}}};
      for (int c = 0; c < 3; ++c)
        for (const auto& px : compositePx) composite.u8(px[static_cast<size_t>(c)]);
      bytes.insert(bytes.end(), composite.b.begin(), composite.b.end());
      std::string err;
      const std::optional<Document> viaFallback =
          openImageAsDocument(bytes.data(), bytes.size(), &err);
      check(!viaFallback.has_value(),
            "D3-control 1: OpenImageIO's OWN linked PSD reader, asked directly to flatten a "
            "PSD with a non-empty ZIP-compressed layer section, ALSO declines it -- confirming "
            "there is no reachable 'silently succeeds if the refusal is skipped' outcome for "
            "THIS refusal reason in THIS build");
    }
    {
      LayerSpec raw;
      raw.top = 0; raw.left = 0; raw.bottom = 2; raw.right = 2;
      raw.compression = 0;
      raw.channels = {{0, {10, 20, 30, 40}}, {1, {10, 20, 30, 40}}, {2, {10, 20, 30, 40}}};
      std::vector<uint8_t> bytes = buildPsd(2, 2, 8, {raw});
      bytes[12] = 0; bytes[13] = 3;  // same header-channel-count patch as above
      ByteWriter composite;
      composite.u16(0);
      const std::array<std::array<uint8_t, 3>, 4> compositePx = {
          {{9, 8, 7}, {6, 5, 4}, {3, 2, 1}, {0, 1, 2}}};
      for (int c = 0; c < 3; ++c)
        for (const auto& px : compositePx) composite.u8(px[static_cast<size_t>(c)]);
      bytes.insert(bytes.end(), composite.b.begin(), composite.b.end());
      std::string err;
      const std::optional<Document> viaFallback =
          openImageAsDocument(bytes.data(), bytes.size(), &err);
      check(viaFallback.has_value() && viaFallback->layers.size() == 1,
            "D3-control 2: the identical shape with a RAW-compressed (not ZIP) layer section "
            "DOES flatten successfully through the same fallback call -- so control 1's refusal "
            "is specific to ZIP, not a general 'OIIO can't flatten any non-empty layer section' "
            "limitation that would have made control 1 vacuous");
    }

    std::filesystem::remove_all(scratch, ec);
  }

  // ==========================================================================
  std::printf("  -- E. Transparent regions cost no tiles --\n");
  // ==========================================================================
  //
  // The guard on io/PsdImport.cpp's `writeLayerPixelsAt()` skip. Measured on
  // a real Photoshop file before that skip existed: a 5000x2559 illustration
  // whose 53 layers were each stored at full canvas size took 6.2 GB
  // resident, 800 tiles per layer regardless of how little each one actually
  // drew -- twelve times this project's whole 512 MB budget (app/Memory),
  // for a 180 MB file.
  //
  // Both halves are asserted, and the second is the one that matters: it is
  // trivial to allocate nothing by dropping pixels, so a test that only
  // checked "fewer tiles" would pass on a reader that lost the picture.
  {
    // A 512x512 layer -- 4x4 = 16 tiles' worth of rectangle -- with a single
    // opaque pixel at (300, 300), which lands in exactly one of them.
    constexpr uint32_t kSide = 512;
    constexpr uint32_t kPx = 300;
    const size_t n = static_cast<size_t>(kSide) * kSide;
    std::vector<uint32_t> r(n, 0), g(n, 0), b(n, 0), a(n, 0);
    const size_t hit = static_cast<size_t>(kPx) * kSide + kPx;
    r[hit] = 200; g[hit] = 100; b[hit] = 50; a[hit] = 255;

    LayerSpec sparse;
    sparse.top = 0; sparse.left = 0;
    sparse.bottom = static_cast<int32_t>(kSide); sparse.right = static_cast<int32_t>(kSide);
    sparse.compression = 0;
    sparse.channels = {{-1, a}, {0, r}, {1, g}, {2, b}};

    const std::vector<uint8_t> bytes = buildPsd(kSide, kSide, 8, {sparse});
    const PsdImportResult res = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(res.ok && res.document.layers.size() == 1,
          "E1: a 512x512 layer holding one opaque pixel imports");
    if (res.ok && res.document.layers.size() == 1 &&
        res.document.layers[0].rgbTiles.has_value()) {
      const TileStore& tiles = *res.document.layers[0].rgbTiles;
      check(tiles.occupiedTileCount() == 1,
            "E2: it occupies exactly ONE tile, not the 16 its rectangle spans -- "
            "transparent samples allocate nothing");

      // The half that stops E2 being satisfiable by simply dropping pixels.
      const Tile* t = tiles.find(tileCoordAt(PixelCoord{static_cast<int32_t>(kPx),
                                                        static_cast<int32_t>(kPx)}));
      bool survived = false;
      if (t != nullptr) {
        const std::array<float, 4> px =
            t->readPixel(tileLocalOffset(PixelCoord{static_cast<int32_t>(kPx),
                                                    static_cast<int32_t>(kPx)}));
        survived = px[3] > 0.99f && std::fabs(px[0] - srgbDecode(200.0f / 255.0f)) < kTol &&
                   std::fabs(px[1] - srgbDecode(100.0f / 255.0f)) < kTol &&
                   std::fabs(px[2] - srgbDecode(50.0f / 255.0f)) < kTol;
      }
      check(survived,
            "E3: and that one pixel is still there, with its own colour -- so E2 was not "
            "bought by discarding content");
    } else {
      check(false, "E2/E3: the sparse layer produced no tile store at all");
    }
  }

  // ==========================================================================
  std::printf("  -- F. lspf layer protection flags: bit 0 -> alphaLocked --\n");
  // ==========================================================================
  //
  // docs/psd-import-gaps.md section 4. Bit 0 (transparency-locked) has a
  // direct home, Layer::alphaLocked -- io/PsdImport.cpp's own comment at the
  // `lspf` case explains why bits 1/2 (composite-/position-locked) are read
  // and then deliberately dropped rather than forced onto Layer::locked,
  // whose promise is broader than either. Two layers here: one with `lspf`
  // bit 0 set, one with no `lspf` block at all -- the second is the
  // anti-vacuity partner, proving the flag is actually read off the file
  // rather than defaulting true by accident.
  {
    LayerSpec locked;
    locked.top = 0; locked.left = 0; locked.bottom = 2; locked.right = 2;
    locked.pascalName = "Alpha Locked";
    locked.compression = 0;
    locked.lspfFlags = 0x00000001u;  // bit 0 set (bits 1/2 clear)
    locked.channels = {{0, std::vector<uint32_t>(4, 255)},
                       {1, std::vector<uint32_t>(4, 255)},
                       {2, std::vector<uint32_t>(4, 255)},
                       {-1, std::vector<uint32_t>(4, 255)}};

    LayerSpec unlocked;
    unlocked.top = 0; unlocked.left = 0; unlocked.bottom = 2; unlocked.right = 2;
    unlocked.pascalName = "No lspf Block";
    unlocked.compression = 0;
    // .lspfFlags left unset -- no `lspf` block is written for this layer.
    unlocked.channels = {{0, std::vector<uint32_t>(4, 255)},
                         {1, std::vector<uint32_t>(4, 255)},
                         {2, std::vector<uint32_t>(4, 255)},
                         {-1, std::vector<uint32_t>(4, 255)}};

    const std::vector<uint8_t> bytes = buildPsd(2, 2, 8, {locked, unlocked});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(r.ok && r.document.layers.size() == 2, "F0: a 2-layer PSD with one lspf block parses");
    if (r.ok && r.document.layers.size() == 2) {
      check(r.document.layers[0].alphaLocked,
            "F1: lspf bit 0 set -> Layer::alphaLocked is true");
      check(!r.document.layers[1].alphaLocked,
            "F2: no lspf block at all -> Layer::alphaLocked stays false "
            "(anti-vacuity: proves F1 is not just always true)");
    }
  }

  // ==========================================================================
  std::printf("  -- H. Raster layer masks (docs/psd-import-gaps.md section 1) --\n");
  // ==========================================================================
  //
  // Every mask channel below is RAW-compressed (compression 0) deliberately:
  // `encodeChannel()`'s RAW path serialises `samples` verbatim and ignores
  // the width/height it is passed, so a mask fixture's own sample count is
  // free to differ from its layer's -- exactly the shape this section needs
  // to exercise -- without this file's builder needing a second, mask-aware
  // encode path. RLE would chop rows using the wrong dimensions for a mask
  // channel whose size differs from its layer's, which is why it is not
  // used here.
  {
    // H1: a mask SMALLER than its layer and OFFSET from it, decoded at the
    // mask's own dimensions. This is THE fixture the "decode at
    // layerWidth x layerHeight" sabotage must catch: reusing the layer's
    // 8x8 for a channel whose declared length was encoded for the mask's
    // own 4x4 makes `decodeChannelData()`'s byte-count check fail outright,
    // so the whole import refuses rather than merely misplacing the mask.
    {
      LayerSpec l;
      l.top = 0; l.left = 0; l.bottom = 8; l.right = 8;
      l.pascalName = "H1";
      l.compression = 0;
      l.channels = {{0, std::vector<uint32_t>(64, 100)},
                    {1, std::vector<uint32_t>(64, 100)},
                    {2, std::vector<uint32_t>(64, 100)},
                    {-1, std::vector<uint32_t>(64, 255)},
                    {-2, std::vector<uint32_t>(16, 128)}};  // 4x4 mask, mid coverage
      l.mask = MaskSpec{/*top=*/2, /*left=*/2, /*bottom=*/6, /*right=*/6,
                        /*defaultColor=*/255, /*flags=*/0x00};
      const std::vector<uint8_t> bytes = buildPsd(8, 8, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 1 && r.document.layers[0].mask.has_value(),
            "H1: a masked layer parses and carries an engaged Layer::mask");
      if (r.ok && !r.document.layers.empty() && r.document.layers[0].mask.has_value()) {
        const Layer& layer = r.document.layers[0];
        check(nearf(maskCoverageAtDoc(layer, 3, 3), 128.0f / 255.0f, kTol),
              "H1: a point INSIDE the (smaller, offset) mask rect reads the mask's own decoded "
              "value -- the property that breaks if the mask is decoded at layerWidth x "
              "layerHeight instead of its own 4x4");
        check(nearf(maskCoverageAtDoc(layer, 0, 0), 1.0f, kTol),
              "H1: a point OUTSIDE the mask rect but still inside the layer reveals (default "
              "colour 255), matching an unallocated tile's own meaning for free");
      }
    }

    // H2: a mask LARGER than its layer -- extends past the layer's own
    // rectangle on every side. No -2 channel at all (metadata rect only),
    // deliberately: this keeps H2 insensitive to the layerWidth/layerHeight
    // decode-size sabotage above (there is no decode call to corrupt), so
    // that sabotage reddens H1 alone, not H1 and H2 together.
    {
      LayerSpec l;
      l.top = 0; l.left = 0; l.bottom = 4; l.right = 4;
      l.pascalName = "H2";
      l.compression = 0;
      l.channels = {{-1, std::vector<uint32_t>(16, 255)}};
      l.mask = MaskSpec{/*top=*/-2, /*left=*/-2, /*bottom=*/6, /*right=*/6,
                        /*defaultColor=*/255, /*flags=*/0x00};
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 1 && r.document.layers[0].mask.has_value(),
            "H2: a mask rect LARGER than its layer's own rect still parses, carrying a mask");
      if (r.ok && !r.document.layers.empty() && r.document.layers[0].mask.has_value()) {
        const Layer& layer = r.document.layers[0];
        check(layer.mask->occupiedTileCount() == 0,
              "H2: with no channel content, the oversized mask still allocates nothing");
        check(nearf(maskCoverageAtDoc(layer, 1, 1), 1.0f, kTol),
              "H2: and reads as fully revealed, same as an absent mask would");
      }
    }

    // H3: default colour 0 -- everything OUTSIDE a small mask rect, but
    // still inside the layer, must be HIDDEN rather than the revealed-by-
    // default an absent tile means. No -2 channel here either, for the same
    // sabotage-isolation reason as H2: this fixture's only job is to prove
    // the default-colour byte is honoured, not to also re-prove mask
    // content decodes at the right size (H1 already owns that).
    {
      LayerSpec l;
      l.top = 0; l.left = 0; l.bottom = 8; l.right = 8;
      l.pascalName = "H3";
      l.compression = 0;
      l.channels = {{-1, std::vector<uint32_t>(64, 255)}};
      l.mask = MaskSpec{/*top=*/2, /*left=*/2, /*bottom=*/6, /*right=*/6,
                        /*defaultColor=*/0, /*flags=*/0x00};
      const std::vector<uint8_t> bytes = buildPsd(8, 8, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 1 && r.document.layers[0].mask.has_value(),
            "H3: a default-colour-0 masked layer parses and carries a mask");
      if (r.ok && !r.document.layers.empty() && r.document.layers[0].mask.has_value()) {
        const Layer& layer = r.document.layers[0];
        check(nearf(maskCoverageAtDoc(layer, 0, 0), 0.0f, kTol),
              "H3: a point OUTSIDE the mask rect but inside the layer is HIDDEN -- default "
              "colour 0, written explicitly rather than left at the tile's own reveal default");
      }
    }

    // H4: flags bit 1 (mask disabled) -- imported as NO mask at all, not as
    // an applied-but-empty one. A real -2 channel is present, at the SAME
    // dimensions as the layer (so this fixture is insensitive to the
    // decode-size sabotage too), specifically to prove the content is
    // discarded wholesale rather than merely its "outside the rect" half.
    {
      LayerSpec l;
      l.top = 0; l.left = 0; l.bottom = 4; l.right = 4;
      l.pascalName = "H4";
      l.compression = 0;
      l.channels = {{-1, std::vector<uint32_t>(16, 255)}, {-2, std::vector<uint32_t>(16, 10)}};
      l.mask = MaskSpec{/*top=*/0, /*left=*/0, /*bottom=*/4, /*right=*/4,
                        /*defaultColor=*/255, /*flags=*/0x02};  // bit 1: disabled
      const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 1 && !r.document.layers[0].mask.has_value(),
            "H4: flags bit 1 (disabled) imports as Layer::mask == nullopt, not an applied mask");
    }

    // H5/H6: the anti-vacuity pairing Section E already established for
    // RGB, here for a mask -- a mask that is fully revealed (255) except
    // ONE painted texel must occupy exactly ONE tile (the inverted empty-
    // tile rule: skip on 1.0, not 0.0) AND that texel's own value must
    // still be readable, so H5 cannot be satisfied by a reader that simply
    // wrote nothing.
    {
      constexpr uint32_t kSide = 300;  // spans a 3x3 grid of 128x128 tiles
      constexpr uint32_t kPx = 200;
      const size_t n = static_cast<size_t>(kSide) * kSide;
      std::vector<uint32_t> maskSamples(n, 255);
      const size_t hit = static_cast<size_t>(kPx) * kSide + kPx;
      maskSamples[hit] = 64;

      LayerSpec l;
      l.top = 0; l.left = 0;
      l.bottom = static_cast<int32_t>(kSide); l.right = static_cast<int32_t>(kSide);
      l.pascalName = "H5H6";
      l.compression = 0;
      // Alpha 0 everywhere -- this fixture is about the MASK store, not the
      // RGB one, so the layer's own tiles are left empty on purpose (cheap)
      // rather than decoding a 300x300 opaque fill nothing here checks.
      l.channels = {{-1, std::vector<uint32_t>(n, 0)}, {-2, maskSamples}};
      l.mask = MaskSpec{/*top=*/0, /*left=*/0, /*bottom=*/static_cast<int32_t>(kSide),
                        /*right=*/static_cast<int32_t>(kSide), /*defaultColor=*/255,
                        /*flags=*/0x00};
      const std::vector<uint8_t> bytes = buildPsd(kSide, kSide, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(r.ok && r.document.layers.size() == 1 && r.document.layers[0].mask.has_value(),
            "H5/H6: a 300x300 mask holding one non-reveal texel imports");
      if (r.ok && !r.document.layers.empty() && r.document.layers[0].mask.has_value()) {
        const Layer& layer = r.document.layers[0];
        check(layer.mask->occupiedTileCount() == 1,
              "H5: it occupies exactly ONE tile, not the 9 its rectangle spans -- reveal (1.0) "
              "samples allocate nothing, the mask's own inverted empty-tile rule");
        check(nearf(maskCoverageAtDoc(layer, static_cast<int32_t>(kPx), static_cast<int32_t>(kPx)),
                    64.0f / 255.0f, kTol),
              "H6: and that one texel's own value is still there -- so H5 was not bought by "
              "discarding content");
        check(nearf(maskCoverageAtDoc(layer, 5, 5), 1.0f, kTol),
              "H6: a point far from the painted texel, in a different (unallocated) tile, "
              "still reveals");
      }
    }

    // H7/H8: flags bit 0, "position relative to layer", both ways, on a
    // layer whose own origin is NOT (0,0) -- the one property none of the
    // three real sample files can discriminate (io/PsdImport.hpp's header:
    // every masked layer in them sits at (0,0)). The identical raw mask
    // bytes (0,0)-(4,4)) are read twice, once with each flag state, and
    // resolve to two different, individually-checkable places: with the
    // bit set, "relative" means the layer's own (top, left) is added to the
    // mask's raw coordinates, so a (0,0)-(4,4) mask lands exactly ON a
    // layer sitting at (200, 100); with the bit clear, the same raw
    // coordinates are already absolute and land nowhere near it. This is a
    // judgement call this module makes in the absence of a real file that
    // exercises it (docs/psd-import-gaps.md says as much); these two
    // fixtures pin THIS module's own choice down, not Photoshop's.
    {
      LayerSpec base;
      base.top = 100; base.left = 200; base.bottom = 104; base.right = 204;  // 4x4 at (200,100)
      base.compression = 0;
      base.channels = {{-1, std::vector<uint32_t>(16, 0)},  // alpha 0: RGB tiles irrelevant here
                       {-2, std::vector<uint32_t>(16, 77)}};
      base.mask = MaskSpec{/*top=*/0, /*left=*/0, /*bottom=*/4, /*right=*/4,
                           /*defaultColor=*/255, /*flags=*/0x01};  // bit 0: relative

      LayerSpec relative = base;
      const std::vector<uint8_t> relBytes = buildPsd(300, 300, 8, {relative});
      const PsdImportResult relResult =
          importPsd(std::span<const uint8_t>(relBytes.data(), relBytes.size()));
      check(relResult.ok && relResult.document.layers.size() == 1,
            "H7: a relative-flagged mask, on a non-(0,0) layer, parses");
      if (relResult.ok && !relResult.document.layers.empty()) {
        const Layer& layer = relResult.document.layers[0];
        check(nearf(maskCoverageAtDoc(layer, 202, 102), 77.0f / 255.0f, kTol),
              "H7: relative (bit 0 set) -- the mask's raw (0,0)-(4,4) is added to the layer's "
              "own (200,100) origin, so a point INSIDE the layer reads real mask content");
      }

      LayerSpec absolute = base;
      absolute.mask->flags = 0x00;  // bit 0 clear: NOT relative
      const std::vector<uint8_t> absBytes = buildPsd(300, 300, 8, {absolute});
      const PsdImportResult absResult =
          importPsd(std::span<const uint8_t>(absBytes.data(), absBytes.size()));
      check(absResult.ok && absResult.document.layers.size() == 1,
            "H8: the identical mask bytes with the relative flag clear also parse");
      if (absResult.ok && !absResult.document.layers.empty()) {
        const Layer& layer = absResult.document.layers[0];
        check(nearf(maskCoverageAtDoc(layer, 202, 102), 1.0f, kTol),
              "H8: absolute (bit 0 clear) -- the SAME raw (0,0)-(4,4) is read as already-"
              "absolute document coordinates, nowhere near the layer at (200,100), so that "
              "same layer-interior point now reveals instead");
        check(nearf(maskCoverageAtDoc(layer, 1, 1), 77.0f / 255.0f, kTol),
              "H8: and the mask's real content landed at (0,0)-(4,4) instead, confirming the "
              "flag changed WHERE the mask applies, not merely whether H7's point matched");
      }
    }

    // H9 (bonus, not itself one of docs/psd-import-gaps.md's four listed
    // sabotages): a mask record whose declared size is larger than the
    // plain 20-byte shape is refused BY NAME, matching this project's
    // "refuse rather than guess" discipline for exactly the same shape of
    // decision C5 (ZIP) and C6 (PSB) already make elsewhere in this file.
    //
    // The declared size (36) must actually be reachable inside this layer's
    // own extra-data field, or `readLayerRecord()` refuses one field
    // earlier with a DIFFERENT message ("runs past the extra-data field") --
    // a true but less specific refusal that would not be testing what this
    // fixture is for. The Pascal name is padded out with extra bytes purely
    // to buy that room; its content is otherwise irrelevant.
    {
      LayerSpec l;
      l.top = 0; l.left = 0; l.bottom = 2; l.right = 2;
      l.pascalName = "H9-PADDING";
      l.compression = 0;
      l.channels = {{-1, std::vector<uint32_t>(4, 255)}};
      l.mask = MaskSpec{0, 0, 2, 2, 255, 0x00, /*overrideSize=*/36};
      const std::vector<uint8_t> bytes = buildPsd(2, 2, 8, {l});
      const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
      check(!r.ok && !r.noLayerData && contains(r.error, "refused by name"),
            "H9: a mask record larger than the plain 20-byte shape is refused by name, not "
            "guessed at");
    }
  }

  // ==========================================================================
  std::printf("  -- G. Layer groups ('lsct'): a bounding-section divider\n");
  std::printf("        opens a group reading forward, its header closes and names it --\n");
  // ==========================================================================
  //
  // docs/psd-import-gaps.md section 3. No sample file this project holds
  // exercises anything past a single flat run of groups (both real files'
  // own table says "group nesting depth: 0"), so G2 below is a hand-written
  // fixture, not something a real Photoshop file has proven -- stated here
  // rather than left to be assumed from the fact that it exists.
  {
    // G1: a single, flat group, alongside a top-level layer outside it. The
    // divider's own Pascal name is set to Photoshop's own real junk string
    // ("</Layer group>", observed on both sample files) specifically so its
    // absence from the imported result is a positive check, not merely an
    // absence of counter-evidence.
    LayerSpec div1;
    div1.top = 0; div1.left = 0; div1.bottom = 0; div1.right = 0;
    div1.pascalName = "</Layer group>";
    div1.lsct = LsctSpec{3, false, "pass", false, 0};  // bounding section divider

    LayerSpec alpha;
    alpha.top = 0; alpha.left = 0; alpha.bottom = 2; alpha.right = 2;
    alpha.pascalName = "Alpha";
    alpha.channels = {{0, std::vector<uint32_t>(4, 10)}, {1, std::vector<uint32_t>(4, 10)},
                      {2, std::vector<uint32_t>(4, 10)}};

    LayerSpec beta;
    beta.top = 0; beta.left = 0; beta.bottom = 2; beta.right = 2;
    beta.pascalName = "Beta";
    beta.channels = {{0, std::vector<uint32_t>(4, 20)}, {1, std::vector<uint32_t>(4, 20)},
                     {2, std::vector<uint32_t>(4, 20)}};

    LayerSpec hdr1;
    hdr1.top = 0; hdr1.left = 0; hdr1.bottom = 0; hdr1.right = 0;
    hdr1.pascalName = "</Layer group>";  // Photoshop writes this on the header too, on
                                         // some files -- the `luni` name below must win.
    hdr1.uniName = "My Group";
    hdr1.lsct = LsctSpec{1, true, "pass", true, 0};  // open folder, blend key 'pass'

    LayerSpec gamma;
    gamma.top = 0; gamma.left = 0; gamma.bottom = 2; gamma.right = 2;
    gamma.pascalName = "Gamma";
    gamma.channels = {{0, std::vector<uint32_t>(4, 30)}, {1, std::vector<uint32_t>(4, 30)},
                      {2, std::vector<uint32_t>(4, 30)}};

    const std::vector<uint8_t> bytes = buildPsd(8, 8, 8, {div1, alpha, beta, hdr1, gamma});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));

    check(r.ok, "G1: a flat group (2 members) plus one top-level layer parses (ok)");
    check(r.document.layers.size() == 4,
          "G1: 5 records (divider, 2 members, header, 1 outside) import as 4 layers -- "
          "the divider is dropped, the header becomes one Group layer");

    bool noJunkName = true;
    for (const Layer& l : r.document.layers)
      if (l.name == "</Layer group>") noJunkName = false;
    check(noJunkName, "G1: '</Layer group>' (the divider's own Pascal name) appears nowhere "
                      "in the imported result");

    if (r.ok && r.document.layers.size() == 4) {
      const Layer& l0 = r.document.layers[0];
      const Layer& l1 = r.document.layers[1];
      const Layer& l2 = r.document.layers[2];
      const Layer& l3 = r.document.layers[3];

      // [group-count] -- deliberately separate from the [membership] check
      // below it: docs/psd-import-gaps.md's sabotage (swap which end of a
      // frame's range is "inside" it) leaves this one green while reddening
      // that one, which is the whole reason these are two `check()` calls
      // and not one. Confirmed by hand: see this file's own note further
      // down, after G2.
      check(l0.name == "Alpha" && l1.name == "Beta" && l2.kind == LayerKind::Group &&
                l2.name == "My Group" && l3.name == "Gamma",
            "G1 [group-count / identity]: order is Alpha, Beta, the group (named from the "
            "header's own luni name, not its Pascal one), then Gamma -- one Group layer, "
            "sitting above the members it closes, exactly where core/LayerSetOps.cpp's own "
            "GroupLayers puts a freshly created one");
      check(!l2.groupTag.empty() && l2.groupTag[0] == 'G',
            "G1: the group carries a freshly assigned groupTag ('G' + Document::nextGroupId)");

      // [membership]
      check(l0.parent == l2.groupTag && l1.parent == l2.groupTag,
            "G1 [membership]: Alpha and Beta both carry `parent` == the group's own groupTag");
      check(l3.parent.empty(),
            "G1 [membership]: Gamma, outside the group, has an empty `parent`");

      check(!anyContains(r.warnings, "My Group"),
            "G1: a 'pass' group blend key emits no warning (pass-through is this build's own "
            "compositing model, not a mismatch)");
    }
  }

  {
    // G2: nesting -- the stack gives it for free, but **no sample file this
    // project holds exercises depth > 0** (both real files' own feature
    // table says nesting depth 0), so this fixture is the only evidence for
    // this part of the mapping. Outer group holds D directly and a nested
    // inner group (holding E) directly.
    LayerSpec outerDiv;
    outerDiv.top = 0; outerDiv.left = 0; outerDiv.bottom = 0; outerDiv.right = 0;
    outerDiv.pascalName = "</Layer group>";
    outerDiv.lsct = LsctSpec{3, false, "pass", false, 0};

    LayerSpec d;
    d.top = 0; d.left = 0; d.bottom = 2; d.right = 2;
    d.pascalName = "D";
    d.channels = {{0, std::vector<uint32_t>(4, 1)}};

    LayerSpec innerDiv;
    innerDiv.top = 0; innerDiv.left = 0; innerDiv.bottom = 0; innerDiv.right = 0;
    innerDiv.pascalName = "</Layer group>";
    innerDiv.lsct = LsctSpec{3, false, "pass", false, 0};

    LayerSpec e;
    e.top = 0; e.left = 0; e.bottom = 2; e.right = 2;
    e.pascalName = "E";
    e.channels = {{0, std::vector<uint32_t>(4, 2)}};

    LayerSpec innerHdr;
    innerHdr.top = 0; innerHdr.left = 0; innerHdr.bottom = 0; innerHdr.right = 0;
    innerHdr.uniName = "Inner";
    innerHdr.lsct = LsctSpec{2, true, "pass", true, 0};  // closed folder

    LayerSpec f;
    f.top = 0; f.left = 0; f.bottom = 2; f.right = 2;
    f.pascalName = "F";
    f.channels = {{0, std::vector<uint32_t>(4, 3)}};

    LayerSpec outerHdr;
    outerHdr.top = 0; outerHdr.left = 0; outerHdr.bottom = 0; outerHdr.right = 0;
    outerHdr.uniName = "Outer";
    outerHdr.lsct = LsctSpec{1, true, "pass", true, 0};

    const std::vector<uint8_t> bytes =
        buildPsd(8, 8, 8, {outerDiv, d, innerDiv, e, innerHdr, f, outerHdr});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));

    check(r.ok, "G2 (fixture-only, no real file has depth > 0): a nested group parses (ok)");
    check(r.document.layers.size() == 5,
          "G2: 7 records (2 dividers, D, E, 2 headers, F) import as 5 layers -- D, E, "
          "the inner group, F, the outer group");

    if (r.ok && r.document.layers.size() == 5) {
      const Layer& lD = r.document.layers[0];
      const Layer& lE = r.document.layers[1];
      const Layer& lInner = r.document.layers[2];
      const Layer& lF = r.document.layers[3];
      const Layer& lOuter = r.document.layers[4];

      check(lD.name == "D" && lE.name == "E" && lInner.kind == LayerKind::Group &&
                lInner.name == "Inner" && lF.name == "F" && lOuter.kind == LayerKind::Group &&
                lOuter.name == "Outer",
            "G2: stacking order is D, E, the inner group, F, the outer group");
      check(lE.parent == lInner.groupTag,
            "G2 [membership]: E belongs to the INNER group, not the outer one");
      check(lD.parent == lOuter.groupTag && lF.parent == lOuter.groupTag,
            "G2 [membership]: D and F are the outer group's own direct children");
      check(lInner.parent == lOuter.groupTag,
            "G2 [membership]: the inner group's own entry is itself a direct child of the "
            "outer group -- nesting is carried by chaining `parent`, not by flattening");
      check(lOuter.parent.empty(), "G2: the outer group is top-level");
    }
  }

  // Sabotage, run by hand and reverted before this file was left as it is
  // (matching section D3-control's own precedent for recording a sabotage
  // result in prose rather than as a permanent runtime toggle): swapped the
  // two bounds of the membership-stamping loop in io/PsdImport.cpp's group
  // header handling (`for (idx = frameStart; idx < doc.layers.size(); ...)`
  // became `for (idx = doc.layers.size(); idx < frameStart; ...)`, an empty
  // range whenever `frameStart` is the loop's natural start rather than its
  // end). Rebuilt and reran this suite: G1's "[group-count / identity]"
  // check STAYED GREEN (the group is still created, still named from the
  // header, still sits above where its members were) while G1's
  // "[membership]" check WENT RED (Alpha and Beta both kept an empty
  // `parent` instead of the group's tag) -- exactly the split
  // docs/psd-import-gaps.md predicts for "swap the push/pop roles", and the
  // reason this file asserts group identity and membership as two `check()`
  // calls rather than one. Reverted immediately after; the source in this
  // file's own tree has never carried the sabotage.

  {
    // G3: a group whose own blend key is NOT 'pass' (an isolated group, in
    // Photoshop's vocabulary) -- imported anyway (core/Composite.hpp:688
    // composites every group here as pass-through regardless) and warned by
    // name, the same discipline an ordinary layer's unmapped blend key gets.
    LayerSpec div3;
    div3.top = 0; div3.left = 0; div3.bottom = 0; div3.right = 0;
    div3.lsct = LsctSpec{3, false, "pass", false, 0};

    LayerSpec x;
    x.top = 0; x.left = 0; x.bottom = 2; x.right = 2;
    x.pascalName = "X";
    x.channels = {{0, std::vector<uint32_t>(4, 5)}};

    LayerSpec hdr3;
    hdr3.top = 0; hdr3.left = 0; hdr3.bottom = 0; hdr3.right = 0;
    hdr3.uniName = "Isolated Group";
    hdr3.lsct = LsctSpec{1, true, "norm", true, 0};  // NOT 'pass'

    const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {div3, x, hdr3});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));

    check(r.ok && r.document.layers.size() == 2,
          "G3: a non-'pass' group blend key still imports the group (ok, 2 layers)");
    if (r.ok && r.document.layers.size() == 2) {
      check(r.document.layers[1].kind == LayerKind::Group &&
                r.document.layers[1].name == "Isolated Group",
            "G3: the group itself imports, named correctly, despite the mismatched blend key");
      check(r.document.layers[0].parent == r.document.layers[1].groupTag,
            "G3: X is still grouped normally -- the blend-key mismatch is a warning, not a "
            "membership change");
    }
    check(anyContains(r.warnings, "Isolated Group") && anyContains(r.warnings, "norm"),
          "G3: the non-'pass' blend key is named in a warning, by group name and by key, "
          "not silently treated as pass-through");
  }

  {
    // G4: unbalanced -- a divider with no matching header anywhere before
    // end of file. Total refusal (io/Descriptor.hpp's own "a refusal is
    // total"), not a best-effort import of everything up to the gap.
    LayerSpec div4;
    div4.top = 0; div4.left = 0; div4.bottom = 0; div4.right = 0;
    div4.lsct = LsctSpec{3, false, "pass", false, 0};

    LayerSpec y;
    y.top = 0; y.left = 0; y.bottom = 2; y.right = 2;
    y.pascalName = "Y";
    y.channels = {{0, std::vector<uint32_t>(4, 7)}};

    const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {div4, y});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(!r.ok && r.document.layers.empty(),
          "G4: a bounding-section divider with no matching header refuses cleanly, and the "
          "refusal is total (no half-built document)");
  }

  {
    // G5: unbalanced the other way -- a header with no divider ever opened
    // before it.
    LayerSpec z;
    z.top = 0; z.left = 0; z.bottom = 2; z.right = 2;
    z.pascalName = "Z";
    z.channels = {{0, std::vector<uint32_t>(4, 9)}};

    LayerSpec hdr5;
    hdr5.top = 0; hdr5.left = 0; hdr5.bottom = 0; hdr5.right = 0;
    hdr5.uniName = "Orphan";
    hdr5.lsct = LsctSpec{1, true, "pass", true, 0};

    const std::vector<uint8_t> bytes = buildPsd(4, 4, 8, {z, hdr5});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(!r.ok && r.document.layers.empty(),
          "G5: a group header with no matching divider open before it refuses cleanly, and "
          "the refusal is total");
  }
  std::printf("  -- I. 'lddg' (Linear Dodge / Add) -> BlendMode::Plus --\n");
  // ==========================================================================
  {
    LayerSpec l;
    l.top = 0; l.left = 0; l.bottom = 2; l.right = 2;
    l.blendKey = "lddg";
    l.pascalName = "I1";
    l.compression = 0;
    l.channels = {{0, std::vector<uint32_t>(4, 10)},
                  {1, std::vector<uint32_t>(4, 20)},
                  {2, std::vector<uint32_t>(4, 30)},
                  {-1, std::vector<uint32_t>(4, 255)}};
    const std::vector<uint8_t> bytes = buildPsd(2, 2, 8, {l});
    const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    check(r.ok && r.document.layers.size() == 1 && r.document.layers[0].blend == "plus",
          "I1: 'lddg' (Linear Dodge/Add) maps to core::BlendMode::Plus");
    check(r.ok && r.warnings.empty(),
          "I1: and, being an exact key-table entry, emits no 'no equivalent' warning");
  }

  // The section footer every other file in `app/selftest/` prints. Not
  // decoration: `ok` alone is returned into `main.cpp`'s conjunction and
  // would fail the suite correctly without this line, but a section with no
  // footer is unfindable in 6300 lines of output -- a reader scanning for
  // "did the PSD reader run, and did it pass" has nothing to search for.
  // Two earlier tracks in this project shipped exactly this gap.
  std::printf("[selftest] psd import %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
