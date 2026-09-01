#include "core/Composite.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "core/Mask.hpp"
#include "core/Parallel.hpp"
#include "core/Pigment.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

namespace np {

// Declared in core/Composite.hpp now that core/DirtyTiles.cpp needs the
// identical question; body unchanged from when this lived in this file's own
// anonymous namespace.
bool layerHoldsPixels(const Layer& layer) noexcept {
  return (layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) ||
         (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value());
}

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

namespace {
// Test-only kill switch for the opaque-floor early exit below. Defaults to
// on; app/selftest/OpaqueFloor.cpp is the only caller that ever turns it
// off, to obtain the pre-optimization walk for a bit-identity comparison
// against the same document composited with the optimization live. Nothing
// in the running application ever calls the setter.
bool g_opaqueFloorEnabledForTesting = true;

// The grain `compositeWalk()`'s own tile loops pass to `core::parallelFor()`
// (core/Parallel.hpp) -- overridable for app/selftest/CompositeParallel.cpp's
// own bit-identity proof, which needs to force BOTH the serial fallback (a
// huge grain) and the parallel path (a tiny one) against the SAME tile
// count to compare them. `kDefaultCompositeParallelGrain` below is the
// number every real caller gets.
//
// **Why this is not `core::kParallelForDefaultGrain` (8, ops/Blur.cpp's own
// number).** That constant was measured against blur's per-tile cost -- one
// convolution pass over 128*128*4 floats -- and this file's own header
// already argues blur is a different shape of work: a composite tile's cost
// is up to N layers' worth of grade/mask/blend arithmetic (`contribute()`,
// `foldClipGroup()`, `mixedPairTexel()`), which app/selftest's own
// [measured] parallel-composite section times directly rather than
// borrowing blur's number on the assumption that "per-tile work" means one
// thing across this codebase.
size_t g_compositeParallelGrain = 4;
}  // namespace

void setOpaqueFloorEnabledForTesting(bool enabled) { g_opaqueFloorEnabledForTesting = enabled; }

void setCompositeParallelGrainForTesting(size_t grain) { g_compositeParallelGrain = grain; }

namespace {
// Resolves a `Layer::parent` string to the index of the Group layer it names,
// or `nullopt` when it does not resolve to any live Group in `doc` -- an
// empty tag, a dangling one, or one that now names a non-Group layer (a
// hand-built fixture, or a `.npaint` this build did not write). Linear in
// the layer count; groups are rare and this is not called per texel.
std::optional<size_t> resolveGroupByTag(const Document& doc, const std::string& tag) {
  if (tag.empty()) return std::nullopt;
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (doc.layers[i].kind == LayerKind::Group && doc.layers[i].groupTag == tag) return i;
  }
  return std::nullopt;
}
}  // namespace

GroupAncestryResult groupAncestry(const Document& doc, size_t index) noexcept {
  GroupAncestryResult out;
  if (index >= doc.layers.size()) return out;
  std::unordered_set<size_t> visited;
  std::string tag = doc.layers[index].parent;
  // Bounded by the layer count rather than by `visited` alone, belt-and-braces
  // against the one shape `visited` cannot catch on its own: none, since
  // `visited` already refuses to revisit an index. The explicit cap is here
  // so the loop's termination does not rest on `unordered_set` alone being
  // correct -- cheap insurance, and `--selftest` exercises the case it is for.
  for (size_t hops = 0; hops < doc.layers.size() && !tag.empty(); ++hops) {
    const std::optional<size_t> parentIdx = resolveGroupByTag(doc, tag);
    if (!parentIdx.has_value()) break;  // dangling tag: no more ancestors
    if (!visited.insert(*parentIdx).second) {
      // Revisiting a group already on this walk: a cycle. Hide rather than
      // loop -- `layerCoverage()`'s own answer to a NaN opacity.
      out.coverage = 0.0f;
      out.cyclic = true;
      return out;
    }
    const Layer& g = doc.layers[*parentIdx];
    out.coverage *= layerCoverage(g);
    if (out.coverage <= 0.0f) return out;  // already invisible; nothing more to learn
    tag = g.parent;
  }
  return out;
}

float groupCoverage(const Document& doc, size_t index) noexcept {
  return groupAncestry(doc, index).coverage;
}

float layerCoverage(const Document& doc, size_t index) noexcept {
  if (index >= doc.layers.size()) return 0.0f;
  const float own = layerCoverage(doc.layers[index]);
  if (own <= 0.0f) return 0.0f;  // skip the ancestor walk when it cannot matter
  return own * groupCoverage(doc, index);
}

std::vector<Op> layerPointOps(const OpStack& ops) {
  std::vector<Op> out;
  // detectRuns() has already dropped disabled entries and split at non-PointA
  // boundaries; walking each run's raw `[startIndex, endIndex)` range and
  // copying `ops.at(i)` verbatim reproduces exactly the same filtered,
  // flattened list `run.ops` (a vector of closures) used to hand back
  // directly -- see OpRun::ops's own doc comment (core/OpStack.hpp) for why
  // "enabled PointA entries, in order" is the filter either representation
  // applies. Only PointA has an implementation anywhere in this codebase
  // (core/OpStack.hpp), so a SpatialB/StrokeC/BakedD entry is simply absent
  // from every run and therefore from this list.
  for (const OpRun& run : ops.detectRuns())
    for (size_t i = run.startIndex; i < run.endIndex; ++i) {
      const Op& op = ops.at(i);
      if (op.enabled) out.push_back(op);
    }
  return out;
}

std::array<float, 4> gradedPremultiplied(const std::array<float, 4>& premultiplied,
                                         const std::vector<Op>& ops) {
  // The early return is the correctness-relevant line, not an optimisation:
  // applyOpsPremultiplied() divides by alpha and multiplies back, which
  // is two correctly-rounded operations and therefore NOT the identity, and
  // PLAN.md step 1's regression boundary (an unchanged document composites
  // byte-identically to the plain sum it replaced) is asserted at zero
  // tolerance. Every layer with no op stack must cost exactly nothing.
  if (ops.empty()) return premultiplied;
  return applyOpsPremultiplied(premultiplied, ops);
}

std::array<float, 4> projectPigmentTexel(const PigmentTexel& texel) noexcept {
  const std::array<float, 3> rgb = latentToRgb(texel.latent);
  const float m = texel.mass;
  return {rgb[0] * m, rgb[1] * m, rgb[2] * m, m};
}

std::array<float, 4> mixedPairTexel(const PigmentTexel& lower,
                                    const std::vector<Op>& lowerOps, float lowerCoverage,
                                    const PigmentTexel& upper,
                                    const std::vector<Op>& upperOps, float upperCoverage) {
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
                                           const std::vector<Op>& ops,
                                           float effectiveCoverage) {
  // Three ways an adjustment layer does nothing at this texel, each returning
  // `below` unchanged rather than computing something that happens to equal
  // it. See core/Composite.hpp §§9-10 for why each has to be bit-exact:
  //   * no ops -- the same rule §1 applies to every other kind's stack;
  //   * no coverage -- a hidden layer, opacity 0, or a mask sample of 0;
  //   * nothing beneath -- there is no colour to grade, and this is also what
  //     keeps applyOpsPremultiplied()'s `a <= 0` guard out of this path
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
                                   std::array<float, 4> src, const std::vector<Op>& ops,
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

namespace {

// ==========================================================================
// The opaque-floor early exit
// ==========================================================================
//
// **The claim.** Walking bottom to top, if some layer j's own final
// premultiplied source texel -- exactly the `src` `blendInto()` would be
// handed for it, after grading, opacity, mask and (for a clip base) its
// group's fold -- has alpha EXACTLY 1.0 at every texel of a tile, AND its
// own blend mode is `Normal`, then for every texel of that tile the walk's
// result is completely independent of layers 0..j-1: `blendPixel(Normal,
// src, dst) == src` bit-for-bit whenever `src[3] == 1.0f`, from the
// premultiplied `over` formula `co = cs + cb*(1-as)` with `as == 1` making
// the `cb` term `cb * 0.0f`, which is exactly `0.0f` for every finite `cb`
// (never approximately). So substituting `src_j` for the whole bottom-up
// fold of layers `0..j` changes nothing downstream: every layer above `j`
// only ever consumes the accumulator as an opaque `dst` argument, whatever
// blend mode or kind it is.
//
// This is why the mechanism below never has to touch layers ABOVE the
// floor at all: skipping layers `0..j-1` for a tile leaves the accumulator
// at transparent black going into layer j's own ordinary contribute()/
// blendInto() call, and `blendPixel(Normal, src_j, {0,0,0,0}) == src_j`
// exactly too (the same formula, `cb == 0`) -- so "skip everything below
// j" and "seed the accumulator with src_j" are the same bits, and the
// walk's own zero-initialized-by-the-caller accumulator already IS that
// seed. No special seeding code is needed; only a skip.
//
// **Why `Normal` only.** For every other mode here, `co` retains a `B(Cs,
// Cb)` term that depends on the backdrop's straight colour even at `as ==
// 1` (core/Blend.hpp's general separable-blend formula; only `over`'s
// `B(Cs,Cb) = Cs` makes that term collapse to something backdrop-
// independent). A fully-opaque `Multiply` layer, for instance, still reads
// and darkens whatever is beneath it -- it does not hide it. See this
// file's own research notes (not reproduced here) for the full derivation;
// `blendModeInfo(*resolved).compositesPixels ? *resolved : Normal` (the
// walk's own fallback rule, factored out below as
// `resolvedPixelBlendMode()`) is the single source of truth for "which mode
// actually reaches `blendPixel()`", reused rather than re-derived so this
// section cannot silently disagree with the real dispatch.
//
// **Why a clip base needs no separate check.** `clipGroupClose()` above
// assigns the closed group's alpha `baseAlpha` **unconditionally** -- not
// `ao` from any member's own blend, not a running fold -- so a clip base's
// contributed alpha is exactly its own effective alpha, regardless of what
// its members are or do. Qualifying as a floor is therefore the identical
// question for a clip base as for an ordinary layer of the same kind: only
// the *colour* a base contributes depends on its members, never whether it
// is opaque. That is why the check below runs unmodified whether or not
// `clips.members[i]` is empty.
//
// **Why `Mix` gets no O(1) shortcut.** A mixed pair's alpha
// (`mixedPairTexel()`'s `[3]`) is a per-texel weighted combination of BOTH
// layers' own coverage, not a property either layer's own tile stores in
// one place -- so there is nothing to cache in a `TileStoreOf` the way
// there is for an ordinary layer's raw alpha/mass channel. `mixPairIsOpaqueEverywhere()`
// below computes it directly, texel by texel, exactly as the real walk's
// mixed-pair branch would -- so a Mix pair that turns out to be the floor
// costs what compositing it would already have cost, and nothing is cached
// across calls for this case (Mix is PRD L5's opt-in, rare in practice).
//
// **Soundness of the per-tile cache this leans on.** `TileStoreOf<T>::
// tileSatisfiesEverywhere()` (core/TileStore.hpp) is the mechanism; its own
// header comment derives why eager invalidation at every write-capable call
// (`getOrCreate`/`findForWrite`/`shareTileFrom`/`rekeyTiles`) is a complete
// enumeration and therefore sound, mirroring core/DirtyTiles.hpp §2's own
// completeness proof for the identical reason: those four are the whole
// closed set of ways to obtain a writable tile from a `TileStoreOf`.
//
// **What is NOT skipped.** An Adjustment layer strictly below the floor
// still runs its own full-canvas/dirty-set pass over the tile in question,
// even though its result is provably about to be discarded the moment the
// floor layer composites over it (the same erasure argument applies
// transitively -- `blendPixel(Normal, src_j, ANYTHING)` is `src_j`
// regardless of what "ANYTHING" is, so an adjustment layer's transformation
// of the accumulator below the floor is wasted work, not a wrong answer).
// This is a deliberate, stated scope limit rather than an oversight: fixing
// it means threading the same per-tile skip through the Adjustment branch's
// own tile enumeration, which this change does not do. It costs nothing in
// correctness -- only in an optimization this change leaves on the table.

// binary16's single bit pattern for exactly 1.0f (IEEE 754 half precision
// has no other representation of it, unlike NaN). `core::MaskTile` already
// relies on the identical fact for `kRevealWord`/`isFullyRevealed()`; this
// is the same constant, independently named because it is being applied to
// a different channel's meaning (alpha/mass, not mask reveal) and a
// selftest section asserts it against `floatToHalf(1.0f)` on its own terms
// rather than trusting either literal to agree with the other by
// coincidence.
constexpr uint16_t kHalfOne = 0x3C00;

// Whether an RGB tile's alpha channel (offset 3 of every 4-channel texel,
// core/TileStore.hpp's own documented layout) reads exactly 1.0 at every
// one of its 16384 texels. Raw half-word comparison rather than
// `readPixel()` -- this test does not need the three colour channels
// `readPixel()` would also decode, and a raw `uint16_t` compare is exact
// for the identical reason `MaskTile::isFullyRevealed()`'s is.
bool tileAlphaIsOneEverywhere(const Tile& t) noexcept {
  const uint16_t* words = t.data();
  for (size_t i = 0; i < Tile::kTexelCount; i += static_cast<size_t>(Tile::kChannels)) {
    if (words[i + 3] != kHalfOne) return false;
  }
  return true;
}

// The identical question for a Pigment tile's mass channel (offset 3 of
// every 7-channel texel, core/Pigment.hpp's own documented layout) -- mass
// IS a Pigment texel's alpha (this file's header §1), so "mass is 1.0
// everywhere" is exactly the Pigment analogue of `tileAlphaIsOneEverywhere`.
bool pigmentTileMassIsOneEverywhere(const PigmentTile& t) noexcept {
  const uint16_t* words = t.data();
  for (size_t i = 0; i < PigmentTile::kTexelCount;
       i += static_cast<size_t>(PigmentTile::kChannels)) {
    if (words[i + 3] != kHalfOne) return false;
  }
  return true;
}

// The walk's own "which mode does blendPixel() actually see" rule (was
// inline at this branch's per-layer resolution below), factored out so the
// floor search can ask the identical question rather than a hand-copied
// approximation of it that could silently drift from the real dispatch.
BlendMode resolvedPixelBlendMode(const Layer& layer) noexcept {
  const std::optional<BlendMode> resolved = blendModeFromName(layer.blend);
  return (resolved.has_value() && blendModeInfo(*resolved).compositesPixels) ? *resolved
                                                                              : BlendMode::Normal;
}

// **The document walk, in one place, for both callers.** `only` restricts it
// to a tile set (null means "every tile a store holds", the pre-incremental
// behaviour); `region` says where the accumulator lives, which for a full
// composite is the canvas and for an incremental one is a rectangle around
// the dirty tiles. Nothing else differs between the two, which is what makes
// an incremental result bit-identical to a full one rather than merely close
// -- see core/Composite.hpp's region section.
void compositeWalk(const Document& doc, const std::unordered_set<TileCoord>* only,
                   const CompositeRegion& region, std::vector<std::string>* warningsOut) {
  // The accumulator texel at a document coordinate, or null when it is
  // outside the canvas or outside the destination rectangle. The canvas test
  // is the one this walk has always made; the rectangle test is a second
  // clip, and for a full composite it is the same rectangle and therefore
  // never fires.
  auto accum = [&](int32_t docX, int32_t docY) -> float* {
    if (docX < 0 || docX >= doc.width || docY < 0 || docY >= doc.height) return nullptr;
    const int32_t rx = docX - region.origin.x;
    const int32_t ry = docY - region.origin.y;
    if (rx < 0 || rx >= region.width || ry < 0 || ry >= region.height) return nullptr;
    return region.pixels + (static_cast<size_t>(ry) * static_cast<size_t>(region.width) +
                            static_cast<size_t>(rx)) *
                               4u;
  };

  // Whether this walk visits a tile at all. A per-tile test, never per texel.
  auto visits = [&](const TileCoord& coord) {
    return only == nullptr || only->find(coord) != only->end();
  };

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

  // --- The opaque-floor early exit ---------------------------------------
  //
  // See this file's anonymous namespace above (`tileAlphaIsOneEverywhere()`
  // and neighbours) for the full derivation. `floorAt(coord)` answers the
  // topmost layer index `j` such that everything strictly below `j` is
  // provably irrelevant to tile `coord` -- 0 when nothing qualifies, which
  // is always correct (the ordinary walk already starts at layer 0). Every
  // answer is computed once, serially, by the pre-warm pass below
  // (`floorForSerial()`) before any tile loop runs -- `floorAt()` is a
  // read-only lookup into the result, safe to call from whichever thread
  // ends up processing a given tile once the walk's own tile loops are
  // parallelized (see this file's own section on that).
  //
  // Memoized here, per call, only to avoid re-running the top-down search
  // twice for one coordinate within one walk (e.g. an RGB and a Pigment
  // layer both touching it) -- the expensive part, a tile's own texel scan,
  // is what actually persists *across* calls, inside each layer's own
  // `TileStoreOf::coverageCache_`.
  std::unordered_map<TileCoord, size_t> floorMemo;

  // Whether layer `li`'s own effective source alpha is exactly 1.0f at
  // EVERY texel of tile `coord`. Applies unmodified to a clip base:
  // `clipGroupClose()` assigns the closed group's alpha to exactly the
  // base's own effective alpha regardless of its members (see this
  // namespace's header comment), so a clip base is opaque here under
  // precisely the condition an ordinary layer of the same kind is.
  auto ownAlphaIsOneEverywhere = [&](size_t li, const TileCoord& coord) -> bool {
    const Layer& l = doc.layers[li];
    if (layerCoverage(doc, li) != 1.0f) return false;
    if (l.mask.has_value()) {
      const MaskTileStore& maskTiles = *l.mask;
      const MaskTile* maskTile = maskTiles.find(coord);
      // Absent means "reveals" (core/Mask.hpp), the opposite reading from an
      // absent content tile -- handled here, before ever asking the generic
      // cache, so `tileSatisfiesEverywhere()` never has to know that a mask
      // store's "absent" and a content store's "absent" mean opposite
      // things.
      if (maskTile != nullptr) {
        const bool reveals = maskTiles.tileSatisfiesEverywhere(
            coord, [](const MaskTile& mt) { return mt.isFullyRevealed(); });
        if (!reveals) return false;
      }
    }
    if (l.kind == LayerKind::RGB && l.rgbTiles.has_value())
      return l.rgbTiles->tileSatisfiesEverywhere(coord, tileAlphaIsOneEverywhere);
    if (l.kind == LayerKind::Pigment && l.pigmentTiles.has_value())
      return l.pigmentTiles->tileSatisfiesEverywhere(coord, pigmentTileMassIsOneEverywhere);
    return false;
  };

  // The identical question for the upper half of a Mix pair at `upperIndex`
  // -- always Normal-blended into the accumulator (this walk's mixed-pair
  // branch below always calls `blendInto(..., BlendMode::Normal, pair)`),
  // so only the alpha condition needs checking, and it has no O(1)
  // shortcut: a pair's alpha is a per-texel combination of both layers' own
  // coverage (`mixedPairTexel()`'s `[3]`), never a property either layer's
  // raw tile stores in one channel. Computed directly, texel by texel,
  // exactly mirroring the real mixed-pair branch -- not cached across
  // calls; see this namespace's header comment on why that is an accepted
  // cost for the rare, opt-in case.
  auto mixPairIsOpaqueEverywhere = [&](size_t upperIndex, const TileCoord& coord) -> bool {
    const Layer& upper = doc.layers[upperIndex];
    const Layer& lower = doc.layers[upperIndex - 1];
    const PigmentTileStore* upTiles =
        upper.pigmentTiles.has_value() ? &*upper.pigmentTiles : nullptr;
    const PigmentTileStore* lowTiles =
        (lower.kind == LayerKind::Pigment && lower.pigmentTiles.has_value())
            ? &*lower.pigmentTiles
            : nullptr;
    const PigmentTile* up = upTiles ? upTiles->find(coord) : nullptr;
    const PigmentTile* low = lowTiles ? lowTiles->find(coord) : nullptr;
    if (up == nullptr && low == nullptr) return false;  // the pair paints nothing here
    const float upperCoverage = layerCoverage(doc, upperIndex);
    const float lowerCoverage = layerCoverage(doc, upperIndex - 1);
    const MaskTileStore* upperMaskTiles = upper.mask.has_value() ? &*upper.mask : nullptr;
    const MaskTileStore* lowerMaskTiles = lower.mask.has_value() ? &*lower.mask : nullptr;
    const MaskTile* upMask = upperMaskTiles ? upperMaskTiles->find(coord) : nullptr;
    const MaskTile* lowMask = lowerMaskTiles ? lowerMaskTiles->find(coord) : nullptr;
    const std::vector<Op> upperOps = layerPointOps(upper.ops);
    const std::vector<Op> lowerOps = layerPointOps(lower.ops);
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      for (int32_t tx = 0; tx < kTileSize; ++tx) {
        const PixelCoord local{tx, ty};
        const PigmentTexel upTexel = up ? up->readTexel(local) : PigmentTexel{};
        const PigmentTexel lowTexel = low ? low->readTexel(local) : PigmentTexel{};
        const float covUp =
            upperMaskTiles ? upperCoverage * maskCoverage(upMask, local) : upperCoverage;
        const float covLow =
            lowerMaskTiles ? lowerCoverage * maskCoverage(lowMask, local) : lowerCoverage;
        const std::array<float, 4> pair =
            mixedPairTexel(lowTexel, lowerOps, covLow, upTexel, upperOps, covUp);
        if (pair[3] != 1.0f) return false;
      }
    }
    return true;
  };

  // **Lazy, mutating, SERIAL ONLY.** Writes `floorMemo` on a miss, exactly as
  // it always has -- and that write is the one thing that makes this unsafe
  // to call from more than one thread at once (`floorMemo` is a bare
  // `std::unordered_map`, the identical hazard `core/Parallel.hpp` documents
  // for `TileStoreOf::getOrCreate`: two threads racing to insert would
  // corrupt its bucket chains, not merely disagree about the answer). This
  // function's only caller as of the parallel walk below is the serial
  // pre-warm pass immediately following it; every tile-loop body downstream
  // (some of them now parallel) reads through `floorAt()` instead, which
  // performs no insertion and is therefore safe to call concurrently, the
  // same "reads of an unmodified map are safe" argument core/Parallel.hpp
  // already makes for `TileStore::find()`.
  auto floorForSerial = [&](const TileCoord& coord) -> size_t {
    if (!g_opaqueFloorEnabledForTesting) return 0;
    const auto memoIt = floorMemo.find(coord);
    if (memoIt != floorMemo.end()) return memoIt->second;
    size_t floor = 0;
    for (size_t i = doc.layers.size(); i-- > 0;) {
      // Not a source the walk visits directly -- the lower half of a mixed
      // pair, or a clip member composited by its base -- so it can never be
      // a floor on its own (§5's "clip members never independently
      // qualify"/"the walk visits it directly").
      if (pairing.consumedByAbove[i] || clips.clippedToBase[i]) continue;
      const Layer& l = doc.layers[i];
      if (l.kind == LayerKind::Adjustment) continue;  // has no alpha of its own
      if (pairing.mixedWithBelow[i]) {
        if (mixPairIsOpaqueEverywhere(i, coord)) {
          floor = i;
          break;
        }
        continue;
      }
      if (!layerHoldsPixels(l)) continue;
      if (resolvedPixelBlendMode(l) != BlendMode::Normal) continue;
      if (ownAlphaIsOneEverywhere(i, coord)) {
        floor = i;
        break;
      }
    }
    floorMemo.emplace(coord, floor);
    return floor;
  };

  // --- Serial pre-warm: every tile any tile loop below could visit --------
  //
  // Enumerated with EXACTLY the skip conditions the walk itself applies
  // (`consumedByAbove`/`clippedToBase`/Adjustment/no-pixels), filtered
  // through `visits()`, so this set is neither a subset (which would leave
  // some tile's floor lazily computed mid-parallel-loop -- unsafe) nor an
  // unnecessarily large superset (the whole canvas, say, which would cost an
  // incremental composite work proportional to the canvas rather than to its
  // dirty tiles). A superset that includes a tile no loop ends up visiting is
  // harmless -- `floorForSerial()` on a coordinate nothing ever reads back is
  // just a wasted lookup, not a correctness question -- so the mixed-pair
  // union below is deliberately computed the same way the real branch
  // computes it rather than approximated.
  {
    std::unordered_set<TileCoord> tilesToWarm;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      if (pairing.consumedByAbove[i] || clips.clippedToBase[i]) continue;
      const Layer& l = doc.layers[i];
      if (l.kind == LayerKind::Adjustment) continue;
      if (pairing.mixedWithBelow[i]) {
        const Layer& lower = doc.layers[i - 1];
        if (l.pigmentTiles.has_value())
          for (const auto& [coord, tile] : *l.pigmentTiles) {
            (void)tile;
            if (visits(coord)) tilesToWarm.insert(coord);
          }
        if (lower.kind == LayerKind::Pigment && lower.pigmentTiles.has_value())
          for (const auto& [coord, tile] : *lower.pigmentTiles) {
            (void)tile;
            if (visits(coord)) tilesToWarm.insert(coord);
          }
        continue;
      }
      if (!layerHoldsPixels(l)) continue;
      if (l.kind == LayerKind::RGB) {
        for (const auto& [coord, tile] : *l.rgbTiles) {
          (void)tile;
          if (visits(coord)) tilesToWarm.insert(coord);
        }
      } else {
        for (const auto& [coord, tile] : *l.pigmentTiles) {
          (void)tile;
          if (visits(coord)) tilesToWarm.insert(coord);
        }
      }
    }
    floorMemo.reserve(tilesToWarm.size());
    for (const TileCoord& coord : tilesToWarm) floorForSerial(coord);
  }

  // The read-only lookup every (possibly parallel) tile-loop body below
  // actually calls. `floorMemo` is now complete for every coordinate any of
  // those loops will query -- the pre-warm pass above guarantees it -- so a
  // miss here can only mean "not warmed for some reason", and 0 (skip
  // nothing) is exactly the safe, conservative answer for that case: it
  // degrades to the pre-opaque-floor walk for that one tile rather than
  // reading an uninitialized entry or racing an insertion.
  auto floorAt = [&](const TileCoord& coord) -> size_t {
    const auto it = floorMemo.find(coord);
    return it == floorMemo.end() ? 0 : it->second;
  };

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
    float* dst = accum(docX, docY);
    if (dst == nullptr) return;  // clipped to the canvas, or outside the region
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

      const float coverage = layerCoverage(doc, i);
      const std::vector<Op> ops = layerPointOps(layer.ops);
      // Both skips are exact no-ops rather than identity arithmetic: opacity 0
      // and an empty stack must leave the accumulator byte-for-byte untouched.
      if (coverage <= 0.0f || ops.empty()) continue;

      const MaskTileStore* adjMask = layer.mask.has_value() ? &*layer.mask : nullptr;
      // Walked a tile at a time so the mask lookup is hoisted exactly as it is
      // for every other kind -- one hash lookup per canvas tile, not one per
      // texel.
      auto adjustTile = [&](const TileCoord& coord) {
        const MaskTile* maskTile = adjMask ? adjMask->find(coord) : nullptr;
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          const int32_t docY = origin.y + ty;
          if (docY >= doc.height) break;
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t docX = origin.x + tx;
            if (docX >= doc.width) break;
            float* dst = accum(docX, docY);
            if (dst == nullptr) continue;
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
      };
      // An Adjustment layer has no tiles, so its extent is the canvas -- or,
      // when this is an incremental walk, the dirty set, which is the one
      // place restricting the walk saves an O(canvas) pass rather than a
      // sparse one. The canvas starts at (0,0), so its tile coordinates run
      // from 0.
      //
      // **Parallel over tiles, exactly like the other three loops in this
      // walk.** `adjustTile()` touches nothing but its own tile's texels
      // (`accum(docX, docY)`, disjoint across tiles) and this layer's own
      // `const` `adjMask`/`ops`/`coverage` -- no clip group, no floor
      // lookup, no shared mutable state of any kind, which is why this is
      // the simplest of the four to parallelize.
      std::vector<TileCoord> tiles;
      if (only != nullptr) {
        tiles.assign(only->begin(), only->end());
      } else {
        // Reserved rather than left to grow: the tile grid's extent is
        // known up front and this avoids `push_back()` reallocating
        // partway through a loop that can run to thousands of tiles on a
        // large canvas.
        const int32_t tilesX = (doc.width + kTileSize - 1) / kTileSize;
        const int32_t tilesY = (doc.height + kTileSize - 1) / kTileSize;
        tiles.reserve(static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY));
        for (int32_t tileY = 0; tileY * kTileSize < doc.height; ++tileY)
          for (int32_t tileX = 0; tileX * kTileSize < doc.width; ++tileX)
            tiles.push_back(TileCoord{tileX, tileY});
      }
      parallelFor(tiles.size(), g_compositeParallelGrain,
                  [&](size_t idx) { adjustTile(tiles[idx]); });
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

    // Resolved once per layer, never per texel: `blend` is a std::string and a
    // string comparison in the inner loop would be the one plausible way to
    // make this walk slow. A name this build cannot honour *here* -- unknown,
    // or a `mix` PRD L5 does not permit -- becomes `Normal`, which is
    // precisely the approximation the warning above describes, made in one
    // place rather than at each texel. `resolvedPixelBlendMode()` is this
    // exact rule, factored out so the opaque-floor search below asks the
    // identical question rather than a copy that could drift from it.
    const BlendMode mode = resolvedPixelBlendMode(layer);

    // Resolved once per layer for the same reason: an Op carrying a Curve
    // copies a std::vector, so doing this per texel would dominate the walk
    // -- and per-texel evaluation now goes through applyOpDirect()'s switch
    // (core/OpStack.hpp, docs/architecture-review.md P0-5), not a per-op
    // closure, so hoisting this list is purely about not re-copying it.
    const std::vector<Op> ops = layerPointOps(layer.ops);

    const float coverage = layerCoverage(doc, i);

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
      std::vector<Op> ops;
      float coverage = 0.0f;
      const MaskTileStore* maskTiles = nullptr;
      const TileStore* rgbTiles = nullptr;
      const PigmentTileStore* pigmentTiles = nullptr;
    };
    std::vector<ClipMember> members;
    for (const size_t mi : clips.members[i]) {
      const Layer& m = doc.layers[mi];
      ClipMember cm;
      cm.coverage = layerCoverage(doc, mi);
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

    // **Per-tile-task-local binding, not a shared mutable field.** The
    // previous shape of this walk rebound `maskTile`/`rgbTile`/`pigmentTile`
    // fields directly on the shared `members` vector, once per tile, read
    // by `foldClipGroup()` afterward -- correct only as long as tiles of
    // this base are processed one at a time, in some order, by a single
    // thread. Parallelizing this layer's own tile loop (below) makes that
    // false: two tiles processed concurrently would both rebind the SAME
    // shared fields, each clobbering the other's, so `foldClipGroup()`
    // could read a tile pointer bound for a *different* tile than the one
    // whose texel it is currently folding -- an ordering-dependent data
    // race, not merely a slow frame. `BoundMember` fixes this by making the
    // per-tile resolution produce a fresh, task-local vector every time
    // (`bindMembersForTile()`), touching nothing shared; the small
    // allocation this costs is paid once per tile of a clip base, exactly
    // where the rebind used to happen, not once per texel.
    struct BoundMember {
      const ClipMember* member = nullptr;
      const MaskTile* maskTile = nullptr;
      const Tile* rgbTile = nullptr;
      const PigmentTile* pigmentTile = nullptr;
    };
    auto bindMembersForTile = [&](const TileCoord& coord) {
      std::vector<BoundMember> bound;
      bound.reserve(members.size());
      for (const ClipMember& m : members) {
        bound.push_back(BoundMember{&m, m.maskTiles ? m.maskTiles->find(coord) : nullptr,
                                    m.rgbTiles ? m.rgbTiles->find(coord) : nullptr,
                                    m.pigmentTiles ? m.pigmentTiles->find(coord) : nullptr});
      }
      return bound;
    };

    // The whole of §13, per texel. `base` is the base's own final premultiplied
    // source texel -- graded and coverage-applied, i.e. exactly what this walk
    // would have blended in had nothing been clipped to it -- and the returned
    // texel takes its place. Returns `base` **bit-identically** when no member
    // contributes here, which is what keeps the bracket's divide-and-multiply
    // out of every texel that does not need it. `bound` is this tile's own
    // task-local binding from `bindMembersForTile()` -- read-only here, never
    // the shared `members` vector's own (now nonexistent) per-tile fields.
    auto foldClipGroup = [&](const std::array<float, 4>& base, const PixelCoord& local,
                             const std::vector<BoundMember>& bound) -> std::array<float, 4> {
      std::array<float, 4> g{};
      bool opened = false;
      for (const BoundMember& bm : bound) {
        const ClipMember& m = *bm.member;
        const float cov = m.maskTiles ? m.coverage * maskCoverage(bm.maskTile, local) : m.coverage;
        if (cov <= 0.0f) continue;  // a skip, not a multiply by zero -- §5
        std::array<float, 4> src{};
        if (!m.adjustment) {
          if (m.rgbTiles != nullptr) {
            if (bm.rgbTile == nullptr) continue;  // no tile here: contributes nothing
            src = bm.rgbTile->readPixel(local);
          } else {
            if (bm.pigmentTile == nullptr) continue;
            src = projectPigmentTexel(bm.pigmentTile->readTexel(local));
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
      const float lowerCoverage = layerCoverage(doc, i - 1);
      const std::vector<Op> lowerOps = layerPointOps(lower.ops);
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

      // Filtered to an index-addressable vector, serially, before any
      // parallel work starts: `visits()`/`floorAt()` are per-tile skip
      // conditions, evaluated once here rather than once per (possibly
      // parallel) worker, so `parallelFor()`'s grain decision reflects the
      // real amount of work and no worker ever has to re-derive "should I
      // run at all". See this file's opaque-floor section for why `i ==
      // floorAt(coord)` (the pair itself IS the floor) still belongs in the
      // set below rather than being skipped too.
      std::vector<TileCoord> toVisit;
      toVisit.reserve(coords.size());
      for (const TileCoord& coord : coords) {
        if (visits(coord) && i >= floorAt(coord)) toVisit.push_back(coord);
      }
      // **Parallel over tiles, never over layers.** Each worker owns one
      // tile end to end -- both halves' reads, the mix, the optional clip
      // fold, and the write into `region.pixels` -- and different tiles
      // never touch the same accumulator address (core/Tile.hpp's own tile
      // partition of the canvas), so concurrent workers cannot race on the
      // destination. What they must not race on is anything SHARED across
      // tiles: `upTiles`/`lowTiles`/`maskTiles`/`lowerMaskTiles` are
      // read-only `find()` calls into stores nothing here mutates (safe,
      // core/Parallel.hpp's own argument for `TileStore::find()`);
      // `bindMembersForTile()` allocates a fresh, worker-local `bound`
      // rather than touching the shared `members` vector (see this file's
      // own comment on `BoundMember` for why that used to be the hazard
      // here); and `ops`/`lowerOps`/`coverage`/`lowerCoverage` are
      // `const` values captured by the outer lambda, never written after
      // this layer's own per-layer setup completes.
      parallelFor(toVisit.size(), g_compositeParallelGrain, [&](size_t idx) {
        const TileCoord coord = toVisit[idx];
        const PigmentTile* up = upTiles ? upTiles->find(coord) : nullptr;
        const PigmentTile* low = lowTiles ? lowTiles->find(coord) : nullptr;
        const MaskTile* upMask = maskTiles ? maskTiles->find(coord) : nullptr;
        const MaskTile* lowMask = lowerMaskTiles ? lowerMaskTiles->find(coord) : nullptr;
        const std::vector<BoundMember> bound =
            members.empty() ? std::vector<BoundMember>{} : bindMembersForTile(coord);
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
            if (!members.empty()) pair = foldClipGroup(pair, local, bound);
            blendInto(docX, docY, BlendMode::Normal, pair);
          }
        }
      });
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
    // `bound` is this tile's own task-local clip-member binding
    // (`bindMembersForTile()`) -- empty when `members.empty()`, in which
    // case `foldClipGroup()` is never called at all (the `if
    // (!members.empty())` below), so a document with no clipped layer pays
    // nothing extra for this parameter existing.
    auto contribute = [&](int32_t docX, int32_t docY, std::array<float, 4> src, float effective,
                          const PixelCoord& local, const std::vector<BoundMember>& bound) {
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
      if (!members.empty()) src = foldClipGroup(src, local, bound);
      blendInto(docX, docY, mode, src);
    };

    if (hasRgb) {
      // Filtered and collected into an index-addressable vector, serially,
      // for the identical reason the mixed-pair branch above does it:
      // `visits()`/`floorAt()` are evaluated once, up front, and the tile
      // pointer is captured here so the (possibly parallel) body below
      // never calls `TileStore::find()` on this layer's own store at all.
      std::vector<std::pair<TileCoord, const Tile*>> toVisit;
      for (const auto& [coord, tile] : *layer.rgbTiles) {
        if (visits(coord) && i >= floorAt(coord)) toVisit.emplace_back(coord, &tile);
      }
      // **Parallel over this layer's own tiles.** See the mixed-pair
      // branch's own comment on what makes a worker body here race-free:
      // disjoint destination addresses per tile, read-only store lookups,
      // and a task-local `bound` rather than the old shared, rebound
      // `members` fields. `maskTile` is looked up fresh per tile inside the
      // worker (one hash lookup per tile, exactly the cost the pre-parallel
      // walk already paid -- only which thread pays it changed).
      parallelFor(toVisit.size(), g_compositeParallelGrain, [&](size_t idx) {
        const TileCoord coord = toVisit[idx].first;
        const Tile& tile = *toVisit[idx].second;
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
        // **The base's tiles are the clipping run's whole extent** (§17):
        // outside them the base's alpha is 0, so the group contributes exactly
        // nothing. A clipped layer's own tiles are never visited for their own
        // sake, which is why clipping can only make this walk cheaper.
        const std::vector<BoundMember> bound =
            members.empty() ? std::vector<BoundMember>{} : bindMembersForTile(coord);
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
            contribute(docX, docY, tile.readPixel(local), effective, local, bound);
          }
        }
      });
    } else {
      std::vector<std::pair<TileCoord, const PigmentTile*>> toVisit;
      for (const auto& [coord, tile] : *layer.pigmentTiles) {
        if (visits(coord) && i >= floorAt(coord)) toVisit.emplace_back(coord, &tile);
      }
      parallelFor(toVisit.size(), g_compositeParallelGrain, [&](size_t idx) {
        const TileCoord coord = toVisit[idx].first;
        const PigmentTile& tile = *toVisit[idx].second;
        const PixelCoord origin = tileOrigin(coord);
        const MaskTile* maskTile = maskTiles ? maskTiles->find(coord) : nullptr;
        const std::vector<BoundMember> bound =
            members.empty() ? std::vector<BoundMember>{} : bindMembersForTile(coord);
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
            contribute(docX, docY, projectPigmentTexel(tile.readTexel(local)), effective, local,
                      bound);
          }
        }
      });
    }
  }
}

}  // namespace

void compositeDocumentPremultipliedInto(const Document& doc, std::vector<float>& buffer,
                                        std::vector<std::string>* warningsOut) {
  if (doc.width <= 0 || doc.height <= 0) {
    buffer.clear();
    return;
  }

  const size_t n = static_cast<size_t>(doc.width) * static_cast<size_t>(doc.height) * 4;
  // Zero-filled: an untouched pixel is transparent black, exactly what
  // core::Tile gives an unwritten texel, so nothing needs a separate "was
  // anything here" flag -- and, per core/Blend.hpp's transparent-backdrop
  // identity, an all-zero accumulator composites the first contributing layer
  // through unchanged **under every mode**, not only under `over`.
  //
  // `buffer` keeps its allocation when the size already matches -- the
  // repeat-call case this function exists for -- and is only reallocated on
  // a size mismatch (the first call, or a canvas-size change), via
  // `assign()`, which zero-fills exactly as the constructor below did. A
  // buffer that already has the right size is re-zeroed with `std::fill`
  // instead: still a full pass over the buffer (see this function's own
  // header comment on why that pass cannot be skipped), just not an
  // allocation.
  if (buffer.size() != n) {
    buffer.assign(n, 0.0f);
  } else {
    std::fill(buffer.begin(), buffer.end(), 0.0f);
  }

  CompositeRegion region;
  region.pixels = buffer.data();
  region.origin = PixelCoord{0, 0};
  region.width = doc.width;
  region.height = doc.height;
  // `nullptr` is "every tile a store holds", which is byte-for-byte the walk
  // this function performed before the incremental path existed.
  compositeWalk(doc, nullptr, region, warningsOut);
}

std::vector<float> compositeDocumentPremultiplied(const Document& doc,
                                                  std::vector<std::string>* warningsOut) {
  // A fresh, empty buffer every call, so this is exactly the one-shot
  // allocate-and-zero-and-walk `compositeDocumentPremultipliedInto()`
  // documents itself as reducing to when its caller does not reuse `buffer`
  // -- the two functions cannot drift because this *is* that function.
  std::vector<float> out;
  compositeDocumentPremultipliedInto(doc, out, warningsOut);
  return out;
}

void compositeDocumentTilesPremultiplied(const Document& doc, const std::vector<TileCoord>& tiles,
                                         const CompositeRegion& region,
                                         std::vector<std::string>* warningsOut) {
  if (doc.width <= 0 || doc.height <= 0) return;
  // An **empty** tile set with a zero-sized region is legal and is how a
  // caller asks for nothing but the warnings: no tile is visited, so `accum()`
  // is never reached and `region.pixels` is never dereferenced -- and the
  // per-layer warnings below are emitted in full, because they were never a
  // property of which tiles a walk visited. ui/DocumentTexture uses it for the
  // edit that moved the revision and moved no texel.
  if (!tiles.empty() && (region.pixels == nullptr || region.width <= 0 || region.height <= 0))
    return;

  // The accumulator starts at transparent black for the same reason the full
  // walk's buffer does, and only over the tiles being recomposited -- a texel
  // outside them is the caller's and is not touched at all. Done here rather
  // than in the caller so that "an untouched texel is transparent black" has
  // one home.
  for (const TileCoord& coord : tiles) {
    const PixelCoord origin = tileOrigin(coord);
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      const int32_t docY = origin.y + ty;
      if (docY < 0 || docY >= doc.height) continue;
      const int32_t ry = docY - region.origin.y;
      if (ry < 0 || ry >= region.height) continue;
      // The run of texels this tile row contributes, clipped to the canvas and
      // to the region, cleared in one pass rather than one texel at a time.
      const int32_t x0 = std::max({origin.x, 0, region.origin.x});
      const int32_t x1 = std::min({origin.x + kTileSize, doc.width,
                                   region.origin.x + region.width});
      if (x1 <= x0) continue;
      float* row = region.pixels + (static_cast<size_t>(ry) * static_cast<size_t>(region.width) +
                                    static_cast<size_t>(x0 - region.origin.x)) *
                                       4u;
      std::fill(row, row + static_cast<size_t>(x1 - x0) * 4u, 0.0f);
    }
  }

  const std::unordered_set<TileCoord> only(tiles.begin(), tiles.end());
  compositeWalk(doc, &only, region, warningsOut);
}

}  // namespace np
