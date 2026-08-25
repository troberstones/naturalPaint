#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ops/PointOps.hpp"  // Curve, CurvePoint, evalCurve

namespace np {

// The brush dynamics link model -- the spine of the BRUSH SETTINGS panel's
// DYNAMICS matrix (design "naturalPaint Panels" turn 4, option 4a).
//
// A *link* says "this input drives that parameter, through this curve". The
// panel draws the whole 8 x 12 space as a matrix precisely so that an empty
// cell is as informative as a filled one -- you can see at a glance that
// nothing drives spacing. That design choice is why the source and target
// sets below are closed enums rather than an open registry: the matrix has a
// fixed shape, and a source that existed but had no column would be invisible
// to the one panel whose whole job is showing you the whole space.
//
// Everything here is pure: no ImGui, no GPU, no PaintSim, no Layer. The panel
// draws it and app/StrokeSession feeds it, but neither is needed to test it.

// ---------------------------------------------------------------------------
// Sources -- the eight normalised inputs, sampled once per dab
// ---------------------------------------------------------------------------
//
// **Every source is normalised to [0,1] here even when the panel displays it
// in its natural unit.** TILT reads "18 deg" and AZIMUTH reads "204 deg" in
// the live gutter, but both arrive as [0,1] -- otherwise every curve in the
// system would need to know its source's domain, and the curve widget (shared
// with the grading stack, which is a [0,1] -> [0,1] widget by contract) could
// not be reused at all. Display conversion belongs to the panel; see
// `sourceDisplay()`.
//
// The design deliberately does not privilege pressure: it is one row like any
// other. That is not a cosmetic decision -- it is why this is an enum with a
// uniform accessor rather than the `bool pressureSize` / `bool pressureFlow`
// pair in app/AppState.hpp's BrushState, which cannot express "tilt drives
// angle" at all.
enum class DynamicSource {
  Pressure,
  Tilt,
  Azimuth,
  Barrel,
  Velocity,
  Fade,
  Noise,
  Random,
};

inline constexpr size_t kDynamicSourceCount = 8;

// Row label in the matrix, and the name in the LINK editor's source popup.
const char* sourceName(DynamicSource source) noexcept;

// The live gutter's text for a normalised value: degrees for the three
// angular sources, two decimals for the rest, and an em dash for Random --
// which has no meaningful "current value" between dabs, and whose gutter cell
// the design draws in the muted grey rather than the foreground.
//
// Writes at most `cap` bytes including the terminator; returns `out`.
const char* sourceDisplay(DynamicSource source, float normalised, char* out, size_t cap) noexcept;

// ---------------------------------------------------------------------------
// Targets -- the twelve parameters a link can drive
// ---------------------------------------------------------------------------
//
// The two-letter column heads are the design's own, and they are the reason
// twelve targets fit in a 322 px column at all. The design names that cost
// honestly ("two-letter column heads, which need learning"), so the full name
// is carried alongside for the LINK editor's popup and for tooltips.
//
// Every one of these now lands somewhere real, but they arrived in three
// waves and it is worth naming which. Six were already fields on BrushState
// (size, angle, roundness, hardness, flow and spacing all read `brush.*`
// directly) before the DYNAMICS matrix's own §2b closed the gap where angle
// and roundness were computed but never reached a dab. Concentration is not a
// seventh field -- it is a second Multiply column onto the SAME `brush.load`
// Flow already reads, exactly as two size links compose in `--selftest`'s own
// worked example. Scatter, Hue, Saturation and Value were genuinely new: none
// had a place to land until `BrushTip::scatter` and the sRGB HSV shift in
// `app/StrokeSession::brushTipFor()` gave them one. Wetness alone stays an
// honest refusal (`targetUnbuildableReason()`): `BrushState::wetness` exists,
// but no CPU deposit route has anywhere to put a water value -- a Pigment
// texel's seven channels do not include one, and giving it one is the solver
// readback bridge's job, not this matrix's.
enum class DynamicTarget {
  Size,           // SZ -- scales BrushTip::radius
  Angle,          // AN -- tip rotation, degrees; additive
  Roundness,      // RD -- minor/major axis ratio of an elliptical tip
  Hardness,       // HD -- scales BrushTip::hardness
  Flow,           // FL -- scales BrushTip::flow
  Scatter,        // SC -- per-dab positional jitter, in radii; additive
  Spacing,        // SP -- scales BrushTip::spacing
  Concentration,  // CT -- scales BrushState::load ("pigment concentration")
  Hue,            // HU -- hue rotation of the deposited colour; additive
  Saturation,     // SA -- scales saturation
  Value,          // VA -- scales value
  Wetness,        // WT -- scales BrushState::wetness
};

inline constexpr size_t kDynamicTargetCount = 12;

// "SZ", "AN", ... -- the matrix column head.
const char* targetAbbrev(DynamicTarget target) noexcept;
// "Size", "Angle", ... -- the LINK editor's target popup.
const char* targetName(DynamicTarget target) noexcept;

// How a target folds together the links that drive it.
//
// **Both operations are commutative and associative, and that is load-bearing
// rather than incidental.** Three sources drive Angle in the design's own
// matrix (tilt, azimuth and barrel all have a filled AN cell), so multi-source
// targets are the normal case, not an edge case -- and if the fold order
// mattered, the resolved value would depend on the order links happened to be
// added to the set, which is a user-invisible detail. Order independence is
// asserted directly in --selftest.
enum class TargetCombine {
  Multiply,  // identity 1.0 -- the target scales its base value
  Add,       // identity 0.0 -- the target offsets its base value
};

TargetCombine targetCombine(DynamicTarget target) noexcept;

// Why `target` cannot be driven from the matrix today, or `nullptr` when it
// can. The wording and the pattern -- a specific sentence naming the missing
// piece, not a generic "not built yet" -- are `app/LayerPanel.hpp`'s own
// `layerKindUnbuildableReason()`, restated here for the same reason: which
// piece is missing differs per target, and that difference is the whole
// information a reader of a greyed cell wants.
//
// Only Wetness returns non-null today. `brush/Deposit`'s Pigment texel has
// seven channels and none of them is water (app/StrokeSession.hpp §1), so
// there is nowhere on the CPU deposit path for a wetness multiplier to go --
// that is the solver readback bridge's job, still owed. Hue, Saturation and
// Value used to belong on this list too, for a different reason (no arbitrary
// foreground colour existed to shift), but that blocker is gone now that
// `BrushState` carries one; see `applyHsvDynamics()` below.
//
// **This answers "is the COLUMN buildable", not "is this CELL"** -- see
// `cellUnbuildableReason()` immediately below for the finer-grained question
// the matrix's individual cells actually need answered.
const char* targetUnbuildableReason(DynamicTarget target) noexcept;

// Why the CELL at (`source`, `target`) cannot be driven, or `nullptr` when it
// can -- a strict refinement of `targetUnbuildableReason()`: every reason
// that function gives still applies (a refused COLUMN refuses every cell in
// it), plus one more that only bites for specific cells within an otherwise
// buildable column.
//
// **HUE/SATURATION/VALUE, from a STROKE-LOCAL source.** `applyHsvDynamics()`
// runs inside `app/StrokeSession::brushTipFor()`, which resolves against
// `dynamicInputsFor()`'s once-per-FRAME hardware sample -- the only place
// this shift happens, because `BrushTip` carries the ALREADY-shifted,
// already-decoded colour (`linearRgb`, the Mixbox `pigment` latent) and not
// the pre-shift sRGB triple, so there is nothing left to re-shift inside the
// per-dab stroke-local correction loop the way Scatter, Flow and the rest
// are. A link from PRESSURE, TILT, AZIMUTH or BARREL to one of these three
// is fully live today; a link from VELOCITY, FADE, NOISE or RANDOM would
// resolve every dab against the same frame-level `dynamicInputsFor()`
// default (0.0) and therefore paint a CONSTANT shift, indistinguishable from
// -- and no better than -- the ORIGINAL `Dry Bristle` defect this whole audit
// item exists to fix. Refusing the twelve cells this describes (three
// targets times four stroke-local sources) is what keeps that defect from
// reappearing in a corner of the matrix nobody happened to test.
const char* cellUnbuildableReason(DynamicSource source, DynamicTarget target) noexcept;

// The identity element for a target's combine rule: 1.0 for Multiply, 0.0 for
// Add. This is what an *undriven* target resolves to, and it is why a link set
// with no links is exactly a no-op rather than approximately one.
float targetIdentity(DynamicTarget target) noexcept;

// ---------------------------------------------------------------------------
// A link
// ---------------------------------------------------------------------------

// The three easing chips in the LINK editor (EASE OUT / LINEAR / S) are
// **presets over the same Curve the widget edits**, not a separate mechanism
// running beside it. One representation means the chips and the curve cannot
// disagree, and it is what lets the design reuse the grading stack's widget
// verbatim -- ops/PointOps.hpp's Curve, evaluated by evalCurve().
enum class EasingPreset {
  Linear,
  EaseOut,
  SCurve,
};

// Control points for a preset, in curve space ([0,1] x [0,1], y up).
Curve easingCurve(EasingPreset preset) noexcept;

// Whether `curve` is (within a small tolerance) one of the three presets --
// what the LINK editor uses to decide which chip to draw as active after the
// user has dragged a control point. Returns false once the curve has been
// edited away from all three, which is the honest answer: the chips describe
// curves, so a curve that is none of them lights none of them.
bool matchesPreset(const Curve& curve, EasingPreset preset) noexcept;

struct BrushLink {
  DynamicSource source = DynamicSource::Pressure;
  DynamicTarget target = DynamicTarget::Size;

  // The response curve, [0,1] -> [0,1]. Empty means linear (evalCurve()'s own
  // degenerate case), so a default-constructed link is a well-defined
  // pass-through rather than something that must be initialised before use.
  Curve curve;

  // **The output range, not an input gate.** At curve output 0 the link
  // resolves to `rangeLo`; at 1, to `rangeHi`. The design's worked example is
  // PRESSURE -> SIZE at RANGE 0.10-1.00, which reads as "size never drops
  // below 10% of the tip radius however light the touch" -- the same
  // semantics as a minimum-diameter control, and the reading that makes the
  // editor's OUT figure (17.1 px against a 24 px radius) come out right.
  //
  // Defaults are per-target (`targetDefaultRange()`): [0,1] scales a Multiply
  // target across its whole span, while Angle wants degrees.
  float rangeLo = 0.0f;
  float rangeHi = 1.0f;

  // Mirrors the source about the middle of its domain before the curve is
  // applied -- t -> 1-t. Kept as a flag rather than folded into the curve so
  // that INVERT survives a curve edit, which is what the toggle in the editor
  // implies: you can flip a hand-drawn response without redrawing it.
  bool invert = false;

  // A link the user has switched off keeps its curve and range. The matrix
  // draws a disabled cell as empty, so this is only reachable from the LINK
  // editor -- but discarding the authored curve on a toggle would make the
  // toggle destructive, which no toggle in this app is.
  bool enabled = true;
};

// The resolved contribution of one link at one source value, already folded
// through invert, curve and range. In `targetCombine()`'s units: a factor for
// a Multiply target, an offset for an Add target.
//
// `source` is clamped to [0,1]; the curve's own output is clamped too, since
// a hand-authored control point can sit outside the unit square (the curve
// widget does not confine y, by ops/PointOps.hpp's contract) and a negative
// size factor is not a thing the deposit path should ever have to defend
// against.
float linkContribution(const BrushLink& link, float source) noexcept;

// ---------------------------------------------------------------------------
// Live inputs, the set, and the resolved result
// ---------------------------------------------------------------------------

// One sample of all eight sources. Every field is normalised to [0,1]; see
// DynamicSource's comment for why the angular ones are not in degrees.
//
// Defaults are the values a mouse produces: full pressure, no tilt, no
// motion. **This matters more than it looks.** A tablet is not required to
// paint, so the whole dynamics system has to degrade to "the tip as authored"
// when the only pointer is a mouse -- and it does, because a default-
// constructed DynamicInputs through a default link set resolves every target
// to its identity.
struct DynamicInputs {
  float pressure = 1.0f;
  float tilt = 0.0f;
  float azimuth = 0.0f;
  float barrel = 0.0f;
  float velocity = 0.0f;
  float fade = 0.0f;
  float noise = 0.0f;
  float random = 0.0f;
};

// Uniform accessor -- the reason the sources are an enum at all. The matrix
// walks rows generically; nothing in the panel special-cases pressure.
float sourceValue(const DynamicInputs& inputs, DynamicSource source) noexcept;

// The twelve resolved values, indexed by DynamicTarget. Undriven targets hold
// their combine rule's identity.
struct DynamicResult {
  float value[kDynamicTargetCount];

  float at(DynamicTarget target) const noexcept {
    return value[static_cast<size_t>(target)];
  }
};

// A brush's whole dynamics configuration: the "12 LINKS" the panel header
// counts.
//
// A flat vector rather than a fixed 8x12 array of optionals. The matrix is
// sparse in every real brush -- the design's own example fills 12 of 96 cells
// -- and the flat form is what the LINK editor iterates, what the preset file
// serialises, and what makes "12 LINKS" a `size()` rather than a scan.
struct BrushLinkSet {
  std::vector<BrushLink> links;
};

// Index of the link driving `target` from `source`, or `kNoLink`.
//
// **At most one link may occupy a matrix cell**, which is what makes the
// matrix a faithful picture of the configuration: a cell is a link, so two
// links in one cell would draw as one. `addLink()` enforces it by replacing.
inline constexpr size_t kNoLink = static_cast<size_t>(-1);
size_t findLink(const BrushLinkSet& set, DynamicSource source,
                DynamicTarget target) noexcept;

// Install a link, replacing any link already in that cell. Returns its index.
size_t addLink(BrushLinkSet& set, const BrushLink& link);

// Remove the link in that cell if there is one. Returns whether it removed
// anything.
bool removeLink(BrushLinkSet& set, DynamicSource source, DynamicTarget target);

// The default output range for a target, used when the LINK editor creates a
// link from an empty cell. [0,1] for the scaling targets; Angle spans a full
// turn in degrees, because a rotation link that defaulted to a one-degree
// span would look broken rather than subtle.
void targetDefaultRange(DynamicTarget target, float& lo, float& hi) noexcept;

// What a new brush starts with: the two links that were `BrushState`'s
// `pressureSize` and `pressureFlow` booleans.
//
// **Those two booleans were already links, with the ranges written into them
// as literals.** Both routes that read them applied exactly `0.25 + 0.75p` to
// the radius and `0.15 + 0.85p` to the flow, which is this model's
// `linkContribution()` at ranges [0.25,1] and [0.15,1] through a linear curve
// -- not approximately, identically. So the migration off the booleans is
// exact rather than a re-tuning, and --selftest asserts the two agree across
// the pressure range rather than taking that on the comment's word.
//
// A brush with no links would also have been defensible, but it would mean a
// tablet did nothing until the user opened the matrix and drew a curve, and
// it would silently change how every existing brush feels.
BrushLinkSet defaultBrushLinks();

// Resolve the whole set against one sample.
//
// Disabled links are skipped. Every target starts at its identity and folds
// in each contribution with its combine rule, so the result is total: all
// twelve entries are always meaningful, whether or not anything drives them.
DynamicResult evaluateLinks(const BrushLinkSet& set, const DynamicInputs& inputs) noexcept;

// ---------------------------------------------------------------------------
// The stroke-local sources -- VELOCITY, FADE, NOISE and RANDOM
// ---------------------------------------------------------------------------
//
// The other four (Pressure, Tilt, Azimuth, Barrel) are hardware readings:
// `app/StrokeSession::dynamicInputsFor()` samples them straight off a pen,
// once per render frame, before a stroke's geometry is even known. These four
// cannot be -- they are properties of the stroke itself (how fast it moved,
// how far it has travelled, a value that should wander smoothly or jump
// freshly along it) -- so they are resolved once per DAB, inside the deposit
// loop, and are pure functions here rather than anything read off AppState.
//
// **Why NOISE and RANDOM may not call `rand()`, seed `std::mt19937` from the
// clock, or hold any mutable generator state at all.** `core/History` (ADR-
// 0005) replays a stroke's own dab stream to reconstruct a document from a
// keyframe, and the golden harness (`tools/golden/run_golden.sh`) compares a
// script's output byte-for-byte across runs of the same binary. Either one
// re-executes this stroke's dabs through this exact arithmetic a second time,
// and expects the second run to agree with the first at every dab. A generator
// with internal state must be advanced in lockstep to replay identically --
// one skipped or extra draw anywhere upstream (a hovered-but-not-painted
// frame, a UI redraw between undo and redo) desyncs it silently, and a
// clock- or `rand()`-seeded one cannot even promise that much, since the
// clock itself does not replay. A pure function of (seed, dab index) or
// (seed, distance) has neither failure mode: called twice with the same
// arguments, in the same process or a different one, it returns the same
// float, forever, with no state to fall out of step.
//
// **The seed.** `app/StrokeSession` carries no other per-stroke identity --
// `Layer::id` is 0 until `core::normalizeLayerIds()` first runs (core/
// Layer.hpp), and nothing else on `OpenDocument` or `Document` names a stroke
// at all -- so one is built from data the stroke already has to carry to be
// replayable in the first place: **its own first recorded dab position.**
// `strokeSeedFromStart()` hashes those two floats' bit patterns. That is
// stroke-local (fixed for the stroke's whole life, known from its very first
// sample) and reproducible under everything this project replays a stroke
// through -- undo/redo (the position stream is what the history entry's tiles
// were painted from), a keyframe replay (the dab stream IS the input), and a
// second run of the same golden script (the same synthetic pointer path
// produces the same first position, bit for bit, on the same build). No
// counter, no clock, nothing outside the stroke's own recorded geometry.
//
// Two strokes that happen to start at the identical texel get the identical
// seed and therefore the identical Noise/Random sequence -- an accepted
// collision, not a defect: this is a *visual variation* feature, not a
// cryptographic one, and `--selftest` demonstrates two strokes starting at
// different positions diverge, which is the property that matters.

// A fast, well-mixed 64-bit hash with no hidden state (Vigna 2015,
// "splitmix64", public domain). Chosen over `std::hash` (implementation-
// defined per type, not required to be stable even within one process) and
// over seeding `std::mt19937` (a generator object, not a pure function, and
// per-call overkill for one float) for the reason above: the same input
// always produces the same output, in this run and in every later one.
uint64_t splitmix64(uint64_t x) noexcept;

// The stroke's own seed, from the bit pattern of its first recorded dab
// position. See this section's own comment for why position rather than a
// counter or a clock.
uint64_t strokeSeedFromStart(float startX, float startY) noexcept;

// RANDOM: a fresh, uniform draw in [0,1) for dab number `dabIndex` of the
// stroke seeded `seed`. Pure in (seed, dabIndex) -- call it twice with the
// same pair and it returns the same float bit for bit, which is the whole
// property a replayed stroke needs. `dabIndex` is `StrokeSession::dabCount()`
// at the moment this dab is being resolved, i.e. 0 for the stroke's first
// dab, 1 for its second, and so on -- **not** an index into `pending_`, which
// resets every frame and would collide across frames.
float dynamicRandomDraw(uint64_t seed, uint32_t dabIndex) noexcept;

// NOISE: smooth and continuous along the stroke, in [0,1] -- value noise over
// arc length rather than a fresh draw per dab, because a fresh draw per dab is
// what RANDOM already is, and a "noise" row that read identically to "random"
// would be one row the matrix did not need. Built from `dynamicRandomDraw()`'s
// own hash at integer lattice points `kNoisePeriodPx` apart, smoothstep-
// interpolated between the two the query distance falls between -- the
// standard value-noise construction, chosen over a sum of sines (which is
// periodic and therefore eventually repeats along a long stroke, visibly) and
// over literally reusing Perlin/Simplex (this only ever needs a 1-D line
// through the field, which the lattice-plus-smoothstep form gives directly,
// with no gradient table or 2-D machinery a 1-D consumer would carry for
// nothing).
float dynamicNoiseAt(uint64_t seed, float distanceAlongStroke) noexcept;

// VELOCITY: pointer speed, normalised to [0,1] by the CURRENT tip's own
// radius rather than by a fixed pixel constant. `stepDistancePx` is how far
// this dab is from the previous one; `radiusPx` is `tip.radius` at the moment
// of the dab. **The empirical basis**: a dab is emitted every `spacing *
// radius` pixels of arc length (`BrushTip::spacingPx()`, ADR-0003), and the
// shipped default spacing is 0.25 -- so at the default tip, four dabs land in
// one radius of travel under NORMAL painting motion. A pointer that covers a
// full radius **between two consecutive dabs** is therefore already moving
// roughly four times faster than an ordinary stroke's dab-to-dab spacing
// implies, which is the geometric definition of "fast" this brush already
// has, without inventing a px/frame constant this class has no clock to
// justify (`brush/StrokePath` is deliberately clockless -- ADR-0003 again:
// deposition depends on distance, never on time or event count). Radii per
// dab-to-dab step, not radii per second: there is no timestamp anywhere in
// `StrokePath` or `StrokeSession` to divide by, and inventing one only for
// this source would make Velocity the one dynamics row whose meaning changed
// if the render loop's frame rate did.
//
// The first dab of a stroke has no previous position -- `stepDistancePx` is
// **0.0 by the caller's own contract**, not a special case here, and 0
// distance normalises to 0 velocity by the same formula as every other
// distance: a stroke that has not moved yet is exactly as "fast" as one held
// perfectly still, which is the same truth `DynamicInputs`' own defaults
// state for pressure and tilt ("the values a mouse produces... no motion").
float dynamicVelocity(float stepDistancePx, float radiusPx) noexcept;

// FADE: a ramp from 0 to 1 over `distanceAlongStroke`, reaching 1 at exactly
// `kFadeLengthPx` and staying there for the rest of the stroke. Distance
// rather than dab count, for ADR-0003's own reason restated once more: dab
// count is spacing-dependent (a pressure-sized brush whose radius shrinks
// mid-stroke emits MORE dabs over the same distance), so a dab-count ramp
// would fade at a different physical LENGTH depending on how hard the stroke
// pressed. Arc length is the one quantity every route in this file already
// agrees is what a stroke "amount" means.
inline constexpr float kFadeLengthPx = 480.0f;  // 20 radii of the 24 px default
                                                // tip (brush/Deposit.hpp's
                                                // BrushTip::radius default) --
                                                // long enough that a fade
                                                // reads as a deliberate stroke
                                                // running out, not a click
float dynamicFade(float distanceAlongStroke) noexcept;

// Whether `source` is one of the four resolved once per dab rather than once
// per frame -- see this section's own comment for why the split exists.
bool sourceIsStrokeLocal(DynamicSource source) noexcept;

// `evaluateLinks()` restricted to the links whose source's
// `sourceIsStrokeLocal()` equals `wantStrokeLocal`. Because `TargetCombine`'s
// fold is commutative and associative (asserted in `--selftest`), resolving
// the hardware half and the stroke-local half of one link set separately and
// then composing the two partial `DynamicResult`s with each target's own rule
// -- multiply the multiplies, add the adds -- reproduces `evaluateLinks()` on
// the whole set exactly. That identity is what lets `app/StrokeSession`
// resolve the two halves at two different times (frame vs dab) without a
// second link-evaluation mechanism, and it is asserted directly in
// `DynamicsSources.cpp` rather than only argued here.
DynamicResult evaluateLinksFiltered(const BrushLinkSet& set, const DynamicInputs& inputs,
                                    bool wantStrokeLocal) noexcept;

// ---------------------------------------------------------------------------
// HUE, SATURATION and VALUE -- shifting the deposited colour
// ---------------------------------------------------------------------------
//
// Applied in sRGB (display-referred, gamma-encoded), the space
// `paint::Pigment::rgb` and the picked foreground colour are already stored
// in -- deliberately NOT in the linear working space `color/Space.hpp`
// documents for document content. Two reasons, not one:
//
//   * HSV/HSB is a perceptual construction over a colour CUBE, built on the
//     assumption that equal steps in each axis read as roughly equal visual
//     steps. That assumption is what sRGB's gamma encoding exists to
//     approximate (`color/Space.hpp`'s own transfer-function comment) -- it
//     is false of scene-linear light, where the same arithmetic hue rotation
//     lands at a visibly different, gamma-dependent angle. Every consumer-
//     facing colour picker (this project's own COLOR panel included) turns
//     its wheel in gamma space for exactly this reason.
//   * `srgbDecode()`/`lut.rgbToLatent()` already expect an sRGB triple as
//     their input (`app/StrokeSession::brushTipFor()`), so shifting BEFORE
//     that decode is one extra step in a pipeline that does not otherwise
//     change -- shifting after would mean decoding, converting back to sRGB
//     to do the perceptual math, then re-encoding, for the identical result
//     paid for twice.
//
// **What this does NOT shift.** In Pigment mode a stroke's density, staining
// and granulation come from the palette row's own physical constants
// (`paint::Pigment`), not from its RGB triple -- there is no formula that
// derives "how granular does this pigment settle" from a colour. A Hue link
// therefore changes the deposited hue while those three properties stay the
// SWATCH's own, unrotated. That is not a bug to hide: `drawLinkEditor()`
// (`ui/MacPaintUI.cpp`) says so on the Hue/Saturation/Value cells, in the
// LINK editor, where a user who wired one up would otherwise have no way to
// find out. It will stop being a caveat, not become a lie, on the day a
// Pigment layer gains a real physical basis for a shifted swatch -- which is
// the solver readback bridge (`app/StrokeSession.hpp` §4), not this file.
struct Hsv {
  float h = 0.0f;  // turns, wraps at 1 -- NOT degrees, to match every other
                   // normalised source/output in this header
  float s = 0.0f;  // 0..1
  float v = 0.0f;  // 0..1
};

// sRGB (each channel 0..1, gamma-encoded) -> HSV. Grey (r==g==b) reports
// s=0 with h=0 by convention -- hue is undefined at zero saturation, and 0
// is the same convention `evalCurve()`'s own degenerate cases use elsewhere
// in this file: a well-defined answer for an input with no "real" one, rather
// than a NaN a caller has to guard against.
Hsv rgbToHsv(std::array<float, 3> srgb) noexcept;

// HSV -> sRGB. `h` wraps (any real value is valid input); `s` and `v` are
// clamped to [0,1] before conversion so an out-of-range shift cannot hand
// back a component outside it either.
std::array<float, 3> hsvToRgb(Hsv hsv) noexcept;

// The DYNAMICS matrix's Hue/Saturation/Value shift, as one call:
// `hueTurnsOffset` is `DynamicResult::at(Hue)` (an Add target, identity 0.0,
// signed turns about the wheel -- `targetDefaultRange()`'s own [-0.5, 0.5]);
// `satMul`/`valMul` are `at(Saturation)`/`at(Value)` (Multiply targets,
// identity 1.0, same [0,1] convention as every other scaling target: a link
// at output 0 mutes the channel to nothing, at 1 leaves it untouched).
//
// **Skips the HSV round trip entirely at the identity** (`hueTurnsOffset ==
// 0.0f && satMul == 1.0f && valMul == 1.0f`) and returns `srgb` unchanged --
// not an optimisation, the same discipline `brush/Deposit.hpp` §2b's round-
// tip branch uses for the identical reason: `rgbToHsv()` then `hsvToRgb()` is
// not guaranteed to round-trip to the bit-identical float at every input (a
// grey pixel's hue is a convention, not a measurement), so running it
// unconditionally would move every existing stroke -- every brush with no
// Hue/Saturation/Value link, which today is all of them -- by a last-bit
// rounding no golden image or `--pigment-stroke-demo` reference asked for.
std::array<float, 3> applyHsvDynamics(std::array<float, 3> srgb, float hueTurnsOffset,
                                      float satMul, float valMul) noexcept;

}  // namespace np
