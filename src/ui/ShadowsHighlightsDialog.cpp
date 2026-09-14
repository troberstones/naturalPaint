#include "ui/ShadowsHighlightsDialog.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/FilterCommandsFilters.hpp"
#include "app/FilterOps.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/ShadowsHighlightsDialog.hpp for why this dialog has no
// `request*Dialog()` function of its own: `AppState::requestAdjustment`
// already opens its popup.
namespace np {
namespace {

// ui/DustScratchesDialog.cpp's own copy of `ui/MacPaintUI.cpp`'s file-scope
// `pixelOpFooter()` -- restated a second time rather than shared, for the
// identical reason that file's header gives (`pixelOpFooter()` is not
// declared in any header this new file can reach).
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

// A visible-but-restrained starting point on open, `drawLocalContrastDialog`'s
// own convention -- a dialog opened at the identity shows a flat preview and
// looks broken, not merely idle. Half a stop each way is a real recovery, not
// a token one, without the highlight-clipped or muddy-shadow look a much
// larger stop count produces on an ordinary photograph.
ShadowsHighlightsParams defaultShadowsHighlightsParams() noexcept {
  ShadowsHighlightsParams p;
  p.blur.sigma = 40.0f;
  p.shadows = 0.5f;
  p.highlights = 0.5f;
  return p;  // tonalWidth keeps the engine's own default (0.15)
}

}  // namespace

void drawShadowsHighlightsDialog(AppState& st) {
  static ShadowsHighlightsParams params = defaultShadowsHighlightsParams();
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (!beginDialog("Shadows/Highlights")) {
    wasOpen = false;
    clearExternalFilterPreview("Shadows/Highlights");
    return;
  }

  OpenDocument* od = st.documents.active();
  DialogEdit edited = dialogSlider("Radius", &params.blur.sigma, 1.0f, 250.0f, "%.1f", "px");
  edited |= dialogSlider("Shadows", &params.shadows, 0.0f, 3.0f, "%.2f", "stops");
  edited |= dialogSlider("Highlights", &params.highlights, 0.0f, 3.0f, "%.2f", "stops");
  edited |= dialogSlider("Tonal Width", &params.tonalWidth, 0.02f, 0.5f, "%.3f");
  dialogHint(
      "Shadows lifts and Highlights lowers, in stops, each weighted by a blurred luminance "
      "guide -- not a per-pixel curve, so the same value in a dark neighbourhood and a bright "
      "one is treated differently. Both 0 leaves the image unchanged. The preview updates when "
      "you release a slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewShadowsHighlights, params, "Shadows/Highlights");
  wasOpen = true;

  filterExtraFooter(
      od, status, shadowsHighlightsCommand(params),
      "Nothing changed (shadows and highlights both 0, or no selected texels).");
  endDialog();
}

}  // namespace np
