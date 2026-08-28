#include "core/OpStack.hpp"

#include <cstddef>
#include <utility>

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

}  // namespace np
