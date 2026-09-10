#include "app/selftest/Support.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "io/PackBits.hpp"
#include "io/PsdExport.hpp"
#include "io/PsdImport.hpp"

namespace np {

// io/PsdExport: the PSD container and its flattened composite (PLAN.md phase
// 15 tier 1, docs/psd-export.md).
//
// **What this section can and cannot prove on its own.** Three of the five
// claims below are checked against numbers computed here, by hand, from the
// format's own layout -- the file header's 40 bytes, the sRGB byte a known
// linear value must produce, and the un-premultiplied byte a translucent texel
// must produce. Those are the ones a round trip through our own code cannot
// see: encode and decode can be wrong in the same direction and agree.
//
// The fourth, the Image Data Section's framing, IS a round trip -- through
// `decodePackBits()`, which two real importers already depend on against real
// Kyle Webster packs and real Photoshop files. That is deliberate and is
// io/PackBits.hpp's own argument: a round trip against a function with that
// much real-world exposure beats a hand-authored expected-bytes fixture, which
// only ever proves the encoder agrees with its author's reading of the spec.
//
// And the fifth thing neither can prove is whether Photoshop and the rest of
// the world actually render the file. docs/psd-export.md calls that out and
// requires an external oracle; psd-tools 1.18.0 was run against this writer's
// output via `--psd-export` and agreed with an independently-produced 8-bit
// PNG of the same document on every pixel. That check lives outside the
// binary, so it is recorded here rather than asserted here.
//
// **The round trip that is NOT here, and why its absence is the design.**
// Tier 1 writes a zero-length Layer and Mask Information section, which is a
// legal flat PSD. `importPsd()` therefore refuses it with `noLayerData` --
// app/OpenAnyFile.cpp routes exactly that case to the flattened OpenImageIO
// path. So the assertion below asks for that refusal by name. A future reader
// finding "importPsd() refuses" in a passing test should not go hunting for a
// bug: the file has no layers to import, on purpose, and tier 2 is what fills
// that section in.

namespace {

// Writes a *straight* (non-premultiplied) linear RGBA value into a document's
// layer, premultiplying on the way in exactly the way io/ImageIO.cpp's
// writeDecodedImageIntoLayer() does -- so these fixtures hold what a real
// opened or painted document holds, not a hand-tuned storage layout the export
// path never sees. Lifted from app/selftest/Export.cpp's own fixture helper for
// that reason.
void writeStraight(Document& doc, size_t layerIndex, int32_t x, int32_t y, float r, float g,
                   float b, float a) {
  TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
  const PixelCoord p{x, y};
  tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
}

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

bool anyWarningContains(const std::vector<std::string>& warnings, const char* needle) {
  for (const std::string& w : warnings)
    if (contains(w, needle)) return true;
  return false;
}

}  // namespace

bool runPsdExportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- The fixture -------------------------------------------------------
  //
  // 5x3, deliberately not square and deliberately not a tile multiple, so a
  // width/height transposition cannot pass and a row-stride error cannot hide.
  // Every value written is exact in half (core/Tile stores half), so nothing
  // below has to carry a storage-precision tolerance -- the only lossy stage
  // in the chain is the 8-bit quantisation each assertion is actually about.
  constexpr int32_t kW = 5;
  constexpr int32_t kH = 3;
  Document doc = Document::createBlank(kW, kH, WorkingSpace{});
  // (1,0): mid grey, fully opaque. The sRGB claim.
  writeStraight(doc, 0, 1, 0, 0.5f, 0.5f, 0.5f, 1.0f);
  // (2,1): three different channels at half opacity. The straight-alpha claim,
  // and -- because the three channels differ -- also a planar-order claim: an
  // interleaved or channel-swapped writer cannot produce these three bytes in
  // these three planes.
  writeStraight(doc, 0, 2, 1, 1.0f, 0.5f, 0.25f, 0.5f);
  // (4,2): white, opaque, in the last pixel of the last row. A height/width
  // transposition or an off-by-one row count puts this somewhere else.
  writeStraight(doc, 0, 4, 2, 1.0f, 1.0f, 1.0f, 1.0f);

  const PsdExportResult psd = writeFlattenedPsd(doc);
  check(psd.ok && psd.error.empty(), "psd export: a 5x3 document writes without error");
  if (!psd.ok) {
    std::printf("    (error was: %s)\n", psd.error.c_str());
    return false;  // nothing below can say anything useful about no bytes
  }

  // --- A. The file header, byte for byte ---------------------------------
  //
  // Hand-computed from docs/psd-export.md's table, NOT captured from this
  // writer's own output -- a captured expectation asserts only that the writer
  // is deterministic. 26 header bytes, then three zero-length section lengths
  // (colour mode data, image resources, layer and mask info), then the Image
  // Data Section's compression word.
  //
  // Note `00 00 00 03` (height) precedes `00 00 00 05` (width). That is Adobe's
  // order and the one field here most likely to be written the other way round;
  // a 5x3 fixture is what makes the two distinguishable.
  {
    const std::vector<uint8_t> expected = {
        0x38, 0x42, 0x50, 0x53,                          // "8BPS"
        0x00, 0x01,                                      // version 1, never PSB
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // 6 reserved zero bytes
        0x00, 0x04,                                      // 4 channels: R G B A
        0x00, 0x00, 0x00, 0x03,                          // height = 3
        0x00, 0x00, 0x00, 0x05,                          // width  = 5
        0x00, 0x08,                                      // depth = 8
        0x00, 0x03,                                      // colour mode 3 = RGB
        0x00, 0x00, 0x00, 0x00,                          // colour mode data length
        0x00, 0x00, 0x00, 0x00,                          // image resources length
        0x00, 0x00, 0x00, 0x00,                          // layer and mask info length
        0x00, 0x01,                                      // image data: PackBits
    };
    check(psd.bytes.size() > expected.size() &&
              std::equal(expected.begin(), expected.end(), psd.bytes.begin()),
          "psd export: the first 40 bytes are the hand-computed header");
  }

  // --- B. The Image Data Section round-trips through decodePackBits ------
  //
  // The merged composite's framing, which is the piece docs/psd-export.md
  // states loosely: ONE row-count table covering every row of every channel
  // (height * 4 entries), then every compressed row. Handing decodePackBits()
  // `height * 4` as its table length and `w * h * 4` as its expected byte count
  // is therefore a direct assertion of that framing -- a per-channel-table
  // writer (the per-LAYER shape) desynchronises on the second channel and this
  // call fails.
  constexpr size_t kImageDataOffset = 40;  // 26 + 4 + 4 + 4 + the u16 compression word
  std::vector<uint8_t> planes;
  {
    const bool decoded =
        decodePackBits(psd.bytes, kImageDataOffset, psd.bytes.size(),
                       static_cast<uint32_t>(kH) * 4, static_cast<size_t>(kW) * kH * 4, planes);
    check(decoded && planes.size() == static_cast<size_t>(kW) * kH * 4,
          "psd export: image data decodes as one table over all four channels");
    if (!decoded) planes.assign(static_cast<size_t>(kW) * kH * 4, 0);
  }

  // Channel c of pixel (x,y), out of the PLANAR layout: all of R, then all of
  // G, then all of B, then all of A.
  auto plane = [&](int c, int32_t x, int32_t y) -> uint8_t {
    return planes[static_cast<size_t>(c) * kW * kH + static_cast<size_t>(y) * kW + x];
  };

  {
    // Nothing was painted at (0,0), so it is transparent -- and the merged
    // composite is a PREVIEW matted on white, so the colour bytes there are
    // 255 and not 0. That is the thing docs/psd-export.md does not mention and
    // that a reading of our own importer could not have found (it never reads
    // the merged section at all); io/PsdExport.hpp records the two independent
    // readers the claim rests on. A straight-RGB writer puts 0,0,0,0 here.
    check(plane(0, 0, 0) == 255 && plane(1, 0, 0) == 255 && plane(2, 0, 0) == 255 &&
              plane(3, 0, 0) == 0,
          "psd export: an unpainted texel is WHITE at alpha 0 -- the matte");
    // And the last pixel of the last row really is where it was written --
    // the check a transposed width/height or a short row table fails. Opaque,
    // so the matte term is zero and this is the same byte either way.
    check(plane(0, 4, 2) == 255 && plane(1, 4, 2) == 255 && plane(2, 4, 2) == 255 &&
              plane(3, 4, 2) == 255,
          "psd export: opaque white lands at the last pixel of the last row");
  }

  // --- C. sRGB, against a byte computed here rather than by the writer ---
  //
  // srgbEncode() is the exact inverse of the srgbDecode() io/PsdImport.cpp
  // applies on the way in. 0.5 linear is 0.735358 encoded, which quantises to
  // 188 -- the literal is spelled out as well as computed, so this cannot pass
  // by both sides calling a broken srgbEncode(). A writer that skipped the
  // transfer function entirely would put 128 here.
  {
    const int expected = static_cast<int>(srgbEncode(0.5f) * 255.0f + 0.5f);
    check(expected == 188, "psd export: srgbEncode(0.5) quantises to 188 (computed here)");
    check(plane(0, 1, 0) == 188 && plane(1, 1, 0) == 188 && plane(2, 1, 0) == 188,
          "psd export: linear 0.5 is written as the sRGB byte 188, not 128");
    check(plane(3, 1, 0) == 255,
          "psd export: alpha 1.0 is written as 255 -- and is never gamma-encoded");
  }

  // --- D. Straight (unassociated) alpha ----------------------------------
  //
  // (2,1) was written with straight RGB (1.0, 0.5, 0.25) at alpha 0.5, so the
  // tile STORES the premultiplied (0.5, 0.25, 0.125, 0.5). The file must carry
  // the straight values back:
  //
  // Straight, then matted on white against the alpha byte the file carries
  // (a8 = 128/255 = 0.501961), which is what the merged composite stores:
  //
  //          straight  encoded  stored = e * a8 + (1 - a8)   premultiplied
  //   R          1.00    1.000                        255             188
  //   G          0.50    0.735                        221             137
  //   B          0.25    0.537                        196              99
  //   A          0.50        -                        128             128
  //
  // Every one of the three differs from its premultiplied counterpart AND from
  // its unmatted one, and the three differ from each other -- so this single
  // texel fails a premultiplying writer, an unmatted writer, and a writer that
  // swapped two planes or emitted one plane twice.
  {
    check(plane(0, 2, 1) == 255, "psd export: a half-transparent texel's R is 255");
    check(plane(1, 2, 1) == 221, "psd export: a half-transparent texel's G is matted (221)");
    check(plane(2, 2, 1) == 196, "psd export: a half-transparent texel's B is matted (196)");
    check(plane(3, 2, 1) == 128, "psd export: a half-transparent texel's alpha is 128");
    // The independently-computed forms, so the literals above are checkable
    // rather than merely stated -- and so the premultiplied bytes this must
    // NOT produce are spelled out in the same arithmetic.
    const float a8 = 128.0f / 255.0f;
    check(static_cast<int>((srgbEncode(0.5f) * a8 + 1.0f - a8) * 255.0f + 0.5f) == 221 &&
              static_cast<int>((srgbEncode(0.25f) * a8 + 1.0f - a8) * 255.0f + 0.5f) == 196,
          "psd export: the matte arithmetic recomputes 221 and 196 here");
    check(static_cast<int>(srgbEncode(0.25f) * 255.0f + 0.5f) == 137 &&
              static_cast<int>(srgbEncode(0.125f) * 255.0f + 0.5f) == 99,
          "psd export: premultiplied would have been 137 and 99, and is neither");
    // And the inverse a reader actually applies, on the bytes actually
    // written: psd-tools' `(c + a - 1) / a`. It must recover the straight
    // encoded value to within the one rounding of `c`, which is 1/255 = 0.004.
    const float recovered = (static_cast<float>(plane(1, 2, 1)) / 255.0f + a8 - 1.0f) / a8;
    check(std::fabs(recovered - srgbEncode(0.5f)) < 0.005f,
          "psd export: a reader's un-matte recovers the straight value");
  }

  // --- E. Refusals are total ---------------------------------------------
  //
  // "A refusal is total" (io/Descriptor.hpp, io/NpaintFile): an error, and NO
  // bytes. A header over truncated or absent pixels is the one outcome worse
  // than a refusal, because it is a file the user will try to open.
  {
    const Document wide = Document::createBlank(30001, 4, WorkingSpace{});
    const PsdExportResult r = writeFlattenedPsd(wide);
    check(!r.ok && r.bytes.empty(), "psd export: a 30001px-wide document is refused, no bytes");
    check(contains(r.error, "30000") && contains(r.error, "PSB"),
          "psd export: the width refusal names PSD's 30,000 limit and PSB");
  }
  {
    const Document tall = Document::createBlank(4, 30001, WorkingSpace{});
    const PsdExportResult r = writeFlattenedPsd(tall);
    check(!r.ok && r.bytes.empty(), "psd export: a 30001px-tall document is refused, no bytes");
  }
  {
    // 30000 itself is inside the limit and must NOT be refused -- an
    // off-by-one in the bound would otherwise pass every test above. Only the
    // refusal check is asked here; compositing a 30000x1 canvas is cheap, but
    // 30000x30000 would not be, which is exactly why the size check runs
    // before the flatten and why this asks psdContainerRefusal() directly.
    Document edge = Document::createBlank(30000, 30000, WorkingSpace{});
    check(psdContainerRefusal(edge).empty(),
          "psd export: 30000x30000 is inside the limit and is not refused");
  }
  {
    const Document zero = Document::createBlank(0, 0, WorkingSpace{});
    const PsdExportResult r = writeFlattenedPsd(zero);
    check(!r.ok && r.bytes.empty(), "psd export: a zero-size document is refused, no bytes");
    check(!r.error.empty(), "psd export: a refusal's error is non-empty (and ok's is not)");
  }
  {
    const Document flat = Document::createBlank(2, 0, WorkingSpace{});
    const PsdExportResult r = writeFlattenedPsd(flat);
    check(!r.ok && r.bytes.empty(), "psd export: a zero-height document is refused, no bytes");
  }

  // --- F. Clipping is named, with a number -------------------------------
  //
  // PRD I11, and io/ExportAs.hpp's precedent ("which highlight value an integer
  // depth will clip"). One sample over white, and the warning has to say both
  // how many and how bright.
  {
    Document hot = Document::createBlank(2, 2, WorkingSpace{});
    writeStraight(hot, 0, 0, 0, 2.0f, 0.5f, 0.5f, 1.0f);
    const PsdExportResult r = writeFlattenedPsd(hot);
    check(r.ok, "psd export: an over-white document still exports");
    check(anyWarningContains(r.warnings, "1 colour sample was") &&
              anyWarningContains(r.warnings, "2.000"),
          "psd export: clipping warns with the count and the brightest value");
  }
  {
    // And an in-range document says nothing about clipping -- a warning that
    // is always there is not a warning.
    check(!anyWarningContains(psd.warnings, "clip"),
          "psd export: an in-range document carries no clipping warning");
  }

  // --- G. What our own importer makes of it ------------------------------
  //
  // Not a bug: tier 1 writes no layer section, so importPsd() refuses with
  // `noLayerData` and app/OpenAnyFile.cpp routes that to the flattened decode
  // path. Asserting the refusal BY NAME is what stops a later reader from
  // "fixing" it -- and what will fail loudly the day tier 2's layer section
  // lands and this file stops being flat.
  {
    const PsdImportResult back = importPsd(psd.bytes);
    check(!back.ok && back.noLayerData,
          "psd export: importPsd() reports noLayerData -- a flat PSD, by design");
  }

  return ok;
}

}  // namespace np
