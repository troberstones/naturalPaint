#include "app/selftest/Support.hpp"

#include "app/ImportImage.hpp"
#include "app/QuitSequence.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Two ways in and one way out: importing an image into the open document
// (app/ImportImage), and quitting with unsaved work open (app/QuitSequence).
//
// One section for both because they are the same defect twice, at opposite
// ends of the application, and the assertions that catch them have the same
// shape.
//
//   * `placeImageAsLayer()` was finished, correct and asserted by
//     app/selftest/PlaceImageAsLayer.cpp -- and had **no caller in the binary
//     outside --selftest**. A feature can be complete and still be absent.
//     Sections A-C assert the operation the File menu now reaches, including
//     the part a screenshot cannot witness: that a refused import leaves the
//     document byte-for-byte as it was.
//   * Quitting set `st.quit` from four places and consulted no document at all,
//     so `DocumentSession::close()` -- and with it PRD I11 and the whole
//     Save / Don't Save / Cancel question -- was bypassed by the one exit every
//     user takes. Sections D-H assert the sequence that replaced it.
//
// **The mistake this file must not repeat.** app/selftest/CloseDecision.cpp
// originally asserted on `documentDisplayName()`, which prefers the *path's*
// filename over the title, so binding a path renamed a document out from under
// its own assertions. Every identity assertion below is on `DocumentId`, which
// is the one thing about a document that a save, a rename or a reorder cannot
// change -- and which is what `QuitSequence` itself is keyed on.
//
// Headless and GPU-free. The save is injected (app/CloseDecision's
// `DocumentSaver`), so sections D-H hold in BOTH NP_USE_OIIO configurations
// (PLAN.md §1.5) rather than going quiet in the OFF build. Sections A-C do
// write files, in a scratch directory of their own that is removed at the end,
// because "a path that does not exist is refused by name" cannot be asserted
// without a filesystem -- and they use PNG, which stb decodes in both
// configurations, so nothing here depends on OpenImageIO either.
// ---------------------------------------------------------------------------
bool runQuitGuardTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- fixtures -------------------------------------------------------------

  // A scratch directory of this section's own, so a developer's files are never
  // in reach and two runs cannot collide over a half-written fixture.
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "np-selftest-quitguard";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);

  // Writes a `w` x `h` opaque PNG and returns its path. Opaque throughout: the
  // premultiply arithmetic is app/selftest/PlaceImageAsLayer.cpp's to assert
  // and is not restated here -- what this section needs from the fixture is
  // that it decodes.
  auto writePng = [&](const char* name, int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = 200;
      px[i + 1] = 120;
      px[i + 2] = 40;
      px[i + 3] = 255;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, w, h, 4, px.data(), w * 4);
    const std::filesystem::path p = scratch / name;
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    out.close();
    return p.string();
  };

  auto writeRaw = [&](const char* name, const std::string& bytes) {
    const std::filesystem::path p = scratch / name;
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return p.string();
  };

  // A document with `title` and one recorded edit, so it is dirty for a named
  // reason -- the summary the question shows is built from these labels.
  auto dirtyDoc = [](const char* title, const char* edit) {
    OpenDocument d = makeBlankOpenDocument(8, 8, WorkingSpace{}, title);
    d.recordEdit(edit);
    return d;
  };

  // The same stub shape app/selftest/CloseDecision.cpp uses, and for the same
  // reason: the real save writes a `.npaint`, which the NP_USE_OIIO=OFF build
  // refuses by design, so a section that used it would assert nothing in half
  // the configurations this project ships.
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
      if (doc.path.empty()) doc.path = "/tmp/selftest-quit-guard.npaint";
      doc.savedRevision = doc.revision;
      doc.unsavedEdits.clear();
      doc.unsavedEditsDropped = 0;
      r.ok = true;
      r.path = doc.path;
      return r;
    };
  };
  const DocumentSaver neverCalled = stubSaver(true, nullptr);

  // Runs a quit to a standstill, recording **which document each question was
  // about, by id**, and answering each with `answerFor(n)` where n is the
  // question's ordinal. Returns the final step so the caller can assert on
  // `exitNow` / `abandoned`.
  //
  // The loop is bounded rather than `while (step.asking)`: a sequencing bug
  // that re-raised the same question forever would otherwise hang the whole
  // suite instead of failing it, and a hung test tells nobody anything.
  struct QuitRun {
    std::vector<DocumentId> asked;
    QuitStep last;
    bool ranAway = false;
  };
  auto driveQuit = [&](DocumentSession& session, QuitSequence& seq, PendingClose& pending,
                       const DocumentSaver& save,
                       const std::vector<CloseAnswer>& answers) {
    QuitRun run;
    run.last = beginQuit(session, seq, pending);
    for (int guard = 0; guard < 16; ++guard) {
      if (!run.last.asking) return run;
      run.asked.push_back(pending.document);
      const size_t which = run.asked.size() - 1;
      const CloseAnswer a =
          which < answers.size() ? answers[which] : CloseAnswer::DontSave;
      run.last = answerQuitQuestion(session, pending, seq, a, save, nullptr);
    }
    run.ranAway = true;
    return run;
  };

  std::printf("  -- A. an import adds one RGB layer, and it becomes active --\n");

  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "Study");
    const size_t layersBefore = doc.document.layers.size();
    const size_t historyBefore = doc.history.entries().size();
    const uint64_t revisionBefore = doc.revision;
    const std::string png = writePng("import-16x16.png", 16, 16);

    const ImportImageResult r = importImageAsLayer(doc, png);
    check(r.ok && doc.document.layers.size() == layersBefore + 1,
          "import: exactly ONE layer is added -- not two, and not zero");
    check(r.imageWidth == 16 && r.imageHeight == 16 && r.warnings.empty(),
          "import: an image that fits the canvas reports its size and warns about nothing");

    const Layer& placed = doc.document.layers.back();
    check(placed.kind == LayerKind::RGB,
          "import: the new layer is RGB-kind -- what the user asked for by name");
    check(placed.rgbTiles.has_value() && placed.rgbTiles->occupiedTileCount() > 0,
          "import: its rgbTiles are engaged AND hold the image -- an RGB layer with an "
          "empty store would look like a successful import of nothing");

    check(r.layerIndex == doc.document.layers.size() - 1,
          "import: the layer lands on TOP of the stack, where it cannot be hidden by "
          "whatever was already above the active layer");
    check(activeLayerIndex(doc) == std::optional<size_t>(r.layerIndex),
          "import: it becomes the ACTIVE layer -- a layer the panel does not select reads "
          "as a layer that failed to arrive");

    check(doc.history.entries().size() == historyBefore + 1,
          "import: EXACTLY one history entry, so one undo takes the import back");
    check(!doc.history.entries().empty() &&
              doc.history.entries().back().label == importImageEditLabel(png),
          "import: the entry is labelled with the file's own name, so a panel with four "
          "imports in it can be navigated");
    check(doc.revision > revisionBefore && doc.isDirty(),
          "import: the document is dirty afterwards -- the layer exists nowhere on disk");
    check(contains(r.status, "16x16") && contains(r.status, "import-16x16.png"),
          "import: the success sentence names the file and the pixel size");
  }

  // Placing onto a document that already has a stack: the existing layers are
  // untouched and the import is on top of all of them.
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "Stacked");
    doc.document.layers.push_back(Layer{});
    doc.document.layers.push_back(Layer{});
    const size_t before = doc.document.layers.size();
    const std::string png = writePng("import-stacked.png", 8, 8);
    doc.activeLayer = 0;  // deliberately NOT the top

    const ImportImageResult r = importImageAsLayer(doc, png);
    check(r.ok && doc.document.layers.size() == before + 1 && r.layerIndex == before,
          "import: with three layers open it lands at the top, not above the active one");
    check(activeLayerIndex(doc) == std::optional<size_t>(before),
          "import: the active layer follows it to the top rather than staying at 0");
  }

  std::printf("  -- B. every refused import changes nothing, and says why --\n");

  // Each of these must add no layer, record no history entry, leave the
  // revision alone and name the file. A silent no-op is the defect this whole
  // module was written against.
  {
    const std::string good = writePng("import-refusal-base.png", 4, 4);
    (void)good;
    const std::string missing = (scratch / "no-such-file.png").string();
    const std::string garbage = writeRaw("import-garbage.png", "this is not an image at all");
    const std::string emptyFile = writeRaw("import-empty.png", "");
    const std::string folder = scratch.string();

    struct Case {
      const char* what;
      std::string path;
      const char* needle;
    };
    const Case cases[] = {
        {"a file that does not exist", missing, "no-such-file.png"},
        {"a file no decoder accepts", garbage, "import-garbage.png"},
        {"an empty file", emptyFile, "import-empty.png"},
        {"a folder", folder, "quitguard"},
        {"an empty path", std::string(), nullptr},
    };

    for (const Case& c : cases) {
      OpenDocument doc = makeBlankOpenDocument(32, 32, WorkingSpace{}, "Untouched");
      const size_t layers = doc.document.layers.size();
      const size_t entries = doc.history.entries().size();
      const uint64_t revision = doc.revision;
      const size_t active = doc.activeLayer;

      const ImportImageResult r = importImageAsLayer(doc, c.path);
      const bool unchanged = doc.document.layers.size() == layers &&
                             doc.history.entries().size() == entries &&
                             doc.revision == revision && doc.activeLayer == active &&
                             !doc.isDirty();
      std::string label = std::string("import refused: ") + c.what;
      // Both halves in one assertion on purpose: an implementation that
      // returned `ok == false` while having already appended the layer would
      // pass a returns-false check and lose nothing visible until the next save.
      check(!r.ok && unchanged, (label + " -- no layer, no history, no revision").c_str());
      check(!r.status.empty() && (c.needle == nullptr || contains(r.status, c.needle)),
            (label + " -- the message names the file").c_str());
    }
  }

  std::printf("  -- C. an image larger than the canvas is warned about, not cropped --\n");

  // The surprise app/ImportImage.hpp records: the overhang is kept in the tile
  // store and is never composited, drawn or exported. Asserted rather than
  // fixed, because cropping or resampling the user's image on their behalf is
  // the one thing an import must not do quietly.
  {
    OpenDocument doc = makeBlankOpenDocument(16, 16, WorkingSpace{}, "Small canvas");
    const std::string png = writePng("import-oversize.png", 300, 200);
    const ImportImageResult r = importImageAsLayer(doc, png);

    check(r.ok && r.imageWidth == 300 && r.imageHeight == 200,
          "oversize: a 300x200 image imports into a 16x16 document at its FULL size -- "
          "nothing is cropped and nothing is resampled");
    check(r.warnings.size() == 1 && contains(r.warnings[0], "300x200") &&
              contains(r.warnings[0], "16x16"),
          "oversize: one warning, naming both sizes, so the cost is visible when it is paid");

    // 300x200 spans ceil(300/128) x ceil(200/128) = 3 x 2 tiles. A 16x16
    // document is one tile. The difference is the pixels that exist and cannot
    // be seen -- five tiles, 640 KiB, of them.
    const Layer& placed = doc.document.layers.back();
    check(placed.rgbTiles.has_value() && placed.rgbTiles->occupiedTileCount() == 6,
          "oversize: the layer really holds all 3x2 tiles the image spans, five of them "
          "wholly or partly outside a one-tile canvas");
  }

  std::printf("  -- D. quitting with nothing unsaved exits at once, asking nothing --\n");

  // The common case, and the one this guard must not have made slower. A
  // confirmation on a session with nothing to lose is the kind of friction that
  // teaches users to answer dialogs without reading them.
  {
    DocumentSession session;
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "A"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "B"));
    QuitSequence seq;
    PendingClose pending;

    const QuitStep step = beginQuit(session, seq, pending);
    check(step.exitNow && !step.asking && !step.abandoned,
          "quit clean: it exits on the call -- no dialog, no queue, no extra frame");
    check(!pending.active() && seq.remaining.empty() && !seq.running,
          "quit clean: NOTHING was stored -- no question can appear on a later frame");
    check(session.count() == 2,
          "quit clean: the clean documents are left alone rather than closed one by one "
          "on the way out, which would be work done for nobody");
  }

  // An empty session is the same fast path and must not be a special case.
  {
    DocumentSession session;
    QuitSequence seq;
    PendingClose pending;
    check(beginQuit(session, seq, pending).exitNow && !seq.running,
          "quit clean: a session with no documents at all exits immediately too");
  }

  std::printf("  -- E. each dirty document is asked about, in order, and only those --\n");

  {
    DocumentSession session;
    session.add(dirtyDoc("Alpha", "stroke"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Beta"));  // clean
    session.add(dirtyDoc("Gamma", "layer add"));
    QuitSequence seq;
    PendingClose pending;
    // By id, read before anything closes: an assertion that has to reach into a
    // closed document to name what it is asserting about is testing its own
    // bookkeeping. And by id rather than display name, which a save renames.
    const DocumentId alpha = session.at(0)->id;
    const DocumentId beta = session.at(1)->id;
    const DocumentId gamma = session.at(2)->id;

    const QuitRun run = driveQuit(session, seq, pending, neverCalled,
                                  {CloseAnswer::DontSave, CloseAnswer::DontSave});
    check(!run.ranAway && run.asked.size() == 2,
          "quit dirty: EXACTLY two questions for two dirty documents -- not one, not three");
    check(run.asked.size() == 2 && run.asked[0] == alpha && run.asked[1] == gamma,
          "quit dirty: in session order, Alpha then Gamma -- left to right along the tab "
          "strip, which is the order the user sees them in");
    bool betaAsked = false;
    for (const DocumentId id : run.asked)
      if (id == beta) betaAsked = true;
    check(!betaAsked,
          "quit dirty: the CLEAN document is never asked about -- a question about a "
          "document with nothing to lose is the one that trains users to click through");
    check(run.last.exitNow && !seq.running,
          "quit dirty: answering the last question finishes the quit and the app exits");
    check(session.count() == 1 && session.at(0) != nullptr && session.at(0)->id == beta,
          "quit dirty: both answered documents closed; the clean one is still open, "
          "untouched, at the moment the process exits");
  }

  // A quit cannot start on top of a question that is already up: two stacked
  // modals about two documents is a dialog nobody can answer correctly.
  {
    DocumentSession session;
    session.add(dirtyDoc("Busy", "stroke"));
    QuitSequence seq;
    PendingClose pending;
    (void)requestDocumentClose(session, 0, pending);  // the tab strip's close box

    const QuitStep step = beginQuit(session, seq, pending);
    check(!step.exitNow && !step.asking && !seq.running && contains(step.status, "Busy"),
          "quit refused: a quit raised while a close question is open is refused by name, "
          "and above all does NOT exit");
    check(pending.document == session.at(0)->id,
          "quit refused: the question that was already up is untouched");
  }

  std::printf("  -- F. Cancel on any question abandons the whole quit --\n");

  {
    DocumentSession session;
    session.add(dirtyDoc("One", "stroke"));
    session.add(dirtyDoc("Two", "stroke"));
    session.add(dirtyDoc("Three", "stroke"));
    QuitSequence seq;
    PendingClose pending;
    const DocumentId two = session.at(1)->id;
    const DocumentId three = session.at(2)->id;

    // Don't Save on the first, Cancel on the second.
    const QuitRun run = driveQuit(session, seq, pending, neverCalled,
                                  {CloseAnswer::DontSave, CloseAnswer::Cancel});
    check(run.asked.size() == 2 && run.last.abandoned && !run.last.exitNow,
          "quit cancel: the third document is NEVER asked about, and the application does "
          "NOT exit -- 'cancel' means stop, not 'ask me about the rest'");
    check(!seq.running && seq.remaining.empty() && !pending.active(),
          "quit cancel: nothing is left in flight, so the next Cmd-Q starts over cleanly "
          "rather than being refused for the rest of the session");
    check(session.count() == 2 && session.at(0)->id == two && session.at(1)->id == three,
          "quit cancel: the two unanswered documents are still open, by identity");
    check(session.at(0)->isDirty() && session.at(1)->isDirty(),
          "quit cancel: and still dirty -- a cancel that quietly marked them clean would "
          "lose the work at the next quit");
    check(!contains(run.last.status, "Three"),
          "quit cancel: the sentence is about the quit stopping, not about a document "
          "that was never asked about");
  }

  // **Don't Save on document one, then Cancel on document two, does NOT
  // re-open document one.** Argued at length in app/QuitSequence.hpp: the user
  // answered a direct question about a named document with its unsaved work
  // spelled out, and reversing that because of something they said about a
  // *different* document treats a considered click as provisional. Pinned here
  // so the decision cannot be reversed by accident.
  {
    DocumentSession session;
    session.add(dirtyDoc("Discarded", "stroke"));
    session.add(dirtyDoc("Kept", "stroke"));
    QuitSequence seq;
    PendingClose pending;
    const DocumentId discarded = session.at(0)->id;

    const QuitRun run = driveQuit(session, seq, pending, neverCalled,
                                  {CloseAnswer::DontSave, CloseAnswer::Cancel});
    bool discardedBack = false;
    for (size_t i = 0; i < session.count(); ++i)
      if (session.at(i)->id == discarded) discardedBack = true;
    check(run.asked.size() == 2 && !discardedBack && session.count() == 1,
          "quit cancel: a document already answered with Don't Save stays closed -- the "
          "cancel stops the quit, it does not un-answer an answered question");
  }

  std::printf("  -- G. a save that fails abandons the quit --\n");

  {
    DocumentSession session;
    session.add(dirtyDoc("Fails", "stroke"));
    session.add(dirtyDoc("Untouched", "stroke"));
    session.at(0)->path = "/tmp/selftest-quit-guard.npaint";
    QuitSequence seq;
    PendingClose pending;
    SaveLog log;
    const DocumentId fails = session.at(0)->id;
    const DocumentId untouched = session.at(1)->id;

    const QuitRun run = driveQuit(session, seq, pending, stubSaver(false, &log),
                                  {CloseAnswer::Save});
    check(run.asked.size() == 1 && run.last.abandoned && !run.last.exitNow,
          "quit save-failed: the quit is abandoned -- it does NOT march on to the next "
          "document past the one whose work the user just asked to keep");
    check(!seq.running && session.count() == 2,
          "quit save-failed: nothing was closed at all");
    check(session.at(0) != nullptr && session.at(0)->id == fails &&
              session.at(0)->isDirty(),
          "quit save-failed: that document is still open and still dirty");
    check(session.at(1) != nullptr && session.at(1)->id == untouched &&
              session.at(1)->isDirty(),
          "quit save-failed: and the one behind it was never asked about or touched");
    check(pending.asking() && pending.document == fails &&
              contains(run.last.status, "told to fail"),
          "quit save-failed: the question stays up carrying the WRITER's own error, which "
          "is the only place in this application an error is shown rather than dimmed");
    check(log.calls == 1 && log.lastId == fails,
          "quit save-failed: the writer was called exactly once, on that document");
  }

  // The successful counterpart, so a silenced saver cannot pass the failure
  // case above: Save really writes, and only then closes.
  {
    DocumentSession session;
    session.add(dirtyDoc("Saves", "stroke"));
    session.at(0)->path = "/tmp/selftest-quit-guard.npaint";
    QuitSequence seq;
    PendingClose pending;
    SaveLog log;
    const DocumentId saves = session.at(0)->id;

    const QuitRun run =
        driveQuit(session, seq, pending, stubSaver(true, &log), {CloseAnswer::Save});
    check(run.asked.size() == 1 && run.last.exitNow && session.empty(),
          "quit save: a successful save finishes the quit");
    check(log.calls == 1 && log.lastId == saves,
          "quit save: the writer really ran on that document -- a quit that closed without "
          "writing would pass a count-only check and lose the work");
  }

  std::printf("  -- H. every question is keyed on identity, not on position --\n");

  // **The assertion this section exists for**, and the same hazard
  // app/CloseDecision.cpp's section F was built for -- except that here it is
  // structural rather than a race: the sequence closes documents as it goes, so
  // a queue of indices would be wrong on its second entry every single time.
  //
  // The fixture is arranged so an index-keyed queue does not crash and does not
  // obviously fail. It would take index 1 for its second question, which by
  // then names the CLEAN document D -- so it would either ask about a document
  // with nothing to lose or skip straight to exit, and either way Gamma's
  // unsaved work would go out of the door unasked.
  {
    DocumentSession session;
    session.add(dirtyDoc("Alpha", "stroke"));
    session.add(dirtyDoc("Beta", "stroke"));
    session.add(dirtyDoc("Gamma", "stroke"));
    session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}, "Delta"));  // clean
    QuitSequence seq;
    PendingClose pending;
    const DocumentId alpha = session.at(0)->id;
    const DocumentId gamma = session.at(2)->id;
    const DocumentId delta = session.at(3)->id;

    const QuitStep first = beginQuit(session, seq, pending);
    check(first.asking && pending.document == alpha,
          "identity: the first question is about Alpha");

    // Something else closes Beta while the question is up -- the File menu, a
    // recovery offer, another tab's close box. Everything behind it shifts down.
    std::string err;
    check(session.close(1, /*discardUnsavedChanges=*/true, &err) && session.count() == 3,
          "identity: Beta is closed underneath the sequence, so index 1 now names Gamma "
          "and index 2 names the clean Delta");

    const QuitStep second =
        answerQuitQuestion(session, pending, seq, CloseAnswer::DontSave, neverCalled, nullptr);
    check(second.asking && pending.document == gamma,
          "identity: the next question is about GAMMA, the document that is still dirty -- "
          "an index-keyed queue would have moved to the clean Delta and let Gamma's work "
          "leave unasked");

    const QuitStep third =
        answerQuitQuestion(session, pending, seq, CloseAnswer::DontSave, neverCalled, nullptr);
    check(third.exitNow && !third.asking,
          "identity: Beta's vanished entry is skipped rather than asked about, so the "
          "queue empties and the quit finishes");
    check(session.count() == 1 && session.at(0) != nullptr && session.at(0)->id == delta,
          "identity: exactly the clean Delta is left, never asked about and never closed");
  }

  // The document being asked about vanishes. Ids are never reused within a run
  // (app/DocumentLifecycle.hpp), so this is skipped rather than acted on -- the
  // bystander that inherited its index is not closed in its place.
  {
    DocumentSession session;
    session.add(dirtyDoc("Doomed", "stroke"));
    session.add(dirtyDoc("Bystander", "stroke"));
    QuitSequence seq;
    PendingClose pending;
    const DocumentId bystander = session.at(1)->id;

    const QuitStep first = beginQuit(session, seq, pending);
    check(first.asking && pending.document == session.at(0)->id,
          "identity: the question is about Doomed");
    std::string err;
    (void)session.close(0, /*discardUnsavedChanges=*/true, &err);

    const QuitStep next =
        answerQuitQuestion(session, pending, seq, CloseAnswer::DontSave, neverCalled, nullptr);
    check(next.asking && pending.document == bystander && session.count() == 1,
          "identity: a question about a document that has GONE closes nothing and moves "
          "to the bystander as its own question, rather than discarding it in silence");
  }

  std::printf("  -- I. the answers mean the same thing on the way out --\n");

  // The quit reuses app/CloseDecision's mapping rather than defining a second
  // one, so Escape is Cancel, Enter is Save, and no key discards -- however the
  // dialog was raised. Asserted here at the quit's own call site, because the
  // requirement is about what a *quit* can do to a user's work, and a build
  // where these two were swapped would look entirely normal until it ate a
  // morning's painting.
  {
    DocumentSession session;
    session.add(dirtyDoc("Held", "stroke"));
    QuitSequence seq;
    PendingClose pending;
    const DocumentId held = session.at(0)->id;

    (void)beginQuit(session, seq, pending);
    const QuitStep escaped = answerQuitQuestion(
        session, pending, seq, closeAnswerForKey(CloseKey::Escape), neverCalled, nullptr);
    check(escaped.abandoned && !escaped.exitNow && session.count() == 1 &&
              session.at(0)->id == held && session.at(0)->isDirty(),
          "keys: Escape during a quit cancels it and destroys nothing");

    const CloseKey allKeys[] = {CloseKey::Escape, CloseKey::Enter};
    bool anyDiscards = false;
    for (const CloseKey k : allKeys)
      if (closeAnswerForKey(k) == CloseAnswer::DontSave) anyDiscards = true;
    check(!anyDiscards,
          "keys: no key reaches Don't Save on the quit path either -- a quit dialog "
          "appearing under a held Return can never discard, however long it is held");
  }

  // `abandonQuit()` is a no-op with no quit running, so the UI callers that
  // reach for it on a plain close cannot report an abandonment that never was.
  {
    QuitSequence idle;
    const QuitStep step = abandonQuit(idle, "should not appear");
    check(!step.abandoned && step.status.empty() && !idle.running,
          "keys: abandoning a quit that is not running does nothing and says nothing");
  }

  std::filesystem::remove_all(scratch, ec);

  std::printf("[selftest] quit guard %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
