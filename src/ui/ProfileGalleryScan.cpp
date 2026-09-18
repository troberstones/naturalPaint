#include "ui/ProfileGalleryScan.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "io/NpaintFile.hpp"
#include "ui/DocumentGallery.hpp"

namespace fs = std::filesystem;

namespace np {
namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

uintmax_t dirBytes(const fs::path& dir) {
  uintmax_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
    total += it->file_size(ec);
  }
  return total;
}

}  // namespace

int runProfileGalleryScan(int count, int width, int height) {
  if (count <= 0) count = 6;
  if (width <= 0) width = 3000;
  if (height <= 0) height = 3000;

  const fs::path root = fs::temp_directory_path() / "np-profile-gallery-scan";
  const fs::path docs = root / "docs";
  const fs::path cache = root / "cache";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(docs, ec);

  std::printf("[profile-gallery-scan] writing %d documents of %dx%d under %s\n", count, width,
              height, docs.string().c_str());
  for (int n = 0; n < count; ++n) {
    Document doc = Document::createBlank(width, height, WorkingSpace{});
    const float hue = static_cast<float>(n) / static_cast<float>(count);
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const PixelCoord at{x, y};
        doc.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at),
                        {hue, static_cast<float>(x) / static_cast<float>(width),
                         static_cast<float>(y) / static_cast<float>(height), 1.0f});
      }
    }
    const std::string path = (docs / ("doc" + std::to_string(n) + ".npaint")).string();
    const NpaintSaveResult saved = saveNpaint(doc, path);
    if (!saved.ok) {
      std::printf("[profile-gallery-scan] FAIL: could not save %s\n", path.c_str());
      return 1;
    }
  }
  std::printf("[profile-gallery-scan] documents on disk: %ju bytes\n", dirBytes(docs));

  auto timed = [&](const char* label, const std::string& cacheDir, GalleryScanStats& st,
                   std::vector<GalleryEntry>& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out = scanDocumentGallery(docs.string(), cacheDir, &st);
    const double ms = msSince(t0);
    std::printf("[profile-gallery-scan] %-22s %9.1f ms   decoded %d, cache hits %d\n", label, ms,
                st.decoded, st.cacheHits);
  };

  GalleryScanStats none, cold, warm;
  std::vector<GalleryEntry> noneOut, coldOut, warmOut;
  timed("no cache", "", none, noneOut);
  timed("cold cache (write)", cache.string(), cold, coldOut);
  timed("warm cache", cache.string(), warm, warmOut);
  std::printf("[profile-gallery-scan] cache on disk: %ju bytes for %d documents\n", dirBytes(cache),
              count);

  bool ok = warm.decoded == 0 && warm.cacheHits == count && warmOut.size() == coldOut.size();
  for (size_t i = 0; ok && i < warmOut.size(); ++i) {
    ok = warmOut[i].path == coldOut[i].path && !warmOut[i].thumb.rgba.empty() &&
         warmOut[i].thumb.rgba == coldOut[i].thumb.rgba;
  }
  std::printf("[profile-gallery-scan] warm scan served every tile from the cache, identical to the "
              "cold scan: %s\n", ok ? "yes" : "NO");
  fs::remove_all(root, ec);
  return ok ? 0 : 1;
}

}  // namespace np
