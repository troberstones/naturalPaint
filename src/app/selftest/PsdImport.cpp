#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "app/OpenAnyFile.hpp"
#include "io/ImageIO.hpp"
#include "io/PsdImport.hpp"

// io/PsdImport (and the app/OpenAnyFile seam it fills) -- headless, GPU-free.
// No genuine Photoshop-authored file is available on this machine (this
// file's io/PsdImport.hpp states that at length, and it applies here
// unchanged): every fixture below is written by a matching PSD *writer*
// built in this file, so a pass proves this reader agrees with this
// author's reading of the published specification -- corroborated, for the
// two facts that mattered most (layer stacking order, and the `flags`
// byte's inverted "visible" bit), against an independent third-party
// reader's behaviour, per io/PsdImport.hpp's own citations -- and does NOT
// prove agreement with what Photoshop itself emits.
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

struct LayerSpec {
  int32_t top = 0, left = 0, bottom = 0, right = 0;
  std::string blendKey = "norm";
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  bool hidden = false;
  std::string pascalName;
  std::optional<std::string> uniName;
  int compression = 0;
  std::vector<ChannelSpec> channels;
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
    ew.u32(0);  // layer mask data: none
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
    unmapped.blendKey = "sLit";  // Soft Light -- no core::BlendMode equivalent
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
      check(l3.blend == "normal" && anyContains(r.warnings, "sLit"),
            "A: 'sLit' (Soft Light, no equivalent) imports as Normal AND is named in a warning");
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
