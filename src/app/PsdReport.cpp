#include "app/PsdReport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "core/VectorShape.hpp"
#include "io/PsdImport.hpp"

namespace np {
namespace {

// What actually arrived in a layer's pixels, derived from the tiles rather
// than from the record that claimed them.
//
// This is the half of the report that cannot be faked by a parser that read
// the header correctly and the channel data not at all: a layer whose record
// says (82,158)-(376,503) and whose tile set is empty parsed its rectangle
// and lost its picture, and that reads as a blank row here rather than as a
// plausible one. `opaque` counts alpha == 1 exactly so a fully-opaque layer
// and a soft-edged one are told apart -- Photoshop's "flats" layers are
// overwhelmingly the former, and a reader that mangled the transparency
// channel tends to produce neither.
struct PixelExtent {
  bool any = false;
  int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;  // inclusive, document space
  size_t tiles = 0;
  size_t nonZeroAlpha = 0;
  size_t opaque = 0;

  // Mean **straight** (un-premultiplied) linear RGB over the alpha > 0
  // pixels, and mean alpha over the same set.
  //
  // Geometry and alpha counts alone cannot catch a channel swap, an
  // off-by-one row stride, or a missing sRGB linearisation: all three leave
  // the extent and the opaque-pixel count exactly right and the picture
  // wrong. A mean can, because an independent reader of the same file can be
  // asked for the identical number -- straight, not premultiplied, so the
  // comparison does not also depend on both readers agreeing about
  // premultiplication, and linear, so it does not depend on both agreeing
  // about where to re-encode.
  double meanR = 0.0, meanG = 0.0, meanB = 0.0, meanA = 0.0;
};

PixelExtent measure(const TileStore& tiles) {
  PixelExtent e;
  e.tiles = tiles.occupiedTileCount();
  for (const auto& [coord, tile] : tiles) {
    const int32_t ox = coord.x * kTileSize;
    const int32_t oy = coord.y * kTileSize;
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const std::array<float, 4> px = tile.readPixel(PixelCoord{x, y});
        if (px[3] <= 0.0f) continue;
        ++e.nonZeroAlpha;
        if (px[3] >= 1.0f) ++e.opaque;
        const double inv = 1.0 / static_cast<double>(px[3]);
        e.meanR += static_cast<double>(px[0]) * inv;
        e.meanG += static_cast<double>(px[1]) * inv;
        e.meanB += static_cast<double>(px[2]) * inv;
        e.meanA += static_cast<double>(px[3]);
        const int32_t dx = ox + x;
        const int32_t dy = oy + y;
        if (!e.any) {
          e.any = true;
          e.minX = e.maxX = dx;
          e.minY = e.maxY = dy;
        } else {
          e.minX = std::min(e.minX, dx);
          e.maxX = std::max(e.maxX, dx);
          e.minY = std::min(e.minY, dy);
          e.maxY = std::max(e.maxY, dy);
        }
      }
    }
  }
  if (e.nonZeroAlpha > 0) {
    const double n = static_cast<double>(e.nonZeroAlpha);
    e.meanR /= n;
    e.meanG /= n;
    e.meanB /= n;
    e.meanA /= n;
  }
  return e;
}

}  // namespace

int runPsdReport(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "psd-report: cannot open %s\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  std::printf("psd-report: %s (%zu bytes)\n", path, bytes.size());

  const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
  if (!r.ok) {
    if (r.noLayerData) {
      // Not a parse failure. Said in these words because the two outcomes
      // need completely different responses: this one means the file really
      // carries no layer section (Maximize Compatibility off) and File > Open
      // would correctly fall back to the flattened composite, whereas any
      // other `!ok` means layer data existed and this reader could not read
      // it -- app/OpenAnyFile.cpp refuses the whole file for exactly that
      // distinction, and a report that blurred it would hide which one held.
      std::fprintf(stderr,
                   "psd-report: no layer data in this file -- it carries only the flattened\n"
                   "            composite, so File > Open falls back to the flattened path.\n"
                   "            (%s)\n",
                   r.error.c_str());
      return 1;
    }
    std::fprintf(stderr, "psd-report: import failed: %s\n", r.error.c_str());
    return 1;
  }

  const Document& doc = r.document;
  std::printf("\ndocument %dx%d, %zu layers\n\n", doc.width, doc.height, doc.layers.size());

  std::printf("%-4s %-34s %-10s %6s %4s %4s %-6s %5s  %-22s %s\n", "#", "name", "blend", "opac",
              "vis", "clip", "label", "tiles", "pixel extent (l,t,r,b)", "coverage");
  std::printf("%-4s %-34s %-10s %6s %4s %4s %-6s %5s  %-22s %s\n", "----",
              "----------------------------------", "----------", "------", "---", "----",
              "------", "-----", "----------------------", "--------");

  // Folded rather than listed: a real document repeats the same handful of
  // blend keys across dozens of layers, and "45 x norm" is the finding where
  // 45 identical lines are noise.
  std::map<std::string, size_t> blendHistogram;
  size_t hidden = 0;
  size_t clipped = 0;
  size_t empty = 0;

  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& l = doc.layers[i];
    ++blendHistogram[l.blend];
    if (!l.visible) ++hidden;
    if (l.clipped) ++clipped;

    PixelExtent e;
    if (l.rgbTiles.has_value()) e = measure(*l.rgbTiles);

    // A Vector layer's content is its shapes; it has no tiles at all, and
    // measuring only tiles would report every imported shape layer as EMPTY
    // -- the exact opposite of the truth, and the reason this branch exists
    // rather than the column simply being blank for it.
    const bool isVector = l.kind == LayerKind::Vector;
    if (!e.any && !isVector) ++empty;

    // `--` for unlabelled ('lclr' absent or index 0), which is the
    // overwhelmingly common case -- an empty column would be easy to misread
    // as a missing measurement rather than "no colour".
    const char* label = l.colorLabel.empty() ? "--" : l.colorLabel.c_str();

    char extent[48];
    char coverage[128];
    if (isVector) {
      // The DRAWN extent, so this column is comparable with the raster rows
      // above it and with an external renderer. Deliberately not
      // `vectorShapesBounds()`, which outsets by the worst case a mitre join
      // could reach -- Photoshop writes miterLimit 100, so that is half the
      // stroke width times 100, and every shape here would report 50 px of
      // margin no join in it actually uses.
      PathBounds b;
      for (const VectorShape& sh : l.shapes) {
        const PathBounds t = pathTightBounds(sh.path);
        if (!t.valid) continue;
        b = b.valid ? PathBounds{true, std::min(b.minX, t.minX), std::min(b.minY, t.minY),
                                 std::max(b.maxX, t.maxX), std::max(b.maxY, t.maxY)}
                    : t;
      }
      if (b.valid) {
        std::snprintf(extent, sizeof(extent), "(%.0f,%.0f,%.0f,%.0f)",
                      static_cast<double>(b.minX), static_cast<double>(b.minY),
                      static_cast<double>(b.maxX), static_cast<double>(b.maxY));
      } else {
        std::snprintf(extent, sizeof(extent), "-- NO SHAPES --");
      }
      size_t filled = 0, stroked = 0;
      for (const VectorShape& sh : l.shapes) {
        if (sh.fill.on) ++filled;
        if (sh.stroke.on) ++stroked;
      }
      // A GRADIENT fill's `rgba` is meaningless (core/VectorShape.hpp), so
      // printing it would report a black fill where the truth is a ramp --
      // the same shape of lie as reporting a Vector layer as EMPTY, which is
      // why this whole branch exists.
      const Paint firstFill = l.shapes.empty() ? Paint{} : l.shapes.front().fill;
      if (firstFill.kind == PaintKind::Gradient) {
        const size_t stops = firstFill.gradient < doc.gradients.size()
                                 ? doc.gradients[firstFill.gradient].stops.colorStops.size()
                                 : 0;
        std::snprintf(coverage, sizeof(coverage),
                      "%zu shape(s), %zu filled, %zu stroked, first fill GRADIENT #%u (%zu "
                      "colour stop(s))",
                      l.shapes.size(), filled, stroked, firstFill.gradient, stops);
      } else {
        const std::array<float, 4>& c = firstFill.rgba;
        std::snprintf(coverage, sizeof(coverage),
                      "%zu shape(s), %zu filled, %zu stroked, first fill linear rgba "
                      "%.5f %.5f %.5f %.5f",
                      l.shapes.size(), filled, stroked, static_cast<double>(c[0]),
                      static_cast<double>(c[1]), static_cast<double>(c[2]),
                      static_cast<double>(c[3]));
      }
      std::printf("%-4zu %-34.34s %-10.10s %6.3f %4s %4s %-6s %5s  %-22s %s\n", i, l.name.c_str(),
                  l.blend.c_str(), static_cast<double>(l.opacity), l.visible ? "Y" : "n",
                  l.clipped ? "Y" : "n", label, "vec", extent, coverage);
      continue;
    }

    if (e.any) {
      std::snprintf(extent, sizeof(extent), "(%d,%d,%d,%d)", e.minX, e.minY, e.maxX + 1,
                    e.maxY + 1);
    } else {
      std::snprintf(extent, sizeof(extent), "-- EMPTY --");
    }

    if (e.nonZeroAlpha == 0) {
      std::snprintf(coverage, sizeof(coverage), "no alpha > 0");
    } else {
      std::snprintf(coverage, sizeof(coverage),
                    "%zu px, %zu opaque, mean linear rgba %.5f %.5f %.5f %.5f", e.nonZeroAlpha,
                    e.opaque, e.meanR, e.meanG, e.meanB, e.meanA);
    }

    std::printf("%-4zu %-34.34s %-10.10s %6.3f %4s %4s %-6s %5zu  %-22s %s\n", i, l.name.c_str(),
                l.blend.c_str(), static_cast<double>(l.opacity), l.visible ? "Y" : "n",
                l.clipped ? "Y" : "n", label, e.tiles, extent, coverage);
  }

  std::printf("\n-- blend keys as imported --\n");
  for (const auto& [name, n] : blendHistogram)
    std::printf("  %-20s %zu layer(s)\n", name.c_str(), n);
  std::printf("  hidden: %zu   clipped: %zu   with no pixels at all: %zu   of %zu\n", hidden,
              clipped, empty, doc.layers.size());

  // Guides (Image Resources 1032), in FILE order -- the order the user
  // created them, not sorted. Empty is the common case, not a failure: see
  // io/PsdImport.hpp's own header on `PsdImportResult::guides`.
  std::printf("\n-- guides: %zu --\n", r.guides.size());
  for (size_t i = 0; i < r.guides.size(); ++i) {
    const PsdGuide& g = r.guides[i];
    std::printf("  %-4zu %-10s %.1f px\n", i, g.vertical ? "vertical" : "horizontal",
               static_cast<double>(g.position));
  }

  // The warnings are per-layer and, like the blend keys, repetitive by nature
  // -- one Photoshop document tends to reach for the same unmapped mode many
  // times. Folded by text for the same reason `--abr-report` folds its notes.
  std::printf("\n-- import warnings, folded by kind --\n");
  if (r.warnings.empty()) {
    std::printf("  (none)\n");
  } else {
    std::map<std::string, size_t> byKind;
    for (const std::string& w : r.warnings) ++byKind[w];
    for (const auto& [what, n] : byKind) std::printf("  %4zu x  %s\n", n, what.c_str());
  }

  if (empty > 0) {
    std::printf(
        "\n**%zu of %zu layers imported with no pixels at all.**\n"
        "A group's own boundary-marker record is legitimately empty and this\n"
        "build imports it as an ordinary layer (io/PsdImport.hpp names group\n"
        "reconstruction as out of scope), so a count matching the document's\n"
        "group count is expected. A count larger than that is not: it means\n"
        "layers whose rectangles parsed lost their channel data.\n",
        empty, doc.layers.size());
  }
  return 0;
}

}  // namespace np
