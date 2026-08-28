#include "ops/AutoLevels.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "color/LutBake.hpp"
#include "color/Shaper.hpp"
#include "color/Space.hpp"

namespace np {
namespace {

// A per-tail clip fraction of 0.5 or more discards at least half of every
// channel's pixels from EACH end, which leaves no bin that both tails agree
// is "kept" -- clamped away from that degenerate zone rather than letting
// computeClipIndices() below hunt for an occupied range that may not exist.
// 0.49 rather than exactly 0.5 keeps the clamp strict (< 0.5), matching
// AutoLevelsParams::clipFraction's own doc comment.
float clampClipFraction(float clipFraction) noexcept {
  return std::clamp(clipFraction, 0.0f, 0.49f);
}

// Bin `i`'s representative DISPLAY-domain value: the midpoint of the
// half-open interval `[i/binCount, (i+1)/binCount)` core/Histogram.cpp's own
// `binIndex()` maps every value in that interval down to -- see
// AutoLevels.hpp's header comment for why this file reads Histogram.cpp
// directly rather than assuming the mapping from its header prose.
float binCenterDisplay(int32_t binIndex, int32_t binCount) noexcept {
  return (static_cast<float>(binIndex) + 0.5f) / static_cast<float>(binCount);
}

// `binCenterDisplay()`, un-encoded back to the scene-linear domain
// LevelsParams::blackIn/whiteIn actually live in (ops/PointOps.hpp's own
// "scene-linear RGB" contract) -- the one domain crossing every solver in
// this file needs, centralised here so it happens exactly once.
float binToLinear(int32_t binIndex, int32_t binCount) noexcept {
  return srgbDecode(binCenterDisplay(binIndex, binCount));
}

// The clip-trimmed occupied range of one channel's histogram: the lowest
// and highest bin index that survive discarding `clipFraction` of that
// channel's own pixel count from EACH tail independently (see
// AutoLevelsParams's doc comment for why "each tail independently" is this
// file's stated convention).
struct ClipIndices {
  int32_t loBin = 0;
  int32_t hiBin = 0;
  // Total pixel count across every bin -- 0 means "nothing to search";
  // every solver below treats that as its own degenerate case rather than
  // trusting loBin/hiBin to mean anything.
  uint64_t total = 0;
};

// Finds `[loBin, hiBin]` by walking the cumulative count in from each end
// until it exceeds `clipFraction * total`: the standard "clip the outermost
// N% of pixels, then take the extremes of what's left" search every
// solver in this file shares. Both loops share one invariant worth stating
// explicitly, since solveEqualize() below leans on it: the bin a loop
// STOPS on is never itself empty. The loop only stops the first time its
// running cumulative sum exceeds `clipCount`; if the bin it just added had
// count 0, the sum would not have changed, so the sum must already have
// exceeded `clipCount` on a PRIOR iteration -- which would have stopped the
// loop already. So `bins[loBin] > 0` and `bins[hiBin] > 0` always hold
// whenever `total > 0` (the only case the loops run at all).
ClipIndices computeClipIndices(const std::vector<uint64_t>& bins, float clipFractionRaw) {
  const int32_t binCount = static_cast<int32_t>(bins.size());
  if (binCount <= 0) return ClipIndices{0, 0, 0};

  uint64_t total = 0;
  for (uint64_t c : bins) total += c;
  if (total == 0) return ClipIndices{0, binCount - 1, 0};

  const float clipFraction = clampClipFraction(clipFractionRaw);
  const uint64_t clipCount = static_cast<uint64_t>(
      std::llround(static_cast<double>(clipFraction) * static_cast<double>(total)));

  int32_t loBin = 0;
  {
    uint64_t cum = 0;
    for (int32_t i = 0; i < binCount; ++i) {
      cum += bins[static_cast<size_t>(i)];
      loBin = i;
      if (cum > clipCount) break;
    }
  }
  int32_t hiBin = binCount - 1;
  {
    uint64_t cum = 0;
    for (int32_t i = binCount - 1; i >= 0; --i) {
      cum += bins[static_cast<size_t>(i)];
      hiBin = i;
      if (cum > clipCount) break;
    }
  }
  // Defensive only: with clipFraction clamped strictly below 0.5, the two
  // searches above cannot cross (each tail's clipCount is strictly under
  // half of `total`, so some bin's cumulative count exceeds it from both
  // directions at or before the same index) -- this guard exists so a
  // future change to the clamp bound fails safely (a degenerate single-bin
  // range) rather than emitting a reversed pair.
  if (hiBin < loBin) hiBin = loBin;
  return ClipIndices{loBin, hiBin, total};
}

// solveAutoTone()/solveAutoContrast()'s shared shape: clip-trimmed
// black/white points, already converted to the scene-linear domain
// LevelsParams needs.
struct ClipRange {
  float blackLinear = 0.0f;
  float whiteLinear = 1.0f;
};

// Degenerate (no data at all): identity -- blackLinear/whiteLinear at
// LevelsParams' own neutral defaults (0 and 1), so a solver that finds
// nothing to measure changes nothing, rather than guessing.
ClipRange computeClipRange(const std::vector<uint64_t>& bins, float clipFractionRaw) {
  const ClipIndices idx = computeClipIndices(bins, clipFractionRaw);
  if (idx.total == 0) return ClipRange{};
  const int32_t binCount = static_cast<int32_t>(bins.size());
  return ClipRange{binToLinear(idx.loBin, binCount), binToLinear(idx.hiBin, binCount)};
}

// solveAutoColor()'s own measurement: the clip-trimmed mean of a channel's
// occupied scene-linear values, weighted by each surviving bin's count.
struct ClippedMean {
  float mean = 0.0f;
  uint64_t count = 0;  // 0 means "no data" -- callers must not trust `mean`.
};

ClippedMean computeClippedMean(const std::vector<uint64_t>& bins, float clipFractionRaw) {
  const ClipIndices idx = computeClipIndices(bins, clipFractionRaw);
  if (idx.total == 0) return ClippedMean{};
  const int32_t binCount = static_cast<int32_t>(bins.size());

  double sum = 0.0;
  uint64_t count = 0;
  for (int32_t i = idx.loBin; i <= idx.hiBin; ++i) {
    const uint64_t c = bins[static_cast<size_t>(i)];
    if (c == 0) continue;
    sum += static_cast<double>(c) * static_cast<double>(binToLinear(i, binCount));
    count += c;
  }
  if (count == 0) return ClippedMean{};
  return ClippedMean{static_cast<float>(sum / static_cast<double>(count)), count};
}

}  // namespace

std::array<LevelsParams, 3> solveAutoTone(const HistogramResult& histogram,
                                           const AutoLevelsParams& tuning) {
  const std::array<const std::vector<uint64_t>*, 3> channels{&histogram.r, &histogram.g,
                                                               &histogram.b};
  std::array<LevelsParams, 3> out{};
  for (size_t c = 0; c < 3; ++c) {
    const ClipRange range = computeClipRange(*channels[c], tuning.clipFraction);
    LevelsParams p;
    p.blackIn = range.blackLinear;
    p.whiteIn = range.whiteLinear;
    out[c] = p;
  }
  return out;
}

std::array<LevelsParams, 3> solveAutoContrast(const HistogramResult& histogram,
                                               const AutoLevelsParams& tuning) {
  const ClipRange range = computeClipRange(histogram.luma, tuning.clipFraction);
  LevelsParams p;
  p.blackIn = range.blackLinear;
  p.whiteIn = range.whiteLinear;
  // Same LevelsParams for all three channels, by construction -- the one
  // line that is the entire hue-preservation argument in this header's own
  // doc comment.
  return {p, p, p};
}

std::array<LevelsParams, 3> solveAutoColor(const HistogramResult& histogram,
                                            const AutoLevelsParams& tuning) {
  const std::array<const std::vector<uint64_t>*, 3> channels{&histogram.r, &histogram.g,
                                                               &histogram.b};
  std::array<float, 3> meanLinear{0.0f, 0.0f, 0.0f};
  std::array<bool, 3> hasData{false, false, false};
  for (size_t c = 0; c < 3; ++c) {
    const ClippedMean m = computeClippedMean(*channels[c], tuning.clipFraction);
    meanLinear[c] = m.mean;
    hasData[c] = m.count > 0;
  }

  std::array<LevelsParams, 3> out{LevelsParams{}, LevelsParams{}, LevelsParams{}};  // neutral
  if (!hasData[0] && !hasData[1] && !hasData[2]) return out;  // no data anywhere: identity

  const float grayTarget = (meanLinear[0] + meanLinear[1] + meanLinear[2]) / 3.0f;
  if (grayTarget <= 0.0f) return out;  // all-black (or otherwise zero-mean): nothing to balance

  for (size_t c = 0; c < 3; ++c) {
    if (!hasData[c] || meanLinear[c] <= 0.0f) continue;  // this channel: leave neutral
    LevelsParams p;
    // output = clamp(input / whiteIn, 0, 1) with blackIn=0, gamma=1,
    // blackOut=0, whiteOut=1 (applyLevelsChannel(), ops/PointOps.hpp) -- a
    // pure multiply by grayTarget/meanLinear[c] in the unclamped region.
    // See this file's header comment for the full derivation.
    p.whiteIn = meanLinear[c] / grayTarget;
    out[c] = p;
  }
  return out;
}

Curve solveEqualize(const HistogramResult& histogram, const AutoLevelsParams& tuning) {
  const std::vector<uint64_t>& bins = histogram.luma;
  const ClipIndices idx = computeClipIndices(bins, tuning.clipFraction);
  // Degenerate: no data, or the clip-trimmed range collapses to a single
  // bin (nothing to redistribute) -- identity, via an empty Curve (see
  // evalCurve()'s own "0 or 1 points is identity" contract,
  // ops/PointOps.hpp).
  if (idx.total == 0 || idx.hiBin <= idx.loBin) return Curve{};

  const int32_t binCount = static_cast<int32_t>(bins.size());

  uint64_t countInRange = 0;
  for (int32_t i = idx.loBin; i <= idx.hiBin; ++i) countInRange += bins[static_cast<size_t>(i)];
  // Positive by construction: computeClipIndices()'s own doc comment proves
  // bins[loBin] > 0 whenever total > 0, which holds here.
  const uint64_t cdfMin = bins[static_cast<size_t>(idx.loBin)];

  const int32_t span = idx.hiBin - idx.loBin;  // >= 1 here (hiBin > loBin, checked above)
  const int32_t numPoints = std::min(kMaxCurvePointsPerChannel, span + 1);  // in [2, cap]

  // Strictly-increasing bin indices spanning [loBin, hiBin] inclusive, at
  // (numPoints - 1) roughly-even steps -- see AutoLevels.hpp's own doc
  // comment on solveEqualize() for why the count is capped at
  // kMaxCurvePointsPerChannel. Each candidate is clamped on both sides: not
  // less than the previous sample + 1 (keeps indices strictly increasing),
  // and not more than would leave enough room for every remaining sample to
  // still land at or before hiBin.
  std::vector<int32_t> sampleBins(static_cast<size_t>(numPoints));
  sampleBins.front() = idx.loBin;
  sampleBins.back() = idx.hiBin;
  for (int32_t k = 1; k < numPoints - 1; ++k) {
    int32_t candidate = idx.loBin + static_cast<int32_t>(std::llround(
                                         (static_cast<double>(k) * static_cast<double>(span)) /
                                         static_cast<double>(numPoints - 1)));
    candidate = std::max(candidate, sampleBins[static_cast<size_t>(k - 1)] + 1);
    candidate = std::min(candidate, idx.hiBin - (numPoints - 1 - k));
    sampleBins[static_cast<size_t>(k)] = candidate;
  }

  Curve curve;
  curve.reserve(static_cast<size_t>(numPoints));
  uint64_t cumulative = 0;
  size_t nextSample = 0;
  for (int32_t i = idx.loBin; i <= idx.hiBin && nextSample < sampleBins.size(); ++i) {
    cumulative += bins[static_cast<size_t>(i)];
    if (sampleBins[nextSample] != i) continue;
    ++nextSample;

    // The classic full-stretch equalization formula -- see AutoLevels.hpp's
    // doc comment on solveEqualize() for why the cdfMin subtraction matters
    // (it is what lets the curve's low end actually reach output 0 rather
    // than stopping at the first occupied bin's own share of the total).
    // `countInRange > cdfMin` always holds here: idx.hiBin (> idx.loBin)
    // has a positive count of its own (same computeClipIndices() proof),
    // so countInRange includes at least bins[loBin] + bins[hiBin] > cdfMin.
    const float cdfNorm = std::clamp(
        static_cast<float>(cumulative - cdfMin) / static_cast<float>(countInRange - cdfMin), 0.0f,
        1.0f);

    // x: this bin's own display value, un-done back through linear to the
    // shaper domain a Curve's control points live in (ADR-0004).
    const float xLinear = binToLinear(i, binCount);
    // y: the equalized OUTPUT display value (cdfNorm is itself already a
    // normalized position in the same [0,1] display domain histogram.luma's
    // bins are in), taken through the identical display -> linear -> shaper
    // chain so both coordinates of the control point agree on domain.
    const float yLinear = srgbDecode(cdfNorm);

    curve.push_back(CurvePoint{shaperEncode(xLinear), shaperEncode(yLinear)});
  }
  return curve;
}

}  // namespace np
