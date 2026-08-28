#include "app/selftest/Support.hpp"

#include "ui/AtelierLayout.hpp"
#include "ui/DockLayout.hpp"

namespace np {
namespace {

// The extent of a rect along the axis `side`'s slots divide, and the offset of
// its leading edge on that axis. Written once here so every tiling assertion
// below reads the same on all four sides, which is the point of the whole
// exercise: the four sides are one body of arithmetic, so they deserve one
// body of assertions rather than four that can drift.
float majorExtent(const AtelierRect& r, DockSide side) {
  return dockStacksVertically(side) ? r.h : r.w;
}
float majorStart(const AtelierRect& r, DockSide side) {
  return dockStacksVertically(side) ? r.y : r.x;
}
float minorExtent(const AtelierRect& r, DockSide side) {
  return dockStacksVertically(side) ? r.w : r.h;
}

// Do the slots and the splitters tile `dock` exactly -- contiguous, in order,
// no overlap, nothing left over, and every one spanning the minor axis in
// full?
//
// An exact equality on floats, deliberately, and it is only defensible because
// `dockTile()` gives the last expanded slot the REMAINDER rather than its own
// computed share. A tolerance here would pass on an implementation that leaks
// a fraction of a pixel per slot, which at thirteen panels is a visible seam.
bool tilesExactly(const AtelierRect& dock, DockSide side, const DockTiling& t) {
  if (t.slots.empty()) return false;
  if (t.splitters.size() != t.slots.size() - 1) return false;

  float cursor = majorStart(dock, side);
  for (size_t i = 0; i < t.slots.size(); ++i) {
    if (majorStart(t.slots[i].rect, side) != cursor) return false;
    if (minorExtent(t.slots[i].rect, side) != minorExtent(dock, side)) return false;
    cursor += majorExtent(t.slots[i].rect, side);
    if (i + 1 < t.slots.size()) {
      if (majorStart(t.splitters[i], side) != cursor) return false;
      if (majorExtent(t.splitters[i], side) != kDockSplitterThickness) return false;
      cursor += kDockSplitterThickness;
    }
  }
  // The whole point: the cursor lands exactly on the dock's far edge.
  return cursor == majorStart(dock, side) + majorExtent(dock, side);
}

std::vector<DockSlotSpec> uniformSpecs(size_t n, float minExtent) {
  std::vector<DockSlotSpec> v;
  for (size_t i = 0; i < n; ++i) {
    DockSlotSpec s;
    s.minExtent = minExtent;
    v.push_back(s);
  }
  return v;
}

// The four sides, and the rect + floor each one is exercised with. A left or
// right dock is a tall column; a top or bottom dock is a wide strip.
struct SideCase {
  DockSide side;
  const char* name;
  AtelierRect dock;
  float minExtent;
};

}  // namespace

// ui/DockLayout -- how the panels inside ONE dock divide it up, and
// ui/AtelierLayout's dock-aware bands and flowing tool grid.
//
// The geometric half of the dockable-panel feature. Headless for the reason
// ui/AtelierLayout's own suite is headless, restated in that file's header: "a
// test that needs a window, a device and a font atlas to check that four bands
// tile a rectangle is a test nobody runs."
//
// The properties this proves are listed in app/SelfTest.hpp. The one worth
// naming here is section 4's: a single weighted pass that clamps each share
// upwards to its floor produces extents that sum to MORE than the dock, and
// the slots silently overrun it. That was this file's first implementation.
// Section 4 drives exactly the case that exposes it and asserts both halves at
// once -- every slot at or above its floor, AND the sum still exact -- because
// either one alone is satisfiable by a wrong implementation.
bool runDockLayoutTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const SideCase kSides[] = {
      {DockSide::Left, "left", AtelierRect{0.0f, 100.0f, 52.0f, 900.0f}, kPanelMinHeight},
      {DockSide::Right, "right", AtelierRect{1278.0f, 100.0f, 322.0f, 900.0f}, kPanelMinHeight},
      {DockSide::Top, "top", AtelierRect{0.0f, 36.0f, 1600.0f, 46.0f}, kPanelMinWidth},
      {DockSide::Bottom, "bottom", AtelierRect{0.0f, 940.0f, 1600.0f, 120.0f}, kPanelMinWidth},
  };

  // ==========================================================================
  // 1. Exact tiling, on all four sides, at a table of slot counts
  // ==========================================================================
  {
    bool allTile = true;
    bool allSpanMinor = true;
    size_t cases = 0;
    for (const SideCase& sc : kSides) {
      for (size_t n = 1; n <= 6; ++n) {
        const DockTiling t = dockTile(sc.dock, sc.side, uniformSpecs(n, sc.minExtent));
        ++cases;
        if (t.slots.size() != n) allTile = false;
        // Only assert exact tiling where the dock can actually hold the
        // minima. The overflow case has its own section below and is
        // *supposed* to run past the edge.
        if (!t.overflowed && !tilesExactly(sc.dock, sc.side, t)) allTile = false;
        for (const DockSlot& s : t.slots)
          if (minorExtent(s.rect, sc.side) != minorExtent(sc.dock, sc.side)) allSpanMinor = false;
      }
    }
    check(cases == 24, "dock layout: the tiling table covers 4 sides x 6 slot counts");
    check(allTile,
          "dock layout: **slots + splitters tile the dock exactly** on every side and count");
    check(allSpanMinor, "dock layout: every slot spans the dock's minor axis in full");

    // The degenerate inputs, which must not produce a degenerate tiling.
    const DockTiling none = dockTile(kSides[1].dock, DockSide::Right, {});
    check(none.slots.empty() && none.splitters.empty(),
          "dock layout: a dock with no panels yields no slots rather than one empty one");
    const DockTiling emptyDock =
        dockTile(AtelierRect{0.0f, 0.0f, 0.0f, 0.0f}, DockSide::Right, uniformSpecs(3, 72.0f));
    check(emptyDock.slots.empty(),
          "dock layout: an empty dock rect yields no slots rather than three zero-height ones");
  }

  // ==========================================================================
  // 2. The weights are ratios
  // ==========================================================================
  {
    const AtelierRect dock{0.0f, 0.0f, 322.0f, 900.0f};
    std::vector<DockSlotSpec> a = uniformSpecs(3, kPanelMinHeight);
    a[0].weight = 1.0f;
    a[1].weight = 2.0f;
    a[2].weight = 1.0f;
    const DockTiling ta = dockTile(dock, DockSide::Right, a);

    // The middle slot has twice the weight, so twice the height (to within the
    // one pixel the floor()-then-remainder rule can move).
    const float h0 = ta.slots[0].rect.h, h1 = ta.slots[1].rect.h, h2 = ta.slots[2].rect.h;
    check(std::fabs(h1 - 2.0f * h0) <= 1.0f,
          "dock layout: a panel with twice the weight gets twice the height");
    check(std::fabs(h0 - h2) <= 1.0f,
          "dock layout: two panels with equal weight get equal height");

    // Scaling EVERY weight by the same factor is a no-op -- which is what
    // makes a splitter drag able to rewrite two weights without renormalising
    // the rest.
    std::vector<DockSlotSpec> scaled = a;
    for (DockSlotSpec& s : scaled) s.weight *= 7.5f;
    const DockTiling ts = dockTile(dock, DockSide::Right, scaled);
    bool identical = ts.slots.size() == ta.slots.size();
    for (size_t i = 0; identical && i < ts.slots.size(); ++i)
      if (ts.slots[i].rect.h != ta.slots[i].rect.h) identical = false;
    check(identical,
          "dock layout: **scaling every weight by the same factor changes nothing** -- "
          "weights are ratios, not pixels");

    // A zero or negative weight is repaired to the default rather than
    // dividing by zero or vanishing.
    std::vector<DockSlotSpec> bad = uniformSpecs(2, kPanelMinHeight);
    bad[0].weight = 0.0f;
    bad[1].weight = -3.0f;
    const DockTiling tb = dockTile(dock, DockSide::Right, bad);
    check(tb.slots.size() == 2 && tb.slots[0].rect.h > 0.0f && tb.slots[1].rect.h > 0.0f &&
              tilesExactly(dock, DockSide::Right, tb),
          "dock layout: a zero or negative weight is treated as the default, not as zero height");
  }

  // ==========================================================================
  // 3. A collapsed panel costs a header, and hands its space to its neighbours
  // ==========================================================================
  {
    const AtelierRect dock{0.0f, 0.0f, 322.0f, 900.0f};
    std::vector<DockSlotSpec> open = uniformSpecs(3, kPanelMinHeight);
    const DockTiling before = dockTile(dock, DockSide::Right, open);

    std::vector<DockSlotSpec> withCollapse = open;
    withCollapse[1].collapsed = true;
    const DockTiling after = dockTile(dock, DockSide::Right, withCollapse);

    check(after.slots[1].rect.h == kPanelHeaderExtent,
          "dock layout: a collapsed panel occupies exactly its header and nothing more");
    check(after.slots[1].collapsed && !after.slots[0].collapsed,
          "dock layout: the tiling reports which slots are collapsed");
    check(after.slots[0].rect.h > before.slots[0].rect.h &&
              after.slots[2].rect.h > before.slots[2].rect.h,
          "dock layout: **collapsing a panel is a size control** -- its space goes to its "
          "expanded neighbours, measurably");
    check(tilesExactly(dock, DockSide::Right, after),
          "dock layout: a dock containing a collapsed panel still tiles exactly");

    // Every panel collapsed: the dock is mostly empty and still tiles, with
    // the last slot absorbing the slack rather than a gap opening up.
    std::vector<DockSlotSpec> allCollapsed = open;
    for (DockSlotSpec& s : allCollapsed) s.collapsed = true;
    const DockTiling tc = dockTile(dock, DockSide::Right, allCollapsed);
    bool everyOneIsAHeader = true;
    for (const DockSlot& s : tc.slots)
      if (s.rect.h != kPanelHeaderExtent) everyOneIsAHeader = false;
    check(everyOneIsAHeader,
          "dock layout: with every panel collapsed each is a header and none is stretched");
  }

  // ==========================================================================
  // 4. The floor holds AND the sum stays exact -- the water-filling property
  // ==========================================================================
  //
  // The case a single weighted pass gets wrong. Three panels in a 400 px dock
  // with a 72 px floor: the minima total 216 and fit easily, but one greedy
  // weight drives a sibling's weighted share below its floor. A single pass
  // clamps that sibling up and leaves the total OVER 400 -- slots overrunning
  // the dock, in the branch `overflowed` reports as fine.
  {
    const AtelierRect dock{0.0f, 0.0f, 322.0f, 400.0f};
    std::vector<DockSlotSpec> greedy = uniformSpecs(3, kPanelMinHeight);
    greedy[0].weight = 40.0f;  // wants ~371 of the ~382 shareable
    greedy[1].weight = 1.0f;   // would get ~9, well under the 72 floor
    greedy[2].weight = 1.0f;
    const DockTiling t = dockTile(dock, DockSide::Right, greedy);

    check(!t.overflowed,
          "dock layout: 3 x 72 px of minima fit a 400 px dock, so this is NOT the overflow case");
    bool everyOneAtOrAboveFloor = true;
    for (const DockSlot& s : t.slots)
      if (s.rect.h < kPanelMinHeight) everyOneAtOrAboveFloor = false;
    check(everyOneAtOrAboveFloor,
          "dock layout: a starved panel is raised to its floor rather than left a sliver");
    check(tilesExactly(dock, DockSide::Right, t),
          "dock layout: **and the dock still tiles exactly** -- raising a panel to its floor "
          "takes the space from its siblings, not from past the dock's edge");
    check(t.slots[1].atMinimum && t.slots[2].atMinimum && !t.slots[0].atMinimum,
          "dock layout: the tiling reports exactly which slots were held at their floor");
    check(t.slots[0].rect.h > t.slots[1].rect.h,
          "dock layout: the greedy panel still gets the lion's share of what remains");
  }

  // ==========================================================================
  // 5. The honest limit: disclosed, not hidden
  // ==========================================================================
  {
    // Six panels with a 72 px floor cannot fit a 200 px dock by any
    // distribution of weight.
    const AtelierRect dock{0.0f, 0.0f, 322.0f, 200.0f};
    const DockTiling t = dockTile(dock, DockSide::Right, uniformSpecs(6, kPanelMinHeight));
    check(t.overflowed, "dock layout: minima that cannot fit the dock set `overflowed`");
    check(t.slots.size() == 6,
          "dock layout: **no panel is silently dropped** when the dock is too small");
    bool allAtFloor = true;
    for (const DockSlot& s : t.slots)
      if (s.rect.h != kPanelMinHeight) allAtFloor = false;
    check(allAtFloor,
          "dock layout: **no panel is silently shrunk below legibility** either -- every one "
          "sits at exactly its floor");
    check(t.usedExtent > dock.h,
          "dock layout: `usedExtent` reports how far past the dock the slots actually run, "
          "which is the number the caller's scroll fallback needs");
    const float expected = 6.0f * kPanelMinHeight + 5.0f * kDockSplitterThickness;
    check(t.usedExtent == expected,
          "dock layout: and that number is exactly 6 floors plus 5 splitters, not an estimate");
  }

  // ==========================================================================
  // 6. A splitter drag moves one boundary and only one
  // ==========================================================================
  {
    const DockDragResult r = dockApplyDrag(200.0f, 200.0f, 1.0f, 1.0f, 72.0f, 50.0f);
    check(std::fabs((r.weightA + r.weightB) - 2.0f) < 1e-4f,
          "dock drag: **the pair's combined weight is conserved**, so every other panel in the "
          "dock keeps the exact size it had");
    check(r.weightA > r.weightB,
          "dock drag: dragging towards the far edge grows the leading panel");
    check(std::fabs(r.weightA - 1.25f) < 1e-3f && std::fabs(r.weightB - 0.75f) < 1e-3f,
          "dock drag: a 50 px drag across a 400 px pair moves exactly one eighth of the weight");

    // Clamped at both floors, and never to a zero weight.
    const DockDragResult far = dockApplyDrag(200.0f, 200.0f, 1.0f, 1.0f, 72.0f, 10000.0f);
    check(far.weightB > 0.0f && far.weightA > 0.0f,
          "dock drag: an over-long drag never produces a zero or negative weight");
    const float impliedB = 400.0f * (far.weightB / (far.weightA + far.weightB));
    check(std::fabs(impliedB - 72.0f) < 0.5f,
          "dock drag: an over-long drag stops with the trailing panel at exactly its floor");

    const DockDragResult back = dockApplyDrag(200.0f, 200.0f, 1.0f, 1.0f, 72.0f, -10000.0f);
    const float impliedA = 400.0f * (back.weightA / (back.weightA + back.weightB));
    check(std::fabs(impliedA - 72.0f) < 0.5f,
          "dock drag: and the same in the other direction, with the leading panel at its floor");

    // A pair too small to hold two floors -- reachable in the overflow case.
    // The clamp collapses to a point and the drag becomes a no-op, which is
    // correct: there is nothing to redistribute.
    const DockDragResult tiny = dockApplyDrag(50.0f, 50.0f, 1.0f, 1.0f, 72.0f, 30.0f);
    check(std::isfinite(tiny.weightA) && std::isfinite(tiny.weightB) && tiny.weightA > 0.0f &&
              tiny.weightB > 0.0f,
          "dock drag: a pair too small for two floors yields finite, positive weights");

    // A degenerate pair must not divide by zero.
    const DockDragResult zero = dockApplyDrag(0.0f, 0.0f, 1.0f, 1.0f, 72.0f, 10.0f);
    check(std::isfinite(zero.weightA) && std::isfinite(zero.weightB),
          "dock drag: a zero-extent pair returns finite weights rather than a NaN");
  }

  // ==========================================================================
  // 7. The dock-aware bands: absent docks vanish, and the default is unchanged
  // ==========================================================================
  {
    // The default extents reproduce the pre-dock chrome exactly. This is the
    // claim ui/AtelierLayout.hpp's `kDefaultDockExtents` makes in prose.
    const AtelierBands b = atelierLayout(0.0f, 0.0f, 1600.0f, 1000.0f, false);
    check(b.leftDock.w == kToolPaletteW,
          "atelier bands: the default left dock is the 52 px tool palette's width");
    check(b.rightDock.w == kRightColumnW,
          "atelier bands: the default right dock is the 322 px controls column's width");
    check(b.topDock.h == kOptionsBarH,
          "atelier bands: the default top dock is the 46 px options bar's height");
    check(b.bottomDock.h == 0.0f, "atelier bands: there is no bottom dock by default");
    check(b.rightDock.right() == 1600.0f,
          "atelier bands: the right dock sits flush with the window's right edge");

    // An empty dock takes its rule with it, and the canvas takes the space.
    AtelierDockExtents noLeft = kDefaultDockExtents;
    noLeft.left = 0.0f;
    const AtelierBands nl = atelierLayout(0.0f, 0.0f, 1600.0f, 1000.0f, false, noLeft);
    check(nl.leftDock.w == 0.0f && nl.canvas.x == 0.0f,
          "atelier bands: **an empty left dock leaves no strip and no seam** -- the canvas "
          "reaches the window edge");
    check(nl.ruleCount == b.ruleCount - 1,
          "atelier bands: suppressing a dock suppresses exactly one rule");
    check(nl.canvas.w == b.canvas.w + kToolPaletteW + kRuleThickness,
          "atelier bands: and the canvas gains exactly the dock's width plus its rule");

    // All four docks at once, and the bands still tile the window.
    AtelierDockExtents all;
    all.left = 200.0f;
    all.right = 322.0f;
    all.top = 46.0f;
    all.bottom = 120.0f;
    const AtelierBands q = atelierLayout(0.0f, 0.0f, 1600.0f, 1000.0f, true, all);
    check(q.bottomDock.h == 120.0f && q.bottomDock.bottom() + kRuleThickness == q.statusBar.y,
          "atelier bands: the bottom dock sits directly above the status bar's rule");
    check(q.canvas.bottom() + kRuleThickness == q.bottomDock.y,
          "atelier bands: and the canvas ends exactly at the bottom dock's rule");
    check(q.canvas.x == q.leftDock.right() + kRuleThickness &&
              q.canvas.right() == q.rightDock.x - kRuleThickness,
          "atelier bands: the canvas is exactly the room the two side docks leave");
    check(q.canvas.y == q.topDock.bottom() + kRuleThickness,
          "atelier bands: and it starts exactly below the top dock's rule");
    check(q.ruleCount == 7, "atelier bands: all four docks plus the tab strip is seven rules");

    // Every dock off: nothing but the fixed chrome, and the canvas is
    // everything between the title bar and the status bar.
    const AtelierBands bare =
        atelierLayout(0.0f, 0.0f, 1600.0f, 1000.0f, false, AtelierDockExtents{});
    check(bare.canvas.x == 0.0f && bare.canvas.w == 1600.0f,
          "atelier bands: **every panel undocked leaves a full-width canvas**, which is the "
          "state 'dock around the app' has to be able to reach");
    check(bare.ruleCount == 2,
          "atelier bands: with no docks at all only the title bar's and status bar's rules "
          "remain");
  }

  // ==========================================================================
  // 8. The tool grid flows, and the default arrangement did not move
  // ==========================================================================
  {
    // A tall narrow panel: one column, and the same cell size the column-only
    // arithmetic picks. This is the assertion that says the generalisation is
    // a superset rather than a replacement.
    const float paletteH = 900.0f;
    const float availH = paletteH - kToolSwatchAreaH;
    const AtelierToolGrid tall = atelierToolGrid(kToolCellMax, availH);
    check(tall.columns == 1, "tool grid: a panel one cell wide resolves to a single column");
    check(tall.cell == atelierToolCellSize(paletteH),
          "tool grid: **and to the same cell size the column-only arithmetic picks** -- the "
          "flowing grid did not move the default arrangement");
    check(!tall.overflows, "tool grid: a 900 px column fits all 18 cells without overflowing");

    // A short wide panel -- a TOOLS panel docked to the top edge. It has to
    // flow into columns or it is not a layout at all.
    const AtelierToolGrid wide = atelierToolGrid(1600.0f, 46.0f);
    check(wide.columns >= kToolCellCount,
          "tool grid: **a 46 px top dock flows all 18 cells into one row**, which is the whole "
          "reason the grid generalised");
    check(wide.rows == 1, "tool grid: and that row really is one row");
    check(!wide.overflows, "tool grid: a full-width strip fits without overflowing");

    // Too small for either. Disclosed, and still yielding a usable grid rather
    // than nothing.
    const AtelierToolGrid tiny = atelierToolGrid(30.0f, 30.0f);
    check(tiny.overflows,
          "tool grid: a panel too small for even the smallest legible cell says so");
    check(tiny.columns >= 1 && tiny.cell == kToolCellMin,
          "tool grid: and still reports a usable grid at the floor rather than zero columns");

    // Every cell accounted for, at a table of sizes -- no tool is ever
    // arithmetically unreachable.
    bool everyCellPlaceable = true;
    const float widths[] = {40.0f, 52.0f, 120.0f, 400.0f, 1600.0f};
    const float heights[] = {46.0f, 120.0f, 400.0f, 900.0f};
    for (const float w : widths)
      for (const float h : heights) {
        const AtelierToolGrid g = atelierToolGrid(w, h);
        if (g.columns * g.rows < kToolCellCount) everyCellPlaceable = false;
        if (g.cell < kToolCellMin || g.cell > kToolCellMax) everyCellPlaceable = false;
      }
    check(everyCellPlaceable,
          "tool grid: at 20 panel sizes the grid always has room for all 18 cells and always "
          "picks a cell size in range");
  }

  // ==========================================================================
  // 9. Every slot has room for its grip -- the regression that shipped
  // ==========================================================================
  //
  // The bug this section exists for: `kPanelMinHeight` was a BODY height, so a
  // panel at exactly its floor had no room for the header that collapses and
  // moves it. The draw code asked "does a header fit in 83 px, given it needs
  // 26 + 72?" and correctly answered no -- for every panel at once. Nine
  // collapsed panels became anonymous grey bars that could not be reopened and
  // four expanded ones could not be moved.
  //
  // Asserted here as arithmetic rather than left to a screenshot, and asserted
  // on BOTH classes of slot, because the outgoing rule got both wrong in
  // different ways.
  {
    check(kPanelMinHeight >= kPanelHeaderExtent + kPanelMinBody,
          "dock layout: **an expanded panel's floor includes its grip** -- the relation whose "
          "absence made every panel in the application headerless at once");
    check(kPanelMinBody > 0.0f,
          "dock layout: and leaves a body behind the grip rather than only the grip");

    // A dock shaped like the real default: four expanded panels and nine
    // collapsed, in a 322 px column.
    std::vector<DockSlotSpec> specs;
    for (size_t i = 0; i < 13; ++i) {
      DockSlotSpec sp;
      sp.collapsed = i >= 4;
      sp.minExtent = kPanelMinHeight;
      specs.push_back(sp);
    }
    bool everyExpandedFitsAGrip = true;
    bool everyCollapsedIsExactlyAGrip = true;
    for (const float dockH : {560.0f, 700.0f, 900.0f, 1180.0f, 1600.0f}) {
      const DockTiling t = dockTile(AtelierRect{0.0f, 0.0f, 322.0f, dockH}, DockSide::Right, specs);
      for (const DockSlot& sl : t.slots) {
        if (sl.collapsed) {
          if (sl.rect.h != kPanelHeaderExtent) everyCollapsedIsExactlyAGrip = false;
        } else if (sl.rect.h < kPanelHeaderExtent + kPanelMinBody) {
          everyExpandedFitsAGrip = false;
        }
      }
    }
    check(everyCollapsedIsExactlyAGrip,
          "dock layout: **a collapsed panel's slot is exactly its grip**, at every dock height -- "
          "so the thing a person clicks to reopen it is the whole panel");
    check(everyExpandedFitsAGrip,
          "dock layout: **and every expanded slot has room for a grip AND a body**, at every "
          "dock height a 13-panel dock is likely to see");
  }

  // ==========================================================================
  // 10. Tearing a panel off: where a drop lands
  // ==========================================================================
  {
    const AtelierRect region{100.0f, 200.0f, 1000.0f, 800.0f};

    const DockDropTarget l = dockDropTargetAt(region, 150.0f, 600.0f);
    const DockDropTarget r = dockDropTargetAt(region, 1050.0f, 600.0f);
    const DockDropTarget t = dockDropTargetAt(region, 600.0f, 250.0f);
    const DockDropTarget b = dockDropTargetAt(region, 600.0f, 950.0f);
    check(l.isDock && l.side == DockSide::Left && r.isDock && r.side == DockSide::Right,
          "dock drop: a drop near the left or right edge targets that dock");
    check(t.isDock && t.side == DockSide::Top && b.isDock && b.side == DockSide::Bottom,
          "dock drop: and near the top or bottom edge, that one");

    const DockDropTarget centre = dockDropTargetAt(region, 600.0f, 600.0f);
    check(!centre.isDock,
          "dock drop: **a drop in the middle is the flyout rail** -- 'not docked anywhere' is a "
          "target, not a failure to hit one");

    // Every point resolves to exactly one target, and the zones cover the
    // region -- swept rather than sampled at a few tidy points.
    bool everyPointResolves = true;
    size_t dockHits = 0, flyoutHits = 0;
    for (int i = 0; i <= 40; ++i)
      for (int j = 0; j <= 40; ++j) {
        const float x = region.x + region.w * (static_cast<float>(i) / 40.0f);
        const float y = region.y + region.h * (static_cast<float>(j) / 40.0f);
        const DockDropTarget d = dockDropTargetAt(region, x, y);
        if (d.isDock) ++dockHits; else ++flyoutHits;
        // A malformed result would be an out-of-range side; the enum makes
        // that unrepresentable, so what is checked is that the sweep reaches
        // both kinds of answer rather than collapsing to one.
        (void)d;
      }
    check(dockHits > 0 && flyoutHits > 0 && everyPointResolves,
          "dock drop: a 41x41 sweep of the region reaches both dock and flyout targets");

    // A point outside the region resolves to the nearest edge rather than to
    // nothing -- a pointer a few pixels past the window edge is a person
    // aiming AT that edge.
    const DockDropTarget outside = dockDropTargetAt(region, -500.0f, 600.0f);
    check(outside.isDock && outside.side == DockSide::Left,
          "dock drop: a drop past the left edge still lands in the left dock, not nowhere");

    // Corners behave the same shape on a wide region as on a tall one, which
    // is the whole reason the test is proportional rather than in pixels.
    // Stated as the property itself: the SAME RELATIVE point gives the SAME
    // answer whatever the region's shape. Written first as two hand-picked
    // expectations, which was wrong in both -- a 2000x400 region put (0.05,
    // 0.10) in the Left zone, not the Top, because 0.05 of the way in from the
    // left really is nearer than 0.10 of the way down. Asserting the invariant
    // instead of two guesses is both correct and the thing worth checking.
    const AtelierRect wide{0.0f, 0.0f, 2000.0f, 400.0f};
    const AtelierRect tall{0.0f, 0.0f, 400.0f, 2000.0f};
    const AtelierRect square{0.0f, 0.0f, 900.0f, 900.0f};
    bool shapeIndependent = true;
    const float probes[][2] = {{0.05f, 0.10f}, {0.10f, 0.05f}, {0.5f, 0.5f},
                               {0.95f, 0.5f},  {0.5f, 0.95f},  {0.02f, 0.02f}};
    for (const auto& pr : probes) {
      const DockDropTarget a = dockDropTargetAt(wide, wide.w * pr[0], wide.h * pr[1]);
      const DockDropTarget b = dockDropTargetAt(tall, tall.w * pr[0], tall.h * pr[1]);
      const DockDropTarget c = dockDropTargetAt(square, square.w * pr[0], square.h * pr[1]);
      if (a.isDock != b.isDock || a.isDock != c.isDock) shapeIndependent = false;
      if (a.isDock && (a.side != b.side || a.side != c.side)) shapeIndependent = false;
    }
    check(shapeIndependent,
          "dock drop: **a drop resolves by proportional distance**, so the same relative point "
          "gives the same answer on a wide, a tall and a square region");

    const DockDropTarget empty = dockDropTargetAt(AtelierRect{}, 10.0f, 10.0f);
    check(!empty.isDock, "dock drop: an empty region targets nothing rather than a random edge");
  }

  // ==========================================================================
  // 11. Which panels a splitter drag redistributes between
  // ==========================================================================
  //
  // **This section exists because of a shipped defect, and its first assertion
  // is that defect stated directly.** The rule was "both immediate neighbours
  // must be expanded", which in the default arrangement -- expanded, collapsed,
  // collapsed, expanded, collapsed, collapsed -- left every one of the five
  // boundaries inert. The user could not resize anything:
  // *"I found that the colorpanel was too big and I couldn't resize it."*
  {
    // Exactly the default right dock's shape.
    std::vector<DockSlotSpec> specs(6);
    specs[0].collapsed = false;  // COLOR
    specs[1].collapsed = true;   // BRUSH LIBRARY
    specs[2].collapsed = true;   // BRUSH EDITOR
    specs[3].collapsed = false;  // LAYERS
    specs[4].collapsed = true;   // HISTORY
    specs[5].collapsed = true;   // COMPS
    const DockTiling t = dockTile(AtelierRect{0.0f, 0.0f, 320.0f, 640.0f}, DockSide::Right, specs);
    check(t.slots.size() == 6 && t.splitters.size() == 5,
          "dock drag: the default-shaped dock tiles into six slots and five boundaries");

    // The old rule, written out here so the assertion below is measured
    // against it rather than against a memory of it.
    size_t liveUnderOldRule = 0;
    for (size_t i = 0; i + 1 < t.slots.size(); ++i)
      if (!t.slots[i].collapsed && !t.slots[i + 1].collapsed) ++liveUnderOldRule;
    check(liveUnderOldRule == 0,
          "dock drag: **under the both-neighbours-expanded rule NONE of them is draggable** -- "
          "which is exactly what shipped, and what the user reported as being unable to resize "
          "the COLOR panel");

    size_t live = 0;
    for (size_t i = 0; i + 1 < t.slots.size(); ++i)
      if (dockDragPairFor(t.slots, i).live) ++live;
    check(live == 3,
          "dock drag: reaching past collapsed neighbours makes three of the five live -- every "
          "boundary that has an expanded panel on both sides of it, however far away");

    const DockDragPair first = dockDragPairFor(t.slots, 0);
    check(first.live && first.indexA == 0 && first.indexB == 3,
          "dock drag: **the boundary under COLOR resizes COLOR against LAYERS**, reaching past "
          "the two collapsed panels between them -- a collapsed panel is ballast, not a wall");
    const DockDragPair mid = dockDragPairFor(t.slots, 2);
    check(mid.live && mid.indexA == 0 && mid.indexB == 3,
          "dock drag: and so does every boundary between the same two, so a person can grab "
          "whichever of those three lines they aimed at");

    const DockDragPair below = dockDragPairFor(t.slots, 3);
    check(!below.live,
          "dock drag: the boundary under LAYERS is inert -- there is no expanded panel below it "
          "at all, so nothing on that side can give or take");
    const DockDragPair last = dockDragPairFor(t.slots, 4);
    check(!last.live, "dock drag: and neither is the one below that, for the same reason");

    check(!dockDragPairFor(t.slots, 5).live && !dockDragPairFor(t.slots, 99).live,
          "dock drag: an index naming no boundary is not live rather than out of bounds");

    // Two adjacent expanded panels still answer with themselves -- the new
    // rule has to be a generalisation of the old one, not a replacement for it.
    std::vector<DockSlotSpec> plain(3);
    const DockTiling pt =
        dockTile(AtelierRect{0.0f, 0.0f, 320.0f, 640.0f}, DockSide::Right, plain);
    const DockDragPair p0 = dockDragPairFor(pt.slots, 0);
    const DockDragPair p1 = dockDragPairFor(pt.slots, 1);
    check(p0.live && p0.indexA == 0 && p0.indexB == 1 && p1.live && p1.indexA == 1 &&
              p1.indexB == 2,
          "dock drag: with nothing collapsed each boundary still names its own two neighbours "
          "-- the reach-past rule generalises the old one rather than replacing it");

    // An all-collapsed dock has nothing to redistribute anywhere, which is the
    // one case where drawing an inert boundary is the honest answer.
    std::vector<DockSlotSpec> allShut(4);
    for (DockSlotSpec& sp : allShut) sp.collapsed = true;
    const DockTiling ct =
        dockTile(AtelierRect{0.0f, 0.0f, 320.0f, 640.0f}, DockSide::Right, allShut);
    bool noneLive = true;
    for (size_t i = 0; i + 1 < ct.slots.size(); ++i)
      if (dockDragPairFor(ct.slots, i).live) noneLive = false;
    check(noneLive,
          "dock drag: a dock of entirely collapsed panels has no live boundary -- fixed sizes "
          "all the way down, and a handle there really would do nothing");
  }

  // ==========================================================================
  // 12. Dropping INSIDE a dock: beside a slot, or onto it
  // ==========================================================================
  {
    std::vector<DockSlotSpec> specs(3);
    specs[1].collapsed = true;
    const AtelierRect dock{100.0f, 50.0f, 320.0f, 620.0f};
    const DockTiling t = dockTile(dock, DockSide::Right, specs);
    const float midX = dock.x + dock.w * 0.5f;

    // Every point inside a slot resolves to that slot, and to exactly one mode.
    bool everyPointInside = true;
    bool modesTile = true;
    for (size_t i = 0; i < t.slots.size(); ++i) {
      const AtelierRect& r = t.slots[i].rect;
      size_t before = 0, into = 0, after = 0;
      for (int k = 0; k < 40; ++k) {
        const float y = r.y + r.h * (static_cast<float>(k) + 0.5f) / 40.0f;
        const DockSlotDrop d = dockSlotDropAt(t, DockSide::Right, midX, y);
        if (!d.valid || d.slotIndex != i) everyPointInside = false;
        if (d.mode == DockSlotDropMode::Before) ++before;
        else if (d.mode == DockSlotDropMode::Into) ++into;
        else ++after;
      }
      if (before + into + after != 40 || before == 0 || into == 0 || after == 0) modesTile = false;
    }
    check(everyPointInside,
          "slot drop: every point inside a slot names that slot -- including the collapsed one, "
          "which is one grip tall");
    check(modesTile,
          "slot drop: **and every slot has all three zones, the collapsed one included** -- a "
          "collapsed panel is one grip tall and must still be droppable ONTO, which is what "
          "makes the edge band a fraction rather than a fixed number of pixels");

    // The edge zones are capped in pixels, not only as a fraction: the whole
    // point is that a tall slot does not spend two thirds of itself on insert.
    {
      const AtelierRect& tall = t.slots[0].rect;
      const DockSlotDrop atCap =
          dockSlotDropAt(t, DockSide::Right, midX, tall.y + kDockSlotEdgeMaxPx + 1.0f);
      check(tall.h * kDockSlotEdgeFraction > kDockSlotEdgeMaxPx &&
                atCap.mode == DockSlotDropMode::Into,
            "slot drop: **a tall slot's insert band is capped in pixels** -- a fraction alone "
            "would give the harder gesture, stacking, the smaller target");
    }

    check(dockSlotDropAt(t, DockSide::Right, midX, t.slots[0].rect.y + 1.0f).mode ==
              DockSlotDropMode::Before,
          "slot drop: the top edge of a slot inserts before it");
    check(dockSlotDropAt(t, DockSide::Right, midX, t.slots[0].rect.bottom() - 1.0f).mode ==
              DockSlotDropMode::After,
          "slot drop: and its bottom edge inserts after it");

    check(!dockSlotDropAt(t, DockSide::Right, dock.x - 20.0f, dock.y + 20.0f).valid,
          "slot drop: a point outside the dock names no slot, so the caller falls back to the "
          "dock-level answer rather than being handed slot 0");

    // A horizontal dock divides its WIDTH, so the three zones run along x.
    std::vector<DockSlotSpec> row(2);
    for (DockSlotSpec& sp : row) sp.minExtent = kPanelMinWidth;
    const AtelierRect band{0.0f, 0.0f, 900.0f, 46.0f};
    const DockTiling rt = dockTile(band, DockSide::Top, row);
    const AtelierRect& r0 = rt.slots[0].rect;
    check(dockSlotDropAt(rt, DockSide::Top, r0.x + 1.0f, 20.0f).mode ==
                  DockSlotDropMode::Before &&
              dockSlotDropAt(rt, DockSide::Top, r0.x + r0.w * 0.5f, 20.0f).mode ==
                  DockSlotDropMode::Into &&
              dockSlotDropAt(rt, DockSide::Top, r0.right() - 1.0f, 20.0f).mode ==
                  DockSlotDropMode::After,
          "slot drop: **in a top dock the three zones run along x**, because that is the axis "
          "that dock divides -- the same rule, not a second one");
  }

  std::printf("[selftest] dock layout %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
