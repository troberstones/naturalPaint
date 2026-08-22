#include "app/selftest/Support.hpp"

#include "io/ExportStates.hpp"

namespace np {

bool runExportStatesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif
  // PLAN.md §1.5, "an unexercised build option is not a seam". A PNG batch is
  // identical in both configurations (PRD I1: PNG has no optional
  // dependency), and part H asserts the EXR answer for *each* build rather
  // than compiling either away.
  std::printf("[selftest] export states: NP_USE_OIIO=%s -- the name template, the plan, the "
              "collision rule and every PNG export answer identically in both configurations; "
              "part H is the seam, where an EXR batch writes four files in ON and is refused "
              "before the first byte in OFF, with io/Export's own string\n",
              kOiioBuild ? "ON" : "OFF");
  std::printf("  PRD I16 + I17 are ONE loop, and the token set is where that shows: there is "
              "one {name} token, not a {comp} and a {layer}, so a template written for comps "
              "works unchanged on layers. Comps and layers differ in exactly two lines of "
              "exportDocumentStates() -- which list is enumerated, and which mutation is "
              "applied to the scratch.\n");
  std::printf("  The brief for this step cited PRD I18 for the name template. I18 is \"revert, "
              "duplicate, save a copy, save incremental, open recent\" (phase 4 step 8). The "
              "row containing the words \"with a name template\" is **I17**.\n");

  namespace fs = std::filesystem;
  std::error_code ec;
  const std::string dir = "selftest_exportstates";
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  auto fileCount = [&]() {
    size_t n = 0;
    std::error_code e;
    for (auto it = fs::directory_iterator(dir, e); it != fs::directory_iterator(); ++it) ++n;
    return n;
  };
  auto readFile = [](const std::string& path) {
    std::vector<uint8_t> bytes;
    std::ifstream in(path, std::ios::binary);
    if (!in) return bytes;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return bytes;
  };

  // --- Fixtures -----------------------------------------------------------
  //
  // Four layers over an opaque base, each a different flat colour, so that a
  // comp switch and a layer isolation are both changes in a *picture* rather
  // than changes in a flag.
  constexpr int32_t kW = 64, kH = 64;
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto fill = [&](Document& doc, size_t layerIndex, const std::array<float, 4>& straight) {
    const std::array<float, 4> pre{straight[0] * straight[3], straight[1] * straight[3],
                                   straight[2] * straight[3], straight[3]};
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) writeRgb(doc, layerIndex, x, y, pre);
  };

  // Four comps of one document: "All on", "Base only", "Red only" and
  // "Green, half". PLAN.md's own verify sentence for this phase is "Export
  // four comps and confirm four correct files with the right names", so the
  // fixture is exactly four.
  auto makeFourCompDoc = [&]() {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers[0].name = "Base";
    addLayer(doc, 1, makeRgbLayer("Red pass"));
    addLayer(doc, 2, makeRgbLayer("Green pass"));
    fill(doc, 0, {0.20f, 0.20f, 0.20f, 1.0f});
    fill(doc, 1, {0.80f, 0.05f, 0.05f, 1.0f});
    fill(doc, 2, {0.05f, 0.80f, 0.05f, 1.0f});

    setLayerVisible(doc, 1, true);
    setLayerVisible(doc, 2, true);
    captureLayerComp(doc, "All on");

    setLayerVisible(doc, 1, false);
    setLayerVisible(doc, 2, false);
    captureLayerComp(doc, "Base only");

    setLayerVisible(doc, 1, true);
    captureLayerComp(doc, "Red only");

    setLayerVisible(doc, 1, false);
    setLayerVisible(doc, 2, true);
    setLayerOpacity(doc, 2, 0.5f);
    captureLayerComp(doc, "Green half");

    // Leave the document in a state that is none of the four, so "the
    // document ends exactly as it started" is a claim with something to be
    // wrong about.
    setLayerVisible(doc, 1, true);
    setLayerOpacity(doc, 2, 0.25f);
    return doc;
  };

  ExportRequest png8;  // PRD I1's no-optional-dependency default.
  png8.format = ImageFormat::Png;
  png8.targetSpace = ExportTargetSpace::Rec709Srgb;
  png8.bitDepth = ExportBitDepth::UInt8;

  // ======================================================================
  // Part A -- the name template (PRD I17), and every hazard in it
  // ======================================================================
  {
    std::string out, err;
    check(resolveExportStateName("{name}", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              out == "Sky.png",
          "template: {name} alone resolves, and the extension comes from the format");
    check(resolveExportStateName("{doc}_{name}_{index}", "Portrait", "Sky", 7, ImageFormat::Png,
                                 &out, &err) &&
              out == "Portrait_Sky_07.png",
          "template: {doc}/{name}/{index}, with the ordinal zero-padded to two digits");
    check(resolveExportStateName("{name}", "", "Sky", 1, ImageFormat::Jpeg, &out, &err) &&
              out == "Sky.jpg",
          "template: the SAME template gives .jpg for a JPEG request -- the extension is "
          "derived, never a token, so a filename cannot disagree with its contents");

    check(resolveExportStateName("{doc}{name}", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              out == "Sky.png",
          "template: an absent {doc} renders as NOTHING and the refusal is deferred to the "
          "resolved name -- substituting 'untitled' would collide two unsaved documents");

    check(!resolveExportStateName("{when}_{name}", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "{when}") && contains(err, "{index}"),
          "template: an unrecognised token is refused, naming it AND the known tokens");
    check(!resolveExportStateName("{date}", "", "Sky", 1, ImageFormat::Png, &out, &err),
          "template: there is deliberately no {date} token -- it would make two runs of one "
          "batch produce different files, so nothing could predict or verify the output");
    check(exportNameTemplateTokens().size() == 3, "template: exactly three tokens, one list");

    check(!resolveExportStateName("{name", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "no matching '}'"),
          "template: an unbalanced '{' is refused by position");
    check(!resolveExportStateName("a}b", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "no matching '{'"),
          "template: a stray '}' is refused by position");

    // The path hazards the brief names, one at a time.
    check(!resolveExportStateName("out/{name}", "", "Sky", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "names a file, not a path"),
          "template literal: a '/' the USER typed refuses once, and says where a folder goes");
    check(!resolveExportStateName("{name}", "", "sky/clouds", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "path separator") && contains(err, "'a/b' and 'a_b'"),
          "substitution: a separator in a LAYER's name is refused, not rewritten to '_' -- "
          "rewriting would collide 'a/b' with 'a_b' and say nothing about either");
    check(!resolveExportStateName("{name}", "", "../../etc/passwd", 1, ImageFormat::Png, &out,
                                  &err),
          "substitution: '../../etc/passwd' cannot escape -- a template resolves to ONE "
          "filename component and '/' is refused wherever it appears");
    check(!resolveExportStateName("{name}", "", "..", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "reserved directory entry"),
          "substitution: '..' on its own is a reserved entry, not a filename");
    check(!resolveExportStateName("{name}", "", "sky:dawn", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "Finder"),
          "substitution: ':' is refused because the macOS Finder DISPLAYS it as '/' -- the file "
          "would exist under a name the user cannot read back");
    check(!resolveExportStateName("{name}", "", ".hidden", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "hidden file"),
          "substitution: a leading '.' is refused -- the export would succeed and the user "
          "would never see the file");
    check(!resolveExportStateName("{name}", "", std::string("a\tb"), 1, ImageFormat::Png, &out,
                                  &err) &&
              contains(err, "control character"),
          "substitution: a control character is refused, io/ExportAs' preset-name rule one "
          "level down");
    check(!resolveExportStateName("{name}", "", "", 1, ImageFormat::Png, &out, &err) &&
              contains(err, "empty filename"),
          "substitution: an empty comp name (which core/LayerCompOps explicitly permits) "
          "resolves to '.png' -- hidden AND a collision magnet -- and is refused");

    const std::string long300(300, 'x');
    check(!resolveExportStateName("{name}", "", long300, 1, ImageFormat::Png, &out, &err) &&
              contains(err, "304") && contains(err, "255"),
          "substitution: a 300-character name is refused with BOTH numbers (304 bytes vs the "
          "255-byte NAME_MAX), not with the word 'long'");
    const std::string long251(251, 'x');
    check(resolveExportStateName("{name}", "", long251, 1, ImageFormat::Png, &out, &err) &&
              out.size() == 255,
          "substitution: 251 + '.png' is exactly 255 and is accepted -- the bound is the real "
          "limit, tested at the edge, not a round number chosen for comfort");
  }

  // ======================================================================
  // Part B -- the plan: everything decided before the first byte
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    ExportStatesRequest req;
    req.source = ExportStateSource::Comps;
    req.format = png8;
    req.outputDirectory = dir;
    req.nameTemplate = "{index}_{name}";

    const ExportStatesReport plan = planStateExport(doc, req);
    check(plan.ok && plan.error.empty() && plan.items.size() == 4,
          "plan: four comps give four rows");
    check(plan.items[0].filename == "01_All on.png" && plan.items[1].filename == "02_Base only.png" &&
              plan.items[2].filename == "03_Red only.png" &&
              plan.items[3].filename == "04_Green half.png",
          "plan: the four filenames, resolved, before anything is written");
    check(plan.items[3].ordinal == 4 && plan.items[3].sourceIndex == 3,
          "plan: ordinal is 1-based position in the selection, sourceIndex is the comp index");
    check(fileCount() == 0, "plan: WRITES NOTHING -- a dialog can show the list for free");

    // A choice of which comps (PRD I17's own words).
    ExportStatesRequest pick = req;
    pick.selection = {3, 1};
    const ExportStatesReport picked = planStateExport(doc, pick);
    check(picked.ok && picked.items.size() == 2 && picked.items[0].sourceIndex == 3 &&
              picked.items[0].ordinal == 1 && picked.items[1].sourceIndex == 1,
          "plan: a selection exports only those comps, in the order given, renumbered from 1");

    ExportStatesRequest bad = req;
    bad.selection = {0, 9};
    const ExportStatesReport badPlan = planStateExport(doc, bad);
    check(!badPlan.ok && contains(badPlan.error, "comp 9") && contains(badPlan.error, "4 comps"),
          "plan: an out-of-range selection is refused with the index AND the count");

    ExportStatesRequest noDir = req;
    noDir.outputDirectory = dir + "/nope";
    check(!planStateExport(doc, noDir).ok &&
              contains(planStateExport(doc, noDir).error, "not an existing directory"),
          "plan: a missing output directory is refused -- nothing is created on your behalf");

    const Document empty = Document::createBlank(kW, kH, WorkingSpace{});
    check(!planStateExport(empty, req).ok &&
              contains(planStateExport(empty, req).error, "no comps to export"),
          "plan: a document with no comps is refused, naming what it has none of");

    // Collisions -- refused, case-insensitively, with both spellings quoted.
    Document twins = makeFourCompDoc();
    twins.comps[1].name = "sky";
    twins.comps[3].name = "SKY";
    ExportStatesRequest byName = req;
    byName.nameTemplate = "{name}";
    const ExportStatesReport clash = planStateExport(twins, byName);
    check(!clash.ok && contains(clash.error, "'sky'") && contains(clash.error, "'SKY'") &&
              contains(clash.error, "without regard to case") && contains(clash.error, "{index}"),
          "collision: 'sky' and 'SKY' are ONE file on APFS, so the batch is refused with both "
          "spellings, the count, and the fix");
    check(exportDocumentStates(twins, byName).written() == 0 && fileCount() == 0,
          "collision: refused BEFORE the first byte -- zero files, not a half-finished batch");
    // The same pair with {index} in the template no longer collides, which
    // proves the refusal is about the resolved name and not about the names.
    check(planStateExport(twins, req).ok,
          "collision: the same two comps with {index} in the template plan cleanly, so the "
          "refusal is about the filename and not about the comp names");
  }

  // ======================================================================
  // Part C -- PLAN.md's verify sentence, literally: "Export four comps and
  // confirm four correct files with the right names"
  // ======================================================================
  std::vector<std::string> fourPaths;
  {
    const Document doc = makeFourCompDoc();
    ExportStatesRequest req;
    req.source = ExportStateSource::Comps;
    req.format = png8;
    req.outputDirectory = dir;
    req.nameTemplate = "{doc}_{index}_{name}";
    req.documentName = "portrait";

    const ExportStatesReport run = exportDocumentStates(doc, req);
    check(run.ok && run.written() == 4 && run.skipped() == 0 && run.failed() == 0,
          "four comps: 4 written, 0 skipped, 0 failed");
    check(fileCount() == 4, "four comps: exactly four files in the directory, no more");

    const char* expected[4] = {"portrait_01_All on.png", "portrait_02_Base only.png",
                               "portrait_03_Red only.png", "portrait_04_Green half.png"};
    bool namesRight = true, sizesRight = true;
    for (size_t i = 0; i < 4; ++i) {
      if (run.items[i].filename != expected[i]) namesRight = false;
      const std::vector<uint8_t> bytes = readFile(run.items[i].path);
      if (bytes.empty() || bytes.size() != run.items[i].bytesWritten) sizesRight = false;
      fourPaths.push_back(run.items[i].path);
    }
    check(namesRight, "four comps: all four names are exactly what the template says");
    check(sizesRight,
          "four comps: every file is on disk and its size equals the reported bytesWritten -- "
          "the report is not bookkeeping about a write that did not happen");

    // Four DIFFERENT pictures. Without this, an exporter that ignored the
    // state-set entirely would still pass every check above.
    bool allDifferent = true;
    std::vector<std::vector<uint8_t>> blobs;
    for (const std::string& p : fourPaths) blobs.push_back(readFile(p));
    for (size_t a = 0; a < blobs.size(); ++a)
      for (size_t b = a + 1; b < blobs.size(); ++b)
        if (blobs[a] == blobs[b]) allDifferent = false;
    check(allDifferent,
          "four comps: the four files are pairwise DIFFERENT bytes -- an exporter that ignored "
          "the state-set would pass every name check above and fail this one");

    // "Correct", defined as something a user can check: each file is what
    // clicking that comp and using File > Export As would produce.
    bool matchesPanelPath = true;
    for (size_t i = 0; i < 4; ++i) {
      Document clicked = doc;
      if (!restoreLayerComp(clicked, i, nullptr).ok) matchesPanelPath = false;
      const ExportResult direct = exportDocumentWithRequest(clicked, png8);
      if (!direct.ok || direct.bytes != blobs[i]) matchesPanelPath = false;
    }
    check(matchesPanelPath,
          "four comps: each file is BYTE-IDENTICAL to clicking that comp in the panel and using "
          "File > Export As -- which is what makes 'correct' checkable by the user");

    // And correct in pixels, decoded back out of the file rather than trusted.
    std::string decodeErr;
    const DecodedImage baseOnly =
        decodeImageLinear(blobs[1].data(), blobs[1].size(), &decodeErr);
    const DecodedImage redOnly = decodeImageLinear(blobs[2].data(), blobs[2].size(), &decodeErr);
    check(baseOnly.valid() && baseOnly.width == 64 && baseOnly.height == 64,
          "four comps: a written file decodes back as a valid 64x64 image");
    const size_t mid = (32u * 64u + 32u) * 4u;
    check(baseOnly.valid() && std::fabs(baseOnly.pixels[mid + 0] - 0.20f) < 0.01f &&
              std::fabs(baseOnly.pixels[mid + 1] - 0.20f) < 0.01f,
          "four comps: 'Base only' really contains the base's grey, read back out of the PNG");
    check(redOnly.valid() && redOnly.pixels[mid + 0] > 0.6f && redOnly.pixels[mid + 1] < 0.15f,
          "four comps: 'Red only' really contains the red pass, read back out of the PNG");

    check(contains(exportStatesSummary(run), "4 of 4"),
          "four comps: the summary states written-of-selected, so 'exported 7 files' can never "
          "be printed when 6 landed");

    std::printf("  four comps written, with what is in each:\n");
    for (size_t i = 0; i < 4; ++i)
      std::printf("    %-34s %6zu bytes  (comp %zu \"%s\", %s)\n", run.items[i].filename.c_str(),
                  run.items[i].bytesWritten, run.items[i].sourceIndex,
                  run.items[i].stateName.c_str(), exportItemOutcomeName(run.items[i].outcome));
  }

  // ======================================================================
  // Part D -- the document ends exactly as it started (the brief's (c))
  // ======================================================================
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fixture");
    od.document = makeFourCompDoc();
    const uint64_t revisionBefore = od.revision;
    const uint64_t structuralBefore = od.structuralRevision;
    const size_t historyBefore = od.history.entries().size();
    const size_t unsavedBefore = od.unsavedEdits.size();
    const ExportResult before = exportDocumentWithRequest(od.document, png8);

    ExportStatesRequest req;
    req.format = png8;
    req.outputDirectory = dir;
    req.nameTemplate = "same_{index}_{name}";
    const ExportStatesReport run = exportDocumentStates(od.document, req);
    check(run.written() == 4, "restoration: the four-comp run under test really wrote four");

    const ExportResult after = exportDocumentWithRequest(od.document, png8);
    check(before.ok && after.ok && before.bytes == after.bytes,
          "restoration: the document composites to BYTE-IDENTICAL bytes after four state "
          "changes -- and it does so structurally, because every entry point takes a const "
          "Document& and the loop works on a copy");
    check(od.revision == revisionBefore && od.structuralRevision == structuralBefore,
          "restoration: the revision counter did not move -- a batch export is not an edit");
    check(od.history.entries().size() == historyBefore && od.unsavedEdits.size() == unsavedBefore,
          "restoration: ZERO history entries and zero unsaved-edit labels for four exports -- "
          "the user's undo does not now contain four junk entries");
    check(!od.isDirty(), "restoration: a clean document is still clean after exporting from it");
    check(od.document.comps.size() == 4 && od.document.nextLayerId != 0,
          "restoration: the comp list and the layer-id counter are untouched");
    // The state the fixture was deliberately left in -- none of the four comps.
    check(od.document.layers[1].visible && od.document.layers[2].opacity == 0.25f,
          "restoration: the exact pre-export state comes back, including a state that is none "
          "of the four comps");
  }

  // ======================================================================
  // Part E -- layers to files (PRD I16, the brief's (d))
  // ======================================================================
  {
    // The Adjustment layer sits in the middle so there is a real item *after*
    // the skip (the ordinal must not renumber), and the clipped layer sits
    // directly above a layer with pixels, because core/LayerOps refuses to
    // clip a layer whose base holds none. "Red pass" covers only the left half
    // of the canvas: a full-canvas opaque layer would make "alone on
    // transparency" and "composited over what is beneath" the same picture,
    // and the comparison below would pass vacuously.
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers[0].name = "Base";
    addLayer(doc, 1, makeAdjustmentLayer("Curves"));
    addLayer(doc, 2, makeRgbLayer("Red pass"));
    addLayer(doc, 3, makeRgbLayer("Clipped highlight"));
    fill(doc, 0, {0.20f, 0.20f, 0.20f, 1.0f});
    const std::array<float, 4> red{0.80f, 0.05f, 0.05f, 1.0f};
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW / 2; ++x) writeRgb(doc, 2, x, y, red);
    fill(doc, 3, {0.05f, 0.05f, 0.90f, 1.0f});
    const LayerOpResult clipResult = setLayerClipped(doc, 3, true);
    check(clipResult.ok && doc.layers[3].clipped,
          "layers: the fixture's clipped layer really is clipped (core/LayerOps refuses to clip "
          "one whose base holds no pixels, so this is not a vacuous setup)");

    const std::string layerDir = dir + "/layers";
    fs::create_directories(layerDir, ec);

    ExportStatesRequest req;
    req.source = ExportStateSource::Layers;
    req.format = png8;
    req.outputDirectory = layerDir;
    req.nameTemplate = "{index}_{name}";

    const ExportStatesReport run = exportDocumentStates(doc, req);
    check(run.items.size() == 4 && run.written() == 3 && run.skipped() == 1 && run.ok,
          "layers: four layers, three files, one skipped -- and the run is still ok, because a "
          "skip is a reported outcome and not a failure");
    check(run.items[1].outcome == ExportItemOutcome::Skipped &&
              contains(run.items[1].reason, "Adjustment") &&
              contains(run.items[1].reason, "no pixels of its own"),
          "layers: an Adjustment layer is REFUSED by name -- isolating one composites to a "
          "fully transparent canvas, and writing that would report success for nothing");
    check(run.items[3].filename == "04_Clipped highlight.png",
          "layers: the ordinal does NOT renumber after a skip -- item 4 is still 04, so a comp "
          "that starts failing cannot rename the files after it");

    // The layer alone on transparency, proven against a hand-built isolation.
    Document isolated = doc;
    for (size_t j = 0; j < isolated.layers.size(); ++j) isolated.layers[j].visible = (j == 2);
    const ExportResult expected = exportDocumentWithRequest(isolated, png8);
    check(expected.ok && readFile(run.items[2].path) == expected.bytes,
          "layers: a layer file is the layer ALONE on transparency, byte-identical to hiding "
          "every other layer -- not the layer composited over what is beneath it");

    // The rejected alternative, run beside the built one (AGENT-BRIEF §5).
    Document prefix = doc;
    for (size_t j = 0; j < prefix.layers.size(); ++j) prefix.layers[j].visible = (j <= 2);
    const ExportResult overBelow = exportDocumentWithRequest(prefix, png8);
    check(overBelow.ok && overBelow.bytes != expected.bytes,
          "layers: the rejected reading -- 'composited over everything beneath' -- really is a "
          "different file, so the choice is a choice; it is one line in the loop "
          "(j <= index instead of j == index) and that is its whole cost");

    // The clipped layer: un-clipped and warned, with the alternative measured.
    check(!run.items[3].warnings.empty() && contains(run.items[3].warnings[0], "unclipped"),
          "layers: an isolated CLIPPED layer is un-clipped and says so -- its clip base is the "
          "layer this state just hid");
    Document stillClipped = doc;
    for (size_t j = 0; j < stillClipped.layers.size(); ++j)
      stillClipped.layers[j].visible = (j == 3);
    const ExportResult clippedOut = exportDocumentWithRequest(stillClipped, png8);
    const std::vector<uint8_t> unclippedOut = readFile(run.items[3].path);
    std::string decErr;
    const DecodedImage clippedImg =
        decodeImageLinear(clippedOut.bytes.data(), clippedOut.bytes.size(), &decErr);
    const DecodedImage unclippedImg =
        decodeImageLinear(unclippedOut.data(), unclippedOut.size(), &decErr);
    const size_t mid = (32u * 64u + 32u) * 4u;
    check(clippedImg.valid() && clippedImg.pixels[mid + 3] == 0.0f,
          "layers: the alternative measured -- leaving the clip on writes a FULLY TRANSPARENT "
          "file (alpha 0 at the centre), which is the silent-empty-file this un-clip avoids");
    check(unclippedImg.valid() && unclippedImg.pixels[mid + 3] > 0.99f &&
              unclippedImg.pixels[mid + 2] > 0.6f,
          "layers: the built answer has the layer's own blue at full alpha in the same texel");

    fs::remove_all(layerDir, ec);
  }

  // ======================================================================
  // Part F -- partial failure (the brief's (e); PRD P4's own acceptance row)
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    const std::string failDir = dir + "/partial";
    fs::remove_all(failDir, ec);
    fs::create_directories(failDir, ec);

    ExportStatesRequest req;
    req.format = png8;
    req.outputDirectory = failDir;
    req.nameTemplate = "{index}";
    // A directory sitting where file 3 must go: fopen("...", "wb") on a
    // directory fails, which is a *write* failure and not an encode refusal --
    // exactly the "disk full / bad path" case (e) asks about.
    fs::create_directories(failDir + "/03.png", ec);
    req.overwriteExisting = true;  // so the pre-flight's exists-check lets it through

    const ExportStatesReport run = exportDocumentStates(doc, req);
    check(!run.ok && run.written() == 2 && run.failed() == 1 && run.notAttempted() == 1,
          "partial failure: file 3 of 4 fails -- 2 written, 1 failed, 1 not attempted");
    check(run.items[2].outcome == ExportItemOutcome::Failed &&
              contains(run.items[2].reason, "03.png"),
          "partial failure: the failing file is named, with the reason, in its own row");
    check(run.items[3].outcome == ExportItemOutcome::NotAttempted &&
              contains(run.items[3].reason, "failed first"),
          "partial failure: the run STOPS -- PRD P4's own row says files 13-40 stay untouched "
          "when file 12 fails");
    check(!fs::exists(failDir + "/04.png", ec),
          "partial failure: file 4 is genuinely not on disk, not merely reported as skipped");
    check(fs::exists(failDir + "/01.png", ec) && fs::exists(failDir + "/02.png", ec),
          "partial failure: the two that landed before it are real files and stay");
    const std::string summary = exportStatesSummary(run);
    check(contains(summary, "2 of 4") && contains(summary, "1 failed") &&
              contains(summary, "not attempted"),
          "partial failure: the summary says 2 of 4, never 'exported 4 files'");

    fs::remove_all(failDir, ec);
  }

  // ======================================================================
  // Part G -- PRD P4's "never partially overwrites an input"
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    const std::string ovDir = dir + "/overwrite";
    fs::remove_all(ovDir, ec);
    fs::create_directories(ovDir, ec);
    {
      std::ofstream seed(ovDir + "/03_Red only.png", std::ios::binary);
      seed << "an existing file that is not ours";
    }

    ExportStatesRequest req;
    req.format = png8;
    req.outputDirectory = ovDir;
    req.nameTemplate = "{index}_{name}";

    const ExportStatesReport refused = exportDocumentStates(doc, req);
    check(!refused.ok && contains(refused.error, "already exists") &&
              contains(refused.error, "0 of 4"),
          "overwrite: an existing output path refuses the WHOLE batch with the counts");
    size_t n = 0;
    for (auto it = fs::directory_iterator(ovDir, ec); it != fs::directory_iterator(); ++it) ++n;
    check(n == 1 && readFile(ovDir + "/03_Red only.png").size() == 33,
          "overwrite: nothing was written and the existing file is byte-for-byte untouched -- "
          "the refusal happens before the first byte, not after two files");

    req.overwriteExisting = true;
    const ExportStatesReport allowed = exportDocumentStates(doc, req);
    check(allowed.ok && allowed.written() == 4 &&
              readFile(ovDir + "/03_Red only.png").size() > 33,
          "overwrite: asked for explicitly, it proceeds and replaces the file -- still never "
          "partially, because the bytes exist in full before anything is opened");

    fs::remove_all(ovDir, ec);
  }

  // ======================================================================
  // Part H -- the NP_USE_OIIO seam, asserted in BOTH builds
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    const std::string exrDir = dir + "/exr";
    fs::remove_all(exrDir, ec);
    fs::create_directories(exrDir, ec);

    ExportRequest exr;
    exr.format = ImageFormat::Exr;
    exr.targetSpace = ExportTargetSpace::Rec709Linear;
    exr.bitDepth = ExportBitDepth::Half;

    const std::string availability = exportRequestAvailability(exr);
    check(availability.empty() == kOiioBuild,
          "seam: EXR half is writable in an NP_USE_OIIO=ON build and not in an OFF build, and "
          "the capability query says which");

    ExportStatesRequest req;
    req.format = exr;
    req.outputDirectory = exrDir;
    req.nameTemplate = "{index}_{name}";

    const ExportStatesReport run = exportDocumentStates(doc, req);
    size_t written = 0;
    for (auto it = fs::directory_iterator(exrDir, ec); it != fs::directory_iterator(); ++it)
      ++written;
    if (kOiioBuild) {
      check(run.ok && run.written() == 4 && written == 4 &&
                run.items[0].filename == "01_All on.exr",
            "seam (ON): a four-comp EXR batch writes four .exr files -- the extension follows "
            "the request, not the template");
    } else {
      check(!run.ok && run.written() == 0 && written == 0 && run.error == availability,
            "seam (OFF): the SAME batch is refused before the first byte, with io/Export's own "
            "string verbatim -- not a reworded one, and not a per-file failure four times over");
      check(contains(run.error, "OpenImageIO") || contains(run.error, "NP_USE_OIIO") ||
                contains(run.error, "EXR") || contains(run.error, "OpenEXR"),
            "seam (OFF): and that string names the build option or the format, so the user can "
            "act on it");
    }
    // What is identical in both, asserted unconditionally: PNG has no
    // optional dependency (PRD I1), so a PNG batch is the same in either.
    ExportStatesRequest pngReq = req;
    pngReq.format = png8;
    pngReq.nameTemplate = "png_{index}";
    const ExportStatesReport pngRun = exportDocumentStates(doc, pngReq);
    check(pngRun.ok && pngRun.written() == 4,
          "seam: a PNG batch writes four files in BOTH configurations -- PRD I1's "
          "no-optional-dependency format is what makes this feature work in either");

    fs::remove_all(exrDir, ec);
  }

  // ======================================================================
  // Part I -- it really is phase 4's presets, resize included (PRD I15/I17)
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    const std::string presetDir = dir + "/preset";
    fs::remove_all(presetDir, ec);
    fs::create_directories(presetDir, ec);

    // A preset saved through io/ExportAs' own store and round-tripped through
    // its own serialiser, then dropped into the batch request as one field
    // assignment. That is what "write through phase 4's Export As presets"
    // has to mean if the two steps share a mechanism rather than a word.
    ExportPreset preset;
    preset.name = "Web preview 32px";
    preset.request = png8;
    preset.request.resize.mode = ExportResizeMode::FitWithin;
    preset.request.resize.maxWidth = 32;
    preset.request.resize.maxHeight = 32;
    ExportPresetStore store;
    std::string presetErr;
    check(store.savePreset(preset, &presetErr), "presets: the preset saves into the store");
    ExportPresetStore reloaded;
    check(reloaded.loadFromString(store.serialize(), "selftest") &&
              reloaded.find("web preview 32px") != nullptr,
          "presets: it round-trips through io/ExportAs' own serialiser and is found "
          "case-insensitively");

    ExportStatesRequest req;
    req.format = reloaded.find("Web preview 32px")->request;
    req.outputDirectory = presetDir;
    req.nameTemplate = "{name}";

    const ExportStatesReport run = exportDocumentStates(doc, req);
    check(run.ok && run.written() == 4, "presets: four comps exported through the loaded preset");
    const std::vector<uint8_t> bytes = readFile(run.items[0].path);
    std::string decErr;
    const DecodedImage img = decodeImageLinear(bytes.data(), bytes.size(), &decErr);
    check(img.valid() && img.width == 32 && img.height == 32,
          "presets: the preset's RESIZE reached the file -- 64x64 comps came out 32x32, so "
          "PRD I15's fourth setting is carried by I17's loop and not quietly dropped");

    fs::remove_all(presetDir, ec);
  }

  // ======================================================================
  // Part J -- the measurement (the brief's (f))
  // ======================================================================
  {
    const Document doc = makeFourCompDoc();
    const std::string perfDir = dir + "/perf";
    fs::remove_all(perfDir, ec);
    fs::create_directories(perfDir, ec);
    using clock = std::chrono::steady_clock;
    auto secondsSince = [](clock::time_point t0) {
      return std::chrono::duration<double>(clock::now() - t0).count();
    };
    constexpr int kReps = 20;
    // Every figure below is the BEST of kReps passes, not the average: the
    // checks that follow compare two of these numbers against each other (or
    // against a fixed fraction of one another), and an average lets a single
    // rep that landed on an unrelated scheduler stall drag the smaller side
    // of a comparison up by more than the stall cost the larger side. The
    // best-of-N is the cost this loop pays when nothing external interrupts
    // it, which is what these comparisons are actually about.

    // (1) the scratch copy, made once per batch.
    double copySeconds = std::numeric_limits<double>::max();
    for (int i = 0; i < kReps; ++i) {
      const clock::time_point t0 = clock::now();
      Document scratch = doc;
      copySeconds = std::min(copySeconds, secondsSince(t0));
      if (scratch.layers.empty()) return false;  // keeps the copy from being elided
    }

    // (2) the shared state-set: the per-item reset plus restoreLayerComp().
    Document scratch = doc;
    double stateSeconds = std::numeric_limits<double>::max();
    for (int i = 0; i < kReps; ++i) {
      const clock::time_point t0 = clock::now();
      for (size_t j = 0; j < scratch.layers.size(); ++j) {
        scratch.layers[j].visible = doc.layers[j].visible;
        scratch.layers[j].opacity = doc.layers[j].opacity;
        scratch.layers[j].blend = doc.layers[j].blend;
        scratch.layers[j].clipped = doc.layers[j].clipped;
      }
      restoreLayerComp(scratch, static_cast<size_t>(i % 4), nullptr);
      stateSeconds = std::min(stateSeconds, secondsSince(t0));
    }

    // (3) the composite and the encode together, which is what
    // exportDocumentWithRequest() is.
    double encodeSeconds = std::numeric_limits<double>::max();
    size_t encodedBytes = 0;
    for (int i = 0; i < kReps; ++i) {
      const clock::time_point t0 = clock::now();
      const ExportResult r = exportDocumentWithRequest(scratch, png8);
      encodeSeconds = std::min(encodeSeconds, secondsSince(t0));
      encodedBytes = r.bytes.size();
    }

    // (4) one comp end to end, and (5) four comps end to end.
    ExportStatesRequest one;
    one.format = png8;
    one.outputDirectory = perfDir;
    one.nameTemplate = "m{index}_{name}";
    one.overwriteExisting = true;
    one.selection = {0};
    double oneSeconds = std::numeric_limits<double>::max();
    for (int i = 0; i < kReps; ++i) {
      const clock::time_point t0 = clock::now();
      const ExportStatesReport r = exportDocumentStates(doc, one);
      oneSeconds = std::min(oneSeconds, secondsSince(t0));
      if (!r.ok) return false;
    }

    ExportStatesRequest four = one;
    four.selection.clear();
    double fourSeconds = std::numeric_limits<double>::max();
    for (int i = 0; i < kReps; ++i) {
      const clock::time_point t0 = clock::now();
      const ExportStatesReport r = exportDocumentStates(doc, four);
      fourSeconds = std::min(fourSeconds, secondsSince(t0));
      if (!r.ok) return false;
    }

    const size_t tiles = doc.layers[0].rgbTiles->occupiedTileCount() +
                         doc.layers[1].rgbTiles->occupiedTileCount() +
                         doc.layers[2].rgbTiles->occupiedTileCount();
    std::printf("  [measured] 64x64, 3 layers, %zu occupied tiles:\n", tiles);
    std::printf("  [measured]   scratch Document copy (shared_ptr slots, no pixels): %.3f us\n",
                copySeconds * 1e6);
    std::printf("  [measured]   shared state-set (reset 4 properties x 3 layers + "
                "restoreLayerComp): %.3f us\n",
                stateSeconds * 1e6);
    std::printf("  [measured]   composite + encode (exportDocumentWithRequest, %zu bytes out): "
                "%.3f us\n",
                encodedBytes, encodeSeconds * 1e6);
    std::printf("  [measured]   one comp end to end (plan + copy + state-set + encode + write): "
                "%.3f us\n",
                oneSeconds * 1e6);
    std::printf("  [measured]   four comps end to end: %.3f us (%.2fx one comp)\n",
                fourSeconds * 1e6, oneSeconds > 0.0 ? fourSeconds / oneSeconds : 0.0);
    std::printf("  [measured]   the state-set is %.2f%% of the composite+encode it feeds, and "
                "the one scratch copy is %.3f%% of a four-comp run\n",
                encodeSeconds > 0.0 ? 100.0 * stateSeconds / encodeSeconds : 0.0,
                fourSeconds > 0.0 ? 100.0 * copySeconds / fourSeconds : 0.0);

    // The same three costs on a document 64x larger in pixels. The point is
    // the *shape*: the copy is O(occupied tiles) because the slots are
    // shared_ptrs, while the composite and the encode are O(pixels) -- so the
    // gap the ratio above reports only widens at a real document size, and
    // "copy once, reset four properties per item" is the right arrangement
    // rather than a coincidence of a 64x64 fixture.
    {
      constexpr int32_t kBigW = 512, kBigH = 512;
      Document big = Document::createBlank(kBigW, kBigH, WorkingSpace{});
      big.layers[0].name = "Base";
      addLayer(big, 1, makeRgbLayer("Red pass"));
      addLayer(big, 2, makeRgbLayer("Green pass"));
      for (size_t l = 0; l < 3; ++l) {
        const std::array<float, 4> c{0.2f + 0.3f * static_cast<float>(l), 0.3f, 0.4f, 1.0f};
        for (int32_t y = 0; y < kBigH; ++y)
          for (int32_t x = 0; x < kBigW; ++x) writeRgb(big, l, x, y, c);
      }
      setLayerVisible(big, 2, false);
      captureLayerComp(big, "A");
      setLayerVisible(big, 2, true);
      captureLayerComp(big, "B");
      setLayerVisible(big, 1, false);
      captureLayerComp(big, "C");
      setLayerOpacity(big, 2, 0.5f);
      captureLayerComp(big, "D");

      constexpr int kBigReps = 3;
      // Best of kBigReps, for the same reason as Part J's main measurement
      // above: bigState feeds a 1% ceiling against bigEncode, and an average
      // over as few as three reps lets one stalled rep swing that ceiling by
      // far more than it swings the much larger bigEncode.
      double bigCopy = std::numeric_limits<double>::max();
      double bigState = std::numeric_limits<double>::max();
      double bigEncode = std::numeric_limits<double>::max();
      double bigFour = std::numeric_limits<double>::max();
      Document bigScratch = big;
      for (int i = 0; i < kBigReps; ++i) {
        clock::time_point t0 = clock::now();
        Document c = big;
        bigCopy = std::min(bigCopy, secondsSince(t0));
        if (c.layers.empty()) return false;

        t0 = clock::now();
        for (size_t j = 0; j < bigScratch.layers.size(); ++j) {
          bigScratch.layers[j].visible = big.layers[j].visible;
          bigScratch.layers[j].opacity = big.layers[j].opacity;
          bigScratch.layers[j].blend = big.layers[j].blend;
          bigScratch.layers[j].clipped = big.layers[j].clipped;
        }
        restoreLayerComp(bigScratch, static_cast<size_t>(i % 4), nullptr);
        bigState = std::min(bigState, secondsSince(t0));

        t0 = clock::now();
        const ExportResult r = exportDocumentWithRequest(bigScratch, png8);
        bigEncode = std::min(bigEncode, secondsSince(t0));
        if (!r.ok) return false;
      }

      ExportStatesRequest bigReq;
      bigReq.format = png8;
      bigReq.outputDirectory = perfDir;
      bigReq.nameTemplate = "big{index}";
      bigReq.overwriteExisting = true;
      for (int i = 0; i < kBigReps; ++i) {
        const clock::time_point t0 = clock::now();
        const ExportStatesReport r = exportDocumentStates(big, bigReq);
        bigFour = std::min(bigFour, secondsSince(t0));
        if (!r.ok) return false;
      }

      std::printf("  [measured] 512x512 (64x the pixels), 3 layers, %zu occupied tiles: copy "
                  "%.3f us, state-set %.3f us, composite+encode %.3f us, four comps end to end "
                  "%.3f us\n",
                  big.layers[0].rgbTiles->occupiedTileCount() * 3, bigCopy * 1e6, bigState * 1e6,
                  bigEncode * 1e6, bigFour * 1e6);
      std::printf("  [measured]   so the state-set is %.3f%% of the composite+encode here, "
                  "against %.2f%% at 64x64 -- the loop's own overhead shrinks as the document "
                  "grows, because it is O(layers) against O(pixels)\n",
                  bigEncode > 0.0 ? 100.0 * bigState / bigEncode : 0.0,
                  encodeSeconds > 0.0 ? 100.0 * stateSeconds / encodeSeconds : 0.0);
      check(bigState < 0.01 * bigEncode,
            "measured: at 512x512 the shared state-set is under 1% of the composite and encode "
            "it feeds -- the mechanism this step adds is not what a comp export costs");
    }

    check(stateSeconds < encodeSeconds,
          "measured: the shared state-set costs less than the composite and encode it feeds, "
          "so the loop's overhead is not what a comp export pays for");
    check(fourSeconds > 1.5 * oneSeconds,
          "measured: four comps cost materially more than one -- the per-comp work really is "
          "per comp, so nothing is being cached away and reported as four exports");

    fs::remove_all(perfDir, ec);
  }

  fs::remove_all(dir, ec);
  check(!fs::exists(dir, ec), "the scratch directory and every file in it are removed");

  std::printf("[selftest] export states %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
