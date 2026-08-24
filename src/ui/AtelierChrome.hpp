#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "core/Document.hpp"
#include "ui/AtelierLayout.hpp"

#include "imgui.h"

// ui/AtelierChrome -- the three bands of docs/ui.md section 2 that the
// outgoing MacPaint chrome did not have, and the rules between every band.
//
// What this file is *not*: the panel bodies. LAYERS, HISTORY, COMPS and the
// simulation sections are still `ui/MacPaintUI.cpp`'s, and this module is
// called from it rather than replacing it wholesale. docs/ui.md section 5 says
// `MacPaintUI.*` and `Theme.*` are "replaced, not extended"; the frame and the
// tokens are replaced here, the panel bodies are not, and pretending otherwise
// by renaming the file would be the whole point of the sentence lost.
namespace np {

// -------------------------------------------------------------- pure parts
//
// Everything above the draw functions is free of ImGui so that `--selftest`
// can assert the strings the chrome shows rather than a screenshot being the
// only place they are ever checked.

// PRD **L1** (P0): "The chrome reports the *working space* -- `LIN16` /
// `LIN32` -- never a legacy 8-bit mode."
//
// Derived from the storage, not from a document field: `core::TileStore` holds
// `uint16_t` half-floats and there is one such type in the build, so `LIN32`
// is unreachable today and this returns the one true answer rather than a
// switch over an enum nobody can set. The static_assert in the .cpp is what
// makes that a fact instead of an assumption -- widen the tile and it stops
// compiling, which is the moment this function has to grow a branch.
const char* workingSpaceLabel(const Document& doc) noexcept;

// PRD **L7** (P1): "The status bar reports real resident memory against the
// budget."
//
// The numerator is `app::residentBytes()` -- the real RSS from
// MACH_TASK_BASIC_INFO, the same number `--selftest`'s idle-memory section
// measures, not an accounting of what this process believes it allocated.
struct ResidentReading {
  size_t bytes = 0;
  size_t budget = 0;
};
ResidentReading atelierResident() noexcept;

// The session's resident budget, the denominator above.
//
// **This number is the wireframe's, made explicit.** docs/ui.md section 2
// draws `214 MB / 512 MB` in the status bar and the PRD's L7 says "against the
// budget" without ever naming one; no other requirement fixes a ceiling
// either. PRD A1's 80 MB is the *idle* floor -- what the application costs
// with no document open -- which is a different measurement and not a budget a
// working session could be held to. So this is 512 MB because the design says
// 512 MB, and the honest half of the readout is the numerator.
constexpr size_t kResidentBudgetBytes = 512u * 1024u * 1024u;

// docs/ui.md section 5: "Mirror view and grayscale preview need visible state.
// All three toggles ... change what the canvas shows without changing the
// document, so each active one must be indicated in the status bar. A user who
// forgets grayscale is on will mix colour blind; a user who forgets a mirror
// is on will sign their work backwards."
//
// Returns the markers for whatever is on, joined by " ", or an empty string
// when the view is clean -- the status bar shows nothing at all rather than a
// row of "off" labels, because a marker that is always present is a marker
// nobody reads.
std::string atelierViewStateMarkers(const CanvasView& view);

// The tool's display name. Moved here from `ui/MacPaintUI.cpp`'s anonymous
// namespace because the options bar names the active tool and the palette
// tooltips name every tool, and two copies of a name table is how a tool ends
// up called two things.
const char* toolName(Tool t);

// -------------------------------------------------------- palette metadata
//
// docs/ui.md section 2's ~26/27-cell palette needs, per tool: a name (above),
// whether it does anything (app/AppState.hpp's Tool comment: only the first
// seven do), a Lucide icon to draw, and the keyboard-shortcut letter its
// tooltip shows. One table backs all four rather than four switches that
// could disagree with each other about which tools exist.

// True for exactly the seven Tool values this build has real behaviour for.
// Everything else is a palette cell that exists for its name/icon/slot only,
// per app/AppState.hpp's own comment -- ui/MacPaintUI.cpp's toolButton()
// reads this to decide whether a cell is clickable at all.
bool toolImplemented(Tool t) noexcept;

// The Lucide icon a tool's cell draws: its name (third_party/lucide/, for
// documentation and debugging) and its PUA codepoint
// (third_party/lucide/codepoints.json) -- verified programmatically against
// that file rather than guessed; the commit message and docs/ui.md's
// substitution table say which of these are exact matches and which are the
// closest available substitute, and why. 0 for a tool with no icon (there is
// none today; every row in the table has one).
const char* toolIconName(Tool t) noexcept;
uint32_t toolIconCodepoint(Tool t) noexcept;

// Every codepoint toolIconCodepoint() can return, deduplicated and ascending,
// plus the "More" overflow cell's own ellipsis glyph (that cell is not a
// Tool -- see ui/MacPaintUI.cpp). This is what ui/Fonts's
// installToolIconFont() merges: built by walking every real Tool value, so a
// tool added without an icon shows up as a gap in this list rather than
// silently drawing nothing forever.
const std::vector<uint32_t>& toolIconCodepoints();

// The "..." overflow cell's own Lucide glyph (`ellipsis`). Not a Tool -- see
// ui/MacPaintUI.cpp's palette loop for why it is drawn separately -- but its
// codepoint has to be in toolIconCodepoints() for the same reason every
// tool's does, so it is named here rather than as a magic number at the one
// call site that draws it.
constexpr uint32_t kMoreIconCodepoint = 57526u;  // "ellipsis"

// docs/shortcuts.md section 1's reserved letter for a tool ("B", "Shift+L"),
// or an empty string when no letter is reserved yet. **This is not a
// working shortcut** -- keymaps/default.json does not bind any tool-select
// key today (see main.cpp's key-down dispatch: every binding it resolves is
// a command, never a tool switch), so this is what a tooltip *shows*, not a
// promise that pressing the key does anything. Wiring that dispatch is a
// separate, later change.
std::string toolShortcutLabel(Tool t);

// The tooltip a palette cell shows on hover, matching the design's own
// "Brush Tool  B" -- name, "Tool", and the shortcut letter when one is
// reserved -- with "Not built yet." appended for the twenty cells
// toolImplemented() says are not, so a disabled cell never merely looks
// inert; it says so.
std::string toolTooltip(Tool t);

// ---------------------------------------------------------- tool groups
//
// "nest similar tools into a flyout to conserve space like photoshop" --
// the user's own words, and Photoshop's own grouping is what `kToolGroups`
// below is copied from, mapped onto this build's `Tool` enum: one visible
// palette cell per group, showing whichever member was last used, marked
// with a small corner triangle when the group has more than one member,
// and a flyout (ui/MacPaintUI.cpp's `toolGroupButton()`) listing the rest
// on right-click or press-and-hold. This is metadata, not UI -- the same
// split `kToolMeta` above draws between "what a cell shows" (free of
// ImGui, tested by `--selftest` without a window) and "how it is drawn"
// (ui/MacPaintUI.cpp).
//
// 17 groups, sized to the widest one (the paint group: Brush, Pencil,
// Water, DryBrush) rather than a `std::vector` per group, so the whole
// table is a `constexpr` array like `kToolMeta` -- no heap, no
// initialization order to reason about, and `--selftest` can walk it at
// compile-observed size.
constexpr int kMaxToolGroupMembers = 4;
struct ToolGroup {
  Tool members[kMaxToolGroupMembers];
  int memberCount;
  // A thin rule below this slot, closing one of docs/ui.md section 2's
  // five design groups (selection/sampling, retouch/fill, paint,
  // vector/text, navigation) -- the same four boundaries
  // ui/MacPaintUI.cpp's retired `kPaletteOrder` used to mark per-`Tool`,
  // now marked per-*slot* since nesting changed which `Tool`s share a
  // slot without changing where the design's own rules fall.
  bool ruleAfter;
};

// Display order matches the user's own table exactly (Move+Frame,
// Marquee, Lasso+PolygonLasso, MagicWand, Crop+Slice, Eyedropper+Measure,
// CloneStamp, Eraser, Gradient+PaintBucket, Brush+Pencil+Water+DryBrush,
// Smudge, Dodge+Burn, Pen+Curve, Text, Shape, Hand, Zoom) -- derived from
// Photoshop's real tool groups, not arbitrary, which is why a group of one
// today (MagicWand, CloneStamp, Eraser, Smudge, Text, Shape, Hand, Zoom)
// still gets its own slot rather than being folded into a neighbour: those
// are where not-yet-built variants land once they exist, per the user's
// own instruction to "keep the pairings even where a group currently has
// one member."
constexpr ToolGroup kToolGroups[] = {
    {{Tool::Move, Tool::Frame}, 2, false},
    {{Tool::Marquee, Tool::EllipseMarquee}, 2, false},
    {{Tool::Lasso, Tool::PolygonLasso}, 2, false},
    {{Tool::MagicWand}, 1, false},
    {{Tool::Crop, Tool::Slice}, 2, false},
    {{Tool::Eyedropper, Tool::Measure}, 2, true},
    {{Tool::CloneStamp}, 1, false},
    {{Tool::Eraser}, 1, false},
    {{Tool::Gradient, Tool::PaintBucket}, 2, true},
    {{Tool::Brush, Tool::Pencil, Tool::Water, Tool::DryBrush}, 4, false},
    {{Tool::Smudge}, 1, false},
    {{Tool::Dodge, Tool::Burn}, 2, true},
    {{Tool::Pen, Tool::Curve}, 2, false},
    {{Tool::Text}, 1, false},
    {{Tool::Shape}, 1, true},
    {{Tool::Hand}, 1, false},
    {{Tool::Zoom}, 1, false},
};
constexpr int kToolGroupCount = static_cast<int>(std::size(kToolGroups));

// Which group `t` belongs to, as an index into `kToolGroups`, or -1 if
// none does. `--selftest`'s completeness check (app/selftest/AtelierChrome.cpp)
// is what proves this is never -1 for a real `Tool` -- a tool added to the
// enum but to no group would otherwise be silently unreachable from the
// palette, the exact failure mode a flyout table (as opposed to a switch
// covering `Tool::Count`) can have without `-Wswitch` catching it.
int toolGroupIndex(Tool t) noexcept;

// A group's *fixed* display default: the first `toolImplemented()` member
// if the group has one, else its first member -- independent of any
// runtime "last used" state (that lives on `AppState::toolGroupCurrent`,
// ui/MacPaintUI.cpp), so `--selftest` can assert it from the table alone,
// the same way it already asserts `toolImplemented()` itself without a
// window. Photoshop's own rule: a group opens on whichever tool actually
// does something, not on group[0], so a user's first encounter with (say)
// the paint group shows Brush -- not Pencil, which happens to list first
// in Photoshop's own UI order but has no behaviour in this build yet.
Tool toolGroupDefaultMember(int groupIndex) noexcept;

// -------------------------------------------------------------- draw parts

// A docs/ui.md section 1 token as a packed ImGui colour. The tokens themselves
// live in ui/AtelierTheme.hpp, which has no ImGui dependency on purpose; this
// is the one-line bridge, here rather than there so that header stays testable
// on its own.
ImU32 atelierToken(uint32_t rgb) noexcept;

// docs/ui.md section 1: "`ui-monospace` for all numerics and caps labels".
//
// The distinction the design's type ramp is actually for at 13 px, and the
// reason section 5 gives for it: "Every numeric in the chrome is monospace and
// right-aligned in a fixed-width cell. With live values this is what stops the
// layout juddering as numbers change."
//
// No-ops when no monospace face loaded (ui/Fonts.hpp's `UiFonts::mono`), so
// every call site is unconditional and a machine without SF Mono or Menlo
// loses the distinction rather than crashing on a null font.
void pushAtelierMono();
void popAtelierMono();

// The 2px `#f3f2f2` rules between major regions (docs/ui.md section 1). Drawn
// on the foreground draw list, after every band's window, so that a rule is
// never covered by the window it borders.
void drawAtelierRules(const AtelierBands& bands);

// ------------------------------------------------------------- the split
//
// PRD **A5** (P1): "Documents present as tabs, with an optional split showing
// two." ui/AtelierLayout.hpp owns the geometry and the reading of docs/ui.md's
// two icons; what lives here is the state and the rule that turns a session of
// open documents into *which document each pane shows*.
//
// **One focused pane, and the focused pane always shows the session's active
// document.** That is the decision the rest of the application depends on and
// it is not a detail: every menu, the LAYERS panel, the HISTORY panel and the
// brush act on `DocumentSession::active()`, so a focus that could point
// somewhere else would mean the panels described one document while a pane the
// user had just clicked showed another. Focusing the companion pane therefore
// *makes its document active* and the two swap roles -- the panes do not move
// on screen, the documents in them do.
//
// So the state is small: the arrangement, the companion's id, and which of the
// two panes currently holds the active document.
struct AtelierSplitState {
  AtelierSplit mode = AtelierSplit::Single;
  // The document in the unfocused pane. 0 when there is none, which is every
  // state except an open split with two documents to put in it.
  DocumentId companion = 0;
  // 0 or 1, indexing `AtelierPanes::pane`. Which one holds the active
  // document.
  int focusedPane = 0;
};

// What each pane shows, after `state` has been normalised against the session.
//
// `count` is 1 whenever there is nothing to put in a second pane -- the split
// is off, only one document is open, or none is. `pane[i]` is null only in the
// no-document case.
struct AtelierPaneDocuments {
  OpenDocument* pane[2] = {nullptr, nullptr};
  size_t count = 1;
  int focusedPane = 0;
};

// Resolve `state` against `session`, repairing it in place, and say what each
// pane shows.
//
// The repairs are the cases a session can produce that a click never does:
// a companion that has been closed, a companion that has become the active
// document, a split with one document left in it. **A closed companion is
// replaced rather than emptied** -- with the document before the active one in
// tab order, or the one after it when the active document is first -- because
// the alternative is a split that silently becomes a single pane the moment a
// tab is closed, which reads as a bug rather than as a rule.
//
// The mode is *not* reset when a document is closed. A user who asked for a
// split gets it back when a second document exists again, rather than having
// to ask twice.
AtelierPaneDocuments atelierPaneDocuments(DocumentSession& session, AtelierSplitState& state);

// docs/ui.md section 2's 34px band: the open documents as tabs (PRD **A5**),
// and at its right edge the two split icons that section 5 asks for.
//
// `statusOut`, when non-null, receives the refusal from a close the session
// declined -- a dirty document names what would be lost (PRD I11), exactly as
// the File menu's own Close Document does, because it is the same call.
//
// `split` is read for the icons' pressed state and written when one is
// clicked. Clicking the *active* arrangement's icon returns to a single pane,
// so the pair are three states between them and the way out is the way in.
//
// Returns true when a new document was asked for (the `+`), which the caller
// makes, since only it knows the canvas dimensions a blank one gets.
bool drawAtelierTabStrip(AppState& st, const AtelierBands& bands, AtelierSplitState& split,
                         std::string* statusOut);

// docs/ui.md section 2's 46px band: the active tool and its options.
//
// No PRESET control: the design draws `PRESET [] Round Bristle 03`, and this
// build has no brush presets at all. A dropdown showing one invented name
// would put a feature in the chrome that does not exist behind it.
// `refusal` is the sentence the brush could not paint, or empty. Shown here
// rather than logged: a locked target makes the brush silently stop working,
// which is the one failure a painter cannot diagnose by looking at the canvas.
void drawAtelierOptionsBar(AppState& st, const AtelierBands& bands, const std::string& refusal);

// docs/ui.md section 2's 26px band: zoom, dimensions and working space,
// resident against budget, and the view-state markers.
void drawAtelierStatusBar(AppState& st, const AtelierBands& bands, uint32_t canvasW,
                          uint32_t canvasH);

}  // namespace np
