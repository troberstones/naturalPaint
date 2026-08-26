#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "app/AppState.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"
#include "paint/Palette.hpp"

// app/DabPreview -- **one dab, rasterised, exactly as the deposit will make
// it.**
//
// ==========================================================================
// 1. What this replaces, and why the thing it replaces was defensible
// ==========================================================================
//
// The BRUSH LIBRARY rows draw a bar whose length tracks radius and whose
// height tracks roundness, and its own comment in `ui/MacPaintUI.cpp` is
// honest about what it is: "It is not a stroke preview ... and it is drawn as
// an obvious abstraction rather than as a fake dab, so it cannot be mistaken
// for one." That was the right call for a bar. It is not a preview, and the
// panel does not claim it is.
//
// What it cannot show is **hardness**, which is the setting a painter most
// needs to see and the one whose slider position tells them least: 0.35 and
// 0.55 are two numbers, and the difference between the marks they make is a
// picture. Roundness and angle it shows only as a proportion, and until
// brush/Deposit.hpp §2b those two did not reach a dab at all.
//
// So this module rasterises the real thing. **The rule that makes it worth
// having is that it computes nothing of its own**: coverage is
// `dabCoverage()`, the mass a covered texel receives is
// `tip.flow * coverage` through `depositTexel()`, and the colour is
// `latentToRgb()` -- the same three functions `depositDab()` calls, in the
// same order, with the same tip. A preview drawn with an ImGui circle and a
// hand-rolled gradient would agree with the brush on the day it was written
// and would drift the first time the falloff changed, and a preview that has
// drifted is worse than the bar it replaced: the bar never claimed to be the
// mark, so nobody trusted it wrongly.
//
// `--selftest`'s `dab preview` section is where that rule is enforced rather
// than merely stated. It samples the preview and a **real `depositDab()` into
// a real `PigmentTileStore`** at the same points and asserts they agree -- at
// zero tolerance for coverage, and at the f16 storage bound for mass, because
// the store rounds and this does not.
//
// ==========================================================================
// 2. Three pressures, not one, and not a scrubber
// ==========================================================================
//
// A dab has no single appearance. `evaluateLinks()` resolves the DYNAMICS
// matrix against the live inputs, and the default brush ships with
// PRESSURE -> SIZE at [0.25,1] and PRESSURE -> FLOW at [0.15,1] -- so "what
// this brush does" is a *family* of marks and a single picture of one of them
// is a picture of the wrong thing.
//
// Three candidates were considered and two rejected:
//
//   * **One dab at full pressure.** Rejected. It is the mark the pen makes
//     least often, and it hides the entire point of a PRESSURE -> SIZE link:
//     a brush with the link and a brush without it draw the identical
//     full-pressure dab. A user checking whether their link does anything
//     would learn nothing, which is the one question a preview is for.
//
//   * **One dab at the live pen pressure.** Rejected, and it is the tempting
//     one because it is the *truest* single frame. It is unreadable in
//     practice: the pressure that matters is the pressure while painting, and
//     while painting the pen is on the canvas and the panel is not being
//     looked at. Between strokes `dynamicInputsFor()` reports whatever the
//     pen last sent, so the preview would sit showing a stale number, or 1.0
//     for a mouse -- i.e. the rejected option above, wearing a disguise.
//
//   * **A scrubber.** Rejected as the *only* mechanism, for a reason that is
//     not about cost: a control that must be dragged before it tells you
//     anything teaches nothing to the user who does not drag it, and the
//     column is already dense. It remains the obvious later addition, and
//     `rasteriseDabPreview()` takes its pressures as data precisely so that
//     adding one is a caller change and not a rewrite.
//
// So: **three cells, at pressure 0.25, 0.60 and 1.00**, left to right, in one
// image. The comparison a user needs is between marks, and a spatial
// comparison is how a difference in shape is actually read -- two marks side
// by side answer "does my link do anything" in one glance and with no
// interaction. 0.25 rather than 0.0 because the low anchor should be a mark a
// hand can actually make and hold; a zero-pressure dab is the pen not
// touching the paper, and for the default link's [0.25,1] range it is also
// the same mark as 0.25 anyway.
//
// **Only pressure varies.** Tilt, azimuth and barrel are taken from the live
// inputs unchanged, so a tip whose roundness is driven by TILT previews at the
// tilt the pen is actually at. Varying two sources at once would need a grid,
// and a grid of nine cells is a data sheet rather than a glance.
//
// ==========================================================================
// 3. One scale for all three cells, and why it is not fit-to-box
// ==========================================================================
//
// Radius runs 1..200 px and the design's tip preview is 64 px, so a preview
// cannot be 1:1 everywhere. The obvious answer -- scale each dab to fill its
// box -- is wrong twice over: it makes the radius slider do nothing visible
// (every brush would preview at the same size, which is the failure the
// `--selftest` section asserts against directly), and it would give the three
// pressure cells three different scales, destroying the one comparison §2
// exists to make.
//
// So the scale is **one number for the whole image, derived from the largest
// of the three tips**: `1:1` until the largest dab would overflow its cell,
// and minified exactly enough to fit after that. Below the crossover the
// preview is life-size and the radius slider moves the mark; above it, every
// brush is drawn at a stated ratio the panel prints. That is Photoshop's own
// behaviour and it is the honest one -- a preview that silently rescales is a
// preview you cannot judge a size from.
//
// ==========================================================================
// 4. The loaded colour, over paper -- not a neutral tone
// ==========================================================================
//
// The LOADED PIGMENT block sits immediately below this preview and already
// shows the colour, so the preview does not *have* to carry it. It does
// anyway, for two reasons and with one cost stated:
//
//   * The question a preview answers is "what will this mark look like", and
//     a mark has a colour. Reading a falloff in a neutral grey and then
//     mentally re-tinting it is a translation step, and the whole value of a
//     picture is that there isn't one.
//   * `tip.flow` above 1 saturates -- `depositTexel()` caps mass at
//     `kMaxMass` -- so a heavily loaded brush lays a *flat* core where a
//     lightly loaded one lays a ramp. That is real, it is currently invisible
//     everywhere in the application, and it is only visible at all in a
//     preview that carries mass into colour rather than drawing coverage.
//
// **The dab is composited over paper, never over the panel.** A dark pigment
// on the near-black chrome is unreadable, and lightening the pigment to
// compensate would be a preview that shows a colour the brush does not have.
// Paper is what the brush actually paints on, it is `kCanvasPaper` (the
// design's own token -- docs/ui.md section 1: "the canvas is the only bright
// surface"), and it makes the preview read as a scrap of the canvas, which is
// what it is.
//
// **The cost, stated rather than discovered:** a near-white pigment on paper
// is nearly invisible. That is what painting Titanium White on white paper
// looks like, so the preview is not wrong -- but a user who picks it will see
// an almost-empty box, and the box's hairline frame (drawn by the panel, not
// by this module) is what still says where the preview is. The alternative --
// a checkerboard, or a mid-grey ground -- would make white legible by showing
// the mark against something the brush will never paint on.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No ImGui, no GPU, no `Layer`, no `Document`.** This module produces a
// block of RGBA bytes from a `BrushTip`; `ui/MacPaintUI.cpp` owns the texture
// and the widget. That is the same split `app/LayerPanel`, `app/CurveEdit`
// and `app/ControlsLayout` already make, and it is what lets the honesty
// claim in §1 be asserted headlessly.
//
// **No stroke.** This is one dab, not a test stroke. A stroke needs spacing,
// a path, and the overlap arithmetic `brush/RgbDeposit.hpp` §2 is about, and
// it would answer a different question ("what does a line look like") in a
// space one seventh the size. The design's TEST STROKE footer is still not
// built and this is not a substitute for it.
//
// **No paper texture, no wetness, no granulation.** `brush/Deposit` simulates
// none of those (its own §0 says so at length), so a preview that showed them
// would be advertising a fidelity the deposit does not have.
namespace np {

// One preview cell, in texels. The 4a design's tip preview is 64 px and this
// is that number; nothing here assumes it is a power of two or square-ish
// beyond the centring arithmetic below.
inline constexpr int kDabPreviewCell = 64;

// The pressures §2 settles on. Data rather than literals in the rasteriser so
// that a scrubber, or a fourth cell, is a caller change.
inline constexpr int kDabPreviewCells = 3;
inline constexpr std::array<float, kDabPreviewCells> kDabPreviewPressures{0.25f, 0.60f, 1.00f};

inline constexpr int kDabPreviewWidth = kDabPreviewCell * kDabPreviewCells;
inline constexpr int kDabPreviewHeight = kDabPreviewCell;

// The largest radius drawn 1:1 (§3). 30 rather than 32 so the widest life-size
// dab still has a two-texel margin inside its cell and does not touch the
// neighbouring one; a dab that reaches the cell edge reads as clipped even
// when it is exactly complete.
inline constexpr float kDabPreviewFitRadius = 30.0f;

// Document pixels per preview texel (§3). Never below 1: a small tip is drawn
// life-size, not magnified, because a magnified 3 px liner would look like a
// soft 30 px round and the radius slider would appear to do nothing.
float dabPreviewScale(float largestRadius) noexcept;

// The offset from a cell's dab centre, in document pixels, of preview texel
// `(px, py)`.
//
// The dab centre sits at the cell's exact geometric centre in texel-CORNER
// coordinates, so texel `kDabPreviewCell/2 - 1` and texel `kDabPreviewCell/2`
// are equidistant from it and a round tip is exactly symmetric -- the same
// half-texel convention `dabPixelBounds()` uses, and the reason `--selftest`
// can assert opposite samples match at zero tolerance rather than at a
// tolerance covering an off-by-half.
struct DabPreviewOffset {
  float dx = 0.0f;
  float dy = 0.0f;
};
DabPreviewOffset dabPreviewOffset(float scale, int cell, int px, int py) noexcept;

// The coverage the DEPOSIT gives that texel. This is `dabCoverage()` of
// `dabPreviewOffset()` and nothing else -- it exists as a named function so
// the rasteriser has no private copy of the mapping and `--selftest` can
// assert the identity rather than trusting it.
float dabPreviewCoverageAt(const BrushTip& tip, float scale, int cell, int px, int py) noexcept;

// What one dab leaves at that texel on **empty paper**: `depositTexel()`
// applied to a zero texel with `tip.flow * coverage` of mass, which is
// literally the body of `depositDab()`'s inner loop.
//
// Returned as a `PigmentTexel` rather than as a colour so that `--selftest`
// can compare it against a real deposited tile's texel field by field. The
// projection to a byte happens only in `rasteriseDabPreview()`.
PigmentTexel dabPreviewTexel(const BrushTip& tip, float scale, int cell, int px, int py) noexcept;

// The rasterised preview: `kDabPreviewWidth * kDabPreviewHeight` RGBA bytes,
// row-major, top to bottom, no padding.
//
// **Display-referred sRGB, opaque.** Dear ImGui's WebGPU pipeline applies a
// gamma chosen from the swapchain format, and `gfx/Context` deliberately takes
// a NON-sRGB surface so that gamma is 1.0 and the chrome's own sRGB bytes
// reach the screen untouched (ui/CanvasQuad.hpp carries that whole argument).
// So these bytes must be encoded exactly as a chrome colour is -- which is
// also why this is the one texture in the application that does NOT go through
// ui/CanvasQuad: that module exists for the *linear* document, and pushing an
// already-encoded image through it would encode it twice.
struct DabPreviewImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;

  // §3's scale, and the three radii it was derived from -- what the panel
  // prints under the preview, so the numbers on screen come from the image
  // that was actually drawn rather than from a second computation of them.
  float scale = 1.0f;
  std::array<float, kDabPreviewCells> radii{};
};

// Rasterise. Pure: same tips in, same bytes out, no ImGui and no GPU.
DabPreviewImage rasteriseDabPreview(const std::array<BrushTip, kDabPreviewCells>& tips);

// The three tips one dab would use right now, at §2's three pressures.
//
// Through `brushTipFor()` and not by reading the sliders, which is the point:
// a preview built from `BrushState::radius` directly would ignore every link
// in the DYNAMICS matrix and would show the same picture for a brush with a
// PRESSURE -> SIZE link and one without.
std::array<BrushTip, kDabPreviewCells> dabPreviewTipsFor(const BrushState& brush,
                                                         const MixboxLut& lut,
                                                         const DynamicInputs& live);

// Whether two tips produce the same preview image.
//
// **Bit equality, not a tolerance**, for `presetMatches()`'s reason: every one
// of these values arrives from a slider or from `applyPresetToBrush()`, so two
// that should be equal are bit-equal, and a tolerance would make a nudge
// smaller than the tolerance fail to redraw -- a preview showing the previous
// brush, which is the exact failure a cache is supposed to be incapable of.
//
// Compares only the fields `rasteriseDabPreview()` reads. `spacing` and
// `opacity` are deliberately absent: nothing in a one-dab preview depends on
// them, and keying on them would re-rasterise 4096 texels because a slider the
// image cannot show moved. `bitmap` (brush/Deposit.hpp §2c) IS compared, by
// pointer: `dabCoverage()` reads it, so a preview cache that ignored it would
// hand back one sampled-tip brush's picture for another's.
bool dabPreviewTipsEqual(const BrushTip& a, const BrushTip& b) noexcept;

// The preview, rasterised only when the tip actually changed.
//
// The BRUSH EDITOR is redrawn every frame the panel is open -- 120 of them a
// second on this machine -- and the image is 12 288 texels of `sqrt`, `sin`
// and a 20-term polynomial. Recomputing it for a panel nobody is interacting
// with is the whole of what this class exists to stop.
//
// `rasterisations()` and `hits()` are public for the reason
// `DocumentTexture::uploads()` is: a cache that never invalidates passes every
// test that draws once, so `--selftest` asserts the count AND that the
// returned image changed -- neither alone is worth anything.
class DabPreviewCache {
 public:
  const DabPreviewImage& imageFor(const std::array<BrushTip, kDabPreviewCells>& tips);

  uint64_t rasterisations() const noexcept { return rasterisations_; }
  uint64_t hits() const noexcept { return hits_; }

  // Monotonic, bumped on every rasterisation. What the GPU side keys its
  // upload on, so a texture is re-uploaded exactly when the pixels moved --
  // one integer comparison for a frame that changed nothing, and no memcmp of
  // 48 KiB to discover the same thing.
  uint64_t generation() const noexcept { return generation_; }

 private:
  std::array<BrushTip, kDabPreviewCells> key_{};
  bool haveKey_ = false;
  DabPreviewImage image_;
  uint64_t rasterisations_ = 0;
  uint64_t hits_ = 0;
  uint64_t generation_ = 0;
};

}  // namespace np
