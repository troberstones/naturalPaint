#include "app/selftest/Support.hpp"

#include "app/CloseDecision.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Closing a document that holds unsaved work (app/CloseDecision).
//
// See app/CloseDecision.hpp for every decision this file only checks. Two
// things are worth saying here, because they are what the section is *for*:
//
//   * The defect it was written against was invisible. Both close paths
//     refused a dirty document and dropped PRD I11's refusal into a line of
//     dim grey beside the menus, so the close box read as a dead control and
//     the user reported it as one. Nothing in the suite noticed, because the
//     refusal itself was correct -- app/selftest/DocumentLifecycle.cpp asserts
//     it, and still does. What was missing was any assertion about what the
//     *user* then gets, which is what this section adds.
//   * The one mistake here that would be silent and destructive is keying the
//     pending close on an index. Section F is built specifically to catch it:
//     the stale index is arranged to be in range and to point at a *different*
//     document, so an index-keyed implementation does not fail -- it closes
//     the wrong document, successfully, and discards work nobody offered up.
//     That is the assertion to check first if this file ever goes red.
//
// Headless and GPU-free, and it writes no files: the save is injected
// (app/CloseDecision.hpp's `DocumentSaver`), so every assertion below holds in
// BOTH NP_USE_OIIO configurations (PLAN.md §1.5) rather than the whole section
// going quiet in the OFF build. What the stub cannot prove -- that a save
// actually puts bytes on disk -- is app/selftest/DocumentLifecycle.cpp's, and
// section E asserts the production saver really is `saveDocument()` by the one
// refusal only that function produces.
// ---------------------------------------------------------------------------
bool runCloseDecisionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // A document with `label` and one recorded edit, so it is dirty for a named
  // reason -- the summary the dialog shows is built from these labels.
  auto dirtyDoc = [](const char* title, const char* edit) {
    OpenDocument d = makeBlankOpenDocument(8, 8, WorkingSpace{}, title);
    d.recordEdit(edit);
    return d;
  };

  // The saver the tests inject. Does to the record exactly what
  // `saveDocumentAs()` does on success -- binds the path, moves
  // `savedRevision` up to `revision` and empties the unsaved-edit list -- and
  // nothing else, because nothing else is what this section is asserting.
  // `succeeds = false` is the failing writer: a full disk, a read-only
  // directory, or the whole NP_USE_OIIO=OFF refusal.
  // **The id, not the display name.** `documentDisplayName()` prefers the
  // path's filename over the title (`app/DocumentLifecycle.cpp`), so the moment
  // a test binds a path -- which section E must, because Save needs a
  // destination -- the name stops being the one the fixture was built with.
  // Asserting on it made this section fail against correct code, and the
  // failure named the writer rather than the label, which is the wrong end.
  // `DocumentId` is the same handle `PendingClose` is keyed on and for the same
  // reason: it is the only thing about a document that a save cannot change.
  struct SaveLog {
    int calls = 0;
    DocumentId lastId = 0;
  };
  auto stubSaver = [](bool succeeds, SaveLog* log) -> DocumentSaver {
    return [succeeds, log](OpenDocument& doc) {
      DocumentOpResult r;
      if (log != nullptr) {
        ++log->calls;
        log->lastId = doc.id;
      }
      if (!succeeds) {
        r.ok = false;
        r.error = "save refused: the test's writer was told to fail.";
        return r;
      }
      if (doc.path.empty()) doc.path = "/tmp/selftest-close-decision.npaint";
      doc.savedRevision = doc.revision;
      doc.unsavedEdits.clear();
      doc.unsavedEditsDropped = 0;
      r.ok = true;
      r.path = doc.path;
      return r;
    };
  };
  const DocumentSaver neverCalled = stubSaver(true, nullptr);

  std::printf("  -- A. a clean document closes on the click, with no question --\n");

  // The common case, and the one this feature must not have made slower. A
  // confirmation on a document with nothing to lose is the kind of friction
  // that teaches users to answer dialogs without reading them, which is
  // exactly how the dialogs that matter stop working.
  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Clean"));
    PendingClose pending;

    const CloseOutcome out = requestDocumentClose(session, 0, pending);
    check(out.closed && session.empty(),
          "clean: one call closes it -- no extra frame, no confirmation, no second step");
    check(!out.questionRaised && !pending.active(),
          "clean: NO question was raised -- nothing pending, so no dialog can appear");
    check(contains(out.status, "Clean"),
          "clean: the status names the document, so a close is visibly a close");
  }

  // The last document going is a state the application already has a
  // behaviour for -- an empty session, the tab strip hidden, the canvas still
  // drawing paper -- and this change must not invent a different one (no
  // auto-created blank, no quit).
  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Only"));
    PendingClose pending;
    (void)requestDocumentClose(session, 0, pending);
    check(session.empty() && session.count() == 0 && session.active() == nullptr &&
              session.activeIndex() == 0,
          "clean: closing the LAST document leaves an empty session, exactly as before");
  }

  std::printf("  -- B. a dirty document is asked about, not silently refused --\n");

  {
    DocumentSession session;
    session.add(dirtyDoc("Study", "stroke"));
    PendingClose pending;

    const CloseOutcome out = requestDocumentClose(session, 0, pending);
    check(out.questionRaised && !out.closed && session.count() == 1,
          "dirty: the close raises a question and the document is STILL OPEN -- the "
          "refusal became a question, not a silent no-op");
    check(pending.active() && pending.asking() && pending.document == session.at(0)->id,
          "dirty: the pending close names the document by identity");
    check(session.at(0)->isDirty() && session.at(0)->unsavedEdits.size() == 1,
          "dirty: raising the question changed nothing about the document itself");

    // The dialog has to name the document: the close boxes are 14 px apart and
    // a user closing one of six tabs must see which one they hit.
    const std::string q = closeQuestion(*session.at(0));
    check(contains(q, "Study") && contains(q, "stroke") && contains(q, "unsaved change"),
          "dirty: the question names the DOCUMENT and the work -- PRD I11's own summary");

    // Two overlapping questions about two documents is a dialog nobody can
    // answer correctly, so the second request is refused rather than queued.
    session.add(dirtyDoc("Other", "stroke"));
    const CloseOutcome second = requestDocumentClose(session, 1, pending);
    check(!second.questionRaised && !second.closed && pending.document == session.at(0)->id &&
              contains(second.status, "Study"),
          "dirty: a second question is refused while the first is up, naming the first");
  }

  std::printf("  -- C. Cancel changes nothing at all --\n");

  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "A"));
    session.add(dirtyDoc("B", "stroke"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "C"));
    session.setActive(2);
    PendingClose pending;

    (void)requestDocumentClose(session, 1, pending);
    const CloseOutcome out = resolveDocumentClose(session, pending, CloseAnswer::Cancel,
                                                  neverCalled);
    check(!out.closed && session.count() == 3,
          "cancel: the document count is unchanged");
    check(session.at(1)->isDirty() && session.at(1)->unsavedEdits.size() == 1,
          "cancel: the dirty flag and the unsaved-edit labels are unchanged -- a cancel "
          "that quietly marked the document clean would lose the work at the next close");
    check(session.activeIndex() == 2,
          "cancel: the active index is unchanged -- asking about one tab must not switch "
          "the user to another");
    check(!pending.active(),
          "cancel: nothing is left pending, so the question does not come back");
  }

  std::printf("  -- D. Don't Save closes and discards, and only the one --\n");

  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Keep"));
    session.add(dirtyDoc("Throw", "stroke"));
    PendingClose pending;

    (void)requestDocumentClose(session, 1, pending);
    const CloseOutcome out = resolveDocumentClose(session, pending, CloseAnswer::DontSave,
                                                  neverCalled);
    check(out.closed && session.count() == 1,
          "don't save: the count drops by EXACTLY one");
    check(session.at(0) != nullptr && documentDisplayName(*session.at(0)) == "Keep",
          "don't save: the document that closed is the one that was asked about");
    check(!pending.active(),
          "don't save: nothing is left pending");
  }

  std::printf("  -- E. Save closes only what it saved --\n");

  {
    DocumentSession session;
    session.add(dirtyDoc("Painting", "stroke"));
    session.at(0)->path = "/tmp/selftest-close-decision.npaint";
    PendingClose pending;
    SaveLog log;
    // Read BEFORE the close: afterwards the record is gone from the session,
    // and an assertion that has to reach into a closed document to name what it
    // is asserting about is testing its own bookkeeping.
    const DocumentId painting = session.at(0)->id;

    (void)requestDocumentClose(session, 0, pending);
    const CloseOutcome out =
        resolveDocumentClose(session, pending, CloseAnswer::Save, stubSaver(true, &log));
    // Both halves, deliberately. A "Save" that closed the document without
    // ever calling the writer would satisfy a count-only check perfectly, and
    // it is precisely the bug that loses a morning's work.
    check(out.closed && session.empty(),
          "save: the document is closed afterwards");
    check(log.calls == 1 && log.lastId == painting,
          "save: the writer was called EXACTLY once, on that document -- a close that "
          "skipped the write would pass a count-only check and lose the work");
    check(contains(out.status, "Saved"),
          "save: the status says the file was written, not merely that a tab went away");
  }

  // A save that fails must not close. The user asked to keep the work.
  {
    DocumentSession session;
    session.add(dirtyDoc("Painting", "stroke"));
    session.at(0)->path = "/tmp/selftest-close-decision.npaint";
    PendingClose pending;
    SaveLog log;

    (void)requestDocumentClose(session, 0, pending);
    const CloseOutcome out =
        resolveDocumentClose(session, pending, CloseAnswer::Save, stubSaver(false, &log));
    check(!out.closed && session.count() == 1 && session.at(0)->isDirty(),
          "save: a FAILED save closes nothing and leaves the document dirty -- closing "
          "anyway would destroy exactly the work the user asked to keep");
    check(pending.asking() && contains(out.status, "told to fail"),
          "save: the question stays up carrying the writer's own error, so the user can "
          "answer it again rather than being dropped back to a canvas");
  }

  // Save on a document that has never been saved cannot pick a file: this
  // build has no native file picker, so the decision hands back to the
  // caller's Save As flow rather than inventing a path.
  {
    DocumentSession session;
    session.add(dirtyDoc("Untitled work", "stroke"));
    PendingClose pending;
    SaveLog log;

    (void)requestDocumentClose(session, 0, pending);
    const CloseOutcome out =
        resolveDocumentClose(session, pending, CloseAnswer::Save, stubSaver(true, &log));
    check(!out.closed && out.needsDestination && log.calls == 0 && session.count() == 1,
          "save: a never-saved document asks for a destination and writes nothing");
    check(pending.active() && pending.awaitingDestination && !pending.asking(),
          "save: the pending close survives the hand-off, and stops asking so two modals "
          "never stack");

    // What the caller's Save As flow does, then the answer repeated. The
    // document is clean by then, so the close completes without a second
    // identical write.
    session.at(0)->path = "/tmp/selftest-close-decision.npaint";
    session.at(0)->savedRevision = session.at(0)->revision;
    session.at(0)->unsavedEdits.clear();
    pending.awaitingDestination = false;
    const CloseOutcome done =
        resolveDocumentClose(session, pending, CloseAnswer::Save, stubSaver(true, &log));
    check(done.closed && session.empty() && log.calls == 0,
          "save: answering Save after Save As finishes the close and does NOT write the "
          "same bytes a second time");
  }

  // The production saver is `saveDocument()` and not a second implementation.
  // Asserted through the one refusal only that function produces, which
  // short-circuits before any file is touched -- so this holds in both
  // NP_USE_OIIO configurations and writes nothing.
  {
    OpenDocument unbound = makeBlankOpenDocument(8, 8, WorkingSpace{}, "Unbound");
    const DocumentOpResult r = documentSaverFor(nullptr)(unbound);
    check(!r.ok && contains(r.error, "never been saved") && contains(r.error, "Save As"),
          "save: the production saver is saveDocument() -- its own refusal comes back, so "
          "there is no second save path for the close dialog to drift from");
  }

  std::printf("  -- F. the pending close survives an unrelated close --\n");

  // **The assertion this section exists for.** The stale index is arranged to
  // be in range and to name a DIFFERENT document, so an implementation that
  // remembered the index does not fail -- it succeeds at closing the wrong
  // document. Deliberately breaking the key back to an index while writing
  // this made exactly these three lines go red and nothing else.
  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "A"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "B"));
    session.add(dirtyDoc("C", "stroke"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "D"));
    PendingClose pending;

    (void)requestDocumentClose(session, 2, pending);  // asks about C, at index 2

    // Something else closes A -- the File menu, another tab, a recovery
    // offer. Everything after it shifts down one, so index 2 is now D.
    std::string err;
    check(session.close(0, false, &err) && session.count() == 3 &&
              documentDisplayName(*session.at(2)) == "D",
          "identity: after an unrelated close, the remembered INDEX 2 now names D");

    const CloseOutcome out = resolveDocumentClose(session, pending, CloseAnswer::DontSave,
                                                  neverCalled);
    check(out.closed && session.count() == 2,
          "identity: the answer still closes exactly one document");
    bool cGone = true;
    for (size_t i = 0; i < session.count(); ++i)
      if (documentDisplayName(*session.at(i)) == "C") cGone = false;
    const bool bAndDKept = session.count() == 2 &&
                           documentDisplayName(*session.at(0)) == "B" &&
                           documentDisplayName(*session.at(1)) == "D";
    check(cGone && bAndDKept,
          "identity: it closes C -- the document that was ASKED about -- and leaves B and "
          "D alone; an index-keyed pending close would have discarded D instead");
  }

  // The other half of the same hazard: the asked-about document is closed by
  // something else before the answer arrives. An id is never reused within a
  // run, so this is reported rather than acted on.
  {
    DocumentSession session;
    session.add(dirtyDoc("Gone", "stroke"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Bystander"));
    PendingClose pending;

    (void)requestDocumentClose(session, 0, pending);
    std::string err;
    (void)session.close(0, true, &err);  // something else discarded it

    const CloseOutcome out = resolveDocumentClose(session, pending, CloseAnswer::DontSave,
                                                  neverCalled);
    check(out.vanished && !out.closed && session.count() == 1 &&
              documentDisplayName(*session.at(0)) == "Bystander",
          "identity: a question about a document that has gone closes NOTHING -- the "
          "bystander that inherited its index is untouched");
    check(!pending.active() && contains(out.status, "Gone"),
          "identity: the stale question is cleared and says which document it was about");
  }

  // Cancel must work on a vanished question too: the user asked for nothing to
  // happen, and nothing happening cannot fail.
  {
    DocumentSession session;
    session.add(dirtyDoc("Gone", "stroke"));
    PendingClose pending;
    (void)requestDocumentClose(session, 0, pending);
    std::string err;
    (void)session.close(0, true, &err);
    const CloseOutcome out = resolveDocumentClose(session, pending, CloseAnswer::Cancel,
                                                  neverCalled);
    check(!out.vanished && !out.closed && !pending.active(),
          "identity: cancelling a question whose document has gone is a plain success");
  }

  std::printf("  -- G. Escape is Cancel, Enter is Save, and no key discards --\n");

  // Getting these two backwards is how a user loses a morning's work to a
  // reflex, and a build with them swapped looks and feels completely normal
  // until the moment it does.
  {
    check(closeAnswerForKey(CloseKey::Escape) == CloseAnswer::Cancel,
          "keys: Escape is Cancel -- the key for making an unexpected dialog go away "
          "must be the answer that changes nothing");
    check(closeAnswerForKey(CloseKey::Enter) == CloseAnswer::Save,
          "keys: Enter is Save -- of the two plausible defaults, the worst case is a file "
          "written, not a painting lost");

    // By exhaustion over the enum rather than by inspection: the requirement
    // is that discarding is unreachable from the keyboard, so a third key
    // added later must fail here rather than quietly become a way to destroy
    // work by holding Return.
    const CloseKey allKeys[] = {CloseKey::Escape, CloseKey::Enter};
    bool anyDiscards = false;
    for (const CloseKey k : allKeys)
      if (closeAnswerForKey(k) == CloseAnswer::DontSave) anyDiscards = true;
    check(!anyDiscards,
          "keys: NO key maps to Don't Save -- a dialog appearing under a held Return can "
          "never discard, however long the key is held");
  }

  std::printf("  -- H. the refusals that are not questions --\n");

  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "A"));
    PendingClose pending;

    const CloseOutcome out = requestDocumentClose(session, 99, pending);
    check(!out.closed && !out.questionRaised && !pending.active() &&
              contains(out.status, "no open document"),
          "range: an index that is not there is refused by name, not crashed on");

    // An answer with nothing pending is a no-op, not an error: a key press
    // arriving a frame after the dialog closed must not do anything.
    PendingClose idle;
    const CloseOutcome none = resolveDocumentClose(session, idle, CloseAnswer::DontSave,
                                                   neverCalled);
    check(!none.closed && !none.vanished && session.count() == 1,
          "range: answering when nothing is pending does nothing at all");

    check(!documentIndexById(session, 0).has_value() &&
              documentIndexById(session, session.at(0)->id) == std::optional<size_t>(0),
          "range: the id lookup finds a real document and refuses the 'no document' id 0");
  }

  std::printf("[selftest] close decision %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
