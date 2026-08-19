// ACEScct log encode/decode -- WGSL port of color/Shaper.cpp's shaperEncode()
// / shaperDecode() (S-2016-005). Constants reproduced exactly from that file,
// not re-derived, not re-rounded -- see color/Shaper.hpp/.cpp for the full
// derivation, the breakpoint value/derivative continuity proof, and why
// ACEScct specifically. One WGSL copy, shared by every LUT-bake kernel that
// needs it (PLAN.md Phase 3 step 4, ADR-0004
// docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md) via
// //#include "include/shaper.wgsl" -- not one hand-copy per kernel file,
// matching this directory's own mixbox.wgsl precedent for shared, no-binding
// WGSL math.
//
// *** Do not change these constants without reading color/Shaper.cpp's own
// header comment and ADR-0004 in full *** -- a saved document's curve
// control points are coordinates in this exact domain; changing the shaper
// later silently reshapes every saved grade.

const kShaperBreakLin: f32 = 0.0078125;              // 2^-7
const kShaperBreakShaped: f32 = 0.1552511415525113;  // == shaperEncode(kShaperBreakLin)
const kShaperSlopeA: f32 = 10.5402377416545;
const kShaperOffsetB: f32 = 0.0729055341958355;
const kShaperLogA: f32 = 9.72;
const kShaperLogB: f32 = 17.52;

// Scene-linear -> shaper-domain (ACEScct log encode). Scalar, matching
// color/Shaper.hpp's own "no clamping, no vector type" precedent -- callers
// apply this per R/G/B channel.
fn shaperEncode(linearVal: f32) -> f32 {
  if (linearVal <= kShaperBreakLin) {
    return kShaperSlopeA * linearVal + kShaperOffsetB;
  }
  return (log2(linearVal) + kShaperLogA) / kShaperLogB;
}

// Shaper-domain -> scene-linear (ACEScct log decode, the exact inverse).
fn shaperDecode(shapedVal: f32) -> f32 {
  if (shapedVal <= kShaperBreakShaped) {
    return (shapedVal - kShaperOffsetB) / kShaperSlopeA;
  }
  return exp2(shapedVal * kShaperLogB - kShaperLogA);
}
