#include "app/selftest/Support.hpp"

namespace np {

bool runProbeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // Byte-quantization tolerance, same magnitude as runImageIOTest()'s kTol8 --
  // every value checked here passed through an 8-bit-per-channel PNG fixture.
  constexpr float kTol8 = 0.01f;

  // --- fixture: a 3x3, fully-opaque PNG with distinct, known bytes per
  // pixel, laid out row-major (row0 = y=0, etc.) so both a point sample and
  // an NxN box average have a hand-computable expectation. Opaque (alpha =
  // 255) deliberately, so premultiplied == straight here -- this fixture is
  // about sample-size averaging, not premultiply; that gets its own,
  // separately alpha < 1 fixture below (runImageIOTest()'s own precedent:
  // keep premultiply out of the arithmetic when a test isn't about it) -----
  const std::optional<Document> gridOpt = [] {
    const uint8_t px[3 * 3 * 4] = {
        10,  20,  30,  255,  40,  50,  60,  255,  70,  80,  90,  255,
        100, 110, 120, 255,  130, 140, 150, 255,  160, 170, 180, 255,
        190, 200, 210, 255,  220, 230, 240, 255,  250, 5,   15,  255,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 3, 3, 4, px, 3 * 4);
    return openImageAsDocument(png.data(), png.size());
  }();
  check(gridOpt.has_value(), "runProbeTest: 3x3 grid fixture PNG decodes");
  if (!gridOpt) {
    std::printf("[selftest] pixel probe %s\n", ok ? "PASS" : "FAIL");
    return ok;
  }
  const Document& grid = *gridOpt;

  // srgbDecode() of a known byte, exactly the way this fixture's expected
  // values are derived everywhere below -- same technique runImageIOTest()
  // already uses for its own expected values, rather than pre-baking
  // decoded floats by hand.
  auto dec = [](uint8_t byte) { return srgbDecode(byte / 255.0f); };

  // --- point sample (sampleSize=1): returns the exact stored pixel, both
  // linear and display-encoded -------------------------------------------
  {
    ProbeParams p;
    p.sampleSize = 1;
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, p);
    const float rLin = dec(130), gLin = dec(140), bLin = dec(150);
    check(near(s.linear[0], rLin, kTol8) && near(s.linear[1], gLin, kTol8) &&
              near(s.linear[2], bLin, kTol8) && near(s.linear[3], 1.0f, kTol8),
          "probePixel: point sample returns the exact known linear pixel value");
    check(near(s.display[0], srgbEncode(rLin), kTol8) &&
              near(s.display[1], srgbEncode(gLin), kTol8) &&
              near(s.display[2], srgbEncode(bLin), kTol8) && near(s.display[3], 1.0f, kTol8),
          "probePixel: point sample's display value is srgbEncode() of its linear value");
    // The fixture's own byte was itself sRGB-encoded, so encode(decode(x))
    // should land back near the original normalized byte -- a second,
    // independent check that display isn't accidentally returning the
    // linear value unencoded (encode and decode are different curves away
    // from 0, so a bug here wouldn't pass the check above by accident).
    check(near(s.display[0], 130 / 255.0f, kTol8) && near(s.display[1], 140 / 255.0f, kTol8) &&
              near(s.display[2], 150 / 255.0f, kTol8),
          "probePixel: point sample's display value round-trips back to the source byte");
  }

  // --- NxN box average, fully-painted interior: sampleSize=3 centred on
  // the fixture's own centre pixel covers exactly its 9 texels, all
  // painted -- isolates averaging correctness from any edge/bounds
  // behaviour (that's the next block) ---------------------------------
  {
    ProbeParams p;
    p.sampleSize = 3;
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, p);
    const uint8_t rBytes[9] = {10, 40, 70, 100, 130, 160, 190, 220, 250};
    const uint8_t gBytes[9] = {20, 50, 80, 110, 140, 170, 200, 230, 5};
    const uint8_t bBytes[9] = {30, 60, 90, 120, 150, 180, 210, 240, 15};
    float rSum = 0, gSum = 0, bSum = 0;
    for (int i = 0; i < 9; ++i) {
      rSum += dec(rBytes[i]);
      gSum += dec(gBytes[i]);
      bSum += dec(bBytes[i]);
    }
    const float rAvg = rSum / 9.0f, gAvg = gSum / 9.0f, bAvg = bSum / 9.0f;
    check(near(s.linear[0], rAvg, kTol8) && near(s.linear[1], gAvg, kTol8) &&
              near(s.linear[2], bAvg, kTol8) && near(s.linear[3], 1.0f, kTol8),
          "probePixel: 3x3 box average over a fully-painted fixture matches the hand-computed "
          "per-channel mean, not a single sample or an edge-clamped value");
    // Genuinely distinct from the centre pixel's own point-sample value
    // (checked above) -- proves this is actually averaging the box, not
    // just re-reading the centre texel under a different sampleSize.
    check(!near(s.linear[0], dec(130), 0.02f),
          "probePixel: the 3x3 average genuinely differs from the centre pixel's own value");
  }

  // --- NxN box at the document's own corner: sampleSize=3 centred on (0,0)
  // asks for x/y in [-1,1], of which only the 2x2 quadrant ((0,0),(1,0),
  // (0,1),(1,1)) is inside a 3x3 document at all. The colour is the straight
  // average of those four, not a darker value and not an edge-repeated one --
  // and the alpha is 1.0, because all four are opaque and the five texels
  // outside the canvas are **not averaged in** -----------------------------
  {
    ProbeParams p;
    p.sampleSize = 3;
    const ProbeSample s = probePixel(grid, PixelCoord{0, 0}, p);
    const float rAvg = (dec(10) + dec(40) + dec(100) + dec(130)) / 4.0f;
    const float gAvg = (dec(20) + dec(50) + dec(110) + dec(140)) / 4.0f;
    const float bAvg = (dec(30) + dec(60) + dec(120) + dec(150)) / 4.0f;
    check(near(s.linear[0], rAvg, kTol8) && near(s.linear[1], gAvg, kTol8) &&
              near(s.linear[2], bAvg, kTol8),
          "probePixel: a box straddling unpainted texels keeps the un-premultiplied colour at "
          "the painted texels' own average, not darkened toward black");
    // **This assertion used to read `4.0f / 9.0f` and it was canonising a
    // bug.** The old sample box was never clipped to the document and the
    // divisor was an unconditional sampleSize*sampleSize, so the five texels
    // this box asks for *outside a 3x3 canvas* were averaged in as
    // {0,0,0,0} and dragged the reported coverage of a fully opaque corner
    // down to 0.44. The colour was unaffected (premultiplied averaging
    // divides the colour sum by the alpha sum, so the zeros cancel), which is
    // exactly why nobody saw it. The property the old line was really after
    // -- that unpainted texels dilute coverage rather than being skipped or
    // edge-repeated -- is genuinely true and is asserted where it belongs, on
    // texels that are unpainted but *inside* the document, in
    // runEyedropperTest(). Here the honest expectation is 1.0.
    check(near(s.linear[3], 1.0f, kTol8),
          "probePixel: a box hanging off the document's corner averages only the texels that "
          "are actually in the document -- an opaque corner reads alpha 1.0, not diluted by "
          "the off-canvas half of the box");
  }

  // --- translucent pixel: proves un-premultiplication actually ran, the
  // same "check against the raw stored value, not just a plausible number"
  // discipline runImageIOTest()'s own premultiply checks use -------------
  {
    const uint8_t px[1 * 1 * 4] = {60, 120, 180, 90};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 4, px, 4);
    const std::optional<Document> docOpt = openImageAsDocument(png.data(), png.size());
    check(docOpt.has_value(), "runProbeTest: translucent 1x1 fixture decodes");
    if (docOpt && docOpt->layers.size() == 1 && docOpt->layers[0].rgbTiles) {
      const ProbeSample s = probePixel(*docOpt, PixelCoord{0, 0});
      const float a = 90 / 255.0f;
      const float rLin = dec(60), gLin = dec(120), bLin = dec(180);
      check(near(s.linear[0], rLin, kTol8) && near(s.linear[1], gLin, kTol8) &&
                near(s.linear[2], bLin, kTol8) && near(s.linear[3], a, kTol8),
            "probePixel: translucent pixel's reported linear colour is the straight (source) "
            "value, not the premultiplied one");

      const Tile* tile = docOpt->layers[0].rgbTiles->find(TileCoord{0, 0});
      check(tile != nullptr, "runProbeTest: translucent fixture's tile exists");
      if (tile) {
        const auto raw = tile->readPixel(PixelCoord{0, 0});
        check(near(raw[0], rLin * a, kTol8) && near(raw[2], bLin * a, kTol8),
              "runProbeTest: sanity check -- the tile itself really does store rgb*a "
              "premultiplied (same fact runImageIOTest() already covers)");
        // The actual "prove un-premultiply ran" assertion: the reported
        // straight colour must genuinely differ from the raw premultiplied
        // storage, not merely be plausible. This fixture's alpha (90/255 ~
        // 0.353) means straight = premultiplied / 0.353 ~ premultiplied *
        // 2.83, so every channel's true gap is well above float/quantization
        // noise -- red, the smallest, is still ~0.029 (srgbDecode(60/255) ~
        // 0.045 vs. its raw premultiplied ~0.016) -- but comfortably below
        // kTol8 (0.01)'s own quantization allowance would be too loose here,
        // so this uses a tighter 0.015 margin instead of runImageIOTest()'s
        // 0.05 (that test's fixture has brighter channels and a bigger gap).
        check(!near(s.linear[0], raw[0], 0.015f) && !near(s.linear[2], raw[2], 0.015f),
              "probePixel: reported colour genuinely differs from the raw premultiplied tile "
              "value -- proves un-premultiplication ran, not just alpha passthrough");
      }
    }
  }

  // --- the three ProbeSource modes on a ONE-layer document, where they are
  // required to agree. This fixture has a single, fully opaque RGB layer at
  // index 0, so "what is on the active layer", "what does the whole stack
  // show" and "what does the stack up to and including the active layer
  // show" are the same question -- and all three must answer it identically,
  // which is what proves the three arms share one box arithmetic rather than
  // each having grown their own. The modes *differing* on a stack built so
  // they must is runEyedropperTest()'s job, not this one's ------------------
  {
    ProbeParams single;
    single.sampleSize = 3;
    single.source = ProbeSource::CurrentLayer;
    ProbeParams all;
    all.sampleSize = 3;
    all.source = ProbeSource::AllLayers;
    ProbeParams below;
    below.sampleSize = 3;
    below.source = ProbeSource::ActiveAndBelow;
    const ProbeSample sSingle = probePixel(grid, PixelCoord{1, 1}, single);
    const ProbeSample sAll = probePixel(grid, PixelCoord{1, 1}, all);
    const ProbeSample sBelow = probePixel(grid, PixelCoord{1, 1}, below);
    check(near(sSingle.linear[0], sAll.linear[0], 1e-6f) &&
              near(sSingle.linear[1], sAll.linear[1], 1e-6f) &&
              near(sSingle.linear[2], sAll.linear[2], 1e-6f) &&
              near(sSingle.linear[3], sAll.linear[3], 1e-6f) &&
              near(sBelow.linear[0], sAll.linear[0], 1e-6f) &&
              near(sBelow.linear[1], sAll.linear[1], 1e-6f) &&
              near(sBelow.linear[2], sAll.linear[2], 1e-6f) &&
              near(sBelow.linear[3], sAll.linear[3], 1e-6f),
          "probePixel: all three ProbeSource modes are wired through and agree on a "
          "single-opaque-layer Document, where they are asking the same question");
  }

  // --- out-of-bounds / never-painted / misuse: all sane, documented,
  // never a crash or garbage read -----------------------------------------
  {
    const ProbeSample farAway = probePixel(grid, PixelCoord{10000, -10000});
    check(near(farAway.linear[0], 0.0f, 1e-6f) && near(farAway.linear[1], 0.0f, 1e-6f) &&
              near(farAway.linear[2], 0.0f, 1e-6f) && near(farAway.linear[3], 0.0f, 1e-6f) &&
              near(farAway.display[0], 0.0f, 1e-6f) && near(farAway.display[3], 0.0f, 1e-6f),
          "probePixel: a far-away, never-painted coordinate reads back fully transparent "
          "black, not a crash or garbage value");

    Document empty;
    const ProbeSample noLayers = probePixel(empty, PixelCoord{0, 0});
    check(near(noLayers.linear[3], 0.0f, 1e-6f),
          "probePixel: a Document with no layers at all is a safe no-op, not a crash");

    const Document blank = Document::createBlank(64, 64, WorkingSpace{});
    const ProbeSample noTiles = probePixel(blank, PixelCoord{5, 5});
    check(near(noTiles.linear[3], 0.0f, 1e-6f),
          "probePixel: a createBlank()'d Document (RGB layer present, zero tiles painted) "
          "reads back fully transparent black");

    ProbeParams badIndex;
    badIndex.activeLayerIndex = 5;  // grid only has one layer, index 0
    const ProbeSample s = probePixel(grid, PixelCoord{1, 1}, badIndex);
    check(near(s.linear[3], 0.0f, 1e-6f),
          "probePixel: an out-of-range activeLayerIndex is a safe no-op, not a crash");

    ProbeParams zeroSize;
    zeroSize.sampleSize = 0;
    const ProbeSample s2 = probePixel(grid, PixelCoord{1, 1}, zeroSize);
    check(near(s2.linear[0], dec(130), kTol8) && near(s2.linear[3], 1.0f, kTol8),
          "probePixel: sampleSize <= 0 is clamped up to 1 (a point sample), not a crash or "
          "divide-by-zero");
  }

  std::printf("[selftest] pixel probe %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
