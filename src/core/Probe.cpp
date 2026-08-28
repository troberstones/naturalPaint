#include "core/Probe.hpp"

#include <algorithm>
#include <optional>
#include <vector>

#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/Premultiply.hpp"

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
std::array<float, 4> sumPremultipliedBox(const TileStore& tiles, const ProbeBox& box) {
  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  for (int32_t y = box.y0; y < box.y1; ++y) {
    for (int32_t x = box.x0; x < box.x1; ++x) {
      const PixelCoord doc{x, y};
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

// The same box sum over a Pigment layer, each texel projected to premultiplied
// RGBA first (core/Composite's `projectPigmentTexel()`, called rather than
// re-derived). A missing tile contributes mass 0, which projects to
// {0,0,0,0} -- the same "no colour contributed" the RGB path relies on, so the
// averaging argument above carries over unchanged.
std::array<float, 4> sumPigmentBox(const PigmentTileStore& tiles, const ProbeBox& box) {
  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  for (int32_t y = box.y0; y < box.y1; ++y) {
    for (int32_t x = box.x0; x < box.x1; ++x) {
      const PixelCoord doc{x, y};
      const PigmentTile* tile = tiles.find(tileCoordAt(doc));
      if (tile == nullptr) continue;
      const std::array<float, 4> px =
          projectPigmentTexel(tile->readTexel(tileLocalOffset(doc)));
      sum[0] += px[0];
      sum[1] += px[1];
      sum[2] += px[2];
      sum[3] += px[3];
    }
  }
  return sum;
}

}  // namespace

const char* probeSampleSizeLabel(int32_t sampleSize) noexcept {
  for (int i = 0; i < kProbeSampleSizeCount; ++i)
    if (kProbeSampleSizes[i] == sampleSize) return kProbeSampleSizeLabels[i];
  return nullptr;
}

ProbeBox probeSampleBox(const Document& doc, PixelCoord at, int32_t sampleSize) noexcept {
  const int32_t size = sampleSize > 0 ? sampleSize : 1;
  const int32_t half = size / 2;  // floor; see ProbeParams::sampleSize's doc comment
  // Widened to int64 for the intersection only. `at` comes from a pointer
  // position the user can drag arbitrarily far off-canvas, and `size` is
  // caller-supplied: `at.x - half` and `at.x - half + size` are both
  // expressible outside int32 for extreme inputs, and signed overflow is UB
  // rather than a wrong answer. Clamping in int64 and narrowing afterwards
  // costs nothing (this runs once per sample, not per texel) and cannot trap.
  const int64_t lo = static_cast<int64_t>(at.x) - half;
  const int64_t hi = lo + size;
  const int64_t loY = static_cast<int64_t>(at.y) - half;
  const int64_t hiY = loY + size;
  const int64_t w = doc.width > 0 ? doc.width : 0;
  const int64_t h = doc.height > 0 ? doc.height : 0;
  ProbeBox box;
  box.x0 = static_cast<int32_t>(std::clamp<int64_t>(lo, 0, w));
  box.x1 = static_cast<int32_t>(std::clamp<int64_t>(hi, 0, w));
  box.y0 = static_cast<int32_t>(std::clamp<int64_t>(loY, 0, h));
  box.y1 = static_cast<int32_t>(std::clamp<int64_t>(hiY, 0, h));
  return box;
}

ProbeSample probePixel(const Document& doc, PixelCoord at, const ProbeParams& params) {
  const int32_t sampleSize = params.sampleSize > 0 ? params.sampleSize : 1;

  // **The sample box is CLIPPED TO THE DOCUMENT, and the divisor is the
  // number of texels actually inside it.**
  //
  // It used to be neither: every loop below walked the full
  // `sampleSize x sampleSize` box wherever it landed, and the divisor was an
  // unconditional `sampleSize * sampleSize`. Texels outside the canvas read
  // back {0,0,0,0} from a tile store that has nothing there, which is the
  // same value an *unpainted* in-document texel gives -- and that conflation
  // is the bug. Those two are not the same thing. An unpainted texel inside
  // the canvas is transparent paper and legitimately dilutes the sample's
  // coverage; a texel outside the canvas is not part of the image at all and
  // has no business being in the average.
  //
  // **Why it survived: the colour was always right, and only the alpha was
  // wrong.** Averaging in premultiplied space and un-premultiplying once at
  // the end (this file's own reasoning, above) divides the colour sum by the
  // alpha sum, and an all-zero texel subtracts from both in the same
  // proportion -- so the zeros cancel out of `rgb/a` exactly and the reported
  // RGB never moved. What moved was the reported alpha: an "11 by 11 Average"
  // taken one pixel in from the corner of a *fully opaque* document reported
  // alpha 6*6/121 = 0.30, and the same box taken two pixels further in
  // reported something different again. A user who never looks at the alpha
  // readout cannot see this, and a user who fills with the picked colour sees
  // a fill that is mysteriously transparent near the edges of the image.
  //
  // Photoshop clips its own sample box to the canvas for the same reason.
  // The one behaviour deliberately NOT changed: unpainted texels *inside* the
  // document still dilute coverage, exactly as before.
  const ProbeBox box = probeSampleBox(doc, at, sampleSize);
  const int32_t texels = box.texels();
  // No overlap with the document at all -- the coordinate is off-canvas and
  // the whole box with it. Transparent black, per the header, and an early
  // return rather than a divide by zero.
  if (texels <= 0) return ProbeSample{};
  const float count = static_cast<float>(texels);

  std::array<float, 4> sum{0.0f, 0.0f, 0.0f, 0.0f};
  bool any = false;

  // How much of the stack `AllLayers` and `ActiveAndBelow` walk. The two modes
  // share one walk and differ only in this bound: `ActiveAndBelow` stops
  // *after* the active layer (hence +1, inclusive), `AllLayers` runs to the
  // top. Written as one number rather than as two copies of the loop because
  // a second copy is how "and below" quietly becomes "all layers" -- the two
  // would agree on every single-layer document, which is most of them.
  const size_t layerCount = doc.layers.size();
  size_t topExclusive = layerCount;
  if (params.source == ProbeSource::ActiveAndBelow) {
    if (params.activeLayerIndex < 0) return ProbeSample{};  // no stack to truncate to
    const size_t active = static_cast<size_t>(params.activeLayerIndex);
    topExclusive = active < layerCount ? active + 1 : layerCount;
  }

  if (params.source == ProbeSource::AllLayers || params.source == ProbeSource::ActiveAndBelow) {
    // **Real `over` compositing, per texel, then averaged** (PLAN.md Phase 5
    // step 1). This used to be a plain sum across layers, and both this
    // function and io/Export's flattener carried the same note: "the moment a
    // second RGB layer can hold content, this loop is exactly where real
    // per-layer alpha-under compositing has to replace the plain sum". That
    // moment is this step, and both places now call the *same* `blendPixel()`
    // (core/Blend) rather than each growing an implementation that has to be
    // kept agreeing -- an eyedropper and an export that disagreed about the
    // colour of a pixel would be a bug the user could see and nobody could
    // explain.
    //
    // **Phase 5 step 2**: that now includes the blend *mode*. Each layer's
    // blend name is resolved to a `BlendMode` here exactly as
    // core/Composite's walk resolves it -- once per layer, with a name this
    // build cannot composite falling back to `Normal` -- so a document with a
    // `multiply` layer reads the same under the eyedropper as it exports. A
    // probe returns a colour and has nowhere to put the warning that
    // fallback deserves; that asymmetry is stated in core/Probe.hpp.
    //
    // Compositing happens **per texel and the box is averaged afterwards**,
    // not the other way round. Averaging each layer first and compositing the
    // averages is a different (and wrong) operation as soon as coverage varies
    // across the box: `over` is not linear in its operands, so the composite
    // of the averages is not the average of the composites.
    //
    // `visible` and `opacity` are honoured here exactly as they are in the
    // flattener, via `layerCoverage()`, because this mode's question is "what
    // colour does the document show at this point" -- the composite, not the
    // union of what the layers happen to hold.
    // **Phase 5 step 3**: Pigment layers, and `Mix`, are read here too. They
    // had to be: this function and io/Export's flattener are the two things
    // that turn a Document into colour, and a build where the eyedropper knew
    // nothing about Pigment layers would report a different colour from the
    // one it exports at the same pixel -- a bug a user can see and nobody can
    // explain. Every piece of arithmetic below is core/Composite's own
    // (`layerCoverage`, `layerPointOps`, `projectPigmentTexel`,
    // `mixedPairTexel`, `gradedPremultiplied`, `blendPixel`), called rather
    // than re-derived; only the *loop* differs, because a probe walks a small
    // box and the flattener walks occupied tiles.
    // Resolved once per layer, before the sample box, for the same reason
    // core/Composite hoists it: `blend` is a std::string, and parsing one per
    // texel per layer would put a string comparison in the innermost loop of
    // an operation that runs on every pointer move. Op stacks are hoisted for
    // a stronger reason -- an Op carrying a Curve copies a std::vector, and
    // core::layerPointOps() (core/Composite.hpp) now hands back raw `Op`
    // copies rather than `PointOp` closures (docs/architecture-review.md
    // P0-5) -- the copy cost this comment is about is unchanged either way.
    const MixPairing pairing = mixPairing(doc);
    // **Phase 5 step 9**: and which layer clips which, for the identical
    // reason -- an eyedropper that did not know about clipping would report a
    // colour the export does not produce. Every piece of arithmetic below is
    // core/Composite's own (`clipRuns`, `clipGroupOpen/Fold/Close`,
    // `adjustedPremultiplied`), called rather than re-derived; only the loop
    // differs, because a probe walks a small box and the flattener walks
    // occupied tiles.
    const ClipRuns clips = clipRuns(doc);
    std::vector<BlendMode> modes(doc.layers.size(), BlendMode::Normal);
    std::vector<std::vector<Op>> ops(doc.layers.size());
    std::vector<float> coverages(doc.layers.size(), 0.0f);
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      const Layer& layer = doc.layers[i];
      // `any` is "is there colour anywhere in the part of the stack this mode
      // looks at", so it is bounded by `topExclusive` too. A document whose
      // only painted layer sits *above* the active one composites to
      // transparent black either way -- the difference is that with the bound
      // here it takes the cheap `!any` exit instead of walking the box to
      // discover the same thing.
      if (i < topExclusive &&
          ((layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) ||
           (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value())))
        any = true;
      const std::optional<BlendMode> named = blendModeFromName(layer.blend);
      if (named.has_value() && blendModeInfo(*named).compositesPixels) modes[i] = *named;
      ops[i] = layerPointOps(layer.ops);
      // `layerCoverage(doc, i)`, not the single-`Layer` overload: a layer
      // inside a group must read the same coverage here that it composites
      // with, or the eyedropper reports a colour the export does not produce
      // -- the exact failure mode this file's own header already states for
      // every other piece of core/Composite's arithmetic it calls.
      coverages[i] = layerCoverage(doc, i);
    }
    // The clipping group layer `li` is the base of, folded at one texel --
    // core/Composite.hpp §13, through that file's own three bracket functions
    // rather than a second copy of them here. Returns `base` bit-identically
    // when no member contributes at this texel, which is the same lazy-open
    // rule the flattener follows and for the same reason.
    auto foldClipGroupAt = [&](std::array<float, 4> base, size_t li,
                               PixelCoord docPos) -> std::array<float, 4> {
      std::array<float, 4> g{};
      bool opened = false;
      for (const size_t mi : clips.members[li]) {
        // A clip group's members are all *above* its base (core/Composite
        // §12), so truncating the stack truncates the group. Without this
        // line an "and below" sample taken on a clip base would still be
        // tinted by the clipped layers stacked on top of it -- which is
        // exactly the contamination the mode exists to remove.
        if (mi >= topExclusive) continue;
        const Layer& m = doc.layers[mi];
        const float cov = coverages[mi] * layerMaskCoverageAt(m, docPos);
        if (cov <= 0.0f) continue;
        const bool adjustment = m.kind == LayerKind::Adjustment;
        std::array<float, 4> src{};
        if (adjustment) {
          if (ops[mi].empty()) continue;  // an empty stack costs exactly nothing (§10)
        } else if (m.kind == LayerKind::RGB && m.rgbTiles.has_value()) {
          const Tile* t = m.rgbTiles->find(tileCoordAt(docPos));
          if (t == nullptr) continue;
          src = t->readPixel(tileLocalOffset(docPos));
        } else if (m.kind == LayerKind::Pigment && m.pigmentTiles.has_value()) {
          const PigmentTile* t = m.pigmentTiles->find(tileCoordAt(docPos));
          if (t == nullptr) continue;
          src = projectPigmentTexel(t->readTexel(tileLocalOffset(docPos)));
        } else {
          continue;  // a kind that holds no pixels contributes nothing here either
        }
        if (!opened) {
          if (!(base[3] > 0.0f)) return base;  // no coverage to clip to: PRD C9, literally
          g = clipGroupOpen(base);
          opened = true;
        }
        g = adjustment ? adjustedPremultiplied(g, ops[mi], cov)
                       : clipGroupFold(g, modes[mi], src, ops[mi], cov);
      }
      return opened ? clipGroupClose(g, base[3]) : base;
    };

    if (any) {
      for (int32_t py = box.y0; py < box.y1; ++py) {
        for (int32_t px = box.x0; px < box.x1; ++px) {
          const PixelCoord docPos{px, py};
          std::array<float, 4> acc{0.0f, 0.0f, 0.0f, 0.0f};
          for (size_t li = 0; li < topExclusive; ++li) {
            const Layer& layer = doc.layers[li];
            // The lower half of a mixed pair is composited by the pair --
            // **unless the pair's upper half is above the truncation**, in
            // which case there is no pair in this mode's stack and the lower
            // half has to composite on its own. `mixedWithBelow[li]` is only
            // ever true for `li-1`'s partner at `li`, so the upper half of the
            // pair `li` is consumed by is exactly `li + 1`.
            if (pairing.consumedByAbove[li] && li + 1 < topExclusive) continue;
            // **Phase 5 step 9**: and a clipped layer is composited by its
            // base, as part of that base's group -- never on its own.
            if (clips.clippedToBase[li]) continue;

            // **Phase 5 step 5**: an Adjustment layer transforms `acc` -- the
            // composite accumulated beneath it -- instead of contributing to
            // it, through core/Composite's own `adjustedPremultiplied()`
            // rather than a second copy of the lerp. It deliberately does not
            // set `any`: an adjustment layer holds no colour, so a document of
            // nothing but adjustment layers still probes as transparent black.
            if (layer.kind == LayerKind::Adjustment) {
              acc = adjustedPremultiplied(
                  acc, ops[li], coverages[li] * layerMaskCoverageAt(layer, docPos));
              continue;
            }

            if (pairing.mixedWithBelow[li]) {
              const Layer& lower = doc.layers[li - 1];
              // **Phase 5 step 4**: each half of the pair modulates its own
              // coverage by its own mask, exactly as core/Composite's walk
              // does, through the same `layerMaskCoverageAt()`. The mixing
              // weight is still the upper texel's mass and is untouched.
              const float covLow = coverages[li - 1] * layerMaskCoverageAt(lower, docPos);
              const float covUp = coverages[li] * layerMaskCoverageAt(layer, docPos);
              const PigmentTile* up = layer.pigmentTiles.has_value()
                                          ? layer.pigmentTiles->find(tileCoordAt(docPos))
                                          : nullptr;
              const PigmentTile* low =
                  (lower.kind == LayerKind::Pigment && lower.pigmentTiles.has_value())
                      ? lower.pigmentTiles->find(tileCoordAt(docPos))
                      : nullptr;
              if (up == nullptr && low == nullptr) continue;
              const PixelCoord local = tileLocalOffset(docPos);
              std::array<float, 4> pair =
                  mixedPairTexel(low ? low->readTexel(local) : PigmentTexel{}, ops[li - 1], covLow,
                                 up ? up->readTexel(local) : PigmentTexel{}, ops[li], covUp);
              // A mixed pair is one unit, and one unit is what a clip base is.
              if (!clips.members[li].empty()) pair = foldClipGroupAt(pair, li, docPos);
              acc = blendPixel(BlendMode::Normal, pair, acc);
              continue;
            }

            // A mask is per-texel opacity, so it multiplies the layer's
            // coverage here for the same reason `layerCoverage()` does -- and
            // the `<= 0` skip below is the flattener's own, so a texel a mask
            // hides outright contributes exactly nothing rather than a
            // multiply by zero. At an unmasked layer this is a multiplication
            // by literal 1.0f, exact for every finite float.
            const float coverage = coverages[li] * layerMaskCoverageAt(layer, docPos);
            if (coverage <= 0.0f) continue;
            std::array<float, 4> src;
            if (layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) {
              const Tile* tile = layer.rgbTiles->find(tileCoordAt(docPos));
              if (tile == nullptr) continue;  // exactly equivalent to compositing {0,0,0,0}
              src = tile->readPixel(tileLocalOffset(docPos));
            } else if (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value()) {
              const PigmentTile* tile = layer.pigmentTiles->find(tileCoordAt(docPos));
              if (tile == nullptr) continue;
              src = projectPigmentTexel(tile->readTexel(tileLocalOffset(docPos)));
            } else {
              continue;
            }
            src = gradedPremultiplied(src, ops[li]);
            if (coverage != 1.0f) {
              src[0] *= coverage;
              src[1] *= coverage;
              src[2] *= coverage;
              src[3] *= coverage;
            }
            // The clipping group this layer is the base of, if any. Empty for
            // every layer of a document with no clipped layers, so the probe's
            // pre-step-9 arithmetic is untouched there.
            if (!clips.members[li].empty()) src = foldClipGroupAt(src, li, docPos);
            acc = blendPixel(modes[li], src, acc);
          }
          sum[0] += acc[0];
          sum[1] += acc[1];
          sum[2] += acc[2];
          sum[3] += acc[3];
        }
      }
    }
  } else if (params.source == ProbeSource::CurrentLayer && params.activeLayerIndex >= 0 &&
             static_cast<size_t>(params.activeLayerIndex) < doc.layers.size()) {
    // Single-layer mode reads that layer's **own** stored colour, deliberately
    // ignoring its `visible`, its `opacity` **and its mask** -- the mask joins
    // that list rather than being an exception to it, because a mask is
    // per-texel opacity (core/Composite.hpp §5) and this mode's question is
    // what the layer holds, not what the document shows. Probing a masked-out
    // texel therefore reports the colour under the mask, which is exactly what
    // makes an eyedropper usable for checking what a mask is hiding. The
    // question this mode asks is
    // "what is on this layer", not "what does the document show" -- so a
    // half-opacity layer still probes at its authored colour, and a hidden
    // layer can still be probed at all. That is Photoshop's own split between
    // "Sample: Current Layer" and "All Layers", and the alternative (returning
    // transparent black for a hidden layer) would make the eyedropper useless
    // for exactly the workflow that has a layer hidden while working on it.
    const Layer& layer = doc.layers[static_cast<size_t>(params.activeLayerIndex)];
    if (layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) {
      sum = sumPremultipliedBox(*layer.rgbTiles, box);
      any = true;
    } else if (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value()) {
      // The same box sum over the layer's own projected colour. It deliberately
      // ignores the layer's own op stack as well as its visible/opacity, for
      // the reason this branch already gives: the question is "what is on this
      // layer", and a Pigment layer holds latents, so the honest answer is the
      // projection of what is stored -- not the graded, faded version of it the
      // document happens to show. The latents themselves have no RGBA
      // representation to return through `ProbeSample`; a pigment-aware probe
      // that reported `c0..c2` is a UI feature nothing asks for yet.
      sum = sumPigmentBox(*layer.pigmentTiles, box);
      any = true;
    }
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
