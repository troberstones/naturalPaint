#include "app/selftest/Support.hpp"

#include <cstdio>
#include <cstring>
#include <optional>

#include "app/AppState.hpp"
#include "app/Command.hpp"
#include "app/Recorder.hpp"
#include "app/SelfTest.hpp"
#include "core/SelectionMask.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName: is the dialog open
#include "ui/AtelierTheme.hpp"
#include "ui/MacPaintUI.hpp"

// Drives the real Grow, Luminance Range and Colour Range dialogs through
// headless ImGui frames: the candidate selection is previewed while open,
// Cancel leaves the document's selection byte-identical, and Apply commits
// exactly the previewed selection through the recorder.
namespace np {
namespace {

bool sameSelection(const std::optional<Selection>& a, const std::optional<Selection>& b) {
  if (a.has_value() != b.has_value()) return false;
  if (!a.has_value()) return true;
  if (a->tiles.occupiedTileCount() != b->tiles.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a->tiles) {
    const SelectionTile* other = b->tiles.find(coord);
    if (other == nullptr ||
        std::memcmp(tile.data(), other->data(), SelectionTile::kTexelCount) != 0)
      return false;
  }
  return true;
}

// Dark left half, bright right half, so a luminance band selects one side.
void fillHalves(OpenDocument& doc) {
  const int32_t w = static_cast<int32_t>(doc.document.width);
  const int32_t h = static_cast<int32_t>(doc.document.height);
  TileStore& tiles = *doc.document.layers[0].rgbTiles;
  for (int32_t y = 0; y < h; ++y)
    for (int32_t x = 0; x < w; ++x) {
      const float v = x < w / 2 ? 0.05f : 0.8f;
      tiles.getOrCreate(TileCoord{x / kTileSize, y / kTileSize})
          .writePixel(PixelCoord{x % kTileSize, y % kTileSize}, {v, v, v, 1.0f});
    }
  doc.recordEdit("select preview fixture", EditKind::Content);
}

}  // namespace

bool runSelectDialogPreviewTest() {
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
  io.DisplaySize = ImVec2(1280.0f, 800.0f);
  io.DeltaTime = 1.0f / 60.0f;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  io.Fonts->AddFontDefault();
  io.IniFilename = nullptr;
  io.ConfigInputTrickleEventQueue = false;

  AppState st;
  const DocumentId id = st.documents.add(makeBlankOpenDocument(256, 256, WorkingSpace{}, "Sel"))->id;
  OpenDocument& od = *st.documents.find(id);
  fillHalves(od);
  od.selection = selectRectangle(64.0f, 64.0f, 192.0f, 192.0f);
  ++od.selectionRevision;

  const ImVec2 idle(1270.0f, 790.0f);
  auto frame = [&]() {
    io.AddMousePosEvent(idle.x, idle.y);
    ImGui::NewFrame();
    drawSelectMenuModals(st);
    ImGui::EndFrame();
  };
  auto press = [&](ImGuiKey key) {
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    for (int i = 0; i < 3; ++i) frame();
  };
  auto open = [&](MenuAction action) {
    requestSelectMenuDialog(action);
    for (int i = 0; i < 4; ++i) frame();
  };
  auto isOpen = [](const char* name) {
    const ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w != nullptr && w->Active;
  };
  Recorder& recorder = sessionRecorder();

  std::printf("  -- A. Grow previews its candidate without touching the selection --\n");
  const std::optional<Selection> before = od.selection;
  const uint64_t revisionBefore = od.selectionRevision;
  std::optional<Selection> grown;
  open(MenuAction::SelectGrow);
  {
    check(isOpen("Grow Selection"), "Grow Selection is open");
    const Selection* preview = selectionDialogPreview(id);
    check(preview != nullptr, "a candidate selection is previewed");
    if (preview != nullptr) grown = *preview;
    check(grown.has_value() && !sameSelection(grown, before),
          "the candidate differs from the document's selection");
    check(sameSelection(od.selection, before) && od.selectionRevision == revisionBefore,
          "the document's selection is unchanged while the dialog is open");
  }

  std::printf("  -- B. Escape leaves the selection byte-identical --\n");
  press(ImGuiKey_Escape);
  check(!isOpen("Grow Selection"), "Escape closes Grow Selection");
  check(selectionDialogPreview(id) == nullptr, "closing clears the preview");
  check(sameSelection(od.selection, before) && od.selectionRevision == revisionBefore,
        "the selection is byte-identical to before the dialog opened");

  std::printf("  -- C. Apply commits exactly the previewed selection, recorded --\n");
  open(MenuAction::SelectGrow);
  {
    const Selection* preview = selectionDialogPreview(id);
    check(preview != nullptr && grown.has_value() && sameSelection(*preview, grown),
          "reopening previews the same candidate");
    recorder.arm(od);
    press(ImGuiKey_Enter);
    check(!isOpen("Grow Selection"), "Return applies and closes");
    check(sameSelection(od.selection, grown), "the committed selection equals the preview");
    check(recorder.steps().size() == 1 && recorder.steps()[0].id == "select_grow",
          "Apply recorded one select_grow step");
    recorder.stop();
    check(selectionDialogPreview(id) == nullptr, "the preview is gone after Apply");
  }

  std::printf("  -- D. Luminance Range recomputes on a settled edit --\n");
  luminanceRangeDialogValues() = LuminanceRangeDialogValues{0.0f, 1.0f, 0.0f};
  const std::optional<Selection> beforeLum = od.selection;
  open(MenuAction::SelectLuminanceRange);
  {
    check(isOpen("Luminance Range"), "Luminance Range is open");
    std::optional<Selection> wide;
    if (const Selection* p = selectionDialogPreview(id)) wide = *p;
    check(wide.has_value() && !sameSelection(wide, beforeLum),
          "the 0..1 band previews a candidate unlike the selection");
    luminanceRangeDialogValues().low = 0.5f;
    frame();
    std::optional<Selection> bright;
    if (const Selection* p = selectionDialogPreview(id)) bright = *p;
    check(bright.has_value() && !sameSelection(bright, wide),
          "raising Low recomputes the candidate");
    check(bright.has_value() && selectionCoverageAt(&*bright, PixelCoord{200, 100}) == 1.0f &&
              selectionCoverageAt(&*bright, PixelCoord{20, 100}) == 0.0f,
          "the new candidate holds the bright half only");
    check(sameSelection(od.selection, beforeLum), "the document's selection is still unchanged");
    recorder.arm(od);
    press(ImGuiKey_Enter);
    check(sameSelection(od.selection, bright), "Apply commits the recomputed candidate");
    check(recorder.steps().size() == 1 && recorder.steps()[0].id == "select_luminance_range",
          "Apply recorded one select_luminance_range step");
    recorder.stop();
  }

  std::printf("  -- E. Colour Range previews, and Cancel leaves the selection alone --\n");
  const std::optional<Selection> beforeColour = od.selection;
  const uint64_t revisionColour = od.selectionRevision;
  open(MenuAction::SelectColourRange);
  {
    check(isOpen("Colour Range"), "Colour Range is open");
    const Selection* p = selectionDialogPreview(id);
    check(p != nullptr && !sameSelection(*p, beforeColour),
          "a candidate unlike the selection is previewed");
    press(ImGuiKey_Escape);
    check(selectionDialogPreview(id) == nullptr, "Escape clears the preview");
    check(sameSelection(od.selection, beforeColour) && od.selectionRevision == revisionColour,
          "the selection is byte-identical after Cancel");
  }

  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);

  std::printf("[selftest] selectDialogPreview %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
