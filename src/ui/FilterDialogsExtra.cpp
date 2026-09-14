#include "ui/FilterDialogsExtra.hpp"

#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/FilterCommandsExtra.hpp"
#include "app/FilterOps.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/FilterDialogsExtra.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp.
namespace np {
namespace {

bool g_highpassRequested = false;
bool g_localContrastRequested = false;
bool g_lensCorrectRequested = false;

// The one door every Filter/Adjustments dialog commits through
// (docs/automation.md §2.3) is `ui/MacPaintUI.cpp`'s file-scope
// `pixelOpFooter()`, which is not declared in any header -- ui/FillDialog.cpp
// made this identical copy first and its own comment gives the full argument
// for copying rather than widening `ui/MacPaintUI.hpp`'s surface for it: the
// three pieces that ARE exported (`dialogStatusLine()`, `DialogFooter`,
// `runPixelCommand()`) are the whole of what this needs.
//
// **The same deliberate omission ui/FillDialog.cpp's own copy states**:
// `pixelOpFooter()` hands a closing success's sentence to `g_docStatus` so it
// stays readable after the popup closes. That variable has no extern
// declaration anywhere, so this copy cannot reach it -- the sentence is shown
// in the dialog itself while it is open (via `status` below) and is lost on a
// successful close instead of surviving into the chrome's message band; a
// real but narrow gap, not a silent one.
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

// The live canvas preview every dialog here wants, through the two door
// functions ui/MacPaintUI.hpp exports for it -- see that header's own comment
// on `setExternalFilterPreview()`. Same shape as ui/MacPaintUI.cpp's own
// (private) `updateFilterPreview()`: refusal and "nothing changed" both clear
// rather than show a preview identical to the live document.
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

// ops/Filters.hpp §6: "a sigma of a few texels sharpens, a sigma of a few
// percent of the image's short side is the clarity slider" -- 30 sits between
// those two readings on this app's own default 1024-texel canvas, without
// claiming to be the one true default that header says does not exist.
// `amount` at 0.5 is a visible but restrained boost either way.
LocalContrastParams defaultLocalContrastParams() noexcept {
  LocalContrastParams p;
  p.blur.sigma = 30.0f;
  p.amount = 0.5f;
  return p;
}

}  // namespace

void requestHighpassDialog() { g_highpassRequested = true; }

void drawHighpassDialog(AppState& st) {
  // ops/Blur.hpp's own worked examples use this -- the same default
  // `drawGaussianBlurDialog()` starts from, since this is the identical
  // engine (`BlurParams`, Gaussian only) feeding `src - blur(src)` instead of
  // a straight blur.
  static float sigma = 8.0f;
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_highpassRequested) {
    g_highpassRequested = false;
    status.clear();
    ImGui::OpenPopup("Highpass");
  }
  if (!beginDialog("Highpass")) {
    wasOpen = false;
    clearExternalFilterPreview("Highpass");
    return;
  }

  OpenDocument* od = st.documents.active();
  const DialogEdit edited = dialogSlider("Radius", &sigma, 0.0f, 250.0f, "%.1f", "px");
  dialogHint("0 leaves the image unchanged. The preview updates when you release the slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewHighpass, sigma, "Highpass");
  wasOpen = true;

  filterExtraFooter(od, status, highpassCommand(sigma),
                    "Nothing changed (radius 0, or no selected texels).");
  endDialog();
}

void requestLocalContrastDialog() { g_localContrastRequested = true; }

void drawLocalContrastDialog(AppState& st) {
  static LocalContrastParams params = defaultLocalContrastParams();
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_localContrastRequested) {
    g_localContrastRequested = false;
    status.clear();
    ImGui::OpenPopup("Local Contrast");
  }
  if (!beginDialog("Local Contrast")) {
    wasOpen = false;
    clearExternalFilterPreview("Local Contrast");
    return;
  }

  OpenDocument* od = st.documents.active();
  DialogEdit edited = dialogSlider("Radius", &params.blur.sigma, 0.0f, 250.0f, "%.1f", "px");
  // Amount may be negative -- ops/Filters.hpp §6 -- so its own slider spans
  // both directions rather than starting at 0 the way every purely-additive
  // filter's does.
  edited |= dialogSlider("Amount", &params.amount, -2.0f, 2.0f, "%.2f");
  dialogHint(
      "0 leaves the image unchanged. Negative flattens; positive adds clarity. The preview "
      "updates when you release a slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewLocalContrast, params, "Local Contrast");
  wasOpen = true;

  filterExtraFooter(od, status, localContrastCommand(params),
                    "Nothing changed (amount 0, or no selected texels).");
  endDialog();
}

void requestLensCorrectDialog() { g_lensCorrectRequested = true; }

void drawLensCorrectDialog(AppState& st) {
  static LensParams params;
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_lensCorrectRequested) {
    g_lensCorrectRequested = false;
    status.clear();
    ImGui::OpenPopup("Lens Correction");
  }
  if (!beginDialog("Lens Correction")) {
    wasOpen = false;
    clearExternalFilterPreview("Lens Correction");
    return;
  }

  OpenDocument* od = st.documents.active();
  // ops/Lens.hpp §1-2: k1/k2 are the radial coefficients (positive corrects
  // barrel distortion), caRed/caBlue the lateral chromatic-aberration scale.
  // A 0.02 range on the CA pair matches that header's own "0.001 is a strong
  // correction" -- the slider needed headroom past it, not up to it.
  DialogEdit edited = dialogSlider("K1", &params.k1, -1.0f, 1.0f, "%.3f");
  edited |= dialogSlider("K2", &params.k2, -1.0f, 1.0f, "%.3f");
  edited |= dialogSlider("Red/cyan fringe", &params.caRed, -0.02f, 0.02f, "%.4f");
  edited |= dialogSlider("Blue/yellow fringe", &params.caBlue, -0.02f, 0.02f, "%.4f");
  dialogHint(
      "All zero leaves the image unchanged. Positive K1 corrects barrel distortion. The preview "
      "updates when you release a slider.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewLensCorrect, params, "Lens Correction");
  wasOpen = true;

  filterExtraFooter(od, status, lensCorrectCommand(params),
                    "Nothing changed (all coefficients 0, invalid, or no selected texels).");
  endDialog();
}

}  // namespace np
