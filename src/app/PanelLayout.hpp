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
// File format, version 3
// ==========================================================================
//
//     naturalPaint-panel-layout 3
//     dock left 52
//     dock right 322
//     dock top 46
//     dock bottom 0
//     panel tools left 1.000 0 0 1
//     panel options top 1.000 0 0 1
//     panel color right 1.000 0 1 1
//     panel histogram right 1.000 0 1 0
//     panel layers right 2.000 0 0 1
//     panel history right 1.000 1 0 1
//     ...
//
// `dock <side> <extent-px>` for each of the four; `panel <key> <placement>
// <weight> <collapsed> <stack> <active>` per section, **in the order the
// panels appear within their own placement**. A panel's position in a dock is
// its position among the other `panel` lines that name the same placement --
// the file has no explicit index, because an index is a second source of truth
// that a hand-edit can put in conflict with the line order.
//
// In the example above COLOR and HISTOGRAM share stack `1`, so they are one
// slot with two tabs, and COLOR is the one on top.
//
// ==========================================================================
// Reading a version 2 file
// ==========================================================================
//
// Version 2 was the same grammar without the last two fields:
//
//     panel <key> <placement> <weight> <collapsed>
//
// **Those files still read**, and by the field count rather than by the
// header's version number: a `panel` line with five fields is a version 2 line
// and lands `stack 0, active 1`, which is precisely what it meant -- nothing
// was stacked, because nothing could be. Seven fields is version 3; six or
// eight is malformed and skipped, because half of a `<stack> <active>` pair
// says nothing about what the other half should be. Reading by shape rather
// than by declared version also means a file whose header says 3 but whose
// lines are short still loads, which is the state a hand-edit most easily
// produces.
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
//  * **Incoherent stack** -- repaired, not rejected, because a stack is a
//    relationship between lines and no single line can be blamed for it:
//      - a stack id shared by fewer than two panels **in the same placement**
//        is cleared to 0, since a stack of one is a panel. This is also what
//        happens to a stack whose members a hand-edit has scattered across two
//        docks, and to one whose second member's line was malformed.
//      - a stack with no active member makes its first member active; a stack
//        with several keeps the first of them and clears the rest.
//    Neither repair can fail, so no arrangement of `stack`/`active` values in
//    a file can produce a slot the UI cannot draw -- which matters more here
//    than for the other fields, because an unreachable tab is a panel the user
//    has lost.
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
  // **Tab stacks.** Zero means this panel occupies a dock slot by itself. Any
  // positive value groups it with every other panel in the SAME placement
  // carrying the same value: they share one slot, a tab strip across the top
  // selects between them, and only the active one's body is drawn.
  //
  // An id rather than a "tabbed with the previous entry" flag because a flag
  // makes the grouping a property of the *order*, and the order is something
  // the user reshuffles: dragging a tab out from the middle of a stack would
  // silently re-form the two halves into one, or split a stack the user never
  // touched. An id survives every reordering the UI can perform.
  //
  // The value itself is meaningless -- only equality matters -- and it is
  // scoped to a placement, so the same id in two docks is two unrelated
  // stacks. `PanelLayout` normalises after every mutation: a stack that drops
  // below two members loses its id, because a "stack" of one is a panel.
  int stack = 0;
  // The visible tab of its stack. Exactly one member of each stack has this
  // set, which `PanelLayout` repairs rather than trusts. Meaningless, and
  // left true, for an unstacked panel.
  bool active = true;
};

// The geometry of a slot -- its weight and its collapsed state -- is its FIRST
// member's, not its active tab's. Two reasons, and they point the same way:
// switching tabs must not resize the slot, and a stack collapses as a unit
// (hiding the tab strip along with the body would leave no way back to the
// other members).
//
// One dock slot, which is either a lone panel or a tab stack.
struct PanelSlot {
  // At least one, in tab order.
  std::vector<ControlsSection> members;
  // Index into `members` of the tab whose body is drawn. Always in range.
  size_t activeIndex = 0;
  bool stacked() const noexcept { return members.size() > 1; }
  ControlsSection leader() const noexcept { return members.front(); }
  ControlsSection activeSection() const noexcept { return members[activeIndex]; }
};

// Version 3 adds the two `PanelEntry` fields above. Version 2 files still
// read: a `panel` line with six tokens is a version 2 line, and lands
// unstacked and active, which is exactly what it meant.
inline constexpr int kPanelLayoutFileVersion = 3;
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

  // The same panels grouped into SLOTS -- which is what a dock actually
  // divides, now that several panels can share one. A slot's position in the
  // returned order is the position of its first member in `sectionsIn()`, so
  // stacking two panels does not reshuffle the ones around them.
  //
  // This, not `sectionsIn()`, is what `ui/DockLayout`'s specs are built from:
  // a stack asks the dock for one slot, not one per tab.
  std::vector<PanelSlot> slotsIn(PanelPlacement placement) const;

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

  // --- tab stacks ---------------------------------------------------------

  // The stack id of `section`, or 0 when it stands alone.
  int stackOf(ControlsSection section) const noexcept;
  // The slot `section` belongs to, whether or not it is stacked.
  PanelSlot slotOf(ControlsSection section) const;

  // Puts `moving` into `target`'s slot as a new tab, appended after the
  // existing members, moving it to `target`'s placement if it was elsewhere.
  //
  // Creates the stack when `target` had none. A no-op when the two are already
  // slot-mates, when they are the same panel, or when `target`'s placement is
  // not a dock -- **a flyout shows one panel, and a hidden panel has no slot
  // to share**, so tabs exist only where a slot does.
  //
  // The moved panel becomes the ACTIVE tab: a tab you asked for and cannot see
  // is the same failure as a panel that moves and disappears.
  void stackWith(ControlsSection moving, ControlsSection target);

  // Takes `section` out of its stack into a slot of its own, immediately after
  // the stack it left. A no-op if it was not stacked. If it was the active
  // tab, the stack's first remaining member becomes active.
  void unstack(ControlsSection section);

  // Makes `section` the visible tab of its stack, without reordering the tabs
  // -- a tab strip that reshuffles itself when clicked is one nobody can aim
  // at twice. A no-op for an unstacked panel.
  void setActiveInStack(ControlsSection section);

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
  // Restores the two stack invariants -- at least two members per stack, and
  // exactly one active member per stack -- after any mutation and after any
  // parse. See the header's round-trip repair rules; a drag can reach the same
  // incoherent states a hand-edited file can, so both go through this.
  void normaliseStacks();
  // The lowest stack id unused in `placement`.
  int freeStackId(PanelPlacement placement) const;

  std::vector<PanelEntry> entries_;
  PanelDockExtents docks_;
};

}  // namespace np
