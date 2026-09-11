#include "app/selftest/Support.hpp"

#include "brush/BrushModelIo.hpp"
#include "brush/Library.hpp"
#include "brush/NativeBrush.hpp"

#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"

namespace np {

// brush/NativeBrush -- naturalPaint's own brush section (load, wetness,
// opacity, grain), beside `BrushModel` (Photoshop's). This is the dedicated
// selftest for the struct itself and for the migration that created it:
// `BrushModel` losing its own `load`/`wetness` leaves, `BrushPreset`/
// `BrushState` gaining one `native` member apiece in place of the four loose
// fields, and `user-presets.txt` round-tripping both the old (pre-`native`)
// format and the new one. See brush/NativeBrush.hpp's own header for the
// full argument for why this struct exists.
bool runNativeBrushTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ========================================================================
  std::printf("  -- A. nativeBrushEqual(): bit equality, one field at a time --\n");
  // ========================================================================
  {
    NativeBrush base;
    base.load = 0.63f;
    base.wetness = 1.05f;
    base.opacity = 0.82f;
    base.grain.enabled = true;
    base.grain.periodX = 30;
    base.grain.periodY = 18;
    base.grain.depth = 0.44f;
    base.grain.strength = 0.91f;

    NativeBrush same = base;
    check(nativeBrushEqual(base, same), "nativeBrushEqual: identical structs match");

    NativeBrush diffLoad = base;
    diffLoad.load = base.load + 0.01f;
    check(!nativeBrushEqual(base, diffLoad), "nativeBrushEqual: load alone differing -- no match");

    NativeBrush diffWetness = base;
    diffWetness.wetness = base.wetness + 0.01f;
    check(!nativeBrushEqual(base, diffWetness),
          "nativeBrushEqual: wetness alone differing -- no match");

    NativeBrush diffOpacity = base;
    diffOpacity.opacity = base.opacity + 0.01f;
    check(!nativeBrushEqual(base, diffOpacity),
          "nativeBrushEqual: opacity alone differing -- no match");

    NativeBrush diffGrain = base;
    diffGrain.grain.strength = base.grain.strength + 0.01f;
    check(!nativeBrushEqual(base, diffGrain),
          "nativeBrushEqual: grain alone differing (via grainParamsEqual) -- no match");
  }

  // ========================================================================
  std::printf("  -- B. BrushModel: load/wetness are gone, 149 leaves remain --\n");
  // ========================================================================
  {
    const std::vector<std::string> paths = brushModelFieldPaths();
    bool hasLoad = false, hasWetness = false;
    for (const std::string& p : paths) {
      if (p == "load") hasLoad = true;
      if (p == "wetness") hasWetness = true;
    }
    check(!hasLoad && !hasWetness,
          "brushModelFieldPaths: neither bare `load` nor bare `wetness` remains -- both moved "
          "to NativeBrush");
    check(paths.size() == 149,
          "brushModelFieldPaths: 149 leaves (151 before load/wetness left for NativeBrush)");
  }

  // ========================================================================
  std::printf("  -- C. applyPresetToBrush() / presetFromBrush(): native round-trips --\n");
  // ========================================================================
  {
    BrushPreset preset;
    preset.name = "Native Round Trip";
    preset.native.load = 1.23456789f;
    preset.native.wetness = 0.234567f;
    preset.native.opacity = 0.777f;
    preset.native.grain.enabled = true;
    preset.native.grain.periodX = 40;
    preset.native.grain.periodY = 12;
    preset.native.grain.depth = 0.6f;
    preset.native.grain.strength = 1.4f;

    BrushState brush;
    applyPresetToBrush(preset, brush);
    check(nativeBrushEqual(brush.native, preset.native),
          "applyPresetToBrush: brush.native matches preset.native bit-identically");

    // Perturb the live brush so presetFromBrush() has something of its own to
    // carry back, rather than accidentally proving the copy by leaving the
    // brush untouched.
    brush.native.load = 0.05f;
    brush.native.opacity = 0.11f;
    brush.native.grain.strength = 0.02f;

    const BrushPreset roundTripped = presetFromBrush("Round Tripped", brush);
    check(nativeBrushEqual(roundTripped.native, brush.native),
          "presetFromBrush: preset.native matches the (perturbed) brush.native bit-identically");
    check(roundTripped.native.load == 0.05f && roundTripped.native.opacity == 0.11f &&
              roundTripped.native.grain.strength == 0.02f,
          "presetFromBrush: the perturbed fields specifically came back exactly");
  }

  // ========================================================================
  std::printf("  -- D. user-presets.txt: an old (pre-NativeBrush) file still loads --\n");
  // ========================================================================
  {
    // Hand-written in the exact shape `f82626d`'s `UserBrushLibraryStore::
    // serialize()` wrote: header, one `preset`, a seven-float `scalars` line
    // (radius, hardness, spacing-in-radii, roundness, angle, load, wetness),
    // and a `grain` line. No `opacity` line -- that key did not exist yet,
    // `BrushPreset` had no field for it to come from. No `model`/`dab`/
    // `link`/`floor` lines -- this fixture does not exercise them.
    const std::string legacyFixture =
        "naturalPaint-user-presets 1\n"
        "preset Legacy Wash\n"
        "scalars 33.5 0.618034 0.366 0.729 47.25 1.14159265 0.874321\n"
        "grain 1 32 18 0.5 1.25\n";

    UserBrushLibraryStore store;
    BrushLibrary lib;
    store.parse(legacyFixture, lib);

    const BrushPreset* back = nullptr;
    for (const BrushPreset& p : lib.presets)
      if (p.name == "Legacy Wash") back = &p;
    check(back != nullptr, "legacy fixture: `Legacy Wash` preset is present after parse()");
    if (back != nullptr) {
      check(back->model.tip.diameterPx == 67.0f && back->model.tip.hardness == 0.618034f &&
                back->model.tip.spacingPercent == 18.3f && back->model.tip.roundness == 0.729f &&
                back->model.tip.angleDeg == 47.25f,
            "legacy fixture: the five `scalars` geometry fields land on `model.tip`, converted "
            "the same way they always were");
      check(back->native.load == 1.14159265f && back->native.wetness == 0.874321f,
            "legacy fixture: `scalars`' trailing load/wetness land on `native`, under the "
            "unchanged `scalars` key");
      check(back->native.grain.enabled && back->native.grain.periodX == 32 &&
                back->native.grain.periodY == 18 && back->native.grain.depth == 0.5f &&
                back->native.grain.strength == 1.25f,
            "legacy fixture: the `grain` line lands on `native.grain`, under the unchanged "
            "`grain` key");
      check(back->native.opacity == 1.0f,
            "legacy fixture: with no `opacity` line (the key did not exist yet), "
            "`native.opacity` is NativeBrush's own default -- the value every preset's opacity "
            "always was before this key could say otherwise");
    }

    // The inverse direction, briefly: a file THIS build writes reads back
    // bit-identically, opacity included -- the fuller round trip (links,
    // model, dab id) is app/selftest/UserBrushLibrary.cpp's own job; this is
    // just `native`'s share of it, including the field that file predates.
    BrushPreset fresh;
    fresh.name = "Fresh";
    fresh.native.load = 0.41f;
    fresh.native.wetness = 1.87f;
    fresh.native.opacity = 0.63f;
    fresh.native.grain.enabled = true;
    fresh.native.grain.periodX = 20;
    fresh.native.grain.periodY = 20;
    fresh.native.grain.depth = 0.3f;
    fresh.native.grain.strength = 0.8f;
    BrushLibrary freshLib;
    freshLib.presets.push_back(fresh);
    const std::string written = store.serialize(freshLib);

    BrushLibrary reloadedLib;
    UserBrushLibraryStore reader;
    reader.parse(written, reloadedLib);
    const BrushPreset* reloaded = nullptr;
    for (const BrushPreset& p : reloadedLib.presets)
      if (p.name == "Fresh") reloaded = &p;
    check(reloaded != nullptr && nativeBrushEqual(reloaded->native, fresh.native),
          "new-format round trip: a file this build writes (opacity line included) reads "
          "`native` back bit-identically");
  }

  // ========================================================================
  std::printf("  -- E. brushTipFor(): tip.flow/opacity/grain come from native, exactly --\n");
  // ========================================================================
  {
    BrushState brush;
    brush.native.load = 0.37f;
    brush.native.opacity = 0.59f;
    brush.native.grain.enabled = true;
    brush.native.grain.periodX = 16;
    brush.native.grain.periodY = 48;
    brush.native.grain.depth = 0.2f;
    brush.native.grain.strength = 1.6f;

    const MixboxLut lut;  // invalid on purpose -- this section is about
                          // native, not pigment (app/selftest/UserBrushLibrary.cpp's own note).
    const BrushTip tip = brushTipFor(brush, lut, /*pressure=*/1.0f);
    check(tip.flow == brush.native.load, "brushTipFor: tip.flow == native.load, exactly");
    check(tip.opacity == brush.native.opacity,
          "brushTipFor: tip.opacity == native.opacity, exactly");
    check(grainParamsEqual(tip.grain, brush.native.grain),
          "brushTipFor: tip.grain == native.grain, exactly");
  }

  std::printf("[selftest] native brush %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
