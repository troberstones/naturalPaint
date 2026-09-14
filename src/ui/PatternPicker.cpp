#include "ui/PatternPicker.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "app/AppState.hpp"
#include "app/DabLibrary.hpp"
#include "app/PatternLibrary.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/DabPicker.hpp"

namespace np {
namespace {

constexpr int kAtlasPx = 1024;
constexpr int kThumbPx = 64;
constexpr float kCellPref = 54.0f;
constexpr float kCellGap = 4.0f;

// Uploaded the first time a cell is on screen, and again only when the library's
// generation says its picture may have changed.
class PatternAtlas {
 public:
  WGPUTextureView viewFor(GpuContext& gpu, PatternLibrary& library, int index) {
    const DabAtlasSlot slot = dabAtlasSlotFor(index, kThumbPx, kAtlasPx);
    if (slot.page < 0) return nullptr;
    while (static_cast<int>(pages_.size()) <= slot.page) {
      Page page;
      WGPUTextureDescriptor td = {};
      td.label = sv("pattern picker atlas");
      td.dimension = WGPUTextureDimension_2D;
      td.size = {static_cast<uint32_t>(kAtlasPx), static_cast<uint32_t>(kAtlasPx), 1};
      td.format = WGPUTextureFormat_RGBA8Unorm;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      page.texture = wgpuDeviceCreateTexture(gpu.device, &td);
      if (page.texture == nullptr) return nullptr;
      page.view = wgpuTextureCreateView(page.texture, nullptr);
      pages_.push_back(page);
    }

    const std::vector<PatternEntry>& entries = library.entries();
    if (index >= static_cast<int>(entries.size())) return pages_[slot.page].view;

    const std::string key = entries[index].id;
    auto& uploaded = pages_[slot.page].uploaded;
    const auto found = uploaded.find(index);
    if (found != uploaded.end() && found->second.first == key &&
        found->second.second == library.generation())
      return pages_[slot.page].view;

    const std::shared_ptr<const PaperField> field = library.resolve(key);
    const std::vector<uint8_t> rgba =
        patternThumbnailRgba(field != nullptr ? *field : PaperField{}, kThumbPx);

    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = pages_[slot.page].texture;
    dst.mipLevel = 0;
    dst.origin = {static_cast<uint32_t>(slot.x), static_cast<uint32_t>(slot.y), 0};
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = static_cast<uint32_t>(kThumbPx) * 4u;
    layout.rowsPerImage = static_cast<uint32_t>(kThumbPx);
    const WGPUExtent3D extent = {static_cast<uint32_t>(kThumbPx),
                                 static_cast<uint32_t>(kThumbPx), 1};
    wgpuQueueWriteTexture(gpu.queue, &dst, rgba.data(), rgba.size(), &layout, &extent);
    uploaded[index] = {key, library.generation()};
    return pages_[slot.page].view;
  }

 private:
  struct Page {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    std::unordered_map<int, std::pair<std::string, uint64_t>> uploaded;
  };
  std::vector<Page> pages_;
};

PatternAtlas g_atlas;
std::vector<std::string> g_scanNotes;

void rescanPatterns(AppState& st) {
  g_scanNotes = st.patternLibrary.rescan();
  std::vector<std::string> notes;
  (void)resolvePatternIds(st.brush.brushLibrary, st.patternLibrary, &notes);
  PatternRef& current = st.brush.model.texture.pattern;
  if (!current.id.empty() && current.field == nullptr)
    current.field = st.patternLibrary.resolve(current.id);
}

void revealPatternFolder(const PatternLibrary& patterns) {
  // Created here, where a user has asked to put something in it, and not by a scan.
  std::error_code ec;
  std::filesystem::create_directories(patterns.userRoot(), ec);
  const std::string url = "file://" + patterns.userRoot();
  (void)SDL_OpenURL(url.c_str());
}

}  // namespace

PatternPickerAction drawPatternPicker(const char* id, PatternLibrary& patterns, GpuContext& gpu,
                                      const std::string& currentId) {
  PatternPickerAction action;
  ImGui::PushID(id);

  const std::vector<PatternEntry>& entries = patterns.entries();
  ImGui::TextDisabled("%zu paper%s", entries.size(), entries.size() == 1 ? "" : "s");
  ImGui::SameLine();
  if (ImGui::SmallButton("Rescan")) action.rescanRequested = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("Reveal")) action.revealRequested = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Opens %s.\nAny image you put in there becomes a paper.",
                      patterns.userRoot().c_str());

  const int count = 1 + static_cast<int>(entries.size());
  const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const DabPickerLayout layout = dabPickerLayoutFor(count, avail, kCellPref, kCellGap);

  const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImGui::InvisibleButton("grid", ImVec2(avail, std::max(1.0f, layout.totalHeight)));
  const bool gridHovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const int hovered =
      gridHovered ? dabPickerCellAt(layout, count, mouse.x - gridOrigin.x, mouse.y - gridOrigin.y)
                  : -1;
  if (hovered >= 0 && ImGui::IsItemClicked()) {
    action.selected = true;
    action.id = hovered == 0 ? std::string() : entries[hovered - 1].id;
  }

  // Only cells inside the scrolling region are drawn, so only they are decoded.
  const float visibleTop = ImGui::GetWindowPos().y;
  const float visibleBottom = visibleTop + ImGui::GetWindowSize().y;
  for (int i = 0; i < count; ++i) {
    float cx = 0.0f, cy = 0.0f;
    dabPickerCellOrigin(layout, i, cx, cy);
    const ImVec2 p0(gridOrigin.x + cx, gridOrigin.y + cy);
    const ImVec2 p1(p0.x + layout.cellSize, p0.y + layout.cellSize);
    if (p1.y < visibleTop || p0.y > visibleBottom) continue;

    const bool isCurrent = (i == 0) ? currentId.empty()
                                    : (!currentId.empty() && entries[i - 1].id == currentId);
    dl->AddRectFilled(p0, p1, atelierToken(kChromeDeep));
    if (i == 0) {
      // None: an empty cell struck through, so it cannot be read as a white paper.
      dl->AddLine(ImVec2(p0.x + 6.0f, p1.y - 6.0f), ImVec2(p1.x - 6.0f, p0.y + 6.0f),
                  atelierToken(kTextSecondary), 1.5f);
    } else if (const WGPUTextureView view = g_atlas.viewFor(gpu, patterns, i - 1)) {
      const DabAtlasSlot slot = dabAtlasSlotFor(i - 1, kThumbPx, kAtlasPx);
      const ImVec2 uv0(static_cast<float>(slot.x) / kAtlasPx, static_cast<float>(slot.y) / kAtlasPx);
      const ImVec2 uv1(static_cast<float>(slot.x + kThumbPx) / kAtlasPx,
                       static_cast<float>(slot.y + kThumbPx) / kAtlasPx);
      // White tint: the thumbnail is the paper's own grey, not a mask to colour.
      dl->AddImage(reinterpret_cast<ImTextureID>(view), p0, p1, uv0, uv1, IM_COL32_WHITE);
    } else {
      dl->AddRectFilled(p0, p1, atelierToken(kChromeMid));
    }
    if (isCurrent)
      dl->AddRect(p0, p1, atelierToken(kAccent), 0.0f, 0, kDividerThickness * 2.0f);
    else if (i == hovered)
      dl->AddRect(p0, p1, atelierToken(kTextPrimary), 0.0f, 0, kDividerThickness);
    else
      dl->AddRect(p0, p1, atelierToken(kDivider), 0.0f, 0, kDividerThickness);
  }

  if (hovered > 0) {
    const PatternEntry& e = entries[hovered - 1];
    if (e.width > 0)
      ImGui::SetTooltip("%s\n%d x %d", e.name.c_str(), e.width, e.height);
    else
      ImGui::SetTooltip("%s", e.name.c_str());
  } else if (hovered == 0) {
    ImGui::SetTooltip("No pattern: the brush uses its own Paper Grain.");
  }

  if (entries.empty()) {
    ImGui::TextDisabled("No papers yet.");
    ImGui::TextDisabled("Load a .abr that carries patterns, or drop an image in the folder.");
  }

  ImGui::PopID();
  return action;
}

void ensurePatternLibraryScanned(AppState& st) {
  const size_t presets = st.brush.brushLibrary.presets.size();
  if (st.patternLibraryScanned && presets == st.patternScanPresetCount) return;
  if (!st.patternLibraryScanned)
    st.patternLibrary.setRoots(patternsUserRootPath(), patternsImportedRootPath());
  st.patternLibraryScanned = true;
  st.patternScanPresetCount = presets;
  rescanPatterns(st);
}

bool drawBrushPatternPicker(AppState& st, GpuContext& gpu) {
  ensurePatternLibraryScanned(st);
  const PatternPickerAction action = drawPatternPicker(
      "texture-pattern", st.patternLibrary, gpu, st.brush.model.texture.pattern.id);
  if (action.rescanRequested) rescanPatterns(st);
  if (action.revealRequested) revealPatternFolder(st.patternLibrary);
  if (!g_scanNotes.empty()) {
    ImGui::TextDisabled("%zu file%s in the folders %s not a paper", g_scanNotes.size(),
                        g_scanNotes.size() == 1 ? "" : "s", g_scanNotes.size() == 1 ? "is" : "are");
    if (ImGui::IsItemHovered()) {
      std::string lines;
      for (size_t i = 0; i < g_scanNotes.size() && i < 12; ++i) lines += g_scanNotes[i] + "\n";
      ImGui::SetTooltip("%s", lines.c_str());
    }
  }
  return action.selected &&
         selectPattern(st.brush.model.texture, st.patternLibrary, action.id);
}

}  // namespace np
