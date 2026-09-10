#include "app/selftest/Support.hpp"

#include <filesystem>

#include "app/BatchDialog.hpp"
#include "app/Command.hpp"
#include "io/ActionFile.hpp"
#include "io/ExportAs.hpp"

namespace np {
namespace {

Command blurStep(double sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return Command{"filter_gaussian_blur", std::move(p)};
}

Action blurAction() {
  Action a;
  a.name = "Soften";
  a.steps = {blurStep(2.0)};
  return a;
}

size_t countFiles(const std::string& dir) {
  namespace fs = std::filesystem;
  size_t n = 0;
  std::error_code e;
  for (auto it = fs::directory_iterator(dir, e); it != fs::directory_iterator(); ++it)
    if (it->is_regular_file()) ++n;
  return n;
}

}  // namespace

bool runBatchDialogTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
  };

  namespace fs = std::filesystem;
  std::error_code ec;
  // A root of its own. app/selftest/Batch.cpp uses `selftest_batch`, and two
  // sections sharing a directory is how one section's `remove_all` deletes the
  // other's fixtures depending on which ran first.
  const std::string root = "selftest_batch_dialog";
  const std::string inDir = root + "/in";
  const std::string outDir = root + "/out";
  fs::remove_all(root, ec);
  fs::create_directories(inDir, ec);
  fs::create_directories(outDir, ec);

  // Three real files, written through the export path so the batch's own
  // opener accepts them.
  std::vector<std::string> sources;
  {
    ExportRequest png;
    for (size_t n = 0; n < 3; ++n) {
      OpenDocument od = makeBlankOpenDocument(24, 16, WorkingSpace{}, "fixture");
      const float v = 0.2f + 0.2f * static_cast<float>(n);
      Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          t.writePixel(PixelCoord{x, y}, {v, v * 0.5f, 1.0f - v, 1.0f});
      char name[128];
      std::snprintf(name, sizeof(name), "%s/plate%zu.png", inDir.c_str(), n);
      std::string error;
      if (!exportDocumentWithRequestToFile(od.document, name, png, &error))
        std::printf("  (fixture write failed: %s)\n", error.c_str());
      sources.emplace_back(name);
    }
  }
  const std::string sourcesText = sources[0] + "\n" + sources[1] + "\n" + sources[2] + "\n";

  std::printf("  -- A. the gates, in the order a user meets them --\n");
  {
    BatchDialogState st;
    BatchDialogView v = batchDialogView(st);
    check(!v.run.enabled && contains(v.run.disabledReason, "action"),
          "gate: with nothing chosen, it asks for the action first");

    st.action = blurAction();
    v = batchDialogView(st);
    check(!v.run.enabled && contains(v.run.disabledReason, "input file"),
          "gate: with an action but no inputs, it asks for inputs");

    st.sourcesText = sourcesText;
    v = batchDialogView(st);
    check(!v.run.enabled && contains(v.run.disabledReason, "output directory"),
          "gate: with inputs but no destination, it asks for one");

    st.outputDirectory = outDir;
    v = batchDialogView(st);
    check(v.run.enabled && v.run.disabledReason.empty(), "gate: and then it is live");
    check(v.sourceCount == 3, "gate: the source count is visible before anything is pressed");
    check(v.actionName == "Soften" && v.actionSteps.size() == 1 &&
              v.actionSteps[0] == "Gaussian Blur",
          "gate: the step list reads in labels, not ids");
  }

  std::printf("  -- B. PREVIEW is reachable exactly whenever RUN is --\n");
  {
    // **The invariant, not a coincidence.** If the two preconditions could
    // differ, the one that could differ is PREVIEW -- and a dialog where the
    // safe button is the one you cannot press has it exactly backwards.
    // Walked over all eight states of the three gates rather than spot-checked.
    bool sameEverywhere = true;
    for (int mask = 0; mask < 8; ++mask) {
      BatchDialogState st;
      if (mask & 1) st.action = blurAction();
      if (mask & 2) st.sourcesText = sourcesText;
      if (mask & 4) st.outputDirectory = outDir;
      const BatchDialogView v = batchDialogView(st);
      if (v.preview.enabled != v.run.enabled) sameEverywhere = false;
      if (v.preview.disabledReason != v.run.disabledReason) sameEverywhere = false;
    }
    check(sameEverywhere,
          "both: over all 8 gate states, PREVIEW and RUN agree exactly");
  }

  std::printf("  -- C. PREVIEW writes nothing, and says so --\n");
  {
    BatchDialogState st;
    st.action = blurAction();
    st.sourcesText = sourcesText;
    st.outputDirectory = outDir;
    const size_t before = countFiles(outDir);

    batchDialogPreview(st);
    const BatchDialogView v = batchDialogView(st);
    check(v.haveReport && v.reportWasPreview, "preview: the report is marked a preview");
    check(v.rows.size() == 3, "preview: and it plans all three files");
    check(countFiles(outDir) == before, "preview: not one file was written");
    check(contains(st.status, "Nothing has been written"),
          "preview: the status says so in words, not by the button's name");
  }

  std::printf("  -- D. RUN writes, and its report is not a preview --\n");
  {
    BatchDialogState st;
    st.action = blurAction();
    st.sourcesText = sourcesText;
    st.outputDirectory = outDir;

    batchDialogRun(st);
    const BatchDialogView v = batchDialogView(st);
    check(v.haveReport && !v.reportWasPreview, "run: the report is NOT marked a preview");
    check(countFiles(outDir) == 3, "run: three files landed");
    check(v.error.empty() && st.report.written() == 3, "run: all three are Written");
    check(!v.summary.empty(), "run: the summary is carried whole, not recomputed");
  }

  std::printf("  -- E. a preview that plans N and a run that refuses cannot both happen --\n");
  {
    // §2's consequence, asserted rather than argued: the refusal a run would
    // hit is the same `planBatch()` the preview showed. This aims an output at
    // an input, which is the refusal that matters most.
    BatchDialogState st;
    st.action = blurAction();
    st.sourcesText = sourcesText;
    st.outputDirectory = inDir;  // the inputs' own directory

    batchDialogPreview(st);
    const BatchDialogView previewed = batchDialogView(st);
    check(!previewed.error.empty(), "agree: the preview refuses");
    check(contains(previewed.error, "input") || contains(previewed.error, "overwrite"),
          "agree: naming the collision");
    const std::string previewError = previewed.error;

    batchDialogRun(st);
    const BatchDialogView ran = batchDialogView(st);
    check(ran.error == previewError, "agree: and the run refuses with the SAME sentence");
    check(countFiles(inDir) == 3, "agree: the inputs are all still there");
  }

  std::printf("  -- F. a whole-run refusal is in the report, not on the button --\n");
  {
    BatchDialogState st;
    st.action = blurAction();
    st.sourcesText = sourcesText;
    st.outputDirectory = root + "/does-not-exist";
    const BatchDialogView v = batchDialogView(st);
    check(v.run.enabled,
          "refusal: the button stays live -- the dialog cannot know the directory is missing "
          "without touching the disk");

    batchDialogPreview(st);
    const BatchDialogView after = batchDialogView(st);
    check(!after.error.empty() && contains(after.error, "does-not-exist"),
          "refusal: and the answer arrives as a refusal naming the directory");
  }

  std::printf("  -- G. the files an action did not change are conspicuous --\n");
  {
    // An exposure of zero stops: legal, available, it succeeds, and it changes
    // not one texel. app/selftest/Batch.cpp's own no-op section uses the same
    // step, and the agreement is deliberate -- this section is asserting that
    // the flag reaches the ROW, so it should provoke the flag the way the
    // module's own tests do rather than inventing a second no-op.
    //
    // **A layer setter does not work here, and the non-vacuity assertion below
    // is what caught it.** `set_layer_opacity` to the value already in place
    // reports `changesPixels == false` -- `fromLayerEdit()` does not set that
    // field at all -- so the file is written as a plain success and nothing is
    // flagged. The first version of this section asserted "every unchanged
    // file is flagged" over a set with no unchanged files in it, and passed.
    BatchDialogState st;
    Action noop;
    noop.name = "Exposure by zero stops";
    JsonValue p = JsonValue::object();
    p.set("stops", JsonValue::number(0.0));
    noop.steps = {Command{"adjust_exposure", std::move(p)}};
    st.action = noop;
    st.sourcesText = sourcesText;
    st.outputDirectory = outDir;

    batchDialogRun(st);
    const BatchDialogView v = batchDialogView(st);
    size_t conspicuous = 0;
    for (const BatchReportRow& row : v.rows)
      if (row.conspicuous) ++conspicuous;
    check(st.report.unchanged() == conspicuous,
          "unchanged: every file app/Batch counted as unchanged is flagged in a row");
    check(conspicuous > 0,
          "unchanged: (non-vacuity) this fixture really did write files unchanged");
    check(contains(v.summary, "UNCHANGED"),
          "unchanged: and the SUMMARY says so -- a flag in a 30-row table is not conspicuous");
  }

  std::printf("  -- H. loading an action drops the report it does not describe --\n");
  {
    const std::string actionPath = root + "/soften.npaction";
    std::string writeError;
    if (!saveActionToFile(actionPath, blurAction(), &writeError))
      std::printf("  (action fixture write failed: %s)\n", writeError.c_str());

    BatchDialogState st;
    st.action = blurAction();
    st.sourcesText = sourcesText;
    st.outputDirectory = outDir;
    batchDialogRun(st);
    check(st.haveReport, "load: (setup) there is a report on screen");

    const bool loaded = batchDialogLoadAction(st, actionPath);
    check(loaded && st.action.name == "Soften", "load: the action came off disk");
    check(!batchDialogView(st).haveReport,
          "load: and the previous action's report went with it");

    const bool missing = batchDialogLoadAction(st, root + "/nope.npaction");
    check(!missing && st.action.name == "Soften",
          "load: a file that will not read changes nothing");
    check(!st.status.empty(), "load: and says why");
  }

  std::printf("  -- I. the source list is what the user typed --\n");
  {
    const std::vector<std::string> parsed =
        batchDialogSources("  a.exr \n\n\t\nb.exr\n   \nb.exr\n");
    check(parsed.size() == 3, "sources: blank and whitespace-only lines are dropped");
    check(parsed.size() == 3 && parsed[0] == "a.exr" && parsed[1] == "b.exr",
          "sources: and the rest are trimmed");
    check(parsed.size() == 3 && parsed[2] == "b.exr",
          "sources: a duplicate is KEPT -- planBatch() refuses it by name, which is a "
          "better answer than de-duplicating behind the user's back");
  }

  fs::remove_all(root, ec);
  return ok;
}

}  // namespace np
