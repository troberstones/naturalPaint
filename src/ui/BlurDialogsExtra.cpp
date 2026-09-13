#include "ui/BlurDialogsExtra.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/BlurCommandsExtra.hpp"
#include "app/FilterOps.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/BlurDialogsExtra.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp. `filterExtraFooter()` and
// `updateExternalPreview()` below are ui/FilterDialogsExtra.cpp's own copies,
// copied again rather than shared -- see that file's comment on
// `filterExtraFooter()` for the full argument (the three pieces that ARE
// exported are the whole of what any of these dialogs needs).
namespace np {
namespace {

bool g_radialBlurRequested = false;
bool g_lensBlurRequested = false;

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
void updateExternalPreview(OpenDocument* od, PreviewFn previewFn, const Params& params) {
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

void requestRadialBlurDialog() { g_radialBlurRequested = true; }

void drawRadialBlurDialog(AppState& st) {
  static RadialBlurParams params;
  static int methodIdx = 0;  // 0 = Spin, 1 = Zoom -- RadialBlurMethod's order
  static std::string status;
  static bool wasOpen = false;  // see ui/MacPaintUI.cpp's Gaussian Blur dialog

  if (g_radialBlurRequested) {
    g_radialBlurRequested = false;
    status.clear();
    // Re-centred on the document that is active NOW, every time the dialog
    // opens -- `defaultBlurCenter()` is app/FilterOps.hpp's own answer to
    // "what does this canvas look like", the same function `doRadialBlur()`
    // falls back to for a hand-written action that omits the centre.
    if (OpenDocument* od0 = st.documents.active()) {
      const PixelCoord c = defaultBlurCenter(*od0);
      params.centerX = static_cast<float>(c.x);
      params.centerY = static_cast<float>(c.y);
    }
    params.amount = 0.0f;
    ImGui::OpenPopup("Radial Blur");
  }
  if (!beginDialog("Radial Blur")) {
    wasOpen = false;
    clearExternalFilterPreview();
    return;
  }

  OpenDocument* od = st.documents.active();

  DialogEdit edited;
  static const char* kMethods[] = {"Spin", "Zoom"};
  if (dialogRadioRow("Method", &methodIdx, kMethods, 2)) {
    edited.changed = true;
    edited.settled = true;
  }
  params.method = methodIdx == 0 ? RadialBlurMethod::Spin : RadialBlurMethod::Zoom;

  const float maxX = od != nullptr ? static_cast<float>(od->document.width) : 0.0f;
  const float maxY = od != nullptr ? static_cast<float>(od->document.height) : 0.0f;
  edited |= dialogDrag("Center X", &params.centerX, 1.0f, 0.0f, maxX, "%.0f", "px");
  edited |= dialogDrag("Center Y", &params.centerY, 1.0f, 0.0f, maxY, "%.0f", "px");

  // Spin's amount is an arc in degrees; Zoom's is a radial scale fraction --
  // ops/RadialBlur.hpp's own field comment. One shared slot, two ranges,
  // reset to 0 (the identity) whenever the method just changed so a Spin
  // sweep the user dialled in cannot silently become a Zoom scale of the
  // same magnitude.
  static int lastMethodIdx = 0;
  if (methodIdx != lastMethodIdx) {
    params.amount = 0.0f;
    lastMethodIdx = methodIdx;
  }
  if (params.method == RadialBlurMethod::Spin) {
    edited |= dialogSlider("Amount", &params.amount, -90.0f, 90.0f, "%.0f", "\xc2\xb0");
  } else {
    edited |= dialogSlider("Amount", &params.amount, -1.0f, 1.0f, "%.2f");
  }

  int samples = params.samples;
  const DialogEdit samplesEdit = dialogSliderInt("Samples", &samples, 1, 32);
  params.samples = samples;
  edited |= samplesEdit;

  dialogHint(
      "Spin blurs along circles about the centre; Zoom blurs along rays from it. Amount 0 "
      "leaves the image unchanged. The preview updates when you release a slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewRadialBlur, params);
  wasOpen = true;

  filterExtraFooter(od, status, radialBlurCommand(params),
                    "Nothing changed (amount 0, or no selected texels).");
  endDialog();
}

void requestLensBlurDialog() { g_lensBlurRequested = true; }

void drawLensBlurDialog(AppState& st) {
  static LensBlurParams params;
  static int bladeIdx = 0;  // 0 = circle; 1..6 = 3..8 blades
  static std::string status;
  static bool wasOpen = false;  // see ui/MacPaintUI.cpp's Gaussian Blur dialog

  if (g_lensBlurRequested) {
    g_lensBlurRequested = false;
    status.clear();
    ImGui::OpenPopup("Lens Blur");
  }
  if (!beginDialog("Lens Blur")) {
    wasOpen = false;
    clearExternalFilterPreview();
    return;
  }

  OpenDocument* od = st.documents.active();

  int radius = params.radius;
  DialogEdit edited = dialogSliderInt("Radius", &radius, 0, kLensBlurMaxRadius, "px");
  params.radius = radius;

  // A combo rather than a plain slider: `bladeCount` is valid only at 0 or in
  // [3, 8] (ops/LensBlur.hpp), and a slider would let the drag pass through
  // 1 or 2 with no way to say those are refused.
  static const char* kBlades[] = {"Circle", "3 blades", "4 blades", "5 blades",
                                  "6 blades", "7 blades", "8 blades"};
  if (dialogCombo("Aperture", &bladeIdx, kBlades, 7)) {
    edited.changed = true;
    edited.settled = true;
  }
  params.bladeCount = bladeIdx == 0 ? 0 : bladeIdx + 2;

  if (params.bladeCount != 0) {
    float rotationDeg = params.bladeRotationRadians * (180.0f / 3.14159265f);
    const DialogEdit rotEdit = dialogSlider("Rotation", &rotationDeg, 0.0f, 360.0f / static_cast<float>(params.bladeCount),
                                            "%.0f", "\xc2\xb0");
    params.bladeRotationRadians = rotationDeg * (3.14159265f / 180.0f);
    edited |= rotEdit;
  }

  edited |= dialogSlider("Highlight Threshold", &params.highlightThreshold, 0.0f, 4.0f, "%.2f");
  edited |= dialogSlider("Highlight Boost", &params.highlightBoost, 0.0f, 4.0f, "%.2f");

  dialogHint(
      "Radius 0 leaves the image unchanged. A bright texel above the highlight threshold blooms "
      "into the aperture's shape. The preview updates when you release a slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewLensBlur, params);
  wasOpen = true;

  filterExtraFooter(od, status, lensBlurCommand(params),
                    "Nothing changed (radius 0, or no selected texels).");
  endDialog();
}

}  // namespace np
