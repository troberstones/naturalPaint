#include "ui/DocumentGallery.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/OpenAnyFile.hpp"
#include "color/Space.hpp"
#include "gfx/Context.hpp"
#include "io/NpaintFile.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Dialog.hpp"

namespace fs = std::filesystem;

namespace np {
namespace {

// --- thumbnail building: app/LayerThumbnail's transfer-function rule, ------
// --- reapplied to a document composite instead of a layer's tiles ---------

// A float in [0,1] to a byte, rounded rather than truncated -- the identical
// helper app/LayerThumbnail.cpp keeps for the identical reason: truncation
// puts 1.0 on 254 for anything short of exactly 255.0f.
uint8_t toByte(float v) noexcept {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<uint8_t>(std::lround(v * 255.0f));
}

// The letterbox: the largest `srcW x srcH`-shaped rect that fits in `cell`.
// Same shape as app/LayerThumbnail.cpp's `fitRect()` (not reused directly --
// that one is file-local to a different translation unit).
void fitRect(int32_t srcW, int32_t srcH, int cell, int& xOut, int& yOut, int& wOut, int& hOut) {
  if (srcW <= 0 || srcH <= 0) {
    xOut = yOut = wOut = hOut = 0;
    return;
  }
  const double scale = std::min(static_cast<double>(cell) / static_cast<double>(srcW),
                                static_cast<double>(cell) / static_cast<double>(srcH));
  wOut = std::max(1, std::min(cell, static_cast<int>(std::lround(srcW * scale))));
  hOut = std::max(1, std::min(cell, static_cast<int>(std::lround(srcH * scale))));
  xOut = (cell - wOut) / 2;
  yOut = (cell - hOut) / 2;
}

// Samples per output texel, per axis. 3 rather than app/LayerThumbnail's 4:
// a gallery tile is almost seven times the area (160^2 against 24^2), so the
// same factor would cost 46x the samples for a picture nobody scrutinises at
// pixel level -- 3x3 keeps one thumbnail under 90 000 samples, still well
// past what a 160 px cell can resolve.
constexpr int kThumbSupersample = 3;

int32_t sampleAt(int32_t lo, int32_t hi, int s) noexcept {
  const int32_t span = hi - lo;
  if (span <= 0) return lo;
  const int32_t off = static_cast<int32_t>((static_cast<int64_t>(2 * s + 1) * span) /
                                           (2 * kThumbSupersample));
  return lo + std::min(off, span - 1);
}

// `composite` is `io/ImageDecode.hpp`'s DecodedImage contract: linear-light
// float RGBA, straight alpha. Builds the letterboxed, sRGB-encoded thumbnail
// exactly as app/LayerThumbnail::layerContentThumbnail() builds a layer's --
// premultiplied average, un-premultiplied once, colour channels encoded,
// alpha (a coverage) left alone.
GalleryThumbnail buildThumbnail(const DecodedImage& composite) {
  GalleryThumbnail out;
  out.rgba.assign(static_cast<size_t>(kGalleryThumbPx) * kGalleryThumbPx * 4, 0);
  if (!composite.valid()) return out;

  fitRect(static_cast<int32_t>(composite.width), static_cast<int32_t>(composite.height),
          kGalleryThumbPx, out.x, out.y, out.w, out.h);
  if (out.w == 0 || out.h == 0) return out;

  const int32_t srcW = static_cast<int32_t>(composite.width);
  const int32_t srcH = static_cast<int32_t>(composite.height);

  for (int oy = 0; oy < out.h; ++oy) {
    const int32_t sy0 = static_cast<int32_t>((static_cast<int64_t>(oy) * srcH) / out.h);
    const int32_t sy1 = static_cast<int32_t>((static_cast<int64_t>(oy + 1) * srcH) / out.h);
    for (int ox = 0; ox < out.w; ++ox) {
      const int32_t sx0 = static_cast<int32_t>((static_cast<int64_t>(ox) * srcW) / out.w);
      const int32_t sx1 = static_cast<int32_t>((static_cast<int64_t>(ox + 1) * srcW) / out.w);

      // Averaged PREMULTIPLIED (the linear operation), un-premultiplied once
      // at the end -- app/LayerThumbnail.cpp section 2's argument, verbatim.
      std::array<double, 4> acc{0.0, 0.0, 0.0, 0.0};
      int taken = 0;
      for (int sj = 0; sj < kThumbSupersample; ++sj) {
        const int32_t y = sampleAt(sy0, sy1, sj);
        for (int si = 0; si < kThumbSupersample; ++si) {
          const int32_t x = sampleAt(sx0, sx1, si);
          const size_t i = (static_cast<size_t>(y) * srcW + static_cast<size_t>(x)) * 4;
          const float a = composite.pixels[i + 3];
          acc[0] += composite.pixels[i + 0] * a;
          acc[1] += composite.pixels[i + 1] * a;
          acc[2] += composite.pixels[i + 2] * a;
          acc[3] += a;
          ++taken;
        }
      }
      if (taken == 0) continue;
      const double inv = 1.0 / static_cast<double>(taken);
      const float a = static_cast<float>(acc[3] * inv);
      const size_t o = (static_cast<size_t>(out.y + oy) * kGalleryThumbPx +
                        static_cast<size_t>(out.x + ox)) *
                       4;
      if (!(a > 0.0f)) continue;  // stays transparent black
      for (int c = 0; c < 3; ++c) {
        const float straight = static_cast<float>(acc[c] * inv) / a;
        out.rgba[o + static_cast<size_t>(c)] = toByte(srgbEncode(straight));
      }
      out.rgba[o + 3] = toByte(a);  // coverage: never gamma-encoded
    }
  }
  return out;
}

std::string formatModifiedLabel(std::time_t t) {
  char buf[16] = {};
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
  return std::string(buf);
}

// --- the atlas: one page, sized to fit exactly this scan's entry count ----
//
// Unlike ui/MacPaintUI.cpp's `LayerThumbAtlas` (a fixed 256 px page reused
// forever, because a layer row's cell is reassigned constantly as the panel
// scrolls and reorders), the gallery scans once per visit and every tile is
// visible at once -- there is no scrolling window smaller than the entry
// count to amortise. So this page is sized to the scan exactly (a square
// grid of `kGalleryThumbPx` cells) and rebuilt, released and replaced each
// time `scanDocumentGallery()` runs, rather than kept at one fixed size and
// keyed per slot.
class GalleryAtlas {
 public:
  ~GalleryAtlas() { release(); }

  void rebuild(GpuContext& gpu, const std::vector<GalleryEntry>& entries) {
    release();
    if (entries.empty()) return;
    cols_ = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(entries.size()))));
    cols_ = std::max(cols_, 1);
    const int pagePx = cols_ * kGalleryThumbPx;

    WGPUTextureDescriptor td = {};
    td.label = sv("document gallery atlas");
    td.dimension = WGPUTextureDimension_2D;
    td.size = {static_cast<uint32_t>(pagePx), static_cast<uint32_t>(pagePx), 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
    if (texture_ == nullptr) return;
    view_ = wgpuTextureCreateView(texture_, nullptr);

    for (size_t i = 0; i < entries.size(); ++i) {
      const GalleryThumbnail& thumb = entries[i].thumb;
      if (thumb.rgba.empty()) continue;
      WGPUTexelCopyTextureInfo dst = {};
      dst.texture = texture_;
      dst.mipLevel = 0;
      dst.origin = {static_cast<uint32_t>((static_cast<int>(i) % cols_) * kGalleryThumbPx),
                    static_cast<uint32_t>((static_cast<int>(i) / cols_) * kGalleryThumbPx), 0};
      dst.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout = {};
      layout.bytesPerRow = static_cast<uint32_t>(kGalleryThumbPx) * 4u;
      layout.rowsPerImage = static_cast<uint32_t>(kGalleryThumbPx);
      const WGPUExtent3D extent = {static_cast<uint32_t>(kGalleryThumbPx),
                                   static_cast<uint32_t>(kGalleryThumbPx), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, thumb.rgba.data(), thumb.rgba.size(), &layout,
                            &extent);
    }
  }

  WGPUTextureView view() const noexcept { return view_; }

  void uvFor(size_t index, ImVec2& uv0, ImVec2& uv1) const noexcept {
    const float n = static_cast<float>(cols_ * kGalleryThumbPx);
    const float x = static_cast<float>((static_cast<int>(index) % cols_) * kGalleryThumbPx);
    const float y = static_cast<float>((static_cast<int>(index) / cols_) * kGalleryThumbPx);
    uv0 = ImVec2(x / n, y / n);
    uv1 = ImVec2((x + kGalleryThumbPx) / n, (y + kGalleryThumbPx) / n);
  }

  // **`wgpuTextureRelease`, never `wgpuTextureDestroy`** -- ui/DocumentTexture
  // .cpp's `release()` measured the crash the latter causes against a write
  // still staged on the queue's pending encoder; this atlas rebuilds on every
  // visit to the gallery and therefore hits exactly that hazard if it ever
  // used the immediate call instead.
  void release() {
    if (view_) wgpuTextureViewRelease(view_);
    if (texture_) wgpuTextureRelease(texture_);
    view_ = nullptr;
    texture_ = nullptr;
    cols_ = 1;
  }

 private:
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int cols_ = 1;
};

GalleryAtlas g_atlas;
std::vector<GalleryEntry> g_entries;
bool g_scanned = false;
// Set after Delete erases an entry from `g_entries` in place (no rescan --
// see that call site's own comment). Checked and cleared at the TOP of the
// next `drawDocumentGallery()` call, same timing `g_scanned` already uses
// and for the same reason: `GalleryAtlas::rebuild()` releases the CURRENT
// WGPU texture view before creating the next one, and this frame's tile
// loop has already recorded `AddImage()` calls against that view by the
// time Delete's dialog commits (the loop runs first; the dialog is drawn
// after it). Rebuilding right there destroyed a view this frame's own
// already-recorded draw commands still pointed at -- reproduced on device
// as `wgpu_core::storage::Storage::get` aborting with "TextureView is no
// longer alive". Deferring to next frame's top, before any AddImage() call
// exists yet, is what already makes the full-rescan path safe.
bool g_atlasNeedsRebuild = false;
// Empty means `iosDocumentsDirectory()` -- see setDocumentGalleryDirectory().
std::string g_dirOverride;

// Forces the next `drawDocumentGallery()` call to rescan and rebuild the
// atlas -- called after either tap below hands control back to the canvas,
// so returning to the gallery later (there is no "back" button yet beyond
// what a future step adds) always shows what is really on disk rather than
// a snapshot from whenever the gallery last opened.
// **Does NOT release the atlas texture.** This is called from inside
// `drawDocumentGallery()`, after the grid has already been drawn this frame
// -- so `ImGui::AddImage()` has already recorded the atlas view's pointer
// into the current draw list, and `ImGui::Render()` (and the WebGPU
// backend's own bind-group rebuild from that pointer, `LayerThumbAtlas`'s
// own comment on the identical convention) has not run yet. Releasing the
// WGPU view synchronously here -- as a first version of this function did --
// frees the resource before that render happens, and the backend then
// builds a bind group from an already-dead handle: a `wgpu_core::storage::
// Storage::get` panic (`abort()`, not a caught error), reproduced by tapping
// a gallery thumbnail. `GalleryAtlas::rebuild()` already calls `release()` on
// itself before creating the new page, so simply not releasing here leaves
// the old page alive (and correctly unreferenced by any *new* draw call)
// until the gallery is shown again in some later frame, by which point this
// frame's draw data has long since been submitted and presented.
void invalidateGalleryScan() {
  g_scanned = false;
  g_entries.clear();
}

// Grid geometry. `kTileW`/`kTileH` size the whole tile including its
// caption; the thumbnail itself is drawn at `kGalleryThumbPx` inside it, top
// aligned, exactly as app/LayerThumbnail's cell sits inside a taller row.
constexpr float kTileGap = kWindowPaddingX * 4.0f;  // 32 -- roomier than a bare 2x on a touch grid
constexpr float kTileCaptionH = 40.0f;              // two text lines' worth
constexpr float kTileW = static_cast<float>(kGalleryThumbPx);
constexpr float kTileH = static_cast<float>(kGalleryThumbPx) + kTileCaptionH;

// The gap between the scrolling child's content origin and the first tile,
// on both axes. A tile's frame is stroked OUTSIDE its thumbnail rect (at
// `-1`, 1 px wide, 2 px while hovered, both centred on the line) -- and a
// borderless `BeginChild()` is given zero window padding by ImGui, so its
// content origin IS its clip-rect corner. At an origin of exactly zero the
// top row and the left column therefore drew their outer edge outside the
// clip and lost it: measured as a jump straight from chrome (45) to the
// paper's own antialiased edge (187) with no hairline (155) in between,
// against four transition rows on an edge that is not clipped. 4 px was the
// bare minimum that fixed the clip; kept well past it here so the grid also
// reads as having a real margin against the window's own edge rather than
// sitting flush against it.
constexpr float kGridInset = 24.0f;

// What one tile reported this frame. A long press is a menu, not a tap, so
// the two are separate answers rather than one `bool`.
struct TileInput {
  bool clicked = false;
  bool longPressed = false;
};

// The one press that has already fired its long-press menu. Held so the
// finger lifting off afterwards does not ALSO read as a tap and open the
// document behind the menu. Cleared at the end of the frame in which the
// button comes up.
ImGuiID g_longPressFired = 0;

// Draws one caption line at its normal size UNLESS it is wider than the
// tile -- a document's name is arbitrary length, the tile is a fixed
// `kTileW`, and at normal size a long name overran into the NEXT tile's
// own thumbnail and caption rather than wrapping or stopping at the edge.
// Left at its normal size (the common case: most names fit) rather than
// shrinking everything uniformly, so a short name still reads at full
// size. `kMinScale` is a readability floor, not what stops the overlap --
// the clip rect is what actually guarantees a name can never bleed past
// `maxWidth`, however long it is.
void drawFitText(ImDrawList* dl, const ImVec2& at, ImU32 color, const char* text, float maxWidth) {
  ImFont* font = ImGui::GetFont();
  const float baseSize = ImGui::GetFontSize();
  const ImVec2 full = font->CalcTextSizeA(baseSize, FLT_MAX, 0.0f, text);
  constexpr float kMinScale = 0.62f;
  const float size = full.x > maxWidth ? baseSize * std::max(kMinScale, maxWidth / full.x) : baseSize;
  dl->PushClipRect(at, ImVec2(at.x + maxWidth, at.y + size + 2.0f), true);
  dl->AddText(font, size, at, color, text);
  dl->PopClipRect();
}

// One "+" or thumbnail tile: the clickable area, the paper ground, the frame,
// and the label underneath. `paper` fills the thumbnail square with the
// theme's canvas white before anything is drawn into it -- a document's
// composite carries its own transparency, and against the dark chrome an
// unpainted or partly-painted document read as a hole rather than as a sheet.
TileInput drawTile(ImDrawList* dl, const ImVec2& at, int stableId, const char* line1,
                   const char* line2) {
  ImGui::SetCursorScreenPos(at);
  // Stable across frames by ENTRY, not by screen position -- touch scrolling
  // (below) moves a tile's `at` every frame a drag is live, and an id derived
  // from position would then change out from under a press already in
  // progress, silently dropping its held/active state mid-gesture (breaking
  // both the long-press timer and drag-to-scroll itself, which also needs a
  // press to survive its own tile moving). `-1` for the one tile with no
  // entry index (the "+" tile), never collides with a `size_t` cast to `int`.
  ImGui::PushID(stableId);
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH + 6.0f));
  const ImGuiID id = ImGui::GetItemID();
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  ImGui::PopID();

  TileInput in;
  if (held && g_longPressFired != id &&
      ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left] >= kGalleryLongPressSeconds &&
      !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    g_longPressFired = id;
    in.longPressed = true;
  }
  // Not a tap if this press ended up dragging: touch scrolling (below) is
  // driven by the same press-and-move gesture, and without this guard every
  // scroll's release would also open whatever tile happened to be under the
  // finger when it lifted.
  in.clicked = clicked && g_longPressFired != id && !ImGui::IsMouseDragging(ImGuiMouseButton_Left);

  const ImVec2 thumbLo = at;
  const ImVec2 thumbHi(at.x + kTileW, at.y + kTileW);
  dl->AddRectFilled(thumbLo, thumbHi, atelierToken(kCanvasPaper), 4.0f);
  dl->AddRect(ImVec2(thumbLo.x - 1.0f, thumbLo.y - 1.0f), ImVec2(thumbHi.x + 1.0f, thumbHi.y + 1.0f),
              hovered ? atelierToken(kAccent) : atelierToken(kHairline), 4.0f, 0, hovered ? 2.0f : 1.0f);

  if (line1 != nullptr) {
    const ImVec2 capAt(at.x, at.y + kTileW + 6.0f);
    drawFitText(dl, capAt, atelierToken(kTextPrimary), line1, kTileW);
    if (line2 != nullptr)
      drawFitText(dl, ImVec2(capAt.x, capAt.y + ImGui::GetFontSize() + 2.0f),
                  atelierToken(kTextSecondary), line2, kTileW);
  }
  return in;
}

// --- long-press menu and its two dialogs, all keyed by PATH ---------------
//
// Path, not index: every one of these actions ends in `invalidateGalleryScan()`,
// after which `g_entries` is empty and will come back re-sorted by mtime, so an
// index captured when the menu opened would name a different document (or none)
// by the time the dialog it opened commits.
const char* const kTileMenuPopup = "##galleryTileMenu";
const char* const kRenamePopup = "Rename Document";
const char* const kDeletePopup = "Delete Document";

std::string g_menuPath;      // the document the menu/dialogs act on
std::string g_menuName;      // its display name, for the dialogs' prose
char g_renameBuf[192] = {};
// A refusal from the last commit attempt, left on the dialog that refused.
std::string g_renameStatus;
std::string g_deleteStatus;
// One-shot latch for `--gallery-menu-demo` (setDocumentGalleryDemoStage()).
GalleryDemoStage g_demoStage = GalleryDemoStage::None;
bool g_demoFired = false;
// A one-line result under the "Documents" heading ("Deleted Sketch"), since
// the tile it refers to is gone by the time it is shown.
std::string g_galleryStatus;

}  // namespace

namespace {

// Surrounding blanks are the user's slip, not their intent: a name is trimmed
// before it is judged AND before it is written, so the two agree.
std::string trimName(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// The longest component most filesystems (APFS, HFS+, ext4) accept is 255
// BYTES, extension included -- so the name itself is capped at 255 less the
// seven `.npaint` costs.
constexpr size_t kMaxNameBytes = 255 - 7;

}  // namespace

std::string galleryRenameRefusal(const std::string& newName) {
  const std::string name = trimName(newName);
  if (name.empty()) return "A document needs a name.";
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
    return "A name cannot contain a slash.";
  // `.` and `..` are the directory itself and its parent; a leading dot hides
  // the file from the Files app, which is the other half of this same folder.
  if (name == "." || name == "..") return "That name is a directory, not a document.";
  if (name.front() == '.') return "A name cannot begin with a dot.";
  if (name.size() > kMaxNameBytes) return "That name is too long.";
  return {};
}

std::string galleryRenamedPath(const std::string& path, const std::string& newName) {
  std::string name = trimName(newName);
  if (name.size() >= 7 && name.compare(name.size() - 7, 7, ".npaint") == 0)
    name.erase(name.size() - 7);
  return (fs::path(path).parent_path() / (name + ".npaint")).string();
}

std::string galleryDuplicatePath(const std::string& path) {
  const fs::path src(path);
  const fs::path dir = src.parent_path();
  const std::string stem = src.stem().string();
  std::error_code ec;
  for (int n = 1; n <= 999; ++n) {
    std::string candidate = stem + " copy";
    if (n > 1) candidate += " " + std::to_string(n);
    const fs::path p = dir / (candidate + ".npaint");
    if (!fs::exists(p, ec)) return p.string();
  }
  return {};
}

std::vector<GalleryEntry> scanDocumentGallery(const std::string& dir) {
  std::vector<GalleryEntry> out;
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return out;  // no Documents dir yet (fresh install) -- an empty
                        // gallery is the correct picture, not a failure.

  for (const fs::directory_entry& entry : it) {
    if (!entry.is_regular_file(ec) || ec) continue;
    if (entry.path().extension().string() != ".npaint") continue;

    GalleryEntry e;
    e.path = entry.path().string();
    e.displayName = entry.path().stem().string();
    const auto ftime = entry.last_write_time(ec);
    if (!ec) {
      // `fs::file_time_type` is `file_clock`, not `system_clock`, and this
      // build's libc++ is not assumed to have C++20's `clock_cast`/`to_sys`
      // (the iOS toolchain here is the vcpkg/OpenColorIO chain
      // ios/vcpkg-overlay-ports pins, not this machine's own). The
      // now-now-delta trick below is the portable pre-clock_cast idiom: both
      // clocks tick at the same rate, so subtracting "now" on one and adding
      // "now" on the other cancels everything but the epoch offset. A
      // mtime label only needs day-granularity, so the sub-second slop this
      // introduces is irrelevant.
      const auto sysNow = std::chrono::system_clock::now();
      const auto fileNow = fs::file_time_type::clock::now();
      const auto sysTime =
          std::chrono::time_point_cast<std::chrono::system_clock::duration>(sysNow + (ftime - fileNow));
      e.mtimeEpoch =
          std::chrono::duration_cast<std::chrono::seconds>(sysTime.time_since_epoch()).count();
      e.modifiedLabel = formatModifiedLabel(std::chrono::system_clock::to_time_t(sysTime));
    }

    // The fast path (docs/ios-spike-plan.md, this feature's whole reason for
    // existing): read part 0 only, never reconstruct the document. See
    // io/NpaintFile.hpp's own comment on `loadNpaintPreviewOnly()` for the
    // cost this avoids relative to `loadNpaint()`.
    const NpaintPreviewResult preview = loadNpaintPreviewOnly(e.path);
    // A file that fails to open or decode still gets an entry (empty
    // thumbnail, drawn as a placeholder by the caller) -- see this header's
    // own comment on why a file must never vanish from its own gallery.
    if (preview.ok) e.thumb = buildThumbnail(preview.composite);

    out.push_back(std::move(e));
  }

  std::sort(out.begin(), out.end(),
            [](const GalleryEntry& a, const GalleryEntry& b) { return a.mtimeEpoch > b.mtimeEpoch; });
  return out;
}

void setDocumentGalleryDirectory(const std::string& dir) {
  g_dirOverride = dir;
  invalidateGalleryScan();
}

void setDocumentGalleryDemoStage(GalleryDemoStage stage) { g_demoStage = stage; }

void drawDocumentGallery(AppState& st, GpuContext& gpu) {
  if (!g_scanned) {
    g_entries = scanDocumentGallery(g_dirOverride.empty() ? iosDocumentsDirectory() : g_dirOverride);
    g_atlas.rebuild(gpu, g_entries);
    g_scanned = true;
    g_atlasNeedsRebuild = false;  // the rescan above already rebuilt it
  } else if (g_atlasNeedsRebuild) {
    g_atlas.rebuild(gpu, g_entries);
    g_atlasNeedsRebuild = false;
  }

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kWindowPaddingX * 2.0f, kWindowPaddingY * 2.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(atelierToken(kChromeBase)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("##DocumentGallery", nullptr, flags);

  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kTextPrimary)));
  ImGui::TextUnformatted("Documents");
  ImGui::PopStyleColor();
  if (!g_galleryStatus.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)), "   %s",
                       g_galleryStatus.c_str());
  }
  ImGui::Dummy(ImVec2(1.0f, 12.0f));

  // A tap must not act while the menu or a dialog is up: a non-modal
  // `BeginPopup()` does not block the window under it, so the click that
  // dismisses the menu would otherwise land on whatever tile it fell over.
  // Read BEFORE the tiles are drawn, since dismissing happens inside them.
  const bool popupWasOpen = ImGui::IsPopupOpen(kTileMenuPopup) ||
                            ImGui::IsPopupOpen(kRenamePopup) || ImGui::IsPopupOpen(kDeletePopup);

  // `NoScrollbar`: a touch gallery scrolls by dragging the content directly
  // (below), the way every other iOS scroll view does -- a desktop-style
  // scrollbar track is neither how a finger expects to scroll nor, at this
  // grid's tile size, wide enough to be a reliable touch target of its own.
  ImGui::BeginChild("##galleryGrid", ImVec2(0, 0), false,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Touch (or mouse) drag-to-scroll. `g_scrollDragArmed` latches at the
  // press that starts inside this child so a drag begun elsewhere (e.g. a
  // dialog opened over the gallery) never hijacks this scroll, and unlatches
  // the moment the button comes up -- the same shape as `g_longPressFired`
  // just above. Applied every frame the drag has crossed ImGui's own click
  // threshold (`IsMouseDragging`), which is also what `drawTile()` checks
  // before honouring a tap or a long press, so the two never fight over the
  // same gesture.
  // `scrollVelocity` carries the fling: a blended estimate of the drag's own
  // speed (not its raw last-frame delta, which touch delivery makes twitchy)
  // that keeps scrolling on its own after release and decays toward zero.
  // `galleryOverscroll` is the rubber band: ImGui clamps `ScrollY` itself to
  // [0, ScrollMaxY] every frame, so a drag or a fling that reaches an edge
  // has nowhere further to push `ScrollY` -- this is a SEPARATE render-time
  // offset (applied below, where `origin` is built) that keeps moving past
  // the edge, resisted, then springs back to zero once nothing is pulling on
  // it any more. That spring is what a plain clamped drag was missing.
  static bool scrollDragArmed = false;
  static float scrollVelocity = 0.0f;
  static float galleryOverscroll = 0.0f;
  {
    const float dt = ImGui::GetIO().DeltaTime;
    const float maxY = ImGui::GetScrollMaxY();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      scrollDragArmed = true;
      scrollVelocity = 0.0f;
    }
    if (scrollDragArmed && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
      const float deltaScroll = -ImGui::GetIO().MouseDelta.y;
      const float scrollY = ImGui::GetScrollY();
      // Resisted (0.4x): a real finger movement still stretches the band,
      // just less than it scrolls in-bounds -- an unresisted 1:1 pull would
      // not read as a band at all, just unclamped scrolling.
      constexpr float kRubberBandResistance = 0.4f;
      if ((scrollY <= 0.0f && deltaScroll < 0.0f) || (scrollY >= maxY && deltaScroll > 0.0f))
        galleryOverscroll += deltaScroll * kRubberBandResistance;
      else
        ImGui::SetScrollY(scrollY + deltaScroll);
      if (dt > 0.0f) scrollVelocity = scrollVelocity * 0.7f + (deltaScroll / dt) * 0.3f;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) scrollDragArmed = false;

    // Coasting: only once the finger is off the glass, decaying ~95% per
    // second (iOS's own general feel, not its exact curve) and stopping
    // outright once it is imperceptible or the content has run out to
    // scroll in that direction.
    if (!scrollDragArmed && scrollVelocity != 0.0f) {
      const float scrollY = ImGui::GetScrollY();
      if ((scrollY <= 0.0f && scrollVelocity < 0.0f) || (scrollY >= maxY && scrollVelocity > 0.0f)) {
        // A fling that arrives at the edge still under speed presses into
        // the band too, rather than just stopping dead against the clamp.
        galleryOverscroll += scrollVelocity * dt * 0.4f;
        scrollVelocity = 0.0f;
      } else {
        ImGui::SetScrollY(scrollY + scrollVelocity * dt);
      }
      scrollVelocity *= std::pow(0.05f, dt);
      if (std::fabs(scrollVelocity) < 4.0f) scrollVelocity = 0.0f;
    }

    // The spring: snaps whatever is left back to zero on its own, whether
    // that came from a drag still in progress or from a fling that pressed
    // into the band. Fast (~98% gone in a quarter second) so it reads as a
    // bounce, not a second, slower drift on top of the fling's own.
    if (!(scrollDragArmed && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) && galleryOverscroll != 0.0f) {
      galleryOverscroll *= std::pow(0.02f, dt / 0.25f);
      if (std::fabs(galleryOverscroll) < 0.5f) galleryOverscroll = 0.0f;
    }
  }

  // `kGridInset` reserved on BOTH edges (not just the left) so the grid gets
  // a real margin all the way around, and `extraMargin` -- half of whatever
  // width is left over once `columns` worth of tiles are laid out -- centres
  // the grid in what remains rather than leaving that leftover as dead space
  // against the right edge only.
  const float totalW = ImGui::GetContentRegionAvail().x;
  const float usableW = totalW - 2.0f * kGridInset;
  const int columns = std::max(1, static_cast<int>((usableW + kTileGap) / (kTileW + kTileGap)));
  const float usedW =
      static_cast<float>(columns) * kTileW + static_cast<float>(std::max(0, columns - 1)) * kTileGap;
  const float extraMargin = std::max(0.0f, (usableW - usedW) * 0.5f);

  const ImVec2 start = ImGui::GetCursorScreenPos();
  // `- galleryOverscroll`: same direction a `ScrollY` increase already moves
  // content (up), so a rubber-band pull past the bottom (positive) keeps
  // pushing tiles up past where `ScrollY`'s own clamp stopped, and a pull
  // past the top (negative) pushes them back down, opening a gap above the
  // first row -- the actual visible half of the spring above.
  const ImVec2 origin(start.x + kGridInset + extraMargin, start.y + kGridInset - galleryOverscroll);
  int col = 0, row = 0;
  int tappedIndex = -1;   // index into g_entries, or -1
  int pressedIndex = -1;  // the entry a long press opened the menu for, or -1
  bool tappedNew = false;

  auto cellPos = [&](int c, int r) {
    return ImVec2(origin.x + c * (kTileW + kTileGap), origin.y + r * (kTileH + kTileGap));
  };

  // The "+" tile first -- "make something new" reads left-to-right as the
  // first choice, and it is the one tile that is always present, even in an
  // empty gallery (this function's own doc comment on a fresh install).
  if (drawTile(dl, cellPos(col, row), -1, "New Document", nullptr).clicked) tappedNew = true;
  {
    // The plus glyph itself, centred in the tile drawn above. Chrome-mid, not
    // the secondary text grey: the tile's ground is now paper, and a #9b9797
    // glyph on #f8f4f4 is barely a glyph.
    const ImVec2 c = cellPos(col, row);
    const ImVec2 mid(c.x + kTileW * 0.5f, c.y + kTileW * 0.5f);
    const float arm = kTileW * 0.18f;
    const ImU32 col32 = atelierToken(kChromeMid);
    dl->AddLine(ImVec2(mid.x - arm, mid.y), ImVec2(mid.x + arm, mid.y), col32, 2.0f);
    dl->AddLine(ImVec2(mid.x, mid.y - arm), ImVec2(mid.x, mid.y + arm), col32, 2.0f);
  }
  ++col;
  if (col >= columns) { col = 0; ++row; }

  for (size_t i = 0; i < g_entries.size(); ++i) {
    const GalleryEntry& e = g_entries[i];
    const ImVec2 p = cellPos(col, row);
    const char* caption2 = e.modifiedLabel.empty() ? nullptr : e.modifiedLabel.c_str();
    const TileInput in = drawTile(dl, p, static_cast<int>(i), e.displayName.c_str(), caption2);
    if (in.clicked) tappedIndex = static_cast<int>(i);
    if (in.longPressed) pressedIndex = static_cast<int>(i);

    if (!e.thumb.rgba.empty() && g_atlas.view() != nullptr) {
      ImVec2 uv0, uv1;
      g_atlas.uvFor(i, uv0, uv1);
      // Letterboxed within the cell, same rect app/LayerThumbnail's own
      // consumer draws into -- the UV rect covers the WHOLE cell (including
      // its transparent margin), so one AddImage() call places both the
      // picture and its correct empty border in one draw.
      dl->AddImage(reinterpret_cast<ImTextureID>(g_atlas.view()), p,
                   ImVec2(p.x + kTileW, p.y + kTileW), uv0, uv1);
    } else {
      // A file that exists but whose composite could not be read or decoded
      // (this header's own "must never vanish" rule) -- an explicit mark
      // rather than a blank square, which would look identical to "no
      // picture yet" on a document that has simply never been painted on.
      dl->AddText(ImVec2(p.x + 8.0f, p.y + kTileW * 0.5f - 8.0f), atelierToken(kWarning),
                  "no preview");
    }

    ++col;
    if (col >= columns) { col = 0; ++row; }
  }

  ImGui::Dummy(ImVec2(1.0f, kGridInset + (row + 1) * (kTileH + kTileGap)));
  ImGui::EndChild();

  // --- the long-press menu and its two dialogs -------------------------
  //
  // Opened and drawn here rather than inside the child: a popup belongs to
  // the ID stack level that opened it, and the two dialogs below outlive the
  // rescan that closes the child's own contents.
  // `--gallery-menu-demo`: see setDocumentGalleryDemoStage(). Acted on once.
  bool demoRename = false;
  bool demoDelete = false;
  if (g_demoStage != GalleryDemoStage::None && !g_demoFired && !g_entries.empty()) {
    g_demoFired = true;
    pressedIndex = 0;
    demoRename = g_demoStage == GalleryDemoStage::Rename;
    demoDelete = g_demoStage == GalleryDemoStage::Delete;
  }
  if (pressedIndex >= 0) {
    const GalleryEntry& e = g_entries[static_cast<size_t>(pressedIndex)];
    g_menuPath = e.path;
    g_menuName = e.displayName;
    // No explicit position: `OpenPopup()` records the pointer's own position
    // and `BeginPopup()` opens there, which is the finger for a long press.
    ImGui::OpenPopup(kTileMenuPopup);
  }

  bool openRename = demoRename;
  bool openDelete = demoDelete;
  if (demoRename || demoDelete) ImGui::CloseCurrentPopup();
  if (ImGui::BeginPopup(kTileMenuPopup)) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)), "%s",
                       g_menuName.c_str());
    ImGui::Separator();
    if (ImGui::Selectable("Rename\xe2\x80\xa6")) openRename = true;
    if (ImGui::Selectable("Duplicate")) {
      const std::string dst = galleryDuplicatePath(g_menuPath);
      if (dst.empty()) {
        g_galleryStatus = "Could not duplicate \xe2\x80\x9c" + g_menuName + "\xe2\x80\x9d: too many copies.";
      } else {
        std::error_code ec;
        fs::copy_file(g_menuPath, dst, ec);
        if (ec) {
          g_galleryStatus =
              "Could not duplicate \xe2\x80\x9c" + g_menuName + "\xe2\x80\x9d: " + ec.message();
        } else {
          g_galleryStatus = "Duplicated \xe2\x80\x9c" + g_menuName + "\xe2\x80\x9d.";
          invalidateGalleryScan();
        }
      }
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::Selectable("Delete\xe2\x80\xa6")) openDelete = true;
    ImGui::EndPopup();
  }

  if (openRename) {
    std::snprintf(g_renameBuf, sizeof(g_renameBuf), "%s", g_menuName.c_str());
    g_renameStatus.clear();
    ImGui::OpenPopup(kRenamePopup);
  }
  if (openDelete) {
    g_deleteStatus.clear();
    ImGui::OpenPopup(kDeletePopup);
  }

  if (beginDialog(kRenamePopup)) {
    // EnterReturnsTrue only tells us the field committed; the footer's own
    // Return is what commits the dialog, so the two must not both fire.
    dialogInputText("Name", g_renameBuf, sizeof(g_renameBuf));
    dialogHint("Renaming the file here renames it in the Files app too \xe2\x80\x94 it is the same "
               "folder.");
    dialogStatusLine(DialogStatus::Error, g_renameStatus);

    DialogFooter footer;
    footer.commit = "Rename";
    switch (dialogFooter(footer)) {
      case DialogAction::Commit: {
        const std::string name(g_renameBuf);
        g_renameStatus = galleryRenameRefusal(name);
        if (g_renameStatus.empty()) {
          const std::string dst = galleryRenamedPath(g_menuPath, name);
          std::error_code ec;
          if (dst != g_menuPath && fs::exists(dst, ec)) {
            g_renameStatus = "A document by that name is already here.";
          } else {
            fs::rename(g_menuPath, dst, ec);
            if (ec) {
              g_renameStatus = ec.message();
            } else {
              g_galleryStatus =
                  "Renamed to \xe2\x80\x9c" + fs::path(dst).stem().string() + "\xe2\x80\x9d.";
              invalidateGalleryScan();
              ImGui::CloseCurrentPopup();
            }
          }
        }
        break;
      }
      case DialogAction::Cancel:
        ImGui::CloseCurrentPopup();
        break;
      default:
        break;
    }
    endDialog();
  }

  if (beginDialog(kDeletePopup)) {
    dialogText("Delete \xe2\x80\x9c%s\xe2\x80\x9d?", g_menuName.c_str());
    // Said plainly because it is true: this is `unlink`, not a move to a
    // Trash the Files app can undo.
    dialogHint("The file is removed from this device. This cannot be undone.");
    dialogStatusLine(DialogStatus::Error, g_deleteStatus);

    DialogFooter footer;
    footer.commit = "Delete";
    footer.commitDestructive = true;
    switch (dialogFooter(footer)) {
      case DialogAction::Commit: {
        std::error_code ec;
        if (!fs::remove(g_menuPath, ec) || ec) {
          g_deleteStatus = ec ? ec.message() : std::string("The file could not be removed.");
        } else {
          g_galleryStatus = "Deleted \xe2\x80\x9c" + g_menuName + "\xe2\x80\x9d.";
          // NOT invalidateGalleryScan(): that clears every entry and forces
          // the next frame to re-read and re-thumbnail every SURVIVING
          // document from disk, just to reflect the removal of one. A delete
          // adds no new data to read -- every remaining entry's thumbnail is
          // already sitting in `g_entries` -- so splicing this one path out
          // and re-uploading the (now smaller) atlas from what is already in
          // memory is the whole update. That is what made the dialog visibly
          // hang on a gallery with more than a couple of documents: the
          // rescan's cost is linear in how many documents were LEFT, not in
          // the one being deleted.
          g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                                          [&](const GalleryEntry& e) { return e.path == g_menuPath; }),
                           g_entries.end());
          // NOT rebuilt here: this frame's tile loop (above, earlier in this
          // same function call) already recorded AddImage() calls against
          // the atlas view rebuild() is about to destroy -- see
          // `g_atlasNeedsRebuild`'s own comment for the abort this caused on
          // device. Deferred to the top of the NEXT call instead.
          g_atlasNeedsRebuild = true;
          ImGui::CloseCurrentPopup();
        }
        break;
      }
      case DialogAction::Cancel:
        ImGui::CloseCurrentPopup();
        break;
      default:
        break;
    }
    endDialog();
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  // The press that fired a menu is spent once the finger is up. Cleared after
  // the tiles have been drawn, so the release frame -- in which ImGui already
  // reports the button as up while `InvisibleButton()` still returns its
  // click -- still sees the flag and swallows that click.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_longPressFired = 0;

  // --- acting on a tap, after the frame's drawing is done --------------
  if (popupWasOpen) {
    // Nothing: this frame's click was the one that dismissed the menu.
  } else if (tappedNew) {
    // The minimal new-document path (this header's own comment): the same
    // size/space main.cpp seeds a session with, not the New Document
    // dialog's size-and-preset modal.
    OpenDocument fresh = makeBlankOpenDocument(2000, 1500, WorkingSpace{});
    st.documents.add(std::move(fresh));
    st.showDocumentGallery = false;
    invalidateGalleryScan();
  } else if (tappedIndex >= 0 && tappedIndex < static_cast<int>(g_entries.size())) {
    if (!st.recentDocumentsLoaded) {
      st.recentDocumentsLoaded = true;
      st.recentDocuments.loadFromFile(defaultRecentDocumentsPath());
    }
    OpenAnyResult opened = openAnyFileAsDocument(g_entries[static_cast<size_t>(tappedIndex)].path,
                                                 &st.recentDocuments);
    if (opened.ok) {
      st.documents.add(std::move(opened.document));
      if (!opened.guides.empty()) st.guides = std::move(opened.guides);
      std::string recentSaveError;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &recentSaveError);
      st.showDocumentGallery = false;
      invalidateGalleryScan();
    }
    // A refusal leaves the gallery open with nothing else drawn to explain
    // it yet -- `opened.status` names the reason (app/OpenAnyFile.hpp's own
    // contract) and belongs on a status line this first version does not
    // have. Documented here rather than silently swallowed.
  }
}

}  // namespace np
