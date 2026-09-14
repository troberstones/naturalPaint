#include "ui/DustScratchesDialog.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/FilterCommandsFilters.hpp"
#include "app/FilterOps.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/DustScratchesDialog.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_dustScratchesRequested = false;

// ui/FilterDialogsExtra.cpp's own copy of `ui/MacPaintUI.cpp`'s file-scope
// `pixelOpFooter()`, restated here for the identical reason that file's own
// comment gives: `pixelOpFooter()` is not declared in any header, and the
// three pieces that ARE exported (`dialogStatusLine()`, `DialogFooter`,
// `runPixelCommand()`) are the whole of what a dialog in its own file needs.
void filterExtraFooter(OpenDocument* od, std::string& status, const Command& command,
                       const char* nothingChangedText) {
  dialogStatusLine(DialogStatus::Error, status);
  DialogFooter footer;
  footer.commit = "Apply";
  footer.commitEnabled = od != nullptr;
  switch (dialogFooter(footer)) {
    case DialogAction::Commit: {
      const PixelCommandOutcome out = runPixelCommand(*od, command, nothingChangedText);
      if (!out.closeDialog) {
        status = out.status;
        break;
      }
      status.clear();
      ImGui::CloseCurrentPopup();
      break;
    }
    case DialogAction::Cancel:
      status.clear();
      ImGui::CloseCurrentPopup();
      break;
    default:
      break;
  }
}

// The live canvas preview, through the two door functions ui/MacPaintUI.hpp
// exports for it -- ui/FilterDialogsExtra.cpp's own `updateExternalPreview()`,
// restated.
template <typename PreviewFn, typename Params>
void updateExternalPreview(OpenDocument* od, PreviewFn previewFn, const Params& params,
                           const char* owner) {
  if (od != nullptr) {
    TileStore tiles;
    const FilterOpResult r = previewFn(*od, params, &tiles);
    if (r.refusal == PixelOpRefusal::None && r.texelsChanged > 0) {
      if (const std::optional<size_t> idx = activeLayerIndex(*od)) {
        setExternalFilterPreview(od->id, *idx, std::move(tiles), owner);
        return;
      }
    }
  }
  clearExternalFilterPreview(owner);
}

}  // namespace

void requestDustScratchesDialog() { g_dustScratchesRequested = true; }

void drawDustScratchesDialog(AppState& st) {
  static int radius = 2;             // texels; ops/Filters.hpp §11
  static float threshold = 0.06f;    // shaper-domain magnitude, unsharp's own scale
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_dustScratchesRequested) {
    g_dustScratchesRequested = false;
    status.clear();
    ImGui::OpenPopup("Dust & Scratches");
  }
  if (!beginDialog("Dust & Scratches")) {
    wasOpen = false;
    clearExternalFilterPreview("Dust & Scratches");
    return;
  }

  OpenDocument* od = st.documents.active();
  DialogEdit edited = dialogSliderInt("Radius", &radius, 1, 8, "px");
  edited |= dialogSlider("Threshold", &threshold, 0.0f, 0.5f, "%.3f");
  dialogHint(
      "Replaces a texel with its neighbourhood's median only where it differs from that "
      "median by more than the threshold, so clean areas are left untouched. The preview "
      "updates when you release a slider.");

  const DustScratchesParams params{radius, threshold};

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewDustScratches, params, "Dust & Scratches");
  wasOpen = true;

  filterExtraFooter(od, status, dustScratchesCommand(params),
                    "Nothing changed (nothing exceeded the threshold, or no selected texels).");
  endDialog();
}

}  // namespace np
