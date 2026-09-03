#include "app/ExportDialog.hpp"

#include <cmath>

namespace np {

ExportResizeField exportResizeField(ExportResizeMode mode) {
  switch (mode) {
    case ExportResizeMode::None: return ExportResizeField::None;
    case ExportResizeMode::Percent: return ExportResizeField::Percent;
    case ExportResizeMode::FitWithin: return ExportResizeField::FitBox;
  }
  return ExportResizeField::None;
}

std::vector<ExportFormatChoice> exportFormatChoices(ExportTargetSpace space,
                                                    ExportBitDepth depth) {
  std::vector<ExportFormatChoice> out;
  const std::vector<FormatCapability>& caps = allFormatCapabilities();
  out.reserve(caps.size());
  for (const FormatCapability& c : caps) {
    ExportFormatChoice choice;
    choice.format = c.format;
    choice.writable = c.canWrite;
    // Only for the unwritable ones. A writable format's refusal string is
    // about the *depth* at this point (exportRefusalReason() falls through to
    // that check once the format passes), and putting a depth complaint on a
    // format row would attach the wrong control's problem to the wrong
    // control -- the Bit depth combo is one line down and already refuses
    // what it cannot offer.
    if (!c.canWrite)
      choice.refusal = exportRefusalReason(c.format, space, depth, nullptr, nullptr);
    out.push_back(std::move(choice));
  }
  return out;
}

ExportBitDepth legaliseExportDepth(ImageFormat format, ExportBitDepth current) {
  const std::vector<ExportBitDepth> depths = offerableExportDepths(format);
  if (depths.empty()) return current;
  for (ExportBitDepth d : depths)
    if (d == current) return current;
  return depths.front();
}

bool exportRequestsEqual(const ExportRequest& a, const ExportRequest& b) {
  if (a.format != b.format) return false;
  if (a.targetSpace != b.targetSpace) return false;
  if (a.bitDepth != b.bitDepth) return false;
  if (a.resize.mode != b.resize.mode) return false;
  // Only the fields this mode reads -- see the header's §2. A dialog keeps
  // the other mode's numbers alive on purpose (ExportResize's own comment),
  // so comparing them here would report "modified" over a value the export
  // will never look at.
  switch (exportResizeField(a.resize.mode)) {
    case ExportResizeField::None:
      return true;
    case ExportResizeField::Percent:
      // The slider writes a float; an exact compare would call a preset
      // modified after a round trip through the widget that landed on the
      // same displayed value. The slider's own format is "%.1f%%", so half a
      // display step is the largest difference that is still the same number
      // on screen.
      return std::fabs(a.resize.percent - b.resize.percent) < 0.05f;
    case ExportResizeField::FitBox:
      return a.resize.maxWidth == b.resize.maxWidth && a.resize.maxHeight == b.resize.maxHeight;
  }
  return true;
}

std::string exportPresetMenuLabel(const ExportPreset* loaded, const ExportRequest& current) {
  if (loaded == nullptr) return "Custom";
  if (exportRequestsEqual(loaded->request, current)) return loaded->name;
  return loaded->name + " (modified)";
}

std::string exportAsBlockedReason(bool documentOpen, const ExportValidation& validation,
                                  std::string_view outputPath) {
  // Order is the order a user would hit them: there is nothing to export
  // before there is a way to export it, and both come before where to put it.
  if (!documentOpen)
    return "Nothing to export: no document is open. File > New Document makes one.";
  if (!validation.ok) return validation.error;
  if (outputPath.empty())
    return "No output file yet: type a path above, or use Choose... to pick one.";
  return std::string();
}

std::string exportStatesBlockedReason(bool documentOpen, std::size_t selectedCount,
                                      const ExportStatesReport& plan) {
  if (!documentOpen)
    return "Nothing to export: no document is open. File > New Document makes one.";
  // Before the plan, because a plan over an empty selection is not a plan --
  // io/ExportStates reads an empty `selection` as "all of them", which is the
  // opposite of what an empty set of checkboxes means here.
  if (selectedCount == 0) return "Nothing is selected, so there is nothing to export.";
  if (!plan.error.empty()) return plan.error;
  return std::string();
}

}  // namespace np
