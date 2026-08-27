#include "app/selftest/Support.hpp"

#include "app/ControlsColumnLayout.hpp"

namespace np {
namespace {

// Every ControlsSection enumerator, independent of both `controlsSections()`'s
// draw order and `ControlsColumnLayout`'s own order -- used only to check the
// exactly-once invariant from the outside, the same role app/
// selftest/ControlsLayout.cpp's own `kAll` plays for `controlsSections()`.
constexpr ControlsSection kAllSections[] = {
    ControlsSection::Color,        ControlsSection::Layers,     ControlsSection::History,
    ControlsSection::Comps,        ControlsSection::Grade,      ControlsSection::Histogram,
    ControlsSection::BrushLibrary, ControlsSection::Brush,      ControlsSection::Pigment,
    ControlsSection::Medium,       ControlsSection::BoardTilt,  ControlsSection::Grid,
    ControlsSection::Solver,
};

bool exactlyOnceEach(const ControlsColumnLayout& layout) {
  const std::vector<ControlsColumnEntry>& entries = layout.entries();
  if (entries.size() != std::size(kAllSections)) return false;
  for (const ControlsSection s : kAllSections) {
    size_t seen = 0;
    for (const ControlsColumnEntry& e : entries)
      if (e.section == s) ++seen;
    if (seen != 1) return false;
  }
  return true;
}

std::string readWhole(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream b;
  b << f.rdbuf();
  return b.str();
}

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

}  // namespace

// app/ControlsColumnLayout -- the headless model behind a configurable
// right-hand controls column: which sections appear, in what order, and
// whether that survives a relaunch. See the header for the full design; this
// suite proves the one property the header calls out as the thing that
// matters most -- **the sequence holds every ControlsSection exactly once,
// always** -- across every mutation, every parse and every disk round trip,
// plus the four round-trip repair rules the brief for this module names by
// name: unknown section ignored, missing section appended, duplicate section
// collapsed to its first occurrence, and a malformed or garbage file falling
// back to the default layout rather than half-applying.
//
// Nothing here reaches src/ui/MacPaintUI.cpp -- that ImGui half is a
// concurrent, separate change; this section is headless and GPU-free and
// proves the model the UI half will be written against.
bool runControlsColumnLayoutTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // ==========================================================================
  // 1. The default layout, and the key table's totality
  // ==========================================================================
  {
    ControlsColumnLayout layout;
    check(exactlyOnceEach(layout),
          "panel layout: a fresh layout holds every ControlsSection exactly once");
    check(layout.entries().size() == controlsSections().size(),
          "panel layout: the default has exactly as many rows as controlsSections()");
    bool orderMatchesDefault = true;
    bool allVisible = true;
    const std::vector<ControlsSectionSpec>& def = controlsSections();
    for (size_t i = 0; i < def.size(); ++i) {
      if (layout.entries()[i].section != def[i].section) orderMatchesDefault = false;
      if (!layout.entries()[i].visible) allVisible = false;
    }
    check(orderMatchesDefault,
          "panel layout: the default order is exactly controlsSections()'s own order");
    check(allVisible, "panel layout: every section starts visible in the default layout");

    // Every enumerator has a key, and every key decodes back to the same
    // enumerator -- the table app/ControlsColumnLayout.cpp keeps is total,
    // the same way controlsSectionSpec() is asserted total for every
    // enumerator in app/selftest/ControlsLayout.cpp.
    bool keysRoundTrip = true;
    bool keysNonEmptyAndLower = true;
    for (const ControlsSection s : kAllSections) {
      const std::string key = controlsSectionKey(s);
      if (key.empty()) keysNonEmptyAndLower = false;
      for (const char c : key)
        if (!((c >= 'a' && c <= 'z') || c == '_')) keysNonEmptyAndLower = false;
      ControlsSection back;
      if (!controlsSectionFromKey(key, &back) || back != s) keysRoundTrip = false;
      for (size_t j = 0; j < std::size(kAllSections); ++j) {
        if (kAllSections[j] == s) continue;
        if (controlsSectionKey(kAllSections[j]) == key) keysRoundTrip = false;  // must be unique
      }
    }
    check(keysNonEmptyAndLower, "panel layout: every key is non-empty, lower-case snake_case");
    check(keysRoundTrip,
          "panel layout: every section's key is unique and decodes back to that section");
    check(!controlsSectionFromKey("not_a_real_section", nullptr),
          "panel layout: a string that names no section is rejected by controlsSectionFromKey()");
  }

  // ==========================================================================
  // 2. Mutators preserve the exactly-once invariant, and do what they say
  // ==========================================================================
  {
    ControlsColumnLayout layout;

    // moveTo the front, the back, an out-of-range index (clamped), and a
    // no-op (already there) -- each checked for the invariant AND for having
    // actually moved (or not) the section asked for.
    layout.moveTo(ControlsSection::Solver, 0);
    check(exactlyOnceEach(layout), "panel layout: moveTo() to the front preserves the invariant");
    check(layout.indexOf(ControlsSection::Solver) == 0,
          "panel layout: moveTo(Solver, 0) actually put SOLVER first");
    check(layout.entries()[1].section == ControlsSection::Color,
          "panel layout: and COLOR, previously first, is now second -- a shift, not a swap");

    layout.moveTo(ControlsSection::Color, 999);  // clamped
    check(exactlyOnceEach(layout), "panel layout: moveTo() with an out-of-range index preserves "
                                    "the invariant");
    check(layout.indexOf(ControlsSection::Color) == layout.entries().size() - 1,
          "panel layout: and the out-of-range index clamps to the last position");

    const size_t beforeNoop = layout.indexOf(ControlsSection::History);
    layout.moveTo(ControlsSection::History, beforeNoop);
    check(layout.indexOf(ControlsSection::History) == beforeNoop && exactlyOnceEach(layout),
          "panel layout: moveTo() to a section's own index is a genuine no-op");

    // moveUp / moveDown, including the boundary no-ops.
    ControlsColumnLayout ordered;
    const size_t firstIdx = 0;
    const ControlsSection first = ordered.entries()[firstIdx].section;
    ordered.moveUp(first);  // already at the front
    check(ordered.indexOf(first) == 0 && exactlyOnceEach(ordered),
          "panel layout: moveUp() on the first section is a no-op, not a crash or a wrap");
    const ControlsSection last = ordered.entries().back().section;
    ordered.moveDown(last);  // already at the back
    check(ordered.indexOf(last) == ordered.entries().size() - 1 && exactlyOnceEach(ordered),
          "panel layout: moveDown() on the last section is a no-op, not a crash or a wrap");

    const ControlsSection second = ordered.entries()[1].section;
    ordered.moveUp(second);
    check(ordered.indexOf(second) == 0 && ordered.indexOf(first) == 1 && exactlyOnceEach(ordered),
          "panel layout: moveUp() swaps a section with its predecessor");
    ordered.moveDown(second);
    check(ordered.indexOf(second) == 1 && ordered.indexOf(first) == 0 && exactlyOnceEach(ordered),
          "panel layout: moveDown() undoes that swap, back to the original order");

    // Visibility is independent of order, and every combination is legal --
    // including hiding EVERY section, which this header's own design note
    // says must not be refused here.
    ControlsColumnLayout vis;
    for (const ControlsSection s : kAllSections) vis.setVisible(s, false);
    check(vis.visibleSections().empty() && exactlyOnceEach(vis),
          "panel layout: hiding every section is legal -- visibleSections() is empty but "
          "entries() still holds all thirteen");
    vis.setVisible(ControlsSection::Layers, true);
    vis.setVisible(ControlsSection::Color, true);
    check(vis.visibleSections().size() == 2 &&
              vis.isVisible(ControlsSection::Layers) && vis.isVisible(ControlsSection::Color) &&
              !vis.isVisible(ControlsSection::Grid),
          "panel layout: setVisible() toggles exactly the section asked, independent of order");
    // visibleSections() preserves column order, not the order setVisible()
    // was called in -- COLOR precedes LAYERS in the default order even
    // though LAYERS was made visible first above.
    check(vis.visibleSections()[0] == ControlsSection::Color &&
              vis.visibleSections()[1] == ControlsSection::Layers,
          "panel layout: visibleSections() is filtered by column order, not by call order");

    layout.resetToDefault();
    check(exactlyOnceEach(layout) && layout.entries()[0].section == ControlsSection::Color &&
              layout.entries()[0].visible,
          "panel layout: resetToDefault() returns to controlsSections()'s own order, fully "
          "visible, after a layout has been rearranged and had sections hidden");
  }

  // ==========================================================================
  // 3. serialize()/parse() round-trips a rearranged, partially-hidden layout
  // ==========================================================================
  {
    ControlsColumnLayout layout;
    layout.moveTo(ControlsSection::Solver, 0);
    layout.moveTo(ControlsSection::Grid, 1);
    layout.setVisible(ControlsSection::History, false);
    layout.setVisible(ControlsSection::Comps, false);

    const std::string text = layout.serialize();
    check(contains(text, kControlsColumnLayoutFileHeader),
          "panel layout: serialize() writes the header line");
    check(contains(text, "section solver 1") && contains(text, "section grid 1") &&
              contains(text, "section history 0") && contains(text, "section comps 0"),
          "panel layout: serialize() writes stable text keys, never an ordinal, with the right "
          "visibility flags");

    ControlsColumnLayout reloaded;
    reloaded.parse(text);
    check(exactlyOnceEach(reloaded), "panel layout: parse() of a well-formed file preserves the "
                                      "invariant");
    bool sameOrderAndVisibility = reloaded.entries().size() == layout.entries().size();
    for (size_t i = 0; sameOrderAndVisibility && i < layout.entries().size(); ++i)
      sameOrderAndVisibility = reloaded.entries()[i].section == layout.entries()[i].section &&
                               reloaded.entries()[i].visible == layout.entries()[i].visible;
    check(sameOrderAndVisibility,
          "panel layout: parse(serialize()) reproduces the exact order and visibility -- not "
          "merely an equivalent one");
  }

  // ==========================================================================
  // 4. Round-trip repair, each rule in isolation
  // ==========================================================================
  {
    // 4a. Unknown section name -- a section that no longer exists is
    // ignored, not treated as malformed.
    ControlsColumnLayout layout;
    layout.parse(
        "naturalPaint-panel-layout 1\n"
        "section color 1\n"
        "section teleport_tool 1\n"  // unknown -- from a build this one is not
        "section layers 1\n"
        "section history 1\n"
        "section comps 1\n"
        "section grade 0\n"
        "section histogram 0\n"
        "section brush_library 0\n"
        "section brush 0\n"
        "section pigment 0\n"
        "section medium 0\n"
        "section board_tilt 0\n"
        "section grid 0\n"
        "section solver 0\n");
    check(exactlyOnceEach(layout),
          "panel layout: repair 4a -- an unknown section name still leaves every real section "
          "present exactly once");
    check(layout.indexOf(ControlsSection::Layers) == 1,
          "panel layout: repair 4a -- and the unknown line contributed no row: LAYERS sits "
          "right after COLOR, not after a phantom third entry");

    // 4b. Missing section -- a file that predates a section (SOLVER, say)
    // must not make it vanish: it is appended, in controlsSections()'s own
    // relative order among whatever else is missing.
    ControlsColumnLayout missing;
    missing.parse(
        "naturalPaint-panel-layout 1\n"
        "section color 1\n"
        "section layers 1\n"
        "section history 1\n"
        "section comps 1\n"
        "section grade 0\n"
        "section brush_library 0\n"
        "section brush 0\n"
        "section pigment 0\n"
        "section medium 0\n"
        // board_tilt, grid and solver are all absent, as if written by a
        // build before they existed.
        "\n");
    check(exactlyOnceEach(missing),
          "panel layout: repair 4b -- **the section this file's writer never knew about is "
          "present anyway** -- this is the silent-no-op failure class this codebase already has "
          "a name for, and it is what this whole repair rule exists to close");
    check(missing.indexOf(ControlsSection::BoardTilt) < missing.indexOf(ControlsSection::Grid) &&
              missing.indexOf(ControlsSection::Grid) < missing.indexOf(ControlsSection::Solver),
          "panel layout: repair 4b -- appended in controlsSections()'s own relative order "
          "(BOARD TILT, then GRID, then SOLVER), not in an arbitrary order");
    check(missing.isVisible(ControlsSection::Grid) && missing.isVisible(ControlsSection::Solver),
          "panel layout: repair 4b -- an appended section is visible by default, matching what "
          "resetToDefault() would have given it");

    // 4b-2. The concrete case C2 (docs/reachability-audit.md; PRD D2, P0)
    // exists to close: a layout file written by a build from BEFORE
    // HISTOGRAM existed -- every OTHER current section present and
    // accounted for, HISTOGRAM simply never mentioned because its author's
    // build had no such enumerator -- must still load with HISTOGRAM
    // present. This is the exact scenario every real user's saved
    // panel-layout.txt is in the instant this section ships: without the
    // missing-section repair, HISTOGRAM would silently never appear for
    // anyone who already has a layout file on disk, which is the "feature
    // that exists that nothing reaches" defect class this step exists to
    // close, one level up (the file would reach it; the repair is what
    // makes the column draw it).
    ControlsColumnLayout predatesHistogram;
    predatesHistogram.parse(
        "naturalPaint-panel-layout 1\n"
        "section color 1\n"
        "section layers 1\n"
        "section history 1\n"
        "section comps 1\n"
        "section grade 0\n"
        "section brush_library 0\n"
        "section brush 0\n"
        "section pigment 0\n"
        "section medium 0\n"
        "section board_tilt 0\n"
        "section grid 0\n"
        "section solver 0\n");  // no "section histogram" line anywhere
    check(exactlyOnceEach(predatesHistogram),
          "panel layout: repair 4b-2 -- **a file written before HISTOGRAM existed still loads "
          "with HISTOGRAM present** -- the specific new-section case this repair rule exists for");
    check(predatesHistogram.isVisible(ControlsSection::Histogram),
          "panel layout: repair 4b-2 -- and it is visible, matching what resetToDefault() would "
          "have given a section this old file never had a chance to hide");
    check(predatesHistogram.entries().back().section == ControlsSection::Histogram,
          "panel layout: repair 4b-2 -- appended at the end, since it is the only section this "
          "file is missing");

    // 4c. Duplicate section name -- the first occurrence wins.
    ControlsColumnLayout dup;
    dup.parse(
        "naturalPaint-panel-layout 1\n"
        "section color 1\n"
        "section layers 0\n"   // first: LAYERS hidden
        "section history 1\n"
        "section comps 1\n"
        "section grade 0\n"
        "section brush_library 0\n"
        "section brush 0\n"
        "section pigment 0\n"
        "section medium 0\n"
        "section board_tilt 0\n"
        "section grid 0\n"
        "section solver 0\n"
        "section layers 1\n");  // duplicate, later, disagreeing: ignored
    check(exactlyOnceEach(dup),
          "panel layout: repair 4c -- a duplicated section still appears exactly once, not "
          "twice");
    check(!dup.isVisible(ControlsSection::Layers),
          "panel layout: repair 4c -- **the FIRST occurrence wins**: LAYERS stayed hidden, the "
          "later contradicting line was ignored");

    // 4d. A malformed line is SKIPPED, which is the fourth per-line repair
    // rather than a verdict on the file. The section that line failed to
    // name is then simply one this pass did not see, so 4b appends it -- so
    // the outcome is always a complete, valid layout, and a file with no
    // recognisable lines at all lands on the full default through exactly
    // the same route.
    auto fallsBackToDefault = [&](const std::string& text, const char* what) {
      ControlsColumnLayout garbage;
      garbage.moveTo(ControlsSection::Solver, 0);  // start from a non-default state
      garbage.parse(text);
      const bool isDefault = [&] {
        if (!exactlyOnceEach(garbage)) return false;
        const std::vector<ControlsSectionSpec>& def = controlsSections();
        for (size_t i = 0; i < def.size(); ++i)
          if (garbage.entries()[i].section != def[i].section || !garbage.entries()[i].visible)
            return false;
        return true;
      }();
      check(isDefault, what);
    };

    fallsBackToDefault("this is not the file format at all, just prose.\nanother line.\n",
                        "panel layout: repair 4d -- a genuinely garbage file falls back to the "
                        "full default layout");
    // Each of these has no salvageable line at all, so every section is
    // missing and 4b rebuilds the default -- the same outcome the old
    // whole-file-invalidation rule produced, by a route that does not
    // depend on there being nothing worth keeping.
    fallsBackToDefault("naturalPaint-panel-layout 1\nsection color\n",  // truncated: no flag
                        "panel layout: repair 4d -- a truncated line (missing the visibility "
                        "flag) is skipped, leaving nothing, so the default is rebuilt");
    fallsBackToDefault("naturalPaint-panel-layout 1\nsection color maybe\n",  // bad flag
                        "panel layout: repair 4d -- a visibility flag that is not exactly 0 or 1 "
                        "is skipped");
    fallsBackToDefault(
        "naturalPaint-panel-layout 1\nsection color 1 extra_token\n",  // extra token
        "panel layout: repair 4d -- a line with an extra trailing token is skipped");

    // **The case that actually distinguishes skip from discard.** Every
    // assertion above lands on the default either way, so none of them can
    // tell the two rules apart -- a file whose good lines happen to be in
    // default order rebuilds the default under both. This one does not: the
    // arrangement is deliberately NOT the default, so discarding the file
    // would silently throw away a layout the user set, and skipping keeps
    // it.
    {
      ControlsColumnLayout salvaged;
      salvaged.parse(
          "naturalPaint-panel-layout 1\nsection solver 1\nsection color 0\ngarbage garbage\n");
      check(exactlyOnceEach(salvaged),
            "panel layout: repair 4d -- a file with one bad line still yields every section "
            "exactly once");
      check(salvaged.entries().size() > 1 &&
                salvaged.entries()[0].section == ControlsSection::Solver &&
                salvaged.entries()[1].section == ControlsSection::Color,
            "panel layout: repair 4d -- **one malformed line does NOT discard the good lines "
            "around it**: SOLVER-then-COLOR, an order the default does not have, survives");
      check(!salvaged.isVisible(ControlsSection::Color),
            "panel layout: repair 4d -- and the visibility the surviving lines carried is kept, "
            "not reset with the bad line");
    }

    // Empty text and a missing file both resolve to the default via the
    // SAME "every section missing" rule as 4b, not a special case.
    ControlsColumnLayout empty;
    empty.moveTo(ControlsSection::Solver, 0);
    empty.parse("");
    const bool emptyIsDefault = [&] {
      if (!exactlyOnceEach(empty)) return false;
      const std::vector<ControlsSectionSpec>& def = controlsSections();
      for (size_t i = 0; i < def.size(); ++i)
        if (empty.entries()[i].section != def[i].section || !empty.entries()[i].visible)
          return false;
      return true;
    }();
    check(emptyIsDefault,
          "panel layout: parsing an empty string resolves to the default layout through the "
          "same 'every section missing' path as 4b, not a separate special case");
  }

  // ==========================================================================
  // 5. Persistence: a real save/load round trip, and the missing-file case,
  //    entirely under $NP_PANEL_LAYOUT so the real settings file is never
  //    touched
  // ==========================================================================
  {
    const std::string root = "selftest_panellayout";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::string path = root + "/panel-layout.txt";

    const char* prev = std::getenv("NP_PANEL_LAYOUT");
    const std::string saved = prev ? prev : "";
    setenv("NP_PANEL_LAYOUT", path.c_str(), 1);

    check(defaultControlsColumnLayoutFilePath() == path,
          "panel layout: $NP_PANEL_LAYOUT overrides the settings path, so this section cannot "
          "touch ~/Library/Application Support/naturalPaint");

    // 5a. Loading a file that has never been saved is the ordinary first-run
    // case, not an error.
    {
      std::string err;
      ControlsColumnLayout fresh;
      fresh.moveTo(ControlsSection::Solver, 0);  // perturb, so a no-op load couldn't hide a bug
      check(!fs::exists(path, ec), "panel layout: no file exists yet");
      check(fresh.loadFromFile(path, &err) && err.empty(),
            "panel layout: loadFromFile() on a missing path returns true, not an error");
      check(exactlyOnceEach(fresh) && fresh.entries()[0].section == ControlsSection::Color,
            "panel layout: and yields the default layout, discarding the pre-load state");
    }

    // 5b. A genuine rearranged, partially-hidden layout survives a save and
    // a reload in a fresh instance.
    ControlsColumnLayout authored;
    authored.moveTo(ControlsSection::Pigment, 0);
    authored.moveTo(ControlsSection::Medium, 1);
    authored.setVisible(ControlsSection::Solver, false);
    authored.setVisible(ControlsSection::Grid, false);
    authored.setVisible(ControlsSection::BoardTilt, false);
    {
      std::string err;
      check(authored.saveToFile(path, &err) && err.empty(),
            "panel layout: saveToFile() of a rearranged, partially-hidden layout succeeds");
    }
    ControlsColumnLayout reloaded;
    {
      std::string err;
      check(reloaded.loadFromFile(path, &err) && err.empty(),
            "panel layout: loadFromFile() reads it back into a brand-new instance");
    }
    check(exactlyOnceEach(reloaded), "panel layout: the reload still holds the invariant");
    bool sameOrderAndVisibility = reloaded.entries().size() == authored.entries().size();
    for (size_t i = 0; sameOrderAndVisibility && i < authored.entries().size(); ++i)
      sameOrderAndVisibility = reloaded.entries()[i].section == authored.entries()[i].section &&
                               reloaded.entries()[i].visible == authored.entries()[i].visible;
    check(sameOrderAndVisibility,
          "panel layout: **the exact order and the exact visibility survive a save, a process-"
          "boundary-shaped reload, and a fresh instance** -- this is the round trip the "
          "persistence exists for");

    // 5c. Durability: an abandoned `.tmp` cannot corrupt the real file, and
    // a completed save actually goes through the temp-file-then-rename path
    // -- app/selftest/UserBrushLibrary.cpp's own durability section, same
    // shape, same reasoning (see this module's header §4 for what this does
    // and does not prove).
    {
      std::ofstream stale(path + ".tmp", std::ios::binary | std::ios::trunc);
      stale << "leftover from an unrelated earlier run";
    }
    check(fs::exists(path + ".tmp", ec), "panel layout: durability -- a stale .tmp is seeded "
                                          "first");

    ControlsColumnLayout known;
    known.moveTo(ControlsSection::Grade, 0);
    {
      std::string err;
      check(known.saveToFile(path, &err) && err.empty(),
            "panel layout: durability -- a real save completes, establishing a known-good "
            "file");
    }
    check(!fs::exists(path + ".tmp", ec),
          "panel layout: durability -- **the save consumed the stale .tmp**, not merely avoided "
          "leaving a new one");
    const std::string goodBytes = readWhole(path);

    // Simulate a crash: a `.tmp` opened, partially written, never renamed.
    // Never call saveToFile() again in this block.
    {
      std::ofstream tmp(path + ".tmp", std::ios::binary | std::ios::trunc);
      tmp << "naturalPaint-panel-layout 1\nsection col";  // cut off mid-line, no rename
    }
    check(fs::exists(path + ".tmp", ec), "panel layout: durability -- the abandoned .tmp exists");
    check(readWhole(path) == goodBytes,
          "panel layout: durability -- **the real file is byte-for-byte unchanged** by an "
          "abandoned .tmp sitting right beside it");

    ControlsColumnLayout afterCrash;
    {
      std::string err;
      check(afterCrash.loadFromFile(path, &err) && err.empty(),
            "panel layout: durability -- loading after the 'crash' reads the last GOOD save");
    }
    check(afterCrash.indexOf(ControlsSection::Grade) == 0,
          "panel layout: durability -- and it is really the known-good layout (GRADE first), "
          "not anything from the abandoned .tmp");

    fs::remove(path + ".tmp", ec);

    // Restore the environment and clean up -- nothing here may leave a trace
    // in the developer's real settings directory.
    if (saved.empty()) unsetenv("NP_PANEL_LAYOUT");
    else setenv("NP_PANEL_LAYOUT", saved.c_str(), 1);
    fs::remove_all(root, ec);
    check(!fs::exists(root, ec), "panel layout: every file this section wrote is removed");
  }

  std::printf("[selftest] controls column layout %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
