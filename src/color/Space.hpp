#pragma once

namespace np {

// A gamut, expressed as CIE 1931 xy chromaticity coordinates for the three
// primaries plus a white point. This is the same representation OpenEXR's
// `chromaticities` attribute stores (see docs/document-format.md's table
// mapping our decisions onto EXR features) and the natural form to author,
// serialise or show in a UI. It is *not* an RGB<->XYZ matrix: deriving one
// is a job for whichever later step actually needs to adapt primaries
// (export to a different gamut, ACEScg support) — nothing in this step
// consumes a matrix, so building one now would be speculative.
struct Primaries {
  float redX = 0.0f, redY = 0.0f;
  float greenX = 0.0f, greenY = 0.0f;
  float blueX = 0.0f, blueY = 0.0f;
  float whiteX = 0.0f, whiteY = 0.0f;
};

// Rec.709 and sRGB share primaries and white point (D65) exactly — they
// differ only in transfer function, which is why there is one primaries
// constant below but four transfer functions further down. Values from the
// ITU-R BT.709 / IEC 61966-2-1 specs.
inline constexpr Primaries kRec709Primaries{
    0.640f, 0.330f,    // R
    0.300f, 0.600f,    // G
    0.150f, 0.060f,    // B
    0.3127f, 0.3290f,  // white (D65)
};

// --- Transfer functions -----------------------------------------------
//
// Each operates on one channel value; callers apply per-channel (RGB is
// three independent floats everywhere else in this codebase too — see
// paint/Palette.hpp's Pigment::rgb and MixboxLut::latentToRgb — so there is
// no existing vector type here worth wrapping these in).
//
// sRGB and Rec.709 are numerically close but genuinely different curves:
// sRGB's has a short linear "toe" segment near black with its own distinct
// slope (12.92) chosen so the curve and its derivative are continuous at
// the breakpoint; Rec.709's OETF has a toe too, but a different breakpoint
// and slope (4.5), and the two are not interchangeable to graphics-quality
// tolerance. Conflating them because the primaries match would be wrong.
//
// Not clamped. Working-space values are linear light and can legitimately
// exceed 1.0 in this pipeline (HDR-ish highlights; see docs/document-format.md
// §6 on HALF's range), and whether to clamp is a display/export policy
// decision, not something the colour math itself should silently impose —
// so values above 1.0 pass through the same formula rather than being
// clipped here. Negative input (which can arise from op headroom, e.g.
// blending) is handled by mirroring the curve about zero — sign(x)*f(|x|) —
// the same technique other linear-light pipelines (e.g. OCIO's sRGB
// transform) use, so a slightly negative value round-trips through
// encode/decode instead of hitting pow() on a negative base and coming out
// NaN.

// Linear -> sRGB-encoded.
float srgbEncode(float linear);
// sRGB-encoded -> linear.
float srgbDecode(float encoded);

// Linear -> Rec.709-encoded (the BT.709 OETF).
float rec709Encode(float linear);
// Rec.709-encoded -> linear (the BT.709 EOTF).
float rec709Decode(float encoded);

// The Document-level colour policy (DESIGN-imaging.md §2; CONTEXT.md's
// "Working space" glossary entry: "Linear-light RGBA, sRGB/Rec.709
// primaries by default"). The working space itself is *always* linear —
// there is no working transfer function to pick, since a transfer function
// only matters at the encode/decode boundary (import/export), never while
// operating on the data — so the one thing that actually varies here is
// which primaries the linear values are defined against. A future
// core/Document holds one of these as a field; keeping primaries as data
// on this struct rather than a hard-coded constant is what makes adopting
// ACEScg later a config change instead of a rewrite.
struct WorkingSpace {
  Primaries primaries = kRec709Primaries;
};

}  // namespace np
