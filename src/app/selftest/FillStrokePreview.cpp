#include "app/selftest/Support.hpp"

#include <array>
#include <cstdio>
#include <string>

#include "app/AppState.hpp"
#include "app/Command.hpp"
#include "app/CommandsFill.hpp"
#include "app/Recorder.hpp"
#include "app/SelfTest.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // FindWindowByName: is the dialog open
#include "ui/AtelierTheme.hpp"
#include "ui/BlurDialogsExtra.hpp"
#include "ui/FillDialog.hpp"
#include "ui/MacPaintUI.hpp"

// Stroke with nothing selected traces the layer's content edge, through the
// `stroke` command; and the Fill and Stroke dialogs preview, through headless
// ImGui frames, exactly what Apply commits.
namespace np {
namespace {

constexpr int32_t kSize = 128;
const std::array<float, 4> kClear{0, 0, 0, 0};
const std::array<float, 4> kRedPx{1, 0, 0, 1};
const std::array<float, 4> kBluePx{0, 0, 1, 1};

std::array<float, 4> texel(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  return t == nullptr ? kClear : t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

void put(TileStore& store, int32_t x, int32_t y, const std::array<float, 4>& px) {
  store.getOrCreate(tileCoordAt(PixelCoord{x, y})).writePixel(tileLocalOffset(PixelCoord{x, y}), px);
}

// An opaque red square [32, 96) on a transparent layer, nothing selected.
OpenDocument squareDocument(const char* name) {
  OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, name);
  TileStore& tiles = *od.document.layers[0].rgbTiles;
  for (int32_t y = 32; y < 96; ++y)
    for (int32_t x = 32; x < 96; ++x) put(tiles, x, y, kRedPx);
  od.recordEdit("square fixture", EditKind::Content);
  return od;
}

bool sameTexels(const TileStore& a, const TileStore& b) {
  for (int32_t y = 0; y < kSize; ++y)
    for (int32_t x = 0; x < kSize; ++x)
      if (texel(a, x, y) != texel(b, x, y)) return false;
  return true;
}

Command blueStroke(StrokeLocation location, float width) {
  StrokeParams sp;
  sp.fill.source = FillSource::Color;
  sp.fill.color = {0.0f, 0.0f, 1.0f};
  sp.width = width;
  sp.location = location;
  return strokeCommand(sp);
}

}  // namespace

bool runFillStrokePreviewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. stroke with nothing selected traces the content edge --\n");
  {
    OpenDocument od = squareDocument("edge outside");
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Outside, 8.0f));
    check(r.ok && r.texelsChanged > 0, "Outside 8 with no selection runs");
    const TileStore& s = *od.document.layers[0].rgbTiles;
    check(texel(s, 24, 64) == kBluePx && texel(s, 31, 64) == kBluePx,
          "Outside: both ends of the band [24, 32) are stroked");
    check(texel(s, 23, 64) == kClear, "Outside: the texel past the band stays transparent");
    check(texel(s, 32, 64) == kRedPx && texel(s, 64, 64) == kRedPx,
          "Outside: the content itself is untouched");
  }
  {
    OpenDocument od = squareDocument("edge inside");
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Inside, 8.0f));
    const TileStore& s = *od.document.layers[0].rgbTiles;
    check(r.ok && texel(s, 32, 64) == kBluePx && texel(s, 39, 64) == kBluePx,
          "Inside: the band [32, 40) is stroked");
    check(texel(s, 40, 64) == kRedPx && texel(s, 31, 64) == kClear,
          "Inside: past the band is red, outside the content is transparent");
  }
  {
    OpenDocument od = squareDocument("edge center");
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Center, 8.0f));
    const TileStore& s = *od.document.layers[0].rgbTiles;
    check(r.ok && texel(s, 28, 64) == kBluePx && texel(s, 35, 64) == kBluePx,
          "Center: the band [28, 36) is stroked");
    check(texel(s, 27, 64) == kClear && texel(s, 36, 64) == kRedPx,
          "Center: either side of the band is untouched");
  }
  {
    // Column 96 at 50% alpha: the traced edge moves half a texel out, so the
    // outer edge of an 8 px Outside band lands mid-texel 104.
    OpenDocument od = squareDocument("edge half alpha");
    for (int32_t y = 32; y < 96; ++y)
      put(*od.document.layers[0].rgbTiles, 96, y, {0.5f, 0.0f, 0.0f, 0.5f});
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Outside, 8.0f));
    const TileStore& s = *od.document.layers[0].rgbTiles;
    const std::array<float, 4> mid = texel(s, 104, 64);
    check(r.ok && mid[3] > 0.25f && mid[3] < 0.75f && texel(s, 103, 64) == kBluePx &&
              texel(s, 105, 64) == kClear,
          "a 50%-alpha edge column puts the band's outer edge mid-texel");
  }
  {
    OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, "edge empty");
    od.document.layers[0].name = "Empty Sky";  // createBlank() leaves it unnamed
    const std::string name = od.document.layers[0].name;
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Center, 4.0f));
    check(!r.ok && !name.empty() && r.status.find("\"" + name + "\"") != std::string::npos,
          "an empty layer with nothing selected is refused, naming the layer");
    const OpenDocument blank = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, "blank");
    check(sameTexels(*od.document.layers[0].rgbTiles, *blank.document.layers[0].rgbTiles),
          "and nothing was painted");
  }
  {
    OpenDocument od = squareDocument("edge recorded");
    Recorder& recorder = sessionRecorder();
    recorder.arm(od);
    const CommandResult r = applyCommand(od, blueStroke(StrokeLocation::Outside, 4.0f));
    check(r.ok && recorder.steps().size() == 1 && recorder.steps()[0].id == "stroke",
          "the selection-free stroke records as a step");
    recorder.stop();
    OpenDocument replayed = squareDocument("edge replayed");
    const CommandResult again = applyCommand(replayed, recorder.steps().empty()
                                                           ? Command{}
                                                           : recorder.steps()[0]);
    check(again.ok && sameTexels(*replayed.document.layers[0].rgbTiles,
                                 *od.document.layers[0].rgbTiles),
          "replaying the recorded step paints the same texels");
  }

  std::printf("  -- B. the Fill and Stroke dialogs preview what Apply commits --\n");
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
  const DocumentId id = st.documents.add(squareDocument("dialogs"))->id;
  OpenDocument& od = *st.documents.find(id);
  auto layer = [&]() -> const TileStore& { return *od.document.layers[0].rgbTiles; };

  // In ui/MacPaintUI.cpp's order: an unnamed External dialog clears the
  // preview every frame it is closed, before Fill and Stroke draw.
  auto frame = [&]() {
    io.AddMousePosEvent(1270.0f, 790.0f);
    ImGui::NewFrame();
    drawRadialBlurDialog(st);
    drawFillDialog(st);
    drawStrokeDialog(st);
    ImGui::EndFrame();
  };
  auto press = [&](ImGuiKey key) {
    io.AddKeyEvent(key, true);
    frame();
    io.AddKeyEvent(key, false);
    for (int i = 0; i < 3; ++i) frame();
  };
  auto isOpen = [](const char* name) {
    const ImGuiWindow* w = ImGui::FindWindowByName(name);
    return w != nullptr && w->Active;
  };

  // Opens a dialog and checks its preview; returns a copy of the preview.
  auto openAndCheck = [&](void (*request)(), const char* name) {
    const TileStore before = layer();
    request();
    for (int i = 0; i < 8; ++i) frame();
    const TileStore* preview = externalFilterPreviewTiles();
    std::string what = std::string(name) + ": open, previewing on the document";
    check(isOpen(name) && externalFilterPreviewDocument() == id && preview != nullptr,
          what.c_str());
    TileStore copy = preview != nullptr ? *preview : TileStore{};
    what = std::string(name) + ": the preview differs from the layer, which is unchanged";
    check(preview != nullptr && !sameTexels(copy, layer()) && sameTexels(layer(), before),
          what.c_str());
    return copy;
  };

  {
    const TileStore before = layer();
    openAndCheck(requestStrokeDialog, "Stroke");
    press(ImGuiKey_Escape);
    check(!isOpen("Stroke") && externalFilterPreviewDocument() == 0,
          "Stroke: Escape closes and clears the preview");
    check(sameTexels(layer(), before), "Stroke: Cancel leaves the layer as it was");

    const TileStore previewed = openAndCheck(requestStrokeDialog, "Stroke");
    press(ImGuiKey_Enter);
    check(!isOpen("Stroke") && sameTexels(layer(), previewed),
          "Stroke: Apply commits exactly the previewed layer");
    check(externalFilterPreviewDocument() == 0, "Stroke: the preview is gone after Apply");
  }
  {
    const TileStore before = layer();
    openAndCheck(requestFillDialog, "Fill");
    press(ImGuiKey_Escape);
    check(!isOpen("Fill") && externalFilterPreviewDocument() == 0 && sameTexels(layer(), before),
          "Fill: Escape clears the preview and leaves the layer");

    const TileStore previewed = openAndCheck(requestFillDialog, "Fill");
    press(ImGuiKey_Enter);
    check(!isOpen("Fill") && sameTexels(layer(), previewed),
          "Fill: Apply commits exactly the previewed layer");
  }

  ImGui::DestroyContext(context);
  ImGui::SetCurrentContext(previous);

  std::printf("[selftest] fillStrokePreview %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
