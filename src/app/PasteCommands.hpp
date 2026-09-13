#pragma once

#include <optional>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "core/Clipboard.hpp"

// app/PasteCommands -- PRD M9's other two paste forms (track `paste`;
// docs/reach-paste.md). Cmd+V's own Paste is core/Clipboard.hpp's
// `pasteAsLayer()`, already wired in ui/MacPaintUI.cpp and untouched here.
//
// Both commands are session state exactly as Paste already is
// (docs/automation.md): Paste Into reads the active selection and the
// clipboard, Paste as New Document reads the clipboard and, failing that,
// the OS pasteboard -- neither is a function of an `OpenDocument`, so both
// are `NotRecordable` (app/CommandCoverage.cpp), the identical classification
// Paste itself already carries.
namespace np {

struct AppState;

// PRD M9.1: pastes `clip` as a new layer directly above `od`'s active layer,
// its tiles centred (whole pixels, so the move stays on PRD D15's lossless
// exact-translate path) on `od.selection`'s bounds, with a layer mask equal
// to that selection's own coverage.
//
// Refuses, by name and touching nothing: an empty clipboard; no engaged
// selection; a selection engaged but covering no pixels. These mirror the
// Edit menu's own enable predicate (ui/MenuModel.cpp's `PasteInto` row), so a
// click that reaches this function at all should not normally refuse -- it
// is checked again here because a refusal from underneath is always the true
// guard, this codebase's own convention (app/TransformSession.hpp section 3
// and others make the same point).
struct PasteIntoResult {
  bool ok = false;
  std::string error;
  size_t layerIndex = 0;
};
PasteIntoResult pasteInto(OpenDocument& od, const Clipboard& clip);

// PRD M9.2, the pure half: a new document sized to `clip`'s own content
// bounds, holding that content as its ONE layer, shifted so the content's
// own top-left lands at the document's origin. `std::nullopt` for an empty
// clipboard. No OS pasteboard, no `AppState` -- independently testable, and
// the internal-clipboard branch `pasteAsNewDocument()` below builds on.
std::optional<OpenDocument> buildDocumentFromClipboard(const Clipboard& clip);

// The whole command. `st.clipboard` (the internal path) wins when it holds
// anything, matching PRD M8's "internal never round-trips the pasteboard"
// rule for ordinary Paste; the OS pasteboard image is tried only when the
// internal clipboard is EMPTY, which is the one case ordinary Paste has
// never had to answer (it simply pastes nothing there) and this command
// cannot get away with, because "create a document" has no other source to
// fall back to. Opens the new document into `st.documents` on success.
struct PasteAsNewDocumentResult {
  bool ok = false;
  std::string error;
  DocumentId id = 0;
};
PasteAsNewDocumentResult pasteAsNewDocument(AppState& st);

}  // namespace np
