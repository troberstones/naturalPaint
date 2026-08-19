#include "core/Probe.hpp"

#include "color/Space.hpp"

namespace np {
namespace {

// Sums premultiplied rgba texels over the sampleSize x sampleSize box
// centred on `at`, read from one layer's tile storage. Returns the raw sum
// -- not yet divided by count, not yet un-premultiplied.
//
// Averaging premultiplied values and un-premultiplying once at the end
// (rather than un-premultiplying every texel first, then averaging
// straight colour) is deliberate, not incidental: a box straddling painted
// and never-painted texels has to treat the never-painted ones as "no
// colour contributed" rather than "contributed black at full weight", and
// summing/dividing in premultiplied space does exactly that -- a texel
// with alpha 0 adds 0 to both the colour sum and the alpha sum, so it
// dilutes the *coverage* of the average without dragging the *colour*
// toward black. Un-premultiplying per texel first would have to invent an
// RGB value for those alpha-0 texels (0 is the obvious choice, but it's
// still invented), and averaging those invented straight values in would
// wrongly darken the reported colour. This is the same reasoning that
// makes premultiplied alpha the correct representation for filtering/
// resampling in general, not something specific to this probe.
//
// Missing tiles (TileStore::find() returning nullptr -- an unpainted
// region, or a coordinate outside anything ever painted) contribute
// {0,0,0,0}, exactly the premultiplied value core::Tile itself gives an
// unwritten texel -- so this needs no separate bounds check.
std::array<float, 4> sumPremultipliedBox(const TileStore& tiles, PixelCoord at,
                                          int32_t sampleSize) {
  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  const int32_t half = sampleSize / 2;  // floor; see ProbeParams::sampleSize's doc comment
  for (int32_t dy = 0; dy < sampleSize; ++dy) {
    for (int32_t dx = 0; dx < sampleSize; ++dx) {
      const PixelCoord doc{at.x - half + dx, at.y - half + dy};
      const Tile* tile = tiles.find(tileCoordAt(doc));
      const std::array<float, 4> px =
          tile ? tile->readPixel(tileLocalOffset(doc)) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
      sum[0] += px[0];
      sum[1] += px[1];
      sum[2] += px[2];
      sum[3] += px[3];
    }
  }
  return sum;
}

// straight[i] = premultiplied[i] / a for the RGB channels, guarding a == 0
// (fully transparent -- RGB is arbitrary under premultiplied alpha, and 0
// is the same convention core::Tile's own value-initialization already
// uses for an untouched texel). Mirrors io/ImageIO.cpp's write-side
// `rgb *= a` (DESIGN-imaging.md §2's storage policy) at this read
// boundary, the same write/read split io/ImageDecode.hpp's header comment
// documents for the opposite (decode) direction.
std::array<float, 4> unpremultiply(const std::array<float, 4>& premultiplied) {
  const float a = premultiplied[3];
  if (a <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
  return {premultiplied[0] / a, premultiplied[1] / a, premultiplied[2] / a, a};
}

}  // namespace

ProbeSample probePixel(const Document& doc, PixelCoord at, const ProbeParams& params) {
  const int32_t sampleSize = params.sampleSize > 0 ? params.sampleSize : 1;
  const float count = static_cast<float>(sampleSize) * static_cast<float>(sampleSize);

  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  bool any = false;

  auto accumulate = [&](const Layer& layer) {
    if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) return;
    const std::array<float, 4> layerSum = sumPremultipliedBox(*layer.rgbTiles, at, sampleSize);
    sum[0] += layerSum[0];
    sum[1] += layerSum[1];
    sum[2] += layerSum[2];
    sum[3] += layerSum[3];
    any = true;
  };

  if (params.sampleAllLayers) {
    // Gathers every RGB-kind, populated-tile-storage layer. This is a
    // plain sum, not Porter-Duff "over" compositing -- there is no blend-
    // mode/compositing implementation anywhere in this codebase yet (a
    // later phase), so there is nothing correct to fake here. It is the
    // right sum for *today's* invariant of at most one such layer (summing
    // zero or one contribution needs no blending math at all); the moment
    // a second RGB/Pigment/Media layer can hold content, this loop is
    // exactly where real per-layer alpha-under compositing has to replace
    // the plain sum -- ProbeParams::sampleAllLayers's own doc comment
    // flags the same thing.
    for (const Layer& layer : doc.layers) accumulate(layer);
  } else if (params.activeLayerIndex >= 0 &&
             static_cast<size_t>(params.activeLayerIndex) < doc.layers.size()) {
    accumulate(doc.layers[static_cast<size_t>(params.activeLayerIndex)]);
  }

  ProbeSample result;
  if (!any) return result;  // ProbeSample{} -- fully transparent black, per the header comment

  const std::array<float, 4> avgPremultiplied{sum[0] / count, sum[1] / count, sum[2] / count,
                                               sum[3] / count};
  result.linear = unpremultiply(avgPremultiplied);
  result.display = {srgbEncode(result.linear[0]), srgbEncode(result.linear[1]),
                     srgbEncode(result.linear[2]), result.linear[3]};
  return result;
}

}  // namespace np
