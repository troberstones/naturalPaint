#pragma once

#include <cstddef>
#include <vector>

#include "ui/AtelierLayout.hpp"  // AtelierRect
#include "ui/AtelierTheme.hpp"   // kRuleThickness

// ui/DockLayout -- **how the panels inside one dock divide it up**, as
// arithmetic.
//
// ==========================================================================
// The instruction this file exists to satisfy
// ==========================================================================
//
// The user's own words: *"revamp the right panel to be dockable, not
// scrollable, I want to be able to put the parts I want in and have them stay
// put, and put others in flyout mode or dock around the app."*
//
// Two halves. `app/PanelLayout` owns "which panels, in which dock" -- the
// part a person edits and that survives a relaunch. **This file owns the
// consequence of the word `not scrollable`:** if the dock as a whole does not
// scroll, then every panel in it has to be given a slot, and those slots have
// to add up to exactly the dock. That is a tiling problem, and this project
// already has a settled answer for what to do with a tiling problem -- see
// ui/AtelierLayout.hpp's own header:
//
//   *"Deliberately free of ImGui itself: the geometry is the part worth
//   testing, and a test that needs a window, a device and a font atlas to
//   check that four bands tile a rectangle is a test nobody runs."*
//
// Same rule here, for the same reason, and `--selftest` asserts the tiling at
// a table of dock sizes and slot counts with no ImGui context anywhere near
// it.
//
// ==========================================================================
// Why a dock needs an axis, and what that costs
// ==========================================================================
//
// The old right-hand column only ever stacked downwards, so "divide the dock"
// meant "divide its height". Panels can now dock to all four edges, and a
// **top or bottom dock is a row, not a column**: the panels in it sit side by
// side and divide the dock's *width*. `DockSide` carries that, and everything
// below is written once, against a `major` axis (the one the slots divide)
// and a `minor` axis (the one every slot spans in full).
//
// The cost of the generalisation is that a panel's *minimum* is now
// side-dependent -- 72 px is a sensible floor for a stacked panel's height and
// a useless one for a side-by-side panel's width -- so `minExtent` is an input
// per slot rather than a constant in this file. `kPanelMinHeight` /
// `kPanelMinWidth` below are the two values the caller actually passes; they
// live here so the numbers are next to the arithmetic that consumes them, not
// so this file picks between them.
//
// ==========================================================================
// The size policy, and the one case it cannot rescue
// ==========================================================================
//
// The user chose drag-resizable slots with per-panel scrolling, over
// content-height (a panel pushed off the bottom by whatever sits above it --
// which is the problem being fixed) and over an equal split (COLOR given as
// much room as LAYERS whether it needs it or not). So:
//
//  * A **collapsed** panel takes exactly `headerExtent` and no share of
//    anything. This is how a person makes room: collapsing is a size control,
//    not just a tidiness one.
//  * An **expanded** panel takes a share of what is left, in proportion to
//    its `weight`, floored at `minExtent`. The weights are what a splitter
//    drag writes back, and what `app/PanelLayout` persists.
//  * A panel whose content exceeds its own slot **scrolls inside that slot**.
//    Nothing here knows about that -- it is `ui/MacPaintUI.cpp`'s child-window
//    flag -- but it is the reason a floor of 72 px is defensible: a panel is
//    never truncated, only shortened.
//
// **The honest limit**, stated the way ui/AtelierLayout.hpp states the tool
// palette's: when the minima plus the headers plus the splitters exceed the
// dock, no distribution of weight can help. `DockTiling::overflowed` says so,
// every expanded slot gets exactly its minimum, and the slots then run past
// the dock's far edge rather than being silently shrunk into illegibility or
// silently dropped. The caller's disclosed fallback is to let the dock itself
// scroll in that one case -- which is the behaviour the whole file exists to
// avoid, reached only when the alternative is a panel too small to use.
//
// ==========================================================================
// Exact tiling
// ==========================================================================
//
// Slots and splitters tile the dock rect **exactly**: no overlap, no gap, and
// the last expanded slot absorbs every rounding error. That last part is
// ui/AtelierLayout.cpp's own trick ("every rounding error lands in the canvas
// rather than in a 26 px bar that would then not sit flush with the window
// edge"), applied to a dock instead of a window, and it is what lets
// `--selftest` assert `sum(extents) == dock extent` as an equality rather than
// as a tolerance.
namespace np {

// Which edge a dock is attached to. Left/Right stack their panels downwards
// and divide the dock's height; Top/Bottom place them side by side and divide
// its width.
enum class DockSide { Left, Right, Top, Bottom };

// True when `side`'s slots divide the dock's HEIGHT (a column of panels).
constexpr bool dockStacksVertically(DockSide side) noexcept {
  return side == DockSide::Left || side == DockSide::Right;
}

// The splitter between two neighbouring slots.
//
// Wider than `kRuleThickness` on purpose, and the difference is the whole
// point: a 2 px rule is a *drawn* separator and a 6 px splitter is a *grabbed*
// one. Dear ImGui hit-tests a rect, the user aims with a mouse, and a 2 px
// target is the kind of control that reads as broken rather than as precise.
// Six is the smallest width at which a drag starts on the first attempt at
// this project's cursor sizes; the rule itself is still drawn 2 px down the
// middle of it, so the splitter looks like the rules everywhere else in the
// chrome and behaves like a handle.
constexpr float kDockSplitterThickness = 6.0f;
static_assert(kDockSplitterThickness >= kRuleThickness,
              "the splitter has to be at least as thick as the rule drawn inside it, or the "
              "rule would be the thing overhanging the handle rather than the other way round.");

// A collapsed panel is its header and nothing else. One row of text plus the
// padding above and below it, which is what `CollapsingHeader` occupies at
// this build's 13 px UI font -- named here rather than measured, because this
// file is ImGui-free by the rule in its own header, and a floor that is a
// pixel or two generous costs a collapsed panel nothing.
constexpr float kPanelHeaderExtent = 26.0f;

// The floor for an EXPANDED panel, per axis. See this header's axis note for
// why there are two of these and why the caller picks.
//
// 72 px of height is the header plus roughly two rows of controls -- the point
// below which a panel stops being a panel and becomes a label with a scrollbar.
// 200 px of width is `app/ControlsLayout.hpp`'s own `kControlsMinWidgetPx`
// (90 px, the width at which a 0..1 slider still resolves to about a hundred
// distinct positions) plus room for the label column beside it; a side-by-side
// panel narrower than that cannot show one labelled control, which is the same
// judgement made about the same widget, on the other axis.
constexpr float kPanelMinHeight = 72.0f;
constexpr float kPanelMinWidth = 200.0f;

// What one panel asks of the dock it sits in.
struct DockSlotSpec {
  // Collapsed panels take `headerExtent` and no share of the remainder.
  bool collapsed = false;
  // The panel's share of the free space, relative to its expanded siblings.
  // Any positive value; the absolute scale is meaningless, only the ratios
  // matter, which is what lets a splitter drag rewrite two of them without
  // having to renormalise the rest.
  float weight = 1.0f;
  // The floor for this panel when expanded -- `kPanelMinHeight` in a vertical
  // dock, `kPanelMinWidth` in a horizontal one.
  float minExtent = kPanelMinHeight;
  // What this panel occupies when collapsed.
  float headerExtent = kPanelHeaderExtent;
};

// Where one panel ended up.
struct DockSlot {
  AtelierRect rect;
  bool collapsed = false;
  // True when this slot was cut to `minExtent` -- i.e. its weight asked for
  // less than the floor allows. Reported rather than inferred because the
  // caller draws a resize handle differently for a slot that cannot shrink
  // further, and recomputing "is this at its floor" at the call site would be
  // the same arithmetic written a second time.
  bool atMinimum = false;
};

// The result of dividing one dock.
struct DockTiling {
  std::vector<DockSlot> slots;
  // Exactly `slots.size() - 1` of them when there are any slots at all, and
  // empty otherwise -- a splitter is *between* two panels and belongs to
  // neither, the same way `AtelierBands`' rules are between bands.
  std::vector<AtelierRect> splitters;
  // The minima did not fit. See this header's "honest limit".
  bool overflowed = false;
  // The extent the slots actually consumed along the major axis. Equal to the
  // dock's own extent in every case except `overflowed`, where it is larger --
  // which is exactly the number a caller needs to size the scroll region it
  // falls back to.
  float usedExtent = 0.0f;
};

// Divide `dock` among `specs`, along the axis `side` implies.
//
// An empty `specs`, or an empty `dock`, yields an empty tiling rather than a
// degenerate one: a dock with no panels in it is a dock that should not be on
// screen at all, and `atelierLayout()` already suppresses it.
DockTiling dockTile(const AtelierRect& dock, DockSide side, const std::vector<DockSlotSpec>& specs);

// Convert a splitter drag into a new pair of weights.
//
// `deltaPx` is how far the splitter between slot `i` and slot `i+1` was
// dragged along the major axis (positive = towards the far edge, i.e. slot `i`
// grows). The two slots' *current* extents and weights go in, and the two new
// weights come out.
//
// **Only the dragged pair changes.** Every other slot's weight is untouched,
// so a drag moves exactly the boundary the user grabbed and does not ripple
// through the whole dock -- which is the behaviour that makes "have them stay
// put" true of the panels a person is not currently dragging. This is only
// possible because weights are ratios with no fixed scale (see
// `DockSlotSpec::weight`): the pair can absorb the change between themselves
// while the others keep the numbers they had.
//
// Clamped so neither slot goes below `minExtent`, and both outputs are always
// strictly positive -- a zero or negative weight would make a slot vanish and
// there would then be no boundary left to drag back.
struct DockDragResult {
  float weightA = 1.0f;
  float weightB = 1.0f;
};
DockDragResult dockApplyDrag(float extentA, float extentB, float weightA, float weightB,
                             float minExtent, float deltaPx);

}  // namespace np
