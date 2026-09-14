#include "ui/BlurDialogsExtra.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "app/AppState.hpp"
#include "app/BlurCommandsExtra.hpp"
#include "app/FilterOps.hpp"
#include "app/RadialBlurHandles.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
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

// Shared with the canvas handles, which draw later in the same frame.
RadialBlurParams g_radialParams;
bool g_radialDialogOpen = false;
bool g_radialHandleSettled = false;
RadialBlurHandleDrag g_radialDrag;
ImVec2 g_radialKeepClear{0.0f, 0.0f};  // the blur centre on screen, for where the dialog opens

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
  RadialBlurParams& params = g_radialParams;
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
  if (!beginDialogAwayFrom("Radial Blur", g_radialKeepClear)) {
    wasOpen = false;
    g_radialDialogOpen = false;
    clearExternalFilterPreview();
    return;
  }
  g_radialDialogOpen = true;

  OpenDocument* od = st.documents.active();

  DialogEdit edited;
  if (g_radialHandleSettled) {
    g_radialHandleSettled = false;
    edited.changed = true;
    edited.settled = true;
  }
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
      "Spin blurs along circles about the centre; Zoom blurs along rays from it. Drag the "
      "handles on the canvas to move the centre and set the amount. Amount 0 leaves the "
      "image unchanged. The preview updates when you release a slider or a handle.");

  if (edited.settled || !wasOpen) updateExternalPreview(od, previewRadialBlur, params);
  wasOpen = true;

  filterExtraFooter(od, status, radialBlurCommand(params),
                    "Nothing changed (amount 0, or no selected texels).");
  endDialog();
}

void drawRadialBlurCanvasHandles(AppState& st, const ViewTransform& view, Vec2 paneMin,
                                 Vec2 paneMax, ImDrawList* dl) {
  const OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    g_radialDrag = RadialBlurHandleDrag{};
    return;
  }
  if (!g_radialDialogOpen) {
    // The dialog re-centres the blur here when it opens, so it opens away from it.
    const PixelCoord c = defaultBlurCenter(*od);
    const Vec2 s = view.toScreen(Vec2{static_cast<float>(c.x), static_cast<float>(c.y)});
    g_radialKeepClear = ImVec2(s.x, s.y);
    g_radialDrag = RadialBlurHandleDrag{};
    return;
  }
  const float docW = static_cast<float>(od->document.width);
  const float docH = static_cast<float>(od->document.height);

  // The dialog is modal, so ImGui reports no hover for the canvas. The handles
  // read the pointer themselves, only where no window (the dialog) is under it.
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const Vec2 pointer{mouse.x, mouse.y};
  const bool overCanvas = !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) &&
                          pointer.x >= paneMin.x && pointer.x < paneMax.x &&
                          pointer.y >= paneMin.y && pointer.y < paneMax.y;

  RadialBlurHandle hovered = RadialBlurHandle::None;
  if (overCanvas)
    hovered = radialBlurHandleAt(radialBlurHandleShape(g_radialParams, view), pointer);
  if (g_radialDrag.handle == RadialBlurHandle::None && hovered != RadialBlurHandle::None &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g_radialDrag = beginRadialBlurHandleDrag(hovered, g_radialParams, view, pointer);
  }
  if (g_radialDrag.handle != RadialBlurHandle::None) {
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      updateRadialBlurHandleDrag(g_radialDrag, view, pointer, docW, docH, &g_radialParams);
    } else {
      g_radialDrag = RadialBlurHandleDrag{};
      g_radialHandleSettled = true;
    }
  }

  const RadialBlurHandleShape shape = radialBlurHandleShape(g_radialParams, view);
  const RadialBlurHandle lit =
      g_radialDrag.handle != RadialBlurHandle::None ? g_radialDrag.handle : hovered;
  const ImU32 casing = IM_COL32(0, 0, 0, 160);
  const ImU32 accent = atelierToken(kAccent);
  const ImVec2 c(shape.center.x, shape.center.y);
  g_radialKeepClear = c;

  if (g_radialParams.method == RadialBlurMethod::Spin) {
    dl->AddCircle(c, kRadialBlurGuidePx, IM_COL32(0, 0, 0, 90), 96, 3.0f);
    dl->AddCircle(c, kRadialBlurGuidePx, IM_COL32(255, 255, 255, 110), 96, 1.0f);
  } else {
    // A tick where the handle rests at amount 0.
    const float dx = shape.rest.x - shape.center.x, dy = shape.rest.y - shape.center.y;
    const float len = std::max(1e-3f, std::hypot(dx, dy));
    const float nx = -dy / len * 6.0f, ny = dx / len * 6.0f;
    const ImVec2 t0(shape.rest.x - nx, shape.rest.y - ny), t1(shape.rest.x + nx, shape.rest.y + ny);
    dl->AddLine(t0, t1, casing, 3.0f);
    dl->AddLine(t0, t1, IM_COL32(255, 255, 255, 200), 1.0f);
  }
  if (shape.guide.size() >= 2) {
    std::vector<ImVec2> pts;
    pts.reserve(shape.guide.size());
    for (const Vec2& g : shape.guide) pts.emplace_back(g.x, g.y);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), casing, ImDrawFlags_None, 4.0f);
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), accent, ImDrawFlags_None, 2.0f);
  }

  const float amountGrow = lit == RadialBlurHandle::Amount ? 1.5f : 0.0f;
  const ImVec2 h(shape.amountHandle.x, shape.amountHandle.y);
  dl->AddCircleFilled(h, 5.0f + amountGrow, casing);
  dl->AddCircleFilled(h, 3.5f + amountGrow, accent);

  const float centreGrow = lit == RadialBlurHandle::Center ? 1.5f : 0.0f;
  dl->AddCircleFilled(c, 5.5f + centreGrow, casing);
  dl->AddCircleFilled(c, 4.0f + centreGrow,
                      lit == RadialBlurHandle::Center ? accent : IM_COL32(255, 255, 255, 245));
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
