#include "app/selftest/Support.hpp"

#include "app/PanelLayout.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/DockLayout.hpp"

namespace np {
namespace {

// Every ControlsSection enumerator, independent of both `controlsSections()`'s
// draw order and `PanelLayout`'s own order -- used only to check the
// exactly-once invariant from the outside, the same role app/
// selftest/ControlsLayout.cpp's own `kAll` plays for `controlsSections()`.
//
// **Written out by hand on purpose.** Deriving this from `controlsSections()`
// would make the invariant check circular: a section missing from that list
// would be missing from this one too, and the test would pass by agreeing with
// the bug.
constexpr ControlsSection kAllSections[] = {
    ControlsSection::Tools,        ControlsSection::Options,   ControlsSection::Color,
    ControlsSection::Layers,       ControlsSection::History,   ControlsSection::Comps,
    ControlsSection::Grade,        ControlsSection::Histogram, ControlsSection::BrushLibrary,
    ControlsSection::Brush,        ControlsSection::Pigment,   ControlsSection::Medium,
    ControlsSection::BoardTilt,    ControlsSection::Grid,      ControlsSection::Solver,
};

constexpr PanelPlacement kAllPlacements[] = {
    PanelPlacement::Left,   PanelPlacement::Right,  PanelPlacement::Top,
    PanelPlacement::Bottom, PanelPlacement::Flyout, PanelPlacement::Hidden,
};

bool exactlyOnceEach(const PanelLayout& layout) {
  const std::vector<PanelEntry>& entries = layout.entries();
  if (entries.size() != std::size(kAllSections)) return false;
  for (const ControlsSection s : kAllSections) {
    size_t seen = 0;
    for (const PanelEntry& e : entries)
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

// app/PanelLayout -- the headless model behind the dockable panel system.
//
// See app/SelfTest.hpp for the full list of what this proves and the header of
// the module itself for the design. This replaces the suite that covered the
// same module under its single-column shape (`app/ControlsColumnLayout`);
// every assertion that suite made is carried forward here, because the
// exactly-once invariant, the key-stability rules and the repair rules are
// properties of a model that grew two axes rather than properties of the
// column that is gone.
//
// The one worth naming at the top is section 5's. `ControlsSection` gained two
// enumerators AT THE FRONT in this revision. Under ordinal-keyed persistence
// every panel-layout file ever written would now decode one or two places off
// -- a user's LAYERS panel silently becoming their SOLVER panel, with no parse
// error anywhere. Section 5 writes a file in the pre-revision grammar, naming
// only the thirteen sections that existed then, and asserts every one of them
// comes back as itself.
bool runPanelLayoutTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // ==========================================================================
  // 1. The default layout IS the outgoing chrome
  // ==========================================================================
  {
    PanelLayout layout;
    check(exactlyOnceEach(layout),
          "panel layout: a fresh layout holds every ControlsSection exactly once");
    check(layout.entries().size() == controlsSections().size(),
          "panel layout: the default has exactly as many rows as controlsSections()");

    check(layout.placementOf(ControlsSection::Tools) == PanelPlacement::Left,
          "panel layout: TOOLS starts in the left dock, where the tool palette band was");
    check(layout.placementOf(ControlsSection::Options) == PanelPlacement::Top,
          "panel layout: OPTIONS starts in the top dock, where the options bar band was");

    bool everythingElseRight = true;
    for (const PanelEntry& e : layout.entries()) {
      if (e.section == ControlsSection::Tools || e.section == ControlsSection::Options) continue;
      if (e.placement != PanelPlacement::Right) everythingElseRight = false;
    }
    check(everythingElseRight,
          "panel layout: every other panel starts in the right dock, where the column was");

    // The right dock's order is `controlsSections()`'s own order with the two
    // new panels removed -- i.e. the outgoing column's order, unchanged.
    const std::vector<ControlsSection> right = layout.sectionsIn(PanelPlacement::Right);
    std::vector<ControlsSection> expected;
    for (const ControlsSectionSpec& spec : controlsSections()) {
      if (spec.section == ControlsSection::Tools || spec.section == ControlsSection::Options)
        continue;
      expected.push_back(spec.section);
    }
    check(right == expected,
          "panel layout: **the right dock's default order is the outgoing column's order** -- "
          "the revamp changed what the layout can express, not what a first run looks like");

    bool allUnitWeight = true;
    for (const PanelEntry& e : layout.entries())
      if (e.weight != kPanelDefaultWeight) allUnitWeight = false;
    check(allUnitWeight, "panel layout: every panel starts at the default weight");

    // **The default collapse set is `controlsSections()`'s `defaultOpen`,
    // inverted** -- asserted in both directions, because either alone is
    // satisfiable by the wrong rule. This is what keeps the default
    // arrangement out of ui/DockLayout's overflow branch: thirteen expanded
    // panels in the right dock exceed its floor budget on any ordinary window,
    // and the dock would fall back to the very scrolling this feature removes.
    bool collapseMatchesDefaultOpen = true;
    size_t expanded = 0;
    for (const ControlsSectionSpec& spec : controlsSections()) {
      if (layout.isCollapsed(spec.section) == spec.defaultOpen) collapseMatchesDefaultOpen = false;
      if (!layout.isCollapsed(spec.section)) ++expanded;
    }
    check(collapseMatchesDefaultOpen,
          "panel layout: **a panel starts expanded exactly when controlsSections() marks it "
          "defaultOpen** -- no second copy of that decision");
    check(expanded == 6,
          "panel layout: which is six panels expanded (TOOLS, OPTIONS, COLOR, LAYERS, HISTORY, "
          "COMPS) and nine collapsed to their headers");

    // And the consequence, checked as arithmetic rather than assumed: the
    // default right dock fits a 1024 px window without overflowing.
    std::vector<DockSlotSpec> specs;
    for (const ControlsSection sec : layout.sectionsIn(PanelPlacement::Right)) {
      DockSlotSpec sp;
      sp.collapsed = layout.isCollapsed(sec);
      sp.weight = layout.weightOf(sec);
      sp.minExtent = kPanelMinHeight;
      sp.headerExtent = kPanelHeaderExtent;
      specs.push_back(sp);
    }
    const DockTiling t =
        dockTile(AtelierRect{0.0f, 0.0f, kRightColumnW, 900.0f}, DockSide::Right, specs);
    check(!t.overflowed,
          "panel layout: **the default right dock does NOT overflow** a 900 px window -- which "
          "is the whole point of collapsing the tuning sections rather than expanding them");

    const PanelDockExtents d = layout.dockExtents();
    check(d.left == kDefaultDockExtents.left && d.right == kDefaultDockExtents.right &&
              d.top == kDefaultDockExtents.top && d.bottom == kDefaultDockExtents.bottom,
          "panel layout: the default dock extents are ui/AtelierLayout's kDefaultDockExtents");
    check(layout.sectionsIn(PanelPlacement::Bottom).empty() &&
              layout.sectionsIn(PanelPlacement::Flyout).empty() &&
              layout.sectionsIn(PanelPlacement::Hidden).empty(),
          "panel layout: nothing starts on the bottom, in a flyout, or hidden");
  }

  // ==========================================================================
  // 2. Keys are stable text, for sections AND for placements
  // ==========================================================================
  {
    bool keysRoundTrip = true;
    bool keysNonEmptyAndLower = true;
    for (const ControlsSection s : kAllSections) {
      const std::string key = controlsSectionKey(s);
      if (key.empty()) keysNonEmptyAndLower = false;
      for (const char c : key)
        if (!((c >= 'a' && c <= 'z') || c == '_')) keysNonEmptyAndLower = false;
      ControlsSection back;
      if (!controlsSectionFromKey(key, &back) || back != s) keysRoundTrip = false;
      for (const ControlsSection other : kAllSections) {
        if (other == s) continue;
        if (controlsSectionKey(other) == key) keysRoundTrip = false;  // must be unique
      }
    }
    check(keysNonEmptyAndLower, "panel layout: every section key is non-empty lower-case snake");
    check(keysRoundTrip,
          "panel layout: every section's key is unique and decodes back to that section");
    check(!controlsSectionFromKey("not_a_real_section", nullptr),
          "panel layout: a string that names no section is rejected");

    bool placementsRoundTrip = true;
    for (const PanelPlacement p : kAllPlacements) {
      const std::string key = panelPlacementKey(p);
      PanelPlacement back;
      if (key.empty() || !panelPlacementFromKey(key, &back) || back != p)
        placementsRoundTrip = false;
    }
    check(placementsRoundTrip,
          "panel layout: every placement's key is non-empty and decodes back to that placement");
    check(!panelPlacementFromKey("northwest", nullptr),
          "panel layout: a string that names no placement is rejected");
    check(panelPlacementIsDock(PanelPlacement::Left) &&
              panelPlacementIsDock(PanelPlacement::Bottom) &&
              !panelPlacementIsDock(PanelPlacement::Flyout) &&
              !panelPlacementIsDock(PanelPlacement::Hidden),
          "panel layout: exactly the four edges count as docks");
  }

  // ==========================================================================
  // 3. Mutators preserve the invariant, and do what they say
  // ==========================================================================
  {
    PanelLayout layout;

    layout.setPlacement(ControlsSection::Layers, PanelPlacement::Bottom);
    check(exactlyOnceEach(layout) &&
              layout.placementOf(ControlsSection::Layers) == PanelPlacement::Bottom,
          "panel layout: setPlacement() moves a panel and keeps the invariant");
    check(layout.sectionsIn(PanelPlacement::Bottom).size() == 1,
          "panel layout: and the target placement now holds exactly that one panel");

    // Weight and collapse survive a trip out to a flyout and back -- the
    // property that makes putting a panel away non-destructive.
    layout.setWeight(ControlsSection::Layers, 3.5f);
    layout.setCollapsed(ControlsSection::Layers, true);
    layout.setPlacement(ControlsSection::Layers, PanelPlacement::Flyout);
    layout.setPlacement(ControlsSection::Layers, PanelPlacement::Right);
    check(layout.weightOf(ControlsSection::Layers) == 3.5f &&
              layout.isCollapsed(ControlsSection::Layers),
          "panel layout: **a panel put away and brought back returns at the size it had**");
    check(layout.sectionsIn(PanelPlacement::Right).back() == ControlsSection::Layers,
          "panel layout: setPlacement() appends at the end of the target placement");

    // An out-of-range index is clamped, not refused.
    layout.setPlacementAt(ControlsSection::Layers, PanelPlacement::Right, 9999);
    check(exactlyOnceEach(layout) &&
              layout.sectionsIn(PanelPlacement::Right).back() == ControlsSection::Layers,
          "panel layout: an out-of-range placement index is clamped to the end, not refused");
    layout.setPlacementAt(ControlsSection::Layers, PanelPlacement::Right, 0);
    check(exactlyOnceEach(layout) &&
              layout.sectionsIn(PanelPlacement::Right).front() == ControlsSection::Layers,
          "panel layout: index 0 puts a panel at the head of its dock");

    // A weight that could poison ui/DockLayout's arithmetic is repaired here.
    layout.setWeight(ControlsSection::Layers, 0.0f);
    check(layout.weightOf(ControlsSection::Layers) >= kPanelMinWeight,
          "panel layout: a zero weight is clamped to the floor, never stored as zero");
    layout.setWeight(ControlsSection::Layers, std::numeric_limits<float>::quiet_NaN());
    check(std::isfinite(layout.weightOf(ControlsSection::Layers)),
          "panel layout: a NaN weight is repaired to the default, never stored");

    // Hiding every panel is legal -- the way back in is the PANELS menu, which
    // is deliberately not in any dock.
    for (const ControlsSection s : kAllSections) layout.setPlacement(s, PanelPlacement::Hidden);
    check(exactlyOnceEach(layout) &&
              layout.sectionsIn(PanelPlacement::Hidden).size() == std::size(kAllSections),
          "panel layout: **hiding every panel at once is legal**, not force-corrected");
    const PanelDockExtents eff = layout.effectiveDockExtents();
    check(eff.left == 0.0f && eff.right == 0.0f && eff.top == 0.0f && eff.bottom == 0.0f,
          "panel layout: with every panel hidden no dock is on screen at all");
    check(layout.dockExtents().right == kDefaultDockExtents.right,
          "panel layout: **and the stored extent is preserved** -- a dock emptied and refilled "
          "comes back the size it was");

    layout.resetToDefault();
    check(exactlyOnceEach(layout) &&
              layout.placementOf(ControlsSection::Tools) == PanelPlacement::Left,
          "panel layout: resetToDefault() restores the default arrangement and the invariant");
  }

  // ==========================================================================
  // 4. moveUp/moveDown act WITHIN a placement, never across one
  // ==========================================================================
  {
    PanelLayout layout;
    // COLOR is the first panel in the right dock; the two entries ahead of it
    // in `entries()` are TOOLS (left) and OPTIONS (top).
    check(layout.indexOf(ControlsSection::Color) == 2,
          "panel layout: COLOR is the third entry overall and the first in the right dock");
    layout.moveUp(ControlsSection::Color);
    check(layout.placementOf(ControlsSection::Color) == PanelPlacement::Right &&
              layout.sectionsIn(PanelPlacement::Right).front() == ControlsSection::Color,
          "panel layout: **moveUp() on the first panel of a dock is a no-op**, not a jump to "
          "whichever dock happens to precede it in the list");

    const std::vector<ControlsSection> before = layout.sectionsIn(PanelPlacement::Right);
    layout.moveDown(ControlsSection::Color);
    const std::vector<ControlsSection> after = layout.sectionsIn(PanelPlacement::Right);
    check(after.size() == before.size() && after[0] == before[1] && after[1] == before[0],
          "panel layout: moveDown() swaps a panel with its next neighbour in the same dock");
    check(layout.placementOf(ControlsSection::Tools) == PanelPlacement::Left &&
              layout.placementOf(ControlsSection::Options) == PanelPlacement::Top,
          "panel layout: and leaves every other dock's contents untouched");

    layout.moveUp(ControlsSection::Color);
    check(layout.sectionsIn(PanelPlacement::Right) == before,
          "panel layout: moveUp() undoes moveDown() exactly");

    // The last panel in a dock cannot move down out of it either.
    const ControlsSection last = layout.sectionsIn(PanelPlacement::Right).back();
    layout.moveDown(last);
    check(layout.sectionsIn(PanelPlacement::Right).back() == last &&
              layout.placementOf(last) == PanelPlacement::Right,
          "panel layout: moveDown() on the last panel of a dock is a no-op");
    check(exactlyOnceEach(layout), "panel layout: reordering never disturbs the invariant");
  }

  // ==========================================================================
  // 5. A version 1 file still reads -- and every section is still itself
  // ==========================================================================
  {
    // Exactly what the single-column build wrote, naming only the thirteen
    // sections that existed before `Tools` and `Options` were inserted at the
    // FRONT of the enum. Under ordinal keys every one of these would now
    // decode two places off.
    const std::string v1 =
        "naturalPaint-panel-layout 1\n"
        "section grade 1\n"
        "section layers 1\n"
        "section color 1\n"
        "section history 0\n"
        "section comps 1\n"
        "section histogram 0\n"
        "section brush_library 1\n"
        "section brush 1\n"
        "section pigment 0\n"
        "section medium 0\n"
        "section board_tilt 0\n"
        "section grid 0\n"
        "section solver 0\n";
    PanelLayout layout;
    layout.parse(v1);

    check(exactlyOnceEach(layout),
          "panel layout: a version 1 file parses to a complete, valid layout");
    const std::vector<ControlsSection> right = layout.sectionsIn(PanelPlacement::Right);
    check(right.size() == 6 && right[0] == ControlsSection::Grade &&
              right[1] == ControlsSection::Layers && right[2] == ControlsSection::Color,
          "panel layout: **version 1's `section <key> 1` lands in the right dock, in order** -- "
          "a user's arrangement from the previous build survives the revamp");
    check(layout.placementOf(ControlsSection::History) == PanelPlacement::Hidden &&
              layout.placementOf(ControlsSection::Solver) == PanelPlacement::Hidden,
          "panel layout: version 1's `section <key> 0` lands hidden");
    check(layout.placementOf(ControlsSection::Tools) == PanelPlacement::Left &&
              layout.placementOf(ControlsSection::Options) == PanelPlacement::Top,
          "panel layout: **the two panels version 1 could not name arrive at their DEFAULT "
          "placements**, not swept into the right dock with everything else");
    const PanelDockExtents d = layout.dockExtents();
    check(d.left == kDefaultDockExtents.left && d.right == kDefaultDockExtents.right &&
              d.top == kDefaultDockExtents.top,
          "panel layout: a file with no `dock` lines lands on the default extents, not on zero");
  }

  // ==========================================================================
  // 6. The round-trip repair rules, each in isolation
  // ==========================================================================
  {
    // Unknown section name: ignored, and the section it failed to name arrives
    // via the append rule.
    PanelLayout unknown;
    unknown.parse(
        "naturalPaint-panel-layout 2\n"
        "panel not_a_section right 1.000 0\n"
        "panel layers bottom 2.000 0\n");
    check(exactlyOnceEach(unknown) &&
              unknown.placementOf(ControlsSection::Layers) == PanelPlacement::Bottom,
          "panel layout: an unknown section name is ignored and the rest of the file applies");

    // Unknown placement name: same treatment.
    PanelLayout badPlace;
    badPlace.parse(
        "naturalPaint-panel-layout 2\n"
        "panel layers northwest 1.000 0\n");
    check(exactlyOnceEach(badPlace) &&
              badPlace.placementOf(ControlsSection::Layers) == PanelPlacement::Right,
          "panel layout: an unknown placement name is ignored and that panel takes its default");

    // Duplicate: first wins.
    PanelLayout dup;
    dup.parse(
        "naturalPaint-panel-layout 2\n"
        "panel layers bottom 1.000 0\n"
        "panel layers top 9.000 1\n");
    check(exactlyOnceEach(dup) && dup.placementOf(ControlsSection::Layers) == PanelPlacement::Bottom &&
              !dup.isCollapsed(ControlsSection::Layers),
          "panel layout: a duplicated section keeps its FIRST occurrence and drops the rest");

    // Missing section: appended at its DEFAULT placement, in
    // controlsSections()'s own relative order.
    PanelLayout missing;
    missing.parse(
        "naturalPaint-panel-layout 2\n"
        "panel layers right 1.000 0\n");
    check(exactlyOnceEach(missing),
          "panel layout: a file naming one section still yields all fifteen");
    check(missing.placementOf(ControlsSection::Tools) == PanelPlacement::Left &&
              missing.placementOf(ControlsSection::Options) == PanelPlacement::Top,
          "panel layout: **an appended section arrives at its default placement**, not swept "
          "into whichever dock the file happened to mention");
    check(missing.sectionsIn(PanelPlacement::Right).front() == ControlsSection::Layers,
          "panel layout: and the section the file DID name keeps its position");

    // Malformed lines: skipped, not fatal. A file that is mostly this format
    // keeps the lines that were readable.
    PanelLayout malformed;
    malformed.parse(
        "naturalPaint-panel-layout 2\n"
        "panel layers bottom\n"                   // too few tokens
        "panel history top 1.000 0 extra\n"       // too many
        "panel comps top 1.000 2\n"               // collapsed is not 0/1
        "panel grade top notanumber 0\n"          // weight is not a number
        "panel histogram top 0.000 0\n"           // weight is not positive
        "panel solver bottom 1.000 1\n");         // the one good line
    check(exactlyOnceEach(malformed),
          "panel layout: a file of mostly-malformed lines still yields a complete layout");
    check(malformed.placementOf(ControlsSection::Solver) == PanelPlacement::Bottom &&
              malformed.isCollapsed(ControlsSection::Solver),
          "panel layout: **a malformed line is SKIPPED, not fatal** -- the readable lines around "
          "it still apply");
    check(malformed.placementOf(ControlsSection::Grade) == PanelPlacement::Right &&
              malformed.placementOf(ControlsSection::Histogram) == PanelPlacement::Right,
          "panel layout: and each skipped line's section arrives via the append rule instead");
    bool everyWeightSane = true;
    for (const PanelEntry& e : malformed.entries())
      if (!std::isfinite(e.weight) || e.weight <= 0.0f) everyWeightSane = false;
    check(everyWeightSane,
          "panel layout: **no zero, negative or NaN weight ever reaches the model** -- which is "
          "what keeps ui/DockLayout from producing a NaN rect");

    // A genuinely foreign file has no salvageable lines, so every section is
    // "missing" and the same rule rebuilds the default -- not by a special
    // case, by the identical path.
    PanelLayout foreign;
    foreign.parse("<?xml version=\"1.0\"?>\n<plist><dict><key>nope</key></dict></plist>\n");
    PanelLayout fresh;
    check(exactlyOnceEach(foreign) && foreign.entries().size() == fresh.entries().size(),
          "panel layout: a foreign file resolves to the default through the append rule");
    bool sameAsFresh = true;
    for (size_t i = 0; i < foreign.entries().size(); ++i)
      if (foreign.entries()[i].section != fresh.entries()[i].section ||
          foreign.entries()[i].placement != fresh.entries()[i].placement)
        sameAsFresh = false;
    check(sameAsFresh, "panel layout: and it really is the default, section for section");

    // An empty file: the same path again.
    PanelLayout empty;
    empty.parse(std::string());
    check(exactlyOnceEach(empty) &&
              empty.placementOf(ControlsSection::Tools) == PanelPlacement::Left,
          "panel layout: an empty file resolves to the default, not to an empty window");
  }

  // ==========================================================================
  // 7. Dock extents: the floor, the zero, and the empty-dock rule
  // ==========================================================================
  {
    PanelLayout layout;
    layout.setDockExtent(PanelPlacement::Right, 10.0f);
    check(layout.dockExtents().right == kDockMinWidth,
          "panel layout: a dock dragged narrower than its floor stops at the floor");
    layout.setDockExtent(PanelPlacement::Top, 5.0f);
    check(layout.dockExtents().top == kDockMinHeight,
          "panel layout: a horizontal dock's floor is a height, not a width");
    layout.setDockExtent(PanelPlacement::Right, 0.0f);
    check(layout.dockExtents().right == 0.0f,
          "panel layout: **zero passes the floor unclamped** -- it is how a dock is switched "
          "off, and clamping it up would make an empty dock impossible");
    layout.setDockExtent(PanelPlacement::Right, -5.0f);
    check(layout.dockExtents().right == 0.0f,
          "panel layout: a negative extent is refused rather than stored");
    layout.setDockExtent(PanelPlacement::Flyout, 300.0f);
    check(layout.dockExtents().left == kDefaultDockExtents.left,
          "panel layout: setDockExtent() on a non-dock placement changes nothing");

    // The empty-dock rule, stated as arithmetic rather than left to the draw
    // code: a dock holding nothing is not on screen, whatever it stores.
    PanelLayout e2;
    e2.setPlacement(ControlsSection::Tools, PanelPlacement::Right);
    check(e2.sectionsIn(PanelPlacement::Left).empty() &&
              e2.effectiveDockExtents().left == 0.0f && e2.dockExtents().left > 0.0f,
          "panel layout: **an emptied dock reports zero extent while remembering its own** -- "
          "so moving its last panel out costs no pixels and moving one back costs no setup");
  }

  // ==========================================================================
  // 8. A real save/load round trip, and the durability shape
  // ==========================================================================
  {
    const std::string saved = std::getenv("NP_PANEL_LAYOUT") ? std::getenv("NP_PANEL_LAYOUT") : "";
    const fs::path root = fs::temp_directory_path() / "np-selftest-panel-layout";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::string path = (root / "panel-layout.txt").string();
    setenv("NP_PANEL_LAYOUT", path.c_str(), 1);
    check(defaultPanelLayoutFilePath() == path,
          "panel layout: $NP_PANEL_LAYOUT redirects the settings path, so this suite never "
          "touches the developer's real one");

    PanelLayout out;
    out.setPlacement(ControlsSection::Layers, PanelPlacement::Bottom);
    out.setPlacement(ControlsSection::History, PanelPlacement::Flyout);
    out.setPlacement(ControlsSection::Solver, PanelPlacement::Hidden);
    out.setWeight(ControlsSection::Color, 2.75f);
    out.setCollapsed(ControlsSection::Comps, true);
    out.setDockExtent(PanelPlacement::Right, 400.0f);
    out.setDockExtent(PanelPlacement::Bottom, 160.0f);
    {
      std::string err;
      check(out.saveToFile(path, &err) && err.empty(),
            "panel layout: a save reports success and no error");
    }
    const std::string bytes = readWhole(path);
    check(contains(bytes.c_str(), "naturalPaint-panel-layout 2"),
          "panel layout: the file is written in the version 2 grammar");
    check(contains(bytes.c_str(), "panel layers bottom") &&
              contains(bytes.c_str(), "panel history flyout") &&
              contains(bytes.c_str(), "dock bottom 160.000"),
          "panel layout: and the bytes really say what the layout says");
    check(!fs::exists(path + ".tmp", ec),
          "panel layout: the save left no .tmp file beside the real one");

    PanelLayout back;
    {
      std::string err;
      check(back.loadFromFile(path, &err) && err.empty(),
            "panel layout: the file loads without error");
    }
    check(exactlyOnceEach(back), "panel layout: a disk round trip preserves the invariant");
    bool identical = back.entries().size() == out.entries().size();
    for (size_t i = 0; identical && i < back.entries().size(); ++i) {
      const PanelEntry& a = out.entries()[i];
      const PanelEntry& b = back.entries()[i];
      if (a.section != b.section || a.placement != b.placement || a.collapsed != b.collapsed)
        identical = false;
      if (std::fabs(a.weight - b.weight) > 1e-3f) identical = false;
    }
    check(identical,
          "panel layout: **every panel comes back with the same placement, order, weight and "
          "collapse state** it went out with");
    const PanelDockExtents d = back.dockExtents();
    check(d.right == 400.0f && d.bottom == 160.0f,
          "panel layout: and every dock extent comes back too");
    check(back.serialize() == out.serialize(),
          "panel layout: serialize() is stable across a round trip, byte for byte");

    // The durability shape app/selftest/UserBrushLibrary.cpp proves for its own
    // file: a stale `.tmp` is consumed by the next save, and an abandoned one
    // cannot corrupt the real file.
    {
      std::ofstream stale(path + ".tmp", std::ios::binary | std::ios::trunc);
      stale << "garbage left by a previous crash";
    }
    {
      std::string err;
      check(out.saveToFile(path, &err) && err.empty() && !fs::exists(path + ".tmp", ec),
            "panel layout: durability -- **the save consumed the stale .tmp**, not merely "
            "avoided leaving a new one");
    }
    const std::string goodBytes = readWhole(path);

    // Simulate a crash: a `.tmp` opened, partially written, never renamed.
    // Never call saveToFile() again in this block.
    {
      std::ofstream tmp(path + ".tmp", std::ios::binary | std::ios::trunc);
      tmp << "naturalPaint-panel-layout 2\npanel col";  // cut off mid-line, no rename
    }
    check(fs::exists(path + ".tmp", ec), "panel layout: durability -- the abandoned .tmp exists");
    check(readWhole(path) == goodBytes,
          "panel layout: durability -- **the real file is byte-for-byte unchanged** by an "
          "abandoned .tmp sitting right beside it");

    PanelLayout afterCrash;
    {
      std::string err;
      check(afterCrash.loadFromFile(path, &err) && err.empty(),
            "panel layout: durability -- loading after the 'crash' reads the last GOOD save");
    }
    check(afterCrash.placementOf(ControlsSection::Layers) == PanelPlacement::Bottom &&
              afterCrash.dockExtents().bottom == 160.0f,
          "panel layout: durability -- and it is really the known-good layout, not anything "
          "from the abandoned .tmp");

    fs::remove(path + ".tmp", ec);

    // Restore the environment and clean up -- nothing here may leave a trace
    // in the developer's real settings directory.
    if (saved.empty()) unsetenv("NP_PANEL_LAYOUT");
    else setenv("NP_PANEL_LAYOUT", saved.c_str(), 1);
    fs::remove_all(root, ec);
    check(!fs::exists(root, ec), "panel layout: every file this section wrote is removed");
  }

  std::printf("[selftest] panel layout %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
