#include "app/CloseDecision.hpp"

#include <string>
#include <utility>

// app/CloseDecision -- implementation. Every design decision is argued in
// CloseDecision.hpp; this file holds the mechanics and comments only where the
// mechanics themselves are not obvious from the header's contract.

namespace np {

CloseAnswer closeAnswerForKey(CloseKey key) noexcept {
  // Written as a switch over the enum rather than a pair of comparisons so
  // that -Wswitch is what catches a third key being added without someone
  // deciding what it means. A key that fell through to a default would take
  // whatever answer happened to be first, and if that were `Save` the mistake
  // would be invisible, while if it were `DontSave` it would be catastrophic.
  switch (key) {
    case CloseKey::Escape:
      return CloseAnswer::Cancel;
    case CloseKey::Enter:
      return CloseAnswer::Save;
  }
  return CloseAnswer::Cancel;
}

bool closeNeedsDecision(const OpenDocument& doc) noexcept { return doc.isDirty(); }

std::string closeQuestion(const OpenDocument& doc) {
  const std::string name = documentDisplayName(doc);
  if (!closeNeedsDecision(doc)) {
    // Not reachable from the dialog, which is only ever raised for a dirty
    // document -- but a caller that asks anyway gets a true sentence rather
    // than one describing unsaved work that does not exist.
    return "'" + name + "' has no unsaved changes.";
  }
  return "'" + name + "' has " + doc.unsavedWorkSummary() +
         ". Save them before closing?";
}

std::optional<size_t> documentIndexById(const DocumentSession& session, DocumentId id) {
  if (id == 0) return std::nullopt;
  for (size_t i = 0; i < session.count(); ++i) {
    const OpenDocument* doc = session.at(i);
    if (doc != nullptr && doc->id == id) return i;
  }
  return std::nullopt;
}

DocumentSaver documentSaverFor(RecentDocuments* recent) {
  return [recent](OpenDocument& doc) -> DocumentOpResult {
    const DocumentOpResult r = saveDocument(doc, {}, recent);
    // The second half of File > Save's own two-step: a successful save has
    // just moved this document to the front of the in-memory MRU, and the
    // list is only useful across launches if it reaches the disk. A failure
    // to write the MRU is deliberately not folded into `r` -- the document
    // was saved, which is what the user asked for, and turning a cosmetic
    // menu-list problem into a "your close failed" message would leave them
    // stuck in a dialog over it.
    if (r.ok && recent != nullptr) {
      std::string saveErr;
      recent->saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    return r;
  };
}

CloseOutcome requestDocumentClose(DocumentSession& session, size_t index,
                                  PendingClose& pending) {
  CloseOutcome out;

  if (pending.active()) {
    // One question at a time. The alternative is a second modal stacked over
    // the first, both asking about documents whose names are behind each
    // other -- and whichever the user answers, they answered it about the
    // dialog they could read, not necessarily the one they clicked.
    out.status = "close refused: '" + pending.name +
                 "' is already waiting for an answer. Answer that first.";
    return out;
  }

  OpenDocument* doc = session.at(index);
  if (doc == nullptr) {
    out.status = "close refused: there is no open document at index " +
                 std::to_string(index) + " (" + std::to_string(session.count()) + " open).";
    return out;
  }

  if (!closeNeedsDecision(*doc)) {
    // The common case, and the whole reason `closeNeedsDecision()` is asked
    // before anything else happens: nothing has been stored, no frame has been
    // spent, and the close is the same single call it always was.
    const std::string name = documentDisplayName(*doc);
    std::string err;
    if (!session.close(index, /*discardUnsavedChanges=*/false, &err)) {
      out.status = err;
      return out;
    }
    out.closed = true;
    out.status = "Closed " + name + ".";
    return out;
  }

  // Dirty. The index is turned into an identity right here, in the same frame
  // as the click, and the index itself is not kept -- see CloseDecision.hpp on
  // why a stored index closes the wrong document.
  pending.document = doc->id;
  pending.name = documentDisplayName(*doc);
  pending.awaitingDestination = false;
  out.questionRaised = true;
  return out;
}

CloseOutcome resolveDocumentClose(DocumentSession& session, PendingClose& pending,
                                  CloseAnswer answer, const DocumentSaver& save) {
  CloseOutcome out;
  if (!pending.active()) return out;  // nothing to answer; not an error

  // Cancel is answered before the document is even looked up. A cancel must
  // work on a question about a document that has already gone, and it must not
  // be able to report `vanished` -- the user asked for nothing to happen, and
  // nothing happening is a success however the session has moved underneath.
  if (answer == CloseAnswer::Cancel) {
    pending.clear();
    return out;
  }

  const std::optional<size_t> index = documentIndexById(session, pending.document);
  if (!index) {
    // The case the identity key exists for. A pending close keyed on an index
    // would have silently closed whatever had shifted into that slot.
    out.vanished = true;
    out.status = "'" + pending.name +
                 "' was closed by something else while the question was open; nothing "
                 "further was done.";
    pending.clear();
    return out;
  }

  OpenDocument* doc = session.at(*index);
  const std::string name = documentDisplayName(*doc);

  if (answer == CloseAnswer::DontSave) {
    std::string err;
    if (!session.close(*index, /*discardUnsavedChanges=*/true, &err)) {
      out.status = err;
      return out;  // the question stays up: the close did not happen
    }
    pending.clear();
    out.closed = true;
    out.status = "Closed " + name + ", discarding its unsaved changes.";
    return out;
  }

  // Save.
  if (!closeNeedsDecision(*doc)) {
    // The document is already clean. Two ways to get here, and both are
    // ordinary: the caller ran its Save As flow while the question was up (see
    // `needsDestination` below), or an undo walked the revision back to the
    // saved one behind the dialog. Writing the file a second time would be an
    // identical write nobody asked for, and on a large document it is a
    // visible pause -- so "save and close" on a document with nothing to save
    // is just a close.
    std::string cleanErr;
    if (!session.close(*index, /*discardUnsavedChanges=*/false, &cleanErr)) {
      out.status = cleanErr;
      return out;
    }
    pending.clear();
    out.closed = true;
    out.status = "Closed " + name + ".";
    return out;
  }

  if (!doc->hasPath()) {
    // There is no native file picker in this build (ui/MacPaintUI.cpp says so
    // at length), so this module cannot choose a destination and must not
    // invent one. The pending close is kept alive and the caller is told to
    // run the Save As flow it already has; answering `Save` again once the
    // document has a path finishes the close.
    pending.awaitingDestination = true;
    out.needsDestination = true;
    out.status = "'" + name + "' has never been saved -- choose a file for it.";
    return out;
  }

  const DocumentOpResult r = save(*doc);
  if (!r.ok) {
    // The writer's own sentence, unchanged. The document stays open, stays
    // dirty and the question stays up -- a user who chose Save and got a
    // failure has not agreed to lose anything.
    pending.awaitingDestination = false;
    out.status = r.error;
    return out;
  }

  std::string err;
  // `discard = false`, deliberately: the save reported success, so the
  // document should be clean, and if it is not then something is wrong and the
  // right answer is to refuse rather than to throw away work on the strength
  // of an assumption.
  if (!session.close(*index, /*discardUnsavedChanges=*/false, &err)) {
    out.status = err;
    return out;
  }
  pending.clear();
  out.closed = true;
  out.status = "Saved " + r.path + " and closed it.";
  return out;
}

}  // namespace np
