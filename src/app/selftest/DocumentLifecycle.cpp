#include "app/selftest/Support.hpp"

namespace np {

// ==========================================================================
// Phase 4 step 8 -- document lifecycle (revert, duplicate document, save a
// copy, save incremental, open recent).
// ==========================================================================
bool runDocumentLifecycleTest() {
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

  // All scratch state lives in one directory rather than in `selftest_*`
  // files beside it, because saveDocumentIncremental() *lists its containing
  // directory* to find the highest existing version -- run in the working
  // directory it would scan whatever else happens to be there, and the
  // answers would depend on the developer's file names. Removed
  // unconditionally at the end of this function, including on the paths whose
  // assertions failed.
  const std::string dir = "selftest_lifecycle";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const auto inDir = [&](const char* name) { return dir + "/" + name; };
  const auto touch = [&](const char* name) {
    std::ofstream f(inDir(name), std::ios::binary);
    f << "not a real document; only its name matters to the version scan\n";
  };

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
  // Bit-exact comparison of two documents' tile storage, at zero tolerance --
  // the same claim io/NpaintFile's own round trip makes and for the same
  // reason: a duplicate is a memory copy and a `.npaint` round trip has no
  // rounding stage, so anything but bit equality is a bug.
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

  // The fixture: a two-layer document plus a carry holding data this build has
  // no knowledge of -- three unrecognised document attributes, one
  // unrecognised layer attribute, and a whole foreign `np:kind="Pigment"`
  // part sitting *between* the two layers. Every lifecycle operation that
  // writes is checked against this, because a revert or a duplicate is
  // exactly where PRD I10's carry-through would get quietly dropped.
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
    NpaintAttribute fl;
    fl.name = "np:futureGamma";
    fl.type = NpaintAttribute::Type::Float;
    fl.floatValue = 2.4f;
    carry.documentAttributes = {s, i, fl};

    NpaintAttribute lm;
    lm.name = "np:futureMaskLink";
    lm.type = NpaintAttribute::Type::String;
    lm.stringValue = "M0007";
    carry.layerAttributes = {{lm}, {}};

    NpaintRawPart pigment;
    pigment.name = "L0002";
    pigment.width = 4;
    pigment.height = 2;
    pigment.tileWidth = 4;
    pigment.tileHeight = 2;
    // Already sorted: OpenEXR keeps channels in a sorted ChannelList, so an
    // unsorted fixture would come back reordered for a reason unrelated to
    // preservation (runNpaintFormatTest's own note).
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
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    second.name = "Top";
    doc.layers.push_back(std::move(second));
    writeStraight(doc, 0, 3, 3, 0.5f, 0.25f, 0.125f, 1.0f);
    writeStraight(doc, 1, 200, 100, 0.75f, 0.5f, 0.25f, 0.5f);
    return doc;
  };
  // The carry a file came back with still holds everything the fixture put in
  // -- the three document attributes at their exact values, the layer
  // attribute, and the foreign part's bytes.
  auto carryIntact = [](const NpaintCarry& c) {
    if (c.documentAttributes.size() != 3) return false;
    bool sawNote = false, sawRev = false, sawGamma = false;
    for (const NpaintAttribute& a : c.documentAttributes) {
      if (a.name == "np:futureNote" && a.stringValue == "written by a newer build") sawNote = true;
      if (a.name == "np:futureRevision" && a.intValue == 424242) sawRev = true;
      if (a.name == "np:futureGamma" && a.floatValue == 2.4f) sawGamma = true;
    }
    if (!(sawNote && sawRev && sawGamma)) return false;
    if (c.rawParts.size() != 1) return false;
    const NpaintRawPart& p = c.rawParts[0];
    if (p.name != "L0002" || p.sampleTypeName != "float") return false;
    if (p.channelNames != std::vector<std::string>{"pig.c0", "pig.m"}) return false;
    const float vals[16] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f,
                            0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f};
    if (p.rawPixels.size() != sizeof(vals)) return false;
    if (std::memcmp(p.rawPixels.data(), vals, sizeof(vals)) != 0) return false;
    if (c.basis != "future-basis-v9") return false;
    if (c.partOrder.size() != 3 || c.partOrder[1].kind != NpaintPartSlot::Kind::RawPart)
      return false;
    return true;
  };

  // --- The record and its identity ---------------------------------------
  {
    const DocumentId a = allocateDocumentId();
    const DocumentId b = allocateDocumentId();
    check(a != 0 && b != 0 && b > a, "document ids are non-zero and monotonic");

    OpenDocument blank = makeBlankOpenDocument(64, 32, WorkingSpace{}, "Sketch");
    check(blank.id != 0 && blank.document.width == 64 && blank.document.height == 32 &&
              blank.document.layers.size() == 1,
          "makeBlankOpenDocument wraps Document::createBlank (PRD C7)");
    check(!blank.isDirty() && !blank.hasPath() && blank.unsavedWorkSummary().empty(),
          "a blank document starts clean and unbound -- nothing to lose, nothing to save over");
    check(documentDisplayName(blank) == "Sketch",
          "an unbound document shows its title");

    blank.recordEdit("place image as layer");
    check(blank.isDirty() && blank.revision == 1,
          "recordEdit marks the document dirty");
    check(contains(blank.unsavedWorkSummary(), "1 unsaved change") &&
              contains(blank.unsavedWorkSummary(), "place image as layer"),
          "...and the summary names the edit, not just its existence (PRD I11)");

    // The cap, and that the *count* stays exact past it -- a refusal that
    // said "32 unsaved changes" when there were 40 would be worse than one
    // that said nothing.
    OpenDocument many = makeBlankOpenDocument(8, 8, WorkingSpace{});
    for (int i = 0; i < 40; ++i) many.recordEdit("stroke " + std::to_string(i));
    check(many.unsavedEdits.size() == kMaxTrackedUnsavedEdits &&
              many.unsavedEditsDropped == 40 - kMaxTrackedUnsavedEdits,
          "the unsaved-edit label list is capped but the count is not");
    check(contains(many.unsavedWorkSummary(), "40 unsaved changes") &&
              contains(many.unsavedWorkSummary(), "and 8 more"),
          "...and the summary says exactly how many labels it is not showing");
  }

  // --- The session owns them ----------------------------------------------
  {
    DocumentSession session;
    check(session.empty() && session.active() == nullptr,
          "a fresh session holds no documents and active() is null, not a dangling reference");

    OpenDocument* a = session.add(makeBlankOpenDocument(16, 16, WorkingSpace{}, "A"));
    OpenDocument* b = session.add(makeBlankOpenDocument(16, 16, WorkingSpace{}, "B"));
    check(session.count() == 2 && session.active() == b,
          "adding a document makes it active");
    const DocumentId aId = a->id;
    session.setActive(0);
    check(session.active() == a && session.find(aId) == a,
          "documents are addressable by index and by id");
    // Pointer stability is the reason for the unique_ptr indirection: a
    // dialog holding `a` must not be looking at freed memory because another
    // document was opened.
    for (int i = 0; i < 32; ++i) session.add(makeBlankOpenDocument(8, 8, WorkingSpace{}));
    check(session.find(aId) == a,
          "a pointer into the session survives 32 more documents being opened");

    session.setActive(0);
    std::string closeErr;
    a->recordEdit("stroke");
    check(!session.close(0, false, &closeErr) && contains(closeErr, "unsaved change") &&
              contains(closeErr, "stroke") && session.count() == 34,
          "closing a dirty document refuses and names what would be lost");
    check(session.close(0, true, &closeErr) && session.count() == 33,
          "...and closes when the discard is explicit");
    check(!session.close(999, true, &closeErr) && contains(closeErr, "no open document"),
          "closing an index that is not there refuses rather than crashing");
  }

  // --- Duplicate document --------------------------------------------------
  //
  // Runs in both builds: duplication touches no file.
  {
    OpenDocument source;
    source.id = allocateDocumentId();
    source.document = makeFixtureDocument();
    source.carry = makeFixtureCarry();
    source.path = inDir("original.npaint");
    source.savedRevision = source.revision;

    OpenDocument copy = duplicateDocument(source);

    check(copy.path.empty(),
          "a duplicate does NOT inherit the original's path -- the next Save cannot overwrite "
          "it");
    check(copy.id != source.id && copy.id != 0,
          "a duplicate is a different document, so it gets a different id");
    check(copy.isDirty() && contains(copy.unsavedWorkSummary(), "duplicate of original.npaint"),
          "a duplicate is unsaved from birth and says what it is a duplicate of");
    check(tilesIdentical(copy.document, source.document),
          "every tile is copied bit-identically at zero tolerance");
    check(carryIntact(copy.carry),
          "the PRD I10 carry -- unknown attributes AND the foreign Pigment part -- comes with "
          "the duplicate");
    check(copy.carry.layerPartNames == source.carry.layerPartNames,
          "...including the layer part ids, so np:parent links inside the foreign part still "
          "point where they did");

    // Deep, not shared.
    writeStraight(copy.document, 0, 3, 3, 0.9f, 0.9f, 0.9f, 1.0f);
    check(std::fabs(readStraightRed(source.document, 0, 3, 3) - 0.5f) < 1e-3f &&
              std::fabs(readStraightRed(copy.document, 0, 3, 3) - 0.9f) < 1e-3f,
          "painting on the duplicate does not reach the original (deep tile copy)");

    // The refusal that makes the unbound path safe rather than merely
    // unbound. This one is identical in both builds: it never reaches
    // io/NpaintFile at all.
    const DocumentOpResult noPath = saveDocument(copy);
    check(!noPath.ok && contains(noPath.error, "never been saved") &&
              contains(noPath.error, "Save As"),
          "Save on the duplicate refuses by name instead of writing to the original's path");
  }

  // --- Save incremental: the naming rule ----------------------------------
  //
  // Pure path arithmetic plus a directory listing, so every case below runs
  // and asserts the same answer in both builds.
  {
    std::string next, err;

    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v001.npaint"),
          "an unversioned name gets _v001 (three digits, zero-padded)");

    touch("paint_v001.npaint");
    touch("paint_v003.npaint");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "the gap at _v002 is stepped over, not filled -- a lower number written later "
          "sorts wrong forever");
    check(nextIncrementalPath(inDir("paint_v001.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "incrementing an OLD version still lands above the highest existing one");

    touch("paint_v009.png");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "a sibling with a different extension is a different series and is ignored");

    touch("other_v050.npaint");
    check(nextIncrementalPath(inDir("paint.npaint"), &next, &err) &&
              next == inDir("paint_v004.npaint"),
          "a sibling with a different base name is a different series and is ignored");

    touch("shot_v7.npaint");
    check(nextIncrementalPath(inDir("shot_v7.npaint"), &next, &err) &&
              next == inDir("shot_v8.npaint"),
          "an already-versioned name is incremented, keeping its own zero padding");

    touch("wide_v999.npaint");
    check(nextIncrementalPath(inDir("wide_v999.npaint"), &next, &err) &&
              next == inDir("wide_v1000.npaint"),
          "padding grows rather than truncating: _v999 -> _v1000");

    touch("render001.npaint");
    check(nextIncrementalPath(inDir("render001.npaint"), &next, &err) &&
              next == inDir("render001_v001.npaint"),
          "a trailing number with no _v marker is part of the name, not a version");

    check(!nextIncrementalPath("", &next, &err) && contains(err, "no path"),
          "an empty path refuses by name");
    check(!nextIncrementalPath(dir + "/no-such-directory/a.npaint", &next, &err) &&
              contains(err, "could not list"),
          "a directory that cannot be listed refuses by name rather than guessing _v001");

    // A relative path stays relative: nothing silently absolutises a name the
    // user typed.
    check(nextIncrementalPath("bare.npaint", &next, &err) && next == "bare_v001.npaint",
          "a bare file name with no directory produces a bare file name");
  }

  // --- Open recent: the MRU -----------------------------------------------
  //
  // Also entirely build-independent.
  {
    RecentDocuments recent;
    std::string err;
    check(recent.add("/tmp/np/a.npaint", &err) && recent.add("/tmp/np/b.npaint", &err) &&
              recent.add("/tmp/np/c.npaint", &err) && recent.entries().size() == 3,
          "the recent list records what it is given");
    check(recent.entries()[0].path == "/tmp/np/c.npaint" &&
              recent.entries()[2].path == "/tmp/np/a.npaint",
          "...most recent first");
    check(recent.entries()[0].displayName == "c.npaint",
          "...with the file name alone for a menu");

    recent.add("/tmp/np/a.npaint", &err);
    check(recent.entries().size() == 3 && recent.entries()[0].path == "/tmp/np/a.npaint",
          "re-adding an entry moves it to the front rather than duplicating it");

    recent.add("/tmp/np/./x/../b.npaint", &err);
    check(recent.entries().size() == 3 && recent.entries()[0].path == "/tmp/np/b.npaint",
          "dedup is on the normalised path, so ./x/../b.npaint is the same entry as b.npaint");

    for (int i = 0; i < 15; ++i) recent.add("/tmp/np/f" + std::to_string(i) + ".npaint", &err);
    check(recent.entries().size() == RecentDocuments::kCapacity &&
              recent.entries()[0].path == "/tmp/np/f14.npaint" &&
              recent.entries()[9].path == "/tmp/np/f5.npaint",
          "capacity is 10, oldest first out");

    check(!recent.add("", &err) && contains(err, "empty path"),
          "an empty path is refused by name");
    check(!recent.add("/tmp/np/bad\nname.npaint", &err) && contains(err, "control character") &&
              contains(err, "one path per line"),
          "a path with a newline is refused by name -- the file format is one path per line");

    // Round trip through the file format.
    const std::string text = recent.serialize();
    RecentDocuments reread;
    reread.loadFromString(text, "test");
    check(reread.entries().size() == recent.entries().size() &&
              reread.entries()[0].path == recent.entries()[0].path &&
              reread.entries().back().path == recent.entries().back().path,
          "serialize -> load preserves the list and its order exactly");

    RecentDocuments dup;
    dup.loadFromString("# header\n\n/tmp/np/a.npaint\n/tmp/np/b.npaint\n/tmp/np/a.npaint\n",
                       "test");
    check(dup.entries().size() == 2 && dup.entries()[0].path == "/tmp/np/a.npaint",
          "a file listing the same path twice loads as one entry, at its most recent position");

    RecentDocuments broken;
    broken.loadFromString("/tmp/np/ok.npaint\n\x01" "bad\n", "recent.txt");
    check(broken.entries().size() == 1 && broken.problems().size() == 1 &&
              contains(broken.problems()[0], "recent.txt:2"),
          "an unusable line is skipped, reported with its line number, and does not fail the "
          "load -- a corrupt recent list can never stop the application starting");

    const std::string recentPath = inDir("recent-documents.txt");
    check(recent.saveToFile(recentPath, &err), "the recent list writes to a real file");
    RecentDocuments fromDisk;
    check(fromDisk.loadFromFile(recentPath) &&
              fromDisk.entries().size() == RecentDocuments::kCapacity &&
              fromDisk.entries()[0].path == recent.entries()[0].path,
          "...and reads back identically");
    RecentDocuments absent;
    check(absent.loadFromFile(inDir("no-such-recent.txt")) && absent.entries().empty() &&
              absent.error().empty(),
          "a recent file that does not exist is an empty list, not an error");

    // The location override, so this test never touches the developer's own
    // list -- io/ExportAs' $NP_EXPORT_PRESETS precedent, same directory.
    const char* previous = std::getenv("NP_RECENT_DOCUMENTS");
    const std::string saved = previous ? previous : "";
    setenv("NP_RECENT_DOCUMENTS", "/tmp/np-selftest-recent.txt", 1);
    check(defaultRecentDocumentsPath() == "/tmp/np-selftest-recent.txt",
          "$NP_RECENT_DOCUMENTS overrides the recent-documents file location");
    unsetenv("NP_RECENT_DOCUMENTS");
    const std::string fallback = defaultRecentDocumentsPath();
    check(contains(fallback, "naturalPaint") && contains(fallback, "recent-documents.txt"),
          "...and the default is the same per-user application-data directory as the presets");
    if (previous) setenv("NP_RECENT_DOCUMENTS", saved.c_str(), 1);

    // The missing-entry rule, which is the one PRD-shaped requirement in the
    // MRU: never silently drop, always say why.
    RecentDocuments missing;
    missing.add(inDir("gone.npaint"), &err);
    std::string why;
    check(recentDocumentMissing(inDir("gone.npaint"), &why) && contains(why, "no longer there"),
          "a recent entry whose file is gone is reported missing, with a reason");
    OpenDocument opened;
    const DocumentOpResult openMissing = openRecentDocument(missing, 0, &opened);
    check(!openMissing.ok && contains(openMissing.error, "no longer there") &&
              contains(openMissing.error, "kept in the list"),
          "opening it refuses by name and says the entry was kept, not dropped");
    check(missing.entries().size() == 1,
          "...and the entry really is still there afterwards");
    check(missing.remove(inDir("gone.npaint")) && missing.entries().empty(),
          "removing it is something the user does deliberately");
    check(!recentDocumentMissing(recentPath, &why) && why.empty(),
          "a recent entry whose file exists is not reported missing");
    check(!openRecentDocument(missing, 3, &opened).ok,
          "an out-of-range recent index refuses rather than reading memory it does not own");
  }

  // --- The file-backed operations -----------------------------------------
  {
    const std::string basePath = inDir("doc.npaint");
    OpenDocument doc;
    doc.id = allocateDocumentId();
    doc.document = makeFixtureDocument();
    doc.carry = makeFixtureCarry();

    RecentDocuments recent;
    const DocumentOpResult saved = saveDocumentAs(doc, basePath, {}, &recent);
    check(saved.ok, "Save As writes a .npaint");

    {
      check(doc.path == basePath && !doc.isDirty(),
            "Save As rebinds the document to the new path and clears the dirty state");
      check(recent.entries().size() == 1 &&
                recent.entries()[0].path == normalizeDocumentPath(basePath),
            "...and records it in Open Recent");

      // ---- Save a copy: the whole point is the path NOT changing ----------
      const std::string copyPath = inDir("doc_copy.npaint");
      const uint64_t revisionBefore = doc.revision;
      doc.recordEdit("stroke before the copy");
      const DocumentOpResult copied = saveDocumentCopy(doc, copyPath);
      check(copied.ok, "save a copy writes the file");
      check(doc.path == basePath,
            "SAVE A COPY LEAVES THE DOCUMENT'S PATH UNCHANGED -- the whole distinction from "
            "Save As");
      check(doc.isDirty() && doc.revision == revisionBefore + 1,
            "...and leaves the document dirty, because the copy is not this document's file");
      const NpaintLoadResult copyBack = loadNpaint(copyPath);
      check(copyBack.ok && tilesIdentical(copyBack.document, doc.document),
            "the copy holds the same pixels, bit for bit");
      check(copyBack.ok && carryIntact(copyBack.carry),
            "...and the same PRD I10 carry, so a copy is not where a foreign part is dropped");
      check(recent.entries().size() == 1,
            "save a copy adds nothing to Open Recent -- a copy was never an open document");
      const DocumentOpResult copyOntoSelf = saveDocumentCopy(doc, basePath);
      check(!copyOntoSelf.ok && contains(copyOntoSelf.error, "already bound"),
            "saving a copy onto the document's own file refuses -- that is a Save, not a copy");

      // ---- Revert ---------------------------------------------------------
      // The saved file still holds the pre-edit pixels; change them in memory
      // and prove revert brings the file's version back.
      writeStraight(doc.document, 0, 3, 3, 0.95f, 0.95f, 0.95f, 1.0f);
      doc.recordEdit("paint stroke on layer 1");
      const DocumentOpResult revertRefused = revertDocument(doc);
      check(!revertRefused.ok && contains(revertRefused.error, "paint stroke on layer 1") &&
                contains(revertRefused.error, "2 unsaved changes"),
            "revert refuses a dirty document and NAMES the edits it would discard (PRD I11)");
      check(std::fabs(readStraightRed(doc.document, 0, 3, 3) - 0.95f) < 1e-2f,
            "...and a refused revert changes nothing");

      const DocumentOpResult reverted = revertDocument(doc, {true});
      check(reverted.ok, "revert with the discard confirmed reloads the file");
      check(std::fabs(readStraightRed(doc.document, 0, 3, 3) - 0.5f) < 1e-3f,
            "...and the pixels are the last saved ones again");
      check(!doc.isDirty() && doc.unsavedEdits.empty() && doc.revision > 0,
            "...leaving the document clean, but with a bumped revision so derived caches "
            "re-read");
      check(carryIntact(doc.carry),
            "the carry after a revert is the FILE's carry, foreign part included -- a revert "
            "is not where PRD I10 data goes missing");

      // A revert whose file has gone must not also destroy the copy in
      // memory. This is the case that turns a mistake into data loss if the
      // load is not done into a temporary first.
      const std::string doomedPath = inDir("doomed.npaint");
      OpenDocument doomed;
      doomed.id = allocateDocumentId();
      doomed.document = makeFixtureDocument();
      check(saveDocumentAs(doomed, doomedPath).ok, "a second document saves for the next case");
      writeStraight(doomed.document, 0, 3, 3, 0.42f, 0.42f, 0.42f, 1.0f);
      doomed.recordEdit("stroke");
      fs::remove(doomedPath, ec);
      const DocumentOpResult lost = revertDocument(doomed, {true});
      check(!lost.ok && !lost.error.empty(),
            "revert forwards the loader's own error when the file has gone");
      check(std::fabs(readStraightRed(doomed.document, 0, 3, 3) - 0.42f) < 1e-2f &&
                doomed.isDirty(),
            "...and the in-memory document is left EXACTLY as it was, not half-replaced");

      // ---- Duplicate, then save: the original must survive ----------------
      OpenDocument dup = duplicateDocument(doc);
      writeStraight(dup.document, 0, 3, 3, 0.125f, 0.125f, 0.125f, 1.0f);
      const std::string dupPath = inDir("doc_dup.npaint");
      check(saveDocumentAs(dup, dupPath).ok, "the duplicate saves to its own chosen path");
      const NpaintLoadResult originalAfter = loadNpaint(basePath);
      check(originalAfter.ok &&
                std::fabs(readStraightRed(originalAfter.document, 0, 3, 3) - 0.5f) < 1e-3f,
            "...and the ORIGINAL file on disk is untouched by it");
      check(originalAfter.ok && carryIntact(originalAfter.carry),
            "...still carrying the foreign part it was saved with");

      // ---- Save incremental, for real -------------------------------------
      const DocumentOpResult inc1 = saveDocumentIncremental(doc, {}, &recent);
      check(inc1.ok && inc1.path == inDir("doc_v001.npaint") && doc.path == inc1.path,
            "save incremental writes _v001 and rebinds the document to it");
      check(recent.entries()[0].path == normalizeDocumentPath(inc1.path),
            "...and it becomes the most recent document");
      const DocumentOpResult inc2 = saveDocumentIncremental(doc, {}, &recent);
      check(inc2.ok && inc2.path == inDir("doc_v002.npaint"),
            "a second incremental save writes _v002");
      const NpaintLoadResult v1 = loadNpaint(inDir("doc_v001.npaint"));
      check(v1.ok && carryIntact(v1.carry),
            "_v001 still exists, unoverwritten, carry intact -- an incremental save never "
            "replaces an earlier version");
      check(!doc.isDirty(), "an incremental save leaves the document clean");

      // ---- Open, and the round trip through openNpaintDocument ------------
      OpenDocument reopened;
      RecentDocuments recent2;
      const DocumentOpResult openedDoc = openNpaintDocument(basePath, &reopened, &recent2);
      check(openedDoc.ok && reopened.path == basePath && !reopened.isDirty(),
            "openNpaintDocument binds the path and starts clean");
      check(carryIntact(reopened.carry),
            "...with the file's carry in the record, which is what makes every save above "
            "preserve it");
      check(recent2.entries().size() == 1 &&
                recent2.entries()[0].path == normalizeDocumentPath(basePath),
            "...and opening a document is what Open Recent is a list of");

      // ---- Open Recent end to end -----------------------------------------
      OpenDocument viaRecent;
      const DocumentOpResult recentOpen = openRecentDocument(recent2, 0, &viaRecent);
      check(recentOpen.ok && viaRecent.path == recent2.entries()[0].path &&
                tilesIdentical(viaRecent.document, reopened.document),
            "opening the first Open Recent entry gives back the same document");

      // ---- The tile cache must not serve the previous contents ------------
      //
      // io/TileResidency stamps size+mtime at *open*, so a residency opened
      // after an overwrite passes its own staleness check while OpenImageIO's
      // cache still holds the old tiles. Measured on this build before the
      // invalidation was added: the read below returned the PREVIOUS pixel
      // value and reported success. Every write path here calls
      // tileCacheInvalidate() for exactly that reason.
      const std::string cachePath = inDir("cached.npaint");
      OpenDocument seed;
      seed.id = allocateDocumentId();
      seed.document = makeFixtureDocument();
      check(saveDocumentAs(seed, cachePath).ok, "a document for the residency case saves");
      // Re-opened rather than reused: `npaintLayerTileSource()` is derived
      // from `NpaintCarry::partOrder`, which only a *load* fills in, so a
      // record that has only ever been saved cannot name its own subimages.
      // That is a real limitation of this step and is recorded as such in
      // DocumentLifecycle.hpp rather than papered over here.
      OpenDocument cachedDoc;
      check(openNpaintDocument(cachePath, &cachedDoc).ok,
            "...and re-opening it gives the carry a residency source can be derived from");

      const auto readThroughCache = [&](float* out) {
        *out = -1.0f;
        const std::optional<TileSourceRef> src =
            npaintLayerTileSource(cachePath, cachedDoc.carry, 0);
        if (!src) return false;
        LayerResidency res;
        std::string resErr;
        if (!openCachedLayerResidency(*src, kTileCacheBudgetBytes, &res, &resErr)) return false;
        const TileFetch f = res.readTile(tileCoordAt(PixelCoord{3, 3}));
        if (!f.tile) return false;
        const std::array<float, 4> px = f.tile->readPixel(tileLocalOffset(PixelCoord{3, 3}));
        *out = px[3] > 0.0f ? px[0] / px[3] : 0.0f;
        return true;
      };

      float firstRead = -1.0f;
      check(readThroughCache(&firstRead) && std::fabs(firstRead - 0.5f) < 1e-3f,
            "the cached residency serves the saved pixel");

      writeStraight(cachedDoc.document, 0, 3, 3, 0.25f, 0.25f, 0.25f, 1.0f);
      cachedDoc.recordEdit("stroke");
      check(saveDocument(cachedDoc).ok && !cachedDoc.isDirty(),
            "Save writes over the document's own bound path and clears the dirty state");
      float secondRead = -1.0f;
      readThroughCache(&secondRead);
      std::printf("    [measured] cached read of an overwritten .npaint: %.4f before the "
                  "rewrite, %.4f after (the file now holds 0.2500)\n",
                  static_cast<double>(firstRead), static_cast<double>(secondRead));
      check(std::fabs(secondRead - 0.25f) < 1e-3f,
            "a cached read after a lifecycle write returns the NEW pixels, not the cache's "
            "previous ones");
    }
  }

  // --- Clean up ------------------------------------------------------------
  fs::remove_all(dir, ec);
  check(!fs::exists(dir, ec), "every scratch file this section wrote is removed");

  std::printf("[selftest] document lifecycle %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
