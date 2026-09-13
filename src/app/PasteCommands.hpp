#pragma once

#include <optional>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "core/Clipboard.hpp"

// app/PasteCommands -- PRD M9's other two paste forms. Cmd+V's own Paste is
// core/Clipboard.hpp's `pasteAsLayer()`, already wired in ui/MacPaintUI.cpp
// and untouched here.
//
// Both are session state, like Paste itself: neither is a function of an
// `OpenDocument` alone, so both are `NotRecordable` (app/CommandCoverage.cpp).
namespace np {

struct AppState;

// PRD M9.1: pastes `clip` as a new layer directly above `od`'s active layer,
// centred (whole pixels, PRD D15's exact path) on `od.selection`'s bounds,
// with a layer mask equal to that selection's own coverage.
//
// Refuses, untouched: an empty clipboard, no engaged selection, or a
// selection covering no pixels -- re-checked here rather than trusted from
// the menu's own enable predicate, this codebase's usual rule.
struct PasteIntoResult {
  bool ok = false;
  std::string error;
  size_t layerIndex = 0;
};
PasteIntoResult pasteInto(OpenDocument& od, const Clipboard& clip);

// PRD M9.2, the pure half: a new document sized to `clip`'s own content
// bounds, holding that content as its one layer, shifted to the origin.
// `std::nullopt` for an empty clipboard.
std::optional<OpenDocument> buildDocumentFromClipboard(const Clipboard& clip);

// The whole command. `st.clipboard` wins when non-empty (PRD M8's "internal
// never round-trips the pasteboard"); the OS pasteboard image is tried only
// when it is empty, since creating a document has nothing else to fall back
// to. Opens the new document into `st.documents` on success.
struct PasteAsNewDocumentResult {
  bool ok = false;
  std::string error;
  DocumentId id = 0;
};
PasteAsNewDocumentResult pasteAsNewDocument(AppState& st);

}  // namespace np
