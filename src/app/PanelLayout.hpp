#pragma once

#include <string>
#include <vector>

#include "app/ControlsLayout.hpp"

// app/PanelLayout -- **where every panel is, how big it is, and whether it is
// docked, flown out or put away** -- as a value the user rearranges and that
// survives a relaunch.
//
// This module replaces `app/ControlsColumnLayout`, which modelled the one
// thing the chrome used to have: an ordered list of sections in a single
// right-hand column, each shown or hidden. The user's instruction retired that
// shape:
//
//   *"revamp the right panel to be dockable, not scrollable, I want to be
//   able to put the parts I want in and have them stay put, and put others in
//   flyout mode or dock around the app"*
//
// -- and, on the question of which edges and which panels:
//
//   *"All four edges but move the brush setting and the tool pallet to
//   dockable panels as well, this makes the UI modular and customizable"*
//
// So a panel's state is no longer a bool. It is **a placement** (one of four
// docks, or flyout, or hidden), **a position** within that placement, **a
// weight** (its share of the dock, which a splitter drag rewrites), and
// **whether it is collapsed**. The four docks additionally carry an extent --
// the left dock's width, the top dock's height -- because those, too, are
// numbers a person drags rather than constants the layout owns.
//
// The division of labour with the rest of the build is unchanged from the
// module this replaces, and is worth restating because it is what keeps the
// whole feature testable: **this is the headless half only.** Nothing here
// draws. `ui/DockLayout` turns a dock's panel list into rectangles (also
// headless, also asserted), and `ui/MacPaintUI.cpp` is the only file that
// knows what a splitter looks like or how a flyout opens.
//
// ==========================================================================
// The invariant that matters most -- unchanged
// ==========================================================================
//
// **The sequence contains every `ControlsSection` enumerator exactly once,
// always.** Placement, order, weight and collapse vary; membership does not.
// Every mutator, `resetToDefault()`, and every `loadFromFile()` / `parse()`
// returns a layout that still holds this, even when the input that produced
// it (a hand-edited file, a file from an older or newer build, a truncated
// write) did not describe one.
//
// **Emptying every dock is legal.** Nothing here forces a panel to stay
// docked, and a layout in which all fifteen panels are hidden is a layout this
// model will happily hold. The way back in is the PANELS menu in the title
// bar, which is deliberately not in any dock -- the same bargain the outgoing
// module struck, now load-bearing for four docks instead of one column.
//
// ==========================================================================
// Serialization: stable string keys, never an enum ordinal
// ==========================================================================
//
// Carried over verbatim from `app/ControlsColumnLayout`, including the reason.
// This project has already been bitten once by ordinal-keyed persisted data:
// an enum grows a new enumerator in the middle of its declared order and every
// file written before that point silently decodes to the WRONG section, with
// no parse error at all -- see app/UserBrushLibrary.hpp §2 for the same lesson
// learned about `DynamicSource`/`DynamicTarget` ordinals.
//
// **This revision is the proof that the rule was worth keeping.** It inserts
// two enumerators (`Tools`, `Options`) at the FRONT of `ControlsSection`.
// Under ordinal keys every panel-layout file ever written would now decode one
// or two places off -- a user's LAYERS panel silently becoming their SOLVER
// panel. Under text keys, an old file simply does not mention `tools` or
// `options`, which is the ordinary "missing section" case handled below by
// construction.
//
// So every section is written and read as a short, stable, lower-case text key
// (`controlsSectionKey()` / `controlsSectionFromKey()`), and so is every
// placement (`panelPlacementKey()` / `panelPlacementFromKey()`).
//
// ==========================================================================
// File format, version 2
// ==========================================================================
//
//     naturalPaint-panel-layout 2
//     dock left 52
//     dock right 322
//     dock top 46
//     dock bottom 0
//     panel tools left 1.000 0
//     panel options top 1.000 0
//     panel color right 1.000 0
//     panel layers right 2.000 0
//     panel history right 1.000 1
//     ...
//
// `dock <side> <extent-px>` for each of the four; `panel <key> <placement>
// <weight> <collapsed>` per section, **in the order the panels appear within
// their own placement**. A panel's position in a dock is its position among
// the other `panel` lines that name the same placement -- the file has no
// explicit index, because an index is a second source of truth that a
// hand-edit can put in conflict with the line order.
//
// ==========================================================================
// Reading a version 1 file
// ==========================================================================
//
// Version 1 was `section <key> <0|1>`, one line per section, order implied,
// the bool meaning shown-in-the-right-column. **Those files still read**, and
// not by a special case bolted on the side: a `section` line is simply a
// second recognised grammar whose meaning is defined in terms of the first --
//
//     section <key> 1   ==   panel <key> right <default weight> 0
//     section <key> 0   ==   panel <key> hidden <default weight> 0
//
// -- which is exactly what those files meant. A user who had arranged their
// column before this revision keeps that arrangement, in the right dock, in
// their order, and the two new panels arrive via the missing-section rule at
// their defaults. Refusing the old file, or silently resetting on it, would
// throw away real user work to save a dozen lines of code.
//
// ==========================================================================
// Round-trip repair, exactly
// ==========================================================================
//
// A file on disk may have been written by an older or a newer build, or by a
// person's editor. `parse()` always yields a layout satisfying the invariant
// above. The rules, applied together, in this order:
//
//  * **Unknown section name, or unknown placement name** -- the line is
//    ignored. Grammatically fine, so it does not itself invalidate the file.
//  * **Duplicate section name** -- the first occurrence is kept, every later
//    one for the same section is ignored.
//  * **Missing section** -- any `ControlsSection` enumerator not named by a
//    (recognised, non-duplicate) line is APPENDED with its DEFAULT placement
//    and weight, in `controlsSections()`'s own order relative to the other
//    missing sections. This is the case this codebase has a name for: "a
//    section added since the file was written must appear, not vanish".
//
//    **Default placement, not a fixed one.** The outgoing module appended a
//    missing section as visible-in-the-column, which was the only place there
//    was. Appending everything to the right dock now would drop a
//    version-1-era user's TOOLS panel into their right-hand column instead of
//    onto the left edge where the design puts it -- so a missing section
//    arrives wherever `resetToDefault()` would have put it.
//  * **Malformed line** -- a grammar failure is SKIPPED, like the unknown-key
//    rule above it and like app/UserBrushLibrary.cpp's per-line salvage. A
//    non-finite or non-positive weight is a grammar failure of this kind: it
//    would make a dock's arithmetic divide by zero or produce a NaN rect, and
//    ui/DockLayout defends itself against that too, but a file should not be
//    the place that defence is first exercised.
//
//    The alternative -- one bad line invalidating the whole file -- was
//    written first and is worse on both cases it is meant to cover. It has no
//    upside on a *foreign* file, because a file that is not this format has no
//    salvageable lines at all, so every section is "missing" and the rule
//    above rebuilds the default anyway. And it has a real cost on a file that
//    is *mostly* this format -- one damaged byte throwing away an arrangement
//    the user built.
//
// A missing file is the ordinary first-run case, not an error: it is treated
// exactly like an empty one, which -- every section "missing" -- resolves to
// `resetToDefault()`'s layout by the same rule, not by a special case.
//
// ==========================================================================
// Persistence mechanics
// ==========================================================================
//
// `~/Library/Application Support/naturalPaint/panel-layout.txt` on macOS,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/panel-layout.txt` elsewhere,
// overridable with `$NP_PANEL_LAYOUT` -- **the same path the outgoing module
// used**, deliberately: the version-1 compatibility above is worth nothing if
// the new build reads a different file from the one the old build wrote.
//
// Written via temp-file-then-rename, matching `app/UserBrushLibrary.cpp`'s
// `writeFileAtomically()`/`syncPath()` in shape and reimplemented rather than
// shared, for that file's own stated reason (app/UserBrushLibrary.hpp §4).
namespace np {

// Where a panel lives.
enum class PanelPlacement {
  // The four docks. A panel here occupies a slot in that dock and is always
  // on screen.
  Left,
  Right,
  Top,
  Bottom,
  // On the rail, opening over the canvas when clicked and closing when
  // dismissed. The user's "flyout mode": visible but not resident.
  Flyout,
  // Not on screen at all, and not on any rail. Reachable only from the PANELS
  // menu, which is what makes this recoverable rather than a trap.
  Hidden,
};

// True for the four placements that are docks (i.e. that `dockSideOf()` can
// answer for).
bool panelPlacementIsDock(PanelPlacement p) noexcept;

// One panel's state.
struct PanelEntry {
  ControlsSection section = ControlsSection::Color;
  PanelPlacement placement = PanelPlacement::Right;
  // This panel's share of its dock, relative to its expanded neighbours --
  // ui/DockLayout.hpp's `DockSlotSpec::weight`. Meaningless for `Flyout` and
  // `Hidden`, and preserved across a trip through them so that a panel put
  // away and brought back returns to the size it had.
  float weight = 1.0f;
  bool collapsed = false;
};

inline constexpr int kPanelLayoutFileVersion = 2;
inline constexpr const char* kPanelLayoutFileHeader = "naturalPaint-panel-layout";

// The default weight, and the floor a parsed weight has to clear.
inline constexpr float kPanelDefaultWeight = 1.0f;
inline constexpr float kPanelMinWeight = 0.01f;

// The stable text key for one section -- see this header's serialization
// section for why it is a key and not `controlsSectionSpec(section).title`
// (a title is UI wording, free to be reworded; a persistence key is not) and
// not `static_cast<int>(section)` (an ordinal, exactly the trap this file
// exists to avoid).
//
// Total and defined for every `ControlsSection` enumerator -- `--selftest`
// asserts that.
const char* controlsSectionKey(ControlsSection section);

// The inverse. Returns false, leaving `*out` unchanged, for any string that is
// not exactly one of `controlsSectionKey()`'s outputs.
bool controlsSectionFromKey(const std::string& key, ControlsSection* out);

// The same pair for placements: `left` / `right` / `top` / `bottom` /
// `flyout` / `hidden`.
const char* panelPlacementKey(PanelPlacement placement);
bool panelPlacementFromKey(const std::string& key, PanelPlacement* out);

// `~/Library/Application Support/naturalPaint/panel-layout.txt` on macOS (see
// this header's persistence section for the other platforms and the
// `$NP_PANEL_LAYOUT` override).
std::string defaultPanelLayoutFilePath();

// The extents of the four docks, in pixels. Zero means the dock is absent --
// ui/AtelierLayout.hpp's `AtelierDockExtents` has the same convention and this
// converts straight into it.
struct PanelDockExtents {
  float left = 0.0f;
  float right = 0.0f;
  float top = 0.0f;
  float bottom = 0.0f;
};

// The floor a dock's extent is clamped to when a person drags its edge. Below
// this a dock is a sliver that cannot show a panel and cannot easily be
// grabbed to drag back.
//
// A left dock of 52 px is the TOOLS panel's own default and is perfectly
// usable, so the width floor is that number rather than ui/DockLayout.hpp's
// `kPanelMinWidth` -- those two answer different questions (how narrow may a
// whole dock be, versus how narrow may one of several SIDE-BY-SIDE panels
// inside a dock be) and conflating them would forbid the default arrangement.
inline constexpr float kDockMinWidth = 52.0f;
inline constexpr float kDockMinHeight = 46.0f;

// The ordered, always-complete model of every panel in the application.
//
// A freshly constructed instance already satisfies the exactly-once
// invariant: the constructor is `resetToDefault()`.
class PanelLayout {
 public:
  PanelLayout();

  // Discards whatever arrangement was in place and returns to the default:
  // TOOLS on the left, OPTIONS on the top, every other section in the right
  // dock in `controlsSections()`'s own order, nothing on the bottom, nothing
  // in a flyout, nothing hidden, and the dock extents `ui/AtelierLayout.hpp`'s
  // `kDefaultDockExtents`.
  //
  // **Four panels start expanded: TOOLS, OPTIONS, COLOR and LAYERS.** The
  // other eleven start as a titled grip, one click from open. A dock does not
  // scroll, so its open panels are taken directly out of each other, which
  // makes the default-open set a *budget allocation* rather than a preference
  // -- and LAYERS starts at a heavier weight than its neighbours because it is
  // the only panel here whose content is a list. See `defaultEntryFor()` in
  // the .cpp for the measurement that settled both numbers, and for why this
  // is no longer `controlsSections()`'s `defaultOpen` inverted.
  //
  // `--selftest` holds this to arithmetic: it lays the default right dock out
  // at the reference window's height and asserts LAYERS has room for three
  // layer rows and that no expanded panel is pinned at its floor.
  void resetToDefault();

  // The full ordered list, every placement included -- what the PANELS menu
  // draws.
  const std::vector<PanelEntry>& entries() const noexcept { return entries_; }

  // The sections in one placement, in their order within it. This is what a
  // dock's draw loop iterates and what `ui/DockLayout`'s slot specs are built
  // from.
  std::vector<ControlsSection> sectionsIn(PanelPlacement placement) const;

  // The index of `section` in `entries()`. Always found -- the invariant
  // guarantees it -- so this returns `entries_.size()` only if that invariant
  // has somehow been broken, which is a state `--selftest` treats as a bug,
  // not an input to handle gracefully.
  size_t indexOf(ControlsSection section) const noexcept;

  PanelPlacement placementOf(ControlsSection section) const noexcept;
  float weightOf(ControlsSection section) const noexcept;
  bool isCollapsed(ControlsSection section) const noexcept;

  // Moves `section` into `placement`, appending it at the END of whatever is
  // already there. Its weight and collapse state are preserved, so a panel
  // moved out to a flyout and back returns at the size it had.
  void setPlacement(ControlsSection section, PanelPlacement placement);

  // Moves `section` into `placement` at position `indexInPlacement` among the
  // panels already there (clamped). This is what a drag-and-drop onto a
  // specific slot calls; `setPlacement()` is the append-to-end shorthand.
  void setPlacementAt(ControlsSection section, PanelPlacement placement,
                      size_t indexInPlacement);

  void setWeight(ControlsSection section, float weight);
  void setCollapsed(ControlsSection section, bool collapsed);

  // Swaps `section` with its neighbour one position earlier/later **within its
  // own placement**. A no-op at the ends of that placement -- there is no
  // wraparound, and neither ever changes which dock a panel is in.
  //
  // Within-placement rather than within-`entries()`: the two were the same
  // thing when there was one column, and are not now. A LAYERS panel in the
  // right dock whose predecessor in `entries()` happens to be a left-dock
  // panel must not swap with it -- from the user's side that is a row that
  // jumps to another edge of the window when they press the up arrow.
  void moveUp(ControlsSection section);
  void moveDown(ControlsSection section);

  const PanelDockExtents& dockExtents() const noexcept { return docks_; }

  // Sets one dock's extent, clamped to `kDockMinWidth`/`kDockMinHeight` --
  // except that an extent of exactly zero is allowed through, because zero is
  // how a dock is switched off and clamping it up to 52 would make an empty
  // dock impossible to hide.
  void setDockExtent(PanelPlacement dock, float extent);

  // The extent a dock should actually be given this frame: its stored extent
  // when it holds at least one panel, and zero when it does not.
  //
  // **A dock with nothing in it is not on screen.** That is the rule that
  // makes "dock around the app" cost nothing when unused -- an untouched
  // bottom dock is not a 46 px empty band, it is absent -- and it lives here,
  // beside the stored extents, rather than in the draw code, so a test can
  // assert it without a window.
  PanelDockExtents effectiveDockExtents() const;

  // Parses `text` per this header's round-trip repair rules, replacing this
  // instance's contents. Accepts both the version 2 grammar and version 1's
  // `section` lines (see this header's version 1 note). Always leaves the
  // invariant satisfied, including on a malformed or empty input.
  void parse(const std::string& text);

  // The exact bytes `saveToFile()` writes for the layout as it stands now,
  // always in the version 2 grammar.
  std::string serialize() const;

  // Reads `path` if it exists and calls `parse()` on its contents. A missing
  // file is the ordinary first-run case, not a failure: it is treated exactly
  // like an empty one, which resolves to `resetToDefault()`'s layout through
  // `parse()`'s own "every section missing" rule -- so this always returns
  // true and always leaves a fully-populated layout, and `errorOut` is only
  // ever set for a file that exists but could not be read to the end.
  bool loadFromFile(const std::string& path, std::string* errorOut);

  // Writes via temp-file-then-rename (see this header's persistence section).
  // `errorOut` is populated on every failure path; a rename that did not
  // complete removes the `.tmp` file rather than leaving it beside the real
  // one.
  bool saveToFile(const std::string& path, std::string* errorOut) const;

 private:
  std::vector<PanelEntry> entries_;
  PanelDockExtents docks_;
};

}  // namespace np
