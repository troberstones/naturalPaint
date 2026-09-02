#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>

#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"

namespace np {

// ---------------------------------------------------------------------------
// docs/reachability-audit.md B6 and B7 -- a Multiply target's floor -- AND
// the shelving that made the whole mechanism this file was written to check
// stop reaching a real stroke at all.
//
// **Historical shape, for context (§1-4 used to prove this):** B6:
// `io/AbrBrushes.cpp`'s importer used to fold Photoshop's Minimum Diameter
// into EVERY link it emitted onto Size, a `TargetCombine::Multiply` target,
// so two links composed as a PRODUCT and each contributing its own copy of
// the floor made it its own SQUARE. The fix moved the floor onto the TARGET
// (`BrushLinkSet::multiplyFloor`), applied once, downstream of the whole
// product. B7: a hardware source idling at zero (tilt, at rest) can multiply
// a brush to a radius of zero; a REPORTING device genuinely at tilt 0 should
// still get Photoshop's own floor behaviour rather than vanishing.
//
// **What changed since:** the brush model migration (see
// `app/StrokeSession.cpp`'s own header) made `BrushModel`/`Variance`
// authoritative for what a dab paints. `brushTipFor()` no longer calls
// `evaluateLinksFiltered(brush.links, ...)` and `StrokeSession::begin()` no
// longer takes a `BrushLinkSet*` -- it takes a `const BrushModel*`. Size
// now resolves through `varianceScale(sizeVariance_, ...)`, whose OWN floor
// (`Variance::minimum`) is applied once, inside that one formula --
// `BrushTip::sizeFloorPx` (the field B6's fix added to carry the
// not-yet-applied Multiply floor between the two halves of the old product)
// is deleted outright, because there is no longer a second half for it to
// wait for. `BrushLinkSet`/`multiplyFloor` still exist -- shelved, not
// deleted, read by the `--advanced-dynamics` matrix editor
// (`ui/DynamicsMatrixPanel.hpp`) -- but nothing that paints reads them any
// more. B6 and B7 are still fixed; they are fixed by a different mechanism
// now, exercised by `app/selftest/DynamicsSources.cpp` and
// `VarianceConsumption.cpp` (Variance's own `minimum` field, not this file's
// `multiplyFloor`).
//
// **What this file checks now:**
//   §1 -- the new invariant this shelving is supposed to guarantee: a
//        `BrushState` with B6's own dramatic pre-fix and post-fix link
//        shapes, floor and all, paints IDENTICALLY to one with no links at
//        all. Not "the floor is applied once" (there is no longer a second
//        half to double-apply it) but "the floor, and the links carrying
//        it, are inert" -- the proof that the shelf is total, not partial.
//   §5 -- unchanged: `evaluateLinks()` never reads `multiplyFloor` at the
//        Dynamics.cpp level, regardless of consumption path.
//   §6 -- rewritten for Part 4: `link`/`floor` lines in `user-presets.txt`
//        are preserved verbatim now, unconditionally, never parsed into a
//        live `BrushLinkSet` -- so a round-trip keeps the TEXT but the live
//        `multiplyFloor` a reload produces is 0.0f, not the floor the file
//        names.
//   §7 -- unchanged: `linkSetsEqual()`/`presetMatches()` still see a floor
//        difference, at the pure-struct level `presetMatches()` still
//        compares (`brush/Library.cpp`) -- unaffected by what does or does
//        not consume the structs it compares.
// ---------------------------------------------------------------------------
bool runMultiplyFloorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  auto makeDoc = [](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{}, "multiply-floor");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makePigmentLayer("p")));
    return od;
  };

  const MixboxLut noLut;  // invalid on purpose -- brushTipFor()'s documented
                          // fallback; radius, not colour, is this file's
                          // whole subject.

  // A generous absolute tolerance for every radius comparison below, derived
  // once here rather than re-guessed per assertion. The literals this file
  // feeds `BrushLink::rangeLo/rangeHi` and `BrushLinkSet::multiplyFloor`
  // (0.1f, 0.15f, 0.2f, 0.25f) are none of them exact binary fractions except
  // 0.25f, so each carries single-precision's own ~1.19e-7 relative rounding;
  // multiplied through a base radius of 100 and folded twice (once per half
  // of a two-phase product) that error is still under 1e-4 absolute -- see
  // the arithmetic worked through in each section's own comment. `kTol` is a
  // full order of magnitude above that (absorbs it with room to spare) and
  // at least two orders below the smallest gap any section here actually
  // distinguishes (4 px, between section 1's "before" and "after"; 25 px,
  // between section 2's correct answer and the double-floored one) -- wide
  // enough to never flake on rounding, narrow enough that it cannot pass a
  // wrong formula off as a right one.
  constexpr float kTol = 1e-3f;
  constexpr float kBaseRadius = 100.0f;

  // A short straight-line stroke, read back through `lastDabRadius()` after
  // it ends. **Not a single `addPoint()` call** -- `brush/StrokePath.hpp`'s
  // own contract says a 0- or 1-sample stroke emits nothing at all, even
  // through `flush()` at `end()` (it needs two real samples on either side of
  // a segment before it will walk it), so a "one-dab" helper built on exactly
  // one `addPoint()` would silently deposit zero dabs and leave
  // `lastDabRadius()` at its untouched 0.0f default -- indistinguishable from
  // a genuinely-floored-to-zero brush. Six points 40 px apart (240 px of
  // path) comfortably clears even this brush's widest possible per-dab
  // spacing (0.25 * 100 px = 25 px at the largest radius any fixture in this
  // file uses), so at least one real dab is always emitted.
  //
  // Builds a `BrushState` whose `model.tip.diameterPx` is `2*radius` (the
  // old shadow scalar this helper's callers used to set directly; Part 5
  // deleted it) and begins the stroke against `&brush.model`, not
  // `&brush.links` -- the argument `StrokeSession::begin()` now takes. Any
  // `BrushLink`s or `multiplyFloor` entries a fixture ALSO populates on
  // `brush.links` are passed nowhere: nothing downstream of this call reads
  // them, which is precisely what section 1 below exists to demonstrate.
  //
  // `inputs`, when given, is also the stroke's `hardwareInputs` -- what the
  // per-dab loop feeds `varianceScale()`/`varianceOffset()` for any
  // pressure/tilt-driven Variance a fixture's `model.shape.size` carries.
  // Every fixture in this file that sets `model.shape.size` uses a `Fade`
  // or `Off` control, never Pressure/Tilt, so `inputs` only matters here for
  // building `tip` (the bootstrap tip `begin()` needs to accept the stroke).
  auto radiusBrush = [](float radius) {
    BrushState brush;
    brush.model.tip.diameterPx = radius * 2.0f;
    return brush;
  };
  auto oneDabRadius = [&](const BrushState& brush, const DynamicInputs& inputs = {}) {
    OpenDocument od = makeDoc(1024, 1024);
    StrokeSession s;
    std::string e;
    const BrushTip tip = brushTipFor(brush, noLut, inputs);
    if (!s.begin(od, 1, tip, Tool::Brush, &e, &brush.model, inputs)) return -1.0f;  // unreachable
                                                                                     // if the
                                                                                     // fixture is
                                                                                     // well-formed
    for (int i = 0; i < 6; ++i) s.addPoint(400.0f + 40.0f * static_cast<float>(i), 500.0f);
    s.end();
    return s.lastDabRadius();
  };

  // ==========================================================================
  // 1. The shelf is total: B6/B7's own dramatic link shapes, floor included,
  //    no longer move a real stroke's radius at all
  // ==========================================================================
  //
  // Reconstructs the exact three fixtures §1/§2/§4 (pre-shelving revisions
  // of this file) used to distinguish by their painted radius -- the old
  // pre-fix squared-floor shape, the old post-fix honest-floor shape, and a
  // stroke-local `rangeHi > 1.0` product -- and shows every one of them now
  // paints identically to a plain brush with NO links at all. Not merely
  // "close": bit-for-bit equal, since `brushTipFor()` no longer reads
  // `brush.links` in any form.
  {
    const BrushState plain = radiusBrush(kBaseRadius);

    // The old "before": two links each baking a 20% floor into their own
    // `rangeLo`, no `multiplyFloor` set.
    BrushState squaredShape = radiusBrush(kBaseRadius);
    BrushLink beforeCtrl;
    beforeCtrl.source = DynamicSource::Pressure;
    beforeCtrl.target = DynamicTarget::Size;
    beforeCtrl.rangeLo = 0.2f;
    beforeCtrl.rangeHi = 1.0f;
    addLink(squaredShape.links, beforeCtrl);
    BrushLink beforeJit;
    beforeJit.source = DynamicSource::Tilt;
    beforeJit.target = DynamicTarget::Size;
    beforeJit.rangeLo = 0.2f;
    beforeJit.rangeHi = 1.0f;
    addLink(squaredShape.links, beforeJit);

    // The old "after": both links honest ([0,1]), the SAME 20% moved onto
    // `multiplyFloor[Size]` instead.
    BrushState honestFloorShape = radiusBrush(kBaseRadius);
    BrushLink afterCtrl;
    afterCtrl.source = DynamicSource::Pressure;
    afterCtrl.target = DynamicTarget::Size;
    afterCtrl.rangeLo = 0.0f;
    afterCtrl.rangeHi = 1.0f;
    addLink(honestFloorShape.links, afterCtrl);
    BrushLink afterJit;
    afterJit.source = DynamicSource::Tilt;
    afterJit.target = DynamicTarget::Size;
    afterJit.rangeLo = 0.0f;
    afterJit.rangeHi = 1.0f;
    addLink(honestFloorShape.links, afterJit);
    honestFloorShape.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.2f;

    // The old §2 counter-example: a hardware link flat at 0.1, a stroke-local
    // `rangeHi` of 2.0, and a 25% floor -- the fixture that used to prove
    // 25 px (floored once, downstream of both halves) rather than 50 px
    // (floored twice) was correct. Both numbers came from `brush.links`;
    // neither can be reached from it any more.
    BrushState twoHalvesShape = radiusBrush(kBaseRadius);
    BrushLink pressureLink;
    pressureLink.source = DynamicSource::Pressure;
    pressureLink.target = DynamicTarget::Size;
    pressureLink.rangeLo = 0.1f;
    pressureLink.rangeHi = 0.1f;
    addLink(twoHalvesShape.links, pressureLink);
    BrushLink fadeLink;
    fadeLink.source = DynamicSource::Fade;
    fadeLink.target = DynamicTarget::Size;
    fadeLink.rangeLo = 0.0f;
    fadeLink.rangeHi = 2.0f;
    addLink(twoHalvesShape.links, fadeLink);
    twoHalvesShape.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.25f;

    DynamicInputs zeroTilt;
    zeroTilt.pressure = 0.0f;
    zeroTilt.hasTilt = true;  // a REPORTING device at rest -- see section 1's
                              // B7 note below on why that distinction
                              // mattered to the OLD mechanism
    zeroTilt.tilt = 0.0f;

    const float plainRadius = oneDabRadius(plain, zeroTilt);
    check(nearf(plainRadius, kBaseRadius, kTol),
          "floor/inert: the plain brush paints at its unattenuated model diameter -- the "
          "baseline every shape below is compared against");
    check(nearf(oneDabRadius(squaredShape, zeroTilt), plainRadius, kTol),
          "floor/inert: the old squared-floor shape (would have painted 4 px) now paints "
          "identically to the plain brush -- its links are never read");
    check(nearf(oneDabRadius(honestFloorShape, zeroTilt), plainRadius, kTol),
          "floor/inert: the old honest-floor shape (would have painted 20 px) now paints "
          "identically to the plain brush -- `multiplyFloor[Size]` is never read either");
    check(nearf(oneDabRadius(twoHalvesShape, zeroTilt), plainRadius, kTol),
          "floor/inert: the old two-halves counter-example (would have painted 25 px, or 50 px "
          "double-floored) now paints identically to the plain brush -- there is no longer a "
          "second half for a floor to be applied to twice, because there is no longer a first "
          "half either");

    // B7's own case, restated: a REAL pen reporting tilt 0 used to be able to
    // multiply a brush to zero, floor or no floor. It cannot any more --
    // `brush.links` is not in the path Size resolves through at all, tilt-
    // reporting or not.
    BrushState tiltFloored = radiusBrush(kBaseRadius);
    BrushLink tiltLink;
    tiltLink.source = DynamicSource::Tilt;
    tiltLink.target = DynamicTarget::Size;
    tiltLink.rangeLo = 0.0f;
    tiltLink.rangeHi = 1.0f;
    addLink(tiltFloored.links, tiltLink);
    tiltFloored.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.15f;
    DynamicInputs reporting;
    reporting.hasTilt = true;
    reporting.tilt = 0.0f;
    check(nearf(oneDabRadius(tiltFloored, reporting), kBaseRadius, kTol),
          "floor/inert: B7's own tilt-0-with-a-floor fixture (would have thinned to 15 px) now "
          "paints at full size -- Size no longer has a hardware link to resolve to 0 in the "
          "first place");
  }

  // ==========================================================================
  // 5. The floor lives outside link RESOLUTION entirely -- Add targets stay
  //    unused, not silently ignored
  // ==========================================================================
  //
  // `multiplyFloor` is not read by `evaluateLinksWhere()` (brush/Dynamics.cpp)
  // at all -- every consumer is a specific call site (`app/StrokeSession.cpp`,
  // `app/DabPreview.cpp`, `ui/MacPaintUI.cpp`) reading `multiplyFloor[Size]`
  // directly, never the link-evaluation engine itself. This proves that
  // generally (any target, not only the three `TargetCombine::Add` ones),
  // and then specifically for Angle, Scatter and Hue -- the three targets a
  // floor is not even a coherent concept for (`BrushLinkSet::multiplyFloor`'s
  // own comment) -- so "unused" is demonstrated, not merely asserted in a
  // header comment nobody runs.
  {
    BrushLinkSet links;
    BrushLink angleLink;
    angleLink.source = DynamicSource::Tilt;
    angleLink.target = DynamicTarget::Angle;
    angleLink.rangeLo = 0.0f;
    angleLink.rangeHi = 45.0f;
    addLink(links, angleLink);
    BrushLink sizeLink;
    sizeLink.source = DynamicSource::Pressure;
    sizeLink.target = DynamicTarget::Size;
    sizeLink.rangeLo = 0.3f;
    sizeLink.rangeHi = 1.0f;
    addLink(links, sizeLink);

    DynamicInputs in;
    in.pressure = 0.6f;
    in.hasTilt = true;
    in.tilt = 0.4f;
    const DynamicResult before = evaluateLinks(links, in);

    // Every slot, including Size's OWN, set to a large nonzero garbage value
    // -- if `evaluateLinks()` read `multiplyFloor` ANYWHERE, at least one of
    // these twelve would move the result.
    for (size_t t = 0; t < kDynamicTargetCount; ++t) links.multiplyFloor[t] = 0.9f;
    const DynamicResult after = evaluateLinks(links, in);

    bool anyMoved = false;
    for (size_t t = 0; t < kDynamicTargetCount; ++t)
      if (before.value[t] != after.value[t]) anyMoved = true;
    check(!anyMoved,
          "floor/unused: setting every `multiplyFloor` slot to 0.9 -- Size's own included -- "
          "changes NONE of `evaluateLinks()`'s twelve resolved values, bit for bit. The floor "
          "is applied only at the specific radius-consuming call sites this file's other "
          "sections exercise, never inside link resolution itself");

    check(after.at(DynamicTarget::Angle) == before.at(DynamicTarget::Angle) &&
              after.at(DynamicTarget::Scatter) == before.at(DynamicTarget::Scatter) &&
              after.at(DynamicTarget::Hue) == before.at(DynamicTarget::Hue),
          "floor/unused: named directly for the three Add targets -- Angle, Scatter, Hue -- "
          "matching `BrushLinkSet::multiplyFloor`'s own comment on why a floor is not even a "
          "coherent concept for an offset the way it is for a scale");
  }

  // ==========================================================================
  // 6. Persistence, rewritten for Part 4: `link`/`floor` lines survive a
  //    round trip through `user-presets.txt` as TEXT, not as a live
  //    `BrushLinkSet` any more
  // ==========================================================================
  //
  // Before the shelving, `UserBrushLibrary.cpp`'s parser built a live
  // `BrushLink`/`multiplyFloor` entry from any well-formed `link`/`floor`
  // line whose ordinals were in this build's range, and only preserved an
  // OUT-OF-range one verbatim (a future build's data this one cannot
  // evaluate). Now that nothing which paints reads `BrushPreset::links`,
  // building that live structure at load time serves no purpose the shelved
  // matrix editor cannot get some other way -- so EVERY well-formed `link`/
  // `floor` line takes the preserve-verbatim path unconditionally, in range
  // or not. `serialize()`'s own WRITE side is untouched (Part 4 only
  // changed the parse conditions): a preset with a live, populated
  // `BrushPreset::links` -- reachable today only from a fixture like this
  // one, since the parser no longer produces one -- still serialises its
  // links normally.
  {
    UserBrushLibraryStore store;
    BrushLibrary lib;
    BrushPreset preset;
    preset.name = "Floored Preset";
    preset.model.tip.diameterPx = 84.0f;  // unrelated to what this section
                                          // checks; just a non-default value
                                          // so this is not accidentally all
                                          // zeroes
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    addLink(preset.links, l);
    preset.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.33f;
    lib.presets.push_back(preset);

    const std::string text = store.serialize(lib);
    check(text.find("floor 0 0.33") != std::string::npos,
          "floor/persist: a preset with a live, populated `links` (unreachable from a real "
          "load any more, but still possible in memory) still serialises its floor -- the WRITE "
          "side is unchanged by Part 4");
    check(text.find("link 0 0") != std::string::npos,
          "floor/persist: ...and its link, by the same unchanged write path");

    BrushLibrary reloaded;
    UserBrushLibraryStore reader;
    reader.parse(text, reloaded);
    check(reloaded.presets.size() == 1 &&
              reloaded.presets[0].links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] ==
                  0.0f &&
              reloaded.presets[0].links.links.empty(),
          "floor/persist: reloading that same text builds NO live link and NO live floor -- "
          "`reloaded.presets[0].links` is exactly the default-constructed `BrushLinkSet`, "
          "proving the parser no longer constructs one from `link`/`floor` lines at all");
    // `0.33f` is not exact in binary32 -- `f9()`'s own `%.9g` writes it as
    // "0.330000013", not "0.33" -- so the exact-equality lookup below uses
    // the real serialised text, not the literal this fixture typed.
    const auto reloadedLines = reader.presetUnknownLines().find("Floored Preset");
    check(reloadedLines != reader.presetUnknownLines().end() &&
              std::find(reloadedLines->second.begin(), reloadedLines->second.end(),
                        std::string("floor 0 0.330000013")) != reloadedLines->second.end(),
          "floor/persist: ...but the exact `floor 0 0.330000013` TEXT is preserved verbatim -- "
          "what used to become live data now becomes an opaque line this build carries forward "
          "without understanding it");

    // Re-serialising the reload reproduces the identical `floor`/`link`
    // lines -- not because they round-tripped through a live struct (they
    // did not; that struct is empty) but because the preserved text is
    // replayed byte-for-byte. This is the load-bearing half of Part 4(a):
    // an imported brush's Minimum Diameter is not silently lost the moment
    // a user hits Save, it simply stops being data this build can edit.
    //
    // **Re-serialised through `reader`, the SAME store that parsed it, not
    // `store`.** `presetUnknownLines_` lives on the `UserBrushLibraryStore`
    // instance, not on the `BrushLibrary` it parses into (`app/
    // UserBrushLibrary.hpp`'s own class layout) -- a fresh `store.serialize
    // (reloaded)` would see an empty map and silently drop every preserved
    // line, which would prove the opposite of Part 4(a)'s promise while
    // looking, at a glance, like the same call the section above already
    // made.
    const std::string resaved = reader.serialize(reloaded);
    check(resaved.find("floor 0 0.330000013") != std::string::npos &&
              resaved.find("link 0 0") != std::string::npos,
          "floor/persist: a save-after-load reproduces both lines exactly, via the preserved "
          "text rather than a reconstructed live structure");

    // A preset with NO floor writes no `floor` line at all -- the "costs
    // nothing" half of `multiplyFloor`'s own default, restated for the file
    // format: a preset saved before this key existed reserialises
    // byte-for-byte the same as it always did.
    BrushLibrary plainLib;
    BrushPreset plain;
    plain.name = "Plain Preset";
    addLink(plain.links, l);
    plainLib.presets.push_back(plain);
    const std::string plainText = store.serialize(plainLib);
    check(plainText.find("floor") == std::string::npos,
          "floor/persist: a preset with the default (no) floor emits no `floor` line at all");

    // What used to be forward compatibility (an out-of-range target ordinal
    // preserved verbatim rather than dropped) is now simply what happens to
    // EVERY `floor` line, in range or not -- restated here with an
    // out-of-range ordinal specifically to show the in-range/out-of-range
    // distinction this fixture used to depend on no longer changes the
    // outcome at all.
    const std::string futureFloor =
        std::string(kUserPresetsFileHeader) + " 1\npreset Future\nscalars 1 1 1 1 1 1 1\nfloor " +
        std::to_string(static_cast<int>(kDynamicTargetCount)) + " 0.5\n";
    BrushLibrary futureLib;
    UserBrushLibraryStore futureReader;
    futureReader.parse(futureFloor, futureLib);
    check(futureLib.presets.size() == 1 &&
              futureLib.presets[0].links.multiplyFloor[0] == 0.0f,
          "floor/persist: an out-of-range target ordinal is not applied to any in-range slot "
          "this build has -- true now for every ordinal, not only out-of-range ones");
    const auto it = futureReader.presetUnknownLines().find("Future");
    check(it != futureReader.presetUnknownLines().end() && it->second.size() == 1 &&
              it->second[0].rfind("floor ", 0) == 0,
          "floor/persist: ...and is preserved verbatim, scoped to its own preset, exactly like "
          "every OTHER `floor` line now, in-range or not");
  }

  // ==========================================================================
  // 7. `linkSetsEqual()`/`presetMatches()` see a floor difference too
  // ==========================================================================
  //
  // `brush/Library.cpp`'s `linkSetsEqual()` is the BRUSH EDITOR's "has the
  // live brush drifted from the preset it was loaded from" check
  // (`presetMatches()`, `brushIsEdited()`). It walks `BrushLinkSet::links`
  // cell by cell, which cannot see `multiplyFloor` at all -- a per-target
  // field, not a per-link one -- so it was extended to compare that array
  // directly. Nothing in this build's UI can currently DRIVE that
  // difference (no control edits `multiplyFloor` live; only an import or a
  // file load ever sets it, and both go through a whole-`BrushLinkSet` copy
  // that keeps the two in step) -- which is exactly why this needed its own
  // assertion rather than being caught by an existing one: the gap was
  // dormant, not exercised by any UI-driven test already in the suite.
  {
    BrushLinkSet a;
    BrushLink l;
    l.source = DynamicSource::Pressure;
    l.target = DynamicTarget::Size;
    l.rangeLo = 0.0f;
    l.rangeHi = 1.0f;
    addLink(a, l);
    BrushLinkSet b = a;  // identical links, identical (default, zero) floor
    check(linkSetsEqual(a, b), "floor/equal: two sets with identical links and no floor compare equal");

    b.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.4f;
    check(!linkSetsEqual(a, b),
          "floor/equal: ...and stop comparing equal the moment ONLY their Size floor differs -- "
          "every `BrushLink` in both sets is still identical, so a walk that only compared "
          "`links` would miss this");

    BrushLinkSet c = a;
    c.multiplyFloor[static_cast<size_t>(DynamicTarget::Angle)] = 0.4f;  // an Add-target slot --
                                                                        // unused by anything
                                                                        // downstream, but still
                                                                        // part of the SET's own
                                                                        // identity
    check(!linkSetsEqual(a, c),
          "floor/equal: ...and the same for a slot nothing ever reads (Angle) -- 'unused' means "
          "no consumer applies it, not that two otherwise-identical sets are the same brush");
  }

  return ok;
}

}  // namespace np
