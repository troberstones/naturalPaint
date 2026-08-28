#include "ui/DabPicker.hpp"

#include "app/DabLibrary.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"
#include "imgui.h"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace np {

DabPickerLayout dabPickerLayoutFor(int count, float availableWidth, float desiredCell,
                                   float spacing) noexcept {
  DabPickerLayout out;
  out.spacing = std::max(0.0f, spacing);
  const float cell = std::max(1.0f, desiredCell);
  const float width = std::max(1.0f, availableWidth);

  // n cells need `n * cell + (n - 1) * spacing`, so the largest n that fits is
  // `(width + spacing) / (cell + spacing)` floored. One column is always
  // allowed: a picker that vanishes when a pane is dragged narrow is worse
  // than one whose cells are too big for it.
  out.columns = static_cast<int>(std::floor((width + out.spacing) / (cell + out.spacing)));
  out.columns = std::max(1, out.columns);

  // Grown back to fill the row exactly, so there is no ragged strip of unused
  // width on the right. With one column this is just the panel width, which is
  // the correct answer for a very narrow pane and not a special case.
  out.cellSize = (width - out.spacing * static_cast<float>(out.columns - 1)) /
                 static_cast<float>(out.columns);
  out.cellSize = std::max(1.0f, out.cellSize);

  const int n = std::max(0, count);
  out.rows = (n + out.columns - 1) / out.columns;
  out.totalHeight = out.rows > 0 ? static_cast<float>(out.rows) * out.cellSize +
                                       static_cast<float>(out.rows - 1) * out.spacing
                                 : 0.0f;
  return out;
}

void dabPickerCellOrigin(const DabPickerLayout& layout, int index, float& xOut,
                         float& yOut) noexcept {
  const int columns = std::max(1, layout.columns);
  const int col = index % columns;
  const int row = index / columns;
  xOut = static_cast<float>(col) * (layout.cellSize + layout.spacing);
  yOut = static_cast<float>(row) * (layout.cellSize + layout.spacing);
}

int dabPickerCellAt(const DabPickerLayout& layout, int count, float x, float y) noexcept {
  if (count <= 0 || layout.cellSize <= 0.0f) return -1;
  if (x < 0.0f || y < 0.0f) return -1;
  const float stride = layout.cellSize + layout.spacing;
  const int col = static_cast<int>(std::floor(x / stride));
  const int row = static_cast<int>(std::floor(y / stride));
  if (col < 0 || col >= std::max(1, layout.columns) || row < 0) return -1;
  // **The gutters are not part of any cell.** A click in the space between two
  // thumbnails selects neither, rather than being absorbed by whichever
  // neighbour the division happened to round toward -- which is how a picker
  // ends up selecting the cell next to the one that was clicked.
  if (x - static_cast<float>(col) * stride > layout.cellSize) return -1;
  if (y - static_cast<float>(row) * stride > layout.cellSize) return -1;
  const int index = row * std::max(1, layout.columns) + col;
  return index < count ? index : -1;
}

int dabPickerFirstCellOfRow(const DabPickerLayout& layout, int row) noexcept {
  return std::max(0, row) * std::max(1, layout.columns);
}

std::vector<uint8_t> dabThumbnailRgba(const BrushTipBitmap& tip, int cell) {
  const int side = std::max(1, cell);
  // Fully transparent, so the margin around a non-square tip shows the panel
  // behind it rather than a white box the tip appears to sit on.
  std::vector<uint8_t> out(static_cast<size_t>(side) * static_cast<size_t>(side) * 4, 0);
  if (tip.width <= 0 || tip.height <= 0 ||
      tip.alpha.size() != static_cast<size_t>(tip.width) * static_cast<size_t>(tip.height))
    return out;

  // Aspect preserved: the longer side fills the cell, the shorter is centred.
  // A tip squashed to a square would make a flat chisel and a round nib look
  // like the same brush, which is the one thing the thumbnail exists to tell
  // apart.
  const float scale =
      static_cast<float>(side) / static_cast<float>(std::max(tip.width, tip.height));
  const int drawW = std::max(1, static_cast<int>(std::lround(tip.width * scale)));
  const int drawH = std::max(1, static_cast<int>(std::lround(tip.height * scale)));
  const int offX = (side - drawW) / 2;
  const int offY = (side - drawH) / 2;

  for (int y = 0; y < drawH; ++y) {
    // Nearest-neighbour, and the header says why: at 1/20 scale a box filter
    // turns every speckled tip into the same grey blob, and shape is the whole
    // job of a thumbnail.
    const int sy = std::min(tip.height - 1, y * tip.height / drawH);
    for (int x = 0; x < drawW; ++x) {
      const int sx = std::min(tip.width - 1, x * tip.width / drawW);
      const uint8_t coverage = tip.alpha[static_cast<size_t>(sy) * tip.width + sx];
      const size_t d = (static_cast<size_t>(offY + y) * side + (offX + x)) * 4;
      out[d] = out[d + 1] = out[d + 2] = 255;  // white, tinted by the vertex colour
      out[d + 3] = coverage;
    }
  }
  return out;
}

int dabAtlasSlotsPerPage(int cellPx, int atlasPx) noexcept {
  const int cell = std::max(1, cellPx);
  const int per = std::max(1, atlasPx / cell);
  return per * per;
}

DabAtlasSlot dabAtlasSlotFor(int index, int cellPx, int atlasPx) noexcept {
  DabAtlasSlot slot;
  const int cell = std::max(1, cellPx);
  const int perRow = std::max(1, atlasPx / cell);
  const int perPage = perRow * perRow;
  const int i = std::max(0, index);
  slot.page = i / perPage;
  const int within = i % perPage;
  slot.x = (within % perRow) * cell;
  slot.y = (within / perRow) * cell;
  return slot;
}

// ---------------------------------------------------------------------------
// The panel. Everything ABOVE this line is pure and tested; everything below
// is the part `--selftest` cannot reach (reachability-audit F4), kept thin on
// purpose -- it calls the functions above, draws what they say, and decides
// nothing of its own.
// ---------------------------------------------------------------------------
namespace {

constexpr int kAtlasPx = 1024;
constexpr int kThumbPx = 64;    // one atlas cell; the drawn cell may differ
constexpr float kCellPref = 54.0f;
constexpr float kCellGap = 4.0f;

// The atlas: shared 1024x1024 RGBA8 pages, so a folder of five hundred tips
// is two texture binds rather than five hundred (header §2).
//
// Never released, which is `DabPreviewTexture`'s own convention in
// ui/MacPaintUI.cpp and gfx/Wgpu.hpp's: a GPU object lives for the process,
// and ImGui's WebGPU backend rebuilds its bind group from the view pointer
// every frame, so a view created once and kept has no stale-bind-group
// hazard. That hazard comes from REPLACING a view, which this never does --
// a rescan rewrites page CONTENTS through `wgpuQueueWriteTexture`, not the
// pages themselves.
class DabAtlas {
 public:
  // Uploads whatever has changed and returns the view for `page`, or null if
  // there is no adapter. `library` is non-const because a cell being seen for
  // the first time decodes its file here -- the lazy cost app/DabLibrary's
  // index exists to defer, paid per visible row rather than per folder.
  WGPUTextureView viewFor(GpuContext& gpu, DabLibrary& library, int index) {
    const DabAtlasSlot slot = dabAtlasSlotFor(index, kThumbPx, kAtlasPx);
    if (slot.page < 0) return nullptr;
    while (static_cast<int>(pages_.size()) <= slot.page) {
      Page page;
      WGPUTextureDescriptor td = {};
      td.label = sv("dab picker atlas");
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

    const std::vector<DabEntry>& entries = library.entries();
    if (index >= static_cast<int>(entries.size())) return pages_[slot.page].view;

    // Keyed on the entry's id AND the library's generation, so a rescan that
    // reordered the folder cannot leave cell 7 showing cell 3's picture.
    const std::string key = entries[index].id;
    const uint64_t generation = library.generation();
    auto& uploaded = pages_[slot.page].uploaded;
    const auto found = uploaded.find(index);
    if (found != uploaded.end() && found->second.first == key &&
        found->second.second == generation)
      return pages_[slot.page].view;

    std::shared_ptr<const BrushTipBitmap> bitmap = entries[index].bitmap;
    if (bitmap == nullptr) bitmap = library.resolve(key);
    // A dab whose file has gone since the scan gets an empty cell rather than
    // a stale one -- `dabThumbnailRgba()` on a null tip is transparent, which
    // reads as "nothing here" and not as "the tip next door".
    const std::vector<uint8_t> rgba =
        dabThumbnailRgba(bitmap != nullptr ? *bitmap : BrushTipBitmap{}, kThumbPx);

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
    // index -> (id, generation) of what is actually in that slot.
    std::unordered_map<int, std::pair<std::string, uint64_t>> uploaded;
  };
  std::vector<Page> pages_;
};

// One atlas for every picker instance, not one each: the tip, the dual tip and
// the pattern pickers all draw the SAME folder, so three atlases would be
// three copies of one set of thumbnails on the GPU.
DabAtlas g_atlas;

}  // namespace

DabPickerAction drawDabPicker(const char* id, DabLibrary& dabs, GpuContext& gpu,
                              const std::string& currentId) {
  DabPickerAction action;
  ImGui::PushID(id);

  // --- Header row --------------------------------------------------------
  //
  // The count, RESCAN and Reveal. **A watched folder nobody can find is not a
  // feature**, which is what Reveal is for; and RESCAN exists because the
  // focus-event scan is a heuristic (app/DabLibrary §2) and a user who just
  // saved a file from another application should not have to guess whether it
  // fired.
  const std::vector<DabEntry>& entries = dabs.entries();
  ImGui::TextDisabled("%zu dab%s", entries.size(), entries.size() == 1 ? "" : "s");
  ImGui::SameLine();
  if (ImGui::SmallButton("Rescan")) action.rescanRequested = true;
  ImGui::SameLine();
  if (ImGui::SmallButton("Reveal")) action.revealRequested = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Opens %s.\nAnything you put in there becomes a dab -- there is no import step.",
                      dabs.userRoot().c_str());

  // --- The grid ----------------------------------------------------------
  //
  // Cell 0 is the PROCEDURAL tip, always, so "go back to the round tip" is
  // reachable from the same grid as everything else rather than from a
  // separate control the user has to know exists.
  const int count = 1 + static_cast<int>(entries.size());
  const float avail = std::max(1.0f, ImGui::GetContentRegionAvail().x);
  const DabPickerLayout layout = dabPickerLayoutFor(count, avail, kCellPref, kCellGap);

  const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // An invisible button over the whole grid, and ONE hit test through
  // `dabPickerCellAt()` -- rather than a button per cell. Per-cell buttons
  // would be a second, independent answer to "which cell is this point in",
  // and the two drifting apart is exactly the bug section C of the selftest
  // exists to make impossible.
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

  // Only the cells on screen are drawn, and -- because a thumbnail is uploaded
  // the first time it is seen -- only they are decoded. This is what makes a
  // folder of five hundred tips cost the same as a folder of twelve to open.
  // A rectangle test and not `ImGuiListClipper`: header §2 says why.
  const ImVec2 clipMin = ImGui::GetWindowPos();
  const float scrollTop = clipMin.y;
  const float scrollBottom = clipMin.y + ImGui::GetWindowSize().y;
  for (int i = 0; i < count; ++i) {
    float cx = 0.0f, cy = 0.0f;
    dabPickerCellOrigin(layout, i, cx, cy);
    const ImVec2 p0(gridOrigin.x + cx, gridOrigin.y + cy);
    const ImVec2 p1(p0.x + layout.cellSize, p0.y + layout.cellSize);
    if (p1.y < scrollTop || p0.y > scrollBottom) continue;

    const bool isCurrent = (i == 0) ? currentId.empty()
                                    : (!currentId.empty() && entries[i - 1].id == currentId);
    dl->AddRectFilled(p0, p1, atelierToken(kChromeDeep));
    if (i == 0) {
      // The procedural tip, drawn rather than thumbnailed: it has no bitmap to
      // make a thumbnail of, and a filled disc is what it actually paints.
      const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
      dl->AddCircleFilled(c, layout.cellSize * 0.32f, atelierToken(kTextPrimary));
    } else if (const WGPUTextureView view = g_atlas.viewFor(gpu, dabs, i - 1)) {
      const DabAtlasSlot slot = dabAtlasSlotFor(i - 1, kThumbPx, kAtlasPx);
      const ImVec2 uv0(static_cast<float>(slot.x) / kAtlasPx,
                       static_cast<float>(slot.y) / kAtlasPx);
      const ImVec2 uv1(static_cast<float>(slot.x + kThumbPx) / kAtlasPx,
                       static_cast<float>(slot.y + kThumbPx) / kAtlasPx);
      // `AddImage` and not `addCanvasQuad()`: coverage is an OPACITY, never
      // gamma-encoded, so this is not a linear-light texture and the
      // ui/CanvasQuad rule does not apply to it. See ui/DabPicker.hpp §2.
      dl->AddImage(reinterpret_cast<ImTextureID>(view), p0, p1, uv0, uv1,
                   atelierToken(kTextPrimary));
    } else {
      // No adapter, or a texture that could not be made. A filled rectangle
      // and not a gap, for drawTestStroke()'s reason: a gap reads as "this
      // dab is empty" when it means "this build could not show it".
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
    const DabEntry& e = entries[hovered - 1];
    ImGui::SetTooltip("%s\n%d x %d%s", e.name.c_str(), e.width, e.height,
                      e.haveSpacing ? "\nthis file suggests a spacing" : "");
  } else if (hovered == 0) {
    ImGui::SetTooltip("The built-in round tip -- no bitmap, shaped by Hardness and Roundness.");
  }

  if (entries.empty()) {
    ImGui::TextDisabled("Nothing in the dab folder yet.");
    ImGui::TextDisabled("Drop a PNG, a .gbr or a .gih into it, or import a .abr.");
  }

  // --- What selecting deliberately does NOT do ---------------------------
  //
  // Header §3: a picker that silently resized the brush would make "try the
  // other tip" an edit to undo twice. Both offers are buttons, and both are
  // disabled with the reason showing when there is nothing to apply -- the
  // "disabled rather than hidden" idiom this application uses everywhere.
  const DabEntry* current = currentId.empty() ? nullptr : dabs.find(currentId);
  ImGui::BeginDisabled(current == nullptr);
  if (ImGui::SmallButton("Use native size") && current != nullptr) {
    action.useNativeSize = true;
    action.nativeWidth = current->width;
    action.nativeHeight = current->height;
  }
  ImGui::EndDisabled();
  if (current == nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("The round tip has no size of its own -- Radius is its size.");

  ImGui::SameLine();
  const bool canSpace = current != nullptr && current->haveSpacing;
  ImGui::BeginDisabled(!canSpace);
  if (ImGui::SmallButton("Use its spacing") && canSpace) {
    action.useFileSpacing = true;
    // The file states a percentage of the brush WIDTH; this application's
    // Spacing is in RADII. Half, therefore, and converted here rather than at
    // the call site so the one place that knows both units does it.
    action.fileSpacing = current->spacingPercent / 100.0f * 0.5f;
  }
  ImGui::EndDisabled();
  if (!canSpace && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Only a GIMP .gbr or .gih carries a spacing. This one does not.");

  ImGui::PopID();
  return action;
}

}  // namespace np
