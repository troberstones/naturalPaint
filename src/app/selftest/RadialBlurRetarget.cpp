#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>

#include "app/AppState.hpp"
#include "app/CanvasView.hpp"
#include "app/FilterOps.hpp"
#include "app/RadialBlurHandles.hpp"
#include "app/SelfTest.hpp"
#include "app/ViewTransform.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName: where the dialog actually landed
#include "ops/RadialBlur.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/BlurDialogsExtra.hpp"
#include "ui/Dialog.hpp"
#include "ui/MacPaintUI.hpp"

// Drives the real Radial Blur dialog, canvas handles and split-pane click
// through headless ImGui frames, in ui/MacPaintUI.cpp's order: the dialog,
// then the focused pane, then the other pane.
namespace np {
namespace {

constexpr float kDisplayW = 1280.0f, kDisplayH = 800.0f;
// A rows split: pane 0 on top, pane 1 below, so the corner away from each
// document's centre differs top to bottom.
const ImVec2 kPaneMin[2] = {ImVec2(0.0f, 0.0f), ImVec2(0.0f, 400.0f)};
const ImVec2 kPaneMax[2] = {ImVec2(900.0f, 400.0f), ImVec2(900.0f, 800.0f)};
const char* kOtherModal = "Retarget Test Other Modal";
// The amount handle dragged 24 px right and 68 px up of the centre: the sweep is
// twice the handle's angle from canvas up, rounded.
const float kDraggedAmount =
    std::round(2.0f * std::atan2(24.0f, 68.0f) * 180.0f / 3.14159265f);  // 39

// A checkerboard, so a spin blur changes texels and the preview exists.
void fillChecker(OpenDocument& doc) {
  const int32_t w = static_cast<int32_t>(doc.document.width);
  const int32_t h = static_cast<int32_t>(doc.document.height);
  TileStore& tiles = *doc.document.layers[0].rgbTiles;
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      Tile& t = tiles.getOrCreate(TileCoord{x / kTileSize, y / kTileSize});
      const float v = ((x / 8 + y / 8) % 2) != 0 ? 0.9f : 0.1f;
      t.writePixel(PixelCoord{x % kTileSize, y % kTileSize}, {v, 1.0f - v, 0.4f, 1.0f});
    }
  }
  doc.recordEdit("retarget fixture", EditKind::Content);
}

ImVec2 paneCentre(int pane) {
  return ImVec2((kPaneMin[pane].x + kPaneMax[pane].x) * 0.5f,
                (kPaneMin[pane].y + kPaneMax[pane].y) * 0.5f);
}

// Each document's centre sits on its pane's centre at zoom 1.
ViewTransform paneView(const OpenDocument& doc, int pane) {
  const ImVec2 c = paneCentre(pane);
  return ViewTransform(CanvasView{},
                       Vec2{static_cast<float>(doc.document.width) * 0.5f,
                            static_cast<float>(doc.document.height) * 0.5f},
                       Vec2{c.x, c.y});
}

bool inRect(ImVec2 p, ImVec2 mn, ImVec2 mx) {
  return p.x >= mn.x && p.x < mx.x && p.y >= mn.y && p.y < mx.y;
}

}  // namespace

bool runRadialBlurRetargetTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  ImGuiContext* previous = ImGui::GetCurrentContext();
  ImGuiContext* context = ImGui::CreateContext();
  ImGui::SetCurrentContext(context);
  applyAtelierTheme();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
  io.DeltaTime = 1.0f / 60.0f;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.Fonts->AddFontDefault();
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;  // a press lands on the frame it is queued

  AppState st;
  const DocumentId bId = st.documents.add(makeBlankOpenDocument(240, 136, WorkingSpace{}, "B"))->id;
  const DocumentId aId = st.documents.add(makeBlankOpenDocument(256, 256, WorkingSpace{}, "A"))->id;
  fillChecker(*st.documents.find(aId));
  fillChecker(*st.documents.find(bId));
  AtelierSplitState split;
  split.mode = AtelierSplit::Rows;

  bool otherModalRequested = false;
  bool otherModalCloseRequested = false;
  auto frame = [&](ImVec2 mouse, bool down) {
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
    ImGui::NewFrame();
    drawRadialBlurDialog(st);
    drawLensBlurDialog(st);  // drawn next in production, and closed
    if (otherModalRequested) {
      otherModalRequested = false;
      ImGui::OpenPopup(kOtherModal);
    }
    if (beginDialog(kOtherModal)) {
      ImGui::TextUnformatted("Another modal.");
      if (otherModalCloseRequested) {
        otherModalCloseRequested = false;
        ImGui::CloseCurrentPopup();
      }
      endDialog();
    }

    const AtelierPaneDocuments panes = atelierPaneDocuments(st.documents, split);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    const int f = panes.focusedPane;
    if (panes.pane[f] != nullptr) {
      ImGui::SetNextWindowPos(kPaneMin[f]);
      ImGui::SetNextWindowSize(ImVec2(kPaneMax[f].x - kPaneMin[f].x, kPaneMax[f].y - kPaneMin[f].y));
      if (ImGui::Begin("##retargetFocusedPane", nullptr, flags)) {
        drawRadialBlurCanvasHandles(st, paneView(*panes.pane[f], f),
                                    Vec2{kPaneMin[f].x, kPaneMin[f].y},
                                    Vec2{kPaneMax[f].x, kPaneMax[f].y},
                                    ImGui::GetWindowDrawList());
      }
      ImGui::End();
    }
    if (panes.count == 2) {
      const int o = 1 - f;
      ImGui::SetNextWindowPos(kPaneMin[o]);
      ImGui::SetNextWindowSize(ImVec2(kPaneMax[o].x - kPaneMin[o].x, kPaneMax[o].y - kPaneMin[o].y));
      if (ImGui::Begin("##retargetOtherPane", nullptr, flags)) {
        if (radialBlurTakesPaneClick(Vec2{kPaneMin[o].x, kPaneMin[o].y},
                                     Vec2{kPaneMax[o].x, kPaneMax[o].y}))
          focusSplitPane(st.documents, split, st.view, o, panes.pane[o]->id);
      }
      ImGui::End();
    }
    ImGui::EndFrame();
  };
  auto click = [&](ImVec2 p) {
    frame(p, false);
    frame(p, true);
    frame(p, false);
  };
  const ImVec2 idle(1270.0f, 790.0f);  // right of the panes and of any dialog corner
  auto activeId = [&]() { return st.documents.active() != nullptr ? st.documents.active()->id : 0; };
  auto dialog = []() { return ImGui::FindWindowByName("Radial Blur"); };
  auto near = [](float a, float b) { return std::fabs(a - b) < 1.0f; };

  const OpenDocument& a = *st.documents.find(aId);
  const OpenDocument& b = *st.documents.find(bId);
  const PixelCoord aCentre = defaultBlurCenter(a);
  const PixelCoord bCentre = defaultBlurCenter(b);

  std::printf("  -- A. fixture: a rows split with A focused on top --\n");
  {
    const AtelierPaneDocuments panes = atelierPaneDocuments(st.documents, split);
    check(panes.count == 2 && activeId() == aId && panes.focusedPane == 0,
          "fixture: two panes, A active in the top pane");
    check(aCentre.x != bCentre.x && aCentre.y != bCentre.y,
          "fixture: the two documents have different centres");
  }

  std::printf("  -- B. the dialog opens away from A's centre --\n");
  requestRadialBlurDialog();
  for (int i = 0; i < 8; ++i) frame(idle, false);
  {
    const ImGuiWindow* w = dialog();
    check(w != nullptr && w->Active, "the dialog is open");
    const RadialBlurParams& p = radialBlurDialogParams();
    check(p.centerX == static_cast<float>(aCentre.x) && p.centerY == static_cast<float>(aCentre.y),
          "centred on A");
    check(w != nullptr && near(w->Pos.x + w->Size.x, kDisplayW - kDialogCornerMargin) &&
              near(w->Pos.y + w->Size.y, kDisplayH - kDialogCornerMargin),
          "bottom-right: A's centre is in the top half");
  }

  std::printf("  -- C. dragging A's amount handle sets the amount and the preview --\n");
  {
    const ImVec2 c = paneCentre(0);
    const ImVec2 rest(c.x, c.y - kRadialBlurGuidePx);
    // ImGui floors the pointer, so the drag ends on whole pixels: 24 right, 68 up.
    const ImVec2 to(c.x + 24.0f, c.y - 68.0f);
    const ImGuiWindow* w = dialog();
    check(w != nullptr && !inRect(rest, w->Pos, w->Pos + w->Size) &&
              !inRect(to, w->Pos, w->Pos + w->Size),
          "fixture: the dialog does not cover the drag");
    frame(rest, false);
    frame(rest, true);
    frame(to, true);
    frame(to, true);
    frame(to, false);
    frame(idle, false);
    frame(idle, false);
    check(radialBlurDialogParams().amount == kDraggedAmount,
          "dragging to 19.4 degrees on Spin sets amount 39 (twice the angle)");
    check(activeId() == aId, "pressing in the focused pane does not change focus");
    check(externalFilterPreviewDocument() == aId, "the preview is on A");
  }

  std::printf("  -- D. a click on the dialog, over the lower pane, does not retarget --\n");
  {
    const ImGuiWindow* w = dialog();
    const ImVec2 onDialog = w != nullptr ? ImVec2(w->Pos.x + 6.0f, w->Pos.y + w->Size.y - 6.0f)
                                         : ImVec2(0.0f, 0.0f);
    check(w != nullptr && inRect(onDialog, kPaneMin[1], kPaneMax[1]) &&
              inRect(onDialog, w->Pos, w->Pos + w->Size),
          "fixture: the point is on the dialog and inside the lower pane");
    click(onDialog);
    frame(idle, false);
    check(activeId() == aId, "A stays focused");
    check(radialBlurDialogParams().centerX == static_cast<float>(aCentre.x) &&
              radialBlurDialogParams().centerY == static_cast<float>(aCentre.y),
          "the centre stays A's");
  }

  std::printf("  -- E. a click in the lower pane moves the blur to B --\n");
  {
    const ImVec2 inB(100.0f, 700.0f);
    const ImGuiWindow* w = dialog();
    check(w != nullptr && !inRect(inB, w->Pos, w->Pos + w->Size), "fixture: the click misses the dialog");
    frame(inB, false);
    frame(inB, true);
    check(activeId() == bId, "the press focuses B");
    frame(inB, false);
    const RadialBlurParams& p = radialBlurDialogParams();
    check(p.centerX == static_cast<float>(bCentre.x) && p.centerY == static_cast<float>(bCentre.y),
          "the next frame re-centres on B");
    check(p.amount == kDraggedAmount && p.method == RadialBlurMethod::Spin,
          "method and amount are kept");
    check(externalFilterPreviewDocument() == bId, "the preview moves to B");
    for (int i = 0; i < 4; ++i) frame(idle, false);
    w = dialog();
    check(w != nullptr && near(w->Pos.x + w->Size.x, kDisplayW - kDialogCornerMargin) &&
              near(w->Pos.y, kDialogCornerMargin),
          "the dialog moves top-right: B's centre is in the bottom half");
    check(w != nullptr && !inRect(paneCentre(1), w->Pos, w->Pos + w->Size),
          "and no longer covers B's centre");
  }

  std::printf("  -- F. under any other modal, a pane click is not the blur's --\n");
  {
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame(idle, false);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame(idle, false);
    frame(idle, false);
    const ImGuiWindow* w = dialog();
    check(w == nullptr || !w->Active, "Escape closes Radial Blur");
    check(externalFilterPreviewDocument() == 0, "closing clears the preview");
    otherModalRequested = true;
    for (int i = 0; i < 4; ++i) frame(idle, false);
    const ImGuiWindow* other = ImGui::FindWindowByName(kOtherModal);
    check(other != nullptr && other->Active, "fixture: another modal is open");
    click(ImVec2(100.0f, 200.0f));
    frame(idle, false);
    check(activeId() == bId, "a click on the top pane leaves B focused");
    check(radialBlurDialogParams().centerX == static_cast<float>(bCentre.x),
          "and the blur's centre is untouched");
  }

  std::printf("  -- G. a corner dialog stays inside the chrome's work area --\n");
  {
    otherModalCloseRequested = true;
    frame(idle, false);
    frame(idle, false);
    const ImGuiWindow* other = ImGui::FindWindowByName(kOtherModal);
    check(other == nullptr || !other->Active, "fixture: the other modal is closed");

    // As if a toolbar filled the top 60 px and a status bar the bottom 40.
    const ImVec2 workMin(0.0f, 60.0f), workMax(kDisplayW, kDisplayH - 40.0f);
    setDialogWorkArea(workMin, workMax);
    requestRadialBlurDialog();
    for (int i = 0; i < 8; ++i) frame(idle, false);
    const ImGuiWindow* w = dialog();
    check(w != nullptr && w->Active, "the dialog reopens");
    check(w != nullptr && near(w->Pos.y, workMin.y + kDialogCornerMargin) &&
              near(w->Pos.x + w->Size.x, workMax.x - kDialogCornerMargin),
          "top-right of the work area, below the toolbar, not of the window");
    setDialogWorkArea(ImVec2(0.0f, 0.0f), ImVec2(0.0f, 0.0f));
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame(idle, false);
    io.AddKeyEvent(ImGuiKey_Escape, false);
    frame(idle, false);
  }

  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);

  std::printf("[selftest] radialBlurRetarget %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
