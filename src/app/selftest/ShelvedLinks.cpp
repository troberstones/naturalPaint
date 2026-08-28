#include "app/selftest/Support.hpp"

#include <string>
#include <vector>

#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"
#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Part 4/5's shelving of the 10x12 link matrix (brush/Dynamics.hpp), the
// dedicated test for it. `BrushLinkSet` stays a member of `BrushPreset`/
// `BrushState` -- it is not deleted, and `--advanced-dynamics`
// (`ui/DynamicsMatrixPanel.hpp`) still shows it -- but nothing that PARSES
// `user-presets.txt` builds a live one from `link`/`floor`/`point` lines any
// more, and nothing that PAINTS (`app/StrokeSession::brushTipFor()`) reads
// one either. Two independent proofs of that, in the two directions a
// regression could reopen it:
//
//   1. -- the FILE side. A fixture with `link`/`point`/`floor` lines round-
//        trips its TEXT byte-for-byte through `parse()`/`serialize()`, even
//        though `parse()` never builds a live `BrushLinkSet` from any of it
//        (`app/selftest/MultiplyFloor.cpp`'s own §6 covers the same ground
//        for a floor-focused fixture; this one exercises a full
//        link+curve+floor preset and pins the exact preserved-line list).
//   2. -- the PAINT side. A hand-built, aggressively different
//        `BrushLinkSet` -- constructed directly in C++, not read from a
//        file, so this cannot be a parser quirk -- produces a BIT-IDENTICAL
//        `BrushTip` from `brushTipFor()` as the same preset with an empty
//        one. Not "the floor cannot be observed any more" (a claim a subtly
//        broken read could still satisfy by accident) but "the field is not
//        read at all", proved by construction.
// ---------------------------------------------------------------------------
bool runShelvedLinksTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  std::printf("-- shelved links: the matrix is shelved, not deleted --\n");

  // --- 1. A fixture's link/floor/point lines round-trip byte-for-byte -----
  {
    const std::string text = std::string(kUserPresetsFileHeader) +
                             " 1\n"
                             "preset Shelved Sample\n"
                             "scalars 21.5 0.62 0.4 0.85 12 0.95 1.1\n"
                             "link 0 0 0.1 0.9 1 0\n"
                             "point 0.25 0.6\n"
                             "point 0.75 0.4\n"
                             "link 1 3 -45 45 0 1\n"
                             "floor 3 0.2\n";

    BrushLibrary lib;
    UserBrushLibraryStore store;
    store.parse(text, lib);
    check(lib.presets.size() == 1, "shelf/text: the fixture's one preset loads");

    // No live structure is built from any of it -- not the links, not the
    // floor slot the `floor` line named.
    check(lib.presets.size() == 1 && lib.presets[0].links.links.empty() &&
              lib.presets[0].links.multiplyFloor[3] == 0.0f,
          "shelf/text: link/floor/point lines build NO live BrushLinkSet -- links.links is "
          "empty and the floor slot they named reads 0.0, the default");

    // But the text itself survived, verbatim, scoped to this preset, in the
    // exact order the file had it.
    const std::vector<std::string> expected = {"link 0 0 0.1 0.9 1 0", "point 0.25 0.6",
                                                "point 0.75 0.4", "link 1 3 -45 45 0 1",
                                                "floor 3 0.2"};
    const auto lines = store.presetUnknownLines().find("Shelved Sample");
    check(lines != store.presetUnknownLines().end() && lines->second == expected,
          "shelf/text: all five link/point/floor lines are preserved verbatim, one entry each, "
          "in the file's own order");

    // Re-serialised through the SAME store that parsed -- `presetUnknownLines_`
    // lives on the `UserBrushLibraryStore` instance, not on the
    // `BrushLibrary` it parses into (`app/UserBrushLibrary.hpp`'s own class
    // layout; `app/selftest/MultiplyFloor.cpp`'s §6 comment names this exact
    // trap) -- the output reproduces every line byte-for-byte.
    const std::string resaved = store.serialize(lib);
    bool allPresent = true;
    for (const std::string& needle : expected)
      if (resaved.find(needle) == std::string::npos) allPresent = false;
    check(allPresent,
          "shelf/text: a save-after-load still contains every line, byte-identical -- an "
          "imported brush's dynamics are not silently lost the moment a user hits Save, they "
          "simply stop being data this build can edit");
  }

  // --- 2. A hand-built BrushLinkSet changes nothing brushTipFor() reads ---
  {
    BrushState withLinks;
    withLinks.model.tip.diameterPx = 60.0f;
    withLinks.model.tip.hardness = 0.4f;
    withLinks.model.tip.roundness = 0.6f;
    withLinks.model.tip.angleDeg = 15.0f;
    withLinks.model.tip.spacingPercent = 30.0f;
    withLinks.load = 0.8f;
    withLinks.pigment = 3;

    // Aggressively different from identity: a Pressure -> Size link that
    // would, if this build still read it, crush the tip toward zero at low
    // pressure, plus a floor that would (under the OLD architecture) rescue
    // it back up -- the exact B6/B7 shape `MultiplyFloor.cpp` exists to
    // check, reused here to show it changes NOTHING through this path.
    BrushLink crush;
    crush.source = DynamicSource::Pressure;
    crush.target = DynamicTarget::Size;
    crush.rangeLo = 0.01f;
    crush.rangeHi = 1.0f;
    addLink(withLinks.links, crush);
    withLinks.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] = 0.9f;

    BrushState noLinks = withLinks;
    noLinks.links = BrushLinkSet{};

    check(!linkSetsEqual(withLinks.links, noLinks.links),
          "shelf/paint: (setup) the two fixtures really do carry different links -- not a "
          "vacuous comparison below");

    const MixboxLut noLut;
    DynamicInputs in;
    in.pressure = 0.05f;  // low pressure: exactly where the old crush link bit hardest
    const BrushTip a = brushTipFor(withLinks, noLut, in);
    const BrushTip b = brushTipFor(noLinks, noLut, in);
    check(a.radius == b.radius && a.angle == b.angle && a.roundness == b.roundness &&
              a.hardness == b.hardness && a.spacing == b.spacing && a.flow == b.flow &&
              a.scatter == b.scatter && a.linearRgb == b.linearRgb,
          "shelf/paint: brushTipFor() returns a BIT-IDENTICAL BrushTip whether or not the "
          "BrushState carries a live, aggressively different BrushLinkSet -- not merely 'the "
          "floor cannot be observed', but 'the field is not read at all'");

    // And at full pressure too, in case a real bug only showed up away from
    // the low end this crush link targets.
    in.pressure = 1.0f;
    const BrushTip a2 = brushTipFor(withLinks, noLut, in);
    const BrushTip b2 = brushTipFor(noLinks, noLut, in);
    check(a2.radius == b2.radius && a2.flow == b2.flow,
          "shelf/paint: ...true at full pressure too, not only where the crush link is "
          "steepest");
  }

  return ok;
}

}  // namespace np
