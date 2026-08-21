#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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
