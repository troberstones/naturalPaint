#include "io/ExportRegions.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <numeric>
#include <system_error>

#include "ops/DocumentTransform.hpp"

namespace np {
namespace fs = std::filesystem;

namespace {

void markSkipped(ExportStateItem& item, std::string reason) {
  item.outcome = ExportItemOutcome::Skipped;
  item.reason = std::move(reason);
}

bool regionMatchesScope(RegionKind kind, RegionExportScope scope) {
  switch (scope) {
    case RegionExportScope::All: return true;
    case RegionExportScope::FramesOnly: return kind == RegionKind::Frame;
    case RegionExportScope::SlicesOnly: return kind == RegionKind::Slice;
  }
  return false;
}

// io/Batch.cpp's own `lowerAscii()` -- ASCII-only, io/ExportStates.cpp's own
// copy's reasoning: a collision this misses is one an ASCII-insensitive
// filesystem also misses.
std::string lowerAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return out;
}

}  // namespace

const char* regionExportNoun() { return "region"; }
const char* regionExportPlural() { return "regions"; }

ExportStatesReport planRegionExport(const Document& doc, const ExportRegionsRequest& request) {
  ExportStatesReport report;

  if (!validateExportNameTemplate(request.nameTemplate, &report.error)) return report;

  const std::string availability = exportRequestAvailability(request.format);
  if (!availability.empty()) {
    report.error = availability;
    return report;
  }

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

  // --- The selection, filtered by scope ------------------------------------
  std::vector<size_t> candidates;
  for (size_t i = 0; i < doc.regions.size(); ++i)
    if (regionMatchesScope(doc.regions[i].kind, request.scope)) candidates.push_back(i);

  std::vector<size_t> selection = request.selection;
  if (selection.empty()) {
    selection = candidates;
  } else {
    for (size_t index : selection) {
      if (index >= doc.regions.size()) {
        report.error = "export refused: the selection names region " + std::to_string(index) +
                       ", but this document has " + std::to_string(doc.regions.size()) +
                       " region(s) (0.." +
                       (doc.regions.empty() ? "-1" : std::to_string(doc.regions.size() - 1)) +
                       ").";
        return report;
      }
      if (!regionMatchesScope(doc.regions[index].kind, request.scope)) {
        report.error = "export refused: the selection names region " + std::to_string(index) +
                       " ('" + doc.regions[index].name + "'), which is a " +
                       regionKindName(doc.regions[index].kind) +
                       " -- outside the scope this run asked for.";
        return report;
      }
    }
  }

  if (selection.empty()) {
    report.error = std::string("export refused: this document has no ") +
                   (request.scope == RegionExportScope::All
                        ? "regions"
                        : (request.scope == RegionExportScope::FramesOnly ? "frames" : "slices")) +
                   " to export.";
    return report;
  }

  const DocumentRegion canvas = documentCanvasRegion(doc);

  report.items.reserve(selection.size());
  for (size_t n = 0; n < selection.size(); ++n) {
    ExportStateItem item;
    item.sourceIndex = selection[n];
    item.ordinal = n + 1;

    const Region& region = doc.regions[item.sourceIndex];
    item.stateName = region.name;

    // The intersection with the canvas -- this header's own rule: a region
    // partly outside exports the intersection, and one wholly outside
    // exports nothing.
    const DocumentRegion rect{region.x, region.y, region.width, region.height};
    const int32_t ix0 = std::max(rect.x, canvas.x);
    const int32_t iy0 = std::max(rect.y, canvas.y);
    const int32_t ix1 = std::min(rect.x + static_cast<int32_t>(rect.width),
                                 canvas.x + static_cast<int32_t>(canvas.width));
    const int32_t iy1 = std::min(rect.y + static_cast<int32_t>(rect.height),
                                 canvas.y + static_cast<int32_t>(canvas.height));

    if (ix1 <= ix0 || iy1 <= iy0) {
      markSkipped(item, std::string(regionKindName(region.kind)) + " " +
                            std::to_string(item.sourceIndex) + " '" + region.name +
                            "' lies entirely outside the canvas (the canvas is " +
                            std::to_string(doc.width) + "x" + std::to_string(doc.height) +
                            "); nothing to export.");
      report.items.push_back(std::move(item));
      continue;
    }

    std::string filename;
    std::string nameError;
    if (!resolveExportStateName(request.nameTemplate, request.documentName, item.stateName,
                                item.ordinal, request.format.format, &filename, &nameError)) {
      markSkipped(item, std::string(regionKindName(region.kind)) + " " +
                            std::to_string(item.sourceIndex) + " '" + item.stateName +
                            "': " + nameError);
    } else {
      item.filename = filename;
      item.path = (fs::path(request.outputDirectory) / filename).string();
    }
    report.items.push_back(std::move(item));
  }

  // --- Collisions, before the first byte ------------------------------------
  for (size_t a = 0; a < report.items.size(); ++a) {
    if (report.items[a].filename.empty()) continue;
    const std::string keyA = lowerAscii(report.items[a].filename);
    for (size_t b = a + 1; b < report.items.size(); ++b) {
      if (report.items[b].filename.empty()) continue;
      if (lowerAscii(report.items[b].filename) != keyA) continue;
      const ExportStateItem& x = report.items[a];
      const ExportStateItem& y = report.items[b];
      report.error =
          "export refused: region " + std::to_string(x.sourceIndex) + " '" + x.stateName +
          "' resolves to '" + x.filename + "' and region " + std::to_string(y.sourceIndex) +
          " '" + y.stateName + "' resolves to '" + y.filename +
          "', which are the same file (filenames are compared without regard to case). " +
          std::to_string(report.items.size()) +
          " regions were selected and 0 files were written. Rename one, or add {index} to the "
          "name template.";
      return report;
    }
  }

  if (!request.overwriteExisting) {
    for (const ExportStateItem& item : report.items) {
      if (item.path.empty()) continue;
      std::error_code existsEc;
      if (!fs::exists(item.path, existsEc)) continue;
      report.error = "export refused: '" + item.path + "' already exists (region " +
                     std::to_string(item.sourceIndex) + " '" + item.stateName + "'). 0 of " +
                     std::to_string(report.items.size()) +
                     " files were written -- a batch never partially overwrites what is already "
                     "there. Choose another output directory or name template, or turn on "
                     "overwrite.";
      return report;
    }
  }

  report.ok = true;
  return report;
}

ExportStatesReport exportDocumentRegions(const Document& doc, const ExportRegionsRequest& request) {
  ExportStatesReport report = planRegionExport(doc, request);
  if (!report.error.empty()) return report;

  bool halted = false;
  std::string haltReason;

  for (ExportStateItem& item : report.items) {
    if (item.outcome == ExportItemOutcome::Skipped) continue;
    if (halted) {
      item.outcome = ExportItemOutcome::NotAttempted;
      item.reason = haltReason;
      continue;
    }

    const Region& region = doc.regions[item.sourceIndex];

    // Crop a scratch copy to the region's intersection with the canvas, bit-
    // exactly, through the same document-geometry op every other crop in
    // this build uses. `selection` is nullptr: an export has no active
    // selection to carry.
    Document scratch = doc;
    const DocumentRegion canvas = documentCanvasRegion(doc);
    const int32_t ix0 = std::max(region.x, canvas.x);
    const int32_t iy0 = std::max(region.y, canvas.y);
    const int32_t ix1 = std::min(region.x + static_cast<int32_t>(region.width),
                                 canvas.x + static_cast<int32_t>(canvas.width));
    const int32_t iy1 = std::min(region.y + static_cast<int32_t>(region.height),
                                 canvas.y + static_cast<int32_t>(canvas.height));
    const DocumentTransformResult cropped = cropDocument(
        scratch, ix0, iy0, static_cast<uint32_t>(ix1 - ix0), static_cast<uint32_t>(iy1 - iy0),
        nullptr);
    if (!cropped.ok) {
      // Not observed in practice -- the plan already proved the intersection
      // is non-empty -- but a crop refusal is per-item, not per-disk, so it
      // is a skip and the batch carries on rather than halting on a defensive
      // check that should never fire.
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = cropped.error;
      continue;
    }

    const ExportResult encoded = exportDocumentWithRequest(scratch, request.format);
    if (!encoded.ok) {
      item.outcome = ExportItemOutcome::Skipped;
      item.reason = encoded.error;
      continue;
    }
    for (const std::string& warning : encoded.warnings) item.warnings.push_back(warning);

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
      halted = true;
      haltReason = "not attempted: region " + std::to_string(item.sourceIndex) +
                   " failed first (" + item.reason + ").";
    }
  }

  report.ok = report.error.empty() && report.failed() == 0;
  return report;
}

}  // namespace np
