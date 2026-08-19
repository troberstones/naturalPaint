#include "color/Shaper.hpp"

#include <cmath>

namespace np {
namespace {

// ACEScct (S-2016-005) constants, reproduced exactly from the published
// spec -- not re-derived, not approximated, not adjusted. See Shaper.hpp
// for why ACEScct and not a bespoke curve.
//
// Piecewise: a linear segment at and below kBreakLin (2^-7, the spec's own
// breakpoint in linear light), and a log2 segment above it. The two
// segments and their derivatives are both continuous at the breakpoint --
// this is what makes them one smooth curve rather than two curves that
// merely happen to touch, which is what ADR-0004's "hard to reverse"
// warning depends on (a kink at the breakpoint would visibly distort every
// curve authored across it). Verified here by direct hand computation
// before landing, the same discipline core/Half.cpp's floatToHalf and
// app/ViewTransform.cpp's matrix derivation already apply in this codebase:
//
// Value continuity at lin = kBreakLin = 2^-7:
//   linear segment:  kSlopeA * 2^-7 + kOffsetB
//                   = 10.5402377416545 / 128 + 0.0729055341958355
//                   = 0.0823456073566758 + 0.0729055341958355
//                   = 0.1552511415525113                          (*)
//   log segment:     (log2(2^-7) + kLogA) / kLogB
//                   = (-7 + 9.72) / 17.52
//                   = 2.72 / 17.52
//                   = 0.15525114155251146...                       (*)
// (*) agree to 1.7e-16 -- double-precision rounding noise, i.e. exact.
// kBreakShaped below is this shared value, computed once so shaperDecode()
// doesn't have to call log2() just to find its own branch boundary.
//
// Derivative continuity at the same point:
//   linear segment:  d/dlin [kSlopeA * lin + kOffsetB] = kSlopeA
//                   = 10.5402377416545
//   log segment:     d/dlin [(log2(lin) + kLogA) / kLogB]
//                   = 1 / (lin * ln2 * kLogB)
//                   at lin = 2^-7:
//                   = 2^7 / (ln2 * kLogB)
//                   = 128 / (0.69314718055994530942 * 17.52)
//                   = 128 / 12.14393860341024182
//                   = 10.540237741654527...
// Agrees with kSlopeA to 2.7e-14 -- again exact to double precision. This
// is the property that proves the constants are genuinely self-consistent
// (the real published spec values), not merely plausible-looking; both
// checks above are pinned permanently by runShaperTest() in
// app/SelfTest.cpp, which re-evaluates both branches independently of this
// file rather than only calling the functions below once.
//
// Known-value sanity check, independent of self-consistency: at lin = 1.0
// (linear > kBreakLin, so the log branch applies), shaperEncode(1.0) =
// (log2(1) + 9.72) / 17.52 = 9.72 / 17.52 = 0.5547945205479452...,
// hand-computable from the spec constants alone.
constexpr float kBreakLin = 0.0078125f;              // 2^-7
constexpr float kBreakShaped = 0.1552511415525113f;  // == shaperEncode(kBreakLin)
constexpr float kSlopeA = 10.5402377416545f;
constexpr float kOffsetB = 0.0729055341958355f;
constexpr float kLogA = 9.72f;
constexpr float kLogB = 17.52f;

}  // namespace

float shaperEncode(float linear) noexcept {
  if (linear <= kBreakLin) return kSlopeA * linear + kOffsetB;
  return (std::log2(linear) + kLogA) / kLogB;
}

float shaperDecode(float shaped) noexcept {
  if (shaped <= kBreakShaped) return (shaped - kOffsetB) / kSlopeA;
  return std::exp2(shaped * kLogB - kLogA);
}

}  // namespace np
