#include "app/selftest/Support.hpp"

#include "app/Batch.hpp"
#include "app/Command.hpp"
#include "app/OpenAnyFile.hpp"
#include "io/ActionFile.hpp"
#include "io/ExportAs.hpp"

namespace np {
namespace {

// FNV-1a over a file's whole contents, plus its length.
//
// **Bytes, deliberately, and not an mtime.** PRD P4 is "never partially
// overwrites an input", and the way that gets broken is `fopen(path, "wb")`
// truncating a file it should never have touched. A truncation moves the
// mtime *and* destroys the bytes; a `stat` comparison would notice the first
// and prove nothing about the second, and the second is the whole
// requirement. The length is folded in so that a file truncated to zero --
// the exact failure -- cannot collide with the empty seed.
uint64_t fileDigest(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return 0;
  uint64_t h = 14695981039346656037ull;
  uint64_t n = 0;
  char buffer[4096];
  while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
    const std::streamsize got = in.gcount();
    for (std::streamsize i = 0; i < got; ++i) {
      h ^= static_cast<unsigned char>(buffer[i]);
      h *= 1099511628211ull;
    }
    n += static_cast<uint64_t>(got);
  }
  h ^= n;
  h *= 1099511628211ull;
  return h;
}

Command step(const char* id, const char* key, double value) {
  Command c;
  c.id = id;
  c.params.set(key, JsonValue::number(value));
  return c;
}

}  // namespace

bool runBatchTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("[selftest] batch: one action over many files. The pre-flight is the section that "
              "matters -- fopen(path, \"wb\") truncates BEFORE the first byte is written, so an "
              "output path that names an input destroys it whether or not the encode "
              "succeeds\n");
  std::printf("  PRD P4 is asserted on BYTES, not on an mtime: every input of a thirty-file run "
              "is hashed before and after, because a truncation moves the mtime and destroys "
              "the contents, and only the second of those is the requirement.\n");

  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string root = "selftest_batch";
  const std::string inDir = root + "/in";
  const std::string outDir = root + "/out";
  fs::remove_all(root, ec);
  fs::create_directories(inDir, ec);
  fs::create_directories(outDir, ec);

  auto countFiles = [](const std::string& dir) {
    size_t n = 0;
    std::error_code e;
    for (auto it = fs::directory_iterator(dir, e); it != fs::directory_iterator(); ++it)
      if (it->is_regular_file()) ++n;
    return n;
  };

  // --- Fixtures: thirty real files on disk ----------------------------------
  //
  // Written through the export path rather than hand-built, so they are files
  // this build's own reader accepts: the batch decodes them again, and a
  // fixture the opener refused would make every assertion below a `Skipped`.
  constexpr size_t kFileCount = 30;
  constexpr int32_t kW = 24, kH = 16;
  std::vector<std::string> sources;
  {
    ExportRequest png;  // 8-bit sRGB PNG at document size -- PRD I1's guarantee.
    for (size_t n = 0; n < kFileCount; ++n) {
      OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fixture");
      // A different flat value per file, so an output written from the wrong
      // input would be visible rather than identical.
      const float v = 0.1f + 0.02f * static_cast<float>(n);
      Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          t.writePixel(PixelCoord{x, y}, {v, v * 0.5f, 1.0f - v, 1.0f});
      char name[64];
      std::snprintf(name, sizeof(name), "%s/plate%02zu.png", inDir.c_str(), n);
      std::string error;
      if (!exportDocumentWithRequestToFile(od.document, name, png, &error))
        std::printf("  (fixture write failed: %s)\n", error.c_str());
      sources.emplace_back(name);
    }
  }
  check(countFiles(inDir) == kFileCount, "fixtures: thirty input files are on disk");

  // The action every non-refusal section below runs: one step, one parameter,
  // and it changes every texel -- so "the file was written" and "the action
  // ran" are distinguishable outcomes rather than one.
  Action grade;
  grade.name = "Batch fixture grade";
  grade.steps.push_back(step("adjust_exposure", "stops", 1.5));
  check(findCommand("adjust_exposure") != nullptr,
        "fixture: the action's one step is a command this build has");

  // ======================================================================
  // A. pathsNameTheSameFile() -- the predicate the pre-flight is built on
  // ======================================================================
  //
  // Asserted directly rather than only through a refused run. A run that
  // refuses proves the pre-flight consulted *something*; only this proves it
  // consulted something that knows `./a.png` and `a.png` are one file.
  std::printf("  -- A. spelling is not identity --\n");
  {
    const std::string a = sources[0];
    check(pathsNameTheSameFile(a, a), "predicate: a path is the same file as itself");
    check(!pathsNameTheSameFile(sources[0], sources[1]),
          "predicate: two different files are not the same file");
    check(!pathsNameTheSameFile(sources[0], ""),
          "predicate: an empty path names nothing, and collides with nothing");

    // Both exist -> the filesystem's own answer.
    check(pathsNameTheSameFile(inDir + "/./plate00.png", a),
          "predicate: './' in the middle does not make a second file");
    check(pathsNameTheSameFile(inDir + "/../in/plate00.png", a),
          "predicate: a '..' segment does not make a second file");
    check(pathsNameTheSameFile(root + "/in/plate00.png", "./" + a),
          "predicate: a leading './' does not make a second file");

    // Case. Which answer is correct depends on the volume this build is on,
    // so the volume is asked rather than assumed -- an assertion that hard-
    // coded "case-insensitive" would be a claim about the tester's disk.
    const std::string shouted = inDir + "/PLATE00.PNG";
    const bool caseInsensitiveVolume = fs::exists(fs::path(shouted), ec) && !ec;
    std::printf("     (this volume is case-%s for filenames)\n",
                caseInsensitiveVolume ? "INsensitive" : "sensitive");
    check(pathsNameTheSameFile(shouted, a) == caseInsensitiveVolume,
          "predicate: a case difference collides iff the volume says it does");

    // Neither exists -> the lexical branch, which is the one the `equivalent`
    // branch can never stand in for.
    check(pathsNameTheSameFile(outDir + "/nothing/../ghost.png", outDir + "/ghost.png"),
          "predicate: two paths that do NOT exist still normalise together");
    check(!pathsNameTheSameFile(outDir + "/ghost.png", outDir + "/other.png"),
          "predicate: and two that do not exist and differ stay apart");
  }

  // ======================================================================
  // B. The pre-flight refuses the WHOLE run, before anything is opened
  // ======================================================================
  std::printf("  -- B. an output that names an input refuses the run --\n");
  {
    // Every output lands back in the input directory under the input's own
    // stem, so plate00.png's output IS plate00.png.
    BatchRequest r;
    r.action = grade;
    r.sources = sources;
    r.outputDirectory = inDir;
    const BatchReport report = planBatch(r);
    check(!report.error.empty() && !report.ok, "collision: the plan refuses");
    check(contains(report.error, "plate00.png") && contains(report.error, "same file as the input"),
          "collision: and the refusal names the file and says why");
    check(report.written() == 0, "collision: the plan wrote nothing");

    // **This is where the byte hashes earn their keep, and it is deliberately
    // here rather than only around the successful run of section D.** Under
    // the healthy code every path in this suite writes to a directory that
    // holds no inputs, so a hash comparison there can never fail whatever the
    // collision check does -- it would be a decoration. This request is the
    // one that WOULD destroy thirty inputs if the pre-flight let it through,
    // so hashing across it is the assertion that goes red when the check does.
    std::vector<uint64_t> before;
    before.reserve(sources.size());
    for (const std::string& s : sources) before.push_back(fileDigest(s));

    const BatchReport run = runBatch(r);
    bool noneAttempted = run.written() == 0 && run.failed() == 0 && run.skipped() == 0;
    for (const BatchItem& item : run.items)
      if (item.outcome != ExportItemOutcome::NotAttempted) noneAttempted = false;
    check(!run.error.empty() && noneAttempted,
          "collision: and the RUN refuses too, with no item attempted");
    check(countFiles(inDir) == kFileCount, "collision: the input directory is untouched");

    bool allSame = true;
    size_t firstDiffering = 0;
    for (size_t n = 0; n < sources.size(); ++n) {
      if (fileDigest(sources[n]) == before[n]) continue;
      allSame = false;
      firstDiffering = n;
      break;
    }
    if (!allSame)
      std::printf("     (input %zu was overwritten: %s)\n", firstDiffering,
                  sources[firstDiffering].c_str());
    check(allSame,
          "PRD P4: the run that WOULD have overwritten its inputs left their bytes identical");

    // **The case string equality already handles is the easy one.** These
    // spell the same directory three other ways.
    for (const std::string& spelling :
         {root + "/in/.", root + "/./in", root + "/out/../in"}) {
      BatchRequest odd = r;
      odd.outputDirectory = spelling;
      const BatchReport oddReport = planBatch(odd);
      if (oddReport.error.empty()) {
        std::printf("  %-58s FAIL   (%s)\n", "collision: a differently spelled output dir refuses",
                    spelling.c_str());
        ok = false;
      }
    }
    check(true, "collision: '/in/.', '/./in' and '/out/../in' all refuse too");

    // **The case a per-item check would miss**: neither file's output is its
    // own input, so a rule that only compared item i's output against item
    // i's source would let both through. `{index}` is 1-based and zero-padded
    // to two digits, so plate02 (item 1) writes plate01.png and plate01
    // (item 2) writes plate02.png -- each one over the other's input, and the
    // two output names differ, so section C's output/output rule does not
    // fire first and mask this.
    BatchRequest crossed;
    crossed.action = grade;
    crossed.sources = {inDir + "/plate02.png", inDir + "/plate01.png"};
    crossed.outputDirectory = inDir;
    crossed.nameTemplate = "plate{index}";
    const BatchReport crossedReport = planBatch(crossed);
    check(!crossedReport.error.empty() && contains(crossedReport.error, "plate01.png") &&
              contains(crossedReport.error, "plate02.png") &&
              contains(crossedReport.error, "same file as the input"),
          "collision: one file's output naming ANOTHER file's input refuses");
  }

  // ======================================================================
  // C. Two outputs resolving to one file
  // ======================================================================
  std::printf("  -- C. two inputs, one output name --\n");
  {
    BatchRequest r;
    r.action = grade;
    r.sources = {sources[0], sources[1]};
    r.outputDirectory = outDir;
    r.nameTemplate = "one";
    const BatchReport report = planBatch(r);
    check(!report.error.empty() && contains(report.error, "both resolve to the output file"),
          "output collision: two inputs sharing an output name refuse the run");
    check(countFiles(outDir) == 0, "output collision: and nothing was written");
  }

  // ======================================================================
  // D. The thirty-file run, and PRD P4 on bytes
  // ======================================================================
  std::printf("  -- D. thirty files, and every input's bytes unchanged --\n");
  size_t writtenBytesOfFirst = 0;
  {
    std::vector<uint64_t> before;
    before.reserve(sources.size());
    for (const std::string& s : sources) before.push_back(fileDigest(s));

    BatchRequest r;
    r.action = grade;
    r.sources = sources;
    r.outputDirectory = outDir;
    r.nameTemplate = "{name}_graded";
    const BatchReport report = runBatch(r);

    check(report.error.empty() && report.ok, "run: thirty files, no whole-run refusal");
    check(report.written() == kFileCount, "run: thirty files written");
    check(report.skipped() == 0 && report.failed() == 0 && report.notAttempted() == 0,
          "run: and nothing skipped, failed or left unattempted");
    check(countFiles(outDir) == kFileCount, "run: thirty files are in the output directory");

    bool allSame = true;
    size_t firstDiffering = 0;
    for (size_t n = 0; n < sources.size(); ++n) {
      if (fileDigest(sources[n]) == before[n]) continue;
      allSame = false;
      firstDiffering = n;
      break;
    }
    if (!allSame)
      std::printf("     (input %zu changed: %s)\n", firstDiffering, sources[firstDiffering].c_str());
    check(allSame, "PRD P4: every input's BYTES are identical after the run");
    check(countFiles(inDir) == kFileCount, "PRD P4: and no input was added to or removed");

    check(report.items.size() == kFileCount && report.items[0].ordinal == 1 &&
              report.items[0].sourceName == "plate00" &&
              report.items[0].filename == "plate00_graded.png",
          "run: the report names each file, its ordinal and its output name");
    writtenBytesOfFirst = report.items[0].bytesWritten;
    check(writtenBytesOfFirst > 0, "run: and reports how many bytes each one wrote");

    // §4/§5: the action changed texels, so nothing is flagged unchanged, and
    // the import's own note reached the row.
    check(report.unchanged() == 0, "run: the action changed texels, so nothing is 'unchanged'");
    check(report.items[0].stepsRun == 1 && report.items[0].texelsChanged > 0,
          "run: one step ran per file and it changed texels");
    bool sawImportWarning = false;
    for (const std::string& w : report.items[0].warnings)
      if (w.rfind("import: ", 0) == 0) sawImportWarning = true;
    check(sawImportWarning,
          "warnings: the opener's own note reaches the row, prefixed 'import:'");
    check(contains(batchSummary(report), "No input file was modified"),
          "run: the summary says so in one sentence");
  }

  // ======================================================================
  // E. Stop at the first Failed; the rest are NotAttempted
  // ======================================================================
  std::printf("  -- E. a failure at file 5 leaves 6..30 untouched and SAYS so --\n");
  {
    const std::string haltDir = root + "/halt";
    fs::create_directories(haltDir, ec);
    // A *directory* where item 5's output file belongs, so `fopen(.., "wb")`
    // on it fails. A real write failure, produced without a full disk.
    fs::create_directories(haltDir + "/plate04.png", ec);

    std::vector<uint64_t> before;
    for (const std::string& s : sources) before.push_back(fileDigest(s));

    BatchRequest r;
    r.action = grade;
    r.sources = sources;
    r.outputDirectory = haltDir;
    const BatchReport report = runBatch(r);

    check(!report.ok && report.failed() == 1, "halt: exactly one item Failed");
    check(report.items[4].outcome == ExportItemOutcome::Failed,
          "halt: and it is file 5, the one whose path cannot be opened");
    check(report.written() == 4, "halt: the four before it were written");
    check(report.notAttempted() == kFileCount - 5,
          "halt: and every one of the twenty-five after it is NotAttempted");

    bool allNotAttempted = true, allNamed = true;
    for (size_t n = 5; n < kFileCount; ++n) {
      if (report.items[n].outcome != ExportItemOutcome::NotAttempted) allNotAttempted = false;
      if (!contains(report.items[n].reason, "plate04.png")) allNamed = false;
    }
    check(allNotAttempted,
          "halt: NotAttempted, not Skipped -- the two mean different things");
    check(allNamed, "halt: each of them names the file that failed first as its reason");

    // Untouched means untouched: no output file exists for any of them.
    bool noLaterOutput = true;
    for (size_t n = 5; n < kFileCount; ++n) {
      char name[128];
      std::snprintf(name, sizeof(name), "%s/plate%02zu.png", haltDir.c_str(), n);
      if (fs::exists(fs::path(name), ec)) noLaterOutput = false;
    }
    check(noLaterOutput, "halt: and not one of their output files was created");

    bool allSame = true;
    for (size_t n = 0; n < sources.size(); ++n)
      if (fileDigest(sources[n]) != before[n]) allSame = false;
    check(allSame, "halt: every input's bytes are still identical");
    check(contains(batchSummary(report), "not attempted"),
          "halt: the summary never says 'wrote 30' when four landed");
  }

  // ======================================================================
  // F. A file that will not open is a Skip, and the run carries on
  // ======================================================================
  std::printf("  -- F. an unreadable input is that file's problem, not the run's --\n");
  {
    const std::string skipDir = root + "/skip";
    fs::create_directories(skipDir, ec);
    const std::string junk = inDir + "/notapicture.txt";
    {
      std::ofstream out(junk, std::ios::binary);
      out << "this is not an image, and never was\n";
    }
    BatchRequest r;
    r.action = grade;
    r.sources = {sources[0], junk, sources[1]};
    r.outputDirectory = skipDir;
    const BatchReport report = runBatch(r);

    check(report.ok, "skip: the run still succeeds overall");
    check(report.items[1].outcome == ExportItemOutcome::Skipped,
          "skip: the file that would not open is Skipped, not Failed");
    check(report.written() == 2 && report.notAttempted() == 0,
          "skip: and the file AFTER it was still processed");
    check(!report.items[1].reason.empty() && contains(report.items[1].reason, "notapicture"),
          "skip: the reason is the opener's own sentence, naming the file");
    fs::remove(junk, ec);
  }

  // ======================================================================
  // G. A silent no-op is the failure mode this feature is built to have
  // ======================================================================
  std::printf("  -- G. thirty files written UNMODIFIED must never read as thirty successes --\n");
  {
    const std::string noopDir = root + "/noop";
    fs::create_directories(noopDir, ec);
    Action nothing;
    nothing.name = "Exposure by zero stops";
    // Legal, available, succeeds -- and changes not one texel.
    nothing.steps.push_back(step("adjust_exposure", "stops", 0.0));

    BatchRequest r;
    r.action = nothing;
    r.sources = {sources[0], sources[1], sources[2]};
    r.outputDirectory = noopDir;
    const BatchReport report = runBatch(r);

    check(report.written() == 3, "no-op: the three files were written");
    check(report.items[0].texelsChanged == 0 && report.items[0].unchangedByAction,
          "no-op: and the row says the action changed nothing in them");
    check(report.unchanged() == 3, "no-op: all three are counted as unchanged");
    check(contains(batchSummary(report), "UNCHANGED"),
          "no-op: the SUMMARY names them -- a per-file warning in a 30-row report is not "
          "conspicuous");
    bool sawActionWarning = false;
    for (const std::string& w : report.items[0].warnings)
      if (w.rfind("action: ", 0) == 0) sawActionWarning = true;
    check(sawActionWarning,
          "no-op: replayAction()'s own zero-texel warning reaches the row as 'action:'");
  }

  // ======================================================================
  // H. Resolution-dependent parameters
  // ======================================================================
  std::printf("  -- H. a sigma authored on a 2k plate is a different filter on an 8k one --\n");
  {
    const std::string mixedDir = root + "/mixed";
    fs::create_directories(mixedDir, ec);
    // One more fixture, deliberately a different size from the thirty.
    const std::string odd = inDir + "/odd.png";
    {
      OpenDocument od = makeBlankOpenDocument(kW * 2, kH * 2, WorkingSpace{}, "odd");
      Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, {0.4f, 0.2f, 0.6f, 1.0f});
      ExportRequest png;
      std::string error;
      exportDocumentWithRequestToFile(od.document, odd, png, &error);
    }

    Action blur;
    blur.name = "Blur";
    blur.steps.push_back(step("filter_gaussian_blur", "sigma", 2.0));

    BatchRequest mixed;
    mixed.action = blur;
    mixed.sources = {sources[0], odd};
    mixed.outputDirectory = mixedDir;
    const BatchReport mixedReport = planBatch(mixed);
    check(!mixedReport.error.empty(), "sigma: a mixed-resolution source set refuses the run");
    check(contains(mixedReport.error, "sigma") && contains(mixedReport.error, "24x16") &&
              contains(mixedReport.error, "48x32"),
          "sigma: and the refusal names the parameter and BOTH sizes");
    check(countFiles(mixedDir) == 0, "sigma: nothing was written");

    BatchRequest even = mixed;
    even.sources = {sources[0], sources[1]};
    const BatchReport evenReport = runBatch(even);
    check(evenReport.ok && evenReport.written() == 2,
          "sigma: the same action over one resolution runs");

    // The same source set with an action carrying no pixel-unit parameter is
    // not refused -- the rule is about the parameter, not about the set.
    BatchRequest exposureOverMixed;
    exposureOverMixed.action = grade;
    exposureOverMixed.sources = {sources[0], odd};
    exposureOverMixed.outputDirectory = mixedDir;
    exposureOverMixed.nameTemplate = "{index}_{name}";
    const BatchReport exposureReport = runBatch(exposureOverMixed);
    check(exposureReport.ok && exposureReport.written() == 2,
          "sigma: and a mixed set with no pixel-unit parameter is NOT refused");

    // `image_size` counts its width and height in pixels and is deliberately
    // not in the table: "resize to 512x512" means the same thing whatever the
    // source was, and docs/automation-plan.md §8 replays exactly that across
    // three differently-sized documents.
    Action resize;
    resize.name = "Resize";
    Command r512;
    r512.id = "image_size";
    r512.params.set("width", JsonValue::number(12));
    r512.params.set("height", JsonValue::number(8));
    resize.steps.push_back(r512);
    BatchRequest resized;
    resized.action = resize;
    resized.sources = {sources[0], odd};
    resized.outputDirectory = mixedDir;
    resized.nameTemplate = "r_{name}";
    const BatchReport resizedReport = runBatch(resized);
    check(resizedReport.ok && resizedReport.written() == 2,
          "sigma: image_size's width/height are a DESTINATION, not a pixel unit");
  }

  // ======================================================================
  // I. The pixel-unit table cannot rot silently
  // ======================================================================
  std::printf("  -- I. the table is held to the command registry, in both directions --\n");
  {
    const std::vector<std::string> names = pixelUnitParameterNames();
    check(!names.empty(), "table: this build declares some pixel-unit parameters");

    // Direction 1: every entry names a command that exists and a parameter
    // that command advertises. A renamed parameter fails here.
    bool everyEntryReal = true;
    std::string firstBad;
    for (const std::string& name : names) {
      for (const std::string& id : commandsWithPixelUnitParameter(name)) {
        const CommandSpec* spec = findCommand(id);
        if (spec == nullptr) {
          everyEntryReal = false;
          if (firstBad.empty()) firstBad = id + " (no such command)";
          continue;
        }
        if (std::find(spec->paramNames.begin(), spec->paramNames.end(), name) ==
            spec->paramNames.end()) {
          everyEntryReal = false;
          if (firstBad.empty()) firstBad = id + " does not advertise '" + name + "'";
        }
      }
    }
    if (!everyEntryReal) std::printf("     (%s)\n", firstBad.c_str());
    check(everyEntryReal,
          "table: every entry names a real command and a parameter it advertises");

    // Direction 2: every command advertising one of those NAMES is in the
    // table. A new filter reusing `radius` fails here instead of quietly
    // being treated as resolution-independent.
    bool everyUserListed = true;
    std::string firstMissing;
    for (const std::string& name : names) {
      const std::vector<std::string> listed = commandsWithPixelUnitParameter(name);
      for (const CommandSpec& spec : allCommands()) {
        if (std::find(spec.paramNames.begin(), spec.paramNames.end(), name) ==
            spec.paramNames.end())
          continue;
        if (std::find(listed.begin(), listed.end(), std::string(spec.id)) != listed.end()) continue;
        everyUserListed = false;
        if (firstMissing.empty()) firstMissing = std::string(spec.id) + "." + name;
      }
    }
    if (!everyUserListed)
      std::printf("     (%s advertises a pixel-unit parameter name and is not in the table -- "
                  "decide whether it is one, and add it to kPixelUnitParams)\n",
                  firstMissing.c_str());
    check(everyUserListed,
          "table: every command advertising one of those names is listed");
  }

  // ======================================================================
  // J. The refusals that come before any of it
  // ======================================================================
  std::printf("  -- J. the whole-run refusals --\n");
  {
    BatchRequest r;
    r.action = grade;
    r.outputDirectory = outDir;
    check(!planBatch(r).error.empty(), "refusal: an empty source set refuses");

    r.sources = {sources[0]};
    r.outputDirectory = root + "/does-not-exist";
    check(contains(planBatch(r).error, "is not an existing directory"),
          "refusal: a missing output directory refuses, and creates nothing");
    check(!fs::exists(fs::path(root + "/does-not-exist"), ec),
          "refusal: and really did not create it");

    r.outputDirectory = outDir;
    r.nameTemplate = "{nope}";
    check(!planBatch(r).error.empty(), "refusal: an unknown template token refuses");

    r.nameTemplate = "{name}";
    Action fromTheFuture;
    fromTheFuture.name = "Newer build";
    fromTheFuture.steps.push_back(step("filter_from_2027", "amount", 1.0));
    r.action = fromTheFuture;
    const std::string unknown = planBatch(r).error;
    check(contains(unknown, "filter_from_2027") && contains(unknown, "not a command this build"),
          "refusal: a step this build cannot evaluate refuses by name, before any file opens");
  }

  fs::remove_all(root, ec);
  return ok;
}

}  // namespace np
