#include "ui/PolarRemapDialog.hpp"

#include <string>

#include "app/AppState.hpp"
#include "app/FilterOps.hpp"
#include "app/PolarRemapCommand.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// See ui/PolarRemapDialog.hpp for the shape this follows and why it is a
// separate translation unit from ui/MacPaintUI.cpp. `filterExtraFooter()` and
// `updateExternalPreview()` below are ui/BlurDialogsExtra.cpp's own copies,
// copied again rather than shared -- see that file's comment on
// `filterExtraFooter()` for the full argument (the three pieces that ARE
// exported are the whole of what any of these dialogs needs).
namespace np {
namespace {

bool g_polarRemapRequested = false;

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

void updateExternalPreview(OpenDocument* od, const PolarRemapParams& params, const char* owner) {
  if (od != nullptr) {
    TileStore tiles;
    const FilterOpResult r = previewPolarRemap(*od, params, &tiles);
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

void requestPolarRemapDialog() { g_polarRemapRequested = true; }

void drawPolarRemapDialog(AppState& st) {
  static PolarRemapParams params;
  static int directionIdx = 0;  // 0 Rect->Polar, 1 Polar->Rect -- the enum's own order
  static std::string status;
  static bool wasOpen = false;  // see ui/MacPaintUI.cpp's Gaussian Blur dialog

  if (g_polarRemapRequested) {
    g_polarRemapRequested = false;
    status.clear();
    ImGui::OpenPopup("Polar Coordinates");
  }
  if (!beginDialog("Polar Coordinates")) {
    wasOpen = false;
    clearExternalFilterPreview("Polar Coordinates");
    return;
  }

  OpenDocument* od = st.documents.active();

  static const char* kDirections[] = {"Rectangular to Polar", "Polar to Rectangular"};
  const bool changed = dialogRadioRow("Direction", &directionIdx, kDirections, 2);
  params.direction =
      directionIdx == 0 ? PolarRemapDirection::RectToPolar : PolarRemapDirection::PolarToRect;

  dialogHint(
      "Maps the canvas onto a circle inscribed in it, or back -- there is no centre or amount to "
      "set; the frame is the whole parameter, the same as Photoshop's own dialog.");

  if (changed || !wasOpen) updateExternalPreview(od, params, "Polar Coordinates");
  wasOpen = true;

  filterExtraFooter(od, status, polarRemapCommand(params), "");
  endDialog();
}

}  // namespace np
