#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// ops/Transform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14, D15,
// D16, D17).
//
// PLAN.md's paragraph, which is the whole specification:
//
//   > Transform lands here too, because it is the same class-B machinery: a
//   > 3x3 matrix per layer, **composed before resampling** so a stack of three
//   > transforms costs one generation - Lanczos3 / Mitchell / Catmull-Rom /
//   > bilinear / nearest - exact paths for flips and 90 degree rotations -
//   > crop, canvas size, image size.
//
// and the trap it attaches to the whole phase:
//
//   > **downscale must prefilter** (area average or descend the mip pyramid);
//   > no reconstruction filter fixes aliasing after the fact.
//
// core/LayerGeometry.hpp is the file that deferred all of this by name. It
// shipped an integer translate and refused everything else -- *"An integer-
// pixel translate is exact and lossless. Anything else is a transform, and
// this build has none."* -- listing what a transform would first need: a
// filter kernel choice, a premultiplied-alpha argument, and a decision about
// interpolating latent pigment triples. This header is that debt paid for the
// first two. The third is still open and is refused by name below.
//
// ==========================================================================
// (1) THE ONE DECISION THIS FILE EXISTS FOR: compose, then resample once
// ==========================================================================
//
// PRD D16: *"Stacked transforms compose their matrices and resample **once**,
// from the original pixels."*
//
// The reason this is a requirement rather than an optimisation is that the
// wrong design **looks right**. Rotate 20 degrees, rotate 20 more, rotate 20
// more: a per-step resampler puts the picture in exactly the same place as a
// composed one. Nothing is misaligned, nothing is offset, no test that checks
// *geometry* can tell the two apart. What differs is that the per-step version
// has run the reconstruction filter over the image three times, and every pass
// convolves the signal again -- detail that a Catmull-Rom pass attenuates is
// not recovered by the next pass, it is attenuated again. The damage is
// monotonic, invisible per step, and permanent. An editor built the wrong way
// degrades a layer a little every time the user nudges a handle.
//
// So the unit of work here is a **matrix**, and pixels are touched exactly
// once per commit. `TransformStack` holds the matrices; `composed()` folds
// them; `transformImage()` is the only thing in this file that reads a source
// texel. --selftest measures the difference rather than asserting it, because
// the claim is quantitative: see runTransformTest() section 4, which round-
// trips the same geometry through 2 resamples and through 4 and reports both
// RMS errors against the original.
//
// The corollary, easy to miss: **the downscale prefilter is folded into the
// same matrix**, not applied as a separate earlier resize. See section 3.
//
// ==========================================================================
// (2) PREMULTIPLIED, LINEAR, AND WHY BOTH WORDS MATTER HERE
// ==========================================================================
//
// core/TileStore.hpp's working space is **linear-light, premultiplied
// rgba16float**. `TransformImage` below is that same space in a flat buffer,
// and every filter in this file runs on it **directly, with no un-premultiply
// step anywhere**. That is not a shortcut, it is the correct order, and it is
// the classic fringe bug when it is got wrong:
//
//   Averaging *straight* RGB weights a fully transparent texel's colour as
//   heavily as an opaque one's. The RGB sitting behind alpha == 0 is arbitrary
//   -- nothing wrote it and nothing can recover it (core/Premultiply.hpp) --
//   so it bleeds into every output texel whose kernel footprint touches it.
//   Scale a logo with a transparent black background and you get a dark halo;
//   scale one saved over transparent white and you get a bright one. Premultip-
//   lied, that texel contributes (0,0,0,0): zero colour *and* zero weight,
//   which is exactly what "there is nothing here" should mean.
//
// PRD principle 2 ("Every operation that averages pixels is defined on linear
// light") covers the other word. This file averages pixels and applies no
// transfer function of any kind; its inputs come from the tile store, which is
// linear by contract, and there is no code path that could hand it encoded
// values without also changing the element type away from float.
//
// ops/Resample.hpp's `resampleAreaAverage()` takes and returns **straight**
// alpha, because its caller (io/ExportAs) sits in the decode/encode pipeline
// where straight is the convention. This file is on the other side of that
// boundary, so the prefilter call in Transform.cpp brackets it with an
// un-premultiply and a re-premultiply. That round trip is documented at the
// call site including what it costs; the alternative -- a second area-average
// written in premultiplied space -- was rejected outright, because two
// implementations of the same integral is exactly how the two of them drift.
//
// **Negative lobes and the alpha guard.** Catmull-Rom, Mitchell and Lanczos3
// all have negative lobes: a high-contrast edge can produce an output alpha
// slightly below zero. A negative alpha is not a representable coverage, and
// core/Premultiply's `unpremultiply()` already defines `a <= 0 -> {0,0,0,0}`
// at every read boundary in the codebase. Rather than let a negative alpha sit
// in a tile and be silently zeroed later by a *different* file, this one
// applies the same rule at the point of creation: an output whose alpha comes
// out below zero is stored as transparent black. RGB is deliberately **not**
// clamped -- neither up to alpha nor down to zero. Rejected: clamping RGB to
// [0,1], which would crush the scene-referred highlights this build can
// genuinely hold (io/Export.hpp deliberately does not clamp at float depths);
// and clamping RGB to alpha to restore the `rgb <= a` inequality, which is a
// display-referred invariant this working space does not claim.
//
// ==========================================================================
// (3) DOWNSCALE MUST PREFILTER, AND WHY NOT THE MIP PYRAMID
// ==========================================================================
//
// A reconstruction kernel -- Catmull-Rom, Lanczos3, any of them -- answers the
// question "what was the signal *between* these samples". It is the right tool
// for magnification and for a rotation at roughly 1:1. It is the wrong tool
// for minification, and using it there is not slightly worse, it is a
// different signal: every source frequency above the destination's Nyquist
// limit folds down into the output as a lower frequency that was never in the
// picture. ops/Resample.hpp's header makes the argument at length and this
// file does not repeat it.
//
// So `transformImage()` measures how much the composed matrix shrinks each
// source axis, and when either shrinks it **area-averages the source down to
// destination density first**, then reconstructs from the prefiltered source.
// The prefilter's own scale is composed into the matrix before the
// reconstruction pass runs, so this is still one generation of resampling and
// not two: the decimation and the reconstruction are the two halves of a
// single resample, in the order that makes the result band-limited.
//
// **Why an exact area average and not the existing mip pyramid.** PLAN.md
// offers both ("area average or descend the mip pyramid"), and this build does
// have a pyramid -- `buildMipChain()`. Three reasons it is the wrong one here,
// in order of weight:
//
//   1. It lives in `ui/NaturalPaintUI.hpp`. ops/ depending on ui/ inverts the
//      dependency direction the whole tree is arranged around; a resampler
//      that cannot run without the UI layer cannot run in a headless export or
//      a --selftest section.
//   2. It is a **per-tile** 128 -> 64 -> ... -> 1 chain built for display
//      zoom, keyed by `mipLevelForZoom()`. Its levels are per 128px tile, not
//      per image, so an image-space transform would have to reassemble one
//      level of it across tiles and would inherit the tile-boundary seam that
//      a per-tile box filter has and a whole-image one does not.
//   3. Its steps are powers of two. A 3.1x reduction must then pick between
//      level 1 (2x, still aliased by a factor of 1.55) and level 2 (4x, then
//      magnified back up, visibly soft). `resampleAreaAverage()` handles the
//      arbitrary factor exactly, with fractional end weights, and has no such
//      choice to get wrong.
//
// **The honest limit of the prefilter, stated rather than buried.** The
// minification factor is taken per source axis from the column norms of the
// matrix's linear part, which is exact for an axis-aligned scale and exact for
// a rotation (both column norms are 1, so no prefilter, correctly). It is an
// *approximation* for an anisotropic scale combined with a rotation, where the
// true minification is direction-dependent and the correct answer is an
// elliptically weighted average (EWA) with a per-destination-texel kernel.
// This file does not implement EWA: a 0.2x-in-x, 1.0x-in-y scale at 45 degrees
// is over-filtered along one diagonal and under-filtered along the other. That
// is a known, bounded, named limitation, not a silent one. For a perspective
// transform the linear part varies across the image and the factor is taken
// from the Jacobian at the destination centre, so a strong perspective is
// correctly filtered near the middle and progressively less so at the far
// edge.
//
// ==========================================================================
// (4) EXACT PATHS (PRD D15) ARE A CORRECTNESS REQUIREMENT, NOT A FAST PATH
// ==========================================================================
//
// PRD D15: *"Flips and 90 degree rotations are **exact** -- no resample."*
//
// A flip is a relabelling of the pixel grid. Every destination texel has
// exactly one source texel sitting exactly under it. Running that through a
// filter kernel -- even a kernel that is theoretically an identity at integer
// offsets -- is a silent quality bug: the weights are computed in float, they
// sum to 1 only to within rounding, and the result is an image that is
// *almost* the input. Flip, flip back, and you have lost something for
// nothing. So `exactRemapKind()` detects the eight elements of the square's
// symmetry group plus an integer translation, and `transformImage()` takes a
// path that performs **no arithmetic on texel values at all** -- it copies the
// four floats. --selftest asserts bit equality with `memcmp`, not a tolerance,
// because a tolerance here would pass on exactly the implementation D15 forbids.
//
// The detection is by **exact** float comparison against 0 and +-1. That is
// deliberate and it puts a requirement on the builders: `transformRotate90()`
// and the flips construct those entries as literals, and products and sums of
// exact 0/+-1 stay exact in IEEE arithmetic, so composing flip . rot90 . flip
// is still on the exact path. `transformRotateDegrees()` therefore **snaps**
// exact multiples of 90 to the exact builder rather than calling `cosf` --
// `cosf(pi/2)` is -4.37e-8, not zero, and a user who types 90 into a rotation
// box must not get a resample because of it. A user who types 90.0001 does get
// one, which is correct.
//
// Scatter, not gather. The exact path iterates *source* texels and writes each
// to its mapped destination, because a signed permutation with integer
// translation is a bijection on the integer grid: every destination inside the
// mapped extent is written exactly once, with no inverse matrix to round.
//
// ==========================================================================
// (5) WHAT IS NOT HERE
// ==========================================================================
//
//   - **Pigment layers.** core/LayerGeometry.hpp already framed the question:
//     DESIGN-imaging.md section 3 says "any op that is a linear combination of
//     pixels stays valid in latent space", so a positive-weight resample of a
//     7-channel `PigmentTile` would be legitimate and a Lanczos3 one, with its
//     negative lobes, would not. This file transforms `Tile` (rgba16float)
//     only. Deciding which kernels a latent triple may pass through is a real
//     decision and it is not made by defaulting.
//   - **Interactive handles, and the undo entry.** PRD D14 asks for handles;
//     this is the op behind them, not the tool. Nothing here touches ui/.
//   - **Straighten, perspective correction from a marked rectangle, lattice
//     warp.** Those are PLAN.md's "solved-for-you variants" and they *solve
//     for* a matrix this file then applies -- `transformFromQuad()` is the
//     piece they will share, and it is here.
//   - **Boundary coverage antialiasing.** See the edge-policy note on
//     `transformImage()`: the source rectangle's own edge comes out hard, not
//     coverage-weighted. The fix is a per-destination-texel coverage of the
//     transformed source polygon, which is the same quantity
//     core/SelectionMask already stores for a marquee -- so the machinery
//     exists in shape, and this is a real gap rather than an impossible one.
//   - **ROI propagation and the tile cache.** Same phase, different track.
//     `transformImage()` is whole-image; a `roi(rect) -> rect` for it is the
//     inverse matrix applied to the rect's corners and is trivial to add on
//     top, but it is not this file's step.
namespace np {

// --------------------------------------------------------------------------
// A point in image space. Pixel *centres* sit at half-integers: texel (i, j)
// covers [i, i+1) x [j, j+1) and its centre is (i + 0.5, j + 0.5). Getting
// this convention wrong is the source of the classic half-pixel shift, which
// is invisible on a translate and obvious as a creeping drift on a rotate
// stack -- which is exactly the stack PRD D16 is about.
// --------------------------------------------------------------------------
struct Point2 {
  float x = 0.0f;
  float y = 0.0f;
};

// A 3x3 homogeneous matrix, row-major: entry (row, col) is `m[row * 3 + col]`.
//
// It maps **column vectors on the left** -- `p' = M * (x, y, 1)` -- so
// `mat3Multiply(a, b)` is `a * b`, which applies **b first and then a**. That
// is the mathematical convention and the one every graphics text uses; the
// alternative (row vectors on the right, so composition reads left to right)
// is defensible but would make this file's matrices transposes of the ones in
// any reference a reader checks it against.
//
// The stored direction is **destination-from-source**: it maps a source pixel
// coordinate to where that pixel lands. That is the direction a user's handles
// manipulate ("drag this corner *there*"). Sampling needs the other direction,
// so `transformImage()` inverts it internally, once, rather than making every
// caller keep an inverse in step with the forward matrix.
struct Mat3 {
  std::array<float, 9> m{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

Mat3 mat3Identity() noexcept;

// `a * b` -- applies `b` first, then `a`. See Mat3's comment.
Mat3 mat3Multiply(const Mat3& a, const Mat3& b) noexcept;

// Inverts via the adjugate. Returns false, leaving `*out` untouched, when the
// determinant is not finite or is too small to invert meaningfully -- a
// degenerate transform (a quad collapsed to a line, a zero scale) has no
// inverse and the resampler must refuse rather than divide by ~0 and fill the
// destination with infinities.
bool mat3Invert(const Mat3& in, Mat3* out) noexcept;

// Maps a point, dividing through by the homogeneous w. A w of zero (a point on
// the horizon of a perspective transform) yields a point at infinity; the
// coordinates come back non-finite and callers that care check.
Point2 mat3MapPoint(const Mat3& t, Point2 p) noexcept;

// --- Builders -------------------------------------------------------------
//
// Everything below composes with everything else via mat3Multiply(). The
// "About" variants are `translate(pivot) * op * translate(-pivot)` spelled
// out, which is the operation a rotate handle actually performs and which
// callers otherwise re-derive (and get backwards) at every call site.

Mat3 transformTranslate(float tx, float ty) noexcept;
Mat3 transformScale(float sx, float sy) noexcept;
Mat3 transformScaleAbout(float sx, float sy, Point2 pivot) noexcept;

// Counter-clockwise in a y-down image space, i.e. visually clockwise on
// screen. **Exact multiples of 90 degrees are snapped** to the exact quarter-
// turn builder -- see section 4 of this header; that snap is what makes a
// rotate box that reads "90" land on PRD D15's no-resample path.
Mat3 transformRotateDegrees(float degrees) noexcept;
Mat3 transformRotateDegreesAbout(float degrees, Point2 pivot) noexcept;

// Shear. `xDegrees` slants vertical lines (x gains a multiple of y),
// `yDegrees` slants horizontal ones. Angles at or past +-90 degrees are
// clamped short of the tangent's pole, because a 90 degree skew is a
// degenerate matrix and refusing it later, in the resampler, would report the
// failure a long way from the slider that caused it.
Mat3 transformSkewDegrees(float xDegrees, float yDegrees) noexcept;

// Mirror about the vertical / horizontal centre line of a `width` x `height`
// canvas. Entries are exact 0 / +-1 plus an integer translation, so the result
// is on the exact path (PRD D15) and stays there under composition.
Mat3 transformFlipHorizontal(uint32_t width) noexcept;
Mat3 transformFlipVertical(uint32_t height) noexcept;

// `quarterTurns` counted counter-clockwise in image space and taken modulo 4
// (negatives welcome). `width`/`height` are the *source* canvas; for an odd
// number of turns the destination canvas is height x width, and the matrix
// maps the source rectangle exactly onto it. Exact entries, exact path.
Mat3 transformRotate90(int quarterTurns, uint32_t width, uint32_t height) noexcept;

// Solves the homography taking the four `src` points to the four `dst` points
// -- PRD D14's four-corner perspective, and the shared engine behind PLAN.md's
// "perspective correction (mark what should be a rectangle and solve the
// homography)".
//
// Eight unknowns from eight equations, by Gaussian elimination with partial
// pivoting in double (the system is badly conditioned for near-degenerate
// quads and a float solve loses corners visibly). Returns false and sets
// `*errorOut` when the system is singular -- three collinear source points, a
// quad collapsed to a line, or a destination quad that folds over itself.
bool transformFromQuad(const std::array<Point2, 4>& src, const std::array<Point2, 4>& dst,
                       Mat3* out, std::string* errorOut);

// --------------------------------------------------------------------------
// The stack. PRD D16's requirement made into a type, so that "compose then
// resample once" is what the API makes easy rather than what a comment asks
// for. `push()` costs nothing; only `transformImage()` reads a texel.
//
// Deliberately not a `std::vector<Mat3>` typedef: the fold order (last pushed
// applies last, so `composed()` multiplies right to left) is the thing a
// caller gets backwards, and it belongs in one place.
// --------------------------------------------------------------------------
class TransformStack {
 public:
  void push(const Mat3& t) { entries_.push_back(t); }
  void clear() noexcept { entries_.clear(); }
  size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }
  const Mat3& at(size_t i) const { return entries_[i]; }

  // The single matrix equivalent to applying every pushed transform in order.
  // An empty stack composes to the identity, which is the right answer and not
  // an error: a transform tool with no edits yet must be a no-op.
  Mat3 composed() const noexcept;

 private:
  std::vector<Mat3> entries_;
};

// --------------------------------------------------------------------------
// Reconstruction kernels. PLAN.md names exactly these five.
//
// All are evaluated **separably** -- a horizontal weight times a vertical one
// -- which is exact for the box and tent kernels and is the standard, and
// standardly accepted, approximation for the cubics and for Lanczos.
//
// Catmull-Rom and Mitchell are the *same* cubic, the Mitchell-Netravali
// family, at two points in its (B, C) parameter space: Catmull-Rom is
// (0, 0.5) and Mitchell is (1/3, 1/3). One function serves both, because
// implementing them separately means two chances for a coefficient typo and
// no benefit.
//
// What each one costs, so a caller can choose rather than guess:
//
//   Nearest     radius 0.5. No interpolation at all. Preserves hard edges and
//               exact palette values; the only correct choice for pixel art
//               and for indexed-like content. Aliases on any non-integer
//               offset.
//   Bilinear    radius 1. Non-negative weights, so no ringing and no
//               out-of-gamut overshoot, ever. Softer than the cubics and
//               noticeably so on repeated use -- which after PRD D16 is a
//               thing that no longer happens by accident.
//   CatmullRom  radius 2, interpolating (passes exactly through the source
//               samples). The sharpest of the three cubics and this file's
//               default. Negative lobes: overshoots at high-contrast edges.
//   Mitchell    radius 2, approximating (does not pass through the samples).
//               Mitchell and Netravali's own compromise point between blur and
//               ringing. Softer than Catmull-Rom, much less overshoot.
//   Lanczos3    radius 3. The flattest passband of the five and the sharpest
//               result; also the largest negative lobes and the most visible
//               ringing on synthetic edges. Three times the kernel width of
//               bilinear per axis, so ~9x the taps.
//
// The default is Catmull-Rom rather than Lanczos3 because this working space
// holds scene-referred linear values: a negative lobe next to a specular
// highlight is proportionally larger there than it is on display-referred
// data, and Lanczos3's lobes are the deepest on offer. Rejected: defaulting to
// bilinear for safety, which would make the default *quietly* the softest
// option and put the burden on users to discover that the good one is a menu
// away.
// --------------------------------------------------------------------------
enum class ResampleKernel { Nearest, Bilinear, CatmullRom, Mitchell, Lanczos3 };

// Half-width of the kernel's support, in source texels.
float resampleKernelRadius(ResampleKernel kernel) noexcept;

// The kernel evaluated at signed offset `t`. Nearest, bilinear and both cubics
// are partitions of unity -- their weights at integer offsets from any sample
// position sum to exactly 1 analytically. **Lanczos3 is not**; its weights sum
// to 1 only to about 1e-3, which unnormalised would be a periodic brightness
// ripple over the whole image. `transformImage()` therefore normalises the
// gathered weights, and by the sum over the *whole* footprint including taps
// that fall off the image -- see the long note at that loop, because the
// choice between the full sum and the clipped sum is the difference between a
// border that fades and a border that smears.
float resampleKernelWeight(ResampleKernel kernel, float t) noexcept;

// Human-readable name, for refusal messages and for a menu.
const char* resampleKernelName(ResampleKernel kernel) noexcept;

// --------------------------------------------------------------------------
// A flat image in the tile store's own space: **linear light, premultiplied
// alpha, RGBA float**, row-major, top to bottom, four floats per texel, no row
// padding.
//
// Float and not half. The tile store is rgba16float and this type is what sits
// between two of them; carrying half through the intermediate would quantise
// twice for nothing, and the prefilter/reconstruct pair is exactly where the
// extra bits are worth having. The narrowing back to half happens once, in
// `tileStoreFromImage()`.
//
// Note the difference from io/ImageDecode.hpp's `DecodedImage`, which is the
// same layout with **straight** alpha because it is on the file side of the
// premultiply boundary. They are not interchangeable and neither converts to
// the other implicitly.
// --------------------------------------------------------------------------
struct TransformImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<float> px;  // width * height * 4

  size_t sampleCount() const noexcept {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  }
  bool valid() const noexcept { return width > 0 && height > 0 && px.size() == sampleCount(); }
};

// The eight symmetries of the square, plus the identity spelled out
// separately. `exactRemapKind()` returns one of these when the composed matrix
// is a signed permutation with an integer translation -- i.e. when PRD D15's
// no-resample path applies -- and `None` otherwise.
//
// Transpose and AntiTranspose are here because they are what a flip composed
// with a quarter turn *is*. Leaving them out would mean flip-then-rotate90
// silently fell off the exact path even though both halves were exact, which
// is precisely the composition PRD D16 encourages users to build.
enum class ExactRemap {
  None,
  Identity,
  FlipHorizontal,
  FlipVertical,
  Rotate90,
  Rotate180,
  Rotate270,
  Transpose,      // reflect about the main diagonal: (x, y) -> (y, x)
  AntiTranspose,  // reflect about the anti-diagonal
};

const char* exactRemapName(ExactRemap kind) noexcept;

// Classifies `dstFromSrc` for PRD D15. Returns `None` unless the linear part
// is **exactly** one of the eight signed permutation matrices (entries
// compared with `==` against 0 and +-1, see section 4) and the translation
// maps pixel centres to pixel centres -- which for a signed permutation means
// the translation components are integers.
//
// The extents are not used for classification, only reported back through
// `transformImage()`; a mismatched destination extent still takes the exact
// path and simply clips.
ExactRemap exactRemapKind(const Mat3& dstFromSrc) noexcept;

// What a transform actually did, so a caller -- and --selftest -- can assert
// the *process* and not only the pixels. PRD D16's claim is about how many
// times the data was resampled, and a claim about a process needs a witness
// that a golden image cannot provide.
struct TransformReport {
  ExactRemap exact = ExactRemap::None;

  // How many times a reconstruction kernel was convolved over the data.
  // **Zero on the exact path** and **one on every other path** -- including
  // one that prefiltered, because the decimation and the reconstruction are
  // the two halves of a single generation (section 3). If this is ever
  // greater than 1 for a single call, something inside has grown a second
  // pass and PRD D16's guarantee is gone.
  int reconstructionPasses = 0;

  bool prefiltered = false;
  uint32_t prefilterWidth = 0;   // source width after the area average
  uint32_t prefilterHeight = 0;  // source height after the area average

  // The per-source-axis minification the matrix asked for, as measured (see
  // section 3's note on how, and on where the measurement is approximate).
  // 1.0 means "no shrink on this axis"; 0.25 means "shrink to a quarter".
  float axisScaleX = 1.0f;
  float axisScaleY = 1.0f;
};

// Knobs. Defaults are the correct-by-default ones: prefiltering on, exact
// paths on. The two `false` settings exist so --selftest can measure what
// turning them off costs -- a claim that prefiltering matters is worth more as
// a measured ratio than as an assertion, and the only way to measure it is to
// be able to run the wrong path deliberately. They are not a user-facing
// setting and nothing in the app should set them.
struct TransformParams {
  ResampleKernel kernel = ResampleKernel::CatmullRom;
  bool prefilterDownscale = true;
  bool allowExactPaths = true;
};

// The one function in this file that reads a source texel.
//
// `dstFromSrc` maps source pixel coordinates to destination pixel coordinates
// (see Mat3). `dstWidth` x `dstHeight` is the destination extent -- for a
// layer transform that is the document canvas, so the layer's pixels move
// *within* the canvas; for a "fit to content" transform, pass the extent
// `transformedBounds()` reports.
//
// **Edge policy**, in two parts, because they answer different questions.
//
// A destination texel whose *source position* falls outside the source image
// is left **transparent black**: there is nothing there, and premultiplied
// zero is what "nothing here" means. That is what keeps a rotated layer's far
// corners empty instead of filled with smeared border colour.
//
// Inside that boundary, a kernel footprint that overhangs the edge **clamps**
// its taps to the edge texel. The alternative -- letting the off-image taps
// contribute (0,0,0,0) at full weight, so the border fades over the kernel's
// support -- is what this file did first, and the measurement harness caught
// it: upscaling a *fully opaque* field 8 -> 21 with Lanczos3 left the border
// at alpha 0.944 instead of 1.0. An "Image Size" that makes an opaque picture
// translucent around the edge is a defect, and it is worst for the sharpest
// kernel, which is the one a user picks when they care most.
//
// Under premultiplied alpha the two policies **differ only where the source's
// own border texels are not transparent**, because clamping replicates
// (0,0,0,0) wherever the content already ended -- which is the fade. So
// clamping costs nothing on a layer whose content stops before the buffer
// edge, and is right for one whose content runs to it.
//
// What neither policy does is antialias the boundary itself. A full-bleed
// layer rotated 30 degrees gets a hard, aliased rectangle edge; a smooth one
// needs the coverage of each destination texel by the transformed source
// *polygon*, which is real machinery (core/SelectionMask's coverage store is
// the shape of it) and is listed in section 5 as absent rather than
// approximated by a filter artefact that happens to look soft.
//
// **Not in-place.** `out` must not be `&src`: every path here clears the
// destination before it reads the source, so an in-place call is refused by
// name rather than silently returning a blank image. Supporting it would mean
// a hidden temporary, and PRD A5's objection is to allocations the caller
// cannot see.
//
// Returns false and sets `*errorOut` when:
//   - `out` is null, is `&src`, or `src` is not `valid()`;
//   - the destination extent has a zero dimension, or is large enough that
//     `dstWidth * dstHeight * 4` floats would overflow the addressable buffer;
//   - `dstFromSrc` is not invertible (see `mat3Invert`) -- a collapsed or
//     zero-scale transform, refused by name rather than filled with infinities.
// `*out` is cleared on failure, so a refused transform cannot leave a
// half-resampled image behind (ops/Resample.cpp's discipline, same reasons).
//
// `report` may be null.
bool transformImage(const TransformImage& src, const Mat3& dstFromSrc, uint32_t dstWidth,
                    uint32_t dstHeight, const TransformParams& params, TransformImage* out,
                    TransformReport* report, std::string* errorOut);

// The axis-aligned bounding box of a `width` x `height` image's four corners
// after `dstFromSrc`, in destination coordinates. The corners are the image's
// *outer* corners (0,0) and (width,height), not pixel centres, so the box
// covers the transformed footprint rather than being half a pixel small on
// every side.
//
// Non-finite for a transform that puts a corner on the horizon; callers doing
// "fit to content" should check before turning it into an extent.
struct TransformBounds {
  float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
};
TransformBounds transformedBounds(const Mat3& dstFromSrc, uint32_t width,
                                  uint32_t height) noexcept;

// --------------------------------------------------------------------------
// PRD D17: crop, canvas size, image size.
//
// The first two **do not resample and cannot**: both are pure index copies at
// integer offsets, and both are therefore bit-exact. Routing them through the
// matrix path would have been tidy and would have made a crop lossy for no
// reason at all, which is the same mistake as filtering a flip.
// --------------------------------------------------------------------------

// Extracts the rectangle `[x, x + width) x [y, y + height)`. The rectangle may
// hang off any edge, including entirely: the parts outside the source come
// back transparent black, so "crop" and "extend the canvas" are one operation
// with a signed origin, and a crop tool that lets the user drag a handle
// outside the image does not need a second code path.
//
// Refuses only a zero extent or an overflowing one.
bool cropImage(const TransformImage& src, int32_t x, int32_t y, uint32_t width, uint32_t height,
               TransformImage* out, std::string* errorOut);

// Where the existing pixels sit inside a new canvas. The nine-cell grid every
// canvas-size dialog offers.
enum class CanvasAnchor {
  TopLeft, TopCenter, TopRight,
  CenterLeft, Center, CenterRight,
  BottomLeft, BottomCenter, BottomRight,
};

// Canvas size: change the extent, do not touch the pixels. Implemented as a
// `cropImage()` with the origin the anchor implies, so there is exactly one
// index-copy loop in this file.
//
// The offset for a centred anchor is **floored**, not rounded: growing a
// canvas by an odd number of pixels has to put the extra pixel on one side,
// and flooring puts it on the right/bottom consistently. Rounding would put it
// on different sides for different parities, which shows up as a one-pixel
// jitter when a user nudges the size field.
bool resizeCanvas(const TransformImage& src, uint32_t width, uint32_t height, CanvasAnchor anchor,
                  TransformImage* out, std::string* errorOut);

// Image size: resample the pixels to a new extent. This one *is* the matrix
// path -- `transformScale(w/srcW, h/srcH)` through `transformImage()` -- so a
// downscale prefilters (PRD D17's own clause) by the same machinery as a
// rotate, and there is no second resizer to keep in step.
//
// A 1:1 request short-circuits to a verbatim copy: a "resize" that changes
// nothing must not perturb a value, and the reconstruction kernel at exactly
// integer offsets is only an identity to within weight rounding. A pure
// downscale likewise ends up bit-identical to `resampleAreaAverage()`'s
// output, because the prefilter lands exactly on the destination extent and
// the leftover reconstruction is detected as identity and skipped -- see
// Transform.cpp.
bool resizeImage(const TransformImage& src, uint32_t width, uint32_t height,
                 const TransformParams& params, TransformImage* out, TransformReport* report,
                 std::string* errorOut);

// --------------------------------------------------------------------------
// The tile-store bridge. This is what makes "a 3x3 matrix per layer" (PLAN.md)
// something other than a claim about a flat buffer.
//
// Rectangular and flat, not sparse. A transform is not a local edit: a rotate
// moves content into tiles that did not exist and empties tiles that did, so
// the sparse structure of the *input* tells you almost nothing about the
// output's. Materialising the region once and writing back once is both
// simpler and, for the region sizes a transform touches, not obviously slower
// than a tile-wise gather that has to look up a scattered footprint per texel.
// The cost is stated rather than hidden: a full 4K canvas round trip is
// 4096*2160*4 floats = 141 MB in flight, transiently, and PRD A5's standing
// objection to invisible allocations applies -- a caller transforming a whole
// document should pass the content bounds from
// `core/LayerGeometry.hpp::layerContentBounds()`, not the canvas.
//
// Half-float round trip: `imageFromTileStore` widens, `tileStoreFromImage`
// narrows. That narrowing is the only quantisation the transform adds beyond
// the resample itself, and it is the same one every write into the store pays.
// --------------------------------------------------------------------------

// Materialises `[originX, originX + width) x [originY, originY + height)` of
// `store` in document coordinates. Absent tiles read as transparent black,
// which is the store's own implicit content for a tile nothing has written.
TransformImage imageFromTileStore(const TileStore& store, int32_t originX, int32_t originY,
                                  uint32_t width, uint32_t height);

// Writes `img` back at `originX, originY`. Texels with alpha exactly zero are
// written like any other -- the whole point is that a transform *clears* what
// it moved away from, and skipping the clear would leave the layer's old
// pixels behind as a ghost.
//
// The one exception is a tile that is both entirely transparent in `img` **and**
// does not already exist in `store`: that one is not allocated at all. A
// transform's destination region is a bounding box, and a rotate leaves most
// of that box's corners empty; allocating 128 KiB for each of them would make
// a 45-degree rotate cost half again what the content does.
void tileStoreFromImage(const TransformImage& img, int32_t originX, int32_t originY,
                        TileStore* store);

}  // namespace np
