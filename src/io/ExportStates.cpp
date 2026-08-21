#include "io/ExportStates.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <string_view>
#include <system_error>

#include "core/LayerCompOps.hpp"
#include "io/Export.hpp"

// The design, the refusals and every rejected alternative live in
// io/ExportStates.hpp. This file is the mechanism: one resolver, one
// pre-flight, one loop.
namespace np {
namespace {

namespace fs = std::filesystem;

// NAME_MAX on APFS, HFS+, ext4 and NTFS alike. The whole component, extension
// included -- a 250-byte layer name plus ".png" is 254 and fits, plus ".jpeg"
// would not, and the refusal has to be right about which.
constexpr size_t kMaxFilenameBytes = 255;

// The three tokens of ExportStates.hpp §5, in the order a help line lists
// them. One list: exportNameTemplateTokens(), exportNameTemplateHelp(), the
// resolver and the unknown-token refusal all read it, so they cannot drift.
constexpr const char* kTokens[] = {"{name}", "{doc}", "{index}"};

std::string lowerAscii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  // ASCII only, deliberately. A real Unicode case fold needs a table this
  // project does not carry, and the hazard being defended against -- APFS and
  // NTFS treating "Sky.png" and "sky.png" as one file -- is overwhelmingly an
  // ASCII one. A collision this misses is a collision the filesystem also
  // misses on the ASCII-insensitive filesystems, so the failure mode is a
  // *missed* refusal on an exotic pair, not a wrong one.
  for (char c : s) out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
  return out;
}

// Everything ExportStates.hpp §6 refuses about a resolved filename component.
// Returns an empty string when the component is usable.
std::string filenameComponentRefusal(const std::string& stem, const std::string& full) {
  if (stem.empty())
    return "the name template resolved to an empty filename. A file called '" + full +
           "' would be hidden, and every empty-named item would resolve to the same one. Add "
           "literal text or {index} to the template, or give this item a name.";
  for (unsigned char c : stem) {
    if (c == '/' || c == '\\')
      return "the resolved filename '" + stem +
             "' contains a path separator. A name template names a file, not a path -- the "
             "output directory is where a directory is chosen. It is not rewritten to '_', "
             "because 'a/b' and 'a_b' would then collide with nothing said about either.";
    if (c == ':')
      return "the resolved filename '" + stem +
             "' contains ':', which the macOS Finder displays as '/' (an HFS inheritance). The "
             "file would be created but its name would read as something else in the user's own "
             "file browser.";
    if (c < 0x20 || c == 0x7f)
      return "the resolved filename contains a control character (byte 0x" +
             [&] {
               char b[8];
               std::snprintf(b, sizeof(b), "%02x", static_cast<unsigned>(c));
               return std::string(b);
             }() +
             "), which is a name a user cannot reliably pick out of a folder again.";
  }
  // `.` and `..` are checked before the leading-dot rule, so each gets the
  // message that is actually true of it: "." is not a hidden file, it is the
  // directory itself.
  if (stem == "." || stem == "..")
    return "the resolved filename '" + stem +
           "' is a reserved directory entry, not a name a file can have.";
  if (stem.front() == '.')
    return "the resolved filename '" + stem +
           "' begins with '.', which makes it a hidden file. The export would succeed and the "
           "user would not see the result.";
  if (full.size() > kMaxFilenameBytes)
    return "the resolved filename is " + std::to_string(full.size()) +
           " bytes long; the maximum filename component this filesystem accepts is " +
           std::to_string(kMaxFilenameBytes) + " bytes.";
  return {};
}

bool kindCanHoldPixels(LayerKind kind) {
  // core/Composite.cpp's own rule (`kind == RGB && rgbTiles` ||
  // `kind == Pigment && pigmentTiles`), asked of the kind alone: an RGB layer
  // whose store has not been allocated yet exports as transparent, exactly as
  // one with an allocated but empty store does, and that is a legitimate file.
  // What is not legitimate is a kind that can *never* hold a pixel.
  return kind == LayerKind::RGB || kind == LayerKind::Pigment;
}

void markSkipped(ExportStateItem& item, std::string reason) {
  item.outcome = ExportItemOutcome::Skipped;
  item.reason = std::move(reason);
  item.filename.clear();
  item.path.clear();
}

}  // namespace

const char* exportStateSourceNoun(ExportStateSource source) {
  return source == ExportStateSource::Comps ? "comp" : "layer";
}

const char* exportStateSourcePlural(ExportStateSource source) {
  return source == ExportStateSource::Comps ? "comps" : "layers";
}

const char* exportItemOutcomeName(ExportItemOutcome outcome) {
  switch (outcome) {
    case ExportItemOutcome::Written: return "written";
    case ExportItemOutcome::Skipped: return "skipped";
    case ExportItemOutcome::Failed: return "FAILED";
    case ExportItemOutcome::NotAttempted: return "not attempted";
  }
  return "?";
}

size_t ExportStatesReport::count(ExportItemOutcome outcome) const noexcept {
  size_t n = 0;
  for (const ExportStateItem& item : items)
    if (item.outcome == outcome) ++n;
  return n;
}

std::vector<std::string> exportNameTemplateTokens() {
  return {kTokens[0], kTokens[1], kTokens[2]};
}

std::string exportNameTemplateHelp() {
  return "{name} the comp's or layer's name, {doc} the document's name, {index} the 1-based "
         "position in the selection (two digits). The extension comes from the export format; "
         "do not write one. There is deliberately no date token -- it would make two runs of "
         "the same batch produce different files.";
}

bool validateExportNameTemplate(const std::string& nameTemplate, std::string* errorOut) {
  auto fail = [&](std::string why) {
    if (errorOut) *errorOut = std::move(why);
    return false;
  };
  if (nameTemplate.empty())
    return fail("export refused: the name template is empty. " + exportNameTemplateHelp());

  for (size_t i = 0; i < nameTemplate.size(); ++i) {
    const char c = nameTemplate[i];
    if (c == '}')
      return fail("export refused: the name template has a '}' with no matching '{' at "
                  "position " +
                  std::to_string(i) + ".");
    if (c != '{') {
      // Literal text. §6: a separator the *user typed* refuses the batch once,
      // rather than refusing every item separately with the same message.
      if (c == '/' || c == '\\' || c == ':')
        return fail(std::string("export refused: the name template contains '") + c +
                    "' at position " + std::to_string(i) +
                    ". A name template names a file, not a path -- choose the folder in the "
                    "output directory field.");
      continue;
    }
    const size_t close = nameTemplate.find('}', i);
    if (close == std::string::npos)
      return fail("export refused: the name template has a '{' with no matching '}' at "
                  "position " +
                  std::to_string(i) + ".");
    const std::string token = nameTemplate.substr(i, close - i + 1);
    const bool known =
        std::find(std::begin(kTokens), std::end(kTokens), token) != std::end(kTokens);
    if (!known)
      return fail("export refused: the name template uses an unrecognised token '" + token +
                  "'. Known tokens: " + exportNameTemplateHelp());
    i = close;
  }
  return true;
}

bool resolveExportStateName(const std::string& nameTemplate, const std::string& documentName,
                            const std::string& stateName, size_t ordinal, ImageFormat format,
                            std::string* outFilename, std::string* errorOut) {
  std::string templateError;
  if (!validateExportNameTemplate(nameTemplate, &templateError)) {
    if (errorOut) *errorOut = std::move(templateError);
    return false;
  }

  std::string stem;
  for (size_t i = 0; i < nameTemplate.size(); ++i) {
    if (nameTemplate[i] != '{') {
      stem.push_back(nameTemplate[i]);
      continue;
    }
    const size_t close = nameTemplate.find('}', i);
    const std::string token = nameTemplate.substr(i, close - i + 1);
    // A token whose value is absent renders as **nothing**, and the refusal is
    // deferred to the resolved name (§6's empty-stem rule). The two
    // alternatives both invent policy that is wrong somewhere: refusing an
    // absent {doc} outright would stop an unsaved document using a template
    // where the document name does not matter, and substituting a placeholder
    // ("untitled") would make two different unsaved documents resolve to the
    // same filenames -- a collision whose cause the user cannot see. Rendering
    // nothing and then checking the result is the one rule with no wrong case,
    // and the plan shows the user the actual filename either way.
    if (token == "{name}") {
      stem += stateName;
    } else if (token == "{doc}") {
      stem += documentName;
    } else {
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%02zu", ordinal);
      stem += buf;
    }
    i = close;
  }

  const char* ext = imageFormatExtension(format);
  std::string full = stem;
  if (ext != nullptr && ext[0] != '\0') {
    full.push_back('.');
    full += ext;
  }

  const std::string refusal = filenameComponentRefusal(stem, full);
  if (!refusal.empty()) {
    if (errorOut) *errorOut = refusal;
    return false;
  }
  if (outFilename) *outFilename = std::move(full);
  return true;
}

ExportStatesReport planStateExport(const Document& doc, const ExportStatesRequest& request) {
  ExportStatesReport report;
  const char* noun = exportStateSourceNoun(request.source);
  const char* plural = exportStateSourcePlural(request.source);

  // --- The template, once ---------------------------------------------------
  if (!validateExportNameTemplate(request.nameTemplate, &report.error)) return report;

  // --- Can this build honour the request at all? ----------------------------
  //
  // **This is the NP_USE_OIIO seam, and it is the reason this check is a
  // batch-level refusal rather than a per-file one.** Whether a
  // (format, depth) pair is writable is a property of the binary, not of any
  // one comp, so an EXR batch in an OFF build refuses here -- before the
  // first byte, with zero files on disk -- rather than producing N identical
  // per-file failures. A PNG batch is identical in both configurations,
  // because PNG has no optional dependency (PRD I1). The string is
  // io/Export's own, verbatim: there is no second vocabulary here.
  const std::string availability = exportRequestAvailability(request.format);
  if (!availability.empty()) {
    report.error = availability;
    return report;
  }

  // --- The output directory -------------------------------------------------
  if (request.outputDirectory.empty()) {
    report.error = "export refused: no output directory was given.";
    return report;
  }
  std::error_code ec;
  if (!fs::is_directory(request.outputDirectory, ec)) {
    report.error = "export refused: '" + request.outputDirectory +
                   "' is not an existing directory. Nothing is created on your behalf here -- "
                   "an export that silently makes folders is an export that puts files "
                   "somewhere you did not look.";
    return report;
  }

  // --- The selection --------------------------------------------------------
  const size_t total =
      request.source == ExportStateSource::Comps ? doc.comps.size() : doc.layers.size();
  if (total == 0) {
    report.error = std::string("export refused: this document has no ") + plural + " to export.";
    return report;
  }
  std::vector<size_t> selection = request.selection;
  if (selection.empty()) {
    selection.resize(total);
    std::iota(selection.begin(), selection.end(), size_t{0});
  }
  for (size_t index : selection) {
    if (index >= total) {
      report.error = "export refused: the selection names " + std::string(noun) + " " +
                     std::to_string(index) + ", but this document has " + std::to_string(total) +
                     " " + plural + " (0.." + std::to_string(total - 1) + ").";
      return report;
    }
  }

  // --- One row per selected item -------------------------------------------
  report.items.reserve(selection.size());
  for (size_t n = 0; n < selection.size(); ++n) {
    ExportStateItem item;
    item.sourceIndex = selection[n];
    // §5: the ordinal is the position in the *selection*, fixed here, before
    // any skip decision below can change what gets written.
    item.ordinal = n + 1;

    bool skipped = false;
    if (request.source == ExportStateSource::Comps) {
      const LayerComp& comp = doc.comps[item.sourceIndex];
      item.stateName = comp.name;
      if (!comp.known) {
        markSkipped(item, "comp " + std::to_string(item.sourceIndex) +
                              " was written by a build this one cannot read (PRD I10 keeps it in "
                              "the document verbatim, but it cannot be restored, so there is no "
                              "state to composite).");
        skipped = true;
      }
    } else {
      const Layer& layer = doc.layers[item.sourceIndex];
      item.stateName = layer.name;
      if (!kindCanHoldPixels(layer.kind)) {
        markSkipped(item, std::string("layer ") + std::to_string(item.sourceIndex) + " '" +
                              layer.name + "' is " +
                              (layer.kind == LayerKind::Adjustment ? "an" : "a") + " " +
                              layerKindName(layer.kind) +
                              " layer: it holds no pixels of its own, so isolating it would "
                              "composite to a fully transparent canvas. Writing that file would "
                              "be reporting success for an image with nothing in it.");
        skipped = true;
      }
    }

    if (!skipped) {
      std::string filename;
      std::string nameError;
      if (!resolveExportStateName(request.nameTemplate, request.documentName, item.stateName,
                                  item.ordinal, request.format.format, &filename, &nameError)) {
        markSkipped(item, std::string(noun) + " " + std::to_string(item.sourceIndex) + " '" +
                              item.stateName + "': " + nameError);
      } else {
        item.filename = filename;
        item.path = (fs::path(request.outputDirectory) / filename).string();
      }
    }
    report.items.push_back(std::move(item));
  }

  // --- Collisions, before the first byte (§7) -------------------------------
  for (size_t a = 0; a < report.items.size(); ++a) {
    if (report.items[a].filename.empty()) continue;
    const std::string keyA = lowerAscii(report.items[a].filename);
    for (size_t b = a + 1; b < report.items.size(); ++b) {
      if (report.items[b].filename.empty()) continue;
      if (lowerAscii(report.items[b].filename) != keyA) continue;
      const ExportStateItem& x = report.items[a];
      const ExportStateItem& y = report.items[b];
      report.error =
          "export refused: " + std::string(noun) + " " + std::to_string(x.sourceIndex) + " '" +
          x.stateName + "' resolves to '" + x.filename + "' and " + noun + " " +
          std::to_string(y.sourceIndex) + " '" + y.stateName + "' resolves to '" + y.filename +
          "', which are the same file (filenames are compared without regard to case, because "
          "APFS and NTFS are). " +
          std::to_string(report.items.size()) + " " + plural +
          " were selected and 0 files were written. Rename one, or add {index} to the name "
          "template.";
      return report;
    }
  }

  // --- Nothing is overwritten unless asked (§7, PRD P4) ---------------------
  if (!request.overwriteExisting) {
    for (const ExportStateItem& item : report.items) {
      if (item.path.empty()) continue;
      std::error_code existsEc;
      if (!fs::exists(item.path, existsEc)) continue;
      report.error = "export refused: '" + item.path + "' already exists (" + noun + " " +
                     std::to_string(item.sourceIndex) + " '" + item.stateName +
                     "'). 0 of " + std::to_string(report.items.size()) +
                     " files were written -- a batch never partially overwrites what is already "
                     "there (PRD P4). Choose another output directory or name template, or turn "
                     "on overwrite.";
      return report;
    }
  }

  report.ok = true;
  return report;
}

ExportStatesReport exportDocumentStates(const Document& doc, const ExportStatesRequest& request) {
  ExportStatesReport report = planStateExport(doc, request);
  if (!report.error.empty()) return report;

  // §2. **One copy, made here and not inside the loop, and the only document
  // anything below writes to.** `doc` is const: the caller's document cannot
  // be reached from this function, so "it is restored afterwards" is a
  // property of the signature rather than a claim about the code. Nothing here
  // calls `recordLayerEdit()` -- a batch export is not an edit, and this
  // module has no OpenDocument to record one against.
  //
  // Cheap because core/TileStore holds `shared_ptr` slots: this copies map
  // nodes and bumps refcounts, not pixels. The loop writes only the four
  // appearance properties, never a tile, so copy-on-write never fires either.
  Document scratch = doc;

  bool halted = false;
  std::string haltReason;

  for (ExportStateItem& item : report.items) {
    if (item.outcome == ExportItemOutcome::Skipped) continue;
    if (halted) {
      item.outcome = ExportItemOutcome::NotAttempted;
      item.reason = haltReason;
      continue;
    }

    // (1) Reset the scratch to the document's own state. Direct assignment,
    // not core/LayerOps' setters: putting a throwaway copy back to values it
    // already held is not an edit a lock has an opinion about, and the
    // setters would refuse three of the four on a locked layer. The apply
    // below *does* honour the lock, and §3 argues why the asymmetry is right.
    for (size_t j = 0; j < scratch.layers.size(); ++j) {
      scratch.layers[j].visible = doc.layers[j].visible;
      scratch.layers[j].opacity = doc.layers[j].opacity;
      scratch.layers[j].blend = doc.layers[j].blend;
      scratch.layers[j].clipped = doc.layers[j].clipped;
    }

    // (2) Set the document state. **The only place in this file where a comp
    // export and a layer export differ**, and each branch is the mutation and
    // nothing else -- no naming, no writing, no reporting.
    if (request.source == ExportStateSource::Comps) {
      LayerCompRestoreReport restored;
      const LayerOpResult result = restoreLayerComp(scratch, item.sourceIndex, &restored);
      if (!result.ok) {
        // Nothing reached the filesystem, so this is a skip and the batch
        // carries on -- a refused restore is a property of this comp, not of
        // the disk. The resolved `filename` is kept rather than cleared: it
        // names the file that would have been written, which is what a report
        // row needs to read as a sentence. `outcome` is what says whether
        // anything is on disk, and `bytesWritten` stays 0.
        item.outcome = ExportItemOutcome::Skipped;
        item.reason = result.error;
        continue;
      }
      std::string summary = layerCompRestoreSummary(restored);
      if (!summary.empty()) item.warnings.push_back(std::move(summary));
    } else {
      for (size_t j = 0; j < scratch.layers.size(); ++j)
        scratch.layers[j].visible = (j == item.sourceIndex);
      Layer& isolated = scratch.layers[item.sourceIndex];
      if (isolated.clipped) {
        // §4: the clip base is the layer below, which this state has just
        // hidden, so a clipped layer would composite to nothing at all.
        isolated.clipped = false;
        item.warnings.push_back(
            "layer '" + isolated.name +
            "' is clipped to the layer below it, which is not in this file. It was exported "
            "**unclipped**, because clipping it to a hidden layer's alpha would have written a "
            "fully transparent image.");
      }
    }

    // (3) Composite and encode -- phase 4 step 7's whole operation, called,
    // not reimplemented. A refusal here is per state and not per disk (JPEG
    // refuses a translucent composite by name, and one comp can be opaque
    // while another is not), so it skips this file and the batch continues.
    const ExportResult encoded = exportDocumentWithRequest(scratch, request.format);
    if (!encoded.ok) {
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = encoded.error;
      continue;
    }
    for (const std::string& warning : encoded.warnings) item.warnings.push_back(warning);

    // (4) Write. The bytes exist in full before anything is opened, which is
    // `exportDocumentToFile()`'s own guarantee held here for the same reason:
    // a refusal must never leave a truncated file behind.
    //
    // This eight-line fwrite duplicates the tail of
    // `exportDocumentWithRequestToFile()` rather than calling it, because
    // calling it would encode the composite a second time -- the bytes are
    // already in hand from step (3), and re-flattening a document to recover a
    // byte count is not a trade worth making. Flagged rather than hidden, the
    // same way io/Export.cpp flags its copy of `unpremultiply()`: this is the
    // second consumer, and a third is when the writer should be hoisted.
    std::FILE* f = std::fopen(item.path.c_str(), "wb");
    if (f == nullptr) {
      item.outcome = ExportItemOutcome::Failed;
      item.reason = "could not open '" + item.path + "' for writing.";
    } else {
      const size_t wrote = std::fwrite(encoded.bytes.data(), 1, encoded.bytes.size(), f);
      const bool closedOk = std::fclose(f) == 0;
      if (wrote != encoded.bytes.size() || !closedOk) {
        item.outcome = ExportItemOutcome::Failed;
        item.reason = "'" + item.path + "' was opened but not fully written (" +
                      std::to_string(wrote) + " of " + std::to_string(encoded.bytes.size()) +
                      " bytes).";
      } else {
        item.outcome = ExportItemOutcome::Written;
        item.bytesWritten = encoded.bytes.size();
      }
    }

    if (item.outcome == ExportItemOutcome::Failed) {
      // §8, and PRD P4's own acceptance row: stop, and leave the rest
      // untouched.
      halted = true;
      haltReason = "not attempted: " + std::string(exportStateSourceNoun(request.source)) + " " +
                   std::to_string(item.sourceIndex) + " failed first (" + item.reason + ").";
    }
  }

  report.ok = report.error.empty() && report.failed() == 0;
  return report;
}

std::string exportStatesSummary(const ExportStatesReport& report) {
  if (!report.error.empty()) return report.error;
  const size_t written = report.written();
  const size_t skipped = report.skipped();
  const size_t failed = report.failed();
  const size_t notAttempted = report.notAttempted();
  const size_t total = report.items.size();

  std::string s;
  if (failed > 0) {
    s = "Export stopped after a write failure: " + std::to_string(written) + " of " +
        std::to_string(total) + " files written, " + std::to_string(failed) + " failed, " +
        std::to_string(notAttempted) + " not attempted";
    if (skipped > 0) s += ", " + std::to_string(skipped) + " skipped";
    s += ". Every file is named with its own outcome in the report.";
    return s;
  }
  s = "Exported " + std::to_string(written) + " of " + std::to_string(total) + " selected.";
  if (skipped > 0)
    s += " " + std::to_string(skipped) +
         " skipped, each named with its reason -- nothing was written for those.";
  return s;
}

}  // namespace np
