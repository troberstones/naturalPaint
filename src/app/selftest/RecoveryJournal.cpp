#include "app/selftest/Support.hpp"

namespace np {

// ==========================================================================
// Phase 4 step 9 -- the recovery journal (ADR-0008; PRD O5-O10).
// ==========================================================================
bool runRecoveryJournalTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // Everything this section writes lives here, and $NP_JOURNAL_DIR points the
  // module at it for the whole run -- so nothing below can touch
  // ~/Library/Application Support/naturalPaint, which the OIIO-build run is
  // separately verified not to create. Same override mechanism, and the same
  // reason, as $NP_RECENT_DOCUMENTS in the lifecycle section above.
  const std::string root = "selftest_journal";
  fs::remove_all(root, ec);
  const char* previousRoot = std::getenv("NP_JOURNAL_DIR");
  const std::string savedRoot = previousRoot ? previousRoot : "";
  setenv("NP_JOURNAL_DIR", root.c_str(), 1);

  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto readStraightRed = [](const Document& doc, size_t layerIndex, int32_t x, int32_t y) {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    if (!t) return -1.0f;
    const std::array<float, 4> px = t->readPixel(tileLocalOffset(p));
    return px[3] > 0.0f ? px[0] / px[3] : 0.0f;
  };
  // Zero tolerance, as everywhere else a `.npaint` round trip is checked:
  // there is no rounding stage anywhere in the chain (io/NpaintFile.hpp's
  // HALF section), so anything short of bit equality is a bug rather than
  // drift.
  auto tilesIdentical = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const auto& ta = a.layers[i].rgbTiles;
      const auto& tb = b.layers[i].rgbTiles;
      if (ta.has_value() != tb.has_value()) return false;
      if (!ta) continue;
      if (ta->occupiedTileCount() != tb->occupiedTileCount()) return false;
      for (const auto& [coord, tile] : *ta) {
        const Tile* other = tb->find(coord);
        if (!other) return false;
        if (std::memcmp(tile.data(), other->data(), sizeof(Tile)) != 0) return false;
      }
    }
    return true;
  };
  // Every `np:*` layer attribute io/NpaintFile writes, compared field by
  // field -- the journal claims to recover the model, and the model is these
  // as much as it is the pixels.
  auto layerMetadataIdentical = [](const Document& a, const Document& b) {
    if (a.layers.size() != b.layers.size()) return false;
    if (a.width != b.width || a.height != b.height) return false;
    for (size_t i = 0; i < a.layers.size(); ++i) {
      const Layer& x = a.layers[i];
      const Layer& y = b.layers[i];
      if (x.kind != y.kind || x.name != y.name || x.blend != y.blend) return false;
      if (x.opacity != y.opacity || x.visible != y.visible || x.locked != y.locked) return false;
      if (x.parent != y.parent) return false;
    }
    return true;
  };

  // The same fixture shape the lifecycle section uses: two layers, every
  // metadata field set to a non-default value, plus a carry holding data this
  // build has no knowledge of. PRD I10 has to survive a journal round trip
  // exactly as it survives a save -- and it does so for the same reason, that
  // the journal calls saveNpaint() with the document's own carry.
  auto makeFixtureCarry = []() {
    NpaintCarry carry;
    NpaintAttribute s;
    s.name = "np:futureNote";
    s.type = NpaintAttribute::Type::String;
    s.stringValue = "written by a newer build";
    NpaintAttribute i;
    i.name = "np:futureRevision";
    i.type = NpaintAttribute::Type::Int;
    i.intValue = 424242;
    carry.documentAttributes = {s, i};

    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    pigment.channelNames = {"pig.c0", "pig.m"};
    pigment.sampleTypeName = "float";
    {
      const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                              0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
      pigment.rawPixels.resize(sizeof(vals));
      std::memcpy(pigment.rawPixels.data(), vals, sizeof(vals));
    }
    NpaintAttribute pk;
    pk.name = "np:kind";
    pk.type = NpaintAttribute::Type::String;
    pk.stringValue = "Pigment";
    pigment.attributes = {pk};
    carry.rawParts = {pigment};
    carry.partOrder = {{NpaintPartSlot::Kind::Layer, 0},
                       {NpaintPartSlot::Kind::RawPart, 0},
                       {NpaintPartSlot::Kind::Layer, 1}};
    carry.layerPartNames = {"L0001", "L0003"};
    carry.basis = "future-basis-v9";
    return carry;
  };
  auto makeFixtureDocument = [&]() {
    Document doc = Document::createBlank(256, 128, WorkingSpace{});
    doc.layers[0].name = "Bottom";
    doc.layers[0].blend = "multiply";
    doc.layers[0].opacity = 0.375f;
    doc.layers[0].visible = false;
    doc.layers[0].locked = true;
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    second.parent = "L0009";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 0.5f);
    return doc;
  };
  auto carryIntact = [](const NpaintCarry& c) {
    if (c.documentAttributes.size() != 2) return false;
    bool sawNote = false, sawRev = false;
    for (const NpaintAttribute& a : c.documentAttributes) {
      if (a.name == "np:futureNote" && a.stringValue == "written by a newer build") sawNote = true;
      if (a.name == "np:futureRevision" && a.intValue == 424242) sawRev = true;
    }
    if (!(sawNote && sawRev)) return false;
    if (c.rawParts.size() != 1) return false;
    const NpaintRawPart& p = c.rawParts[0];
    if (p.name != "L0002" || p.sampleTypeName != "float") return false;
    if (p.channelNames != std::vector<std::string>{"pig.c0", "pig.m"}) return false;
    const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                            0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
    if (p.rawPixels.size() != sizeof(vals)) return false;
    if (std::memcmp(p.rawPixels.data(), vals, sizeof(vals)) != 0) return false;
    if (c.basis != "future-basis-v9") return false;
    return c.partOrder.size() == 3 && c.partOrder[1].kind == NpaintPartSlot::Kind::RawPart;
  };

  auto writeTextFile = [](const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
  };
  // A second, independent FNV-1a 64, written here rather than exposed from
  // app/Journal on purpose: a test that reuses the implementation it is
  // checking proves only that the implementation agrees with itself. When
  // this one and the module's disagree, the "a hand-built entry is intact"
  // assertion below fails.
  auto fnv1a = [](const std::string& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) {
      h ^= c;
      h *= 1099511628211ull;
    }
    return h;
  };

  // --- Availability, and the honest answer for each build -----------------
  {
    check(journalAvailable(), "the journal is available now that this build has a writer");
    const std::string why = journalUnavailableReason();
    check(why.empty(), "...and there is nothing to explain when it is available");
  }

  // --- Where the scratch lives --------------------------------------------
  {
    check(defaultJournalRootPath() == root, "$NP_JOURNAL_DIR overrides the scratch location");
    unsetenv("NP_JOURNAL_DIR");
    const std::string fallback = defaultJournalRootPath();
    setenv("NP_JOURNAL_DIR", root.c_str(), 1);
    check(contains(fallback, "naturalPaint") && contains(fallback, "recovery"),
          "the default is a per-user application-data path under naturalPaint/recovery");
    check(!contains(fallback, "Caches"),
          "...and deliberately NOT under Caches, which the system may purge -- after a "
          "crash the journal is the only copy of the work");
  }

  // --- The timer rule, as a pure function ---------------------------------
  //
  // Asserted in both builds because journalWriteDue() is where the rule
  // lives; JournalSession::tick() calls it and holds no second copy, so a
  // build with no session still checks the timer itself.
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "Timing");
    JournalEntryState never;
    check(journalWriteDue(doc, never, 0.0, 60.0) == JournalDue::No,
          "a clean document is never due -- nothing to lose");

    doc.recordEdit("add layer");  // structural by default
    check(journalWriteDue(doc, never, 0.0, 60.0) == JournalDue::Structural,
          "a dirty document that has never been journalled is due immediately");

    JournalEntryState written;
    written.everWritten = true;
    written.revision = doc.revision;
    written.structuralRevision = doc.structuralRevision;
    written.lastWriteSeconds = 100.0;
    check(journalWriteDue(doc, written, 100.0, 60.0) == JournalDue::No,
          "...and is not due again until something changes");

    OpenDocument painted = doc;
    painted.recordEdit("paint stroke", EditKind::Content);
    check(journalWriteDue(painted, written, 100.0, 60.0) == JournalDue::No,
          "a content edit does NOT trigger a write of its own (ADR-0008 rejects a disk "
          "write per keystroke)");
    check(journalWriteDue(painted, written, 159.9, 60.0) == JournalDue::No,
          "...it waits for the interval");
    check(journalWriteDue(painted, written, 160.0, 60.0) == JournalDue::Interval,
          "...and fires when the interval has elapsed, to the second");

    OpenDocument structural = doc;
    structural.recordEdit("delete layer", EditKind::Structural);
    check(journalWriteDue(structural, written, 100.0, 60.0) == JournalDue::Structural,
          "a STRUCTURAL edit is due at once, not at the next interval (PRD O5)");
    check(structural.structuralRevision == doc.structuralRevision + 1 &&
              painted.structuralRevision == doc.structuralRevision,
          "...because only a structural edit moves structuralRevision");
    check(painted.isDirty() && painted.revision == doc.revision + 1,
          "...while both kinds are equally dirty and equally unsaved");

    JournalEntryState held = written;
    held.overdue = true;
    check(journalWriteDue(doc, held, 100.0, 60.0) == JournalDue::Overdue,
          "a write deferred by a stroke stays due, rather than waiting a whole interval");

    OpenDocument saved = painted;
    saved.savedRevision = saved.revision;
    check(journalWriteDue(saved, written, 1.0e9, 60.0) == JournalDue::No,
          "a saved document is not due at any time -- its content is in the user's file");
  }

  // --- Beginning a session ------------------------------------------------
  {
    JournalSession session;
    std::string beginError;
    const bool begun = session.begin({}, &beginError);
    check(begun, "a journal session begins now that this build can write");
    {
      check(session.active() && contains(session.directory(), root.c_str()),
            "the session directory is under the configured root");
      const std::string name = fs::path(session.directory()).filename().string();
      // PRD O8's "named and dated", in the directory name itself:
      // session-YYYYMMDD-HHMMSS-<pid>.
      bool shaped = name.rfind("session-", 0) == 0 && name.size() >= 24;
      if (shaped)
        for (size_t i = 8; i < 16; ++i)
          if (i != 16 && !std::isdigit(static_cast<unsigned char>(name[i]))) shaped = false;
      check(shaped, "...and is named and dated: session-YYYYMMDD-HHMMSS-<pid>");
      check(fs::exists(session.directory() + "/session.txt", ec) &&
                fs::exists(session.directory() + "/session.lock", ec),
            "...with a dated descriptor and a lock file beside it");

      // The flock probe: our own live session must never be offered back to
      // us. This is the assertion that a pid check could not make, and it is
      // the same mechanism that makes a lock left by a machine that lost
      // power impossible -- the kernel releases it when the holder dies.
      check(discoverRecoverySessions().empty(),
            "a LIVE session is not offered for recovery -- the flock probe, not the pid");

      std::string finishError;
      check(session.finishClean(&finishError) && !fs::exists(session.directory(), ec),
            "a clean shutdown removes the whole scratch directory");
      check(discoverRecoverySessions().empty(),
            "...so a session that ended normally leaves nothing to offer (PRD O8)");
    }
  }

  // --- A hand-built journal entry: the integrity check, in both builds -----
  //
  // The model file here is 64 bytes of nonsense rather than a real `.npaint`,
  // deliberately: what is being checked is that the size-and-hash record is
  // consulted BEFORE the file format reader is, which is what makes a
  // truncated journal a named refusal instead of a half-loaded document. In
  // the build with no reader at all, that ordering is provable -- the refusal
  // must be about truncation and must NOT be io/NpaintFile's missing-backend
  // message.
  {
    const std::string sessionDir = root + "/session-20260101-120000-4242";
    fs::create_directories(sessionDir, ec);
    writeTextFile(sessionDir + "/session.txt",
                  "# naturalPaint recovery session v1\nstartedAtEpoch 1767268800\n"
                  "startedAtLocal 2026-01-01 12:00:00\npid 4242\nend\n");
    const std::string modelPath = sessionDir + "/doc-0001.npaint";
    const std::string modelBody(64, 'x');
    writeTextFile(modelPath, modelBody);

    auto sidecar = [&](uint64_t bytes, uint64_t hash, bool terminate) {
      std::string s = "# naturalPaint journal entry v1";
      s += "\nid 7";
      s += "\nslot 1";
      s += "\nmodel doc-0001.npaint";
      s += "\nmodelBytes " + std::to_string(bytes);
      s += "\nmodelHash " + std::to_string(hash);
      s += "\npath /tmp/some/where/painting.npaint";
      s += "\ntitle ";
      s += "\ndisplayName painting.npaint";
      s += "\nrevision 4";
      s += "\nsavedRevision 1";
      s += "\nstructuralRevision 2";
      s += "\nresidency Eager";
      s += "\neditsDropped 0";
      s += "\nedit place image as layer";
      s += "\nedit duplicate";
      s += "\nunsavedSummary 3 unsaved changes";
      s += "\nwrittenAtEpoch 1767268860";
      s += "\nwrittenAtLocal 2026-01-01 12:01:00\n";
      if (terminate) s += "end\n";
      return s;
    };
    const std::string sidecarPath = sessionDir + "/doc-0001.journal";

    // (1) Sound entry: size and hash agree.
    writeTextFile(sidecarPath, sidecar(64, fnv1a(modelBody), true));
    std::vector<RecoverySession> found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1,
          "an unclean scratch directory is discovered, with its journalled document");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      check(found[0].startedAtLocal == "2026-01-01 12:00:00" && found[0].pid == 4242,
            "...named and dated from what the session recorded, not from a file mtime "
            "(which would date it to the crash)");
      check(found[0].documents[0].intact && found[0].documents[0].problem.empty(),
            "...and its integrity record checks out (two independent FNV-1a agreeing)");
      check(found[0].documents[0].displayName == "painting.npaint" &&
                found[0].documents[0].boundPath == "/tmp/some/where/painting.npaint" &&
                found[0].documents[0].unsavedSummary == "3 unsaved changes",
            "...offered by name, with the file it came from and what is unsaved");

      OpenDocument out;
      const DocumentOpResult r = recoverDocument(found[0].documents[0], &out);
      check(!r.ok && !contains(r.error, "truncated") && !contains(r.error, "hash"),
            "an intact entry gets as far as the format reader (this one is not a real "
            ".npaint, so the reader is what refuses it)");
    }

    // (2) Truncated model: the size disagrees.
    writeTextFile(modelPath, std::string(40, 'x'));
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 && !found[0].documents[0].intact,
          "a TRUNCATED journalled document is detected");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      const RecoveryDocument& entry = found[0].documents[0];
      check(contains(entry.problem, "truncated") && contains(entry.problem, "40") &&
                contains(entry.problem, "64"),
            "...and the refusal names both byte counts rather than saying 'corrupt'");
      OpenDocument out;
      const DocumentOpResult r = recoverDocument(entry, &out);
      check(!r.ok && contains(r.error, "truncated"),
            "...and recovery refuses it on the integrity record, BEFORE the format reader "
            "ever gets it");
      check(fs::exists(modelPath, ec) && fs::exists(sidecarPath, ec),
            "...leaving both journal files exactly where they were: a recovery that can "
            "destroy what it failed to read is worse than none");
    }

    // (3) Right length, wrong contents.
    writeTextFile(modelPath, std::string(63, 'x') + "y");
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 &&
              !found[0].documents[0].intact &&
              contains(found[0].documents[0].problem, "hash"),
          "a model file of the right length but the wrong contents is refused too");

    // (4) The sidecar itself truncated -- the crash that happens *during* the
    // journal write. Rename-into-place makes this unreachable in practice,
    // which is exactly why the reader must still refuse it rather than trust
    // the mechanism.
    writeTextFile(modelPath, modelBody);
    writeTextFile(sidecarPath, sidecar(64, fnv1a(modelBody), false));
    found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].documents.size() == 1 &&
              !found[0].documents[0].intact &&
              contains(found[0].documents[0].problem, "truncated"),
          "a journal entry with no terminating 'end' line is refused, not half-read");

    // (5) Explicit discard is the only thing that deletes.
    found = discoverRecoverySessions();
    std::string discardError;
    check(discardRecoverySession(found[0], &discardError) && !fs::exists(sessionDir, ec),
          "discarding a session is a separate, explicit call -- nothing else deletes");
    check(discoverRecoverySessions().empty(), "...and it is gone from the offer");
  }

  // --- The real thing: write, crash, recover ------------------------------
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.carry = makeFixtureCarry();
    doc.path = "/tmp/np-not-written-to/original.npaint";
    doc.recordEdit("place image as layer");
    const Document before = doc.document;

    DocumentSession documents;
    OpenDocument* live = documents.add(std::move(doc));
    std::string crashedDirectory;

    {
      JournalSession session;
      std::string beginError;
      check(session.begin({}, &beginError), "a session begins for the round trip");
      crashedDirectory = session.directory();

      // A structural edit is due at once, so one tick at t=0 writes it.
      JournalTickResult tickResult = session.tick(documents, {0.0, false});
      check(tickResult.documentsWritten == 1 && tickResult.activeDocumentWritten &&
                tickResult.errors.empty(),
            "the timer writes the ACTIVE document on a tick -- no deactivation anywhere "
            "(PRD O6)");
      check(fs::exists(session.directory() + "/doc-0001.npaint", ec) &&
                fs::exists(session.directory() + "/doc-0001.journal", ec),
            "...as a `.npaint` written by saveNpaint plus its sidecar (PRD O7)");
      check(!fs::exists(session.directory() + "/doc-0001.tmp.npaint", ec),
            "...with the write-to-temp-then-rename temporary gone");
      check(!fs::exists(live->path, ec),
            "...and the user's own file untouched: autosave never writes over it (PRD O9)");

      tickResult = session.tick(documents, {0.1, false});
      check(tickResult.documentsWritten == 0,
            "an unchanged document is not rewritten on every tick");

      // PRD O6, the part the ADR says the old scheme got wrong: a tile
      // changed in the ACTIVE document reaches disk on the timer.
      writeStraight(live->document, 0, 3, 3, 0.875f, 0.5f, 0.25f, 1.0f);
      live->recordEdit("paint stroke on layer 1", EditKind::Content);
      tickResult = session.tick(documents, {0.2, false});
      check(tickResult.documentsWritten == 0,
            "a content edit alone does not write before the interval");
      tickResult = session.tick(documents, {0.2 + kJournalIntervalSeconds, true});
      check(tickResult.documentsWritten == 0 && tickResult.deferredByStroke == 1,
            "a due write that collides with an active stroke is deferred (PRD O10)");
      tickResult = session.tick(documents, {0.3 + kJournalIntervalSeconds, false});
      check(tickResult.documentsWritten == 1 && tickResult.activeDocumentWritten,
            "...and happens on the first tick after the stroke ends, not an interval later");

      // The in-process crash: the session's destructor releases the lock and
      // leaves the directory. No signal, no kill -9, no second process --
      // and it is the same code path an abnormal exit takes, because
      // finishClean() is the only thing that removes anything.
    }
    check(fs::exists(crashedDirectory, ec),
          "a session that ends without finishClean leaves its scratch directory behind");

    std::vector<RecoverySession> found = discoverRecoverySessions();
    check(found.size() == 1 && found[0].directory == crashedDirectory &&
              found[0].documents.size() == 1,
          "...and the now-unlocked directory is offered on the next launch");
    if (found.size() == 1 && found[0].documents.size() == 1) {
      const RecoveryDocument& entry = found[0].documents[0];
      check(entry.intact && entry.displayName == "original.npaint" &&
                entry.boundPath == live->path,
            "the offer names the document and the file it came from");
      check(!found[0].startedAtLocal.empty() && found[0].startedAtLocal.size() == 19,
            "...and is dated (PRD O8)");

      OpenDocument recovered;
      const DocumentOpResult r = recoverDocument(entry, &recovered);
      check(r.ok, "recovery reads the journal back through loadNpaint");
      if (r.ok) {
        check(tilesIdentical(recovered.document, live->document),
              "every tile comes back bit-identical at zero tolerance, the paint stroke "
              "included");
        check(!tilesIdentical(recovered.document, before),
              "...and is genuinely the journalled state, not the state it was opened in");
        check(std::fabs(readStraightRed(recovered.document, 0, 3, 3) - 0.875f) < 1.0e-3f,
              "...so the ACTIVE document's dirty tiles really did reach disk on the timer");
        check(layerMetadataIdentical(recovered.document, live->document),
              "every np:* layer attribute survives -- kind, name, blend, opacity, visible, "
              "locked, parent");
        check(carryIntact(recovered.carry),
              "the PRD I10 carry survives journal -> recover: unknown attributes and a "
              "whole foreign part");
        check(recovered.path == live->path,
              "the recovered document is bound to the file it came from...");
        check(recovered.isDirty() && recovered.revision == live->revision &&
                  recovered.savedRevision == live->savedRevision,
              "...and comes back dirty, with the same revision it was journalled at");
        check(recovered.unsavedEdits.size() == 2 &&
                  recovered.unsavedEdits[0] == "place image as layer" &&
                  recovered.unsavedEdits[1] == "paint stroke on layer 1",
              "...naming what is unsaved, in order, so the user knows what was rescued");
        check(recovered.id != live->id,
              "...with a fresh document id: a recovered document is not the same document");
      }
      check(fs::exists(entry.modelPath, ec),
            "a successful recovery deletes nothing either -- a crash before the user saves "
            "must not cost them the journal twice");
    }

    // A saved document's journal is dropped: its content is in the user's own
    // file, and offering it next launch would train the user to dismiss the
    // one dialog that must not be dismissed reflexively.
    {
      JournalSession session;
      std::string beginError;
      check(session.begin({}, &beginError), "a second session begins");
      session.tick(documents, {0.0, false});
      check(session.entryCount() == 1, "the dirty document is journalled");
      live->savedRevision = live->revision;  // as a successful save leaves it
      const JournalTickResult afterSave = session.tick(documents, {1.0, false});
      check(afterSave.entriesDropped == 1 && session.entryCount() == 0 &&
                !fs::exists(session.directory() + "/doc-0001.npaint", ec),
            "...and its journal is dropped once it is clean and bound (ADR-0008)");
      std::string finishError;
      session.finishClean(&finishError);
    }
    fs::remove_all(crashedDirectory, ec);
  }

  // ========================================================================
  // The asynchronous writer -- PRD O10's sentence, made literally true.
  // ========================================================================
  //
  // Everything above this line runs the journal in its **synchronous**
  // configuration, which is `JournalOptions`' default and is why none of it
  // changed when the writer thread landed: a section that asserts what is on
  // disk after this tick must not have to reason about a race to do it.
  //
  // The cost of that choice is that the configuration users actually run is
  // the one the rest of the suite does not exercise, which is exactly
  // PLAN.md 1.5's "an unexercised path is not a seam". So the async path is
  // exercised here, deliberately and on its own terms: the write landing at
  // all, the coalescing that is the entire reason there is a queue, the
  // snapshot being a copy rather than a view of a document the main thread
  // keeps editing, shutdown not racing `remove_all()` against a live write,
  // and a failure that has no `JournalTickResult` to go into still reaching
  // the one main() prints.
  //
  // **No test below sleeps for a fixed time.** Two primitives replace that:
  // `waitForIdle()`, which blocks until the queue is drained, and
  // `asyncWriteInFlight()`, which lets a test *establish* that a write is
  // genuinely running before testing what happens during one, instead of
  // assuming it from a timer. Where a margin is still relied on it is stated
  // and it is three orders of magnitude (a 32.0 MiB write measured at
  // ~0.085 s below, against enqueues measured in microseconds).

  // One `key value` line out of a sidecar. A fourth reader of that format in
  // this file, and deliberately not the module's: see the FNV note above --
  // a test that parses with the code it is checking proves only that the
  // code agrees with itself.
  auto sidecarValue = [](const std::string& path, const std::string& key) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const size_t space = line.find(' ');
      if (space == std::string::npos) continue;
      if (line.substr(0, space) == key) return line.substr(space + 1);
    }
    return std::string();
  };

  // Blocks until the writer has actually claimed a job, and reports whether
  // it saw one. Returns false rather than looping forever if the write
  // finished first -- that would mean the test's own premise (that there is
  // a window during which a write is running) did not hold, which is a
  // failure to report, not a hang.
  auto waitForInFlight = [](JournalSession& s) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (s.asyncWriteInFlight()) return true;
      if (s.asyncWritesPerformed() > 0) return false;
      std::this_thread::yield();
    }
    return false;
  };

  // The same 2048x2048 / 256-tile / 32.0 MiB fixture the interval arithmetic
  // is measured against below, used here for the opposite reason: a write
  // that takes ~0.085 s is a window wide enough to enqueue into, by a factor
  // of about a thousand over what an enqueue costs.
  auto makeBigDocument = [&]() {
    Document big = Document::createBlank(2048, 2048, WorkingSpace{});
    for (int32_t ty = 0; ty < 16; ++ty)
      for (int32_t tx = 0; tx < 16; ++tx)
        writeStraight(big, 0, tx * 128 + 1, ty * 128 + 1, 0.5f, 0.25f, 0.125f, 1.0f);
    return big;
  };

  auto beginAsync = [](JournalSession& s, std::string* errorOut) {
    JournalOptions options;
    options.asynchronous = true;
    return s.begin(options, errorOut);
  };

  // --- An asynchronous write lands, and does not re-arm itself ------------
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.carry = makeFixtureCarry();
    doc.path = "/tmp/np-not-written-to/async.npaint";
    doc.recordEdit("place image as layer");

    DocumentSession documents;
    OpenDocument* live = documents.add(std::move(doc));

    JournalSession session;
    std::string beginError;
    check(beginAsync(session, &beginError), "an asynchronous session begins");
    const std::string dir = session.directory();

    const JournalTickResult handed = session.tick(documents, {0.0, false});
    check(handed.documentsWritten == 1 && handed.errors.empty(),
          "one tick hands the structural edit to the writer thread");
    session.waitForIdle();
    check(session.asyncWritesPerformed() == 1,
          "...and waitForIdle() returns with exactly one write performed");
    check(fs::exists(dir + "/doc-0001.npaint", ec) &&
              fs::exists(dir + "/doc-0001.journal", ec),
          "...leaving the model and its sidecar on disk, written off the calling thread");
    check(!fs::exists(dir + "/doc-0001.tmp.npaint", ec),
          "...with the write-to-temp-then-rename temporary gone from the writer's path too");
    check(sidecarValue(dir + "/doc-0001.journal", "revision") ==
                  std::to_string(live->revision) &&
              sidecarValue(dir + "/doc-0001.journal", "structuralRevision") ==
                  std::to_string(live->structuralRevision),
          "...and the sidecar's revision and structuralRevision are the snapshot's");
    check(sidecarValue(dir + "/doc-0001.journal", "displayName") == "async.npaint",
          "...including the two fields only an OpenDocument could answer, precomputed "
          "by the calling thread");

    // The failure mode point 5 of the design exists to prevent, asserted
    // directly: if the entry's state were left for the writer to update,
    // journalWriteDue() would still say Structural on the very next tick and
    // every tick after it, snapshotting a whole document each time.
    size_t enqueuedAgain = 0;
    for (int i = 0; i < 8; ++i)
      enqueuedAgain += session.tick(documents, {1.0 + i, false}).documentsWritten;
    session.waitForIdle();
    check(enqueuedAgain == 0 && session.asyncWritesPerformed() == 1,
          "eight further ticks with no new edit enqueue NOTHING -- the entry state is "
          "updated at enqueue time, so nothing re-arms the write every frame");

    std::string finishError;
    check(session.finishClean(&finishError) && !fs::exists(dir, ec),
          "an asynchronous session still finishes clean");
  }

  // --- Coalescing: six ganged toggles are not six document saves ----------
  //
  // The case this whole change exists for. `recordLayerEdit()` makes an
  // eye-icon visibility toggle a *structural* edit, which is due immediately
  // rather than on the interval, so six rapid clicks used to be six full
  // `saveNpaint()` calls of the whole document -- measured at ~1.0 s each on
  // a real 5000x2559 50-layer file, i.e. about six seconds of frozen window.
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeBigDocument();
    doc.recordEdit("fill");

    DocumentSession documents;
    OpenDocument* live = documents.add(std::move(doc));

    JournalSession session;
    std::string beginError;
    check(beginAsync(session, &beginError), "an asynchronous session begins for the gang");
    const std::string dir = session.directory();

    session.tick(documents, {0.0, false});
    check(waitForInFlight(session),
          "the first write is established to be in flight, so what follows really does "
          "queue behind it rather than racing it");

    for (int i = 0; i < 6; ++i) {
      writeStraight(live->document, 0, 5, 5, 0.1f * static_cast<float>(i + 1), 0.0f, 0.0f,
                    1.0f);
      live->recordEdit("toggle layer visibility");
      session.tick(documents, {0.001 * (i + 1), false});
    }
    session.waitForIdle();
    check(session.asyncWritesPerformed() <= 2,
          "six ganged structural edits produce at most TWO writes, not six -- a snapshot "
          "still queued is REPLACED, not appended");
    const NpaintLoadResult landed = loadNpaint(dir + "/doc-0001.npaint");
    check(landed.ok && std::fabs(readStraightRed(landed.document, 0, 5, 5) - 0.6f) < 1.0e-2f,
          "...and the file that lands holds the FINAL state of the gang, not an "
          "intermediate one that was already stale when it was queued");

    std::string finishError;
    session.finishClean(&finishError);
  }

  // --- The snapshot is a snapshot, not a view -----------------------------
  //
  // Two documents, because the point has to be made without a data race even
  // when the code under test is wrong: the big document holds the writer busy
  // while the *second* document's already-queued snapshot sits untouched, and
  // the main thread edits that second document in the meantime. If the queue
  // held a reference to the live document instead of a copy of it, the write
  // would pick up the later edit.
  {
    OpenDocument big;
    big.id = allocateDocumentId();
    big.document = makeBigDocument();
    big.recordEdit("fill");

    OpenDocument subject;
    subject.id = allocateDocumentId();
    subject.document = makeFixtureDocument();
    subject.path = "/tmp/np-not-written-to/subject.npaint";
    writeStraight(subject.document, 0, 7, 7, 0.25f, 0.0f, 0.0f, 1.0f);
    subject.recordEdit("place image as layer");

    DocumentSession documents;
    documents.add(std::move(big));
    OpenDocument* live = documents.add(std::move(subject));

    JournalSession session;
    std::string beginError;
    check(beginAsync(session, &beginError), "an asynchronous session begins for the snapshot");
    const std::string dir = session.directory();

    // One tick enqueues both, in session order: the big document takes slot
    // 1 and is picked up first, the subject takes slot 2 and waits.
    check(session.tick(documents, {0.0, false}).documentsWritten == 2,
          "one tick hands two dirty documents over, in session order");
    check(waitForInFlight(session),
          "the big document's write is in flight, so the subject's snapshot is still "
          "sitting in the queue untouched");

    writeStraight(live->document, 0, 7, 7, 0.875f, 0.0f, 0.0f, 1.0f);
    live->recordEdit("paint stroke", EditKind::Content);
    session.waitForIdle();

    const NpaintLoadResult landed = loadNpaint(dir + "/doc-0002.npaint");
    check(landed.ok &&
              std::fabs(readStraightRed(landed.document, 0, 7, 7) - 0.25f) < 1.0e-2f,
          "the writer wrote the document as it was AT ENQUEUE TIME (0.25), not as the "
          "main thread has since made it (0.875) -- the queue holds a copy, not a view");

    std::string finishError;
    session.finishClean(&finishError);
  }

  // --- A failure the writer had nowhere to report -------------------------
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.recordEdit("place image as layer");

    DocumentSession documents;
    documents.add(std::move(doc));

    JournalSession session;
    std::string beginError;
    check(beginAsync(session, &beginError), "an asynchronous session begins for the failure");
    const std::string dir = session.directory();

    // Take write permission off the scratch directory, so `saveNpaint()`
    // cannot create its file. A permission failure rather than a full disk
    // because it is the one write failure a test can produce on demand and
    // undo again on the next line but one.
    fs::permissions(dir, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, ec);
    const JournalTickResult handed = session.tick(documents, {0.0, false});
    check(handed.errors.empty(),
          "the tick that enqueues a doomed write reports nothing -- it cannot yet know");
    session.waitForIdle();
    const JournalTickResult later = session.tick(documents, {1.0, false});
    check(later.errors.size() == 1 && contains(later.errors[0], "journal"),
          "...and the writer thread's failure surfaces through a LATER tick's errors, on "
          "the channel main() already prints");
    check(later.documentsWritten == 0,
          "...without the failure turning every subsequent frame into another attempt");

    fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
    std::string finishError;
    session.finishClean(&finishError);
  }

  // --- Shutdown with a write in flight ------------------------------------
  //
  // `finishClean()` ends with `fs::remove_all(directory_)`. A writer still
  // inside `saveNpaint()` at that moment is writing through a path whose
  // parent is being deleted, and -- worse -- is reading members of an object
  // its owner believes it is finished with. The join has to come first, and
  // this is the assertion that it does.
  {
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeBigDocument();
    doc.recordEdit("fill");

    DocumentSession documents;
    documents.add(std::move(doc));

    JournalSession session;
    std::string beginError;
    check(beginAsync(session, &beginError), "an asynchronous session begins for shutdown");
    const std::string dir = session.directory();

    session.tick(documents, {0.0, false});
    check(waitForInFlight(session), "a write is established to be in flight when shutdown begins");

    std::string finishError;
    const bool finished = session.finishClean(&finishError);
    check(!session.asyncWriteInFlight(),
          "finishClean() joined the writer BEFORE remove_all() -- no write is still "
          "running when it returns");
    check(finished && !fs::exists(dir, ec),
          "...and the scratch directory is removed cleanly, with nothing racing it");
  }

  // --- What the interval costs, measured ----------------------------------
  {
    // io/TileResidency's own "realistic document": 2048x2048, every one of
    // the 256 tiles occupied, i.e. 32.0 MiB of half data. The interval in
    // app/Journal.hpp is derived from this number, so it is measured every
    // run rather than quoted from a comment.
    Document big = Document::createBlank(2048, 2048, WorkingSpace{});
    for (int32_t ty = 0; ty < 16; ++ty)
      for (int32_t tx = 0; tx < 16; ++tx)
        writeStraight(big, 0, tx * 128 + 1, ty * 128 + 1, 0.5f, 0.25f, 0.125f, 1.0f);
    check(big.layers[0].rgbTiles->occupiedTileCount() == 256,
          "the cost fixture really is 256 tiles (32.0 MiB of half data)");

    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = std::move(big);
    doc.recordEdit("fill");

    JournalSession session;
    std::string beginError;
    double writeSeconds = 0.0;
    if (session.begin({}, &beginError)) {
      std::string writeError;
      check(session.writeEntry(doc, &writeError, &writeSeconds), "one journal write of it");

      // Best of three writes to the same slot, not one: writeEntry() keeps
      // the entry's slot and overwrites atomically, so calling it again is
      // still "one journal write of this document", and the 3% ceiling
      // below exists to catch a real regression in the writer rather than a
      // single write that landed on unrelated disk contention.
      for (int rep = 0; rep < 2; ++rep) {
        double repSeconds = 0.0;
        std::string repError;
        check(session.writeEntry(doc, &repError, &repSeconds),
              "and a repeat write of the same slot");
        writeSeconds = std::min(writeSeconds, repSeconds);
      }

      const auto copyStart = std::chrono::steady_clock::now();
      const Document snapshot = doc.document;
      const auto copyEnd = std::chrono::steady_clock::now();
      const double copySeconds = std::chrono::duration<double>(copyEnd - copyStart).count();
      check(snapshot.layers[0].rgbTiles->occupiedTileCount() == 256, "and one deep copy of it");

      const double duty = writeSeconds / kJournalIntervalSeconds;
      std::printf("    [measured] journal write of a 2048x2048 document (256 tiles, 32.0 MiB "
                  "half): %.3f s\n",
                  writeSeconds);
      std::printf("    [measured] interval %.0f s -> %.2f%% duty cycle; a crash loses at most "
                  "%.0f s of edits\n",
                  kJournalIntervalSeconds, duty * 100.0, kJournalIntervalSeconds);
      std::printf("    [measured] deep copy of the same document: %.3f s -- what a background "
                  "writer would still cost this thread without COW tiles (Phase 5 step 6)\n",
                  copySeconds);
      // A ceiling, not a target: the point is to catch an order-of-magnitude
      // regression in the writer, not to police normal variance.
      check(duty < 0.03,
            "one journal write stays under 3% of the interval -- the arithmetic the "
            "interval was chosen from still holds");

      // fsync vs F_FULLFSYNC, which is the measurement app/Journal's
      // durability choice rests on.
      const std::string syncPath = session.directory() + "/sync-probe.bin";
      const std::string payload(1u << 20, 'z');
      auto timeSync = [&](bool full) {
        const int fd = ::open(syncPath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) return -1.0;
        ssize_t wrote = ::write(fd, payload.data(), payload.size());
        (void)wrote;
        const auto t0 = std::chrono::steady_clock::now();
        if (full)
          ::fcntl(fd, F_FULLFSYNC);
        else
          ::fsync(fd);
        const auto t1 = std::chrono::steady_clock::now();
        ::close(fd);
        return std::chrono::duration<double, std::milli>(t1 - t0).count();
      };
      const double plain = timeSync(false);
      const double full = timeSync(true);
      std::printf("    [measured] durability of a 1 MiB write: fsync %.2f ms, F_FULLFSYNC "
                  "%.2f ms (%.1fx) -- why the journal uses the first\n",
                  plain, full, plain > 0.0 ? full / plain : 0.0);
      check(plain >= 0.0 && full >= 0.0, "both durability calls are available on this system");

      std::string finishError;
      session.finishClean(&finishError);
    }
  }

  // --- What discovery costs at launch (PRD A2's budget) -------------------
  {
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<RecoverySession> found = discoverRecoverySessions();
    const auto t1 = std::chrono::steady_clock::now();
    std::printf("    [measured] launch-time discovery over %zu session(s): %.3f ms of PRD "
                "A2's 100 ms cold-start budget\n",
                found.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
  }

  // --- Clean up ------------------------------------------------------------
  fs::remove_all(root, ec);
  check(!fs::exists(root, ec), "every scratch file this section wrote is removed");
  if (previousRoot)
    setenv("NP_JOURNAL_DIR", savedRoot.c_str(), 1);
  else
    unsetenv("NP_JOURNAL_DIR");

  std::printf("[selftest] recovery journal %s\n", ok ? "PASS" : "FAIL");
  return ok;
}



}  // namespace np
