#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "color/Space.hpp"
#include "core/Probe.hpp"
#include "core/ResourcePaths.hpp"
#include "imgui.h"
#include "paint/Palette.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {

// ---------------------------------------------------------------------------
// T25a -- the scene-referred foreground colour.
//
// **The report this answers**: "the canvas supports fp16 data, but the colour
// picker only shows values clamped to 1, what can we do about it?" The answer
// taken is *scene-referred*: a value above white is a real measurement, it
// must survive, and it must be readable. The answer explicitly NOT taken is
// EDR output -- the vendored `webgpu.h` has no colour-space field on
// `WGPUSurfaceConfiguration` at all and the surface is `BGRA8Unorm`, so
// nothing in this build can make the monitor brighter and nothing below
// pretends otherwise.
//
// **What made this a design question rather than a deletion.** The clamp that
// destroyed the information (`ui/MacPaintUI.cpp`'s eyedropper) was documented
// and its comment made a correct argument: three places downstream clamp
// anyway, so clamping at the source meant "the number the picker shows is the
// number the next stroke uses, instead of a fourth value only the clamps know
// about." Deleting the clamp alone would have produced exactly that fourth
// value. So the fix is the whole set: one field holds the truth
// (`BrushState::rgb`, contract in app/AppState.hpp), the clamps move to the
// destinations that genuinely cannot carry it and are **named** there
// (`color/Space.hpp`'s `clampToDisplayRange()`), and the UI says which routes
// clamp.
//
// This section is the tripwire for every clause of that. The assertions are
// grouped by the claim they defend, and each group is written so it can fail:
//
//   1. The transfer curves round-trip an over-range value. If they did not,
//      the single-field design would be impossible and no amount of UI would
//      rescue it.
//   2. The named display-range predicate and clamp mean what the header says,
//      including at the boundary (1.0 is IN range, not over it) and below zero.
//   3. **The eyedropper preserves a >1 sample** -- the concrete bug in the
//      report, asserted against an independently computed encode rather than
//      against the probe's own output.
//   4. The two decoders still agree, ON AN OVER-RANGE VALUE. `app/AppState.hpp`
//      has always promised that `foregroundLinearRgba()` and `brushTipFor()`'s
//      `tip.linearRgb` agree, and `--selftest` has always asserted it -- but
//      only over in-range colours, where a stray clamp in either one would
//      have been invisible. This is the same assertion in the range where it
//      can actually catch one.
//   5. **The pigment route clamps, deliberately and provably**, on a real
//      512x512 LUT rather than on the no-LUT fallback (which would make the
//      assertion inert: an invalid LUT returns a zero Latent for every input,
//      so "over-range and clamped give the same answer" would be true of a
//      function that ignored its arguments). The fallback arm is asserted
//      separately, because it is a second copy of the same policy.
//   6. An RGB-layer stroke does NOT clamp -- the other half of what the panel
//      promises the user. A section that only proved the clamps would pass
//      just as happily on a build that clamped everything.
bool runSceneReferredColourTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] scene-referred colour: over-range foreground, named clamps, "
              "eyedropper preservation\n");

  // Relative, because the values under test span 0.25 to 12.0 and one
  // absolute epsilon cannot be right for both ends. `srgbEncode`/`srgbDecode`
  // are a pow() pair, so a round trip is accurate to a few ulps of the
  // magnitude rather than of 1.0.
  auto nearRel = [](float got, float want, float rel) {
    return std::fabs(got - want) <= rel * std::max(1.0f, std::fabs(want));
  };
  constexpr float kRoundTrip = 1.0e-5f;
  constexpr float kExact = 0.0f;

  // Same helper, same wording, as runEyedropperTest()'s: the tile store is
  // PREMULTIPLIED, so a straight value is written through `rgb *= a`.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };

  // =======================================================================
  // 1. The curves carry an over-range value, both ways
  // =======================================================================
  //
  // This is the load-bearing property of the whole design: because
  // `srgbEncode`/`srgbDecode` are unclamped and monotonic, ONE field can be
  // both "sRGB-encoded" (what the palette, the swatch and the LUT want) and
  // "scene-referred" (what a HALF texel holds). If a clamp were ever added
  // to either curve "for safety", the foreground would silently stop being
  // able to hold what the eyedropper reads, and every assertion further down
  // would fail with it -- which is the point of asserting the curves first.
  {
    bool roundTrips = true;
    bool encodesOverOne = true;
    bool monotonic = true;
    float previous = -1.0f;
    const float kLinears[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.5f, 4.0f, 12.0f};
    for (const float linear : kLinears) {
      const float encoded = srgbEncode(linear);
      if (!nearRel(srgbDecode(encoded), linear, kRoundTrip)) roundTrips = false;
      if (linear > 1.0f && !(encoded > 1.0f)) encodesOverOne = false;
      if (!(encoded > previous)) monotonic = false;
      previous = encoded;
    }
    check(roundTrips,
          "curves: srgbEncode/srgbDecode round-trip 0.25 through 12.0 -- the property that "
          "lets ONE field be both sRGB-encoded and scene-referred");
    check(encodesOverOne && monotonic,
          "curves: and the encoding of a linear value above 1.0 is itself above 1.0, and "
          "strictly increasing, so \"over range\" is the same question either side of it");
    // **White must not light the over-range badge, and in `float` that is a
    // measurement rather than an algebraic fact.** `srgbEncode(1.0)` is
    // `1.055 * 1^(1/2.4) - 0.055`, which is 1.0 exactly in real arithmetic
    // and 0.99999994 in single precision -- one ulp BELOW, measured here and
    // printed rather than asserted as an equality that is not true.
    //
    // The direction is the whole point and is what is asserted: one ulp
    // below leaves plain white inside the display range, and one ulp *above*
    // would put a badge and a warning line on every fully-white foreground in
    // the program. `exceedsDisplayRange()` is strict-greater for the same
    // reason, and the two facts together are what make "the badge means
    // something unusual happened" true. If a future revision of these curves
    // (a different constant, a fused multiply-add, a different pow) pushed
    // that ulp the other way, this is the assertion that would say so.
    const float whiteEncoded = srgbEncode(1.0f);
    std::printf("  [measured] srgbEncode(1.0f) = %.9g (1.0f %+.3e), srgbDecode(1.0f) = %.9g\n",
                static_cast<double>(whiteEncoded),
                static_cast<double>(whiteEncoded - 1.0f),
                static_cast<double>(srgbDecode(1.0f)));
    check(whiteEncoded <= 1.0f && nearRel(whiteEncoded, 1.0f, 1.0e-6f) &&
              !exceedsDisplayRange(std::array<float, 3>{whiteEncoded, whiteEncoded,
                                                        whiteEncoded}),
          "curves: white encodes to 1.0 or a hair under it, never over -- so a plain white "
          "foreground never lights the over-range badge on a rounding error");
    // Negatives mirror rather than clipping (color/Space.hpp), so the
    // pipeline can genuinely hold one and it dies at the same destinations.
    check(srgbEncode(-0.5f) < 0.0f && nearRel(srgbDecode(srgbEncode(-0.5f)), -0.5f, kRoundTrip),
          "curves: negatives mirror rather than clip, so the range this contract has to "
          "describe is (-inf, +inf) and not [0, inf)");
  }

  // =======================================================================
  // 2. The named display range: the predicate and the clamp
  // =======================================================================
  {
    check(!exceedsDisplayRange(std::array<float, 3>{1.0f, 1.0f, 1.0f}) &&
              !exceedsDisplayRange(std::array<float, 3>{0.0f, 0.5f, 1.0f}),
          "range: a value AT the ceiling or the floor is in range -- white and black are not "
          "over-range colours and must not be badged as ones");
    check(exceedsDisplayRange(std::array<float, 3>{1.0f, 1.0f, 1.0001f}) &&
              exceedsDisplayRange(std::array<float, 3>{-0.0001f, 0.5f, 0.5f}),
          "range: and a value a ten-thousandth past either end is over it, on ANY channel -- "
          "the predicate is an or over three, not a test of the red one");
    const std::array<float, 3> over{2.5f, 0.5f, -0.25f};
    const std::array<float, 3> clamped = clampToDisplayRange(over);
    check(clamped[0] == 1.0f && clamped[1] == 0.5f && clamped[2] == 0.0f,
          "range: clampToDisplayRange() clips PER CHANNEL and leaves in-range channels alone "
          "-- it is not a scale-to-fit and must never become a tone-mapper");
    check(clampToDisplayRange(clamped) == clamped && !exceedsDisplayRange(clamped),
          "range: and its output is in range and idempotent, so a second clamp downstream of "
          "a first cannot move a colour again");
  }

  // =======================================================================
  // 2b. The picker's numeric row is not a clamp
  // =======================================================================
  //
  // **A deliberately weak assertion, kept because nothing else can hold this
  // ground.** `ImGuiColorEditFlags_HDR` is what removes the `[0,1]` bound
  // from `ColorEdit4()`'s DragFloats, and without it the first drag of any
  // channel after an over-range pick pulls the whole triple back into range.
  // Neither harness can see that: `--selftest` has no ImGui frame at all, and
  // the clamp only runs while `g.ActiveId` is the drag (`DragBehavior()`'s own
  // guard), so a golden screenshot with no input is byte-identical with the
  // flag and without it. That is measured rather than assumed -- the flag was
  // removed on purpose and the `color_overrange` view passed unchanged.
  //
  // So this asserts the bit is in the named set, which can only fail if
  // someone edits `rgbColorPickerFlags()` -- and editing it is precisely the
  // regression. A weak tripwire over a real hole beats no tripwire.
  {
    const int flags = rgbColorPickerFlags();
    check((flags & ImGuiColorEditFlags_HDR) != 0,
          "picker: the RGB picker is drawn WITH ImGuiColorEditFlags_HDR, so its numeric row "
          "is not a fourth clamp on an over-range foreground");
    check((flags & ImGuiColorEditFlags_Float) != 0 &&
              (flags & ImGuiColorEditFlags_DisplayRGB) != 0,
          "picker: and with Float + DisplayRGB, so that row prints 1.516 rather than an "
          "8-bit 255 that could not express an over-range value at all");
  }

  // =======================================================================
  // 3. The eyedropper preserves a sample brighter than white
  // =======================================================================
  //
  // The concrete bug behind the report. `ui/MacPaintUI.cpp`'s pick used to
  // read `std::clamp(out.sample.display[c], 0.0f, 1.0f)`; picking a 2.5 gave
  // back a 1.0 and nothing said a number had been destroyed.
  //
  // The expected value is `srgbEncode()` of the fixture's own straight
  // channels, computed here -- NOT read out of `out.sample.display`, which
  // would make this an assertion that the pick copied a struct field rather
  // than that it kept the measurement.
  {
    AppState st;
    OpenDocument od;
    od.document = Document::createBlank(8, 8, WorkingSpace{});
    // 2.5 / 0.5 / 4.0: one channel over, one comfortably in range, one much
    // further over. The in-range channel is there so a blanket "multiply
    // everything" bug cannot pass, and the two over-range channels differ so
    // a clamp to some other constant cannot pass either. Alpha is 1.0 so the
    // premultiply above is the identity and the un-premultiply on read
    // cannot be what is being tested here.
    writeStraight(od.document, 0, 3, 3, 2.5f, 0.5f, 4.0f, 1.0f);
    od.activeLayer = 0;
    st.documents.add(std::move(od));

    const EyedropperPick pick = applyEyedropperPick(st, PixelCoord{3, 3});
    check(pick.applied, "eyedropper: a pick on an over-range texel is applied like any other");
    check(nearRel(st.brush.rgb[0], srgbEncode(2.5f), kRoundTrip) &&
              nearRel(st.brush.rgb[1], srgbEncode(0.5f), kRoundTrip) &&
              nearRel(st.brush.rgb[2], srgbEncode(4.0f), kRoundTrip),
          "eyedropper: the picked triple lands in BrushState::rgb UNCLAMPED -- this is the "
          "line the report was about, and it used to answer 1.000");
    check(st.brush.rgb[0] > 1.0f && st.brush.rgb[2] > st.brush.rgb[0] &&
              st.brush.rgb[1] < 1.0f,
          "eyedropper: two channels are above 1.0 and DIFFER from each other, so a clamp to "
          "any single constant fails here rather than passing on a coincidence");
    check(pick.overRange && exceedsDisplayRange(st.brush.rgb),
          "eyedropper: the pick reports the over-range STATE, not just a large number -- the "
          "options bar has to be able to say the swatch has stopped agreeing with the field");
    check(pick.report.find("Above the display range") != std::string::npos &&
              st.lastPickReport == pick.report,
          "eyedropper: and it SAYS so in the sentence the options bar shows, rather than "
          "leaving a swatch that is a lie by necessity to be discovered");
    check(foregroundSrgb(st.brush) == st.brush.rgb,
          "eyedropper: foregroundSrgb() -- the one answer every consumer reads -- returns the "
          "over-range triple, so there is no second, quietly-clamped foreground");

    // ---------------------------------------------------------------------
    // 4. The two decoders still agree, in the range where it can now fail
    // ---------------------------------------------------------------------
    const std::array<float, 4> lin = foregroundLinearRgba(st.brush);
    check(nearRel(lin[0], 2.5f, kRoundTrip) && nearRel(lin[1], 0.5f, kRoundTrip) &&
              nearRel(lin[2], 4.0f, kRoundTrip) && lin[3] == 1.0f,
          "decoders: foregroundLinearRgba() decodes the over-range foreground back to the "
          "document's own linear values, so the bucket and the gradient fill with 2.5");
    MixboxLut noLut;
    const BrushTip fallbackTip = brushTipFor(st.brush, noLut, 1.0f);
    check(std::fabs(fallbackTip.linearRgb[0] - lin[0]) <= kExact &&
              std::fabs(fallbackTip.linearRgb[1] - lin[1]) <= kExact &&
              std::fabs(fallbackTip.linearRgb[2] - lin[2]) <= kExact,
          "decoders: brushTipFor()'s linearRgb is BIT-IDENTICAL to it above 1.0 too -- the "
          "existing agreement assertion, in the range a stray clamp could hide in");
    check(fallbackTip.linearRgb[0] > 1.0f && fallbackTip.linearRgb[2] > 1.0f,
          "decoders: and they agree on a value that is genuinely over range, so this cannot "
          "pass by both of them clamping to the same 1.0");

    // ---------------------------------------------------------------------
    // 5b. The no-LUT fallback clamps the PIGMENT half (and only that half)
    // ---------------------------------------------------------------------
    //
    // `brushTipFor()`'s else-arm copies the sRGB triple into `Latent::c`.
    // Before T25a that copy was verbatim, so an over-range foreground put a
    // weight above 1 into a latent that `mixLatents()` then lerps against
    // real ones -- a divergence between the two arms of one branch, in the
    // build configuration least likely to have it noticed.
    check(!exceedsDisplayRange(fallbackTip.pigment.c) && fallbackTip.pigment.c[0] == 1.0f &&
              fallbackTip.pigment.c[2] == 1.0f,
          "pigment: the no-LUT fallback CLAMPS its latent, so both arms of that branch agree "
          "that pigment is bounded even though only one of them calls the LUT");
    check(fallbackTip.pigment.c[1] == st.brush.rgb[1],
          "pigment: and it clamps rather than rescaling -- the in-range channel is passed "
          "through untouched, so the fallback is still the colour it always was");
  }

  // =======================================================================
  // 5. The pigment route clamps, on a REAL LUT
  // =======================================================================
  //
  // Deliberately not on `MixboxLut noLut`. Under NP_USE_MIXBOX an unloaded
  // LUT returns a zero `Latent` for every input, so "over-range and clamped
  // give the same answer" would be a true statement about a function that
  // ignored its arguments -- an inert assertion wearing the shape of a real
  // one. So this loads the file, and if it cannot be loaded it says so and
  // asserts nothing, rather than quietly asserting nothing.
  {
    MixboxLut lut;
    const std::string path = mixboxLutPath();
    if (!lut.load(path)) {
      std::printf("  %-58s %s\n", "pigment: mixbox LUT not loadable -- section skipped", "pass");
      std::printf("    (looked in %s; the clamp assertions below need a real LUT because an\n"
                  "     unloaded one answers the same zero Latent for every input)\n",
                  path.c_str());
    } else {
      const std::array<float, 3> over{1.4f, 0.5f, 2.0f};
      const std::array<float, 3> clamped = clampToDisplayRange(over);
      const Latent zOver = lut.rgbToLatent(over[0], over[1], over[2]);
      const Latent zClamped = lut.rgbToLatent(clamped[0], clamped[1], clamped[2]);
      check(zOver.c == zClamped.c && zOver.res == zClamped.res,
            "pigment: rgbToLatent() answers an over-range triple exactly as it answers the "
            "clamped one -- the clamp is real, deliberate, and inside the engine");
      // The inert-assertion guard, stated as an assertion of its own: the LUT
      // must actually distinguish colours, or the equality above proves
      // nothing. `latentToRgb`'s residual makes the round trip exact, so a
      // different in-range colour must give a different latent.
      const Latent zOther = lut.rgbToLatent(0.2f, 0.7f, 0.3f);
      check(!(zOther.c == zClamped.c && zOther.res == zClamped.res),
            "pigment: and this LUT genuinely distinguishes colours, so the equality above is "
            "a clamp rather than a function that ignores its arguments");
    }
  }

  // =======================================================================
  // 6. An RGB-layer stroke does NOT clamp
  // =======================================================================
  //
  // The other half of the promise the COLOR panel's badge makes ("swatch +
  // PIGMENT clamp to 1.000; RGB strokes keep it"). Asserted through a real
  // deposit into a real tile store rather than through `tip.linearRgb`
  // alone, because "the tip carried the value" and "the texel holds the
  // value" are two claims and the second is the one the user sees.
  {
    AppState st;
    st.brush.colorMode = ColorMode::Rgb;
    st.brush.rgb = {srgbEncode(3.0f), srgbEncode(0.25f), srgbEncode(1.0f)};
    check(exceedsDisplayRange(st.brush.rgb),
          "stroke: the fixture foreground is genuinely over range before the stroke, so the "
          "assertion below is about the deposit rather than about the fixture");

    MixboxLut noLut;
    BrushTip solid = brushTipFor(st.brush, noLut, 1.0f);
    solid.radius = 10.0f;
    solid.hardness = 1.0f;
    solid.flow = 1.0f;
    solid.opacity = 1.0f;

    TileStore store;
    RgbStroke stroke;
    stroke.begin(solid.linearRgb, solid.opacity);
    stroke.depositDab(store, solid, Vec2{64.0f, 64.0f}, 128, 128, nullptr, nullptr);
    stroke.end();

    const PixelCoord at{64, 64};
    const std::array<float, 4> texel =
        store.getOrCreate(tileCoordAt(at)).readPixel(tileLocalOffset(at));
    // The tolerance is HALF's, not float's: the tile store is f16, so 3.0
    // survives but the deposit's own multiply-accumulate is not obliged to
    // land on it bit for bit.
    check(texel[3] > 0.99f && nearRel(texel[0], 3.0f, 1.0e-2f),
          "stroke: an RGB-layer deposit of an over-range foreground writes 3.0 into the HALF "
          "texel -- the canvas holds what the picker now shows");
    check(texel[0] > 1.0f,
          "stroke: and it is genuinely above 1.0 in the document, which is the whole of what "
          "\"scene-referred\" buys and the reason the clamps had to move rather than stay");
  }

  std::printf("[selftest] scene-referred colour %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
