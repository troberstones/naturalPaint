#include "app/DabPreview.hpp"

#include <algorithm>
#include <cmath>

#include "app/StrokeSession.hpp"
#include "color/Space.hpp"
#include "core/Pigment.hpp"

namespace np {

namespace {

// The paper the dab is composited over: `ui/AtelierTheme.hpp`'s `kCanvasPaper`
// (0xf8f4f4), as three floats.
//
// **Spelled out here rather than included**, for `brushTipFor()`'s reason:
// `app/` does not include `ui/` -- the dependency runs the other way -- and
// this module has to stay headlessly testable besides. `--selftest` asserts
// the two agree, which is the guard that spelling a constant twice needs; a
// duplicated colour with no such assertion is how a theme change leaves one
// panel a shade off for a year.
constexpr float kPaperSrgb[3] = {0xf8 / 255.0f, 0xf4 / 255.0f, 0xf4 / 255.0f};

uint8_t toByte(float linear) noexcept {
  // `latentToRgb()` can leave the [0,1] box -- core/Pigment.hpp says so
  // outright, and calls a value above 1 "a pigment mixture outside the model's
  // domain" rather than a highlight. There is no byte for it, so it is
  // clamped, here, at the display boundary and nowhere earlier: clamping the
  // *latent* would change what the preview says the deposit does.
  const float e = srgbEncode(std::clamp(linear, 0.0f, 1.0f));
  const float scaled = e * 255.0f + 0.5f;
  return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

}  // namespace

float dabPreviewScale(float largestRadius) noexcept {
  // Written as a negated comparison so a NaN radius -- which nothing should
  // produce, but which a link with a degenerate curve could -- takes the 1:1
  // branch rather than propagating into every texel offset in the image.
  if (!(largestRadius > kDabPreviewFitRadius)) return 1.0f;
  return largestRadius / kDabPreviewFitRadius;
}

DabPreviewOffset dabPreviewOffset(float scale, int cell, int px, int py) noexcept {
  constexpr float kHalf = static_cast<float>(kDabPreviewCell) * 0.5f;
  const float centreX = static_cast<float>(cell) * static_cast<float>(kDabPreviewCell) + kHalf;
  DabPreviewOffset o;
  // Texel `(px, py)` is sampled at its centre, `(px + 0.5, py + 0.5)`, which
  // is `dabPixelBounds()`'s convention and the reason the two halves of a
  // round tip are exact mirrors of each other here: the dab centre sits on a
  // texel *boundary*, so texel 31 is at -0.5 and texel 32 at +0.5, and float
  // negation is exact.
  o.dx = (static_cast<float>(px) + 0.5f - centreX) * scale;
  o.dy = (static_cast<float>(py) + 0.5f - kHalf) * scale;
  return o;
}

float dabPreviewCoverageAt(const BrushTip& tip, float scale, int cell, int px,
                           int py) noexcept {
  const DabPreviewOffset o = dabPreviewOffset(scale, cell, px, py);
  // The deposit's own function, and the only place coverage comes from in this
  // module. brush/Deposit.hpp's declaration says why there is no second one.
  return dabCoverage(tip, o.dx, o.dy);
}

PigmentTexel dabPreviewTexel(const BrushTip& tip, float scale, int cell, int px,
                             int py) noexcept {
  // These three lines are `depositDab()`'s inner loop, in its order and with
  // its guard -- the product tested, not the coverage. `depositDab()` skips a
  // texel it would deposit nothing into rather than writing it, which is what
  // keeps a dab's reported footprint equal to what it changed
  // (brush/Deposit.hpp §3); testing `coverage > 0` instead would diverge for a
  // `flow` of 0, where the deposit writes nothing at all and this would return
  // a mass-0 texel carrying the brush's latent (§1(ii)'s limit case). The two
  // are indistinguishable once rasterised -- mass 0 draws as paper either way
  // -- and they are NOT indistinguishable to the assertion that compares this
  // against a real deposited tile, which is the assertion that matters.
  //
  // **`grainCoverageAt(tip.grain, ..., px, py)` -- the same call
  // `depositDab()`'s own loop makes, at the same point, on `(px, py)` playing
  // the absolute coordinate a real deposit's `(x, y)` would be.**
  // brush/Deposit.hpp §2e states the convention this stands on: this preview
  // has no `Document` and therefore no true absolute position, so it treats
  // its own preview-texel grid as if it WERE the document's, with this cell's
  // top-left at document (0,0). That is what makes the identity below
  // provable rather than merely plausible: build a real `PigmentTileStore`,
  // `depositDab()` one dab into a canvas sized `kDabPreviewCell x
  // kDabPreviewCell` centred at this cell's own dab centre, and its texel
  // `(x, y)` is the bit-identical integer pair as this function's `(px, py)`
  // for cell 0 -- so grain, keyed on that integer pair in both places, agrees
  // too. `--selftest`'s grain section builds exactly that pair and compares.
  const float cov = grainCoverageAt(tip.grain, dabPreviewCoverageAt(tip, scale, cell, px, py),
                                    px, py);
  const float deltaMass = tip.flow * cov;
  if (!(deltaMass > 0.0f)) return PigmentTexel{};
  return depositTexel(PigmentTexel{}, tip.pigment, deltaMass);
}

DabPreviewImage rasteriseDabPreview(const std::array<BrushTip, kDabPreviewCells>& tips) {
  DabPreviewImage img;
  img.width = kDabPreviewWidth;
  img.height = kDabPreviewHeight;
  img.rgba.assign(static_cast<size_t>(kDabPreviewWidth) *
                      static_cast<size_t>(kDabPreviewHeight) * 4u,
                  0u);

  // §3: ONE scale, from the largest of the three, so the three cells are
  // comparable. Deriving it per cell would make a PRESSURE -> SIZE link
  // invisible -- every cell would draw a dab of the same on-screen size --
  // which is the exact question the three cells exist to answer.
  float largest = 0.0f;
  for (int c = 0; c < kDabPreviewCells; ++c) {
    img.radii[static_cast<size_t>(c)] = tips[static_cast<size_t>(c)].radius;
    largest = std::max(largest, tips[static_cast<size_t>(c)].radius);
  }
  img.scale = dabPreviewScale(largest);

  const float paperLinear[3] = {srgbDecode(kPaperSrgb[0]), srgbDecode(kPaperSrgb[1]),
                                srgbDecode(kPaperSrgb[2])};
  const uint8_t paperByte[3] = {toByte(paperLinear[0]), toByte(paperLinear[1]),
                                toByte(paperLinear[2])};

  for (int cell = 0; cell < kDabPreviewCells; ++cell) {
    const BrushTip& tip = tips[static_cast<size_t>(cell)];
    for (int py = 0; py < kDabPreviewHeight; ++py) {
      for (int lx = 0; lx < kDabPreviewCell; ++lx) {
        const int px = cell * kDabPreviewCell + lx;
        const size_t base = (static_cast<size_t>(py) * static_cast<size_t>(kDabPreviewWidth) +
                             static_cast<size_t>(px)) *
                            4u;
        img.rgba[base + 3] = 255u;  // opaque: the ground is paper, not the panel

        const PigmentTexel t = dabPreviewTexel(tip, img.scale, cell, px, py);
        if (!(t.mass > 0.0f)) {
          img.rgba[base + 0] = paperByte[0];
          img.rgba[base + 1] = paperByte[1];
          img.rgba[base + 2] = paperByte[2];
          continue;
        }

        // `core/Composite` projects a Pigment texel as
        // `(latentToRgb(latent) * mass, mass)` -- premultiplied -- and this is
        // that projection composited `over` paper. Written as the straight-
        // alpha lerp rather than as `premul + dst*(1-a)` because the two are
        // the same number and the lerp says what it means at a glance; the
        // premultiplied form is core/Composite's because that is the form its
        // *storage* is in, which this has no equivalent of.
        const std::array<float, 3> pigment = latentToRgb(t.latent);
        for (int ch = 0; ch < 3; ++ch)
          img.rgba[base + static_cast<size_t>(ch)] =
              toByte(pigment[static_cast<size_t>(ch)] * t.mass +
                     paperLinear[ch] * (1.0f - t.mass));
      }
    }
  }
  return img;
}

std::array<BrushTip, kDabPreviewCells> dabPreviewTipsFor(const BrushState& brush,
                                                         const MixboxLut& lut,
                                                         const DynamicInputs& live) {
  std::array<BrushTip, kDabPreviewCells> tips{};
  for (size_t i = 0; i < kDabPreviewCells; ++i) {
    // The live sample with ONE field replaced (§2): a tip whose roundness is
    // driven by TILT must preview at the tilt the pen is actually at, or the
    // preview is answering a question about a pen nobody is holding.
    DynamicInputs in = live;
    in.pressure = kDabPreviewPressures[i];
    tips[i] = brushTipFor(brush, lut, in);
  }
  return tips;
}

bool dabPreviewTipsEqual(const BrushTip& a, const BrushTip& b) noexcept {
  // `bitmap` compares by POINTER (shared_ptr's own `==`), not by pixel
  // content -- consistent with every other field here being bit equality
  // rather than a tolerance. Two different `BrushTipBitmap`s can otherwise
  // share every scalar checked below (radius, hardness, roundness, angle and
  // even pigment happening to match across two different sampled-tip
  // presets), and without this the cache would hand back one brush's dab
  // image for another's -- the exact failure `generation()`'s own doc comment
  // says this class exists to make impossible.
  //
  // `dualTip` compares by pointer for the identical reason, and `dualBlend`
  // is checked alongside it rather than assumed to move in lockstep: two
  // presets could in principle share one second tip but combine it by a
  // different `BlnM`, and that changes what `dabCoverage()` draws (§2d) even
  // though `dualTip`'s own pointer is unchanged.
  //
  // `grain` is checked too, by value (`grainParamsEqual()`, brush/Grain.hpp)
  // -- `dabPreviewTexel()` now reads it (brush/Deposit.hpp §2e), so a GRAIN
  // slider moved with every other field unchanged must still redraw. Missing
  // this would be the identical failure `bitmap`'s own comment describes:
  // the cache handing back a picture for the paper this brush no longer has.
  return a.radius == b.radius && a.hardness == b.hardness && a.flow == b.flow &&
         a.roundness == b.roundness && a.angle == b.angle && a.pigment == b.pigment &&
         a.bitmap == b.bitmap && a.dualTip == b.dualTip && a.dualBlend == b.dualBlend &&
         grainParamsEqual(a.grain, b.grain);
}

const DabPreviewImage& DabPreviewCache::imageFor(
    const std::array<BrushTip, kDabPreviewCells>& tips) {
  if (haveKey_) {
    bool same = true;
    for (size_t i = 0; i < kDabPreviewCells && same; ++i)
      same = dabPreviewTipsEqual(key_[i], tips[i]);
    if (same) {
      ++hits_;
      return image_;
    }
  }
  image_ = rasteriseDabPreview(tips);
  key_ = tips;
  haveKey_ = true;
  ++rasterisations_;
  ++generation_;
  return image_;
}

}  // namespace np
