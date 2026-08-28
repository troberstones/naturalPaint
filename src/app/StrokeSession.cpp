#include "app/StrokeSession.hpp"

#include <algorithm>
#include <cmath>

#include "color/Space.hpp"

namespace np {
namespace {

// A per-dab XOR salt, folded into the seed before drawing SCATTER's random
// direction -- see `applyPerDabScatter()`. Any fixed odd 64-bit constant
// would decorrelate the two streams equally well; this one is the ASCII
// bytes of "scatter1" read as a little-endian word, chosen only so a reader
// grepping the binary for something recognisable has a chance of finding it,
// not for any property of the bits themselves.
constexpr uint64_t kScatterAngleSalt = 0x3172657474616373ULL;  // "scatter1" (LE)

// Two 2*pi's worth of named precision rather than a bare literal: the
// dynamics matrix's other angular sources (`sourceDisplay()`'s tilt/azimuth/
// barrel cases) all go through `%.0f` degrees, and this is the one place in
// the deposit path that turns a [0,1) draw into radians directly.
constexpr float kTwoPi = 6.28318530717958647692f;
// A quarter turn -- what `applyPerDabScatter()` rotates the stroke's own
// tangent by to find its perpendicular.
constexpr float kHalfPi = kTwoPi * 0.25f;

// Folds the per-dab corrections from `strokeLocalLinks_` -- resolved against
// VELOCITY/FADE/NOISE/RANDOM/DIRECTION/INITIAL-DIRECTION alone, via
// `evaluateLinksFiltered()` -- onto a tip already resolved against the four
// hardware sources (`brushTipFor()`'s own `dyn`, baked into `base` at
// `setTip()` time).
//
// **Multiplying/adding the stroke-local factor onto the already-resolved
// field, rather than re-running `evaluateLinks()` on the WHOLE link set with
// both halves of the inputs filled in, is not a shortcut -- it is required by
// this file's per-dab granularity.** The hardware four are sampled once per
// render frame (`dynamicInputsFor()`), before this dab's position is even
// known; the stroke-local four are sampled once per DAB, inside this very
// loop. There is no single instant at which "the whole `DynamicInputs`" for
// this dab actually exists. Composing two partial resolutions of the SAME
// link set is what `evaluateLinksFiltered()`'s own comment proves equal to
// resolving it in one pass: `TargetCombine`'s fold is commutative and
// associative (asserted in `--selftest`'s "order independence" section), so
// multiplying the two Multiply-target partials and adding the two Add-target
// partials reproduces exactly the number one whole-set `evaluateLinks()`
// call would have produced, had one been possible.
//
// Spacing is deliberately NOT corrected here: `BrushTip::spacingPx()` is
// consumed once, by `StrokePath`, before this loop ever runs (`pending_` is
// already the emitted dab stream by the time `depositPending()` executes),
// so a stroke-local spacing correction at this point would have no consumer
// -- it could not un-emit or re-emit a dab that has already been decided.
// **Does NOT apply `base.sizeFloorPx`.** `out = base;` below carries it
// through unchanged (the floor in pixels does not move just because the
// stroke-local half of the product is about to multiply `radius` again), and
// it stays unapplied until `depositPending()`'s own single `std::max()` after
// this function returns -- see `BrushTip::sizeFloorPx`'s comment for why
// applying it here, before that multiply, would be the wrong half of the
// counter-example it works through.
BrushTip applyStrokeLocalCorrection(const BrushTip& base, const DynamicResult& corr) noexcept {
  BrushTip out = base;
  out.radius *= corr.at(DynamicTarget::Size);
  out.hardness *= corr.at(DynamicTarget::Hardness);
  out.roundness *= corr.at(DynamicTarget::Roundness);
  out.angle += corr.at(DynamicTarget::Angle);
  out.flow *= corr.at(DynamicTarget::Flow) * corr.at(DynamicTarget::Concentration);
  out.scatter += corr.at(DynamicTarget::Scatter);
  return out;
}

}  // namespace

// SCATTER's own axis. Default (Photoshop's own default, "Both Axes"
// unticked, docs/reachability-audit.md B5): confined to the stroke's
// PERPENDICULAR -- `stepDx`/`stepDy` is the identical step vector
// `dynamicDirection()` turns into DIRECTION (`depositPending()`'s own `(p -
// prevDab)`), rotated a quarter turn, so a rougher stroke reads as WIDER
// rather than as a second, blurrier line running along the original. One
// random draw per dab, off a SALTED copy of the stroke seed, chooses which
// of the two resulting directions the dab lands on -- salted rather than
// reused from `dynamicRandomDraw(seed, dabIndex)` directly, because a brush
// that links RANDOM to both SCATTER and FLOW (`brush/Library.cpp`'s `Dry
// Bristle` does exactly this) would otherwise jitter and thin in visible
// lockstep -- wide exactly when faint, narrow exactly when heavy -- which
// reads as one coupled effect rather than the two independent ones the
// matrix promises.
//
// `tip.scatterBothAxes` switches to the OLD isotropic behaviour: the same one
// draw picks a full-circle angle instead of a side, so a dab can land
// anywhere on a ring of radius `magnitude` around `centre` -- what this
// function did unconditionally before B5, correct for a brush that actually
// asks for it and wrong (a blurrier line, not a rougher one) for every brush
// that did not.
//
// **The first dab's zero step vector is not a special case.** `stepDx =
// stepDy = 0.0` is the caller's own contract for "no previous position yet"
// (`app/StrokeSession.hpp`'s `depositPending()`, the identical convention
// `dynamicDirection()`'s own header section states), and `std::atan2(0, 0)`
// is `0` by the C++ standard for the signs this file ever produces -- "due
// +x" -- so the first dab's perpendicular is due +y, exactly as deterministic
// as every later dab's, rather than undefined.
Vec2 applyPerDabScatter(Vec2 centre, const BrushTip& tip, uint64_t seed, uint32_t dabIndex,
                        float stepDx, float stepDy) noexcept {
  if (tip.scatter == 0.0f) return centre;  // the identity: no branch taken,
                                           // no draw spent, for every brush
                                           // with nothing linked to SCATTER
  const float draw = dynamicRandomDraw(seed ^ kScatterAngleSalt, dabIndex);
  const float magnitude = tip.scatter * tip.radius;
  if (tip.scatterBothAxes) {
    const float angle = draw * kTwoPi;
    return Vec2{centre.x + std::cos(angle) * magnitude, centre.y + std::sin(angle) * magnitude};
  }
  const float perp = std::atan2(stepDy, stepDx) + kHalfPi;
  const float side = (draw < 0.5f) ? -1.0f : 1.0f;
  return Vec2{centre.x + std::cos(perp) * magnitude * side,
             centre.y + std::sin(perp) * magnitude * side};
}

const char* strokeRouteName(StrokeRoute route) noexcept {
  switch (route) {
    case StrokeRoute::None: return "none";
    case StrokeRoute::CpuDeposit: return "cpu-deposit";
    case StrokeRoute::RgbDeposit: return "rgb-deposit";
    case StrokeRoute::RgbErase: return "rgb-erase";
    case StrokeRoute::PigmentErase: return "pigment-erase";
    case StrokeRoute::PaintSim: return "paint-sim";
  }
  return "?";
}

StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept {
  // The one bit that separates §1's two families of stroke: a deposit adds and
  // an erase removes, and every row below differs only in that. Carried as a
  // flag through one shared body rather than as a second copy of the target
  // tests, because the target half of the question -- locked before kind, a
  // store to write being part of the question, "no target" being its own row --
  // has exactly one right answer and two copies of it would drift.
  bool erasing = false;

  switch (tool) {
    case Tool::Brush:
    case Tool::DryBrush:
      break;
    // PRD F9/F10 (both P0) and ADR-0007. This case used to sit in the
    // not-built list below, so the eraser routed nowhere and did nothing at
    // all -- see the header §1's Eraser rows.
    case Tool::Eraser:
      erasing = true;
      break;
    // Water deposits water and no pigment, and a Pigment tile has no water
    // channel -- see the header §1.
    case Tool::Water:
      return StrokeRoute::PaintSim;
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    // The palette cells app/AppState.hpp's Tool comment says exist for
    // their name/icon/slot only -- none of them is built, so none of them can
    // route anywhere but here. Listed rather than caught by a `default:` so
    // that a real implementation moves its tool out of this list instead of
    // silently keeping the fallback -Wswitch would no longer force a look at.
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::CloneStamp:
    case Tool::PaintBucket:
    case Tool::Gradient:
    case Tool::Pencil:
    case Tool::Smudge:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      return StrokeRoute::None;
  }

  // **No target at all is the one case the solver canvas is right for**, and
  // it is checked first so that it reads as its own row rather than as the
  // remainder of the ones below it (header §1's last paragraph). No document is
  // open, so there is no layer the user could have aimed at, and watercolour
  // and oil paint the dense canvas texture legitimately.
  //
  // **Except for the eraser**, which is the one row where the two families part
  // company: `sim::PaintSim` has no alpha and no erase step, so an eraser sent
  // there would run the paint path and *add* pigment where the user asked for
  // its removal. Nothing to erase, so nowhere to go.
  if (target == nullptr) return erasing ? StrokeRoute::None : StrokeRoute::PaintSim;

  // Locked before kind, so a locked layer refuses for being locked whatever it
  // is made of -- and so the UI's "clear its Lock in LAYERS" message is the one
  // a user gets for the one problem they can actually fix.
  if (target->locked) return StrokeRoute::None;

  // A store to write is part of the question, not a precondition: `Layer.hpp`'s
  // "at most one of rgbTiles and pigmentTiles is engaged" allows a layer of
  // either kind with neither, and painting one has nowhere to go.
  //
  // **A Pigment layer takes both now**, and the header §1 carries the argument
  // for the row that changed. It used to refuse the erase by name, on one
  // stated condition -- `depositDab(PigmentTileStore&, ...)` had no selection
  // parameter, so a Pigment erase built then would have been the one tool on
  // that layer kind either stopping at the marching ants alone or destroying
  // paint outside a selection drawn to protect it. `brush/Deposit` §4 is that
  // gate and `brush/PigmentErase` is the row, and they landed together as the
  // one decision the refusal asked for.
  if (target->kind == LayerKind::Pigment && target->pigmentTiles)
    return erasing ? StrokeRoute::PigmentErase : StrokeRoute::CpuDeposit;
  if (target->kind == LayerKind::RGB && target->rgbTiles) {
    // Alpha lock refuses the ERASER, and only the eraser. `StrokeRoute::RgbErase`
    // exists to take alpha OUT of the layer (brush/RgbErase.hpp §0), which is
    // exactly the quantity `alphaLocked` freezes (core/Layer.hpp's own comment
    // on the member; brush/RgbDeposit.hpp §4.5 derives the rule) -- letting the
    // erase through would make the flag decorative. `RgbDeposit` is NOT
    // affected: painting on an alpha-locked layer is the whole feature, not a
    // refusal, and `brush/RgbDeposit.cpp`'s `depositRgbTexel()` is where the
    // colour-only composite actually happens, not here.
    if (erasing && target->alphaLocked) return StrokeRoute::None;
    return erasing ? StrokeRoute::RgbErase : StrokeRoute::RgbDeposit;
  }

  // Everything left is a real target that cannot take a stroke: an Adjustment
  // layer (no tiles by construction), a Media/Strokes/Text/Flats layer (no
  // storage built yet), or a Pigment/RGB layer whose store was never allocated.
  // **`None`, not `PaintSim`** -- see §1. Falling through to the solver here is
  // what made "select an RGB layer and paint" put colour on the canvas texture
  // instead of on the layer, invisibly, one line below the locked row that
  // exists to prevent exactly that.
  return StrokeRoute::None;
}

const char* strokeEditLabel(Tool tool) noexcept {
  switch (tool) {
    case Tool::Brush: return "brush stroke";
    case Tool::DryBrush: return "dry brush stroke";
    case Tool::Water: return "water stroke";
    // **"erase", not "brush stroke"** -- a noun for what the edit did, in the
    // form `core/LayerOps`' `editLabel` uses. PRD O2's panel is scanned to find
    // an edit to undo, and a row that says "brush stroke" about a gesture that
    // took paint *off* is the one row a user cannot recognise.
    case Tool::Eraser: return "erase";
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    // Same not-built list as strokeRouteFor() above, and the same reason
    // it is spelled out rather than a `default:` -- none of these can begin
    // a stroke (strokeRouteFor() refuses them all), so none of them needs a
    // label of its own; they fall through to the generic one below.
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::CloneStamp:
    case Tool::PaintBucket:
    case Tool::Gradient:
    case Tool::Pencil:
    case Tool::Smudge:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      break;
  }
  return "stroke";
}

// --- the pixel-writing ops that are not strokes (header §6) ----------------

bool toolWritesRgbPixels(Tool tool) noexcept {
  return tool == Tool::PaintBucket || tool == Tool::Gradient;
}

bool toolBeginsStroke(Tool tool) {
  // Two synthetic targets cover every row of §1's table that is not the
  // no-target case: a writable RGB layer and a writable Pigment layer. Neither
  // is locked and both have a store, so a tool that refuses on both refuses for
  // being that tool -- which is exactly the question. Probing rather than
  // re-listing is the whole point: `strokeRouteFor()` is one table, and a tool
  // moving out of its not-built arm changes this answer for free.
  Layer rgb;
  rgb.kind = LayerKind::RGB;
  rgb.rgbTiles.emplace();
  Layer pigment;
  pigment.kind = LayerKind::Pigment;
  pigment.pigmentTiles.emplace();
  return strokeRouteFor(tool, nullptr) != StrokeRoute::None ||
         strokeRouteFor(tool, &rgb) != StrokeRoute::None ||
         strokeRouteFor(tool, &pigment) != StrokeRoute::None;
}

bool toolDrawsSelection(Tool tool) noexcept {
  return tool == Tool::Marquee || tool == Tool::EllipseMarquee || tool == Tool::Lasso ||
         tool == Tool::PolygonLasso || tool == Tool::MagicWand;
}

bool toolSamplesCanvas(Tool tool) noexcept { return tool == Tool::Eyedropper; }

bool toolPansView(Tool tool) noexcept { return tool == Tool::Hand; }

PixelOpRefusal pixelOpRefusalFor(const Layer* target) noexcept {
  if (target == nullptr) return PixelOpRefusal::NoLayer;
  // Before the storage test, so a locked RGB layer refuses for the reason that
  // has a fix. See the header.
  if (target->locked) return PixelOpRefusal::Locked;
  // **`rgbTiles`, not `kind == RGB`.** core/Layer.hpp allows a layer of either
  // paint kind to carry neither store, and ops/FloodFill and ops/Gradient both
  // take a `TileStore&` -- so the question is whether there is one to write,
  // not what the kind tag says. `strokeRouteFor()` draws the same distinction
  // just above its own fallthrough, for the same reason.
  if (!target->rgbTiles.has_value()) return PixelOpRefusal::NoRgbStore;
  return PixelOpRefusal::None;
}

std::string pixelOpRefusalMessage(PixelOpRefusal reason, const Layer* target,
                                  const char* opName) {
  const std::string op = opName != nullptr ? opName : "fill";
  const std::string name = target != nullptr ? target->name : std::string();
  switch (reason) {
    case PixelOpRefusal::None:
      return {};
    case PixelOpRefusal::NoLayer:
      // No name to give, because there is nothing to name -- and the fix is a
      // different one from the other two, so it says so rather than reusing
      // "pick a layer in LAYERS" for a stack that may have none.
      return "no layer: the " + op +
             " has nothing to fill. Open a document, or add a layer in LAYERS.";
    case PixelOpRefusal::Locked:
      return "locked layer: \"" + name + "\" cannot take the " + op +
             ". Clear its Lock in LAYERS.";
    case PixelOpRefusal::NoRgbStore:
      // Names the KIND as well as the layer: "Pigment" is the answer to "why
      // not", and a user who has just made a layer from the NEW popup's first
      // entry has no other way in the chrome to find out which of the seven
      // kinds they picked.
      return "\"" + name + "\" is " +
             (target != nullptr ? layerKindName(target->kind) : "?") +
             " and has no RGB pixels for the " + op + ". Pick an RGB layer in LAYERS.";
  }
  return {};
}

BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut, float pressure) {
  DynamicInputs in;
  in.pressure = pressure;  // evaluateLinks() clamps; see linkContribution()
  return brushTipFor(brush, lut, in);
}

BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut,
                     const DynamicInputs& inputs) {
  // Resolved here rather than at each call site so the two routes cannot drift
  // apart. A pen configured for size-only pressure must feel the same
  // whichever layer kind it lands on -- which is the reason the two hardcoded
  // curves were pulled into one place before, and the reason the link set that
  // replaced them is read in one place now.
  //
  // **The four HARDWARE sources only, and the filter is load-bearing rather
  // than an optimisation.** This file's own section comment above
  // `applyStrokeLocal()` states the split everything downstream depends on: a
  // tip arrives at `setTip()` "already resolved against the four hardware
  // sources", and `StrokeSession` folds the stroke-local six
  // (VELOCITY/FADE/NOISE/RANDOM/DIRECTION/INITIAL-DIRECTION) on per DAB,
  // because those are sampled at a position that does not exist yet when
  // this runs.
  //
  // This line used to read `evaluateLinks(brush.links, inputs)` -- the WHOLE
  // set -- and the consequence was that every stroke-local link was applied
  // TWICE: once here against `DynamicInputs`' defaults, and once per dab with
  // its real value. A source defaulted to 0 makes its link contribute exactly
  // `rangeLo`, so the spurious extra factor was the link's own floor. An
  // imported Photoshop brush with a 30% minimum size painted at 30% of the
  // size it asked for, and one whose floor was 0.00 painted **nothing at all**
  // -- silently, with a completely healthy link set and no refusal anywhere.
  //
  // Measured over Kyle's Runny Inkers (`--brush-sheet`, peak stroke width per
  // brush): the attenuation matched each brush's RANDOM -> Size floor across
  // all twelve, the one brush in the library with no such link was the only
  // one at full width, and the brush whose floor is 0.00 was invisible.
  const DynamicResult dyn = evaluateLinksFiltered(brush.links, inputs,
                                                  /*wantStrokeLocal=*/false);
  const float sizeMul = dyn.at(DynamicTarget::Size);
  const float flowMul = dyn.at(DynamicTarget::Flow);

  BrushTip tip;
  tip.radius = brush.radius * sizeMul;
  // The floor UNDER this product, in pixels -- computed from `brush.radius`
  // itself, the UNSCALED base radius, while it is still the value in hand
  // (the line just above already multiplied it by `sizeMul` into `tip.radius`).
  // Deliberately not applied to `tip.radius` here; see `BrushTip::sizeFloorPx`'s
  // own comment (brush/Deposit.hpp) for the whole argument, including the
  // worked counter-example for why applying `max()` at this point -- before
  // the stroke-local half of the product has had its own turn -- is wrong
  // rather than merely early.
  tip.sizeFloorPx =
      brush.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] * brush.radius;
  tip.hardness = brush.hardness * dyn.at(DynamicTarget::Hardness);
  // **These two used to be dropped here**, and brush/Deposit.hpp §2b is the
  // whole account of what that cost: two sliders, two DYNAMICS columns, a
  // shipped preset and a Photoshop importer all describing a tip shape that
  // nothing painted. Each takes its own combine rule from the matrix rather
  // than a rule chosen here -- Roundness multiplies (a link scales the ratio)
  // and Angle adds (a link offsets the rotation), which is exactly what
  // `targetCombine()` says of each, and is why `evaluateLinks()` hands back
  // 1.0 and 0.0 respectively for an undriven target and these two lines are
  // therefore a no-op for a brush with no links.
  tip.roundness = brush.roundness * dyn.at(DynamicTarget::Roundness);
  tip.angle = brush.angle + dyn.at(DynamicTarget::Angle);
  // Straight through, unscaled by any dynamic: a sampled tip's PIXELS are not
  // a thing SIZE/ROUNDNESS/ANGLE dynamics resolve against per dab -- those
  // three still apply, through `tip.radius`/`tip.roundness`/`tip.angle` above,
  // exactly as they do for the procedural tip (brush/Deposit.hpp §2c). What
  // would NOT make sense is a `DynamicTarget` that swaps which bitmap is
  // loaded mid-stroke, and there is no such target for the same reason there
  // is no `DynamicTarget::Pigment`.
  tip.bitmap = brush.tipBitmap;
  // Straight through, unscaled by any dynamic, for the identical reason
  // `tip.bitmap` above is: a Dual Brush's second tip (brush/Deposit.hpp §2d)
  // is not a thing SIZE/ROUNDNESS/ANGLE dynamics resolve against per dab --
  // those three still apply to the PRIMARY tip through `tip.radius`/
  // `tip.roundness`/`tip.angle` above, and (§2d) the nested tip inherits none
  // of them, exactly as it inherited none of them from the descriptor either.
  tip.dualTip = brush.dualTip;
  tip.dualBlend = brush.dualBlend;
  // `BrushState::load` is "pigment concentration" and ranges 0..2.5; a tip's
  // `flow` is "mass laid down per dab where coverage is 1" and is deliberately
  // not clamped to [0,1] (brush/Deposit.hpp: "a flow above 1 is a legitimate
  // one dab saturates the paper tip"). So this is the same number, scaled by
  // pressure, and not a remapping that would make the LOAD slider mean two
  // things.
  //
  // **`Concentration` is a SECOND Multiply column onto this same product**,
  // not a second field -- brush/Dynamics.hpp's own comment on the target says
  // so ("scales BrushState::load"). Composing it here, multiplied alongside
  // `flowMul` rather than folded into a combined dynamic target, is exactly
  // what `--selftest`'s "a Multiply target COMPOSES its sources" section
  // already proves of two links on ONE target -- Flow and Concentration are
  // simply two different cells the matrix lets a user drive independently,
  // and both were always meant to reach the one number a dab actually lays
  // down.
  tip.flow = brush.load * flowMul * dyn.at(DynamicTarget::Concentration);
  tip.spacing = brush.spacing * dyn.at(DynamicTarget::Spacing);
  // `DynamicTarget::Scatter`'s resolved magnitude (an Add target, identity
  // 0.0) -- see `BrushTip::scatter`'s own comment for why this is a magnitude
  // and not yet a position. Undriven, every existing brush gets exactly 0.0
  // here, which is the identity `applyPerDabScatter()` (below, in the deposit
  // loop) treats as "no offset" and skips outright.
  tip.scatter = dyn.at(DynamicTarget::Scatter);
  tip.scatterBothAxes = brush.scatterBothAxes;
  // Straight through, unscaled: there is no `DynamicTarget::Opacity` in
  // brush/Dynamics' twelve, and inventing one here rather than in the matrix
  // that draws them would give the DYNAMICS panel a target it cannot show. The
  // clamp to a legal alpha is `RgbStroke::begin()`'s, at the point of use.
  tip.opacity = brush.opacity;
  // Straight through, unscaled: no `DynamicTarget::Grain` exists, for
  // `tip.opacity`'s own reason immediately above.
  tip.grain = brush.grain;

  // **The foreground, not `defaultPalette()[brush.pigment]`.** This used to
  // read the palette row directly, which was the same thing right up until
  // `BrushState` gained a second way to say a colour -- and it is the line
  // that decides whether an eyedropper pick can paint at all. Header §7.
  const std::array<float, 3> fg = foregroundSrgb(brush);

  // **HUE, SATURATION and VALUE shift the foreground itself, before either
  // decode below** -- brush/Dynamics.hpp's own section comment is the whole
  // argument for doing this in sRGB, at this exact point in the pipeline, and
  // for what it deliberately leaves alone (the palette row's density, staining
  // and granulation, which `drawLinkEditor()` says plainly on these three cells
  // rather than only in a header comment).
  //
  // **It shifts `fg`, not `pigment.rgb`, and that is a merge decision worth
  // recording.** The dynamics work was written against a worktree where the
  // foreground was always the palette row, so it shifted `pigment.rgb` and said
  // so: "a known, stated migration point once colorMode/.rgb actually lands."
  // It has landed -- `fg` above is `foregroundSrgb(brush)`, which is the picked
  // colour in RGB mode and the palette swatch in PIGMENT mode. Shifting the
  // palette row here instead would mean a Hue link silently did nothing to an
  // eyedropper-picked colour, which is the same class of half-wired path both
  // tracks existed to remove.
  //
  // At the identity -- every brush with no Hue/Saturation/Value link, which
  // today is every shipped preset -- `applyHsvDynamics()`'s own short-circuit
  // hands back its input bit-identical, so this line changes nothing about any
  // existing stroke.
  const std::array<float, 3> shiftedRgb =
      applyHsvDynamics(fg, dyn.at(DynamicTarget::Hue), dyn.at(DynamicTarget::Saturation),
                       dyn.at(DynamicTarget::Value));

  // **The same colour, decoded, for the other layer kind** (brush/Deposit.hpp's
  // `linearRgb`). Derived here, from the same `shiftedRgb` the latent below is
  // derived from, so the two representations of one load are produced by one
  // statement pair and cannot name different colours.
  //
  // The foreground is display-referred sRGB and a document part is
  // scene-referred linear (DESIGN-imaging.md, PRD B6). This is exactly
  // `ui/MacPaintUI::foregroundLinearRgba()`'s decode, and it is spelled out
  // again rather than called because `app/` must not include `ui/` -- the
  // dependency runs the other way. `--selftest` asserts the two agree, which is
  // the guard that spelling it twice needs.
  tip.linearRgb = {srgbDecode(shiftedRgb[0]), srgbDecode(shiftedRgb[1]),
                   srgbDecode(shiftedRgb[2])};

  if (lut.valid())
    tip.pigment = lut.rgbToLatent(shiftedRgb[0], shiftedRgb[1], shiftedRgb[2]);
  else
    // No LUT: `Latent::c` is Mixbox's own three weights and the fourth
    // (white) is derived, so a straight copy of the sRGB triple is not the
    // right latent -- but it is a colour in the right family, and a build
    // that never loaded the 512x512 PNG painting *something* beats one that
    // paints white. The LUT is loaded by main.cpp before any UI exists, so
    // this branch is for tests and for a broken install.
    tip.pigment.c = {shiftedRgb[0], shiftedRgb[1], shiftedRgb[2]};
  return tip;
}

const Pigment& foregroundPhysicalConstants(const BrushState& brush) noexcept {
  const std::vector<Pigment>& palette = defaultPalette();
  // **palette[0] for a bad index here, and BLACK for a bad index in
  // `foregroundSrgb()` below.** The two disagreeing is deliberate, not an
  // oversight: the solver has to have some physically plausible paint to run
  // with, and one real row's constants are as good as any invented ones,
  // whereas a *colour* has an unambiguous "nothing" value, and returning it
  // makes a bad index visible rather than plausible. `--selftest` pins both.
  const size_t index =
      brush.pigment >= 0 && static_cast<size_t>(brush.pigment) < palette.size()
          ? static_cast<size_t>(brush.pigment)
          : 0;
  return palette[index];
}

std::array<float, 3> foregroundSrgb(const BrushState& brush) noexcept {
  if (brush.colorMode == ColorMode::Rgb) return brush.rgb;
  const std::vector<Pigment>& palette = defaultPalette();
  // **Out of range falls back to entry 0, NOT to black**, and the difference
  // from `ui/MacPaintUI`'s `foregroundLinearRgba(int)` -- which answers black,
  // and is asserted to (app/selftest/SelectionTools.cpp) -- is deliberate.
  // They are two different questions, exactly as that function's own header
  // says: the index form is the *palette* question ("what colour is row N"),
  // and a row that does not exist has no colour, so black. This is the
  // *foreground* question ("what paint does the next stroke lay down"), and the
  // answer has to be paint. `brushTipFor()` has clamped to entry 0 since long
  // before there was a second way to say a colour, app/selftest/ActiveLayer.cpp
  // asserts it, and routing this through the palette form's contract instead
  // silently turned a bad index into a black stroke.
  //
  // An out-of-range index is an invariant violation either way -- `pigment` is
  // a plain `int` on a public aggregate, so nothing stops one. The choice is
  // only about which wrong answer is least destructive to a painting, and
  // black is the one a user would have to undo.
  if (brush.pigment < 0 || static_cast<size_t>(brush.pigment) >= palette.size()) {
    if (palette.empty()) return {0.0f, 0.0f, 0.0f};
    return {palette[0].rgb[0], palette[0].rgb[1], palette[0].rgb[2]};
  }
  const Pigment& p = palette[static_cast<size_t>(brush.pigment)];
  return {p.rgb[0], p.rgb[1], p.rgb[2]};
}

const char* foregroundName(const BrushState& brush) noexcept {
  if (brush.colorMode == ColorMode::Rgb) return "Custom RGB";
  const std::vector<Pigment>& palette = defaultPalette();
  if (brush.pigment < 0 || static_cast<size_t>(brush.pigment) >= palette.size())
    return "(no pigment)";
  return palette[static_cast<size_t>(brush.pigment)].name;
}

DynamicInputs dynamicInputsFor(const AppState& st) noexcept {
  DynamicInputs in;
  in.pressure = st.penSeen ? st.penPressure : 1.0f;
  in.tilt = st.penTilt;
  in.azimuth = st.penAzimuth;
  in.barrel = st.penBarrel;
  return in;
}

void applyPresetToBrush(const BrushPreset& preset, BrushState& brush) {
  brush.radius = preset.radius;
  brush.hardness = preset.hardness;
  brush.spacing = preset.spacing;
  brush.roundness = preset.roundness;
  brush.angle = preset.angle;
  brush.load = preset.load;
  brush.wetness = preset.wetness;
  brush.links = preset.links;
  brush.tipBitmap = preset.tipBitmap;
  // Carried with the bitmap, never separately: an id naming a tip the brush is
  // not using would be written to `user-presets.txt` on the next Save and
  // would resolve, next launch, to a brush nobody made.
  brush.dabId = preset.dabId;
  brush.dualTip = preset.dualTip;
  brush.dualBlend = preset.dualBlend;
  brush.scatterBothAxes = preset.scatterBothAxes;
  brush.grain = preset.grain;
}

BrushPreset presetFromBrush(std::string name, const BrushState& brush) {
  BrushPreset p;
  p.name = std::move(name);
  p.radius = brush.radius;
  p.hardness = brush.hardness;
  p.spacing = brush.spacing;
  p.roundness = brush.roundness;
  p.angle = brush.angle;
  p.load = brush.load;
  p.wetness = brush.wetness;
  p.links = brush.links;
  p.tipBitmap = brush.tipBitmap;
  p.dabId = brush.dabId;
  p.dualTip = brush.dualTip;
  p.dualBlend = brush.dualBlend;
  p.scatterBothAxes = brush.scatterBothAxes;
  p.grain = brush.grain;
  return p;
}

bool brushIsEdited(const BrushState& brush) {
  if (brush.brushLibrary.active >= brush.brushLibrary.presets.size()) return false;
  const BrushPreset& p = brush.brushLibrary.presets[brush.brushLibrary.active];
  return !presetMatches(p, brush.radius, brush.hardness, brush.spacing, brush.roundness,
                        brush.angle, brush.load, brush.wetness, brush.links, brush.grain);
}

bool StrokeSession::begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
                          std::string* errorOut, const BrushLinkSet* strokeLocalLinks) {
  if (errorOut != nullptr) errorOut->clear();
  const auto refuse = [&](std::string why) {
    if (errorOut != nullptr) *errorOut = std::move(why);
    return false;
  };

  if (layerIndex >= doc.document.layers.size())
    return refuse("stroke refused: layer " + std::to_string(layerIndex) +
                  " is out of range for a document with " +
                  std::to_string(doc.document.layers.size()) + " layers.");

  Layer& layer = doc.document.layers[layerIndex];
  // The route function is the single table (header §1); this refusal reads it
  // rather than re-deriving the same conditions, so the two cannot disagree
  // about which strokes a locked or non-Pigment layer accepts.
  const StrokeRoute route = strokeRouteFor(tool, &layer);
  if (!strokeRouteWritesLayer(route))
    return refuse(std::string("stroke refused: the ") + strokeEditLabel(tool) + " on layer " +
                  std::to_string(layerIndex) + " ('" + layer.name + "', " +
                  layerKindName(layer.kind) + (layer.locked ? ", locked" : "") +
                  (layer.alphaLocked ? ", alpha-locked" : "") + ") routes to " +
                  strokeRouteName(route) + ", which does not write a layer.");

  doc_ = &doc;
  layerIndex_ = layerIndex;
  layerCount_ = doc.document.layers.size();
  tip_ = tip;
  route_ = route;
  tool_ = tool;
  label_ = strokeEditLabel(tool);

  // The ink, latched for the whole stroke -- brush/RgbDeposit.hpp §2 on why the
  // colour and the ceiling may not move once the accumulator has started, and
  // §3 on the accumulator being allocated here and freed at `end()`.
  //
  // The `else` is not redundant. A pigment or erase stroke that follows an RGB
  // deposit must leave nothing of it behind, for exactly `StrokePath::reset()`'s
  // reason below: alpha carried across strokes would let the ceiling of the last
  // stroke cap the first dab of the next -- and a session begun without a
  // matching `end()` (a window blur, an interrupted drag) is the case that
  // reaches this line holding tiles.
  // `layer.alphaLocked`, latched with the ink and the ceiling for the identical
  // reason (brush/RgbDeposit.hpp's `begin()`): a lock cleared or set mid-drag
  // must not change which composite the dabs already spent are read back
  // through. Read here rather than inside `rgb_` because `Layer` is what this
  // file already has in hand and `brush/RgbDeposit` deliberately knows nothing
  // about one (its header §5, "No Document, no Layer").
  if (route_ == StrokeRoute::RgbDeposit)
    rgb_.begin(tip.linearRgb, tip.opacity, layer.alphaLocked);
  else
    rgb_.end();

  // The strength, latched with it and for the identical reason
  // (brush/RgbErase.hpp §2): a stroke whose floor moved half way through has no
  // well-defined floor. `tip.opacity` is the same slider the deposit route reads
  // as its ceiling -- one control, one meaning, "the fraction of the maximum
  // effect one stroke may reach" -- and `tip.linearRgb` is deliberately NOT
  // read, because an eraser that had a colour would be a brush painting the
  // background (ADR-0007's rejected model).
  //
  // The `else` carries the same weight as the one above, one direction further:
  // erasure carried across strokes would let the floor of the last stroke stop
  // the first dab of the next, so a second pass would refuse to cut deeper.
  if (route_ == StrokeRoute::RgbErase)
    erase_.begin(tip.opacity);
  else
    erase_.end();

  // The Pigment erase route's own accumulator, latched from the same slider for
  // the same reason (brush/PigmentErase.hpp §2). Three `begin()`/`end()` pairs
  // rather than one switch because each one's `else` is the load-bearing half:
  // whichever route this stroke took, the other two must be left holding no
  // tiles, and an interrupted drag is exactly the case that reaches here with
  // one of them still live.
  if (route_ == StrokeRoute::PigmentErase)
    pigErase_.begin(tip.opacity);
  else
    pigErase_.end();

  // Leftover arc length and point history from whatever stroke happened
  // before must not bleed into this one -- brush/StrokePath::reset()'s own
  // contract, and the same call ui/MacPaintUI already makes at pen-down.
  path_.reset();
  pending_.clear();
  frameTiles_.clear();
  strokeTiles_.clear();
  dabs_ = 0;
  texels_ = 0;

  // VELOCITY/FADE/NOISE/RANDOM/DIRECTION/INITIAL-DIRECTION's own state, for
  // the identical reason: a second stroke that inherited the first's seed,
  // previous position, travelled distance or LATCHED heading would draw
  // correlated noise, measure velocity, read a heading and lock its initial
  // angle against a point on a different path entirely.
  strokeLocalLinks_ = strokeLocalLinks;
  seed_ = 0;
  seedLatched_ = false;
  prevDabX_ = 0.0f;
  prevDabY_ = 0.0f;
  havePrevDab_ = false;
  distanceTravelled_ = 0.0f;
  initialDirection_ = 0.0f;
  initialDirectionLatched_ = false;
  // `smoothPressure()`'s own per-stroke state -- see its header comment on
  // why leaking the previous stroke's last smoothed pressure into this one
  // would corrupt this stroke's very first dab.
  smoothedPressure_ = 0.0f;
  pressureSmoothLatched_ = false;
  return true;
}

float StrokeSession::smoothPressure(float rawPressure) noexcept {
  if (!pressureSmoothLatched_) {
    smoothedPressure_ = rawPressure;  // no history yet -- start AT the
                                      // input rather than ramping up from
                                      // 0 (this method's own header comment)
    pressureSmoothLatched_ = true;
  } else {
    smoothedPressure_ = dynamicPressureEma(smoothedPressure_, rawPressure);
  }
  return smoothedPressure_;
}

void StrokeSession::depositPending() {
  frameTiles_.clear();
  if (pending_.empty()) return;

  Document& doc = doc_->document;
  // The target is re-validated on **every** frame, not just at pen-down: the
  // document is a public aggregate, and a stroke that outlived its layer would
  // otherwise be a dangling write -- or, worse, would deposit the rest of
  // itself into whatever slid into that index. Both are the same class of
  // silent, invisible mistake the header refuses for a locked target.
  //
  // The check is the layer *count* plus the layer's own kind, store and lock,
  // and the header says what that does and does not cover.
  if (doc.layers.size() != layerCount_ || layerIndex_ >= doc.layers.size()) {
    pending_.clear();
    return;
  }
  Layer& layer = doc.layers[layerIndex_];
  // Against the route latched at pen-down, not merely against "some deposit
  // route" -- header §5's second paragraph on why a Pigment layer swapped for
  // an RGB one at the same index must end the stroke rather than continue it.
  //
  // **Asked with `tool_`, the session's own tool, and not with a stand-in.**
  // This line used to pass `Tool::Brush` on the argument that Brush and DryBrush
  // agree on every target and nothing else could have begun a session. The
  // eraser retires that argument: Brush and Eraser give two *different* answers
  // about one unchanged RGB layer, so a stand-in would find the route "changed"
  // on the second frame of every erase and silently drop the rest of the drag --
  // a tool that works for one frame and then stops, which is harder to diagnose
  // than one that never works at all.
  if (strokeRouteFor(tool_, &layer) != route_) {
    pending_.clear();
    return;
  }

  // PRD E1 (P0): the active selection bounds every deposit and every erase.
  // Read live rather than latched at pen-down -- nothing can install a selection
  // during a pointer drag, and reading it here means a session that outlived one
  // somehow cannot write through a stale bound.
  //
  // Hoisted out of the call below because **every** route takes it now, so no
  // two of them can end up reading the selection different ways. It used to be
  // hoisted for two of three, with the pigment route taking none at all -- and
  // that gap was PRD E1 unmet on the layer kind `Document::createBlank()`
  // makes: a natural-media stroke painted straight through the marching ants
  // while the RGB branch on the line below it passed this same pointer.
  const Selection* selection = doc_->selection.has_value() ? &*doc_->selection : nullptr;

  // **One dab at a time, calling each route's own SINGULAR `*Dab()` rather
  // than its batched `*Dabs()`.** The four batched functions
  // (`rgb_.depositDabs()`, `erase_.eraseDabs()`, `pigErase_.eraseDabs()`,
  // the free `depositDabs()`) are, every one of them, exactly this loop --
  // `for (dab : dabs) { call the singular form; accumulate; } sortUniqueTiles()`
  // -- with no per-dab hook a caller could use to vary the tip. This loop
  // reproduces that shape by hand so it CAN vary the tip: VELOCITY, FADE,
  // NOISE, RANDOM, DIRECTION and INITIAL DIRECTION (brush/Dynamics.hpp) are
  // stroke-local sources that must be resolved once per dab, not once per
  // frame, and the only place a dab's own index and position exist is here.
  //
  // **This is provably a no-op when `strokeLocalLinks_` is null** -- the
  // default every existing caller of `begin()` still gets. With it null,
  // `dabTip` below is `tip_` unconditionally and `centre` is `p`
  // unconditionally (`applyPerDabScatter()`'s own identity branch: `tip_.
  // scatter` is 0.0 for any brush with nothing linked to SCATTER, which is
  // every shipped preset before this file existed), so this loop calls the
  // IDENTICAL sequence of singular `*Dab()` calls, in the IDENTICAL order,
  // accumulating into the IDENTICAL vector, sorted once at the end exactly as
  // the batched functions do internally -- bit-for-bit the same floating-
  // point operations as the code this replaced. No golden image, no
  // `--pigment-stroke-demo` reference and no existing `--selftest` assertion
  // about a dab's footprint or a stroke's byte-identical undo has anything to
  // notice.
  size_t frameTexels = 0;
  for (const Vec2& p : pending_) {
    // The seed, latched once from the stroke's very FIRST dab position --
    // brush/Dynamics.hpp's own section comment on why position rather than a
    // counter, and why this must happen here rather than at `begin()`, which
    // has no position yet to latch.
    if (!seedLatched_) {
      seed_ = strokeSeedFromStart(p.x, p.y);
      seedLatched_ = true;
    }

    // VELOCITY's step distance -- 0.0 on this stroke's first dab, exactly
    // `dynamicVelocity()`'s documented "no previous position" contract --
    // and FADE/NOISE's running arc length, both measured dab-to-dab rather
    // than sample-to-sample (this file's own member comments on why).
    //
    // DIRECTION's own step vector is the same `(p - prevDab)` difference
    // `stepDist` is the magnitude of, kept as its two signed components
    // rather than collapsed to a distance -- `dynamicDirection()` needs the
    // heading, not the length. Zeroed on the first dab for the identical
    // reason `stepDist` is: `brush/Dynamics.hpp`'s own comment on
    // `dynamicDirection()` is what makes `std::atan2(0, 0)` the documented,
    // not accidental, answer for "no previous position yet".
    const float dx = havePrevDab_ ? p.x - prevDabX_ : 0.0f;
    const float dy = havePrevDab_ ? p.y - prevDabY_ : 0.0f;
    const float stepDist = havePrevDab_ ? std::hypot(dx, dy) : 0.0f;
    distanceTravelled_ += stepDist;

    // INITIAL DIRECTION's own latch -- the `seed_`/`seedLatched_` shape,
    // restated for a resolved VALUE instead of an identity (brush/
    // Dynamics.hpp's own "INITIAL DIRECTION" section is the argument this
    // is the code for). Fires once, on the first dab this stroke has a
    // real `(dx, dy)` for -- one dab LATER than `seed_`'s own latch, since
    // a heading needs a second position and the stroke's very first dab
    // never has one (`havePrevDab_` is false there, exactly the case this
    // guards against re-latching on).
    //
    // Computed unconditionally, like `stepDist` and `distanceTravelled_`
    // just above, so this is provably a no-op when `strokeLocalLinks_` is
    // null for the identical reason those are: nothing below reads
    // `initialDirection_` in that case, and a member write nobody reads
    // cannot change what gets deposited.
    if (havePrevDab_ && !initialDirectionLatched_) {
      initialDirection_ = dynamicDirection(dx, dy);
      initialDirectionLatched_ = true;
    }

    BrushTip dabTip = tip_;
    if (strokeLocalLinks_ != nullptr) {
      DynamicInputs local{};
      local.velocity = dynamicVelocity(stepDist, tip_.radius);
      local.fade = dynamicFade(distanceTravelled_);
      local.noise = dynamicNoiseAt(seed_, distanceTravelled_);
      local.random = dynamicRandomDraw(seed_, static_cast<uint32_t>(dabs_));
      local.direction = dynamicDirection(dx, dy);
      local.initialDirection = initialDirection_;
      const DynamicResult corr =
          evaluateLinksFiltered(*strokeLocalLinks_, local, /*wantStrokeLocal=*/true);
      dabTip = applyStrokeLocalCorrection(tip_, corr);
    }

    // The floor, applied exactly once, HERE -- the one point downstream of
    // BOTH halves of the Multiply product: `brushTipFor()`'s hardware half,
    // baked into `tip_.radius`/`tip_.sizeFloorPx` back at `setTip()` time,
    // and the stroke-local half just folded in above by
    // `applyStrokeLocalCorrection()` when there is one (when there is not,
    // `dabTip` is `tip_` unconditionally, and this is still the correct --
    // and only -- place to floor a product with no second half). See
    // `BrushTip::sizeFloorPx`'s own comment (brush/Deposit.hpp) for the
    // worked counter-example this ordering exists to satisfy. A no-op for
    // every brush with no Minimum Diameter: `sizeFloorPx` is 0.0f there, and
    // `std::max(x, 0.0f)` cannot lower an `x` that `linkContribution()`
    // already never lets go negative.
    dabTip.radius = std::max(dabTip.radius, dabTip.sizeFloorPx);
    lastDabRadius_ = dabTip.radius;

    const Vec2 centre =
        applyPerDabScatter(p, dabTip, seed_, static_cast<uint32_t>(dabs_), dx, dy);

    // The four routes differ in exactly this call, and each takes
    // `selection`. Everything around it -- the tile bookkeeping, the
    // counters, the revision bump and the single history entry -- is shared,
    // because none of it is a property of what a texel is made of or of
    // which direction the stroke moves it.
    const DepositCount c =
        route_ == StrokeRoute::RgbErase
            ? erase_.eraseDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height, selection,
                              &frameTiles_)
        : route_ == StrokeRoute::PigmentErase
            ? pigErase_.eraseDab(*layer.pigmentTiles, dabTip, centre, doc.width, doc.height,
                                 selection, &frameTiles_)
        : route_ == StrokeRoute::RgbDeposit
            ? rgb_.depositDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height, selection,
                              &frameTiles_)
            : depositDab(*layer.pigmentTiles, dabTip, centre, doc.width, doc.height, selection,
                        &frameTiles_);
    frameTexels += c.texels;
    ++dabs_;
    prevDabX_ = p.x;
    prevDabY_ = p.y;
    havePrevDab_ = true;
  }
  sortUniqueTiles(frameTiles_);
  texels_ += frameTexels;

  if (!frameTiles_.empty()) {
    strokeTiles_.insert(strokeTiles_.end(), frameTiles_.begin(), frameTiles_.end());
    sortUniqueTiles(strokeTiles_);
  }
  pending_.clear();
}

const std::vector<TileCoord>& StrokeSession::addPoint(float x, float y) {
  frameTiles_.clear();
  if (doc_ == nullptr) return frameTiles_;

  path_.addPoint(x, y, tip_.spacingPx(), pending_);
  depositPending();

  // Live feedback, header §3: the revision is what invalidates
  // ui/DocumentTexture's cache, and it moves only when pigment actually
  // landed. No history entry -- that is pen-up's job, once.
  if (!frameTiles_.empty()) ++doc_->revision;
  return frameTiles_;
}

const std::vector<TileCoord>& StrokeSession::end() {
  if (doc_ == nullptr) return strokeTiles_;

  path_.flush(tip_.spacingPx(), pending_);
  depositPending();

  OpenDocument* doc = doc_;
  doc_ = nullptr;  // the session is over before the record, so a re-entrant
                   // caller cannot deposit into a half-ended stroke
  // The accumulator's whole life is one stroke (brush/RgbDeposit.hpp §3), and
  // it is dropped here rather than at the next `begin()` so an application
  // sitting idle after a long stroke is not holding 64 KiB per tile it painted.
  // Unconditional: `end()` on an idle accumulator is a no-op, and a branch here
  // would be one more place the routes could disagree about cleanup. All three
  // are dropped, not the one this stroke used -- exactly one of them was ever
  // live, and asking which at cleanup time is how the other two keep their tiles
  // after an interrupted drag.
  rgb_.end();
  erase_.end();
  pigErase_.end();

  // Exactly one entry, and only for a stroke that put something down --
  // header §2.
  if (texels_ > 0) doc->recordEdit(label_, EditKind::Content);
  return strokeTiles_;
}

}  // namespace np
