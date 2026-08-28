#include "app/selftest/Support.hpp"

#include "app/BrushLibraryFile.hpp"
#include "app/BrushRowIcon.hpp"
#include "app/DabPreview.hpp"
#include "app/StrokeSession.hpp"
#include "app/selftest/DescFixture.hpp"

namespace np {
namespace {

// --- `.abr` fixtures ------------------------------------------------------
//
// Built byte by byte, exactly as `app/selftest/AbrBrushes.cpp` does and for
// the same two reasons: a real brush pack is somebody else's copyrighted work
// and megabytes besides, and an adversarial fixture is not something you find
// lying around -- it is something you build one field at a time.
//
// `runny_inkers.abr` (a genuine 2.4 MB Kyle Webster pack) does sit in the
// repository root during development and io/AbrBrushes was driven against it
// by hand. It is gitignored, and nothing in --selftest may depend on it: a
// suite that only passes on the machine that has the file is not a suite.

struct FixtureBrush {
  const char* name = "Test Brush";
  double diameterPx = 40.0;
  double spacingPercent = 25.0;
  double roundnessPercent = 100.0;
  double angleDeg = 0.0;
  bool useTipDynamics = false;
  int sizeControl = 0;     // bVTy; 2 is Pen Pressure
  double sizeJitter = 0.0;
};

void appendFixtureBrush(DescFixture& f, const FixtureBrush& s) {
  f.objc("brushPreset", "brushPreset", 5);
  f.key4("Nm  ").textv(s.name);
  f.key4("Brsh").objc("sampledBrush", "sampledBrush", 4u);
  f.key4("Dmtr").untf("#Pxl", s.diameterPx);
  f.key4("Angl").untf("#Ang", s.angleDeg);
  f.key4("Rndn").untf("#Prc", s.roundnessPercent);
  f.key4("Spcn").untf("#Prc", s.spacingPercent);
  f.keyN("useTipDynamics").boolv(s.useTipDynamics);
  f.keyN("minimumDiameter").untf("#Prc", 0.0);
  f.key4("szVr").objc("brVr", "brVr", 3);
  f.key4("bVTy").longv(s.sizeControl);
  f.keyN("jitter").untf("#Prc", s.sizeJitter);
  f.key4("Mnm ").untf("#Prc", 0.0);
}

std::vector<uint8_t> abrBytes(const std::vector<FixtureBrush>& brushes, uint16_t version = 6) {
  DescFixture body;
  body.version();
  body.descriptor("null", "null", 1);
  body.key4("Brsh").vlls(static_cast<uint32_t>(brushes.size()));
  for (const FixtureBrush& b : brushes) appendFixtureBrush(body, b);

  DescFixture f;
  f.u16v(version).u16v(2);
  // An odd-length `samp` in front, which every real pack has and which the
  // section walk therefore has to step over.
  const std::vector<uint8_t> samp{1, 2, 3};
  f.code("8BIM").code("samp").u32v(static_cast<uint32_t>(samp.size()));
  for (const uint8_t b : samp) f.u8v(b);
  f.u8v(0);
  f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(body.bytes.size()));
  for (const uint8_t b : body.bytes) f.u8v(b);
  return f.bytes;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  f.close();
  return static_cast<bool>(f);
}

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

size_t countPresetsOf(const BrushLibrary& lib, uint32_t id) {
  size_t n = 0;
  for (const BrushPreset& p : lib.presets)
    if (p.libraryId == id) ++n;
  return n;
}

bool hasPresetNamed(const BrushLibrary& lib, const char* name) {
  for (const BrushPreset& p : lib.presets)
    if (p.name == name) return true;
  return false;
}

// How many lines a serialised file has. Used to prove a brush name carrying a
// newline did not split its own record in two.
size_t lineCount(const std::string& s) {
  size_t n = 0;
  for (const char c : s)
    if (c == '\n') ++n;
  return n;
}

}  // namespace

// app/BrushLibraryFile: which `.abr` libraries are loaded, remembered across
// launches, read only when a brush from one is picked.
//
// Four things that can each fail silently and expensively:
//
//   * **The preferences file**, which a hand edit or a half-written save must
//     not be able to turn into "your brush libraries are gone".
//   * **Laziness**, which a single eager read defeats without changing any
//     answer -- so it is asserted with a counter, not with an outcome.
//   * **The failed load**, which this codebase has repeatedly shipped as a
//     silent no-op.
//   * **Unload**, which deletes presets and must delete exactly the right
//     ones.
bool runBrushLibraryFileTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // Everything this section writes lives here, and $NP_BRUSH_LIBRARIES points
  // the module at it -- so nothing below can touch
  // ~/Library/Application Support/naturalPaint, which the OIIO-build run is
  // separately verified not to create. Same override mechanism, and the same
  // reason, as $NP_RECENT_DOCUMENTS and $NP_JOURNAL_DIR elsewhere in this
  // suite.
  const std::string root = "selftest_brushlib";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const std::string prefsPath = root + "/brush-libraries.txt";
  const char* previousPrefs = std::getenv("NP_BRUSH_LIBRARIES");
  const std::string savedPrefs = previousPrefs ? previousPrefs : "";
  setenv("NP_BRUSH_LIBRARIES", prefsPath.c_str(), 1);

  check(defaultBrushLibraryFilePath() == prefsPath,
        "brushlib: $NP_BRUSH_LIBRARIES overrides the settings path, so this section cannot "
        "touch the real one");

  const std::string packA = root + "/inkers.abr";
  const std::string packB = root + "/washes.abr";
  // Three and two, and deliberately different counts: an unload that removed
  // "a library's worth" by count rather than by id would pass against two
  // libraries of the same size.
  const std::vector<FixtureBrush> brushesA{
      {"Inker One", 40.0, 25.0, 100.0, 0.0, false, 0, 0.0},
      {"Inker Two", 90.0, 12.0, 40.0, 30.0, false, 0, 0.0},
      // The one with a link, so the "an unloaded row previews without its
      // dynamics" assertion below has something to be about.
      {"Inker Three", 24.0, 30.0, 100.0, 0.0, true, 2, 0.0},
  };
  const std::vector<FixtureBrush> brushesB{
      {"Wash One", 120.0, 8.0, 30.0, 45.0, false, 0, 0.0},
      {"Wash Two", 60.0, 10.0, 100.0, 0.0, false, 0, 0.0},
  };
  check(writeBytes(packA, abrBytes(brushesA)) && writeBytes(packB, abrBytes(brushesB)),
        "brushlib: two `.abr` fixtures written, byte by byte, with no brush pack shipped");

  const MixboxLut lut;  // invalid on purpose: brushTipFor() has a documented
                        // no-LUT branch and this section is about rows, not
                        // pigment.

  // ======================================================================
  // 1. The cache is the icon's INPUTS, and it is complete
  // ======================================================================
  //
  // The claim the whole design rests on (app/BrushLibraryFile.hpp §4): a row
  // stores seven numbers rather than pixels, and that is enough to rasterise
  // the real dab. Asserted by rasterising both ways and comparing bytes --
  // and then asserting the negative case too, because "two images are equal"
  // is worth nothing from a comparison that cannot come out unequal.
  {
    BrushState brush;
    BrushPreset plain;
    plain.name = "Plain";
    plain.model.tip.diameterPx = 52.0f;
    plain.model.tip.hardness = 0.42f;
    plain.model.tip.roundness = 0.55f;
    plain.model.tip.angleDeg = 21.0f;
    plain.load = 1.1f;
    plain.links = BrushLinkSet{};

    const DabPreviewImage fromPreset =
        rasteriseDabPreview(brushPresetIconTips(plain, brush, lut));
    const DabPreviewImage fromRow =
        rasteriseDabPreview(brushRowIconTips(brushRowFor(plain), brush, lut));
    check(fromRow.rgba == fromPreset.rgba && !fromRow.rgba.empty(),
          "brushlib: a cached ROW alone rasterises the byte-identical dab the loaded preset "
          "does -- the cache lost nothing an icon needs");

    // **Inverted from what this section proved before.** The same preset
    // with one link used to previews differently from its row, because the
    // row had no dynamics to preview -- true back when a link was live data
    // `app/DabPreview` resolved into the pictured tip. It is not any more:
    // the matrix is shelved (`ui/DynamicsMatrixPanel.hpp`) and
    // `app/StrokeSession::brushTipFor()` -- what both `brushPresetIconTips()`
    // and `brushRowIconTips()` ultimately call, through `dabPreviewTipsFor()`
    // -- does not read `BrushLinkSet` in any form any more. A link changes
    // nothing about either icon now, so the two rasterise byte-identical,
    // same as the unlinked case just above.
    BrushPreset linked = plain;
    addLink(linked.links, BrushLink{DynamicSource::Pressure, DynamicTarget::Size});
    const DabPreviewImage linkedPreset =
        rasteriseDabPreview(brushPresetIconTips(linked, brush, lut));
    const DabPreviewImage linkedRow =
        rasteriseDabPreview(brushRowIconTips(brushRowFor(linked), brush, lut));
    check(linkedPreset.rgba == linkedRow.rgba && !linkedPreset.rgba.empty(),
          "brushlib: a preset WITH a link previews IDENTICALLY to its row now -- the shelved "
          "matrix has nothing left to be honestly missing");
    check(brushRowFor(linked).linkCount == 1,
          "brushlib: the row still caches the link COUNT even though it no longer changes the "
          "picture -- a row can still say what it is missing structurally");

    // **What proves the comparison above is capable of failing, restated for
    // the new architecture**: two structurally DIFFERENT presets (not a
    // link, which is now inert either way) still rasterise different icons,
    // so "the two images agree" above is a real measurement and not a
    // vacuously-true one from a broken comparison.
    BrushPreset wider = plain;
    wider.model.tip.diameterPx = plain.model.tip.diameterPx * 2.0f;
    const DabPreviewImage widerPreset =
        rasteriseDabPreview(brushPresetIconTips(wider, brush, lut));
    check(widerPreset.rgba != fromPreset.rgba,
          "brushlib: a preset with a genuinely different radius previews differently -- the "
          "rasteriser really can tell two icons apart, so its agreement above means something");
  }

  // ======================================================================
  // 2. Import, then a round trip through the file
  // ======================================================================
  std::string roundTripText;
  uint32_t idA = 0, idB = 0;
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    const size_t builtIns = lib.presets.size();
    BrushLibraryStore store;

    const BrushLibraryLoadResult ra = store.importFile(packA, lib);
    const BrushLibraryLoadResult rb = store.importFile(packB, lib);
    idA = ra.libraryId;
    idB = rb.libraryId;
    check(ra.ok && rb.ok && lib.presets.size() == builtIns + 5,
          "brushlib: importing two libraries appends 3 + 2 presets beside the built-ins");
    check(countPresetsOf(lib, 0) == builtIns && countPresetsOf(lib, idA) == 3 &&
              countPresetsOf(lib, idB) == 2,
          "brushlib: every imported preset carries its own library's id and no built-in "
          "acquired one");
    check(idA != 0 && idB != 0 && idA != idB,
          "brushlib: library ids are nonzero and distinct -- 0 is the built-ins and is not a "
          "library any call can name");

    check(store.importFile(packA, lib).ok && lib.presets.size() == builtIns + 5,
          "brushlib: importing the same path twice does not produce two libraries, which "
          "would put two rows behind every brush and make Remove take only half");

    roundTripText = store.serialize(lib);

    BrushLibraryStore back;
    back.parse(roundTripText);
    bool sameLibraries = back.libraries().size() == 2;
    if (sameLibraries) {
      for (size_t i = 0; i < 2 && sameLibraries; ++i) {
        const RememberedLibrary& before = store.libraries()[i];
        const RememberedLibrary& after = back.libraries()[i];
        sameLibraries = before.path == after.path && before.size == after.size &&
                        before.mtime == after.mtime && before.rows.size() == after.rows.size();
        for (size_t r = 0; r < after.rows.size() && sameLibraries; ++r)
          sameLibraries = brushRowsEqual(before.rows[r], after.rows[r]);
      }
    }
    check(sameLibraries,
          "brushlib: write then read gives back the same libraries -- paths, sizes, mtimes "
          "and every cached row at BIT equality, which `%.9g` is what buys");
    check(back.abrReads() == 0,
          "brushlib: and reading the preferences opened no `.abr` at all");
  }

  // ======================================================================
  // 3. Lazy: rows draw with the `.abr` unread, and one use reads once
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    const size_t builtIns = lib.presets.size();
    BrushLibraryStore store;
    store.parse(roundTripText);
    store.refreshStatuses();
    // The ids are minted fresh by this parse, so they are not idA/idB.
    const uint32_t lazyA = store.libraries()[0].id;
    const uint32_t lazyB = store.libraries()[1].id;

    check(store.abrReads() == 0 && store.statCalls() == 2,
          "brushlib: a launch costs one stat() per remembered library and ZERO `.abr` reads "
          "-- a stat is not a read, and that difference is the whole feature");

    const std::vector<BrushPaneRow> rows = store.paneRows(lib);
    check(rows.size() == builtIns + 5,
          "brushlib: the pane draws every remembered brush before any library is read");
    size_t unloadedRows = 0;
    bool namedFromCache = false;
    for (const BrushPaneRow& r : rows) {
      if (r.presetIndex == kNoPresetIndex) ++unloadedRows;
      if (r.row.name == "Inker Two" && r.presetIndex == kNoPresetIndex) namedFromCache = true;
    }
    check(unloadedRows == 5 && namedFromCache,
          "brushlib: those rows carry no preset index and name their brushes from the cache "
          "-- an eager load fails this by making the count zero");
    check(store.abrReads() == 0,
          "brushlib: drawing the whole pane still read no `.abr` -- the assertion an eager "
          "load cannot survive");

    // §5's first use.
    const BrushLibraryLoadResult first = store.useLibrary(lazyA, lib);
    check(first.ok && store.abrReads() == 1 && countPresetsOf(lib, lazyA) == 3,
          "brushlib: the first use of a row reads its library exactly once and its presets "
          "appear");
    const BrushLibraryLoadResult second = store.useLibrary(lazyA, lib);
    check(second.ok && store.abrReads() == 1,
          "brushlib: a second use reads nothing -- a cache that quietly re-reads gives the "
          "same answers and only the counter can see it");
    check(store.useLibrary(lazyB, lib).ok && store.abrReads() == 2 &&
              countPresetsOf(lib, lazyB) == 2,
          "brushlib: and the OTHER library is still unread until it too is used");

    // The rows that were drawn from the cache now resolve to real presets, and
    // to the same brushes -- otherwise a click would select something other
    // than the row it was aimed at.
    bool resolves = true;
    for (const BrushPaneRow& r : store.paneRows(lib)) {
      if (r.libraryId == 0) continue;
      if (r.presetIndex == kNoPresetIndex) resolves = false;
      else if (lib.presets[r.presetIndex].name != r.row.name) resolves = false;
    }
    check(resolves,
          "brushlib: after loading, every row points at a preset with the name the cache "
          "showed -- a click lands on the brush the user aimed at");
  }

  // ======================================================================
  // 4. A load that FAILS is visible and recoverable
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    const size_t builtIns = lib.presets.size();
    BrushLibraryStore store;
    store.parse(roundTripText);

    // Pack B goes missing between launches: moved, deleted, or on a volume
    // that is not mounted.
    const std::vector<uint8_t> savedB = abrBytes(brushesB);
    fs::remove(packB, ec);
    store.refreshStatuses();
    const uint32_t gone = store.libraries()[1].id;
    check(store.libraries()[1].status == BrushLibraryStatus::Missing &&
              contains(store.libraries()[1].failure, "washes.abr"),
          "brushlib: a library whose file has gone is Missing at launch and its message "
          "NAMES the file");
    check(store.paneRows(lib).size() == builtIns + 5,
          "brushlib: its rows are still drawn -- the names are how a user recognises which "
          "pack went missing");

    const size_t activeBefore = lib.active;
    const float radiusBefore = brush.model.tip.diameterPx;
    const BrushLibraryLoadResult failed = store.useLibrary(gone, lib);
    check(!failed.ok && contains(failed.status, "washes.abr"),
          "brushlib: clicking one of its rows fails with a message naming the file -- not a "
          "silent no-op, which is what a row that just does nothing would be");
    check(lib.active == activeBefore && brush.model.tip.diameterPx == radiusBefore &&
              lib.presets.size() == builtIns,
          "brushlib: and NOTHING moved -- not the active index, not the live brush, not the "
          "preset list. A failed pick must not change the brush in hand");
    check(store.libraries()[1].status == BrushLibraryStatus::Failed ||
              store.libraries()[1].status == BrushLibraryStatus::Missing,
          "brushlib: the row stays in a state the pane can offer Retry and Remove on");

    // Recovery: the volume comes back. The retry is the same call.
    check(writeBytes(packB, savedB), "brushlib: the missing file is restored");
    const BrushLibraryLoadResult retried = store.useLibrary(gone, lib);
    check(retried.ok && countPresetsOf(lib, gone) == 2,
          "brushlib: retrying after the file comes back loads it -- a failure cannot wedge "
          "the row into needing a restart");

    // A file that is there and is not a `.abr` this build reads.
    const std::string bad = root + "/ancient.abr";
    check(writeBytes(bad, abrBytes(brushesA, 1)), "brushlib: a version-1 `.abr` is written");
    BrushLibrary lib2;
    BrushLibraryStore store2;
    const BrushLibraryLoadResult refused = store2.importFile(bad, lib2);
    check(!refused.ok && contains(refused.status, "ancient.abr") &&
              contains(refused.status, "version"),
          "brushlib: a corrupt or unsupported `.abr` is refused with io/AbrBrushes' own "
          "words, prefixed by the file it came from");
    check(store2.libraries().empty(),
          "brushlib: a failed IMPORT leaves nothing behind -- a typed path that was wrong "
          "must not become a permanently broken row in the pane and in the file");
  }

  // ======================================================================
  // 5. A hand-edited, truncated or newer file
  // ======================================================================
  {
    // -- Truncated mid-record. What survives is what was complete.
    const size_t cut = roundTripText.find("row 60 ");  // library B's first row: Wash One, d=120 -> r=60
    check(cut != std::string::npos, "brushlib: the truncation point is where it was meant to be");
    BrushLibraryStore truncated;
    truncated.parse(roundTripText.substr(0, cut));
    check(truncated.libraries().size() == 2 && truncated.libraries()[0].rows.size() == 3 &&
              truncated.libraries()[1].rows.empty(),
          "brushlib: a file cut off mid-library keeps the whole library before the cut and "
          "the surviving header of the one after it -- what was readable, not nothing");
    check(contains(truncated.libraries()[0].path, "inkers.abr") &&
              truncated.libraries()[0].size != 0,
          "brushlib: and that library's path and size came through intact");

    // -- A garbage line. It costs its own line and nothing else.
    std::string garbled = roundTripText;
    garbled.insert(garbled.find("row 20 "), "?!?! this is not a key ????\n");
    BrushLibraryStore withGarbage;
    withGarbage.parse(garbled);
    check(withGarbage.libraries().size() == 2 && withGarbage.libraries()[0].rows.size() == 3 &&
              withGarbage.libraries()[1].rows.size() == 2,
          "brushlib: a garbage line loses no library and no row -- every line is independently "
          "meaningful, so there is nothing for one bad line to take with it");

    // -- A `row` that cannot be read loses exactly that row. Not promoted to
    //    an unknown line, which would keep a corrupt record alive forever.
    std::string shortRow = roundTripText;
    shortRow.insert(shortRow.find("row 20 "), "row 12 0.5 oops\n");
    BrushLibraryStore withShortRow;
    withShortRow.parse(shortRow);
    check(withShortRow.libraries()[0].rows.size() == 3,
          "brushlib: a `row` with too few numbers drops that row and keeps the others");
    bool reEmitted = contains(withShortRow.serialize(BrushLibrary{}), "oops");
    check(!reEmitted,
          "brushlib: and is NOT written back out -- an unreadable record must not outlive "
          "the read that failed on it");

    // -- A key from a later version, in a library's scope. §3: kept, and
    //    written back, or this build silently strips whatever the newer one
    //    added the moment it is run once.
    std::string newer = roundTripText;
    newer.replace(0, newer.find('\n'), "naturalPaint-brush-libraries 99");
    newer.insert(newer.find("row 20 "), "icon-hash 9f3c1a2b\nthumbnail-scale 2\n");
    BrushLibraryStore fromNewer;
    fromNewer.parse(newer);
    check(fromNewer.fileVersion() == 99 && fromNewer.libraries().size() == 2,
          "brushlib: a file written by a later version is READ, not refused -- refusing it "
          "turns 'you opened the older build once' into 'your libraries are gone'");
    const std::string rewritten = fromNewer.serialize(BrushLibrary{});
    check(contains(rewritten, "icon-hash 9f3c1a2b") && contains(rewritten, "thumbnail-scale 2"),
          "brushlib: **both unknown keys survive a read/write cycle verbatim** -- this build "
          "PRESERVES what it does not understand rather than dropping it");
    BrushLibraryStore twice;
    twice.parse(rewritten);
    check(twice.libraries().size() == 2 && twice.libraries()[0].rows.size() == 3 &&
              twice.libraries()[0].unknownLines.size() == 2,
          "brushlib: and a second cycle is stable -- the preserved lines stay with their own "
          "library and do not multiply");

    // -- An unknown line belonging to a library leaves with it, which is
    //    correct: it was that library's data.
    BrushLibrary lib3;
    BrushLibraryStore drop;
    drop.parse(rewritten);
    std::string msg;
    check(drop.unload(drop.libraries()[0].id, lib3, &msg),
          "brushlib: the library carrying the unknown keys is unloaded");
    check(!contains(drop.serialize(lib3), "icon-hash"),
          "brushlib: its preserved lines go with it -- they described a library that is no "
          "longer in the list");

    // -- Degenerate files.
    BrushLibraryStore empty;
    empty.parse("");
    BrushLibrary lib4;
    empty.resolveActive(lib4);
    check(empty.libraries().empty() && empty.unknownLines().empty() && lib4.active == 0,
          "brushlib: an empty file is a fresh install, not an error");
    BrushLibraryStore junk;
    junk.parse("\n\n   \nnot even close\nlibrary\n");
    check(junk.libraries().empty(),
          "brushlib: a file of nothing but noise yields no libraries, and a `library` line "
          "with no path is dropped rather than remembered as unloadable");
  }

  // ======================================================================
  // 6. A brush name that would break the file
  // ======================================================================
  //
  // A name arrives from a `.abr`'s UTF-16 string and can hold anything. One
  // newline inside one name splits its record in two and every line after it
  // is read as something else.
  {
    const std::string nasty = root + "/nasty.abr";
    check(writeBytes(nasty, abrBytes({{"Bad\nName\tHere", 30.0, 20.0, 100.0, 0.0, false, 0,
                                       0.0}})),
          "brushlib: an `.abr` whose brush name contains a newline is written");
    BrushLibrary lib;
    BrushLibraryStore store;
    check(store.importFile(nasty, lib).ok, "brushlib: it imports");
    const std::string text = store.serialize(lib);
    // header + library + size + mtime + one row + active = 6.
    check(lineCount(text) == 6,
          "brushlib: it serialises to exactly one row line -- a raw newline in a brush name "
          "would have made two, and every line after it would parse as a different record");
    BrushLibraryStore back;
    back.parse(text);
    check(back.libraries().size() == 1 && back.libraries()[0].rows.size() == 1 &&
              back.libraries()[0].rows[0].name == "Bad Name Here",
          "brushlib: and comes back as one row whose control characters became spaces");
  }

  // ======================================================================
  // 7. Unload
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    const size_t builtIns = lib.presets.size();
    std::vector<std::string> builtInNames;
    for (const BrushPreset& p : lib.presets) builtInNames.push_back(p.name);
    BrushLibraryStore store;
    const uint32_t a = store.importFile(packA, lib).libraryId;
    const uint32_t b = store.importFile(packB, lib).libraryId;

    // A Duplicate of an imported brush. It must survive the unload: that is
    // the only way to keep one brush from a pack you are removing.
    lib.active = lib.presets.size() - 1;  // "Wash Two"
    applyPresetToBrush(lib.presets[lib.active], brush);
    BrushPreset kept = presetFromBrush("My Wash", brush);
    check(kept.libraryId == 0,
          "brushlib: Duplicate produces a preset the USER owns -- `presetFromBrush()` leaves "
          "libraryId at 0, so no unload can take it");
    lib.presets.push_back(kept);

    std::string message;
    check(store.unload(0, lib, &message) == false && contains(message, "built-in"),
          "brushlib: id 0 is refused by name -- the built-ins are safe because there is no "
          "library with that id, not because a guard remembers to check");
    check(store.unload(9999, lib, &message) == false,
          "brushlib: an id that is not there is refused rather than removing something else");

    check(store.unload(a, lib, &message) && contains(message, "inkers.abr") &&
              contains(message, "3"),
          "brushlib: unloading names the library and how many brushes went");
    check(countPresetsOf(lib, a) == 0 && countPresetsOf(lib, b) == 2,
          "brushlib: it removed exactly that library's presets and left the other library's "
          "alone");
    bool allBuiltInsSurvive = countPresetsOf(lib, 0) == builtIns + 1;
    for (const std::string& n : builtInNames)
      if (!hasPresetNamed(lib, n.c_str())) allBuiltInsSurvive = false;
    check(allBuiltInsSurvive && hasPresetNamed(lib, "My Wash"),
          "brushlib: every built-in survives by name, and so does the Duplicate that was "
          "made from an imported brush");
    check(store.libraries().size() == 1 && !contains(store.serialize(lib), "inkers.abr"),
          "brushlib: and the library leaves the preferences file, or it walks back in next "
          "launch");
  }

  // ======================================================================
  // 8. Unloading the library the ACTIVE preset came from
  // ======================================================================
  //
  // The decision (app/BrushLibraryFile.hpp §6): `active` falls back to the
  // first built-in and **the live brush is not touched**. The visible result
  // is EDITED on a built-in, which is exactly true of a brush in hand that
  // came from a pack that is gone -- and Duplicate/Revert are the recovery,
  // using machinery the pane already has.
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    BrushLibraryStore store;
    const uint32_t a = store.importFile(packA, lib).libraryId;

    // "Inker Two" is diameter 90, so radius 45 -- nothing a built-in has.
    size_t pick = 0;
    for (size_t i = 0; i < lib.presets.size(); ++i)
      if (lib.presets[i].name == "Inker Two") pick = i;
    lib.active = pick;
    applyPresetToBrush(lib.presets[pick], brush);
    const float heldRadius = brush.model.tip.diameterPx / 2.0f;
    check(!brushIsEdited(brush) && heldRadius == 45.0f,
          "brushlib: picking an imported brush leaves the live brush matching it, unedited");

    std::string message;
    check(store.unload(a, lib, &message), "brushlib: its library is unloaded");
    check(lib.active < lib.presets.size() && lib.presets[lib.active].libraryId == 0,
          "brushlib: `active` lands on a preset no library owns -- never on another imported "
          "brush, and never dangling, because the EDITED badge is defined against it");
    check(lib.active == 0 && lib.presets[0].name == "Round Bristle 03",
          "brushlib: specifically the first built-in, which is where a fresh install starts");
    check(brush.model.tip.diameterPx / 2.0f == heldRadius,
          "brushlib: **the live brush did not change.** Tidying a library list must not "
          "alter the mark the user is about to make");
    check(brushIsEdited(brush),
          "brushlib: so the pane shows EDITED on a built-in -- which is TRUE, and is how the "
          "user is told they are holding a brush the library no longer contains");

    // An active preset that SURVIVES an unload must not slide onto its
    // neighbour. Erasing earlier presets shifts every later index down.
    BrushState brush2;
    BrushLibrary& lib2 = brush2.brushLibrary;
    BrushLibraryStore store2;
    const uint32_t a2 = store2.importFile(packA, lib2).libraryId;
    (void)store2.importFile(packB, lib2);
    size_t washTwo = 0;
    for (size_t i = 0; i < lib2.presets.size(); ++i)
      if (lib2.presets[i].name == "Wash Two") washTwo = i;
    lib2.active = washTwo;
    check(store2.unload(a2, lib2, &message) &&
              lib2.presets[lib2.active].name == "Wash Two",
          "brushlib: unloading a library BEFORE the active preset re-points `active` at the "
          "same brush -- leaving the index alone would silently select whatever moved into "
          "the slot");
  }

  // ======================================================================
  // 9. The remembered selection is recorded, never restored at launch
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    BrushLibraryStore store;
    const uint32_t a = store.importFile(packA, lib).libraryId;
    (void)a;
    size_t pick = 0;
    for (size_t i = 0; i < lib.presets.size(); ++i)
      if (lib.presets[i].name == "Inker Three") pick = i;
    lib.active = pick;
    const std::string text = store.serialize(lib);
    check(contains(text, "active 1 2"),
          "brushlib: the active preset is written as (library ordinal, index within it) -- "
          "not a flat index, which would point somewhere else depending on what loaded");

    BrushState fresh;
    BrushLibraryStore relaunch;
    relaunch.parse(text);
    relaunch.refreshStatuses();
    relaunch.resolveActive(fresh.brushLibrary);
    check(relaunch.abrReads() == 0,
          "brushlib: restoring the selection read NO `.abr` -- restoring it eagerly is "
          "precisely the launch cost this whole module exists to refuse");
    check(fresh.brushLibrary.presets[fresh.brushLibrary.active].libraryId == 0,
          "brushlib: so `active` stays on a built-in and the app starts on a brush it has");
    check(relaunch.pendingActiveLibrary() == relaunch.libraries()[0].id &&
              relaunch.pendingActiveRow() == 2,
          "brushlib: the remembered row is recorded instead, so the pane can mark it and the "
          "user pays for it by clicking it");

    // A hand-edited `active` that points nowhere must not leave a dangling
    // index.
    std::string bogus = text;
    bogus.replace(bogus.rfind("active 1 2"), 10, "active 7 44");
    BrushState fresh2;
    BrushLibraryStore bad;
    bad.parse(bogus);
    bad.resolveActive(fresh2.brushLibrary);
    check(fresh2.brushLibrary.active < fresh2.brushLibrary.presets.size() &&
              fresh2.brushLibrary.presets[fresh2.brushLibrary.active].libraryId == 0 &&
              bad.pendingActiveLibrary() == 0,
          "brushlib: an `active` line naming a library that is not there falls back to a "
          "built-in rather than leaving `active` pointing at nothing");
  }

  // ======================================================================
  // 10. The icon cache invalidates when its source changes
  // ======================================================================
  //
  // **Paired assertions, deliberately.** A count that moved proves nothing on
  // its own (a cache that never caches passes it) and an image that differs
  // proves nothing on its own (a cache that never caches passes that too). The
  // three together -- rasterisation count, hit count, and the bytes -- are
  // what a cache has to satisfy.
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    BrushLibraryStore store;
    const uint32_t id = store.importFile(packA, lib).libraryId;
    const BrushRow before = store.find(id)->rows[0];
    const std::string text = store.serialize(lib);

    DabPreviewCache icons;
    const std::vector<uint8_t> firstImage =
        icons.imageFor(brushRowIconTips(before, brush, lut)).rgba;
    check(icons.rasterisations() == 1 && icons.hits() == 0 && !firstImage.empty(),
          "brushlib: the first icon is rasterised");
    (void)icons.imageFor(brushRowIconTips(before, brush, lut));
    check(icons.rasterisations() == 1 && icons.hits() == 1,
          "brushlib: asking again for the same row hits the cache -- without this the "
          "invalidation assertion below would pass against a cache that never caches");

    // The pack is rebuilt with a different first brush AND a different length.
    // **Length rather than mtime**: mtime has one-second granularity on this
    // filesystem, so a rewrite inside the same second would leave it unchanged
    // and make the staleness check time-dependent -- a test that fails once
    // every few hundred runs, which is worse than no test. `sameFile` requires
    // size AND mtime to match, so a size change alone is decisive and is not a
    // race.
    const std::vector<FixtureBrush> rebuilt{
        {"Inker One Reworked Considerably", 240.0, 40.0, 60.0, 15.0, false, 0, 0.0},
        {"Inker Two", 90.0, 12.0, 40.0, 30.0, false, 0, 0.0},
        {"Inker Three", 24.0, 30.0, 100.0, 0.0, true, 2, 0.0},
    };
    check(writeBytes(packA, abrBytes(rebuilt)), "brushlib: the pack is rebuilt, longer");

    BrushState brush2;
    BrushLibrary& lib2 = brush2.brushLibrary;
    BrushLibraryStore relaunch;
    relaunch.parse(text);
    relaunch.refreshStatuses();
    const uint32_t id2 = relaunch.libraries()[0].id;
    check(relaunch.libraries()[0].status == BrushLibraryStatus::Stale,
          "brushlib: the recorded size no longer matches, so the library is Stale -- a cache "
          "that could not tell would draw rows naming brushes the pack no longer contains");
    check(relaunch.useLibrary(id2, lib2).ok, "brushlib: using it re-reads the file");
    const BrushRow after = relaunch.find(id2)->rows[0];
    check(!brushRowsEqual(before, after) && after.name == "Inker One Reworked Considerably" &&
              after.radius == 120.0f,
          "brushlib: the cached row was REPLACED by what the file now says");

    const std::vector<uint8_t> secondImage =
        icons.imageFor(brushRowIconTips(after, brush2, lut)).rgba;
    check(icons.rasterisations() == 2 && secondImage != firstImage,
          "brushlib: **and the icon both re-rasterised AND came out different.** The count "
          "alone passes against a cache that never invalidates; the bytes alone pass against "
          "one that never caches");

    // Restore the fixture, so a later run of this section starts from the same
    // place as this one did.
    check(writeBytes(packA, abrBytes(brushesA)), "brushlib: the pack fixture is restored");
  }

  // ======================================================================
  // 11. Paths this build will not put in a line-based file
  // ======================================================================
  {
    BrushLibrary lib;
    BrushLibraryStore store;
    const BrushLibraryLoadResult r = store.importFile(std::string("/tmp/one\ntwo.abr"), lib);
    check(!r.ok && contains(r.status, "control character") && store.libraries().empty(),
          "brushlib: a path containing a newline is refused at entry rather than sanitised -- "
          "a sanitised path names a different file");
    check(!store.importFile("   ", lib).ok,
          "brushlib: an empty path is refused rather than remembered as a library nobody can "
          "find");
  }

  // ======================================================================
  // 12. The file on disk, end to end
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    BrushLibraryStore store;
    (void)store.importFile(packA, lib);
    std::string err;
    check(store.saveToFile(prefsPath, lib, &err) && err.empty(),
          "brushlib: the preferences file is written to the settings path");

    BrushState fresh;
    BrushLibraryStore relaunch;
    check(relaunch.loadFromFile(prefsPath, fresh.brushLibrary, &err) &&
              relaunch.libraries().size() == 1 && relaunch.libraries()[0].rows.size() == 3 &&
              relaunch.abrReads() == 0,
          "brushlib: and read back at the next launch -- one library, three rows, no `.abr` "
          "opened");

    BrushState none;
    BrushLibraryStore missing;
    check(missing.loadFromFile(root + "/does-not-exist.txt", none.brushLibrary, &err) &&
              err.empty() && missing.libraries().empty(),
          "brushlib: a preferences file that does not exist is a fresh install, not an error "
          "line the user learns to ignore");
  }

  // Restore the environment for whatever runs after this section.
  if (savedPrefs.empty()) unsetenv("NP_BRUSH_LIBRARIES");
  else setenv("NP_BRUSH_LIBRARIES", savedPrefs.c_str(), 1);
  fs::remove_all(root, ec);
  check(!fs::exists(root, ec),
        "brushlib: every file this section wrote is removed, including both `.abr` fixtures");

  std::printf("[selftest] brush library file %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
