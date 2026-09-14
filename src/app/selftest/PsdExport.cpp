#include "app/selftest/Support.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/LayerOps.hpp"
#include "core/VectorRaster.hpp"
#include "io/Export.hpp"
#include "io/PackBits.hpp"
#include "io/PsdExport.hpp"
#include "io/PsdImport.hpp"
#include "io/PsdLayerExtras.hpp"
#include "io/PsdLayerSection.hpp"
#include "io/PsdVectorPath.hpp"
#include "io/PsdVectorWrite.hpp"

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

// --- Section H's fixture kit ---------------------------------------------

std::vector<uint8_t> hexBytes(const char* hex) {
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  const std::string t(hex);
  std::vector<uint8_t> out;
  out.reserve(t.size() / 2);
  for (size_t i = 0; i + 1 < t.size(); i += 2)
    out.push_back(static_cast<uint8_t>((v(t[i]) << 4) | v(t[i + 1])));
  return out;
}

Anchor anchorAt(float px, float py, float ix, float iy, float ox, float oy) {
  Anchor a;
  a.pt = PathPoint{px, py};
  a.in = PathPoint{ix, iy};
  a.out = PathPoint{ox, oy};
  return a;
}

// A closed rectangle with every handle sitting on its own anchor -- straight
// edges, which is what makes the winding argument in H3 about winding and not
// about curve shape.
SubPath closedRect(float x0, float y0, float x1, float y1) {
  SubPath s;
  s.closed = true;
  const PathPoint pts[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  for (const PathPoint& p : pts) {
    Anchor a;
    a.pt = p;
    a.in = p;
    a.out = p;
    s.anchors.push_back(a);
  }
  return s;
}

// A document whose ONLY layer is a Vector layer holding `path` filled with
// `rgba`. `createBlank()` seeds an RGB layer, so the `clear()` is what makes
// this a vector-only fixture -- it cost two failed assertions the last time
// this file was touched.
Document vectorOnlyDoc(int32_t w, int32_t h, Path path, std::array<float, 4> rgba,
                       const char* name) {
  Document doc = Document::createBlank(w, h, WorkingSpace{});
  doc.layers.clear();
  Layer v = makeVectorLayer(name);
  VectorShape shape;
  shape.path = std::move(path);
  shape.fill.on = true;
  shape.fill.rgba = rgba;
  shape.id = 1;
  v.shapes.push_back(std::move(shape));
  doc.layers.push_back(std::move(v));
  return doc;
}

// Alpha coverage at each probe, through the same `rasterizeVectorLayer()` the
// compositor reaches -- so "these two paths render the same" is asked of the
// renderer rather than of a second implementation of it.
std::vector<float> coverageAt(const std::vector<VectorShape>& shapes, int32_t w, int32_t h,
                              const std::vector<PixelCoord>& probes,
                              const GradientTable& gradients = {}) {
  const TileStore tiles = rasterizeVectorLayer(shapes, gradients, w, h);
  std::vector<float> out;
  out.reserve(probes.size());
  for (const PixelCoord& p : probes) {
    const Tile* t = tiles.find(tileCoordAt(p));
    out.push_back(t == nullptr ? 0.0f : t->readPixel(tileLocalOffset(p))[3]);
  }
  return out;
}

// Does `haystack` contain `needle` as a byte subsequence?
bool containsBytes(const std::vector<uint8_t>& haystack, const std::vector<uint8_t>& needle) {
  if (needle.empty() || needle.size() > haystack.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
    if (std::equal(needle.begin(), needle.end(), haystack.begin() + static_cast<long>(i)))
      return true;
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

  // --- H. A Vector layer exports its GEOMETRY and its pixels --------------
  //
  // docs/psd-vector-shapes.md's S1, closed. `writePsd()` takes its composite
  // from `flattenDocumentToLinear()`, which materialises Vector layers, and
  // handed the RAW document to `writePsdLayerAndMaskInfo()` -- so a PSD
  // written from a document with shape layers used to carry a merged image
  // that shows the shape and a layer record with nothing in it. Open the file
  // anywhere and the picture was right; open its layers and the artwork was
  // gone.
  //
  // **This section previously asserted that loss**, and it is now the other
  // way round: the record carries a real `vsms` + `SoCo` shape layer AND a
  // rasterised cache, which is what Photoshop itself writes with Maximize
  // Compatibility on. The half that has not changed is the first one below --
  // the flattener still draws the shape into the composite, which is what a
  // reader that parses no layer section sees.
  {
    // Deliberately 37x23: prime, not square, not a tile multiple, and the
    // fixed-point encoding divides VERTICAL by height and HORIZONTAL by
    // width, so a transposed divisor pair moves every point and cannot pass.
    // The coordinates are deliberately non-dyadic (5.3, not 8) so that the
    // 8.24 quantisation is actually exercised rather than landing exactly on
    // a representable step, and every anchor's `in`, `pt` and `out` differ
    // from each other in both axes -- a writer that transposed y and x, or
    // that wrote the three points in the wrong order, fails the comparison
    // below rather than producing a plausible different shape.
    constexpr int32_t kVW = 37;
    constexpr int32_t kVH = 23;
    Path quad;
    SubPath sub;
    sub.closed = true;
    sub.anchors.push_back(anchorAt(5.3f, 4.1f, 3.1f, 6.7f, 9.9f, 2.3f));
    sub.anchors.push_back(anchorAt(31.7f, 4.1f, 27.2f, 2.9f, 33.4f, 7.1f));
    sub.anchors.push_back(anchorAt(31.7f, 18.9f, 30.1f, 14.4f, 26.6f, 20.2f));
    sub.anchors.push_back(anchorAt(5.3f, 18.9f, 8.8f, 21.1f, 2.2f, 15.5f));
    quad.subpaths.push_back(sub);
    // A colour whose three channels differ and none of which is 0 or 1, so a
    // channel swap and a missing transfer function both fail.
    const std::array<float, 4> kFill{0.25f, 0.5f, 0.75f, 1.0f};
    Document vec = vectorOnlyDoc(kVW, kVH, quad, kFill, "a filled quad");

    check(vec.layers.size() == 1 && vec.layers[0].kind == LayerKind::Vector &&
              !vec.layers[0].rgbTiles.has_value(),
          "psd export: the fixture IS a Vector layer with no raster of its own");

    // H1. The composite half, unchanged by S1: the exporter's own flattener
    // draws the quad.
    const DecodedImage flat = flattenDocumentToLinear(vec);
    size_t litTexels = 0;
    for (size_t i = 3; i < flat.pixels.size(); i += 4)
      if (flat.pixels[i] > 0.5f) ++litTexels;
    check(flat.valid() && litTexels > 100,
          "psd export: flattenDocumentToLinear() still rasterises the shape into the composite");

    const PsdExportResult layered = writeLayeredPsd(vec);
    check(layered.ok, "psd export: a document whose only layer is Vector still writes");
    check(!anyWarningContains(layered.warnings, "exports EMPTY"),
          "psd export: and it NO LONGER says the layer exports EMPTY -- that warning was S1");
    check(!anyWarningContains(layered.warnings, "Bezier geometry"),
          "psd export: nor that its Bezier geometry could not be carried");

    // H2. The layer half, through our own importer: a Vector layer, with the
    // geometry this writer put in the `vsms` block.
    const PsdImportResult back = importPsd(layered.bytes);
    check(back.ok && back.document.layers.size() == 1,
          "psd export: the layered file re-imports as exactly one layer");
    const bool isVec = back.ok && back.document.layers.size() == 1 &&
                       back.document.layers[0].kind == LayerKind::Vector;
    check(isVec,
          "psd export: and that layer comes back as LayerKind::Vector, not an empty RGB record");
    check(isVec && back.document.layers[0].shapes.size() == 1,
          "psd export: carrying exactly one shape");

    bool geometryOk = isVec && back.document.layers[0].shapes.size() == 1;
    const SubPath* got = nullptr;
    if (geometryOk) {
      const Path& p = back.document.layers[0].shapes[0].path;
      geometryOk =
          p.subpaths.size() == 1 && p.subpaths[0].anchors.size() == 4 && p.subpaths[0].closed;
      if (geometryOk) got = &p.subpaths[0];
    }
    check(geometryOk,
          "psd export: one CLOSED subpath of four anchors -- PSD's knot list does not repeat "
          "the final anchor and neither does core::SubPath");

    if (got != nullptr) {
      // The tolerance is COMPUTED from the encoding, not chosen: 8.24 fixed
      // point divides each axis into 2^24 steps and the decoder's float
      // result carries an ulp on top.
      const float tolX = psdPathCoordTolerance(kVW);
      const float tolY = psdPathCoordTolerance(kVH);
      float worstX = 0.0f, worstY = 0.0f;
      for (size_t i = 0; i < 4; ++i) {
        const Anchor& a = sub.anchors[i];
        const Anchor& b = got->anchors[i];
        const float dx[3] = {std::fabs(a.pt.x - b.pt.x), std::fabs(a.in.x - b.in.x),
                             std::fabs(a.out.x - b.out.x)};
        const float dy[3] = {std::fabs(a.pt.y - b.pt.y), std::fabs(a.in.y - b.in.y),
                             std::fabs(a.out.y - b.out.y)};
        for (int k = 0; k < 3; ++k) {
          worstX = std::max(worstX, dx[k]);
          worstY = std::max(worstY, dy[k]);
        }
      }
      check(worstX <= tolX && worstY <= tolY,
            "psd export: every anchor AND handle survives inside the 8.24 fixed-point bound");
      // And the bound is not vacuous: these coordinates really are quantised,
      // so the error is nonzero. A pass with a worst error of exactly 0 would
      // be testing dyadic coordinates rather than the encoding.
      check(worstX > 0.0f || worstY > 0.0f,
            "psd export: ...and the quantisation really bit -- the fixture is not dyadic");
    }

    // H3. The fill colour. `SoCo` carries full doubles, not bytes, so this
    // round trip is tight to float precision rather than to 1/255 -- a writer
    // that quantised the colour to 8 bits on the way out fails it.
    if (isVec && back.document.layers[0].shapes.size() == 1) {
      const std::array<float, 4>& got4 = back.document.layers[0].shapes[0].fill.rgba;
      const bool on = back.document.layers[0].shapes[0].fill.on;
      check(on && std::fabs(got4[0] - kFill[0]) < 1e-5f &&
                std::fabs(got4[1] - kFill[1]) < 1e-5f && std::fabs(got4[2] - kFill[2]) < 1e-5f,
            "psd export: the fill colour survives through SoCo's sRGB doubles, all three "
            "channels and in order");
    }

    // H4. The Maximize-Compatibility raster, read out of the record's own
    // channel data -- which our own importer cannot show, because it turns
    // the record back into a tile-less Vector layer. So this asks
    // `buildPsdLayerRecord()` directly and decodes the alpha channel.
    {
      PsdLayerRecord rec;
      std::vector<std::string> warnings;
      const bool built = buildPsdLayerRecord(vec.layers[0], vec, rec, warnings);
      check(built && rec.right - rec.left == kVW && rec.bottom - rec.top == kVH,
            "psd export: the Vector layer's record has a real rectangle, not an empty one");
      // Channel order is -1, 0, 1, 2 -- alpha first.
      bool alphaOk = built && !rec.channels.empty() && rec.channels[0].id == -1;
      std::vector<uint8_t> alpha;
      if (alphaOk)
        alphaOk = decodePackBits(rec.channels[0].data, 2, rec.channels[0].data.size(),
                                 static_cast<uint32_t>(kVH), static_cast<size_t>(kVW) * kVH,
                                 alpha);
      check(alphaOk, "psd export: its alpha channel decodes to one byte per texel");
      if (alphaOk)
        check(alpha[static_cast<size_t>(11) * kVW + 18] == 255 && alpha[0] == 0,
              "psd export: and holds the rasterised shape -- opaque inside, empty at (0,0)");
    }

    // H5. `vsms` is framed as an `8BIM` block inside the record's extra data,
    // and it is the only vector key written. `vmsk` is deliberately absent --
    // io/PsdVectorWrite.hpp argues why.
    {
      PsdLayerRecord rec;
      std::vector<std::string> warnings;
      buildPsdLayerRecord(vec.layers[0], vec, rec, warnings);
      check(containsBytes(rec.extraBlocks, {'8', 'B', 'I', 'M', 'v', 's', 'm', 's'}),
            "psd export: the record's extra data carries an 8BIM/vsms block");
      check(containsBytes(rec.extraBlocks, {'8', 'B', 'I', 'M', 'S', 'o', 'C', 'o'}),
            "psd export: ...and an 8BIM/SoCo block beside it");
      check(!containsBytes(rec.extraBlocks, {'8', 'B', 'I', 'M', 'v', 'm', 's', 'k'}),
            "psd export: and NO vmsk -- one key, one meaning (io/PsdVectorWrite.hpp)");
      check(rec.extraBlocks.size() % 2 == 0,
            "psd export: the extra-block region is even, which io/PsdImport's block walk needs");
    }
  }

  // --- H6. The fill rule survives as a RENDERING, not as a label ----------
  //
  // PSD has no fill-rule field: the rule is carried by the per-subpath boolean
  // operation, so `encodePsdVectorShapeMask()` writes Union for NonZero and
  // Exclude for EvenOdd and `composePsdSubPaths()` folds either back.
  //
  // Asserting that the enum comes back equal would restate the encoder. What
  // is asserted instead is what the claim is FOR: two concentric squares of
  // the SAME winding draw a solid block under NonZero and a donut under
  // EvenOdd, and each must still do so after a round trip through a file.
  //
  // **The two states are diffed before either is trusted** -- a round trip
  // that returned the wrong rule would still pass if the two rules happened to
  // render alike, so the first assertion is that they do not.
  {
    constexpr int32_t kW2 = 64;
    constexpr int32_t kH2 = 64;
    auto donut = [](FillRule rule) {
      Path p;
      p.rule = rule;
      p.subpaths.push_back(closedRect(8, 8, 56, 56));
      p.subpaths.push_back(closedRect(24, 24, 40, 40));
      return p;
    };
    // (32,32) is inside the inner square; (12,12) is between the two.
    const std::vector<PixelCoord> probes{{32, 32}, {12, 12}};

    Document nz = vectorOnlyDoc(kW2, kH2, donut(FillRule::NonZero), {1, 0, 0, 1}, "nonzero");
    Document eo = vectorOnlyDoc(kW2, kH2, donut(FillRule::EvenOdd), {1, 0, 0, 1}, "evenodd");

    const std::vector<float> nzBefore = coverageAt(nz.layers[0].shapes, kW2, kH2, probes);
    const std::vector<float> eoBefore = coverageAt(eo.layers[0].shapes, kW2, kH2, probes);
    check(nzBefore[0] > 0.9f && eoBefore[0] < 0.1f && nzBefore[1] > 0.9f && eoBefore[1] > 0.9f,
          "psd export: the two fill rules genuinely differ on this fixture -- solid vs donut");

    const PsdExportResult nzFile = writeLayeredPsd(nz);
    const PsdExportResult eoFile = writeLayeredPsd(eo);
    const PsdImportResult nzBack = importPsd(nzFile.bytes);
    const PsdImportResult eoBack = importPsd(eoFile.bytes);
    const bool bothBack =
        nzFile.ok && eoFile.ok && nzBack.ok && eoBack.ok &&
        nzBack.document.layers.size() == 1 && eoBack.document.layers.size() == 1 &&
        nzBack.document.layers[0].kind == LayerKind::Vector &&
        eoBack.document.layers[0].kind == LayerKind::Vector &&
        nzBack.document.layers[0].shapes.size() == 1 &&
        eoBack.document.layers[0].shapes.size() == 1;
    check(bothBack, "psd export: both fill-rule fixtures round-trip as Vector layers");
    if (bothBack) {
      const std::vector<float> nzAfter =
          coverageAt(nzBack.document.layers[0].shapes, kW2, kH2, probes);
      const std::vector<float> eoAfter =
          coverageAt(eoBack.document.layers[0].shapes, kW2, kH2, probes);
      check(std::fabs(nzAfter[0] - nzBefore[0]) < 0.02f &&
                std::fabs(nzAfter[1] - nzBefore[1]) < 0.02f,
            "psd export: a NonZero compound renders the same after the round trip (Union)");
      check(std::fabs(eoAfter[0] - eoBefore[0]) < 0.02f &&
                std::fabs(eoAfter[1] - eoBefore[1]) < 0.02f,
            "psd export: an EvenOdd compound renders the same after the round trip (Exclude)");
      check(eoBack.document.layers[0].shapes[0].path.rule == FillRule::EvenOdd,
            "psd export: and the EvenOdd rule itself comes back, not just its rendering");
    }
  }

  // --- H7. The SoCo bytes are Photoshop's own -----------------------------
  //
  // The strongest check available without Photoshop on the machine: the real
  // `SoCo` block Photoshop wrote for `App Icon Shape` in Apple's
  // `App Icon Template.psd`, black. The same literal as
  // app/selftest/PsdImport.cpp's `kRealSocoHex` -- duplicated rather than
  // shared because that file belongs to the reader and this assertion is the
  // writer's.
  //
  // It passes byte for byte, which settles both descriptor quirks at once: a
  // four-character Key writes a length of ZERO, and a UnicodeString's trailing
  // NUL is INSIDE its count. Either written the reasonable way produces a
  // different, still-parseable block and fails here.
  {
    const std::vector<uint8_t> real = hexBytes(
        "00000010000000010000000000006e756c6c0000000100000000436c72204f626a630000"
        "000100000000000052474243000000030000000052642020646f75620000000000000000"
        "0000000047726e20646f7562000000000000000000000000426c2020646f756200000000"
        "00000000");
    const std::vector<uint8_t> mine = encodePsdSolidColorBlock({0.0f, 0.0f, 0.0f, 1.0f});
    check(mine == real,
          "psd export: encodePsdSolidColorBlock(black) is byte-identical to Photoshop's own "
          "SoCo block");
  }

  // --- H8. What a shape layer still loses, named with numbers -------------
  //
  // A PSD shape layer is exactly one path and one fill. A core::Layer is a
  // list of shapes each with a fill, a stroke, a stroke style and a clip. Each
  // loss is warned about separately, so a user reading the report knows which
  // one bit -- PRD I11.
  {
    Document multi = vectorOnlyDoc(32, 32, Path{}, {1, 0, 0, 1}, "two shapes");
    multi.layers[0].shapes[0].path.subpaths.push_back(closedRect(2, 2, 14, 14));
    multi.layers[0].shapes[0].stroke.on = true;
    multi.layers[0].shapes[0].strokeStyle.width = 2.0f;
    VectorShape second;
    second.path.subpaths.push_back(closedRect(18, 18, 30, 30));
    second.fill.on = true;
    second.id = 2;
    multi.layers[0].shapes.push_back(std::move(second));

    const PsdExportResult r = writeLayeredPsd(multi);
    check(r.ok, "psd export: a Vector layer with two shapes and a stroke still writes");
    check(anyWarningContains(r.warnings, "two shapes") &&
              anyWarningContains(r.warnings, "1 of its 2"),
          "psd export: the warning names the layer and COUNTS the shapes PSD cannot carry");
    check(anyWarningContains(r.warnings, "its stroke is not carried"),
          "psd export: and names the stroke separately, rather than one blanket sentence");
  }
  {
    // A shape with no fill at all. `Paint::on == false` is genuinely "no
    // fill", which PSD records only in a `vstk` descriptor this build does not
    // write -- so no `SoCo` is emitted and the loss is named. A black `SoCo`
    // instead would paint a shape nobody authored, which is the
    // `PNG/4 - Layer.png` trap seen from the writing side.
    Document unfilled = vectorOnlyDoc(32, 32, Path{}, {1, 0, 0, 1}, "no fill");
    unfilled.layers[0].shapes[0].path.subpaths.push_back(closedRect(4, 4, 28, 28));
    unfilled.layers[0].shapes[0].fill.on = false;

    PsdLayerRecord rec;
    std::vector<std::string> warnings;
    buildPsdLayerRecord(unfilled.layers[0], unfilled, rec, warnings);
    check(containsBytes(rec.extraBlocks, {'8', 'B', 'I', 'M', 'v', 's', 'm', 's'}),
          "psd export: a fill-less shape still writes its geometry");
    check(!containsBytes(rec.extraBlocks, {'8', 'B', 'I', 'M', 'S', 'o', 'C', 'o'}),
          "psd export: ...and NO SoCo -- a black one would paint a shape nobody authored");
    check(anyWarningContains(warnings, "has no fill"),
          "psd export: and the report says so by name");
  }

  // --- I. `lclr`, the per-layer sheet colour ------------------------------
  //
  // Eight bytes: a u16 index then six zeros. Index 0 is unlabelled and 1..7
  // are Photoshop's menu order, which is `kLayerColorLabelNames`' order. The
  // expected bytes below are REAL, dumped from `App Icon Template.psd`, not
  // captured from this writer.
  {
    check(encodePsdLclrBlock(1) == hexBytes("0001000000000000") &&
              encodePsdLclrBlock(3) == hexBytes("0003000000000000") &&
              encodePsdLclrBlock(4) == hexBytes("0004000000000000"),
          "psd export: lclr's red/yellow/green payloads match the bytes Photoshop wrote");

    uint16_t index = 0;
    check(psdLayerColorLabelIndex("red", index) && index == 1,
          "psd export: 'red' is index 1 -- the array's position plus one");
    index = 0;
    check(psdLayerColorLabelIndex("grey", index) && index == 7,
          "psd export: 'grey' is index 7, the last of the seven");
    index = 99;
    check(!psdLayerColorLabelIndex("", index),
          "psd export: the empty label has no index -- it is the format's own 'no label'");
    check(!psdLayerColorLabelIndex("teal", index),
          "psd export: and a label outside the seven has none either, rather than a guess");
  }
  {
    Document doc3 = Document::createBlank(4, 4, WorkingSpace{});
    writeStraight(doc3, 0, 1, 1, 0.5f, 0.5f, 0.5f, 1.0f);

    PsdLayerRecord plain;
    std::vector<std::string> w0;
    buildPsdLayerRecord(doc3.layers[0], doc3, plain, w0);
    check(!containsBytes(plain.extraBlocks, {'8', 'B', 'I', 'M', 'l', 'c', 'l', 'r'}),
          "psd export: an unlabelled layer writes NO lclr block at all");

    doc3.layers[0].colorLabel = "green";
    PsdLayerRecord green;
    std::vector<std::string> w1;
    buildPsdLayerRecord(doc3.layers[0], doc3, green, w1);
    check(containsBytes(green.extraBlocks, hexBytes("3842494d6c636c72000000080004000000000000")),
          "psd export: a green layer writes 8BIM/lclr, length 8, index 4 -- the whole framed "
          "block, byte for byte");
    check(!anyWarningContains(w1, "colour label"),
          "psd export: and a label this build knows is not warned about");

    doc3.layers[0].colorLabel = "teal";
    PsdLayerRecord teal;
    std::vector<std::string> w2;
    buildPsdLayerRecord(doc3.layers[0], doc3, teal, w2);
    check(!containsBytes(teal.extraBlocks, {'8', 'B', 'I', 'M', 'l', 'c', 'l', 'r'}),
          "psd export: a label outside the seven writes no block rather than a guessed index");
    check(anyWarningContains(w2, "teal"),
          "psd export: and the warning names the label, so the loss is actionable");
  }

  // --- J. The encoder against the decoder it inverts ----------------------
  //
  // io/PsdVectorPath's `decodePsdPathRecords()` is the function
  // `encodePsdVectorShapeMask()` inverts, so the two are checked against each
  // other directly as well as through a whole file -- a framing error inside
  // the block then shows here, in eight lines, rather than as a missing layer.
  {
    Path p;
    p.subpaths.push_back(closedRect(10, 20, 300, 400));
    const PsdVectorShapeMask mask = encodePsdVectorShapeMask(p, 1024, 768);
    check(!mask.bytes.empty() && mask.subpathsWritten == 1 && mask.saturatedCoords == 0,
          "psd path encode: a simple rectangle encodes to one subpath and saturates nothing");
    // 8-byte header, two all-zero fill-rule records, one length record, four
    // knots -- and padded to a multiple of 4, which is where Photoshop's own
    // `192 = 8 + 7*26 + 2` comes from.
    check(mask.bytes.size() == 8 + 7 * kPsdPathRecordBytes + 2,
          "psd path encode: 8 + 7 records + 2 pad bytes, exactly Photoshop's own arithmetic");
    check(mask.bytes.size() % 2 == 0,
          "psd path encode: ...and even, which the extra-block walk needs");

    PsdPathStream stream;
    std::string err;
    const bool decoded = decodePsdPathRecords(mask.bytes, 1024, 768, stream, err);
    check(decoded && stream.warnings.empty() && stream.subpaths.size() == 1,
          "psd path encode: our own decoder reads it back with no warnings");
    if (decoded && stream.subpaths.size() == 1) {
      check(stream.subpaths[0].op == PsdPathOp::Union && stream.subpaths[0].opKnown,
            "psd path encode: NonZero was written as Union, and reads back as one");
      check(stream.subpaths[0].sub.closed && stream.subpaths[0].sub.anchors.size() == 4,
            "psd path encode: four closed knots, with no repeated final anchor");
    }

    // The saturation guard: a point past 128 document-widths clamps rather
    // than wrapping, and says how many did.
    Path far;
    far.subpaths.push_back(closedRect(0, 0, 1e9f, 10));
    const PsdVectorShapeMask sat = encodePsdVectorShapeMask(far, 1024, 768);
    check(sat.saturatedCoords > 0,
          "psd path encode: a point 128+ canvases off the page saturates and is counted");

    // A subpath of one anchor encloses nothing and writes no block at all.
    Path thin;
    SubPath one;
    one.closed = true;
    one.anchors.push_back(anchorAt(1, 1, 1, 1, 1, 1));
    thin.subpaths.push_back(one);
    const PsdVectorShapeMask none = encodePsdVectorShapeMask(thin, 64, 64);
    check(none.bytes.empty() && none.subpathsWritten == 0,
          "psd path encode: a one-anchor subpath encloses nothing and writes no block");
  }

  return ok;
}

}  // namespace np
