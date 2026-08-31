#include "app/ProfileToggle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "core/Composite.hpp"
#include "core/DirtyTiles.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "io/PsdImport.hpp"
#include "ui/DocumentTexture.hpp"

namespace np {

namespace {

// `ui/DocumentTexture::viewFor()`'s CPU-only work for one key miss, with the
// `wgpuQueueWriteTexture` calls removed and nothing else changed -- see
// ProfileToggle.hpp for why that omission does not change what this measures.
// `halves` is the canvas-sized straight-alpha f16 mirror
// (`DocumentTexture::halves_`'s analogue here), patched in place for an
// incremental key miss rather than rebuilt, so a caller timing this does not
// credit the incremental path with a canvas-sized allocation it does not pay
// for in production.
void applyDocumentUpdate(const Document& snapshot, const Document& doc,
                         std::vector<uint16_t>& halves) {
  DocumentDirtyTiles dirty = documentDirtyTiles(snapshot, doc);
  if (!dirty.everything &&
      preferFullRecomposite(dirty.tiles.size(), canvasTileCount(doc))) {
    dirty.everything = true;
    dirty.tiles.clear();
  }

  if (dirty.everything) {
    halves = compositeDocumentStraightHalf(doc, nullptr);
    return;
  }
  if (dirty.tiles.empty()) return;  // an empty key miss: nothing to composite or pack

  size_t i = 0;
  while (i < dirty.tiles.size()) {
    size_t j = i;
    while (j < dirty.tiles.size() && dirty.tiles[j].y == dirty.tiles[i].y) ++j;

    const PixelCoord first = tileOrigin(dirty.tiles[i]);
    const PixelCoord last = tileOrigin(dirty.tiles[j - 1]);
    const int32_t x0 = std::max(first.x, 0);
    const int32_t x1 = std::min(last.x + kTileSize, doc.width);
    const int32_t y0 = std::max(first.y, 0);
    const int32_t y1 = std::min(first.y + kTileSize, doc.height);
    if (x1 <= x0 || y1 <= y0) {
      i = j;
      continue;  // a store may hold a tile entirely outside the canvas
    }

    const size_t bandTexels = static_cast<size_t>(x1 - x0) * static_cast<size_t>(y1 - y0);
    std::vector<float> scratch(bandTexels * 4, 0.0f);

    CompositeRegion region;
    region.pixels = scratch.data();
    region.origin = PixelCoord{x0, y0};
    region.width = x1 - x0;
    region.height = y1 - y0;
    const std::vector<TileCoord> band(dirty.tiles.begin() + static_cast<ptrdiff_t>(i),
                                      dirty.tiles.begin() + static_cast<ptrdiff_t>(j));
    compositeDocumentTilesPremultiplied(doc, band, region, nullptr);

    for (size_t k = i; k < j; ++k) {
      const PixelCoord origin = tileOrigin(dirty.tiles[k]);
      const int32_t tx0 = std::max(origin.x, 0);
      const int32_t tx1 = std::min(origin.x + kTileSize, doc.width);
      if (tx1 <= tx0) continue;
      for (int32_t y = y0; y < y1; ++y) {
        packStraightHalfRow(
            scratch.data() +
                (static_cast<size_t>(y - y0) * static_cast<size_t>(region.width) +
                 static_cast<size_t>(tx0 - x0)) *
                    4u,
            static_cast<size_t>(tx1 - tx0),
            halves.data() +
                (static_cast<size_t>(y) * static_cast<size_t>(doc.width) +
                 static_cast<size_t>(tx0)) *
                    4u);
      }
    }
    i = j;
  }
}

}  // namespace

int runProfileToggle(const char* path, int layerIndex, int iterations) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "profile-toggle: cannot open %s\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
  if (!r.ok) {
    std::fprintf(stderr, "profile-toggle: import failed: %s\n", r.error.c_str());
    return 1;
  }
  Document doc = r.document;
  if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= doc.layers.size()) {
    std::fprintf(stderr, "profile-toggle: layer index %d out of range (0..%zu)\n", layerIndex,
                 doc.layers.size() - 1);
    return 1;
  }
  if (iterations < 1) iterations = 1;

  std::printf("profile-toggle: %s\n", path);
  std::printf("profile-toggle: layer %d = \"%s\" (%dx%d canvas, %zu layers), %d iterations\n",
              layerIndex, doc.layers[static_cast<size_t>(layerIndex)].name.c_str(), doc.width,
              doc.height, doc.layers.size(), iterations);

  // One untimed composite first: the timed loop below should measure steady-
  // state cost, not the allocator growth and page faults the very first
  // composite of a freshly-imported document always pays. It also seeds
  // `halves`, exactly as `DocumentTexture::viewFor()`'s first (always full)
  // key miss seeds `halves_`, and `snapshot` -- the previous composite's
  // document -- for the first timed iteration's `documentDirtyTiles()` call
  // to compare against.
  std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc, nullptr);
  Document snapshot = doc;

  std::vector<double> ms(static_cast<size_t>(iterations));
  uint64_t fullCount = 0, incrementalCount = 0, emptyCount = 0;
  for (int i = 0; i < iterations; ++i) {
    Layer& layer = doc.layers[static_cast<size_t>(layerIndex)];
    layer.visible = !layer.visible;

    const DocumentDirtyTiles peek = documentDirtyTiles(snapshot, doc);
    if (peek.everything ||
        (!peek.everything && preferFullRecomposite(peek.tiles.size(), canvasTileCount(doc))))
      ++fullCount;
    else if (peek.tiles.empty())
      ++emptyCount;
    else
      ++incrementalCount;

    const auto t0 = std::chrono::steady_clock::now();
    applyDocumentUpdate(snapshot, doc, halves);
    const auto t1 = std::chrono::steady_clock::now();
    ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();

    snapshot = doc;
    if (halves.empty() && doc.width > 0 && doc.height > 0) {
      std::fprintf(stderr, "profile-toggle: empty composite result, aborting\n");
      return 1;
    }
  }

  std::vector<double> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0.0;
  for (double v : ms) sum += v;
  std::printf("profile-toggle: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms (n=%d)\n",
              sorted.front(), sorted[sorted.size() / 2], sum / static_cast<double>(ms.size()),
              sorted.back(), iterations);
  std::printf("profile-toggle: %llu full, %llu incremental, %llu empty key miss(es)\n",
              static_cast<unsigned long long>(fullCount),
              static_cast<unsigned long long>(incrementalCount),
              static_cast<unsigned long long>(emptyCount));
  return 0;
}

}  // namespace np
