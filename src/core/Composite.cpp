#include "core/Composite.hpp"

#include <optional>
#include <unordered_set>

#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

namespace np {

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
    const std::optional<BlendMode> resolved = blendModeFromName(layer.blend);
    const bool implementedHere =
        resolved.has_value() && (blendModeInfo(*resolved).compositesPixels ||
                                 (blendModeInfo(*resolved).compositesLatents &&
                                  pairing.mixedWithBelow[i]));
    if (!implementedHere && warningsOut != nullptr) {
      std::string mixReason;
      if (resolved == BlendMode::Mix) {
        if (layer.kind != LayerKind::Pigment)
          mixReason = "this is a " + std::string(layerKindName(layer.kind)) +
                      " layer, not a Pigment layer";
        else if (i == 0)
          mixReason = "it is the bottom layer, so there is nothing beneath it to mix with";
        else if (doc.layers[i - 1].kind != LayerKind::Pigment)
          mixReason = "the layer beneath it is a " +
                      std::string(layerKindName(doc.layers[i - 1].kind)) +
                      " layer, not a Pigment layer";
        else
          mixReason = "the Pigment layer beneath it is already half of another mixed pair, "
                      "and chained mixes are not implemented (core/Composite.hpp says why)";
      }
      warningsOut->push_back(unimplementedBlendWarning(i, layer, mixReason));
    }

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
            blendInto(docX, docY, BlendMode::Normal,
                      mixedPairTexel(lowTexel, lowerOps, covLow, upTexel, ops, covUp));
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
    auto contribute = [&](int32_t docX, int32_t docY, std::array<float, 4> src, float effective) {
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
      blendInto(docX, docY, mode, src);
    };

    if (hasRgb) {
      for (const auto& [coord, tile] : *layer.rgbTiles) {
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
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
            contribute(docX, docY, tile.readPixel(local), effective);
          }
        }
      }
    } else {
      for (const auto& [coord, tile] : *layer.pigmentTiles) {
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
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
            contribute(docX, docY, projectPigmentTexel(tile.readTexel(local)), effective);
          }
        }
      }
    }
  }

  return out;
}

}  // namespace np
