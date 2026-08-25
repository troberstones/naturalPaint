#pragma once
#include <functional>
#include <optional>
#include <string>

#include "app/DocumentLifecycle.hpp"

// app/CloseDecision -- what closing a document does when that document holds
// unsaved work.
//
// --- The defect this module repairs ----------------------------------------
//
// `DocumentSession::close()` refuses a dirty document and writes a sentence
// naming exactly what would be lost (PRD I11). Both close paths -- the tab
// strip's `x` and File > Close Document -- called it with
// `discardUnsavedChanges = false` and dropped the refusal into `g_docStatus`,
// which surfaces as a run of dim disabled text beside the menus. So clicking
// the close box on a tab you had painted on produced no visible change at all,
// and the user's report was the accurate one: "clicking on the x for a
// document tab has no effect."
//
// That outcome is worse than either of the two the refusal was choosing
// between. A user who is told nothing concludes the control is broken, and a
// control believed broken is one nobody presses again -- including on the
// clean document where it would have worked. A silent refusal teaches the
// wrong lesson about the whole tab strip.
//
// PRD I11 is not what needed relaxing. "A save that would lose data names
// exactly what" stays, word for word: the sentence this module shows is
// `unsavedWorkSummary()`'s, the same one the refusal carried. What changes is
// that naming the work is now a **question** rather than a dead end. Save /
// Don't Save / Cancel is what every editor asks, and it is the only shape that
// lets the user say which of the two things they actually meant.
//
// --- Why a module, rather than a popup in the tab strip ---------------------
//
// Three reasons, in the order they bite.
//
//  1. **There are two close paths and they must not diverge.** The tab strip
//     lives in `ui/AtelierChrome.cpp` and File > Close Document in
//     `ui/MacPaintUI.cpp`. A dialog written into one of them is a dialog the
//     other does not have, and "the menu discards without asking" is precisely
//     the data-loss bug PRD I11 exists to prevent. Both now call
//     `requestDocumentClose()` and neither owns the rule.
//  2. **The question outlives the click.** A dialog answers on a later frame,
//     and the tab strip is redrawn from scratch every frame in between. The
//     state that survives those frames cannot be a local in the drawing loop.
//  3. **The decision is testable and the drawing is not.** Everything here is
//     free of ImGui, so `--selftest` asserts what each answer does to the
//     session rather than a screenshot being the only witness.
//
// --- Index or identity: identity, and the reason is not stylistic -----------
//
// A close request arrives as an *index* -- the tab that was clicked, or
// `activeIndex()` from the menu. An index is the correct input, because a
// click identifies a position on screen and nothing else. It is the wrong
// thing to **remember**.
//
// `DocumentSession::close()` erases from the middle of its vector, so closing
// document 1 of three moves what was document 2 down to index 1. Between the
// click that raises the question and the answer that resolves it, any number
// of frames pass, and in those frames the File menu, the recovery offer or
// another tab's close box can remove a document. A pending close keyed on the
// index 1 would then close *the wrong document*, silently, discarding work the
// user never said to discard -- and it would do so most often in exactly the
// situation the dialog exists for, because the user who is being asked about
// one dirty document is the user with several documents open.
//
// So `PendingClose` holds a `DocumentId`. That identity is monotonically
// allocated and never reused within a run (app/DocumentLifecycle.hpp), which
// makes the failure mode fail *safely*: an id whose document has gone is not
// some other document's id, it is no document's, and `resolveDocumentClose()`
// reports `vanished` and does nothing. The index is converted to an id inside
// `requestDocumentClose()`, in the same frame as the click, and is never
// stored.
//
// The cost is one linear scan of the session per answer, over a list that is
// at most a few dozen entries and is walked once per button press. That is not
// a cost worth a second lookup table.
namespace np {

// --- The question -----------------------------------------------------------

// The three-way answer. There is no fourth: "save a copy and close" and "save
// elsewhere and close" are Save As followed by Save, and offering them here
// would put two more buttons in front of a user who is trying to leave.
enum class CloseAnswer { Save, DontSave, Cancel };

// The keys the dialog binds, and the only two it binds.
enum class CloseKey { Escape, Enter };

// Escape means Cancel and Enter means Save. **No key maps to `DontSave`**, and
// that omission is the requirement rather than an oversight:
//
//  * Escape is what a user presses to make a dialog they did not expect go
//    away, so it must be the answer that changes nothing.
//  * Enter is what a user presses to get past a dialog they did expect, and
//    the two candidates for it are Save and Don't Save. Save is the one whose
//    worst case is a file written that the user would have discarded. Don't
//    Save's worst case is a morning's painting gone. Those are not comparable,
//    so Enter is Save.
//  * Discarding is therefore reachable only by aiming at a button and pressing
//    it. A dialog that appears under a held Return key cannot destroy anything
//    however long the key is held.
//
// `--selftest` asserts both mappings by name, because a build in which they
// were swapped would look and feel entirely normal right up until the first
// time it ate someone's work.
CloseAnswer closeAnswerForKey(CloseKey key) noexcept;

// True when closing `doc` has to ask -- which is exactly "it is dirty".
//
// A separate name rather than a call to `isDirty()` at each site, so the two
// close paths ask one question with one answer, and so the clean-document fast
// path is a thing that can be pointed at: a clean document closes on one
// click, with no dialog, no confirmation and no extra frame. Making the common
// case slower is how a safety feature becomes the thing users route around.
bool closeNeedsDecision(const OpenDocument& doc) noexcept;

// The sentence the dialog shows. Names the document, then names the work, then
// asks.
//
// It names the document because the tab strip's close boxes are 14 px apart
// and a user closing one of six tabs must be able to see which one they hit --
// a dialog that says only "You have unsaved changes" is a dialog that gets
// answered about the wrong document.
//
// The middle clause is `unsavedWorkSummary()` verbatim, which is PRD I11's own
// "names exactly what". There is deliberately no second sentence composed here
// to drift from it, exactly as `drawDocumentDialogs()`'s Revert popup shows
// `revertDocument()`'s refusal rather than a paraphrase.
std::string closeQuestion(const OpenDocument& doc);

// The close that has been asked about and not yet answered. Zero-initialised
// means "nothing pending", which is the state for all but a handful of frames
// in a session.
//
// Lives on `AppState` (session state by that header's own rule -- it belongs
// to the process and names a document, not to a widget), and it has to: the
// tab strip raises it from one translation unit and the dialog answers it from
// another, so a function-local static would be invisible to half of the
// feature.
struct PendingClose {
  // The document the question is about. 0 when there is no question up.
  DocumentId document = 0;

  // Its display name as it was when the question was raised.
  //
  // **Not what the dialog draws** -- that reads the live record through
  // `closeQuestion()`, so a Save As performed while the question is up renames
  // the dialog too. This copy exists for the one moment the live record is
  // gone: the status line after the document has been closed, or after
  // something else closed it, still has to say which document it is talking
  // about.
  std::string name;

  // Set when Save was chosen for a document that has never been saved. The
  // question stays pending across the caller's Save As flow, and answering
  // Save again once a path exists finishes the close.
  bool awaitingDestination = false;

  bool active() const noexcept { return document != 0; }
  // True when the three-way dialog should be on screen. False while the caller
  // is running its own Save As flow on our behalf, so two modals never stack.
  bool asking() const noexcept { return document != 0 && !awaitingDestination; }

  void clear() noexcept {
    document = 0;
    name.clear();
    awaitingDestination = false;
  }
};

// What a request or an answer did. One struct for both entry points, since a
// caller wants to react to the same four facts either way.
struct CloseOutcome {
  // The document is gone from the session -- exactly one document, and exactly
  // the one that was asked about.
  bool closed = false;

  // The question has been raised and `pending` now names it. The document is
  // still open and entirely untouched.
  bool questionRaised = false;

  // Save was chosen for a document with no file to write. Nothing was saved,
  // nothing was closed, and the pending close is still live: the caller must
  // run its Save As flow and then answer `Save` again.
  bool needsDestination = false;

  // The document the question named is no longer in the session -- something
  // else closed it while the question was up. Nothing was done, and the
  // pending close has been cleared.
  bool vanished = false;

  // One sentence for the status line, or empty. Never invented here when a
  // lower layer already produced one: a failed save carries `saveDocument()`'s
  // own error, unchanged.
  std::string status;
};

// --- Doing it ---------------------------------------------------------------

// Where the document with `id` currently sits, or nothing.
//
// `DocumentSession::find()` answers with a pointer, which is what a caller
// that wants to *read* the document needs; `close()` takes an index, so the
// pending close needs this second form. Here rather than on `DocumentSession`
// because it is this module's problem: nothing else in the application keeps
// an identity across frames and then needs a position back.
std::optional<size_t> documentIndexById(const DocumentSession& session, DocumentId id);

// How the close path saves. Injected rather than called directly for one
// reason and not the usual one: `--selftest` must assert what each answer does
// to the session in **both** NP_USE_OIIO configurations (PLAN.md 1.5), and the
// real save writes a `.npaint`, which the OFF build refuses by design. A stub
// saver lets the decision be asserted headlessly in either build.
//
// The application never writes its own: `documentSaverFor()` below is the one
// production instance, and it is the File menu's own call.
using DocumentSaver = std::function<DocumentOpResult(OpenDocument&)>;

// The saver the running application uses.
//
// This is `saveDocument()` -- the same function File > Save calls, with the
// same empty `NpaintSaveOptions` and the same recent list -- followed by the
// same persistence of that list. It is a function rather than a lambda written
// out at each of the two close paths precisely so there is no second save
// implementation to drift: the whole requirement is that Save from the close
// dialog and Save from the menu are one path.
//
// `recent` may be null, which is what a caller with no MRU (a test, a future
// headless driver) passes.
DocumentSaver documentSaverFor(RecentDocuments* recent);

// Ask to close the document at `index`.
//
// **A clean document closes here and now**, on this call, in the same frame as
// the click, and `pending` is not touched: `outcome.closed` is true and no
// dialog was ever raised. That is the case nearly every close is, and it costs
// nothing more than it did before.
//
// A dirty document raises the question: `pending` is filled in,
// `outcome.questionRaised` is true, and **the document is still open and
// completely unchanged**. The refusal has become a question; it has not become
// a close.
//
// An out-of-range index, or a request made while another question is already
// up, is refused with a sentence and changes nothing. One question at a time
// is not a limitation -- two overlapping modals about two documents is a
// dialog a user cannot answer correctly.
CloseOutcome requestDocumentClose(DocumentSession& session, size_t index,
                                  PendingClose& pending);

// Answer the pending question.
//
// * **Cancel** clears the pending close and does nothing else. The document
//   count, the document's dirty state, its unsaved-edit labels and the
//   session's active index are all exactly what they were.
// * **Don't Save** closes and discards, through `close(..., discard = true)` --
//   the one call in the application that passes true for a document the user
//   is looking at, and it is reached only by pressing that button.
// * **Save** writes through `save`, and closes **only if the write reported
//   ok**. A save that fails -- a full disk, a read-only directory, the whole
//   NP_USE_OIIO=OFF refusal -- leaves the document open, still dirty, with the
//   question still up and the writer's own error in `status`. Closing anyway
//   would be the single worst thing this module could do: the user asked to
//   keep the work and would lose it to a message they had no chance to read.
//   The close then uses `discard = false`, so a document that somehow stayed
//   dirty through a successful save is refused rather than quietly discarded.
// * **Save on a document that is already clean** is a plain close, with no
//   write at all. That is not a corner case: it is how the caller's Save As
//   flow finishes a close that was waiting for a file name.
// * **Save on a document that has never been saved** cannot pick a file. See
//   `needsDestination`.
//
// If the document has gone in the meantime, `vanished` is set and nothing is
// closed. This is the case the identity key exists for.
CloseOutcome resolveDocumentClose(DocumentSession& session, PendingClose& pending,
                                  CloseAnswer answer, const DocumentSaver& save);

}  // namespace np
