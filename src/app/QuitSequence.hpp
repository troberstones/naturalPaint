#pragma once
#include <string>
#include <vector>

#include "app/CloseDecision.hpp"
#include "app/DocumentLifecycle.hpp"

// app/QuitSequence -- quitting with unsaved work open.
//
// --- The defect this module repairs ----------------------------------------
//
// `main.cpp` ran `while (!st.quit)` and four places set `st.quit = true`: the
// `SDL_EVENT_QUIT` that Cmd-Q and a signal produce, the window's close button
// (`SDL_EVENT_WINDOW_CLOSE_REQUESTED`), the keymap's `"quit"` action, and File
// > Quit. **None of them looked at a document.** `DocumentSession::close()` was
// never called on the way out, so the refusal PRD I11 exists for -- and the
// Save / Don't Save / Cancel question app/CloseDecision built on top of it --
// were both bypassed completely by the one exit every user takes.
//
// So a session with three painted, unsaved documents closed on one keystroke
// and said nothing. That is worse than the silent close box app/CloseDecision
// was written against, because a close box only ever discards one document and
// this discarded all of them; and it is worse than a crash, because the
// recovery journal's whole contract is that a *clean* shutdown removes the
// scratch directory (PRD O8) -- so the exit that threw the work away was also
// the exit that deleted the copy of it.
//
// --- Why the sequence is a module, and not a loop in the event handler -----
//
// Because a quit is not one decision, it is N of them, and the N answers
// arrive on N different frames.
//
// app/CloseDecision.hpp's author wrote that "a correct quit flow needs to ask
// about *each* dirty document in turn", and shaped `PendingClose` to carry it:
// one question at a time, keyed on `DocumentId`, answered by whichever dialog
// is up. This module is the thing that holds the *queue* between those
// answers. It reuses `requestDocumentClose()` and `resolveDocumentClose()`
// verbatim rather than reimplementing the three-way answer, which is what
// makes "Save / Don't Save / Cancel means the same thing on the way out as it
// does on a tab" true by construction -- including Escape = Cancel, Enter =
// Save, and no key at all mapping to Don't Save.
//
// Being ImGui-free and `AppState`-free, `--selftest` asserts the whole
// sequence headlessly: how many questions, in what order, about which
// documents, and what each answer does to the session.
//
// --- Identity, again, and for a sharper reason than before -----------------
//
// The queue holds `DocumentId`s, never indices. app/CloseDecision.hpp already
// argues why a *pending* close cannot hold an index; a quit queue makes the
// hazard structural rather than occasional, because **the sequence closes
// documents as it goes**. Answering the first question renumbers everything
// behind it, so a queue of indices would be wrong on its second entry every
// single time -- not in a race, but always.
//
// --- Closing as we go, and the "Don't Save then Cancel" question -----------
//
// Each answered document is closed there and then, through
// `resolveDocumentClose()`. The alternative -- collect all N answers, then act
// -- would make Cancel perfectly restorative, and it was rejected:
//
//  * **It cannot actually be atomic.** Save is one of the three answers, and a
//    file that has been written cannot be un-written. A sequence that
//    pretended to be all-or-nothing would be lying about the half of it that
//    touches the disk.
//  * It would need a second, parallel notion of "closed but not yet closed"
//    for the tab strip, the journal and the layers panel to disagree about.
//
// So: **Don't Save on document 1 followed by Cancel on document 2 does NOT
// re-open document 1.** The argument for reopening is that the user only meant
// to discard it *as part of* quitting, and the quit did not happen. The
// argument against, which wins:
//
//  * The user was asked one direct question about one named document, with its
//    unsaved work spelled out, and answered it. Reversing that answer because
//    of something they said about a *different* document treats a considered
//    click as provisional.
//  * There is nothing to re-open to. Don't Save discarded the work; restoring
//    an empty shell of the document under its old name would be worse than not
//    restoring it -- it would look like the work was still there.
//  * Cancel's promise is "stop asking me", and it is kept exactly: the
//    remaining questions are dropped, the remaining documents are untouched,
//    and the application does not exit.
//
// What Cancel must never do is leave the user mid-quit, and it does not: the
// sequence is cleared, so the next Cmd-Q starts over from the documents that
// are actually still open.
//
// --- Why this cannot hang `--screenshot` -----------------------------------
//
// `--screenshot <path>` uses `st.quit = true` as its capture-and-exit
// mechanism, and `tools/golden/run_golden.sh` drives the application that way.
// A dirty-check bolted onto `st.quit` would block the golden harness on a
// modal it has no way to answer, forever.
//
// The guard is therefore keyed on a **different flag**. `AppState::requestQuit`
// is what a *user* asking to leave sets; `AppState::quit` remains the plain
// "the loop stops now" boolean it always was, and the screenshot path still
// writes it directly. Nothing in this module reads or writes `quit`, and
// nothing in the screenshot path writes `requestQuit`, so there is no
// expression through which one can reach the other. On top of that, main.cpp
// services `requestQuit` by exiting immediately whenever `--screenshot` was
// passed -- which matters for the one case that is not hypothetical: SDL turns
// SIGINT and SIGTERM into `SDL_EVENT_QUIT`, so a harness that times out and
// signals the process must still be able to kill it.
namespace np {

// The documents a quit has still to ask about, front first, by identity.
//
// Lives on `AppState` for the same reason `PendingClose` does: it is raised in
// main.cpp's event loop and advanced from ui/MacPaintUI.cpp's dialog, so a
// function-local static in either would be invisible to the other half.
struct QuitSequence {
  // Includes the document currently being asked about, at the front. Popped
  // only when its question has been answered and acted on.
  std::vector<DocumentId> remaining;

  // False for every frame of a normal session. A quit is in flight only
  // between the keystroke and the last answer.
  bool running = false;

  void clear() noexcept {
    remaining.clear();
    running = false;
  }
};

// What one step of the sequence did. All four flags are false for the steps
// that merely moved to the next question, which is the common case.
struct QuitStep {
  // The quit is finished and there is nothing left to ask: the caller should
  // stop the frame loop. This is what sets `AppState::quit`, and it is the only
  // thing in this module that ever leads to it.
  bool exitNow = false;

  // A question is now up; `pending` names it and the dialog will draw it.
  bool asking = false;

  // The quit was called off -- by Cancel, by a save that failed, or by a
  // refusal. Nothing further will be asked and the application does not exit.
  bool abandoned = false;

  // Save was chosen for a document that has never been saved. The caller must
  // run its Save As flow, exactly as it does for a single-document close; the
  // sequence stays exactly where it is.
  bool needsDestination = false;

  // One sentence for the status line, or empty. Never invented here when a
  // lower layer already produced one -- a failed save carries the writer's own
  // error, unchanged.
  std::string status;
};

// Start a quit.
//
// **With no dirty documents this returns `exitNow` immediately**, having
// touched neither `seq` nor `pending`: no dialog, no queue, no extra frame.
// That is what almost every quit is, and a safety feature that made it slower
// would be a safety feature users learn to route around.
//
// With dirty documents, the queue is filled with their ids **in session
// order** -- index 0 first, which is left-to-right along the tab strip, the
// order the user sees them in -- and the first question is raised. Clean
// documents are never enqueued, so they are never asked about and, since the
// process is about to exit, never closed either.
//
// Refused, with a sentence and no other effect, if a quit is already running
// or if some other close question is already waiting for an answer. Two
// stacked modals about two documents is a dialog nobody can answer correctly,
// which is `requestDocumentClose()`'s own rule; this is the same rule one
// level up.
QuitStep beginQuit(DocumentSession& session, QuitSequence& seq, PendingClose& pending);

// Answer the question that is up.
//
// **Call this for every answer, whether or not a quit is running.** With no
// quit in flight it is exactly `resolveDocumentClose()` and returns an
// otherwise-empty step -- which is the point: the dialog has four exits (three
// buttons, two keys, a vanished document and a dismissal), and one of them
// forgetting to advance the sequence would strand a quit forever. `outcomeOut`,
// when non-null, receives the underlying `CloseOutcome` so the caller can react
// to `needsDestination` the way it already does.
//
// With a quit running:
//
//  * **Cancel abandons the whole quit.** The remaining documents are not asked
//    about. The user said stop; continuing to badger them about the other four
//    documents is not what "cancel" means anywhere else.
//  * **Save / Don't Save** close that document and the sequence moves to the
//    next, or reports `exitNow` when the queue empties.
//  * **A save that FAILED abandons the quit**, and deliberately leaves the
//    question up carrying the writer's own error: the error has to be readable
//    somewhere, and the dialog is the only place in this application that shows
//    one without it being a line of dim grey beside the menus. That document
//    stays open and stays dirty. Answering the question again now closes just
//    that one document, because the quit is no longer running.
//  * A document that vanished from under the sequence is skipped, not acted
//    on -- the whole reason the queue is keyed on identity.
QuitStep answerQuitQuestion(DocumentSession& session, PendingClose& pending, QuitSequence& seq,
                            CloseAnswer answer, const DocumentSaver& save,
                            CloseOutcome* outcomeOut = nullptr);

// Call off a running quit, for a reason the caller knows about and this module
// does not -- today, the user backing out of the Save As dialog a
// `needsDestination` answer opened.
//
// A no-op returning an empty step when no quit is running, so a caller does not
// have to guard it and cannot report an abandonment that never happened.
QuitStep abandonQuit(QuitSequence& seq, std::string why);

}  // namespace np
