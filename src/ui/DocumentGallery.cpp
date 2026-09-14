#include "ui/DocumentGallery.hpp"

#include <algorithm>
#include <array>
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
constexpr float kTileGap = kWindowPaddingX * 2.0f;  // 16 -- ui/AtelierTheme's own unit
constexpr float kTileCaptionH = 40.0f;              // two text lines' worth
constexpr float kTileW = static_cast<float>(kGalleryThumbPx);
constexpr float kTileH = static_cast<float>(kGalleryThumbPx) + kTileCaptionH;

// One "+" or thumbnail tile: the clickable area, the frame, and the label
// underneath. Returns true on a tap (released while still hovered, the same
// contract `ImGui::InvisibleButton()` gives every other click target in this
// codebase).
bool drawTile(ImDrawList* dl, const ImVec2& at, const char* line1, const char* line2) {
  ImGui::SetCursorScreenPos(at);
  // Stable per grid cell for this frame -- the screen position is unique
  // across the tiles this function draws in one call.
  ImGui::PushID(static_cast<int>(at.y * 100000.0f + at.x));
  const bool clicked = ImGui::InvisibleButton("##tile", ImVec2(kTileW, kTileH + 6.0f));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();

  const ImVec2 thumbLo = at;
  const ImVec2 thumbHi(at.x + kTileW, at.y + kTileW);
  dl->AddRect(ImVec2(thumbLo.x - 1.0f, thumbLo.y - 1.0f), ImVec2(thumbHi.x + 1.0f, thumbHi.y + 1.0f),
              hovered ? atelierToken(kAccent) : atelierToken(kHairline), 4.0f, 0, hovered ? 2.0f : 1.0f);

  if (line1 != nullptr) {
    const ImVec2 capAt(at.x, at.y + kTileW + 6.0f);
    dl->AddText(capAt, atelierToken(kTextPrimary), line1);
    if (line2 != nullptr)
      dl->AddText(ImVec2(capAt.x, capAt.y + ImGui::GetFontSize() + 2.0f),
                  atelierToken(kTextSecondary), line2);
  }
  return clicked;
}

}  // namespace

std::string iosDocumentsDirectory() {
  const char* home = std::getenv("HOME");
  return (home != nullptr ? std::string(home) : std::string()) + "/Documents";
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

void drawDocumentGallery(AppState& st, GpuContext& gpu) {
  if (!g_scanned) {
    g_entries = scanDocumentGallery(iosDocumentsDirectory());
    g_atlas.rebuild(gpu, g_entries);
    g_scanned = true;
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
  ImGui::Dummy(ImVec2(1.0f, 12.0f));

  ImGui::BeginChild("##galleryGrid", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const float availW = ImGui::GetContentRegionAvail().x;
  const int columns = std::max(1, static_cast<int>((availW + kTileGap) / (kTileW + kTileGap)));

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  int col = 0, row = 0;
  int tappedIndex = -1;   // index into g_entries, or -1
  bool tappedNew = false;

  auto cellPos = [&](int c, int r) {
    return ImVec2(origin.x + c * (kTileW + kTileGap), origin.y + r * (kTileH + kTileGap));
  };

  // The "+" tile first -- "make something new" reads left-to-right as the
  // first choice, and it is the one tile that is always present, even in an
  // empty gallery (this function's own doc comment on a fresh install).
  if (drawTile(dl, cellPos(col, row), "New Document", nullptr)) tappedNew = true;
  {
    // The plus glyph itself, centred in the tile drawn above.
    const ImVec2 c = cellPos(col, row);
    const ImVec2 mid(c.x + kTileW * 0.5f, c.y + kTileW * 0.5f);
    const float arm = kTileW * 0.18f;
    const ImU32 col32 = atelierToken(kTextSecondary);
    dl->AddLine(ImVec2(mid.x - arm, mid.y), ImVec2(mid.x + arm, mid.y), col32, 2.0f);
    dl->AddLine(ImVec2(mid.x, mid.y - arm), ImVec2(mid.x, mid.y + arm), col32, 2.0f);
  }
  ++col;
  if (col >= columns) { col = 0; ++row; }

  for (size_t i = 0; i < g_entries.size(); ++i) {
    const GalleryEntry& e = g_entries[i];
    const ImVec2 p = cellPos(col, row);
    const char* caption2 = e.modifiedLabel.empty() ? nullptr : e.modifiedLabel.c_str();
    if (drawTile(dl, p, e.displayName.c_str(), caption2)) tappedIndex = static_cast<int>(i);

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

  ImGui::Dummy(ImVec2(1.0f, (row + 1) * (kTileH + kTileGap)));
  ImGui::EndChild();
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);

  // --- acting on a tap, after the frame's drawing is done --------------
  if (tappedNew) {
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
