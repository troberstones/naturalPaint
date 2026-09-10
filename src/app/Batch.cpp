#include "app/Batch.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

#include "app/Command.hpp"
#include "app/OpenAnyFile.hpp"
#include "app/Replay.hpp"
#include "io/ActionFile.hpp"
#include "io/Export.hpp"

namespace np {

namespace fs = std::filesystem;

namespace {

// ASCII-only lowering, io/ExportStates.cpp's own helper and its own reasoning:
// a collision this misses is one an ASCII-insensitive filesystem also misses,
// so the failure mode is a run that goes ahead rather than a name silently
// rewritten. See app/Batch.hpp §1 for what the branch that uses it does and
// does not cover.
std::string lowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

// --- the pixel-unit table (app/Batch.hpp §2) ------------------------------
//
// One row per (command id, parameter) whose meaning is **relative to the
// source's resolution**. Every value here is in document texels, and each is
// quoted from the header that defines it rather than inferred from its name:
//
//   filter_gaussian_blur.sigma   app/FilterOps.hpp:173, "in document texels"
//   filter_unsharp_mask.radius   ops/Filters.hpp:368, "Gaussian sigma, or a
//                                box radius"
//   filter_median.radius         ops/Filters.hpp:894, "window half-width in
//                                document texels"
//   filter_motion_blur.radius    ops/Filters.hpp:995, "half-length of the
//                                smear in document texels"
//   filter_emboss.dx / .dy       ops/Filters.hpp:805, "the compare offset, in
//                                document texels"
//   fill_with_pattern.origin_x   ops/Pattern.hpp:187, "where the pattern's own
//     / .origin_y                (0, 0) lands in document space"
//
// **What is deliberately NOT here**, because the distinction is the rule
// itself: `image_size` and `canvas_size` carry a width and a height counted in
// pixels, and they are absolute destinations rather than relative amounts.
// "Resize to 512x512" means the same thing on a 2k plate and an 8k one --
// docs/automation-plan.md §8 names replaying exactly that across three
// differently-sized documents as an acceptance case, so treating it as
// resolution-dependent would refuse the plan's own example.
// `lens_correct`'s k1/k2 are normalised radial coefficients, and
// `adjust_*`'s parameters are all per-texel functions of value.
//
// This table is held to the registry from `--selftest` in both directions;
// see app/Batch.hpp §2 for why it lives here rather than as a `CommandSpec`
// field.
struct PixelUnitParam {
  const char* commandId;
  const char* paramName;
};

constexpr PixelUnitParam kPixelUnitParams[] = {
    {"filter_gaussian_blur", "sigma"}, {"filter_unsharp_mask", "radius"},
    {"filter_median", "radius"},       {"filter_motion_blur", "radius"},
    {"filter_emboss", "dx"},           {"filter_emboss", "dy"},
    {"fill_with_pattern", "origin_x"}, {"fill_with_pattern", "origin_y"},
};

// "filter_gaussian_blur's sigma" for every pixel-unit parameter `action`
// actually carries, in step order, each named once. Empty means §2's sizing
// pass does not run at all.
std::vector<std::string> pixelUnitParametersCarriedBy(const Action& action) {
  std::vector<std::string> found;
  for (const Command& step : action.steps) {
    for (const PixelUnitParam& row : kPixelUnitParams) {
      if (step.id != row.commandId) continue;
      if (step.params.find(row.paramName) == nullptr) continue;
      std::string named = std::string(row.commandId) + "'s " + row.paramName;
      if (std::find(found.begin(), found.end(), named) == found.end())
        found.push_back(std::move(named));
    }
  }
  return found;
}

std::string joinQuoted(const std::vector<std::string>& items) {
  std::string s;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) s += i + 1 == items.size() ? " and " : ", ";
    s += items[i];
  }
  return s;
}

}  // namespace

size_t BatchReport::count(ExportItemOutcome outcome) const noexcept {
  size_t n = 0;
  for (const BatchItem& item : items)
    if (item.outcome == outcome) ++n;
  return n;
}

size_t BatchReport::unchanged() const noexcept {
  size_t n = 0;
  for (const BatchItem& item : items)
    if (item.outcome == ExportItemOutcome::Written && item.unchangedByAction) ++n;
  return n;
}

// app/Batch.hpp §1 states what each of the two branches covers. They are
// disjoint on purpose: exactly one of them decides any given pair, so neither
// can quietly stand in for the other when it is wrong.
bool pathsNameTheSameFile(const std::string& a, const std::string& b) {
  if (a.empty() || b.empty()) return false;

  std::error_code ea, eb;
  const bool aExists = fs::exists(fs::path(a), ea) && !ea;
  const bool bExists = fs::exists(fs::path(b), eb) && !eb;

  if (aExists && bExists) {
    // The filesystem's own answer, which settles case, Unicode normalisation,
    // symlinks and hard links together because it compares the file rather
    // than the name. This is the branch that guards a real input: an output
    // path that names one necessarily names something that exists.
    std::error_code ec;
    const bool same = fs::equivalent(fs::path(a), fs::path(b), ec);
    return !ec && same;
  }

  // At least one side does not exist, so there is nothing to ask the
  // filesystem about and nothing to lose either. Lexical normalisation plus an
  // ASCII fold: `.`, `..`, a trailing slash, relative-vs-absolute and case.
  std::error_code ca, cb;
  const fs::path pa = fs::weakly_canonical(fs::path(a), ca);
  const fs::path pb = fs::weakly_canonical(fs::path(b), cb);
  const std::string sa = ca ? fs::path(a).lexically_normal().string() : pa.string();
  const std::string sb = cb ? fs::path(b).lexically_normal().string() : pb.string();
  return lowerAscii(sa) == lowerAscii(sb);
}

std::vector<std::string> pixelUnitParameterNames() {
  std::vector<std::string> names;
  for (const PixelUnitParam& row : kPixelUnitParams) names.emplace_back(row.paramName);
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

std::vector<std::string> commandsWithPixelUnitParameter(const std::string& paramName) {
  std::vector<std::string> ids;
  for (const PixelUnitParam& row : kPixelUnitParams)
    if (paramName == row.paramName) ids.emplace_back(row.commandId);
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids;
}

BatchReport planBatch(const BatchRequest& request) {
  BatchReport report;

  // --- (a) every step names a command this build has ------------------------
  //
  // `readAction()` already refuses this at load, so a `--batch` run cannot
  // reach here with an unknown id. A dialog building a `BatchRequest` in
  // memory can, which is the case this gate is for. Same rule, stated for the
  // same reason: an action is *executed*, and a step that cannot be evaluated
  // writes a wrong file that looks fine (docs/automation-plan.md §7).
  for (size_t i = 0; i < request.action.steps.size(); ++i) {
    const Command& step = request.action.steps[i];
    if (findCommand(step.id) == nullptr) {
      report.error = "batch refused: step " + std::to_string(i + 1) + " of the action names '" +
                     step.id +
                     "', which is not a command this build has. Nothing was opened and nothing "
                     "was written.";
      return report;
    }
  }

  // --- (b) the template, once ----------------------------------------------
  if (!validateExportNameTemplate(request.nameTemplate, &report.error)) return report;

  // --- (c) can this build write the requested format at all? ----------------
  //
  // A property of the binary, not of any one file, so it is a whole-run
  // refusal before the first byte rather than N identical per-file failures.
  // io/ExportStates' own NP_USE_OIIO seam, and its string verbatim.
  const std::string availability = exportRequestAvailability(request.format);
  if (!availability.empty()) {
    report.error = availability;
    return report;
  }

  // --- (d) the output directory --------------------------------------------
  if (request.outputDirectory.empty()) {
    report.error = "batch refused: no output directory was given.";
    return report;
  }
  {
    std::error_code ec;
    if (!fs::is_directory(fs::path(request.outputDirectory), ec)) {
      report.error = "batch refused: '" + request.outputDirectory +
                     "' is not an existing directory. Nothing is created on your behalf here -- "
                     "a batch that silently makes folders is a batch that puts files somewhere "
                     "you did not look.";
      return report;
    }
  }

  // --- (e) the source set ---------------------------------------------------
  if (request.sources.empty()) {
    report.error =
        "batch refused: no input files were given. A run over nothing reports success and does "
        "nothing, which is the outcome this whole module exists to keep out.";
    return report;
  }
  for (size_t n = 0; n < request.sources.size(); ++n) {
    if (request.sources[n].empty()) {
      report.error = "batch refused: input " + std::to_string(n + 1) + " of " +
                     std::to_string(request.sources.size()) + " is an empty path.";
      return report;
    }
  }

  // --- (f) one row per source ----------------------------------------------
  report.items.reserve(request.sources.size());
  for (size_t n = 0; n < request.sources.size(); ++n) {
    BatchItem item;
    item.sourcePath = request.sources[n];
    // Fixed here, before any skip decision, for io/ExportStates §5's reason:
    // re-running a batch whose third input now fails must not renumber the
    // fourth input's file.
    item.ordinal = n + 1;
    item.sourceName = fs::path(request.sources[n]).stem().string();

    std::string filename;
    std::string nameError;
    // `{name}` and `{doc}` both get the source's stem -- in a batch the item
    // and the document are the same thing. See `BatchRequest::nameTemplate`.
    if (!resolveExportStateName(request.nameTemplate, item.sourceName, item.sourceName,
                                item.ordinal, request.format.format, &filename, &nameError)) {
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = nameError;
    } else {
      item.filename = filename;
      item.outputPath = (fs::path(request.outputDirectory) / filename).string();
    }
    report.items.push_back(std::move(item));
  }

  // --- (g) two outputs resolving to one file --------------------------------
  //
  // Refused with both inputs named, for io/ExportStates §7's reason: a
  // disambiguating suffix would make the mapping from source to file depend on
  // enumeration order, so inserting one input silently moves every "-2" to a
  // different picture. Case-insensitive, because APFS is.
  for (size_t i = 0; i < report.items.size(); ++i) {
    if (report.items[i].filename.empty()) continue;
    for (size_t j = i + 1; j < report.items.size(); ++j) {
      if (report.items[j].filename.empty()) continue;
      if (lowerAscii(report.items[i].filename) != lowerAscii(report.items[j].filename)) continue;
      report.error = "batch refused: '" + report.items[i].sourcePath + "' and '" +
                     report.items[j].sourcePath + "' both resolve to the output file '" +
                     report.items[i].filename + "' (as '" + report.items[j].filename +
                     "'). Nothing was written. Add {index} to the name template, or narrow the "
                     "source set.";
      return report;
    }
  }

  // --- (h) THE pre-flight: no output may name an input ----------------------
  //
  // app/Batch.hpp §1, and the reason this module exists. Every planned output
  // is compared against every source, not only against its own -- an output
  // rule that can reach the input set at all is the thing being refused, and
  // "file 4's output happens to be file 11's input" is exactly the case a
  // per-item check would miss.
  //
  // Whole-run, before anything is opened: `fopen(path, "wb")` truncates before
  // the first byte is written, so the input is destroyed the moment the output
  // is opened, encode success or not.
  for (const BatchItem& item : report.items) {
    if (item.outputPath.empty()) continue;
    for (const std::string& source : request.sources) {
      if (!pathsNameTheSameFile(item.outputPath, source)) continue;
      report.error =
          "batch refused: the output for '" + item.sourcePath + "' is '" + item.outputPath +
          "', which is the same file as the input '" + source +
          "'. Opening it for writing would destroy that input before a single byte was "
          "encoded, so the WHOLE run is refused and nothing has been opened. Choose a "
          "different output directory or name template.";
      return report;
    }
  }

  // --- (i) resolution-dependent parameters (app/Batch.hpp §2) ---------------
  //
  // Only reached by an action that actually carries one, because the pass
  // decodes every source and nothing else in a batch does.
  const std::vector<std::string> pixelParams = pixelUnitParametersCarriedBy(request.action);
  if (!pixelParams.empty()) {
    bool haveSize = false;
    int32_t width = 0, height = 0;
    std::string sizedBy;
    for (const BatchItem& item : report.items) {
      if (item.outcome == ExportItemOutcome::Skipped) continue;
      const OpenAnyResult probe = openAnyFileAsDocument(item.sourcePath);
      // A source that will not open has no size to compare. It is not refused
      // here: the loop opens it again and reports it as this file's own skip,
      // with the opener's own sentence, which is where a reader looks for it.
      if (!probe.ok) continue;
      if (!haveSize) {
        haveSize = true;
        width = probe.document.document.width;
        height = probe.document.document.height;
        sizedBy = item.sourcePath;
        continue;
      }
      if (probe.document.document.width == width && probe.document.document.height == height)
        continue;
      report.error =
          "batch refused: this action carries " + joinQuoted(pixelParams) +
          ", which is measured in document texels, and the source files are not all the same "
          "size -- '" +
          sizedBy + "' is " + std::to_string(width) + "x" + std::to_string(height) + " and '" +
          item.sourcePath + "' is " + std::to_string(probe.document.document.width) + "x" +
          std::to_string(probe.document.document.height) +
          ". An action does not record the resolution it was authored at, so there is nothing "
          "to scale the parameter against; running it anyway would write files that are wrong "
          "for some of the set and right for the rest. Nothing was written.";
      return report;
    }
  }

  report.ok = report.error.empty();
  return report;
}

BatchReport runBatch(const BatchRequest& request) {
  BatchReport report = planBatch(request);
  // Nothing below this line runs until the pre-flight has passed, which is
  // what makes "no input was opened for writing" a property of the control
  // flow rather than a claim about it.
  if (!report.error.empty()) return report;

  bool halted = false;
  std::string haltReason;

  for (BatchItem& item : report.items) {
    if (item.outcome == ExportItemOutcome::Skipped) continue;
    if (halted) {
      item.outcome = ExportItemOutcome::NotAttempted;
      item.reason = haltReason;
      continue;
    }

    // (1) Open. Read-only, and the import's own warnings come with it --
    // app/Batch.hpp §5: a file that imported lossily and exported perfectly is
    // a success of the wrong picture.
    OpenAnyResult opened = openAnyFileAsDocument(item.sourcePath);
    for (const std::string& warning : opened.warnings) item.warnings.push_back("import: " + warning);
    if (!opened.ok) {
      // §3: a property of this file, already proven to be about this file.
      // The run carries on.
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = opened.status;
      continue;
    }
    OpenDocument doc = std::move(opened.document);

    // (2) Replay, on the document this loop owns. `replayAction()` runs on its
    // own scratch copy and commits only on success (app/Replay §1), so a
    // refusal here has not touched `doc` either -- and `doc` is a decoded
    // copy of the input, never the input.
    if (!request.action.steps.empty()) {
      const ReplayResult replayed = replayAction(doc, request.action);
      item.stepsRun = replayed.steps.size();
      for (const ReplayStepReport& step : replayed.steps) item.texelsChanged += step.texelsChanged;
      for (const std::string& warning : replayed.warnings)
        item.warnings.push_back("action: " + warning);
      if (!replayed.ok) {
        item.outcome = ExportItemOutcome::Failed;
        item.reason = replayed.status;
        halted = true;
        haltReason = "not attempted: the action was refused by '" + item.sourcePath + "' first (" +
                     replayed.status + ").";
        continue;
      }
      // §4. Still written; counted, and named in the summary.
      item.unchangedByAction = item.stepsRun > 0 && item.texelsChanged == 0;
    }

    // (3) Composite and encode -- phase 4 step 7's whole operation, called
    // rather than reimplemented. A refusal is a property of this composite
    // (JPEG refuses a translucent one), so it skips this file.
    const ExportResult encoded = exportDocumentWithRequest(doc.document, request.format);
    for (const std::string& warning : encoded.warnings) item.warnings.push_back("export: " + warning);
    if (!encoded.ok) {
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = encoded.error;
      continue;
    }

    // (4) Write. The bytes exist in full before anything is opened.
    std::string writeError;
    if (!writeEncodedExportToFile(item.outputPath, encoded.bytes, &writeError)) {
      item.outcome = ExportItemOutcome::Failed;
      item.reason = writeError;
      halted = true;
      haltReason = "not attempted: '" + item.sourcePath + "' failed to write first (" +
                   writeError + ").";
      continue;
    }
    item.outcome = ExportItemOutcome::Written;
    item.bytesWritten = encoded.bytes.size();
  }

  report.ok = report.error.empty() && report.failed() == 0;
  return report;
}

std::string batchSummary(const BatchReport& report) {
  if (!report.error.empty()) return report.error;

  const size_t written = report.written();
  const size_t skipped = report.skipped();
  const size_t failed = report.failed();
  const size_t notAttempted = report.notAttempted();
  const size_t unchanged = report.unchanged();
  const size_t total = report.items.size();

  std::string s;
  if (failed > 0) {
    s = "Batch stopped after a failure: " + std::to_string(written) + " of " +
        std::to_string(total) + " files written, " + std::to_string(failed) + " failed, " +
        std::to_string(notAttempted) + " not attempted";
    if (skipped > 0) s += ", " + std::to_string(skipped) + " skipped";
    s += ". No input file was modified. Every file is named with its own outcome in the report.";
  } else {
    s = "Batch wrote " + std::to_string(written) + " of " + std::to_string(total) + " files";
    if (skipped > 0) s += ", skipping " + std::to_string(skipped);
    s += ". No input file was modified.";
  }
  // app/Batch.hpp §4: the count that stops "30 files written" being printed
  // over thirty files the action did nothing to.
  if (unchanged > 0) {
    s += " " + std::to_string(unchanged) + " of the written files were UNCHANGED by the action -- "
         "every step it ran on them reported zero texels changed.";
  }
  return s;
}

int runBatchCli(const char* actionPath, const char* outputDirectory,
                const std::vector<std::string>& sources, const char* formatToken,
                const char* nameTemplate) {
  if (actionPath == nullptr || outputDirectory == nullptr) {
    std::fprintf(stderr,
                 "--batch <action.npaction> <output-dir> <file...>\n"
                 "  optional: --batch-format <token>   (png exr jpeg tiff ...)\n"
                 "            --batch-template <text>  (default \"{name}\")\n");
    return 1;
  }

  BatchRequest request;
  std::string error;
  if (!loadActionFromFile(actionPath, &request.action, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 1;
  }
  request.outputDirectory = outputDirectory;
  request.sources = sources;
  if (nameTemplate != nullptr) request.nameTemplate = nameTemplate;
  if (formatToken != nullptr) {
    ImageFormat format = ImageFormat::Png;
    if (!exportFormatFromToken(formatToken, &format)) {
      std::fprintf(stderr, "--batch-format: '%s' is not a format token this build writes.\n",
                   formatToken);
      return 1;
    }
    request.format.format = format;
  }

  const BatchReport report = runBatch(request);

  // One line per file, then the summary -- the shape `--abr-report` and
  // `--psd-report` already print in. A refused run has no items, so the
  // summary (which is the refusal, verbatim) is the whole output.
  for (const BatchItem& item : report.items) {
    std::printf("%-12s %s -> %s", exportItemOutcomeName(item.outcome), item.sourcePath.c_str(),
                item.filename.empty() ? "(no output name)" : item.filename.c_str());
    if (item.outcome == ExportItemOutcome::Written) {
      std::printf("  %zu bytes", item.bytesWritten);
      if (item.unchangedByAction) std::printf("  [UNCHANGED by the action]");
    }
    std::printf("\n");
    if (!item.reason.empty()) std::printf("               %s\n", item.reason.c_str());
    for (const std::string& warning : item.warnings)
      std::printf("               %s\n", warning.c_str());
  }
  std::printf("%s\n", batchSummary(report).c_str());
  return report.ok ? 0 : 1;
}

}  // namespace np
