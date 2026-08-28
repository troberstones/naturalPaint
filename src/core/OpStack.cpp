#include "core/OpStack.hpp"

#include <cmath>
#include <cstddef>
#include <utility>

#include "color/Shaper.hpp"
#include "core/Premultiply.hpp"

namespace np {
namespace {

// Turns one PointA-classed, enabled Op into a callable ops::PointOp bound to
// that entry's own params -- the bridge detectRuns() uses internally to
// build each run's composed op list. A free function rather than a member,
// matching ops/PointOps.cpp's own precedent of keeping small internal
// helpers in an anonymous namespace rather than as class methods.
//
// Precondition: `op.opClass == OpClass::PointA`. Never called otherwise --
// detectRuns() only calls this from inside its PointA run-building loop, on
// entries it has already confirmed are class PointA (and, separately,
// enabled -- see detectRuns()'s own call site).
PointOp toPointOp(const Op& op) {
  switch (op.pointKind) {
    case PointOpKind::Levels: {
      const std::array<LevelsParams, 3> p = op.levels;
      return [p](const std::array<float, 3>& rgb) { return applyLevels(rgb, p); };
    }
    case PointOpKind::Curves: {
      const std::array<Curve, 3> p = op.curves;
      return [p](const std::array<float, 3>& rgb) { return applyCurves(rgb, p); };
    }
    case PointOpKind::Exposure: {
      const ExposureParams p = op.exposure;
      return [p](const std::array<float, 3>& rgb) { return applyExposure(rgb, p); };
    }
    case PointOpKind::Saturation: {
      const SaturationParams p = op.saturation;
      return [p](const std::array<float, 3>& rgb) { return applySaturation(rgb, p); };
    }
    case PointOpKind::Grayscale: {
      const GrayscaleParams p = op.grayscale;
      return [p](const std::array<float, 3>& rgb) { return applyGrayscale(rgb, p); };
    }
    case PointOpKind::ChannelMixer: {
      const ChannelMixerParams p = op.channelMixer;
      return [p](const std::array<float, 3>& rgb) { return applyChannelMixer(rgb, p); };
    }
  }
  // Unreachable for a valid PointOpKind value (every enumerator is handled
  // above) -- an identity function rather than undefined behaviour if this
  // is ever somehow reached.
  return [](const std::array<float, 3>& rgb) { return rgb; };
}

}  // namespace

size_t OpStack::add(Op op) {
  ops_.push_back(std::move(op));
  ++version_;
  return ops_.size() - 1;
}

void OpStack::remove(size_t index) {
  (void)ops_.at(index);  // bounds-check -- throws std::out_of_range on misuse
  ops_.erase(ops_.begin() + static_cast<std::ptrdiff_t>(index));
  ++version_;
}

void OpStack::reorder(size_t from, size_t to) {
  (void)ops_.at(from);  // bounds-check both indices before mutating anything
  (void)ops_.at(to);
  Op moved = std::move(ops_[from]);
  ops_.erase(ops_.begin() + static_cast<std::ptrdiff_t>(from));
  ops_.insert(ops_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
  ++version_;
}

void OpStack::setEnabled(size_t index, bool enabled) {
  ops_.at(index).enabled = enabled;
  ++version_;
}

void OpStack::setOp(size_t index, Op op) {
  ops_.at(index) = std::move(op);
  ++version_;
}

std::vector<OpRun> OpStack::detectRuns() const {
  std::vector<OpRun> runs;
  const size_t n = ops_.size();
  size_t i = 0;
  while (i < n) {
    if (ops_[i].opClass != OpClass::PointA) {
      ++i;
      continue;
    }
    OpRun run;
    run.startIndex = i;
    // A disabled PointA entry does NOT end the while condition below (it
    // only gates whether toPointOp() is called) -- see OpStack.hpp's
    // detectRuns() doc comment for why op *class*, not *enabled state*, is
    // what defines a run boundary.
    while (i < n && ops_[i].opClass == OpClass::PointA) {
      if (ops_[i].enabled) run.ops.push_back(toPointOp(ops_[i]));
      ++i;
    }
    run.endIndex = i;
    runs.push_back(std::move(run));
  }
  return runs;
}

const char* pointOpKindName(PointOpKind kind) noexcept {
  switch (kind) {
    case PointOpKind::Levels:       return "Levels";
    case PointOpKind::Curves:       return "Curves";
    case PointOpKind::Exposure:     return "Exposure";
    case PointOpKind::Saturation:   return "Saturation";
    case PointOpKind::Grayscale:    return "Grayscale";
    case PointOpKind::ChannelMixer: return "Channel Mixer";
  }
  // Unreachable for a valid enumerator; a name rather than a crash if a value
  // is ever cast in from outside the enum, which is what a serialised file's
  // kind code is before io/OpSerial validates it.
  return "?";
}

std::string opDisplayName(const Op& op) {
  switch (op.opClass) {
    case OpClass::PointA:   return pointOpKindName(op.pointKind);
    case OpClass::SpatialB: return "spatial op";
    case OpClass::StrokeC:  return "stroke op";
    case OpClass::BakedD:   return "baked op";
    case OpClass::Unknown:
      // PRD I10's entry-level case. The bytes are carried verbatim and their
      // size is the only thing this build honestly knows about the op, so it
      // is what the name says -- a row reading "unrecognised op (48 bytes)" is
      // the difference between a preserved op and a lost one being visible.
      return "unrecognised op (" + std::to_string(op.unrecognised.size()) + " bytes)";
  }
  return "?";
}

// docs/architecture-review.md P0-5. See OpStack.hpp's doc comment on this
// function for the full "why a switch instead of the toPointOp() closure
// above" reasoning; this is the switch itself, one arm per PointOpKind,
// each calling the exact same ops/PointOps.hpp function toPointOp()'s
// matching lambda would have called -- the math is identical, only the
// dispatch mechanism differs, which is what keeps this a performance-only
// change (a document composites to the same bytes either way, verified by
// runGradeDispatchTest()'s regression check against the closure path,
// app/selftest/GradeDispatch.cpp).
std::array<float, 3> applyOpDirect(const std::array<float, 3>& rgb, const Op& op) noexcept {
  switch (op.pointKind) {
    case PointOpKind::Levels:       return applyLevels(rgb, op.levels);
    case PointOpKind::Curves:       return applyCurves(rgb, op.curves);
    case PointOpKind::Exposure:     return applyExposure(rgb, op.exposure);
    case PointOpKind::Saturation:   return applySaturation(rgb, op.saturation);
    case PointOpKind::Grayscale:    return applyGrayscale(rgb, op.grayscale);
    case PointOpKind::ChannelMixer: return applyChannelMixer(rgb, op.channelMixer);
  }
  // Unreachable for a valid PointOpKind value (every enumerator handled
  // above, and -Werror=switch keeps that exhaustive); identity rather than
  // undefined behaviour if this is ever somehow reached, matching
  // toPointOp()'s own unreachable-fallback convention just above.
  return rgb;
}

std::array<float, 4> applyOpsPremultiplied(const std::array<float, 4>& premultiplied,
                                            const std::vector<Op>& ops) {
  // Identical bracket to ops::applyPointOpsPremultiplied() (ops/PointOps.cpp)
  // -- see that function's own doc comment for why `a <= 0` short-circuits
  // and why alpha is never written by the loop below.
  const std::array<float, 4> undone = unpremultiply(premultiplied);
  const float a = undone[3];
  if (a <= 0.0f) return undone;

  std::array<float, 3> straight{undone[0], undone[1], undone[2]};
  for (const Op& op : ops) straight = applyOpDirect(straight, op);

  return {straight[0] * a, straight[1] * a, straight[2] * a, a};
}

// =============================================================================
// applyOpsPremultipliedBatch() -- op-outer, texel-inner batching
// =============================================================================
//
// See OpStack.hpp's doc comment on applyOpsPremultipliedBatch() for the
// contract and the bit-exactness argument. This is the "how".
//
// --- AoS -> SoA, and why -----------------------------------------------------
//
// `texels` arrives as `std::array<float,4>` per texel (AoS) -- the natural
// shape for a tile buffer, and the shape applyOpsPremultiplied() itself
// takes. Left in that layout, each op's loop would stride 4 floats to reach
// the next texel's same channel, which is exactly the shape autovectorizers
// handle least reliably (it needs to prove a 4-wide deinterleave is safe and
// worth it, rather than trusting the loop is already contiguous). Unpacked
// once into four separate contiguous arrays -- r[], g[], b[], a[] -- every
// per-op loop below reads and writes unit-stride float arrays, which is the
// shape LLVM's autovectorizer targets first and most reliably. The transpose
// is paid once per batch call, not once per op, so it amortises over
// |ops| -- the same amortisation principle the op-outer restructuring itself
// is built on.
//
// --- The `a <= 0` guard, without a per-lane branch in every op's loop ------
//
// applyOpsPremultiplied()'s single-texel bracket short-circuits transparent
// texels (`a <= 0` -> return {0,0,0,0} unchanged, never touching `ops` at
// all). A batch mixes transparent and opaque texels in the same run, so that
// branch can't be hoisted out of the texel loop the way an op's own
// parameters can. Rather than testing it inside every op's hot loop (which
// would turn every one of the four vectorised loops below into a masked,
// harder-to-vectorise one), this function runs every op over every lane
// unconditionally -- including transparent ones, whose straight RGB is
// already {0,0,0,0} from unpremultiply() and which an op like ChannelMixer's
// nonzero offset row can, correctly per the ops/PointOps.hpp contract, move
// away from zero -- and then overwrites exactly the transparent lanes back
// to {0,0,0,0} in the repacking pass at the very end. That epilogue is
// O(n) with no op-chain work behind it (unlike the alternative of masking
// inside every op), and it reproduces applyOpsPremultiplied()'s per-texel
// `a <= 0` result exactly: a transparent texel's fate never depended on
// what `ops` computed in the first place.
namespace {

// Levels and Curves are deliberately written as plain per-lane scalar loops
// -- calling the *exact* scalar functions applyOpDirect() itself calls
// (applyLevelsChannel(), evalCurve() bracketed by shaperEncode/shaperDecode)
// -- not because they can't be hand-vectorised, but because their dominant
// cost is a transcendental this build has no vectorised libm for (no
// SLEEF/libmvec dependency, and none is being added -- "prefer plain C++ the
// compiler vectorises" does not extend to hand-rolling a polynomial
// approximation of pow/log2/exp2 whose last bit would then have to be proven
// identical to libm's own, a strictly larger and riskier piece of work than
// this task's scope).
//
// **What actually happens is worth recording, because it is not "fully
// scalar" either.** Checked in the generated assembly (`otool -tV`, arm64),
// not assumed: LLVM's loop vectoriser *does* pack these loops' surrounding
// arithmetic -- Levels' `(input-blackIn)/range` and the two clamps,
// Curves'/the shaper's linear-vs-log branch select -- into 4-wide NEON
// groups, four lanes at a time. What it can't vectorise is the call in the
// middle: for each 4-wide group it extracts each lane back to a scalar
// register, calls `powf`/`log2f`/`exp2f` (a real libm call, `bl` to a
// dyld stub -- confirmed by symbol name, not inferred from instruction
// shape) once per lane, and re-inserts the four results before continuing
// the vectorised tail. Curves' evalCurve() (a control-point walk, not a
// single libm call) doesn't even get this much: it stays an out-of-line
// `bl` to evalCurve() itself, one call per lane, unchanged from the
// per-texel switch. Either way the transcendental/walk is still paid once
// per lane, which is the expensive part -- so the measured speedup for
// these two ops is modest (~1.06-1.07x, see GradeDispatch.cpp's per-op
// breakdown), from loop/parameter hoisting rather than from SIMD doing
// their dominant cost in parallel. This is still correct to call
// "batched" (op-outer, texel-inner, parameters hoisted once) and still
// bit-exact by construction (same scalar calls, same values, only the
// surrounding shuffle differs) -- it is just not the 4x this file's other
// four ops get, and that gap is the honest, measured answer, not a bug.
void applyLevelsBatch(float* r, float* g, float* b, size_t n,
                      const std::array<LevelsParams, 3>& p) {
  for (size_t i = 0; i < n; ++i) r[i] = applyLevelsChannel(r[i], p[0]);
  for (size_t i = 0; i < n; ++i) g[i] = applyLevelsChannel(g[i], p[1]);
  for (size_t i = 0; i < n; ++i) b[i] = applyLevelsChannel(b[i], p[2]);
}

void applyCurvesBatch(float* r, float* g, float* b, size_t n,
                      const std::array<Curve, 3>& channels) {
  float* const planes[3] = {r, g, b};
  for (int c = 0; c < 3; ++c) {
    const Curve& curve = channels[static_cast<size_t>(c)];
    // Identity: skip the shaper round-trip entirely, same as applyCurves()
    // -- see PointOps.hpp's evalCurve() doc comment for why this is exact
    // rather than relying on shaperDecode(shaperEncode(x)) == x.
    if (curve.size() < 2) continue;
    float* plane = planes[c];
    for (size_t i = 0; i < n; ++i) {
      const float shapedIn = shaperEncode(plane[i]);
      const float shapedOut = evalCurve(curve, shapedIn);
      plane[i] = shaperDecode(shapedOut);
    }
  }
}

// Pure elementwise multiply, independent per lane -- the textbook
// autovectorisable shape. `__restrict` tells the compiler the three
// pointers cannot alias each other (they are three distinct
// std::vector<float>'s own backing storage), which is what lets it emit
// unit-stride SIMD loads/stores instead of conservatively re-checking
// aliasing on every iteration.
void applyExposureBatch(float* __restrict r, float* __restrict g, float* __restrict b, size_t n,
                        const ExposureParams& p) {
  const float mult = std::exp2(p.stops);
  for (size_t i = 0; i < n; ++i) r[i] *= mult;
  for (size_t i = 0; i < n; ++i) g[i] *= mult;
  for (size_t i = 0; i < n; ++i) b[i] *= mult;
}

// Same expression, same left-to-right evaluation order as
// ops::computeLuma()/applySaturation() (ops/PointOps.cpp) -- `luma` is read
// from all three original channels before any of the three are overwritten
// (each lane's own r[i]/g[i]/b[i] read happens before that lane's own
// write), so this is bit-identical per lane to the scalar function, just
// |n| of them run side by side instead of one at a time.
void applySaturationBatch(float* __restrict r, float* __restrict g, float* __restrict b, size_t n,
                          const SaturationParams& p) {
  const std::array<float, 3> w = p.lumaWeights;
  const float scale = p.scale;
  for (size_t i = 0; i < n; ++i) {
    const float luma = w[0] * r[i] + w[1] * g[i] + w[2] * b[i];
    const float rr = r[i], gg = g[i], bb = b[i];
    r[i] = luma + (rr - luma) * scale;
    g[i] = luma + (gg - luma) * scale;
    b[i] = luma + (bb - luma) * scale;
  }
}

void applyGrayscaleBatch(float* __restrict r, float* __restrict g, float* __restrict b, size_t n,
                         const GrayscaleParams& p) {
  const std::array<float, 3> w = p.lumaWeights;
  for (size_t i = 0; i < n; ++i) {
    const float luma = w[0] * r[i] + w[1] * g[i] + w[2] * b[i];
    r[i] = luma;
    g[i] = luma;
    b[i] = luma;
  }
}

// Same left-to-right accumulation order as applyChannelMixer()'s
// `row[0]*rgb[0] + row[1]*rgb[1] + row[2]*rgb[2] + row[3]` -- all three
// outputs are computed from `rr`/`gg`/`bb` (each lane's original, pre-this-op
// values) before any of r[i]/g[i]/b[i] is overwritten, matching the scalar
// function reading `rgb` once and returning a fresh array rather than
// mutating it mid-computation.
void applyChannelMixerBatch(float* __restrict r, float* __restrict g, float* __restrict b,
                            size_t n, const ChannelMixerParams& p) {
  const std::array<std::array<float, 4>, 3> m = p.matrix;
  for (size_t i = 0; i < n; ++i) {
    const float rr = r[i], gg = g[i], bb = b[i];
    r[i] = m[0][0] * rr + m[0][1] * gg + m[0][2] * bb + m[0][3];
    g[i] = m[1][0] * rr + m[1][1] * gg + m[1][2] * bb + m[1][3];
    b[i] = m[2][0] * rr + m[2][1] * gg + m[2][2] * bb + m[2][3];
  }
}

}  // namespace

void applyOpsPremultipliedBatch(std::span<std::array<float, 4>> texels,
                                 const std::vector<Op>& ops) {
  const size_t n = texels.size();
  if (n == 0) return;

  // AoS -> SoA, straight (un-premultiplied) domain -- see this section's
  // header comment above for why. `unpremultiply()`'s own `a <= 0` guard
  // runs here, per lane; a transparent lane's r/g/b/a all land at exactly
  // 0.0f, which is what the repacking epilogue below keys off to reproduce
  // applyOpsPremultiplied()'s per-texel short-circuit without a per-op
  // branch.
  std::vector<float> rBuf(n), gBuf(n), bBuf(n), aBuf(n);
  float* const r = rBuf.data();
  float* const g = gBuf.data();
  float* const b = bBuf.data();
  float* const a = aBuf.data();
  for (size_t i = 0; i < n; ++i) {
    const std::array<float, 4> undone = unpremultiply(texels[i]);
    r[i] = undone[0];
    g[i] = undone[1];
    b[i] = undone[2];
    a[i] = undone[3];
  }

  // Op-outer, texel-inner: each op's own switch arm and parameter unpack
  // happen once, here, not once per texel.
  for (const Op& op : ops) {
    switch (op.pointKind) {
      case PointOpKind::Levels:       applyLevelsBatch(r, g, b, n, op.levels); break;
      case PointOpKind::Curves:       applyCurvesBatch(r, g, b, n, op.curves); break;
      case PointOpKind::Exposure:     applyExposureBatch(r, g, b, n, op.exposure); break;
      case PointOpKind::Saturation:   applySaturationBatch(r, g, b, n, op.saturation); break;
      case PointOpKind::Grayscale:    applyGrayscaleBatch(r, g, b, n, op.grayscale); break;
      case PointOpKind::ChannelMixer: applyChannelMixerBatch(r, g, b, n, op.channelMixer); break;
      // No `default:` -- -Werror=switch keeps this exhaustive over
      // PointOpKind the same way applyOpDirect()'s switch already is.
    }
  }

  // SoA -> AoS, re-premultiplied, plus the transparent-lane epilogue.
  //
  // **The epilogue is redundant with the unpack above, deliberately, and it
  // is worth saying so rather than leaving a reader to assume it is what
  // makes transparent lanes correct.** `unpremultiply()` zeroes a lane's
  // ALPHA as well as its RGB for `a <= 0`, so the `else` branch below would
  // already write `{r*0, g*0, b*0, 0}` = `{0,0,0,0}` for such a lane no
  // matter what the op chain did to its straight RGB. Removing this `if`
  // entirely reddens nothing, and so does making the unpack read raw alpha
  // instead of unpremultiplied alpha -- both measured, not assumed.
  // Breaking BOTH reddens all seven of app/selftest/GradeDispatch.cpp's
  // batch-vs-scalar comparisons, which is the honest description: two
  // mechanisms, jointly load-bearing, individually untestable while the
  // other stands.
  //
  // It is kept because the redundancy is free (one predicted branch per
  // texel, outside every op's hot loop) and because it is the mechanism
  // that would still hold if a future op could return a non-finite straight
  // value: `inf * 0.0f` is NaN, not 0. No op in the current set can --
  // `applyLevelsChannel()` floors its range and clamps its normalised `t`
  // before `pow`, and the other five are finite-in/finite-out for finite
  // params -- so today this is defence against a change, not against an
  // input.
  for (size_t i = 0; i < n; ++i) {
    if (a[i] <= 0.0f) {
      texels[i] = {0.0f, 0.0f, 0.0f, 0.0f};
    } else {
      texels[i] = {r[i] * a[i], g[i] * a[i], b[i] * a[i], a[i]};
    }
  }
}

}  // namespace np
