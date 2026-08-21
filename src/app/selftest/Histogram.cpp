#include "app/selftest/Support.hpp"

namespace np {

// PLAN.md Phase 3 step 7 ("Histogram over the visible region"). See
// SelfTest.hpp for the full breakdown; in short: empty/all-transparent,
// hand-computed per-channel bin placement (with a mixed-in alpha=0 texel
// that must contribute nothing), region clipping within one tile,
// HistogramParams::wholeDocument()'s exact span, and an un-premultiply
// proof on a translucent pixel -- mirroring runProbeTest()'s translucent-
// pixel discipline of checking against a specific hand-computed value, not
// just a plausible-looking one. Pure CPU -- computeHistogram() only ever
// reads a Document's tiles, no PaintSim or gpu involvement.
bool runHistogramTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- empty/all-transparent Document: createBlank() allocates zero tiles,
  // so every bin across all four channels stays zero and sampleCount is 0 --
  {
    const Document blank = Document::createBlank(64, 64, WorkingSpace{});
    const HistogramResult h = computeHistogram(blank, HistogramParams::wholeDocument(blank));
    check(h.sampleCount == 0,
          "computeHistogram: an all-transparent/unpainted Document reports sampleCount 0");
    bool allZero = true;
    for (uint64_t c : h.r) allZero = allZero && (c == 0);
    for (uint64_t c : h.g) allZero = allZero && (c == 0);
    for (uint64_t c : h.b) allZero = allZero && (c == 0);
    for (uint64_t c : h.luma) allZero = allZero && (c == 0);
    check(allZero, "computeHistogram: ...and every bin in all four channels is zero");
  }

  // --- hand-computed per-channel bin placement, plus a mixed-in alpha=0
  // texel that must contribute to nothing: pure red/green/blue opaque
  // texels at three distinct coordinates in one tile, plus a fourth,
  // alpha=0 texel carrying an otherwise-distinct colour that would be
  // trivially detectable if it leaked into any bin ------------------------
  {
    Document doc = Document::createBlank(32, 32, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{0, 0}, {1.0f, 0.0f, 0.0f, 1.0f});  // opaque red
    tile.writePixel(PixelCoord{1, 0}, {0.0f, 1.0f, 0.0f, 1.0f});  // opaque green
    tile.writePixel(PixelCoord{2, 0}, {0.0f, 0.0f, 1.0f, 1.0f});  // opaque blue
    tile.writePixel(PixelCoord{3, 0}, {0.5f, 0.5f, 0.5f, 0.0f});  // alpha 0 -- must not count

    const HistogramResult h = computeHistogram(doc, HistogramParams::wholeDocument(doc));

    check(h.sampleCount == 3,
          "computeHistogram: three opaque texels count, the alpha=0 texel does not");

    // srgbEncode(1.0) == 1.0 and srgbEncode(0.0) == 0.0 exactly (pow(x, *)
    // with x in {0,1} is exact in IEEE float), so every one of these lands
    // in bin 0 or bin (binCount-1) with no rounding ambiguity -- these are
    // exact integer equality checks, not tolerance-based.
    const size_t last = h.r.size() - 1;  // 255 at the default binCount == 256
    check(h.r[last] == 1 && h.r[0] == 2,
          "computeHistogram: R bin 255 holds the red texel; R bin 0 holds green+blue's R=0");
    check(h.g[last] == 1 && h.g[0] == 2,
          "computeHistogram: G bin 255 holds the green texel; G bin 0 holds red+blue's G=0");
    check(h.b[last] == 1 && h.b[0] == 2,
          "computeHistogram: B bin 255 holds the blue texel; B bin 0 holds red+green's B=0");

    // Luma: 0.2126 (red), 0.7152 (green), 0.0722 (blue) at binCount=256 ->
    // floor(x*256) = 54, 183, 18 -- each with a comfortable (>0.4-bin)
    // margin from the nearest integer boundary, so this is exact too.
    check(h.luma[54] == 1 && h.luma[183] == 1 && h.luma[18] == 1,
          "computeHistogram: Luma bins hold exactly the hand-computed Rec.709 luma bin for each "
          "of red/green/blue");
    uint64_t lumaTotal = 0;
    for (uint64_t c : h.luma) lumaTotal += c;
    check(lumaTotal == 3,
          "computeHistogram: no other Luma bin picked up a stray count -- exactly three "
          "qualifying texels total, matching sampleCount");
  }

  // --- region clipping: a region narrower than one allocated tile excludes
  // pixels inside that same tile but outside the region -------------------
  {
    Document doc = Document::createBlank(128, 128, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{5, 5}, {1.0f, 0.0f, 0.0f, 1.0f});    // inside the region below
    tile.writePixel(PixelCoord{50, 50}, {0.0f, 1.0f, 0.0f, 1.0f});  // same tile, outside it

    HistogramParams params;
    params.regionMin = PixelCoord{0, 0};
    params.regionMax = PixelCoord{10, 10};
    const HistogramResult h = computeHistogram(doc, params);

    check(h.sampleCount == 1,
          "computeHistogram: a region narrower than one tile counts only the texel inside it");
    const size_t last = h.r.size() - 1;
    check(h.r[last] == 1 && h.g[last] == 0,
          "computeHistogram: the in-region red texel is counted; the out-of-region green texel "
          "(same tile) is not");
  }

  // --- HistogramParams::wholeDocument(): spans exactly {0,0} to
  // {width,height}, and using it actually reaches a pixel at the document's
  // far corner (regionMax is exclusive, so this also proves the span is
  // {width,height}, not {width-1,height-1}) -------------------------------
  {
    Document doc = Document::createBlank(20, 15, WorkingSpace{});
    const HistogramParams params = HistogramParams::wholeDocument(doc);
    check(params.regionMin.x == 0 && params.regionMin.y == 0 && params.regionMax.x == 20 &&
              params.regionMax.y == 15,
          "HistogramParams::wholeDocument: spans exactly {0,0} to {width,height}");

    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{19, 14}, {1.0f, 1.0f, 1.0f, 1.0f});  // the document's far corner
    const HistogramResult h = computeHistogram(doc, params);
    check(h.sampleCount == 1,
          "computeHistogram: wholeDocument()'s region reaches the document's far corner pixel "
          "(19,14) -- proves regionMax is genuinely {width,height}, not {width-1,height-1}");
  }

  // --- translucent (partial-alpha) pixel bins at its un-premultiplied
  // straight colour, not its stored premultiplied value: premultiplied
  // (0.25, 0, 0, 0.5) un-premultiplies to linear (0.5, 0, 0) ---------------
  {
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    Tile& tile = doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{0, 0}, {0.25f, 0.0f, 0.0f, 0.5f});

    const HistogramResult h = computeHistogram(doc, HistogramParams::wholeDocument(doc));
    check(h.sampleCount == 1, "computeHistogram: the translucent texel counts exactly once");

    const float straightDisplay = srgbEncode(0.5f);         // expected: un-premultiplied
    const float premultipliedDisplay = srgbEncode(0.25f);   // what a premultiply bug would bin at
    const int32_t straightBin =
        std::clamp(static_cast<int32_t>(std::floor(straightDisplay * 256.0f)), 0, 255);
    const int32_t premultipliedBin =
        std::clamp(static_cast<int32_t>(std::floor(premultipliedDisplay * 256.0f)), 0, 255);
    check(straightBin != premultipliedBin,
          "runHistogramTest: sanity check -- srgbEncode(0.5) and srgbEncode(0.25) land in "
          "different bins, so this fixture can actually distinguish un-premultiplied from "
          "premultiplied binning");

    check(h.r[static_cast<size_t>(straightBin)] == 1,
          "computeHistogram: a translucent texel bins at srgbEncode() of its un-premultiplied "
          "straight colour -- the hand-computed premultiplied (0.25,0,0,0.5) -> straight "
          "(0.5,0,0) case");
    check(h.r[static_cast<size_t>(premultipliedBin)] == 0,
          "computeHistogram: ...and NOT at srgbEncode() of its raw stored premultiplied value "
          "(0.25) -- proves un-premultiplication actually ran, not just alpha passthrough");
    check(h.g[0] == 1 && h.b[0] == 1,
          "computeHistogram: the translucent texel's G/B channels (0 both pre- and "
          "post-un-premultiply) land in bin 0 either way");
  }

  std::printf("[selftest] histogram %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
