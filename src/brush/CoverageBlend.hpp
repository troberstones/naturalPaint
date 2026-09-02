#pragma once

#include <cstdint>

namespace np {

// brush/CoverageBlend -- how two coverage values combine into one.
//
// **One enum and one function for BOTH the Dual Brush and the Texture panel**,
// because they are the same question asked in two panels: take the tip's
// coverage and a second coverage, return one. Photoshop names them separately
// (`BlnM` on the `dualBrush` object, `textureBlendMode` beside `Txtr`) and
// draws them in different places, but the ids overlap heavily and the
// arithmetic is identical. Two enums would mean two switches, two sets of
// formulas, and two places to add Linear Burn.
//
// Measured need across the four packs on this machine:
//
//   `BlnM` (66 dual-brush presets):  linearHeight 29, Ovrl 11, hardMix 10,
//                                    CBrn 8, linearBurn 4, hMix 2, CDdg 1,
//                                    Mltp 1
//   `textureBlendMode` (84):         Hght 31, Sbtr 29, CBrn 15, hardMix 4,
//                                    CDdg 2, linearHeight 2, Drkn 1
//
// Lives in its own header rather than in brush/Deposit.hpp or brush/Grain.hpp
// because both of those need it and Deposit already includes Grain -- putting
// it in either would either invert that edge or force the other to reach
// through a header that is not about blending.

enum class CoverageBlend : uint8_t {
  // The four this build has composited since Dual Brush arrived. Their
  // formulas are unchanged and their arithmetic is bit-identical.
  Multiply,   // Mltp
  Overlay,    // Ovrl
  ColorBurn,  // CBrn
  HardMix,    // hMix / hardMix

  // Standard Photoshop separable blends, exact on scalars.
  LinearBurn,  // linearBurn
  ColorDodge,  // CDdg
  Darken,      // Drkn
  Subtract,    // Sbtr

  // **Height is the expired patent's mechanism, not Adobe's.** US 5,347,620
  // (Zimmer, expired, public domain -- docs/brush-model-references.md) gives
  // the paper-tooth overlay as `F = clamp(P*S*O1 - G, 0, 1)` where `G` is the
  // grain surface height at that pixel: deep valleys fill, peaks get skipped.
  // That is a height-map combination with a published mechanism this project
  // is licensed to implement from, and it is what brush/Grain has always
  // computed.
  //
  // **On a scalar field that is the same arithmetic as Subtract, and
  // Photoshop distinguishes the two.** This build does not reproduce the
  // difference; both ids resolve to the same formula and the import report
  // counts them so the approximation is visible rather than assumed harmless.
  Height,  // Hght

  // **LinearHeight is REFUSED, deliberately, and `applyCoverageBlend()` will
  // not compute it.** It is a height-map blend from the Texture panel with no
  // per-pixel formula in any source consulted, and it is the single largest
  // unmapped mode: 29 of the 66 dual-brush presets name it. brush/Deposit.hpp
  // has refused it since Dual Brush shipped, for the reason that still holds
  // -- a wrong guess here would PAINT, and painting a guess is worse than
  // reporting a gap. It is a member of this enum so the importer can name what
  // it found; `coverageBlendIsRenderable()` is what callers ask before using
  // one.
  LinearHeight,  // linearHeight
};

const char* coverageBlendName(CoverageBlend blend) noexcept;

// Whether `applyCoverageBlend()` has a formula for this mode. False only for
// `LinearHeight`. A caller that gets false must not substitute a different
// blend -- report the gap instead.
bool coverageBlendIsRenderable(CoverageBlend blend) noexcept;

// `base` is the primary coverage, `second` the thing being combined into it --
// the dual brush's second tip, or the paper's grain height. Both in [0,1];
// the result is clamped to [0,1].
//
// `LinearHeight` returns `base` unchanged, so a caller that ignores
// `coverageBlendIsRenderable()` gets the primary tip alone rather than a
// guess. That is a fallback, not a formula, and it is not a licence to skip
// the check: the report counts what was refused, and it can only count what
// the importer told it.
float applyCoverageBlend(CoverageBlend blend, float base, float second) noexcept;

}  // namespace np
