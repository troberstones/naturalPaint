#pragma once

#include <cstddef>
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
// Seven of these already exist as fields on BrushState or BrushTip (size,
// hardness, flow, spacing, concentration, wetness) or on the palette entry
// (the three pigment constants are not targets -- they belong to the pigment,
// not the brush). Five are new parameters that the tip does not yet have:
// angle, roundness, scatter, and the hue/saturation/value jitter trio.
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

}  // namespace np
