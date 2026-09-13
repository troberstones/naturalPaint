#include "ui/RepairDialogs.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/RepairCommandsExtra.hpp"
#include "app/FilterOps.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/RepairDialogs.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_contentAwareFillRequested = false;
bool g_seamHealRequested = false;

// A cheap reseed for the "Recompute" button -- splitmix64's own constants,
// one step, so pressing it repeatedly walks a long, non-repeating sequence
// without needing a system RNG a dialog would then have to seed itself.
uint64_t nextRepairSeed(uint64_t s) noexcept {
  s += 0x9e3779b97f4a7c15ULL;
  s = (s ^ (s >> 30)) * 0xbf58476d1ce4e5b9ULL;
  return (s ^ (s >> 27)) * 0x94d049bb133111ebULL;
}

template <typename PreviewFn, typename Params>
void updateRepairPreview(OpenDocument* od, PreviewFn previewFn, const Params& params) {
  if (od != nullptr) {
    TileStore tiles;
    const FilterOpResult r = previewFn(*od, params, &tiles);
    if (r.refusal == PixelOpRefusal::None && r.texelsChanged > 0) {
      if (const std::optional<size_t> idx = activeLayerIndex(*od)) {
        setExternalFilterPreview(od->id, *idx, std::move(tiles));
        return;
      }
    }
  }
  clearExternalFilterPreview();
}

}  // namespace

void requestContentAwareFillDialog() { g_contentAwareFillRequested = true; }

void drawContentAwareFillDialog(AppState& st) {
  static ContentAwareFillRequest params;
  static std::string status;
  static bool wasOpen = false;

  if (g_contentAwareFillRequested) {
    g_contentAwareFillRequested = false;
    status.clear();
    ImGui::OpenPopup("Content-Aware Fill");
  }
  if (!beginDialog("Content-Aware Fill")) {
    wasOpen = false;
    clearExternalFilterPreview();
    return;
  }

  OpenDocument* od = st.documents.active();

  DialogEdit edited =
      dialogSliderInt("Patch size", &params.patchRadius, 1, kPatchMatchMaxPatchRadius, "texels");
  edited |= dialogSliderInt("Quality", &params.iterations, 1, kPatchMatchMaxIterations,
                            "iterations");
  dialogHint(
      "Synthesises the SELECTED texels from the surrounding texture, patch by patch. A larger "
      "patch preserves bigger structure; more iterations searches harder for a good match.");

  if (od != nullptr) {
    const PixelOpRefusal reason = contentAwareFillRefusal(*od);
    if (reason != PixelOpRefusal::None)
      dialogStatusLine(DialogStatus::Error,
                       pixelOpRefusalMessage(reason, activeLayerOf(*od), "content-aware fill"));
  }

  if (edited.settled || !wasOpen) updateRepairPreview(od, previewContentAwareFill, params);
  wasOpen = true;

  if (!status.empty()) dialogStatusLine(DialogStatus::Error, status);

  DialogFooter footer;
  footer.commit = "Fill";
  footer.commitEnabled = od != nullptr;
  footer.alternate = "Recompute";
  const DialogAction act = dialogFooter(footer);
  if (act == DialogAction::Alternate) {
    params.seed = nextRepairSeed(params.seed);
    updateRepairPreview(od, previewContentAwareFill, params);
  } else if (act == DialogAction::Commit && od != nullptr) {
    const PixelCommandOutcome out =
        runPixelCommand(*od, contentAwareFillCommand(params),
                       "Nothing changed -- the fill matched what was already there.");
    if (!out.closeDialog) {
      status = out.status;
    } else {
      // `g_docStatus` is `ui/MacPaintUI.cpp`'s own file-static, unreachable
      // from this translation unit -- ui/FilterDialogsExtra.cpp's own comment
      // names the identical gap: a successful close's sentence is shown here
      // while the dialog is open and is lost on close rather than surviving
      // into the chrome's message band.
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  } else if (act == DialogAction::Cancel) {
    status.clear();
    ImGui::CloseCurrentPopup();
  }
  endDialog();
}

void requestSeamHealDialog() { g_seamHealRequested = true; }

void drawSeamHealDialog(AppState& st) {
  static SeamHealRequest params;
  static std::string status;
  static bool wasOpen = false;

  if (g_seamHealRequested) {
    g_seamHealRequested = false;
    status.clear();
    ImGui::OpenPopup("Seam Heal");
  }
  if (!beginDialog("Seam Heal")) {
    wasOpen = false;
    clearExternalFilterPreview();
    return;
  }

  OpenDocument* od = st.documents.active();

  DialogEdit edited = dialogSliderInt("Band width", &params.bandWidth, 1, 256, "texels");
  edited |= dialogSliderInt("Patch size", &params.patchRadius, 1, kPatchMatchMaxPatchRadius,
                            "texels");
  edited |= dialogSliderInt("Quality", &params.iterations, 1, kPatchMatchMaxIterations,
                            "iterations");
  dialogHint(
      "Offsets the layer by half, repairs a band along each seam with synthesised texture, then "
      "offsets back. Everything outside the two bands is left exactly as it was.");

  if (od != nullptr) {
    const PixelOpRefusal reason = seamHealRefusalFor(*od);
    if (reason == PixelOpRefusal::SelectionActive)
      dialogStatusLine(DialogStatus::Error,
                       pixelOpRefusalMessage(reason, activeLayerOf(*od), "seam heal"));
  }

  if (edited.settled || !wasOpen) updateRepairPreview(od, previewSeamHeal, params);
  wasOpen = true;

  if (!status.empty()) dialogStatusLine(DialogStatus::Error, status);

  DialogFooter footer;
  footer.commit = "Heal";
  footer.commitEnabled = od != nullptr;
  footer.alternate = "Recompute";
  const DialogAction act = dialogFooter(footer);
  if (act == DialogAction::Alternate) {
    params.seed = nextRepairSeed(params.seed);
    updateRepairPreview(od, previewSeamHeal, params);
  } else if (act == DialogAction::Commit && od != nullptr) {
    const PixelCommandOutcome out =
        runPixelCommand(*od, seamHealCommand(params),
                       "Nothing changed (band width 0, or no editable layer).");
    if (!out.closeDialog) {
      status = out.status;
    } else {
      // See `drawContentAwareFillDialog()`'s identical comment above: this
      // translation unit cannot reach `g_docStatus`.
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  } else if (act == DialogAction::Cancel) {
    status.clear();
    ImGui::CloseCurrentPopup();
  }
  endDialog();
}

}  // namespace np
