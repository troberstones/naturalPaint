#include "core/Composite.hpp"

#include <optional>
#include <unordered_set>

#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

namespace np {
namespace {

// Whether a layer has an alpha of its own for something to be clipped by --
// i.e. whether the walk would ever composite a texel *from* it. Exactly the
// test `compositeDocumentPremultiplied()` makes before it walks a layer's
// tiles, kept in one place so a clip base and a compositable layer cannot
// become two different ideas. An Adjustment layer is false here by
// construction (it holds no tiles at all), which is core/Composite.hpp §12's
// "the alpha of the layer below is not a quantity such a layer has".
bool layerHoldsPixels(const Layer& layer) noexcept {
  return (layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) ||
         (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value());
}

}  // namespace

float layerMaskCoverageAt(const Layer& layer, PixelCoord at) noexcept {
  if (!layer.mask.has_value()) return 1.0f;
  return maskCoverage(layer.mask->find(tileCoordAt(at)), tileLocalOffset(at));
}

float layerCoverage(const Layer& layer) noexcept {
  if (!layer.visible) return 0.0f;
  const float o = layer.opacity;
  if (!(o > 0.0f)) return 0.0f;  // also catches NaN
  return o < 1.0f ? o : 1.0f;
}

std::vector<PointOp> layerPointOps(const OpStack& ops) {
  std::vector<PointOp> out;
  // detectRuns() has already dropped disabled entries and split at non-PointA
  // boundaries; flattening the runs in order is the whole conversion. Only
  // PointA has an implementation anywhere in this codebase (core/OpStack.hpp),
  // so a SpatialB/StrokeC/BakedD entry is simply absent from every run and
  // therefore from this list.
  for (const OpRun& run : ops.detectRuns())
    out.insert(out.end(), run.ops.begin(), run.ops.end());
  return out;
}

std::array<float, 4> gradedPremultiplied(const std::array<float, 4>& premultiplied,
                                         const std::vector<PointOp>& ops) {
  // The early return is the correctness-relevant line, not an optimisation:
  // applyPointOpsPremultiplied() divides by alpha and multiplies back, which
  // is two correctly-rounded operations and therefore NOT the identity, and
  // PLAN.md step 1's regression boundary (an unchanged document composites
  // byte-identically to the plain sum it replaced) is asserted at zero
  // tolerance. Every layer with no op stack must cost exactly nothing.
  if (ops.empty()) return premultiplied;
  return applyPointOpsPremultiplied(premultiplied, ops);
}

std::array<float, 4> projectPigmentTexel(const PigmentTexel& texel) noexcept {
  const std::array<float, 3> rgb = latentToRgb(texel.latent);
  const float m = texel.mass;
  return {rgb[0] * m, rgb[1] * m, rgb[2] * m, m};
}

std::array<float, 4> mixedPairTexel(const PigmentTexel& lower,
                                    const std::vector<PointOp>& lowerOps, float lowerCoverage,
                                    const PigmentTexel& upper,
                                    const std::vector<PointOp>& upperOps, float upperCoverage) {
  // The three projections, each graded by the stacks that apply to it. The
  // mixed one carries both, in stack order (bottom first), because it is the
  // single projection standing in for both layers.
  const std::array<float, 4> pLow = gradedPremultiplied(projectPigmentTexel(lower), lowerOps);
  const std::array<float, 4> pUp = gradedPremultiplied(projectPigmentTexel(upper), upperOps);

  // The mix itself. `t` is the UPPER layer's mass -- never its opacity; see
  // core/Composite.hpp §3 on why that distinction is the whole of PRD C3 here.
  PigmentTexel mixed;
  mixed.latent = mixLatents(lower.latent, upper.latent, upper.mass);
  // Coverage unions exactly as `over` does it, which is what keeps a mixed
  // pair's alpha the same alpha any other blend mode would have produced
  // (core/Blend.hpp: "alpha is `over`'s under every mode").
  mixed.mass = upper.mass + lower.mass * (1.0f - upper.mass);
  std::array<float, 4> pMix =
      gradedPremultiplied(gradedPremultiplied(projectPigmentTexel(mixed), lowerOps), upperOps);

  // Each layer independently participates or does not, weighted by its own
  // coverage. The three non-zero combinations, and the fourth (neither
  // participates) contributes nothing.
  const float both = lowerCoverage * upperCoverage;
  const float lowOnly = lowerCoverage * (1.0f - upperCoverage);
  const float upOnly = (1.0f - lowerCoverage) * upperCoverage;
  std::array<float, 4> out{};
  for (int i = 0; i < 4; ++i) out[i] = both * pMix[i] + lowOnly * pLow[i] + upOnly * pUp[i];
  return out;
}

std::array<float, 4> adjustedPremultiplied(const std::array<float, 4>& below,
                                           const std::vector<PointOp>& ops,
                                           float effectiveCoverage) {
  // Three ways an adjustment layer does nothing at this texel, each returning
  // `below` unchanged rather than computing something that happens to equal
  // it. See core/Composite.hpp §§9-10 for why each has to be bit-exact:
  //   * no ops -- the same rule §1 applies to every other kind's stack;
  //   * no coverage -- a hidden layer, opacity 0, or a mask sample of 0;
  //   * nothing beneath -- there is no colour to grade, and this is also what
  //     keeps applyPointOpsPremultiplied()'s `a <= 0` guard out of this path
  //     rather than making a copy of it here.
  if (ops.empty() || !(effectiveCoverage > 0.0f) || !(below[3] > 0.0f)) return below;

  const std::array<float, 4> graded = gradedPremultiplied(below, ops);

  // Alpha is never written: a grade is a colour operation and coverage is not
  // colour (core/Composite.hpp §11). Assigning `below[3]` rather than
  // `graded[3]` makes that true by construction rather than by trusting
  // ops/PointOps' contract from a distance -- the two are equal, and only one
  // of them stays equal if a future op ever breaks that contract.
  std::array<float, 4> out{below[0], below[1], below[2], below[3]};
  if (effectiveCoverage >= 1.0f) {
    // Assigned, not lerped: `below + 1.0f*(graded - below)` is two rounded
    // operations and is not `graded`.
    out[0] = graded[0];
    out[1] = graded[1];
    out[2] = graded[2];
    return out;
  }
  for (int i = 0; i < 3; ++i) out[i] = below[i] + effectiveCoverage * (graded[i] - below[i]);
  return out;
}

ClipRuns clipRuns(const Document& doc) {
  ClipRuns runs;
  const size_t n = doc.layers.size();
  runs.members.assign(n, {});
  runs.clippedToBase.assign(n, false);
  runs.clippedWithoutBase.assign(n, false);

  // **The three lines that make a run clip to ONE base.** `base` advances only
  // when a layer is *not* clipped, so every layer of a consecutive clipped run
  // sees the same base and no clipped layer is ever anybody's base. The
  // cumulative reading -- each clipped layer clipping to the one below it --
  // is the one this loop is shaped to rule out; core/Composite.hpp §12 says
  // why it is worth ruling out explicitly.
  size_t base = n;  // n means "nothing below is eligible"
  bool baseHoldsPixels = false;
  for (size_t i = 0; i < n; ++i) {
    const Layer& layer = doc.layers[i];
    if (!layer.clipped) {
      base = i;
      baseHoldsPixels = layerHoldsPixels(layer);
      continue;
    }
    runs.any = true;
    if (base == n || !baseHoldsPixels) {
      runs.clippedWithoutBase[i] = true;
      continue;
    }
    runs.clippedToBase[i] = true;
    runs.members[base].push_back(i);
  }
  return runs;
}

std::array<float, 4> clipGroupOpen(const std::array<float, 4>& basePremultiplied) noexcept {
  const float a = basePremultiplied[3];
  if (!(a > 0.0f)) return {0.0f, 0.0f, 0.0f, 0.0f};  // also catches NaN
  // The base's straight colour, considered opaque. Alpha is the literal 1.0f
  // rather than `a / a` so the invariant is a constant and not a rounding.
  return {basePremultiplied[0] / a, basePremultiplied[1] / a, basePremultiplied[2] / a, 1.0f};
}

std::array<float, 4> clipGroupFold(const std::array<float, 4>& group, BlendMode mode,
                                   std::array<float, 4> src, const std::vector<PointOp>& ops,
                                   float effectiveCoverage) {
  // The same two lines every other kind's source goes through -- grade, then
  // scale by one coverage scalar -- so a clipped layer and an unclipped one
  // differ in what they are blended *against*, never in how they are prepared.
  src = gradedPremultiplied(src, ops);
  if (effectiveCoverage != 1.0f) {
    src[0] *= effectiveCoverage;
    src[1] *= effectiveCoverage;
    src[2] *= effectiveCoverage;
    src[3] *= effectiveCoverage;
  }
  std::array<float, 4> out = blendPixel(mode, src, group);
  // Assigned, not computed. `as + 1*(1 - as)` is two roundings and is not
  // exactly 1 for every `as`; a clipping group's coverage has to *be* the
  // base's, not round to it (core/Composite.hpp §13).
  out[3] = 1.0f;
  return out;
}

std::array<float, 4> clipGroupClose(const std::array<float, 4>& group, float baseAlpha) noexcept {
  return {group[0] * baseAlpha, group[1] * baseAlpha, group[2] * baseAlpha, baseAlpha};
}

std::string clippedLayerWithoutBaseWarning(const Document& doc, size_t layerIndex) {
  std::string s = "layer " + std::to_string(layerIndex);
  if (layerIndex < doc.layers.size() && !doc.layers[layerIndex].name.empty())
    s += " (\"" + doc.layers[layerIndex].name + "\")";
  s += " asks to be clipped, but there is nothing beneath it to clip to. ";

  // Which of the three reasons applies, because what a user should do about
  // them differs: un-clip the layer, un-clip the run below it, or put a layer
  // that actually holds pixels under it.
  if (layerIndex == 0) {
    s += "It is the bottom layer of a " + std::to_string(doc.layers.size()) +
         "-layer stack, and PRD C9 clips a layer by \"the alpha of the layer below it\" -- at "
         "index 0 there is no layer below. ";
  } else {
    // The nearest non-clipped layer below, which is what `clipRuns()` looked
    // for and did not accept.
    size_t j = layerIndex;
    while (j > 0 && doc.layers[j - 1].clipped) --j;
    if (j == 0) {
      s += "Every layer beneath it is clipped as well, down to layer 0, so the whole run has "
           "no base -- a clipped layer is never another clipped layer's base (core/Composite.hpp "
           "§12), which is what keeps a run of them clipping to one alpha instead of eroding "
           "each other. ";
    } else {
      const Layer& below = doc.layers[j - 1];
      s += "The nearest layer below it that is not itself clipped is layer " +
           std::to_string(j - 1) + ", a " + std::string(layerKindName(below.kind)) +
           " layer, which holds no pixels of its own -- so it has no alpha for this layer to be "
           "clipped by. Clipping is not resolved by searching further down, because that would "
           "clip to something other than the layer below. ";
    }
  }

  s += "It was composited **unclipped** instead, so the composite is an approximation of what "
       "the layer stack means -- but every texel the layer holds is still in it. Dropping the "
       "layer would let one bit of metadata be the thing that makes a layer's pixels vanish. "
       "The flag itself is preserved exactly and is written back unchanged (PRD I10).";
  return s;
}

std::string adjustmentLayerBlendWarning(size_t layerIndex, const Layer& layer) {
  std::string s = "layer " + std::to_string(layerIndex);
  if (!layer.name.empty()) s += " (\"" + layer.name + "\")";
  s += " is an Adjustment layer carrying blend mode \"" + layer.blend +
       "\", which this build does not apply. A blend mode combines a source texel with the "
       "backdrop, and an Adjustment layer has no source of its own -- its op stack transforms "
       "the composite below it (PRD C5), so the operand that would play the source is the "
       "backdrop itself. Its result was taken over the composite below at the layer's own "
       "opacity and mask instead, as \"";
  s += kDefaultBlendName;
  s += "\" does. The value itself is preserved exactly and is written back unchanged (PRD I10).";
  return s;
}

std::string unimplementedBlendWarning(size_t layerIndex, const Layer& layer,
                                      const std::string& mixReason) {
  std::string s = "layer " + std::to_string(layerIndex);
  if (!layer.name.empty()) s += " (\"" + layer.name + "\")";
  s += " asks for blend mode \"" + layer.blend + "\", which this build cannot composite. ";

  // The two cases are worth distinguishing in the sentence, because what a
  // user should do about them differs: an unknown name came from a newer
  // build and means "this build is behind the document", while a misplaced
  // `mix` means the document asks for a mix where PRD L5 says one is not
  // defined, and the fix is a layer arrangement rather than a newer build.
  if (blendModeFromName(layer.blend).has_value()) {
    s += "`Mix` is a Kubelka-Munk lerp between two pigment latents, so PRD L5 defines it only "
         "between a Pigment layer and the Pigment layer directly beneath it, and each layer "
         "can be the lower half of at most one mixed pair. Here, " +
         (mixReason.empty() ? std::string("that does not hold") : mixReason) + ". ";
  } else {
    s += "It is not one of the modes core/Blend implements (";
    bool first = true;
    for (const BlendModeInfo& info : allBlendModes()) {
      if (!info.compositesPixels) continue;
      if (!first) s += ", ";
      s += info.name;
      first = false;
    }
    s += "), so it most likely comes from a newer build. ";
  }

  s += "It was composited as \"";
  s += kDefaultBlendName;
  s += "\" (source-over) instead, so the composite is an approximation of what the layer stack "
       "means. The value itself is preserved exactly and is written back unchanged (PRD I10).";
  return s;
}

std::vector<float> compositeDocumentPremultiplied(const Document& doc,
                                                  std::vector<std::string>* warningsOut) {
  if (doc.width <= 0 || doc.height <= 0) return {};

  const size_t w = static_cast<size_t>(doc.width);
  const size_t h = static_cast<size_t>(doc.height);

  // Zero-filled: an untouched pixel is transparent black, exactly what
  // core::Tile gives an unwritten texel, so nothing needs a separate "was
  // anything here" flag -- and, per core/Blend.hpp's transparent-backdrop
  // identity, an all-zero accumulator composites the first contributing layer
  // through unchanged **under every mode**, not only under `over`.
  std::vector<float> out(w * h * 4, 0.0f);

  // Which layers form a mixed pair, resolved once for the document (PRD L5,
  // core/Blend's `mixPairing()`), so this walk, `blendIsImplementedForLayer()`
  // and core/Probe cannot disagree about it.
  const MixPairing pairing = mixPairing(doc);

  // Which layer clips which, resolved once for the document for the identical
  // reason (PRD C9, this file's header §12). A document with no clipped layer
  // pays three empty vectors and no tile access at all, and every branch below
  // that mentions clipping is then not taken -- which is what makes step 1's
  // byte-identity boundary survive this step.
  const ClipRuns clips = clipRuns(doc);

  // **The blend-name warning, in one place**, because as of this step there are
  // two callers: the walk's own per-layer iteration, and a clipping base
  // reporting for the members it composites on their behalf. Without the
  // second, a clipped layer would be the one kind of layer whose unhonourable
  // blend went unreported -- silently, which is the thing §7 exists to forbid.
  auto warnUnimplementedBlend = [&](size_t li) {
    if (warningsOut == nullptr) return;
    const Layer& l = doc.layers[li];
    const std::optional<BlendMode> resolved = blendModeFromName(l.blend);
    const bool implementedHere =
        resolved.has_value() && (blendModeInfo(*resolved).compositesPixels ||
                                 (blendModeInfo(*resolved).compositesLatents &&
                                  pairing.mixedWithBelow[li]));
    if (implementedHere) return;
    std::string mixReason;
    if (resolved == BlendMode::Mix) {
      if (l.kind != LayerKind::Pigment)
        mixReason =
            "this is a " + std::string(layerKindName(l.kind)) + " layer, not a Pigment layer";
      else if (li == 0)
        mixReason = "it is the bottom layer, so there is nothing beneath it to mix with";
      else if (doc.layers[li - 1].kind != LayerKind::Pigment)
        mixReason = "the layer beneath it is a " +
                    std::string(layerKindName(doc.layers[li - 1].kind)) +
                    " layer, not a Pigment layer";
      else if (l.clipped)
        mixReason = "it is clipped, and a clip and a mix are two different relationships with "
                    "the same neighbour -- a mix composites the two layers as one unit, while a "
                    "clip makes the lower one the alpha that decides where the upper one shows "
                    "(PRD C9). No pair forms when either half is clipped";
      else if (doc.layers[li - 1].clipped)
        mixReason = "the Pigment layer beneath it is clipped, so it belongs to a clipping run "
                    "whose base is further down; mixing it into a pair with this layer would "
                    "composite it outside its own group and the clip would silently stop "
                    "applying";
      else
        mixReason = "the Pigment layer beneath it is already half of another mixed pair, "
                    "and chained mixes are not implemented (core/Composite.hpp says why)";
    }
    warningsOut->push_back(unimplementedBlendWarning(li, l, mixReason));
  };

  // Blends one already-scaled premultiplied texel into the accumulator.
  auto blendInto = [&](int32_t docX, int32_t docY, BlendMode mode,
                       const std::array<float, 4>& src) {
    if (docX < 0 || docX >= doc.width || docY < 0 || docY >= doc.height) return;  // clipped
    float* dst = &out[(static_cast<size_t>(docY) * w + static_cast<size_t>(docX)) * 4];
    const std::array<float, 4> composited = blendPixel(mode, src, {dst[0], dst[1], dst[2], dst[3]});
    dst[0] = composited[0];
    dst[1] = composited[1];
    dst[2] = composited[2];
    dst[3] = composited[3];
  };

  // Bottom to top: index 0 first, so each layer is `src` over everything
  // already accumulated beneath it. See the header on why this direction is
  // the file format's and not a free choice.
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& layer = doc.layers[i];
    // The lower half of a mixed pair is composited *by* the pair, one
    // iteration later, and never on its own -- the mix replaces both layers
    // rather than sitting over one of them.
    if (pairing.consumedByAbove[i]) continue;
    // A clipped layer with a base is composited **by** that base, as part of
    // its group, and never on its own -- the same relationship the line above
    // expresses for the lower half of a mixed pair. See §13.
    if (clips.clippedToBase[i]) continue;
    // A layer that asks to be clipped with nothing to clip to is composited
    // unclipped and warned about by name -- §17. Warned before every skip
    // below it, for §7's reason: whether the composite is an approximation is
    // a property of the document, not of whether this particular hidden layer
    // happened to matter today.
    if (clips.clippedWithoutBase[i] && warningsOut != nullptr)
      warningsOut->push_back(clippedLayerWithoutBaseWarning(doc, i));

    // **An Adjustment layer inverts the walk**: it contributes nothing and
    // instead transforms what is already accumulated beneath it. See this
    // file's header, §§8-11. It is handled before the storage test below
    // because it is the one kind that is *supposed* to have no storage.
    if (layer.kind == LayerKind::Adjustment) {
      // Warned about before the coverage test, for the same reason §7's
      // unimplemented blend is: whether the composite is an approximation of
      // what the document says is a property of the document, not of whether
      // this particular hidden layer happened to matter today.
      const std::optional<BlendMode> adjBlend = blendModeFromName(layer.blend);
      if ((!adjBlend.has_value() || *adjBlend != BlendMode::Normal) && warningsOut != nullptr)
        warningsOut->push_back(adjustmentLayerBlendWarning(i, layer));

      const float coverage = layerCoverage(layer);
      const std::vector<PointOp> ops = layerPointOps(layer.ops);
      // Both skips are exact no-ops rather than identity arithmetic: opacity 0
      // and an empty stack must leave the accumulator byte-for-byte untouched.
      if (coverage <= 0.0f || ops.empty()) continue;

      const MaskTileStore* adjMask = layer.mask.has_value() ? &*layer.mask : nullptr;
      // Walked a tile at a time so the mask lookup is hoisted exactly as it is
      // for every other kind -- one hash lookup per canvas tile, not one per
      // texel. The canvas starts at (0,0), so its tile coordinates run from 0.
      for (int32_t tileY = 0; tileY * kTileSize < doc.height; ++tileY) {
        for (int32_t tileX = 0; tileX * kTileSize < doc.width; ++tileX) {
          const MaskTile* maskTile = adjMask ? adjMask->find(TileCoord{tileX, tileY}) : nullptr;
          const PixelCoord origin = tileOrigin(TileCoord{tileX, tileY});
          for (int32_t ty = 0; ty < kTileSize; ++ty) {
            const int32_t docY = origin.y + ty;
            if (docY >= doc.height) break;
            for (int32_t tx = 0; tx < kTileSize; ++tx) {
              const int32_t docX = origin.x + tx;
              if (docX >= doc.width) break;
              float* dst = &out[(static_cast<size_t>(docY) * w + static_cast<size_t>(docX)) * 4];
              const float effective =
                  adjMask ? coverage * maskCoverage(maskTile, PixelCoord{tx, ty}) : coverage;
              const std::array<float, 4> adjusted = adjustedPremultiplied(
                  {dst[0], dst[1], dst[2], dst[3]}, ops, effective);
              // Three channels, deliberately: `adjusted[3]` is `dst[3]` by
              // construction (core/Composite.hpp §11), and not writing it at
              // all is what makes "an adjustment layer cannot change coverage"
              // a property of this loop rather than a property of a contract
              // two files away.
              dst[0] = adjusted[0];
              dst[1] = adjusted[1];
              dst[2] = adjusted[2];
            }
          }
        }
      }
      continue;
    }

    const bool hasRgb = layer.kind == LayerKind::RGB && layer.rgbTiles.has_value();
    const bool hasPigment = layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value();
    if (!hasRgb && !hasPigment) continue;

    // Warned about even when the layer contributes nothing, and warned about
    // before the coverage test: whether the composite is an approximation is a
    // property of the document, not of whether this particular hidden layer
    // happened to matter today. A user who unhides it must not discover the
    // approximation only then.
    // `blendIsImplementedForLayer()` answers exactly this, but it recomputes
    // the pairing on every call; this walk already has it, so the same
    // question is asked of the same data instead of once per layer.
    warnUnimplementedBlend(i);
    const std::optional<BlendMode> resolved = blendModeFromName(layer.blend);

    // Resolved once per layer, never per texel: `blend` is a std::string and a
    // string comparison in the inner loop would be the one plausible way to
    // make this walk slow. A name this build cannot honour *here* -- unknown,
    // or a `mix` PRD L5 does not permit -- becomes `Normal`, which is
    // precisely the approximation the warning above describes, made in one
    // place rather than at each texel.
    const BlendMode mode = (resolved.has_value() && blendModeInfo(*resolved).compositesPixels)
                               ? *resolved
                               : BlendMode::Normal;

    // Resolved once per layer for the same reason: building a PointOp
    // allocates a closure, so doing it per texel would dominate the walk.
    const std::vector<PointOp> ops = layerPointOps(layer.ops);

    const float coverage = layerCoverage(layer);

    // Resolved once per layer for the same reason as everything above it: the
    // per-*tile* lookup is hoisted out of the texel loops below, so a masked
    // layer costs one hash lookup per tile rather than one per texel, and an
    // unmasked one costs a null check. `nullptr` here means "this layer has no
    // mask", which `maskCoverage()` and the branches below both read as a
    // uniform 1.0 -- the identical arithmetic path an unmasked layer took
    // before this step, which is what keeps step 1's byte-identity boundary
    // exact (see this file's header, §5).
    const MaskTileStore* maskTiles = layer.mask.has_value() ? &*layer.mask : nullptr;

    // --- The clipping group this layer is the base of (§§12-17) -----------
    //
    // Resolved once per layer like everything above it, and **empty for every
    // layer of a document with no clipped layers**, which is what lets the
    // three tile walks below keep their pre-step-9 arithmetic exactly.
    //
    // A base is never the lower half of a mixed pair. It cannot be: for that,
    // the layer directly above it would have to carry a `mix` that paired,
    // which `blendModeAvailableForLayer()` refuses when that layer is clipped
    // -- and every layer directly above a base is, by definition, clipped.
    struct ClipMember {
      bool adjustment = false;
      BlendMode mode = BlendMode::Normal;
      std::vector<PointOp> ops;
      float coverage = 0.0f;
      const MaskTileStore* maskTiles = nullptr;
      const TileStore* rgbTiles = nullptr;
      const PigmentTileStore* pigmentTiles = nullptr;
      // Rebound once per tile **of the base**, never per texel -- the same
      // hoist every other store lookup in this walk gets.
      const MaskTile* maskTile = nullptr;
      const Tile* rgbTile = nullptr;
      const PigmentTile* pigmentTile = nullptr;
    };
    std::vector<ClipMember> members;
    for (const size_t mi : clips.members[i]) {
      const Layer& m = doc.layers[mi];
      ClipMember cm;
      cm.coverage = layerCoverage(m);
      cm.ops = layerPointOps(m.ops);
      cm.maskTiles = m.mask.has_value() ? &*m.mask : nullptr;
      if (m.kind == LayerKind::Adjustment) {
        // Reported before the coverage test, exactly as an unclipped
        // Adjustment layer's is -- §11's blend rule does not change because
        // the layer is clipped, and neither does the reporting of it.
        const std::optional<BlendMode> mb = blendModeFromName(m.blend);
        if ((!mb.has_value() || *mb != BlendMode::Normal) && warningsOut != nullptr)
          warningsOut->push_back(adjustmentLayerBlendWarning(mi, m));
        cm.adjustment = true;
        // The same two exact no-ops §10 gives an unclipped adjustment layer:
        // an empty stack and zero coverage must cost nothing at all, which
        // here means never being a member that could open the group.
        if (cm.ops.empty() || cm.coverage <= 0.0f) continue;
      } else if (layerHoldsPixels(m)) {
        warnUnimplementedBlend(mi);
        const std::optional<BlendMode> mb = blendModeFromName(m.blend);
        cm.mode = (mb.has_value() && blendModeInfo(*mb).compositesPixels) ? *mb : BlendMode::Normal;
        if (m.kind == LayerKind::RGB)
          cm.rgbTiles = &*m.rgbTiles;
        else
          cm.pigmentTiles = &*m.pigmentTiles;
        if (cm.coverage <= 0.0f) continue;
      } else {
        // A kind that holds no pixels (Media/Strokes/Text/Flats, or a
        // malformed RGB layer with no store) contributes nothing here for the
        // same reason it contributes nothing anywhere else in this walk.
        continue;
      }
      members.push_back(std::move(cm));
    }

    // Rebinds every member's per-tile pointers for one tile of the base.
    auto bindMemberTiles = [&](const TileCoord& coord) {
      for (ClipMember& m : members) {
        m.maskTile = m.maskTiles ? m.maskTiles->find(coord) : nullptr;
        m.rgbTile = m.rgbTiles ? m.rgbTiles->find(coord) : nullptr;
        m.pigmentTile = m.pigmentTiles ? m.pigmentTiles->find(coord) : nullptr;
      }
    };

    // The whole of §13, per texel. `base` is the base's own final premultiplied
    // source texel -- graded and coverage-applied, i.e. exactly what this walk
    // would have blended in had nothing been clipped to it -- and the returned
    // texel takes its place. Returns `base` **bit-identically** when no member
    // contributes here, which is what keeps the bracket's divide-and-multiply
    // out of every texel that does not need it.
    auto foldClipGroup = [&](const std::array<float, 4>& base,
                             const PixelCoord& local) -> std::array<float, 4> {
      std::array<float, 4> g{};
      bool opened = false;
      for (const ClipMember& m : members) {
        const float cov = m.maskTiles ? m.coverage * maskCoverage(m.maskTile, local) : m.coverage;
        if (cov <= 0.0f) continue;  // a skip, not a multiply by zero -- §5
        std::array<float, 4> src{};
        if (!m.adjustment) {
          if (m.rgbTiles != nullptr) {
            if (m.rgbTile == nullptr) continue;  // no tile here: contributes nothing
            src = m.rgbTile->readPixel(local);
          } else {
            if (m.pigmentTile == nullptr) continue;
            src = projectPigmentTexel(m.pigmentTile->readTexel(local));
          }
        }
        if (!opened) {
          // A base with no coverage clips everything away, which is PRD C9
          // read literally -- and is also what keeps clipGroupOpen()'s
          // division unreachable rather than guarded twice.
          if (!(base[3] > 0.0f)) return base;
          g = clipGroupOpen(base);
          opened = true;
        }
        // A clipped **Adjustment** layer's "composite below" is the group, not
        // the document accumulator -- §14, and the whole of why a clipped
        // adjustment layer grades its base and nothing else. `g[3]` is exactly
        // 1.0f here, so `adjustedPremultiplied()`'s own un-premultiply is a
        // division by one and its `below.a <= 0` early-out is unreachable.
        g = m.adjustment ? adjustedPremultiplied(g, m.ops, cov)
                         : clipGroupFold(g, m.mode, src, m.ops, cov);
      }
      return opened ? clipGroupClose(g, base[3]) : base;
    };

    if (pairing.mixedWithBelow[i]) {
      // A mixed pair. Deliberately *not* skipped when this layer's coverage is
      // zero: the lower layer is this branch's responsibility now, so an
      // invisible mixing layer still has to let the layer beneath it through.
      // `mixedPairTexel()` handles that -- at upperCoverage 0 it reduces to
      // `lowerCoverage * Plow`, exactly what the lower layer would have
      // contributed on its own.
      const Layer& lower = doc.layers[i - 1];
      const float lowerCoverage = layerCoverage(lower);
      const std::vector<PointOp> lowerOps = layerPointOps(lower.ops);
      // Both halves of the pair carry their own mask, and each one modulates
      // only its own coverage -- `covLow` and `covUp` in this file's header
      // §3, now per texel. The mixing weight `t` is `upper.mass` and is
      // untouched by either, which is the whole of §5's argument.
      const MaskTileStore* lowerMaskTiles = lower.mask.has_value() ? &*lower.mask : nullptr;
      const PigmentTileStore* upTiles =
          layer.pigmentTiles.has_value() ? &*layer.pigmentTiles : nullptr;
      const PigmentTileStore* lowTiles =
          (lower.kind == LayerKind::Pigment && lower.pigmentTiles.has_value())
              ? &*lower.pigmentTiles
              : nullptr;

      // The union of both layers' occupied tiles: a texel where only one of
      // them has a tile still mixes (against an all-zero, mass-0 texel, which
      // is what an unallocated tile means), and a texel where neither does
      // contributes nothing under every combination, so the union is exactly
      // the set worth visiting.
      std::unordered_set<TileCoord> coords;
      if (upTiles)
        for (const auto& [coord, tile] : *upTiles) {
          (void)tile;
          coords.insert(coord);
        }
      if (lowTiles)
        for (const auto& [coord, tile] : *lowTiles) {
          (void)tile;
          coords.insert(coord);
        }

      for (const TileCoord& coord : coords) {
        const PigmentTile* up = upTiles ? upTiles->find(coord) : nullptr;
        const PigmentTile* low = lowTiles ? lowTiles->find(coord) : nullptr;
        const MaskTile* upMask = maskTiles ? maskTiles->find(coord) : nullptr;
        const MaskTile* lowMask = lowerMaskTiles ? lowerMaskTiles->find(coord) : nullptr;
        if (!members.empty()) bindMemberTiles(coord);
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          const int32_t docY = origin.y + ty;
          if (docY < 0 || docY >= doc.height) continue;
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t docX = origin.x + tx;
            if (docX < 0 || docX >= doc.width) continue;
            const PixelCoord local{tx, ty};
            const PigmentTexel upTexel = up ? up->readTexel(local) : PigmentTexel{};
            const PigmentTexel lowTexel = low ? low->readTexel(local) : PigmentTexel{};
            // A mask multiplies its own layer's coverage and nothing else. At
            // an unmasked texel both products are by a uniform 1.0f -- exact
            // for every finite float -- so a mixed pair with no masks reaches
            // `mixedPairTexel()` with bit-identical arguments to the ones it
            // received before this step.
            const float covUp = maskTiles ? coverage * maskCoverage(upMask, local) : coverage;
            const float covLow =
                lowerMaskTiles ? lowerCoverage * maskCoverage(lowMask, local) : lowerCoverage;
            // `over` always: the pair's own arithmetic *is* the mix, and what
            // it produces meets everything beneath it as ordinary coverage.
            std::array<float, 4> pair =
                mixedPairTexel(lowTexel, lowerOps, covLow, upTexel, ops, covUp);
            // **A mixed pair is a perfectly good clip base** (§15): the pair is
            // one unit, and one unit is what a base is. No special case is
            // needed -- the pair's output texel *is* the base's `S`.
            if (!members.empty()) pair = foldClipGroup(pair, local);
            blendInto(docX, docY, BlendMode::Normal, pair);
          }
        }
      }
      continue;
    }

    // A hidden layer, or one at zero opacity, contributes **exactly** nothing:
    // this is a skip, not a multiply by zero that could still perturb the
    // accumulator through a signed zero or a NaN texel. --selftest asserts the
    // hidden case at zero tolerance.
    if (coverage <= 0.0f) continue;

    // One scale-and-blend, shared by both storage shapes so the two branches
    // differ only in how a texel is obtained. `effective` is the layer's
    // coverage already multiplied by its mask sample at this texel -- one
    // scalar, because a mask and an opacity are the same quantity and compose
    // as a plain product (this file's header, §5).
    auto contribute = [&](int32_t docX, int32_t docY, std::array<float, 4> src, float effective,
                          const PixelCoord& local) {
      src = gradedPremultiplied(src, ops);
      // The one place opacity enters. At the default 1.0 this is a
      // multiplication by literal 1.0f -- exact for every finite float --
      // which is half of why an unchanged document composites bit-identically
      // to the plain sum this replaced.
      if (effective != 1.0f) {
        src[0] *= effective;
        src[1] *= effective;
        src[2] *= effective;
        src[3] *= effective;
      }
      // The clipping group, if this layer is a base (§13). The branch is a
      // per-layer constant, and the *whole* of what step 9 added to this hot
      // path: a document with no clipped layer never enters it, so the texel
      // blended below is the one this line blended before that step.
      if (!members.empty()) src = foldClipGroup(src, local);
      blendInto(docX, docY, mode, src);
    };

    if (hasRgb) {
      for (const auto& [coord, tile] : *layer.rgbTiles) {
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
        // **The base's tiles are the clipping run's whole extent** (§17):
        // outside them the base's alpha is 0, so the group contributes exactly
        // nothing. A clipped layer's own tiles are never visited for their own
        // sake, which is why clipping can only make this walk cheaper.
        if (!members.empty()) bindMemberTiles(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          const int32_t docY = origin.y + ty;
          if (docY < 0 || docY >= doc.height) continue;  // clipped to the canvas
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t docX = origin.x + tx;
            if (docX < 0 || docX >= doc.width) continue;
            const PixelCoord local{tx, ty};
            // A texel the mask has hidden outright is **skipped**, exactly as
            // a hidden layer is, and for the identical reason: multiplying by
            // zero is not the same as not contributing if the stored texel is
            // a NaN or a signed zero. That also makes an all-0.0 mask
            // byte-identical to the layer being deleted.
            const float effective =
                maskTiles ? coverage * maskCoverage(maskTile, local) : coverage;
            if (effective <= 0.0f) continue;
            contribute(docX, docY, tile.readPixel(local), effective, local);
          }
        }
      }
    } else {
      for (const auto& [coord, tile] : *layer.pigmentTiles) {
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
        if (!members.empty()) bindMemberTiles(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          const int32_t docY = origin.y + ty;
          if (docY < 0 || docY >= doc.height) continue;
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t docX = origin.x + tx;
            if (docX < 0 || docX >= doc.width) continue;
            const PixelCoord local{tx, ty};
            const float effective =
                maskTiles ? coverage * maskCoverage(maskTile, local) : coverage;
            if (effective <= 0.0f) continue;
            // The mask multiplies the **projection**, never the mass that goes
            // into it: `projectPigmentTexel()` is handed the stored texel
            // unchanged. §5 says why that is the difference between a mask and
            // an eraser.
            contribute(docX, docY, projectPigmentTexel(tile.readTexel(local)), effective, local);
          }
        }
      }
    }
  }

  return out;
}

}  // namespace np
