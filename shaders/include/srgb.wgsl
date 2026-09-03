// shaders/include/srgb -- the sRGB transfer function, as WGSL.
//
// A faithful port of color/Space.cpp's `srgbEncode()`/`srgbDecode()`,
// constants and all, including the two properties that are easy to drop and
// that this codebase depends on:
//
//  * **Unclamped.** Working-space values are linear light and legitimately
//    exceed 1.0 (color/Space.hpp: whether to clamp is a display/export policy
//    decision, not the colour maths'). Values above 1 pass through the same
//    power branch rather than being clipped here.
//  * **Mirrored about zero** -- `sign(x) * f(|x|)`. Negative inputs arise from
//    op headroom and from the gamut conversion at import (a Display P3 red is
//    negative in Rec.709 green and blue), and `pow()` of a negative base is
//    NaN. Mirroring is what makes a slightly negative value round-trip
//    instead of poisoning the texel.
//
// Kept in an include rather than inlined per kernel so there is one copy to
// hold against the C++, the same arrangement include/shaper.wgsl already has.

const kSrgbLinearBreak  = 0.0031308;
const kSrgbEncodedBreak = 0.04045;
const kSrgbToeSlope     = 12.92;
const kSrgbGamma        = 2.4;
const kSrgbScale        = 1.055;
const kSrgbOffset       = 0.055;

fn srgbEncode(linear : f32) -> f32 {
  var sign = 1.0;
  if (linear < 0.0) { sign = -1.0; }
  let x = abs(linear);
  var encoded : f32;
  if (x <= kSrgbLinearBreak) {
    encoded = x * kSrgbToeSlope;
  } else {
    encoded = kSrgbScale * pow(x, 1.0 / kSrgbGamma) - kSrgbOffset;
  }
  return sign * encoded;
}

fn srgbDecode(encoded : f32) -> f32 {
  var sign = 1.0;
  if (encoded < 0.0) { sign = -1.0; }
  let x = abs(encoded);
  var linear : f32;
  if (x <= kSrgbEncodedBreak) {
    linear = x / kSrgbToeSlope;
  } else {
    linear = pow((x + kSrgbOffset) / kSrgbScale, kSrgbGamma);
  }
  return sign * linear;
}
