#include "app/QuitSequence.hpp"

#include <algorithm>
#include <optional>
#include <utility>

// app/QuitSequence -- implementation. Every design decision is argued in
// QuitSequence.hpp; this file holds the mechanics and comments only where the
// mechanics are not obvious from the header's contract.

namespace np {

namespace {

// Raise the next question, skipping anything that no longer needs one.
//
// Two kinds of skip, both ordinary rather than exceptional:
//
//  * the document has gone (something else closed it while an earlier question
//    was up), so there is nobody to ask about; and
//  * the document is no longer dirty -- an undo behind the dialog walked the
//    revision back to the saved one. It is **not** closed on the way past: a
//    clean document has nothing to lose, and the process is about to exit
//    anyway, so closing it would be work done for no one.
//
// Returns `exitNow` when the queue empties, which is the only place in this
// module that decides a quit is finished.
QuitStep pumpQuitSequence(DocumentSession& session, QuitSequence& seq, PendingClose& pending) {
  QuitStep step;
  while (!seq.remaining.empty()) {
    const DocumentId id = seq.remaining.front();
    const std::optional<size_t> index = documentIndexById(session, id);
    if (!index) {
      seq.remaining.erase(seq.remaining.begin());
      continue;
    }
    OpenDocument* doc = session.at(*index);
    if (doc == nullptr || !closeNeedsDecision(*doc)) {
      seq.remaining.erase(seq.remaining.begin());
      continue;
    }

    // The index is derived here, one line before it is used, and never stored
    // -- see QuitSequence.hpp on why a queue of indices is wrong on its second
    // entry every time.
    const CloseOutcome out = requestDocumentClose(session, *index, pending);
    if (out.questionRaised) {
      step.asking = true;
      return step;
    }

    // Unreachable in practice: the document was just confirmed to exist and to
    // be dirty, and no question can be pending here, so `requestDocumentClose()`
    // has nothing left to refuse for. Handled rather than asserted because the
    // alternative to handling it is a quit that silently stalls with no
    // question on screen and no way to make progress.
    seq.clear();
    step.abandoned = true;
    step.status = out.status.empty()
                      ? std::string("Quit abandoned: a document could not be asked about.")
                      : out.status;
    return step;
  }

  seq.clear();
  step.exitNow = true;
  return step;
}

}  // namespace

QuitStep beginQuit(DocumentSession& session, QuitSequence& seq, PendingClose& pending) {
  QuitStep step;

  if (seq.running) {
    // A second Cmd-Q while the first is still being answered. Ignored rather
    // than restarted: restarting would re-enqueue documents the user has
    // already answered about.
    step.status = "Quit is already in progress -- answer the question that is open.";
    return step;
  }
  if (pending.active()) {
    step.status = "Quit refused: '" + pending.name +
                  "' is already waiting for an answer. Answer that first.";
    return step;
  }

  // The fast path, and it must stay first: nothing is allocated, nothing is
  // stored, and `seq` and `pending` are not touched at all when there is
  // nothing to lose.
  for (size_t i = 0; i < session.count(); ++i) {
    const OpenDocument* doc = session.at(i);
    if (doc != nullptr && closeNeedsDecision(*doc)) seq.remaining.push_back(doc->id);
  }
  if (seq.remaining.empty()) {
    step.exitNow = true;
    return step;
  }

  seq.running = true;
  return pumpQuitSequence(session, seq, pending);
}

QuitStep answerQuitQuestion(DocumentSession& session, PendingClose& pending, QuitSequence& seq,
                            CloseAnswer answer, const DocumentSaver& save,
                            CloseOutcome* outcomeOut) {
  // Read before the answer: `resolveDocumentClose()` clears `pending` on its
  // way out, so afterwards there is nothing left to say which document was
  // being asked about. This is the value the queue is popped by -- by identity,
  // not by position, even though it is the front entry, so that a queue that
  // somehow drifted out of step drops the document that was actually answered.
  const DocumentId asked = pending.document;

  const CloseOutcome outcome = resolveDocumentClose(session, pending, answer, save);
  if (outcomeOut != nullptr) *outcomeOut = outcome;

  QuitStep step;
  step.status = outcome.status;
  if (!seq.running) return step;  // an ordinary close; nothing to sequence

  if (answer == CloseAnswer::Cancel) {
    seq.clear();
    step.abandoned = true;
    // Not `outcome.status`: a cancel produces none, and the user who pressed it
    // is owed confirmation that the whole quit stopped rather than just this
    // one question.
    step.status = "Quit cancelled -- nothing else was closed.";
    return step;
  }

  if (outcome.needsDestination) {
    // The caller runs its Save As flow and answers `Save` again. The queue does
    // not move, and neither does `pending` -- see app/CloseDecision.hpp's
    // `awaitingDestination`.
    step.needsDestination = true;
    return step;
  }

  if (outcome.closed || outcome.vanished) {
    if (asked != 0)
      seq.remaining.erase(std::remove(seq.remaining.begin(), seq.remaining.end(), asked),
                          seq.remaining.end());
    QuitStep next = pumpQuitSequence(session, seq, pending);
    // The close's own sentence survives when the next step has nothing to say,
    // so "Closed X, discarding its unsaved changes." is not swallowed by the
    // machinery that moved on to Y.
    if (next.status.empty()) next.status = std::move(step.status);
    return next;
  }

  // Neither closed, nor handed off, nor cancelled: the answer could not be
  // carried out. The only way to get here today is a save that failed, and the
  // requirement is explicit -- a failed save abandons the quit rather than
  // moving on to the next document, because moving on would march past the one
  // document whose work the user just asked to keep.
  //
  // `pending` is deliberately left as `resolveDocumentClose()` left it, which
  // for a failed save is still asking, carrying the writer's error. See
  // QuitSequence.hpp.
  seq.clear();
  step.abandoned = true;
  if (step.status.empty()) step.status = "Quit abandoned: the answer could not be carried out.";
  return step;
}

QuitStep abandonQuit(QuitSequence& seq, std::string why) {
  QuitStep step;
  if (!seq.running) return step;
  seq.clear();
  step.abandoned = true;
  step.status = std::move(why);
  return step;
}

}  // namespace np
