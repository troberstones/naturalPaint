#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "paint/Palette.hpp"

// app/StrokePreview -- **a whole stroke, painted by the real stroke engine,
// so the BRUSH EDITOR shows what a slider actually does.**
//
// ==========================================================================
// 1. Why one dab was not enough, stated as what it could not show
// ==========================================================================
//
// `app/DabPreview` rasterises three dabs at three pressures, and its own §5
// says plainly what it is not: "**No stroke.** This is one dab, not a test
// stroke... The design's TEST STROKE footer is still not built and this is
// not a substitute for it." This module is that footer.
//
// The gap is not a matter of degree. A dab is a picture of the tip; a stroke
// is a picture of the BRUSH, and the settings a painter reaches for most
// often change only the second:
//
//   * **Spacing** -- the single most consequential number in the panel, and
//     completely invisible in a one-dab preview. 0.02 and 0.9 draw the
//     identical dab and utterly different marks.
//   * **Scatter**, and whether it throws perpendicular to travel or on both
//     axes (`BrushTip::scatterBothAxes`). A scatter of 0 and a scatter of 2
//     are the same single dab.
//   * **DIRECTION and INITIAL DIRECTION** driving Angle or Roundness. There is
//     no direction of travel in a stationary dab at all, so every one of those
//     links previews as its identity.
//   * **Velocity and Fade**, for the same reason: both are stroke-local
//     (`sourceIsStrokeLocal()`), and a preview with no stroke has no value to
//     give them.
//   * **The Dual Brush's second tip** and its blend mode, whose whole job is
//     breaking up the first tip's edge ALONG a stroke.
//   * **Overlap.** `depositTexel()` saturates at `kMaxMass`, so a tightly
//     spaced brush lays a flat ridge where a loose one lays separate marks --
//     the arithmetic `brush/RgbDeposit.hpp` §2 is about, and it needs two
//     overlapping dabs before it exists.
//   * **Paper grain** (brush/Grain.hpp), which is keyed to ABSOLUTE document
//     position: a single dab samples one patch of the field, and whether the
//     paper reads as paper is a property of a mark that travels across it.
//   * **The Minimum Diameter floor** (docs/reachability-audit.md B6), which by
//     construction only shows where the size product would otherwise collapse
//     -- i.e. in a taper's tails, which a fixed-pressure dab does not have.
//
// ==========================================================================
// 2. The rule, inherited verbatim: this module computes nothing of its own
// ==========================================================================
//
// `app/DabPreview` §1 argues that a preview earns its place only by calling
// the same functions the deposit does, because "a preview drawn with an ImGui
// circle and a hand-rolled gradient would agree with the brush on the day it
// was written and would drift the first time the falloff changed." A stroke
// has far more to drift: spacing arithmetic, the dab emission cadence, the
// stroke-local dynamics pass, per-dab scatter, the size floor, grain.
//
// So this does not re-implement any of it. It builds a real `OpenDocument`
// with a real Pigment layer, opens a real `app/StrokeSession`, feeds it real
// `addPoint()` calls, and reads the real `PigmentTileStore` back out. Every
// behaviour listed in §1 arrives because the engine produced it, not because
// this file knows about it -- which is also why this file needs no change when
// a new dynamic source lands.
//
// The cost is honest and worth naming: this is far heavier than rasterising
// three dabs. It allocates a document, runs a stroke and walks tiles. That is
// why `ui/MacPaintUI.cpp` caches it on a generation counter and re-renders
// only when the brush actually changes, exactly as it already does for the dab
// preview -- and why `--selftest` asserts that cache actually invalidates
// rather than trusting it.
//
// ==========================================================================
// 3. The same path and the same taper as `--brush-sheet`, deliberately
// ==========================================================================
//
// `app/BrushSheet.cpp` has rendered every imported `.abr` this way for weeks:
// one full sine period so the tangent turns through every direction, and a
// `sin(pi t)` pressure taper that runs 0 -> 1 -> 0 so the mark opens and
// closes. Those sheets are what this project has actually been reading when
// judging whether a brush is right.
//
// This preview uses the identical path and the identical taper, and that is
// the point rather than a convenience: a painter comparing the panel against a
// sheet must not have to account for two different test strokes. `sin(pi t)`
// rather than a triangle for the reason BrushSheet's own comment gives -- a
// triangle's corner at t=0.5 shows up as a visible kink in the width of any
// brush with a strong PRESSURE -> Size link, and a kink in a preview reads as
// a defect in the brush.
//
// ==========================================================================
// 4. One INTEGER scale, and why integer specifically
// ==========================================================================
//
// Radius runs 1..200 px (`kBrushRadiusMin`/`Max`) and the strip is
// `kStrokePreviewHeight` texels tall, so a preview cannot be 1:1 everywhere --
// `app/DabPreview` §3 has the whole argument for why a stated ratio beats
// fit-to-box, and it applies here unchanged.
//
// What is different here is that the scale is forced to a whole number. The
// document is rendered at exactly `scale` times the strip's size and box-
// filtered down, so **every output texel is the mean of exactly `scale x
// scale` input texels** -- no partial coverage at a box edge, no resampling
// kernel, no argument about what a fractional-scale filter should do at the
// rim of a dab. A `--selftest` that wants to check "this preview really is the
// stroke the engine painted" can then average the source itself and compare,
// which is not a comparison a fractional scale would admit.
//
// The scale is derived from the widest reach the brush can achieve, not from
// its nominal radius: a PRESSURE -> Size link with `rangeHi` above 1, a
// rotated bitmap tip's half-diagonal, and scatter all make the mark wider than
// `brush.radius` says, and a preview that clipped them would be lying about
// exactly the settings §1 exists to show.
//
// ==========================================================================
// 5. A Pigment layer, and what that scopes out
// ==========================================================================
//
// The scratch document carries one Pigment layer, so the stroke takes
// `StrokeRoute::CpuDeposit` -- `brush/Deposit`'s route, the one `dabCoverage()`
// and `depositTexel()` and grain all live on, and the one `app/DabPreview`
// already previews.
//
// **So a brush pointed at an RGB layer previews on the pigment route, not its
// own.** That is a real caveat and it is stated rather than hidden: the two
// routes share the tip, the spacing and the whole dynamics resolution and
// differ in what a covered texel stores, so the SHAPE of the mark this shows
// is right for both and its colour handling is the pigment one. The
// alternative -- previewing on whichever route the live layer selection
// implies -- would make the panel's picture change when the user clicked a
// different layer without touching the brush, which is a worse lie than this
// one.
//
// **No selection is applied.** A preview is a scrap of clean paper, not the
// user's canvas, so the marching ants do not gate it. `depositDab()` takes the
// selection as an explicit argument and this passes `nullptr`, which is
// `core/SelectionMask.hpp`'s own "no restriction" identity rather than a
// special case.
namespace np {

// The strip, in display texels. 288 wide fits the 322 px right column
// (`ui/AtelierLayout.hpp`'s `kRightColumnW`) with the column's own padding to
// spare; 96 tall is enough for the S-curve's amplitude plus a dab's radius on
// each side at 1:1, which is what makes an ordinary 20 px brush preview
// life-size rather than minified.
inline constexpr int kStrokePreviewWidth = 288;
inline constexpr int kStrokePreviewHeight = 96;

// The path, in DOCUMENT pixels at scale 1 (§3). Amplitude is peak-to-centre.
inline constexpr float kStrokePreviewMarginX = 22.0f;
inline constexpr float kStrokePreviewAmplitude = 18.0f;
// Path points, not dabs -- `brush/StrokePath` decides how many dabs these
// become from the tip's own spacing, which is the entire point of §1's
// spacing bullet.
inline constexpr int kStrokePreviewSamples = 320;

// The widest the mark can get, in document pixels: the largest radius any
// pressure along the taper resolves to, grown by scatter's own reach and by a
// rotated bitmap tip's half-diagonal. This is what §4's scale is derived from,
// and it is exposed so `--selftest` can assert the derivation rather than
// infer it from a rendered image.
float strokePreviewReach(const BrushState& brush, const MixboxLut& lut);

// Document pixels per preview texel (§4). A whole number, never below 1: a
// small tip is drawn life-size, not magnified.
int strokePreviewScale(float reach) noexcept;

struct StrokePreviewImage {
  int width = 0;
  int height = 0;

  // §4's integer ratio. 1 means life-size.
  int scale = 1;

  // `width * height` RGBA bytes, row-major, top to bottom, no padding.
  // **Display-referred sRGB, opaque** -- `app/DabPreview`'s
  // `DabPreviewImage` carries the whole argument for why the preview is
  // encoded like a chrome colour and drawn through ImGui's own pipeline
  // rather than through `ui/CanvasQuad`.
  std::vector<uint8_t> rgba;

  // What the engine actually did, so the panel can say something true when
  // the strip looks empty instead of leaving a blank box that reads as "this
  // brush deposits nothing".
  //
  // `dabs` is `StrokeSession::dabs()` -- the count `brush/StrokePath` emitted
  // for this spacing, which is the number a spacing slider is really moving.
  // `texels` is how many texels received mass. A stroke that emitted dabs but
  // wrote no texels is a real state (a radius that rounds to nothing, a floor
  // of zero under a collapsed size product) and is worth telling apart from a
  // stroke that never began.
  size_t dabs = 0;
  size_t texels = 0;

  // Set when `StrokeSession::begin()` REFUSED, with its own reason string.
  // Not an error to swallow: the refusal text is the most useful thing the
  // panel can show, and it is the same text the canvas would have shown.
  bool refused = false;
  std::string refusal;
};

// Paint one test stroke with `brush` and rasterise it (§2, §3).
//
// Deterministic: `app/StrokeSession` seeds its per-dab draws from the stroke's
// own start position (`strokeSeedFromStart()`), and this always starts at the
// same place, so a given `BrushState` always produces byte-identical output.
// That is what lets the cache in `ui/MacPaintUI.cpp` compare generations
// rather than images, and what lets `--selftest` assert a scatter or noise
// link changes the mark without the assertion being a coin flip.
StrokePreviewImage rasteriseStrokePreview(const BrushState& brush, const MixboxLut& lut);

// `--stroke-preview <out.png> [radius] [spacing]`: write the strip the panel
// draws, at the size the panel draws it, for the default brush with those two
// fields optionally overridden (negative means "leave the default").
//
// Headless and read-only apart from the one file it writes. It exists because
// "is this numerically the stroke the engine painted" and "does this read as a
// brush stroke at 288x96" are different questions: `--selftest` answers the
// first, and only an eye answers the second. Without this, answering the
// second means launching the GUI, finding a panel and photographing it --
// which is exactly the friction `--brush-sheet` was added to remove for the
// contact sheet.
//
// Returns a process exit code.
int runStrokePreviewDump(const char* outPath, float radiusOverride, float spacingOverride);

// The key a cached preview is valid for, and the whole reason this is a type
// rather than a `BrushState` copy.
//
// A `BrushState` carries fields this preview does not read (`tool`, `wetness`,
// the palette index behind an already-resolved pigment) and would re-render
// on, and -- worse -- a field it DOES read could be added without anything
// noticing. So the key is what the preview actually consumes:
//
//   * the tips `brushTipFor()` resolves at the pressures the taper visits,
//     compared with `brushTipEqual()` -- the COMPLETE tip comparison, guarded
//     by a `static_assert` on `sizeof(BrushTip)` so a new tip field is a
//     compile error rather than a stale picture (brush/Deposit.hpp);
//   * the link SET, compared with `linkSetsEqual()`, because the stroke-local
//     sources (VELOCITY, FADE, NOISE, RANDOM, DIRECTION, INITIAL DIRECTION)
//     are resolved inside `StrokeSession` per dab and never appear in a tip
//     at all -- a brush whose only edit was a RANDOM -> Scatter range would
//     otherwise hand back the previous stroke unchanged.
//
// Sampled at the same nine pressures `strokePreviewReach()` uses, for the same
// reason it uses nine: a curve or an inversion can put a difference between
// two brushes anywhere in [0,1], and two endpoints would miss it.
inline constexpr int kStrokePreviewKeyPressures = 9;

struct StrokePreviewKey {
  std::array<BrushTip, kStrokePreviewKeyPressures> tips{};
  BrushLinkSet links;
};

StrokePreviewKey strokePreviewKeyFor(const BrushState& brush, const MixboxLut& lut);
bool strokePreviewKeysEqual(const StrokePreviewKey& a, const StrokePreviewKey& b) noexcept;

// Cache one rasterised strip against its key. Same shape and same contract as
// `app/DabPreview`'s `DabPreviewCache` -- `generation()` is what the GPU side
// keys its upload on, so a texture is re-uploaded exactly when the pixels
// moved and an unchanged frame costs one integer comparison rather than a
// memcmp of the image.
//
// The counters are public for the reason that class's are: `--selftest` proves
// the cache invalidates instead of trusting it, and a cache that silently
// stopped invalidating is exactly the defect this preview would show as "the
// slider does nothing", which is the complaint the preview exists to answer.
class StrokePreviewCache {
 public:
  const StrokePreviewImage& imageFor(const BrushState& brush, const MixboxLut& lut);

  uint64_t rasterisations() const noexcept { return rasterisations_; }
  uint64_t hits() const noexcept { return hits_; }
  uint64_t generation() const noexcept { return generation_; }

 private:
  StrokePreviewKey key_{};
  bool haveKey_ = false;
  StrokePreviewImage image_;
  uint64_t rasterisations_ = 0;
  uint64_t hits_ = 0;
  uint64_t generation_ = 0;
};

}  // namespace np
