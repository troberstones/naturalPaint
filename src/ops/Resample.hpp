#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ops/Resample (PLAN.md "Phase 4 -- Write it out", step 7: "Export As --
// format, space, depth *and resize*, with saveable presets (PRD I15).
// **Downscale prefilters; see the phase 6 warning**").
//
// That warning, quoted in full because it is the entire reason this file
// exists rather than a one-line `sample the nearest source texel` loop
// inside the export path:
//
//   > ⚠️ Two domain traps. **Add noise runs in the shaper domain** [...]. And
//   > **downscale must prefilter** (area average or descend the mip
//   > pyramid); no reconstruction filter fixes aliasing after the fact. Both
//   > are the most commonly botched operations in this category.
//
// "No reconstruction filter fixes aliasing after the fact" is the load-
// bearing half. Point-sampling a source image at a coarser grid is not a
// slightly-worse resize -- it is a *different signal*: every spatial
// frequency above the destination's Nyquist limit folds back into the
// output as a lower frequency that was never in the picture. Once folded, no
// amount of smoothing on the destination recovers it, because the aliased
// energy is now indistinguishable from real content. The prefilter has to
// happen on the *source* side, before decimation, which is what this file
// does and what a naive resize does not. --selftest measures the difference
// rather than asserting it (runExportAsTest(): a 1-px checkerboard reduced
// 8x aliases to full amplitude through the naive path and to numerically
// nothing through this one).
//
// --- Which filter, and why -----------------------------------------------
//
// **Exact area average** -- each destination texel is the integral of the
// source over the destination texel's own footprint, with fractional weights
// at the ends of the footprint so non-integer scale factors are handled
// exactly rather than by rounding the footprint to whole source texels.
// Implemented separably (a horizontal pass then a vertical one), which for a
// box kernel is exactly equivalent to the 2-D integral and costs
// O(w*h) instead of O(w*h*kernel_area).
//
// Chosen over Lanczos3/Mitchell -- both of which PLAN.md phase 6 lists for
// the general transform op -- for three reasons, in order of weight:
//
//  1. It is the phase 6 warning's own prescription ("area average or descend
//     the mip pyramid"), and the mip alternative needs an integer power-of-
//     two chain that an arbitrary export size does not have.
//  2. **Its weights are all non-negative**, so no output value can leave the
//     convex hull of its source footprint. Windowed-sinc and Mitchell kernels
//     have negative lobes, which overshoot at high-contrast edges -- ringing.
//     On display-referred 8-bit output ringing merely looks bad; on the
//     scene-referred float output this project can now write (EXR half /
//     32-bit float, PLAN.md step 2), a negative lobe next to a specular
//     highlight produces genuinely *negative* linear radiance, which is not a
//     representable colour and which the [0,1] clamp deliberately does not
//     apply to at float depths (io/Export.hpp). A filter that can manufacture
//     negative light in an export is the wrong default for an export.
//  3. It has no free parameters, so there is no B/C/lobe-count decision to
//     get wrong here and then have to keep compatible with saved presets
//     forever.
//
// The honest cost of that choice, stated rather than buried: a box kernel's
// frequency response is a sinc, whose first sidelobe is only -13 dB down, so
// it does not remove *all* above-Nyquist energy -- a downscale of a pattern
// whose period does not divide the scale factor keeps a small residual
// ripple. --selftest measures that residual too, on a deliberately awkward
// period-3 pattern, so the number is visible rather than implied. A sharper
// passband is what Lanczos buys, and phase 6's transform op -- which owns
// filter *choice* as a user-facing decision -- is where that belongs.
//
// --- Linear light --------------------------------------------------------
//
// PRD principle 2 ("Every operation that averages pixels is defined on linear
// light") and PRD B2. This function averages pixels, so its input must be
// linear-light values; it has no idea what a transfer function is and applies
// none. That is not left to a caller's good intentions: in this codebase the
// only caller is io/ExportAs, which sits between flattenDocumentToLinear()
// (which produces linear light by contract) and encodeLinearImage() (which
// applies the target transfer function), so the resize is in linear light by
// pipeline construction and cannot accidentally be moved after the encode
// without also moving it into a function that takes bytes. --selftest proves
// the resulting *number* rather than the ordering: a black/white checker
// halved lands on sRGB code 188 (the encoding of linear 0.5), not 128 (the
// average of the two encoded codes, which decodes to linear 0.214).
//
// --- Alpha ---------------------------------------------------------------
//
// Input and output are **straight** (non-premultiplied) alpha -- the same
// convention io/ImageDecode.hpp's DecodedImage carries, so nothing has to
// convert at the boundary. Internally the filter runs on *premultiplied*
// values and un-premultiplies once at the end, which is the only correct
// order: averaging straight RGB weights a fully transparent texel's colour
// as heavily as an opaque one's, so the arbitrary RGB sitting behind alpha=0
// bleeds into the result. --selftest measures that too (a transparent green
// texel next to an opaque red one contributes exactly 0.0 green here, and
// 0.5 green through a straight-alpha average).
//
// --- Downscale only ------------------------------------------------------
//
// A request for a destination larger than the source in either axis is
// **refused by name**, not silently satisfied. Upscaling is a different
// problem -- there is no data to average, so the answer is entirely
// determined by the choice of *reconstruction* filter (bilinear, bicubic,
// Catmull-Rom, Lanczos3), which is a user-facing decision PLAN.md phase 6
// owns and lists by name. An export dialog that quietly upscaled with a
// filter nobody chose would be inventing detail and labelling it as the
// document's. See io/ExportAs.hpp for how the resize modes are defined so
// that "fit within a box" never has to upscale.
//
// A 1:1 request is not a resize at all and is short-circuited to a verbatim
// copy, so the premultiply/un-premultiply round trip cannot perturb a single
// value by an ulp when nothing was asked for.
namespace np {

// Area-average downscale of a straight-alpha, linear-light RGBA float image.
//
// `src` holds `srcWidth * srcHeight * 4` floats, row-major top-to-bottom, no
// row padding -- byte-identical in layout to io/ImageDecode.hpp's
// DecodedImage::pixels, which is what this is always called on. `out` is
// resized to `dstWidth * dstHeight * 4` on success and cleared on failure, so
// a refused request can never leave a partially resampled buffer behind (the
// same discipline io/Export.cpp applies to files).
//
// Returns false, and sets `*errorOut` (when non-null) to a message that names
// the specific thing refused, when:
//   - `out` is null, or `src` is null;
//   - the source or destination has a zero dimension;
//   - `dstWidth > srcWidth` or `dstHeight > srcHeight` -- upscale, see this
//     header's "downscale only" section. The error names both sizes and the
//     axis that grew.
//
// Values are not clamped: a linear input above 1.0 (a scene-referred
// highlight) averages normally and stays above 1.0, because whether to clamp
// is an *export-policy* decision keyed to the destination bit depth, made in
// io/Export where it already lives.
bool resampleAreaAverage(const float* src, uint32_t srcWidth, uint32_t srcHeight,
                         uint32_t dstWidth, uint32_t dstHeight, std::vector<float>* out,
                         std::string* errorOut);

}  // namespace np
