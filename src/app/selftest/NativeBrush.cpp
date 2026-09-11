#include "app/selftest/Support.hpp"

#include "brush/BrushModelIo.hpp"
#include "brush/Library.hpp"
#include "brush/NativeBrush.hpp"

#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"

namespace np {

// brush/NativeBrush -- naturalPaint's own brush section (load, wetness,
// grain), beside `BrushModel` (Photoshop's). This is the dedicated selftest
// for the struct itself and for the migration that created it: `BrushModel`
// losing its own `load`/`wetness` leaves, `BrushPreset`/`BrushState` gaining
// one `native` member apiece in place of the three loose fields, and
// `user-presets.txt` round-tripping both the old (pre-`native`) format and
// the new one. It also pins what the migration must NOT do: move
// `BrushState::opacity`, which stays session state outside `native` (see
// brush/NativeBrush.hpp's own header for both arguments).
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
    brush.native.wetness = 0.11f;
    brush.native.grain.strength = 0.02f;

    const BrushPreset roundTripped = presetFromBrush("Round Tripped", brush);
    check(nativeBrushEqual(roundTripped.native, brush.native),
          "presetFromBrush: preset.native matches the (perturbed) brush.native bit-identically");
    check(roundTripped.native.load == 0.05f && roundTripped.native.wetness == 0.11f &&
              roundTripped.native.grain.strength == 0.02f,
          "presetFromBrush: the perturbed fields specifically came back exactly");
  }

  // ========================================================================
  std::printf("  -- C2. BrushState::opacity is session state, NOT preset state --\n");
  // ========================================================================
  {
    // `opacity` was deliberately left OUT of `native` (brush/NativeBrush.hpp's
    // header): it is options-bar state a preset does not carry, so a painter
    // who lowered it keeps it across preset picks, and the EDITED badge has
    // no preset value for it to disagree with. These are the two
    // painter-visible consequences, asserted end to end.
    BrushPreset preset;
    preset.name = "Opacity Is Sticky";
    preset.native.load = 0.66f;

    // Every built-in plus the hand-made preset above: none of them may move
    // a lowered opacity. Zero tolerance: the claim is "untouched", and
    // nothing between the write of 0.4f and the read does arithmetic on it.
    std::vector<BrushPreset> picks = defaultBrushLibrary().presets;
    picks.push_back(preset);
    bool allKept = true;
    for (const BrushPreset& p : picks) {
      BrushState painter;
      painter.opacity = 0.4f;
      applyPresetToBrush(p, painter);
      if (painter.opacity != 0.4f) allKept = false;
    }
    check(allKept,
          "applyPresetToBrush: picking a preset leaves a lowered brush.opacity (0.4) exactly "
          "where it was -- every built-in and a hand-made one");

    BrushState picked;
    picked.brushLibrary.presets = {preset};
    picked.brushLibrary.active = 0;
    applyPresetToBrush(preset, picked);
    check(!brushIsEdited(picked), "brushIsEdited: a freshly picked preset is not EDITED");
    picked.opacity = 0.4f;
    check(!brushIsEdited(picked),
          "brushIsEdited: moving OPACITY alone does NOT raise EDITED -- a preset carries no "
          "opacity to disagree with");
    // The control that keeps the assertion above from passing vacuously: the
    // same badge on the same brush DOES rise for a field a preset carries.
    picked.native.load = preset.native.load * 0.5f;
    check(brushIsEdited(picked),
          "brushIsEdited: control -- moving LOAD (a `native` field) does raise EDITED");
  }

  // ========================================================================
  std::printf("  -- D. user-presets.txt: old (pre-NativeBrush) and interim files load --\n");
  // ========================================================================
  {
    // Hand-written in the exact shape `f82626d`'s `UserBrushLibraryStore::
    // serialize()` wrote: header, one `preset`, a seven-float `scalars` line
    // (radius, hardness, spacing-in-radii, roundness, angle, load, wetness),
    // the `model` lines `brushModelToLines()` emitted for every non-default
    // leaf -- including the two retired ones, `load`/`wetness`, which that
    // build's visitor still walked -- and a `grain` line. No `dab`/`link`/
    // `floor` lines -- this fixture does not exercise them.
    //
    // The two `model` values deliberately DIFFER from `scalars`' trailing
    // pair: the old build painted with `scalars`' (its `BrushModel::load`/
    // `wetness` were read by nothing), so a reader that let the dead copy
    // overwrite the live one would move `native` and fail the check below.
    const std::string legacyFixture =
        "naturalPaint-user-presets 1\n"
        "preset Legacy Wash\n"
        "scalars 33.5 0.618034 0.366 0.729 47.25 1.14159265 0.874321\n"
        "model load 0.5\n"
        "model wetness 2\n"
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
      // Accepted AND dropped: not preserved as an unknown line (which would
      // re-emit them on every save, forever), because their meaning is known
      // -- they are an older build's dead copies, not a newer build's field.
      const auto& unknown = store.presetUnknownLines();
      BrushLibrary resaveLib;
      resaveLib.presets.push_back(*back);
      const std::string resaved = store.serialize(resaveLib);
      check(unknown.find("Legacy Wash") == unknown.end() &&
                resaved.find("model load") == std::string::npos &&
                resaved.find("model wetness") == std::string::npos,
            "legacy fixture: the retired `model load`/`model wetness` lines are accepted and "
            "dropped -- not preserved as unknown, not written back on save");
    }

    // The INTERIM build's shape: the first cut of this migration briefly put
    // `opacity` in `native` and wrote an `opacity <v>` line after `grain`
    // (and had already stopped writing `model load`/`model wetness`). That
    // was reverted -- opacity is session state a preset does not carry -- but
    // a file that build saved may exist. It must load with every other value
    // intact, and its `opacity` line must be accepted and DROPPED exactly as
    // the retired `model` paths above are: not preserved as unknown, not
    // written back on save.
    const std::string interimFixture =
        "naturalPaint-user-presets 1\n"
        "preset Interim Wash\n"
        "scalars 33.5 0.618034 0.366 0.729 47.25 1.14159265 0.874321\n"
        "grain 1 32 18 0.5 1.25\n"
        "opacity 0.4\n";
    UserBrushLibraryStore interimStore;
    BrushLibrary interimLib;
    interimStore.parse(interimFixture, interimLib);
    const BrushPreset* interim = nullptr;
    for (const BrushPreset& p : interimLib.presets)
      if (p.name == "Interim Wash") interim = &p;
    check(interim != nullptr && interim->native.load == 1.14159265f &&
              interim->native.wetness == 0.874321f && interim->native.grain.enabled &&
              interim->native.grain.strength == 1.25f,
          "interim fixture: a file with an `opacity` line still loads, `native` intact");
    if (interim != nullptr) {
      const auto& interimUnknown = interimStore.presetUnknownLines();
      BrushLibrary interimResaveLib;
      interimResaveLib.presets.push_back(*interim);
      const std::string interimResaved = interimStore.serialize(interimResaveLib);
      check(interimUnknown.find("Interim Wash") == interimUnknown.end() &&
                interimResaved.find("\nopacity") == std::string::npos,
            "interim fixture: the `opacity` line is accepted and dropped -- not preserved as "
            "unknown, not written back on save");
    }

    // The inverse direction, briefly: a file THIS build writes reads back
    // bit-identically -- the fuller round trip (links, model, dab id) is
    // app/selftest/UserBrushLibrary.cpp's own job; this is just `native`'s
    // share of it. And it writes no `opacity` line: a preset has none.
    BrushPreset fresh;
    fresh.name = "Fresh";
    fresh.native.load = 0.41f;
    fresh.native.wetness = 1.87f;
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
          "new-format round trip: a file this build writes reads `native` back "
          "bit-identically");
    check(written.find("\nopacity") == std::string::npos,
          "new-format writer: no `opacity` line -- a preset does not carry opacity");
  }

  // ========================================================================
  std::printf("  -- E. brushTipFor(): flow/grain from native, opacity from BrushState --\n");
  // ========================================================================
  {
    BrushState brush;
    brush.native.load = 0.37f;
    brush.opacity = 0.59f;
    brush.native.grain.enabled = true;
    brush.native.grain.periodX = 16;
    brush.native.grain.periodY = 48;
    brush.native.grain.depth = 0.2f;
    brush.native.grain.strength = 1.6f;

    const MixboxLut lut;  // invalid on purpose -- this section is about
                          // native, not pigment (app/selftest/UserBrushLibrary.cpp's own note).
    const BrushTip tip = brushTipFor(brush, lut, /*pressure=*/1.0f);
    check(tip.flow == brush.native.load, "brushTipFor: tip.flow == native.load, exactly");
    check(tip.opacity == brush.opacity,
          "brushTipFor: tip.opacity == BrushState::opacity, exactly (as at base)");
    check(grainParamsEqual(tip.grain, brush.native.grain),
          "brushTipFor: tip.grain == native.grain, exactly");
  }

  std::printf("[selftest] native brush %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
