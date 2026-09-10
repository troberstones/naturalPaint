#include "app/PsdExportCli.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/TileStore.hpp"
#include "io/Export.hpp"
#include "io/PsdExport.hpp"

namespace np {
namespace {

// Straight linear RGBA into a layer, premultiplied on the way in exactly the
// way io/ImageIO.cpp's writeDecodedImageIntoLayer() does. The fixture has to
// hold what a real painted document holds, or the thing psd-tools looks at is
// not the thing users will export.
void writeStraight(Document& doc, size_t layerIndex, int32_t x, int32_t y, float r, float g,
                   float b, float a) {
  TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
  const PixelCoord p{x, y};
  tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
}

// 129x97: odd on both axes, a multiple of no tile size, and not square -- so a
// transposed width/height, a row-stride error and a plane-length error all
// produce a visibly different picture rather than a subtly different one.
constexpr int32_t kW = 129;
constexpr int32_t kH = 97;

Document buildFixture() {
  Document doc = Document::createBlank(kW, kH, WorkingSpace{});
  doc.layers[0].name = "ramp";

  // Base: a two-axis linear ramp, opaque, with a transparent left margin. The
  // margin is what makes the layer above it produce a FRACTIONAL composite
  // alpha somewhere -- without it every pixel would be opaque and the straight
  // vs premultiplied question would have no pixel that could answer it.
  constexpr int32_t kMargin = 24;
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = kMargin; x < kW; ++x) {
      const float u = static_cast<float>(x - kMargin) / static_cast<float>(kW - 1 - kMargin);
      const float v = static_cast<float>(y) / static_cast<float>(kH - 1);
      writeStraight(doc, 0, x, y, u, v, 0.25f, 1.0f);
    }
  }

  // Above it, a half-opaque rectangle straddling the margin. Over the
  // transparent part the composite is alpha 0.5 carrying this exact straight
  // colour; over the ramp it is an ordinary `over`.
  Layer top;
  top.kind = LayerKind::RGB;
  top.name = "half-opaque patch";
  top.rgbTiles.emplace();
  doc.layers.push_back(std::move(top));
  for (int32_t y = 12; y < 60; ++y)
    for (int32_t x = 6; x < 70; ++x) writeStraight(doc, 1, x, y, 0.9f, 0.1f, 0.4f, 0.5f);

  return doc;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(bytes.data()),
          static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(f);
}

void printWarnings(const char* what, const std::vector<std::string>& warnings) {
  for (const std::string& w : warnings) std::printf("  %s warning: %s\n", what, w.c_str());
}

}  // namespace

int runPsdExportDemo(const char* outPath, bool layered) {
  const std::string psdPath = outPath;
  const std::string pngPath = psdPath + ".png";

  const Document doc = buildFixture();
  std::printf("--psd-export%s: %dx%d, %zu layers\n", layered ? " (layered)" : "", doc.width,
              doc.height, doc.layers.size());

  // Both tiers from one fixture, so a difference between the two files is a
  // difference in the writers and not in what was written.
  const PsdExportResult psd = layered ? writeLayeredPsd(doc) : writeFlattenedPsd(doc);
  if (!psd.ok) {
    std::fprintf(stderr, "--psd-export: refused: %s\n", psd.error.c_str());
    return 1;
  }
  printWarnings("psd", psd.warnings);
  if (!writeFile(psdPath, psd.bytes)) {
    std::fprintf(stderr, "--psd-export: could not write %s\n", psdPath.c_str());
    return 1;
  }
  std::printf("  wrote %s (%zu bytes)\n", psdPath.c_str(), psd.bytes.size());

  // The independent reference, through a path that shares nothing with the one
  // above below the flatten: io/Export's existing 8-bit sRGB PNG encoder.
  const ExportResult png =
      exportDocument(doc, ImageFormat::Png, ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
  if (!png.ok) {
    std::fprintf(stderr, "--psd-export: the reference PNG was refused: %s\n", png.error.c_str());
    return 1;
  }
  printWarnings("png", png.warnings);
  if (!writeFile(pngPath, png.bytes)) {
    std::fprintf(stderr, "--psd-export: could not write %s\n", pngPath.c_str());
    return 1;
  }
  std::printf("  wrote %s (%zu bytes) -- the independent reference\n", pngPath.c_str(),
              png.bytes.size());
  return 0;
}

}  // namespace np
