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

    // **Placement follows the role**, so a section added to
    // `controlsSections()` lands somewhere defensible with no second edit.
    // Asserted against the roles rather than against a copy of the list, or
    // the test would only be checking that two hand-written tables match.
    bool placementFollowsRole = true;
    for (const PanelEntry& e : layout.entries()) {
      if (e.section == ControlsSection::Tools || e.section == ControlsSection::Options) continue;
      const ControlsSectionRole role = controlsSectionSpec(e.section).role;
      const PanelPlacement want =
          (role == ControlsSectionRole::View || role == ControlsSectionRole::Simulation)
              ? PanelPlacement::Flyout
              : PanelPlacement::Right;
      if (e.placement != want) placementFollowsRole = false;
    }
    check(placementFollowsRole,
          "panel layout: **a Tool or Document panel starts in the right dock and a View or "
          "Simulation panel starts on the flyout rail** -- the occasional roles do not spend a "
          "grip apiece of a dock that does not scroll");
    check(layout.sectionsIn(PanelPlacement::Flyout).size() == 7,
          "panel layout: which is seven panels on the rail -- and the rail is not empty on a "
          "first run, which is the mode the revamp was asked for by name");

    // Whatever is in the right dock is in `controlsSections()`'s own order --
    // i.e. the outgoing column's order with the flyout sections lifted out.
    const std::vector<ControlsSection> right = layout.sectionsIn(PanelPlacement::Right);
    std::vector<ControlsSection> expected;
    for (const ControlsSectionSpec& spec : controlsSections())
      if (layout.placementOf(spec.section) == PanelPlacement::Right)
        expected.push_back(spec.section);
    check(right == expected,
          "panel layout: **the right dock's default order is the outgoing column's order** -- "
          "the revamp changed what the layout can express, not how the column reads");

    // LAYERS is the one panel that does not start at the default weight, and
    // the reason is in `defaultEntryFor()`: every other panel here is a
    // fixed-height form, and a layer panel is a list.
    bool othersUnitWeight = true;
    for (const PanelEntry& e : layout.entries())
      if (e.section != ControlsSection::Layers && e.weight != kPanelDefaultWeight)
        othersUnitWeight = false;
    check(othersUnitWeight, "panel layout: every panel but LAYERS starts at the default weight");
    check(layout.weightOf(ControlsSection::Layers) > kPanelDefaultWeight,
          "panel layout: **LAYERS starts heavier than its neighbours** -- it is the only panel "
          "in the dock whose content is a list, and a list at a form's height shows nothing");

    size_t expanded = 0;
    for (const ControlsSectionSpec& spec : controlsSections())
      if (!layout.isCollapsed(spec.section)) ++expanded;
    check(!layout.isCollapsed(ControlsSection::Tools) &&
              !layout.isCollapsed(ControlsSection::Options) &&
              !layout.isCollapsed(ControlsSection::Color) &&
              !layout.isCollapsed(ControlsSection::Layers),
          "panel layout: TOOLS, OPTIONS, COLOR and LAYERS start expanded");
    check(expanded == 4,
          "panel layout: and nothing else does -- the other four in the right dock start as a "
          "titled grip, because in a dock the default-open set is a budget and not a preference");

    // ------------------------------------------------------------------
    // And the consequence, as arithmetic against the real chrome rather than
    // as intent.
    //
    // **This replaces a `!overflowed` assertion that was far too weak to
    // catch the bug it was written for.** A dock in which every panel has
    // been squeezed to exactly `kPanelMinHeight` does not overflow either,
    // and that is precisely the state the previous default reached: four
    // expanded panels sharing 330 px gave LAYERS a 57 px body, which is the
    // document line and the filter field and then the bottom of the dock.
    // Zero layer rows, by default, on a first run. So the assertion is now
    // about how much room the panels actually get.
    // Both numbers measured off the golden harness's own `layers` capture
    // rather than guessed, because a guess here is a test that agrees with
    // itself: the first version of this assertion put the panel's chrome at
    // 46 px, forgot the BLEND/OPACITY row entirely, and passed green while the
    // real panel showed one and a half rows.
    const float kRowH = 40.0f;     // one LAYERS row: two lines of text plus its padding
    const float kChromeH = 112.0f;  // document line + filter field + BLEND/OPACITY + list frame
    const AtelierBands bands = atelierLayout(0.0f, 0.0f, 1280.0f, 790.0f, true);
    std::vector<DockSlotSpec> specs;
    size_t layersIndex = 0;
    for (const ControlsSection sec : layout.sectionsIn(PanelPlacement::Right)) {
      if (sec == ControlsSection::Layers) layersIndex = specs.size();
      DockSlotSpec sp;
      sp.collapsed = layout.isCollapsed(sec);
      sp.weight = layout.weightOf(sec);
      sp.minExtent = kPanelMinHeight;
      sp.headerExtent = kPanelHeaderExtent;
      specs.push_back(sp);
    }
    const DockTiling t = dockTile(bands.rightDock, DockSide::Right, specs);
    check(!t.overflowed,
          "panel layout: the default right dock does not overflow the reference window");
    const float layersBody =
        t.slots.empty() ? 0.0f : t.slots[layersIndex].rect.h - kPanelHeaderExtent;
    check(layersBody >= kChromeH + 3.0f * kRowH,
          "panel layout: **the default LAYERS panel has room for three layer rows** at the "
          "reference window size -- the property the outgoing `!overflowed` check did not "
          "constrain, and whose absence made a first run's layer panel show no layers");

    bool nonePinned = true;
    for (const DockSlot& s : t.slots)
      if (!s.collapsed && s.atMinimum) nonePinned = false;
    check(nonePinned,
          "panel layout: and no expanded panel is pinned at its floor -- a default that lands "
          "every panel on `minExtent` is one the window is too small for, not one that fits");

    const PanelDockExtents d = layout.dockExtents();
    check(d.left == kDefaultDockExtents.left && d.right == kDefaultDockExtents.right &&
              d.top == kDefaultDockExtents.top && d.bottom == kDefaultDockExtents.bottom,
          "panel layout: the default dock extents are ui/AtelierLayout's kDefaultDockExtents");
    check(layout.sectionsIn(PanelPlacement::Bottom).empty() &&
              layout.sectionsIn(PanelPlacement::Hidden).empty(),
          "panel layout: nothing starts on the bottom or hidden -- the bottom dock is empty "
          "space the user may claim, and nothing is out of reach on a first run");
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
    // Compared against a fresh default rather than against a literal, so this
    // assertion keeps testing the append RULE if the defaults ever move again
    // -- which they have once already, when GRADE and HISTOGRAM went from the
    // right dock to the flyout rail.
    PanelLayout defaults;
    check(malformed.placementOf(ControlsSection::Grade) ==
                  defaults.placementOf(ControlsSection::Grade) &&
              malformed.placementOf(ControlsSection::Histogram) ==
                  defaults.placementOf(ControlsSection::Histogram),
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
    check(contains(bytes.c_str(), "naturalPaint-panel-layout 3"),
          "panel layout: the file is written in the version 3 grammar");
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

  // ==========================================================================
  // 9. Tab stacks
  // ==========================================================================
  //
  // The user's instruction: *"tab support for putting multiple panels into a
  // stack."* Everything below is about the two invariants that make a stack
  // safe to draw -- at least two members, exactly one of them visible -- and
  // about the states a drag can reach that would otherwise break them.
  {
    PanelLayout l;
    check(l.stackOf(ControlsSection::Color) == 0 && l.stackOf(ControlsSection::Layers) == 0,
          "panel stack: nothing starts stacked");
    check(l.slotsIn(PanelPlacement::Right).size() ==
              l.sectionsIn(PanelPlacement::Right).size(),
          "panel stack: with nothing stacked a dock has exactly one slot per panel");

    // --- forming one ------------------------------------------------------
    l.stackWith(ControlsSection::History, ControlsSection::Color);
    check(l.stackOf(ControlsSection::Color) != 0 &&
              l.stackOf(ControlsSection::History) == l.stackOf(ControlsSection::Color),
          "panel stack: stacking gives both panels the same id");
    check(l.slotsIn(PanelPlacement::Right).size() ==
              l.sectionsIn(PanelPlacement::Right).size() - 1,
          "panel stack: **and the dock now has one FEWER slot than it has panels** -- which is "
          "the whole point: a stack asks the dock for one slot, not one per tab");

    const PanelSlot s = l.slotOf(ControlsSection::Color);
    check(s.stacked() && s.members.size() == 2 && s.members[0] == ControlsSection::Color &&
              s.members[1] == ControlsSection::History,
          "panel stack: the target keeps its position and the mover is appended after it");
    check(s.activeSection() == ControlsSection::History,
          "panel stack: **the panel you just moved is the one you can see** -- a tab a person "
          "asked for and cannot find is the same failure as a panel that disappears");
    check(s.leader() == ControlsSection::Color,
          "panel stack: and the LEADER is still the target, so the slot keeps its size");

    // --- switching tabs ---------------------------------------------------
    l.setActiveInStack(ControlsSection::Color);
    const PanelSlot s2 = l.slotOf(ControlsSection::Color);
    check(s2.activeSection() == ControlsSection::Color && s2.members[0] == ControlsSection::Color &&
              s2.members[1] == ControlsSection::History,
          "panel stack: **switching tabs does not reorder them** -- a tab strip that reshuffles "
          "itself when clicked is one nobody can aim at twice");

    // The slot's geometry is the leader's, so switching tabs cannot resize it.
    l.setWeight(ControlsSection::Color, 3.0f);
    l.setWeight(ControlsSection::History, 9.0f);
    l.setActiveInStack(ControlsSection::History);
    check(l.weightOf(l.slotOf(ControlsSection::Color).leader()) == 3.0f,
          "panel stack: the slot's weight is its LEADER's, so bringing another tab to the front "
          "cannot change the slot's size");

    // --- taking one out ---------------------------------------------------
    l.unstack(ControlsSection::History);
    check(l.stackOf(ControlsSection::History) == 0 && l.stackOf(ControlsSection::Color) == 0,
          "panel stack: **unstacking one member clears the OTHER's id too** -- a stack of one "
          "is a panel, and a lone tab strip is chrome with nothing to select");
    {
      const std::vector<ControlsSection> right = l.sectionsIn(PanelPlacement::Right);
      size_t ci = right.size(), hi = right.size();
      for (size_t i = 0; i < right.size(); ++i) {
        if (right[i] == ControlsSection::Color) ci = i;
        if (right[i] == ControlsSection::History) hi = i;
      }
      check(ci < right.size() && hi == ci + 1,
            "panel stack: and the panel that left sits right where the stack was, not at the "
            "bottom of the dock");
    }

    // --- three-way, and leaving by another route --------------------------
    PanelLayout l3;
    l3.stackWith(ControlsSection::History, ControlsSection::Color);
    l3.stackWith(ControlsSection::Comps, ControlsSection::Color);
    check(l3.slotOf(ControlsSection::Color).members.size() == 3,
          "panel stack: a third panel joins the same stack rather than starting a new one");
    l3.setPlacement(ControlsSection::Comps, PanelPlacement::Flyout);
    check(l3.stackOf(ControlsSection::Comps) == 0 &&
              l3.slotOf(ControlsSection::Color).members.size() == 2,
          "panel stack: **moving a panel to another placement takes it out of its stack** -- it "
          "is not sharing that slot any more");
    l3.setPlacement(ControlsSection::History, PanelPlacement::Bottom);
    check(l3.stackOf(ControlsSection::Color) == 0,
          "panel stack: and the two-member stack it left behind collapses to a lone panel");

    // --- what stacking refuses -------------------------------------------
    PanelLayout lr;
    lr.stackWith(ControlsSection::Color, ControlsSection::Color);
    check(lr.stackOf(ControlsSection::Color) == 0,
          "panel stack: a panel cannot be stacked with itself");
    lr.stackWith(ControlsSection::Color, ControlsSection::Grade);  // GRADE starts on the rail
    check(lr.stackOf(ControlsSection::Color) == 0 &&
              lr.placementOf(ControlsSection::Color) == PanelPlacement::Right,
          "panel stack: **tabs exist only where a slot does** -- a flyout draws one panel and a "
          "hidden panel draws none, so stacking onto either is refused rather than half-done");

    // --- the mover follows the target into ITS dock -----------------------
    PanelLayout lm;
    lm.setPlacement(ControlsSection::Comps, PanelPlacement::Bottom);
    lm.stackWith(ControlsSection::Color, ControlsSection::Comps);
    check(lm.placementOf(ControlsSection::Color) == PanelPlacement::Bottom &&
              lm.stackOf(ControlsSection::Color) == lm.stackOf(ControlsSection::Comps) &&
              lm.stackOf(ControlsSection::Comps) != 0,
          "panel stack: stacking onto a panel in another dock moves the mover to that dock");

    // --- ids are scoped to a placement ------------------------------------
    PanelLayout ls;
    ls.stackWith(ControlsSection::History, ControlsSection::Color);
    ls.setPlacement(ControlsSection::Comps, PanelPlacement::Bottom);
    ls.setPlacement(ControlsSection::Layers, PanelPlacement::Bottom);
    ls.stackWith(ControlsSection::Layers, ControlsSection::Comps);
    check(ls.slotOf(ControlsSection::Color).members.size() == 2 &&
              ls.slotOf(ControlsSection::Comps).members.size() == 2,
          "panel stack: two docks each hold their own stack, even when the ids collide -- a "
          "stack id is scoped to its placement");

    // --- the file ---------------------------------------------------------
    PanelLayout w;
    w.stackWith(ControlsSection::History, ControlsSection::Color);
    w.setActiveInStack(ControlsSection::Color);
    PanelLayout r;
    r.parse(w.serialize());
    check(r.slotOf(ControlsSection::Color).members == w.slotOf(ControlsSection::Color).members &&
              r.slotOf(ControlsSection::Color).activeIndex ==
                  w.slotOf(ControlsSection::Color).activeIndex,
          "panel stack: a stack survives a serialize/parse round trip, members and active tab "
          "both");
  }

  // ==========================================================================
  // 10. The stack repair rules, each from a file
  // ==========================================================================
  {
    // A stack id used once is not a stack.
    PanelLayout lone;
    lone.parse(
        "naturalPaint-panel-layout 3\n"
        "panel color right 1.000 0 7 1\n");
    check(exactlyOnceEach(lone) && lone.stackOf(ControlsSection::Color) == 0,
          "panel stack: **a stack id shared by fewer than two panels is cleared** -- a file "
          "cannot produce a tab strip with one tab in it");

    // A stack scattered across two docks is not a stack either.
    PanelLayout split;
    split.parse(
        "naturalPaint-panel-layout 3\n"
        "panel color right 1.000 0 4 1\n"
        "panel layers bottom 1.000 0 4 1\n");
    check(split.stackOf(ControlsSection::Color) == 0 && split.stackOf(ControlsSection::Layers) == 0,
          "panel stack: and neither is one whose members a hand-edit has put in different docks "
          "-- a stack is panels sharing ONE slot");

    // No active member: the first becomes active.
    PanelLayout dark;
    dark.parse(
        "naturalPaint-panel-layout 3\n"
        "panel color right 1.000 0 2 0\n"
        "panel layers right 1.000 0 2 0\n");
    check(dark.slotOf(ControlsSection::Color).members.size() == 2 &&
              dark.slotOf(ControlsSection::Color).activeSection() == ControlsSection::Color,
          "panel stack: **a stack with no visible tab gets one** -- the first member -- rather "
          "than drawing a slot whose body is nothing at all");

    // Two active members: the first wins, like every other duplicate here.
    PanelLayout twice;
    twice.parse(
        "naturalPaint-panel-layout 3\n"
        "panel color right 1.000 0 2 1\n"
        "panel layers right 1.000 0 2 1\n");
    check(twice.slotOf(ControlsSection::Color).activeSection() == ControlsSection::Color,
          "panel stack: a stack with two visible tabs keeps the first, the same first-wins rule "
          "a duplicated section line gets");

    // --- version 2 files, and the shapes that are not files at all --------
    PanelLayout v2;
    v2.parse(
        "naturalPaint-panel-layout 2\n"
        "panel layers right 2.000 0\n"
        "panel color right 1.000 1\n");
    check(exactlyOnceEach(v2) && v2.stackOf(ControlsSection::Layers) == 0 &&
              v2.weightOf(ControlsSection::Layers) == 2.0f &&
              v2.isCollapsed(ControlsSection::Color) &&
              v2.sectionsIn(PanelPlacement::Right).front() == ControlsSection::Layers,
          "panel stack: **a version 2 line reads unstacked and active**, which is exactly what "
          "it meant -- recognised by its field count, not by the header's version number");

    PanelLayout ragged;
    ragged.parse(
        "naturalPaint-panel-layout 3\n"
        "panel layers right 2.000 0 1\n"          // six fields: half a pair
        "panel color right 1.000 0 1 1 9\n"       // eight fields
        "panel history right 1.000 0 -3 1\n"      // negative stack id
        "panel comps right 1.000 0 notanum 1\n"   // stack id is not a number
        "panel grade right 1.000 0 2 7\n"         // active is not 0/1
        "panel solver bottom 1.000 1 0 1\n");     // the one good line
    check(exactlyOnceEach(ragged) &&
              ragged.placementOf(ControlsSection::Solver) == PanelPlacement::Bottom &&
              ragged.isCollapsed(ControlsSection::Solver),
          "panel stack: every malformed shape of the new fields is SKIPPED, not fatal -- the "
          "readable line around them still applies");
    PanelLayout freshDefaults;
    check(ragged.placementOf(ControlsSection::Layers) ==
                  freshDefaults.placementOf(ControlsSection::Layers) &&
              ragged.placementOf(ControlsSection::Color) ==
                  freshDefaults.placementOf(ControlsSection::Color),
          "panel stack: and each skipped section arrives via the append rule at its default");
  }

  std::printf("[selftest] panel layout %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
