#include "app/StrokePreview.hpp"

#include <algorithm>
#include <cmath>

#include "app/StrokeSession.hpp"
#include "color/Space.hpp"
#include "core/LayerOps.hpp"
#include "brush/Library.hpp"
#include "core/Pigment.hpp"
#include "core/ResourcePaths.hpp"
#include "core/TileStore.hpp"
#include "app/DocumentLifecycle.hpp"
#include "io/Export.hpp"

#include <cstdio>

namespace np {
namespace {

// The same paper `app/DabPreview` composites over, and the same byte literals
// -- one number in two files would be two papers, and the two previews sit one
// above the other in the same panel where any difference would read as a bug in
// whichever one the eye landed on second. See that file's §4 for why paper and
// not a neutral tone, and for the stated cost (a near-white pigment on paper is
// nearly invisible, because that is what painting white on white looks like).
constexpr float kPaperSrgb[3] = {0xf8 / 255.0f, 0xf4 / 255.0f, 0xf4 / 255.0f};

constexpr float kPi = 3.14159265358979f;

uint8_t toByte(float linear) noexcept {
  const float s = srgbEncode(linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear));
  const int v = static_cast<int>(s * 255.0f + 0.5f);
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

float strokePreviewReach(const BrushState& brush, const MixboxLut& lut) {
  // **Sampled through `brushTipFor()` rather than read off `brush.radius` and
  // the link ranges.** The resolved radius is a product of every enabled
  // Multiply link, a curve, an inversion, and -- since B6 -- a floor applied
  // outside the link set entirely (`BrushTip::sizeFloorPx`). Reconstructing
  // that here would be a second copy of `brushTipFor()` that drifts, which is
  // precisely what this module's §2 exists to refuse. So the widest radius is
  // simply the widest one the engine reports across the taper this preview
  // actually paints.
  //
  // Nine samples over [0,1] rather than the two endpoints: an INVERTED link,
  // or a curve dragged into a hump, can put the maximum anywhere in between,
  // and a preview clipped at the one pressure nobody sampled is worse than an
  // over-generous margin.
  float reach = 0.0f;
  float scatter = 0.0f;
  for (int i = 0; i <= 8; ++i) {
    const float p = static_cast<float>(i) / 8.0f;
    const BrushTip tip = brushTipFor(brush, lut, p);
    float r = std::max(tip.radius, tip.sizeFloorPx);
    // A rotated bitmap tip reaches its half-DIAGONAL, not its half-width --
    // `app/BrushSheet.cpp`'s `widestRadius()` makes the same correction, and
    // for the same reason: an ANGLE link turns the bitmap under the mark and
    // a box sized to the un-rotated extent clips the corners.
    if (tip.bitmap && tip.bitmap->width > 0 && tip.bitmap->height > 0) {
      const float w = static_cast<float>(tip.bitmap->width);
      const float h = static_cast<float>(tip.bitmap->height);
      r *= std::hypot(w, h) / std::max(w, h);
    }
    reach = std::max(reach, r);
    // Scatter is in RADII (brush/Deposit.hpp) and displaces the dab CENTRE, so
    // it adds to the reach rather than scaling it.
    scatter = std::max(scatter, tip.scatter * r);
  }

  // **`tip.scatter` above is only half the story, and the missing half is the
  // half most brushes actually use.** `brushTipFor()` resolves the HARDWARE
  // sources only -- `evaluateLinksFiltered(..., wantStrokeLocal=false)` -- and
  // Photoshop's Scatter arrives as a `RANDOM -> Scatter` link, RANDOM being
  // stroke-local. So a scattered brush reports `tip.scatter == 0` here and
  // would size its document as though the mark never left the centreline, and
  // then scatter its dabs straight off the top of it.
  //
  // The link set is the only place that reach exists before the stroke runs,
  // so it is read directly. Scatter is a `TargetCombine::Add` target, so the
  // widest displacement a link can contribute is its own `rangeHi`, and links
  // onto one Add target sum -- hence the sum rather than a max. `fabs` because
  // a range may be authored negative (the sign picks a side; the reach is the
  // same either way).
  float linkScatter = 0.0f;
  for (const BrushLink& l : brush.links.links)
    if (l.enabled && l.target == DynamicTarget::Scatter)
      linkScatter += std::fabs(l.rangeHi);
  return reach + scatter + linkScatter * reach;
}

int strokePreviewScale(float reach) noexcept {
  // **The amplitude scales with the document; the reach does not.** That
  // asymmetry is the whole of this function and it is worth spelling out,
  // because the obvious formulation gets it backwards and CLIPS. The stroke's
  // S-curve is defined in STRIP texels and multiplied up by `scale` when the
  // document is built, so its swing is always `kStrokePreviewAmplitude` strip
  // texels however big the document is. The mark's reach is a real radius in
  // DOCUMENT pixels, so in strip texels it shrinks as `reach / scale`.
  //
  // Writing the requirement in strip texels, where both terms are comparable:
  //
  //     amplitude + reach/scale + air/2  <=  height/2
  //
  // which rearranges to `scale >= reach / budget`, with the budget being
  // whatever vertical room is left for the mark once the swing and the air
  // have taken theirs. Doing this in DOCUMENT pixels instead -- `2*(amplitude
  // + reach) + air <= height*scale` -- looks equivalent and is not: it treats
  // the amplitude as a constant the scale can outgrow, when in fact the swing
  // grows in lockstep with the document and never gets any easier to fit. At
  // radius 120 that version answers 1:3, which puts the top of the mark 30
  // texels ABOVE the document, and `--selftest`'s "top and bottom rows are
  // still uniform paper" assertion is what caught it.
  //
  // `air` is 8 strip texels total so the widest dab keeps a few texels clear
  // of the strip's own hairline frame; a mark that touches the frame reads as
  // clipped even when it is exactly complete, which is `app/DabPreview`'s own
  // argument for `kDabPreviewFitRadius` being 30 rather than 32.
  constexpr float kAirTexels = 8.0f;
  const float budget =
      (static_cast<float>(kStrokePreviewHeight) - kAirTexels) * 0.5f - kStrokePreviewAmplitude;
  if (!(budget > 0.0f)) return 1;  // a strip too short to hold its own swing
  if (!(reach > budget)) return 1;
  // Ceil, so the mark always fits: a scale that rounded down would clip the
  // widest brushes, and clipping is the one failure a preview must not have --
  // a clipped mark reads as a brush that stops, which is a defect the brush
  // does not have.
  const int s = static_cast<int>(std::ceil(reach / budget));
  return s < 1 ? 1 : s;
}

StrokePreviewImage rasteriseStrokePreview(const BrushState& brush, const MixboxLut& lut) {
  StrokePreviewImage img;
  img.width = kStrokePreviewWidth;
  img.height = kStrokePreviewHeight;
  img.scale = strokePreviewScale(strokePreviewReach(brush, lut));

  const int32_t docW = kStrokePreviewWidth * img.scale;
  const int32_t docH = kStrokePreviewHeight * img.scale;

  const float paperLinear[3] = {srgbDecode(kPaperSrgb[0]), srgbDecode(kPaperSrgb[1]),
                                srgbDecode(kPaperSrgb[2])};
  const uint8_t paperByte[3] = {toByte(paperLinear[0]), toByte(paperLinear[1]),
                                toByte(paperLinear[2])};
  img.rgba.assign(static_cast<size_t>(kStrokePreviewWidth) *
                      static_cast<size_t>(kStrokePreviewHeight) * 4u,
                  0u);
  for (size_t i = 0; i < img.rgba.size(); i += 4) {
    img.rgba[i + 0] = paperByte[0];
    img.rgba[i + 1] = paperByte[1];
    img.rgba[i + 2] = paperByte[2];
    img.rgba[i + 3] = 255u;  // opaque: the ground is paper, not the panel
  }

  // A scratch document with ONE Pigment layer (§5). `makeBlankOpenDocument()`
  // creates an RGB layer at index 0, so the Pigment layer this route needs is
  // appended and painted at index 1 -- the same two lines `--selftest`'s own
  // pigment sections use, rather than a private document builder.
  OpenDocument od = makeBlankOpenDocument(docW, docH, WorkingSpace{}, "stroke-preview");
  recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                               makePigmentLayer("stroke-preview")));

  // The brush as given, forced onto the tool this preview previews. Everything
  // else -- radius, links, grain, colour, the lot -- is the user's, untouched.
  BrushState previewBrush = brush;
  previewBrush.tool = Tool::Brush;

  StrokeSession stroke;
  std::string refusal;
  if (!stroke.begin(od, od.document.layers.size() - 1, brushTipFor(previewBrush, lut, 0.0f),
                    previewBrush.tool, &refusal, &previewBrush.links)) {
    // Not swallowed and not rendered as an empty box: the panel prints this.
    img.refused = true;
    img.refusal = refusal;
    return img;
  }

  const float x0 = kStrokePreviewMarginX * static_cast<float>(img.scale);
  const float x1 = static_cast<float>(docW) - kStrokePreviewMarginX * static_cast<float>(img.scale);
  const float cy = static_cast<float>(docH) * 0.5f;
  const float amp = kStrokePreviewAmplitude * static_cast<float>(img.scale);

  for (int s = 0; s <= kStrokePreviewSamples; ++s) {
    const float t = static_cast<float>(s) / static_cast<float>(kStrokePreviewSamples);
    const float x = x0 + (x1 - x0) * t;
    // One full period, so the tangent turns through every direction and a
    // DIRECTION -> Angle link has something to say (§1, §3).
    const float y = cy - amp * std::sin(t * 2.0f * kPi);
    // The taper: 0 at both ends, 1 in the middle (§3).
    const float pressure = std::sin(t * kPi);
    // Through the SAME EMA the canvas and the brush sheet run pressure
    // through, including its per-stroke reset in `begin()` above -- otherwise
    // this preview would show a brush responding to pressure a shade more
    // sharply than the one the pen actually drives.
    stroke.setTip(brushTipFor(previewBrush, lut, stroke.smoothPressure(pressure)));
    stroke.addPoint(x, y);
  }
  stroke.end();

  img.dabs = stroke.dabCount();
  img.texels = stroke.texelsWritten();

  // Read the stroke back and box-filter it down by exactly `scale` (§4). Every
  // output texel is the mean of exactly `scale * scale` inputs -- the whole
  // reason the ratio is an integer.
  //
  // The mean is taken over the COMPOSITED colour rather than over mass and
  // latent separately, because averaging a latent is not a meaningful
  // operation: two texels carrying different pigments at half mass each
  // composite to a blend of both over paper, and averaging their latents would
  // invent a third pigment neither dab deposited.
  const PigmentTileStore& store = *od.document.layers[od.document.layers.size() - 1].pigmentTiles;
  const float inv = 1.0f / static_cast<float>(img.scale * img.scale);
  for (int py = 0; py < kStrokePreviewHeight; ++py) {
    for (int px = 0; px < kStrokePreviewWidth; ++px) {
      float acc[3] = {0.0f, 0.0f, 0.0f};
      for (int sy = 0; sy < img.scale; ++sy) {
        for (int sx = 0; sx < img.scale; ++sx) {
          const PixelCoord at{px * img.scale + sx, py * img.scale + sy};
          const PigmentTile* tile = store.find(tileCoordAt(at));
          const PigmentTexel t =
              tile != nullptr ? tile->readTexel(tileLocalOffset(at)) : PigmentTexel{};
          if (!(t.mass > 0.0f)) {
            acc[0] += paperLinear[0];
            acc[1] += paperLinear[1];
            acc[2] += paperLinear[2];
            continue;
          }
          // `core/Composite`'s own projection of a Pigment texel over a
          // ground, written as the straight-alpha lerp for `app/DabPreview`'s
          // stated reason: it is the same number as the premultiplied form and
          // it says what it means at a glance.
          const std::array<float, 3> pigment = latentToRgb(t.latent);
          const float a = t.mass > 1.0f ? 1.0f : t.mass;
          for (int ch = 0; ch < 3; ++ch)
            acc[ch] += pigment[static_cast<size_t>(ch)] * a + paperLinear[ch] * (1.0f - a);
        }
      }
      const size_t base = (static_cast<size_t>(py) * static_cast<size_t>(kStrokePreviewWidth) +
                           static_cast<size_t>(px)) *
                          4u;
      for (int ch = 0; ch < 3; ++ch)
        img.rgba[base + static_cast<size_t>(ch)] = toByte(acc[ch] * inv);
      img.rgba[base + 3] = 255u;
    }
  }
  return img;
}



StrokePreviewKey strokePreviewKeyFor(const BrushState& brush, const MixboxLut& lut) {
  StrokePreviewKey key;
  for (int i = 0; i < kStrokePreviewKeyPressures; ++i) {
    const float p =
        static_cast<float>(i) / static_cast<float>(kStrokePreviewKeyPressures - 1);
    key.tips[static_cast<size_t>(i)] = brushTipFor(brush, lut, p);
  }
  key.links = brush.links;
  return key;
}

bool strokePreviewKeysEqual(const StrokePreviewKey& a, const StrokePreviewKey& b) noexcept {
  for (size_t i = 0; i < a.tips.size(); ++i)
    if (!brushTipEqual(a.tips[i], b.tips[i])) return false;
  return linkSetsEqual(a.links, b.links);
}

const StrokePreviewImage& StrokePreviewCache::imageFor(const BrushState& brush,
                                                       const MixboxLut& lut) {
  const StrokePreviewKey key = strokePreviewKeyFor(brush, lut);
  if (haveKey_ && strokePreviewKeysEqual(key_, key)) {
    ++hits_;
    return image_;
  }
  key_ = key;
  haveKey_ = true;
  image_ = rasteriseStrokePreview(brush, lut);
  ++rasterisations_;
  ++generation_;
  return image_;
}

int runStrokePreviewDump(const char* outPath, float radiusOverride, float spacingOverride) {
  MixboxLut lut;
  const std::string lutPath = mixboxLutPath();
  if (!lut.load(lutPath)) {
    std::fprintf(stderr, "stroke-preview: could not load the Mixbox LUT at %s\n", lutPath.c_str());
    return 1;
  }

  BrushState brush;
  if (radiusOverride > 0.0f) brush.radius = radiusOverride;
  if (spacingOverride > 0.0f) brush.spacing = spacingOverride;

  const StrokePreviewImage img = rasteriseStrokePreview(brush, lut);
  std::printf(
      "stroke-preview: %dx%d at 1:%d  radius %.1f  spacing %.2f  dabs %zu  texels %zu%s%s\n",
      img.width, img.height, img.scale, static_cast<double>(brush.radius),
      static_cast<double>(brush.spacing), img.dabs, img.texels,
      img.refused ? "  REFUSED: " : "", img.refused ? img.refusal.c_str() : "");

  // Straight back out through the same RGBA bytes the panel uploads -- writing
  // the DISPLAYED image, not a re-render of it, so what this file shows is
  // exactly what the widget shows and not a second rasterisation that could
  // differ. `rasteriseStrokePreview()` already encoded them as sRGB (its own
  // `StrokePreviewImage` comment), so the export is told they are Rec709/sRGB
  // rather than being asked to encode them again.
  OpenDocument out = makeBlankOpenDocument(img.width, img.height, WorkingSpace{}, "stroke-preview");
  TileStore& tiles = *out.document.layers[0].rgbTiles;
  for (int y = 0; y < img.height; ++y) {
    for (int x = 0; x < img.width; ++x) {
      const size_t base = (static_cast<size_t>(y) * static_cast<size_t>(img.width) +
                           static_cast<size_t>(x)) * 4u;
      const PixelCoord p{x, y};
      tiles.getOrCreate(tileCoordAt(p))
          .writePixel(tileLocalOffset(p),
                      {srgbDecode(static_cast<float>(img.rgba[base + 0]) / 255.0f),
                       srgbDecode(static_cast<float>(img.rgba[base + 1]) / 255.0f),
                       srgbDecode(static_cast<float>(img.rgba[base + 2]) / 255.0f), 1.0f});
    }
  }
  std::string error;
  if (!exportDocumentToFile(out.document, outPath, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                            ExportBitDepth::UInt8, &error)) {
    std::fprintf(stderr, "stroke-preview: export failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("stroke-preview: wrote %s\n", outPath);
  return 0;
}

}  // namespace np
