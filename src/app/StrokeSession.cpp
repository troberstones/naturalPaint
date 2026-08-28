#include "app/StrokeSession.hpp"

#include <algorithm>
#include <cmath>

#include "brush/ToolOptionsBlend.hpp"
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

// Scatter COUNT's own per-SUB-DAB salt (Part 1, `PsScatter::count`) --
// multiplied by `subIndex` and XORed into the seed before
// `applyPerDabScatter()` draws SCATTER's own angle, so each of the N
// sub-dabs stamped at one nominal position gets an INDEPENDENT draw rather
// than all of them landing on the identical offset. Any fixed odd 64-bit
// constant decorrelates the sub-dab stream from the stroke's own seed
// equally well; this one is the ASCII bytes of "subdab01" read as a
// little-endian word, chosen for the same "recognisable in a hex dump"
// reason `kScatterAngleSalt` was.
//
// **Provably zero at `subIndex == 0`** -- `kScatterSubDabSalt *
// static_cast<uint64_t>(0)` is exactly `0` by the definition of unsigned
// multiplication, so the seed this constant is XORed into is bit-identical
// to the seed alone whenever there is only one sub-dab (`resolvedCount ==
// 1`, every existing preset before this feature). That is the structural
// half of `applyPerDabScatter()`'s own no-op proof; `app/selftest/
// ScatterCount.cpp` is the other half, asserted end to end.
constexpr uint64_t kScatterSubDabSalt = 0x3130626164627573ULL;  // "subdab01" (LE)

// Two 2*pi's worth of named precision rather than a bare literal: the
// dynamics matrix's other angular sources (`sourceDisplay()`'s tilt/azimuth/
// barrel cases) all go through `%.0f` degrees, and this is the one place in
// the deposit path that turns a [0,1) draw into radians directly.
constexpr float kTwoPi = 6.28318530717958647692f;
// A quarter turn -- what `applyPerDabScatter()` rotates the stroke's own
// tangent by to find its perpendicular.
constexpr float kHalfPi = kTwoPi * 0.25f;

// The per-dab correction used to be `applyStrokeLocalCorrection()`, folding a
// `strokeLocalLinks_` resolution (VELOCITY/FADE/NOISE/RANDOM/DIRECTION/
// INITIAL-DIRECTION against `BrushLinkSet`) onto a tip already resolved
// against the four hardware sources. It is gone: Size, Angle, Roundness and
// Scatter are now resolved ENTIRELY inside `depositPending()`'s own per-dab
// loop below, in one `varianceScale()`/`varianceOffset()` call each per dab
// per site (`brush/Variance.hpp`'s own load-bearing invariant -- its floor is
// applied once, inside the formula, so a stroke-begin base times a second
// per-dab correction would apply it twice). There is no longer a base value
// to correct.

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
                        float stepDx, float stepDy, uint32_t subIndex) noexcept {
  if (tip.scatter == 0.0f) return centre;  // the identity: no branch taken,
                                           // no draw spent, for every brush
                                           // with nothing linked to SCATTER
  // Scatter COUNT's own sub-dab salt (Part 1) -- exactly `0` when
  // `subIndex == 0` (unsigned multiplication by zero), so the seed the draw
  // below reads from is bit-identical to `seed` alone for the single-sub-dab
  // case, which is every existing caller and every preset with `Cnt ` == 1.
  const uint64_t subSeed = seed ^ (kScatterSubDabSalt * static_cast<uint64_t>(subIndex));
  const float draw = dynamicRandomDraw(subSeed ^ kScatterAngleSalt, dabIndex);
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
  // **The old `dyn`/`evaluateLinksFiltered(brush.links, ...)` resolution is
  // gone.** The 10x12 link matrix (`brush/Dynamics.hpp`) is shelved
  // (`ui/DynamicsMatrixPanel.hpp`) -- nothing that paints reads
  // `BrushState::links` any more. What drives a stroke now is
  // `brush/BrushModel`/`brush/Variance`, Photoshop's own shape, decoded
  // straight off the `.abr` file (or authored by hand for the four built-ins,
  // `brush/Library.cpp`'s `defaultBrushLibrary()`).
  const BrushModel& model = brush.model;
  (void)inputs;  // the four HARDWARE sources reach a stroke through
                 // `StrokeSession::begin()`/`setTip()`'s own `hardwareInputs`
                 // parameter now, latched alongside the tip rather than
                 // resolved into it here -- see that header's own comment.

  BrushTip tip;
  // **Size, Angle and Roundness are BASE values only -- unvaried.** Their
  // Variance objects (`PsShapeDynamics::size`/`angle`/`roundness`) are
  // resolved ENTIRELY inside `StrokeSession`'s per-dab loop
  // (`depositPending()`), in one `varianceScale()`/`varianceOffset()` call
  // each per dab, never here and never split into a base-here/correction-there
  // pair -- `brush/Variance.hpp`'s own header is the whole argument: its
  // floor is applied exactly once, inside the formula, so a second partial
  // resolution composed onto this base would apply it twice and reintroduce
  // audit B6 in a new shape.
  //
  // A caller that reads this tip WITHOUT going through `StrokeSession` --
  // `app/DabPreview.cpp`'s row-icon preview is the one that matters -- sees
  // this unvaried base and nothing else: a narrower preview than the one a
  // real stroke paints, in the exact same "a preview cell is not a stroke"
  // sense that file's own comment already uses for the (now-removed)
  // `sizeFloorPx`. Stated here rather than left to be discovered.
  tip.radius = model.tip.diameterPx * 0.5f;
  tip.angle = model.tip.angleDeg;
  tip.roundness = model.tip.roundness;
  // The Add-target identity (`BrushTip::scatter`'s own comment) -- the real
  // per-dab magnitude comes from `model.scatter.scatter` inside the per-dab
  // loop, the identical split Size/Angle/Roundness get above.
  tip.scatter = 0.0f;

  // Straight passthrough: `PsTipShape` carries no Variance for hardness, so
  // there is nothing to resolve per dab -- Photoshop's own panel has no
  // Hardness jitter control either.
  tip.hardness = model.tip.hardness;
  // `Spcn` is a percentage OF THE DIAMETER (brush/BrushModel.hpp's own
  // comment on `PsTipShape::spacingPercent`), but `BrushTip::spacing` is in
  // RADII (its own comment, brush/Deposit.hpp: `spacingPx() { return spacing
  // * radius; }`) -- a diameter is two radii, so a given fraction OF THE
  // DIAMETER is DOUBLE that same fraction of the radius. `/100` alone would
  // silently halve every imported brush's dab spacing; `io/AbrBrushes.cpp`'s
  // own `abrSpacingToRadii()` names this identical conversion
  // (`percentOfDiameter / 100 * 2`) for the sibling import path that fills a
  // `BrushTip` directly.
  tip.spacing = model.tip.spacingPercent / 100.0f * 2.0f;

  // **`tipBitmap`/`dualTip`/`dualBlend` stay `BrushState`'s own fields, not
  // `model.tip.dab.bitmap`/`model.dual`.** Both name the same imported data,
  // but `BrushState`'s copies are the ones actually resolved at runtime
  // (`app/DabLibrary`'s id -> bitmap lookup, `applyPresetToBrush()`'s
  // lockstep copy) and are not among the five scalars this migration deletes
  // -- reading them here is the same zero-risk passthrough the old code
  // already did, rather than a second, unproven resolution path through the
  // model's own `DabRef`.
  tip.bitmap = brush.tipBitmap;
  tip.dualTip = brush.dualTip;
  tip.dualBlend = brush.dualBlend;
  tip.scatterBothAxes = model.scatter.bothAxes;

  // **Unscaled HERE by anything Photoshop calls Transfer -- not because it
  // stays unscaled, but because this is not where the scaling happens.** The
  // old code multiplied `brush.load` by two matrix columns (`Flow`,
  // `Concentration`); both are retired with the matrix. `PsTransfer::opacity`/
  // `.flow` (`opVr`/`prVr`) are now wired -- Part 2 of this phase -- but at
  // `StrokeSession::begin()`, not here: Opacity is a per-STROKE ceiling that
  // must be resolved once and latched into the RGB/erase accumulators'
  // OWN members, and Flow's resolved multiplier has to survive `setTip()`
  // rebuilding this very tip from a fresh call to this function every frame
  // -- neither of which this function, called from both `begin()` and
  // `setTip()` with no memory of which, can do on its own. See `begin()`'s
  // own comment for the full argument. So this is `brush.load`/
  // `brush.opacity` alone, same as it always was for a brush with no Flow/
  // Concentration link -- the base value Transfer's resolved multiplier
  // scales, not the resolved value itself.
  //
  // Scatter Count (`PsScatter::count`/`countJitter`) is wired too, in
  // `app/StrokeSession.cpp`'s `depositPending()` -- a per-DAB resolution, so
  // it belongs beside Size/Angle/Roundness/Scatter there rather than here.
  tip.flow = brush.load;
  // Straight through, unscaled: there is no per-dab Grain dynamic in either
  // the matrix or the model.
  tip.opacity = brush.opacity;
  tip.grain = brush.grain;

  // The `Md ` per-stroke blend mode (Part 3, `PsToolOptions::blendMode`),
  // mapped once at the edge. Unlike Transfer and Scatter Count, this is
  // groundwork only: `blend` is not read by any of the four deposit routes
  // yet (see `BrushTip::blend`'s own comment, brush/Deposit.hpp, for the two
  // obstacles a bounded investigation found). Falls back to `Normal` on
  // refusal -- the same value `BrushTip::blend`'s own default member
  // initializer already gives, so ignoring the `false` return here is
  // exactly today's implicit behaviour for every brush this build has ever
  // painted, ".abr"-imported or not.
  blendModeFromPsToolOptions(model.options.blendMode, tip.blend);

  // **The foreground, not `defaultPalette()[brush.pigment]`.** This used to
  // read the palette row directly, which was the same thing right up until
  // `BrushState` gained a second way to say a colour -- and it is the line
  // that decides whether an eyedropper pick can paint at all. Header §7.
  const std::array<float, 3> fg = foregroundSrgb(brush);

  // **HUE, SATURATION and VALUE are identity here, deliberately, and this is
  // a named scope boundary rather than a silent drop.** `PsColorDynamics`
  // exists in the model (`brush/BrushModel.hpp`'s own struct) but is on for
  // only 1 of the 101 presets measured, and how it composes with the
  // foreground the matrix used to shift is not settled -- wiring it in is
  // future work, not this commit's. So this stops consulting the (now
  // shelved) matrix for Hue/Saturation/Value and passes the identity
  // `(0, 1, 1)` instead -- hue's identity is the additive 0.0f turns, but
  // saturation and value are MULTIPLIERS, so their identity is 1.0f, not
  // 0.0f (`applyHsvDynamics()`'s own short-circuit checks exactly this
  // triple). This is what hands the result back bit-identical to `fg` --
  // i.e. every stroke's foreground reaches the canvas exactly as picked,
  // same as a brush with no Hue/Saturation/Value link already painted.
  const std::array<float, 3> shiftedRgb = applyHsvDynamics(fg, 0.0f, 1.0f, 1.0f);

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
  // Radius/hardness/spacing/roundness/angle used to be five explicit copies
  // here -- gone along with the fields themselves (brush/Library.hpp's own
  // comment); `brush.model = preset.model` below already carries all five,
  // in lockstep, exactly as `BrushPreset::model`'s own comment always said
  // it would once something read the model to paint.
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
  // Carried in lockstep with everything above, for the reason
  // `BrushState::model`'s own comment gives: this is the one direction that,
  // until now, had somewhere to write a model FROM (`BrushPreset::model`) but
  // nowhere to write it TO -- fixing only this half without the mirror below
  // would make picking a preset look like it worked while Duplicate still
  // dropped everything the model carries.
  brush.model = preset.model;
}

BrushPreset presetFromBrush(std::string name, const BrushState& brush) {
  BrushPreset p;
  p.name = std::move(name);
  // The mirror of `applyPresetToBrush()`'s own removed five-scalar copy --
  // `p.model = brush.model` below carries all five now.
  p.load = brush.load;
  p.wetness = brush.wetness;
  p.links = brush.links;
  p.tipBitmap = brush.tipBitmap;
  p.dabId = brush.dabId;
  p.dualTip = brush.dualTip;
  p.dualBlend = brush.dualBlend;
  p.scatterBothAxes = brush.scatterBothAxes;
  p.grain = brush.grain;
  // The other half of the lockstep above. This is the direction that used to
  // not exist at all -- `BrushState` had no `model` field to read -- which is
  // the exact mechanism of the defect `BrushPreset::model`'s comment
  // describes: Duplicate on an imported brush produced a preset whose model
  // was default-constructed, silently discarding the texture, transfer,
  // Dual Brush cadence and blend mode the import decoded.
  p.model = brush.model;
  return p;
}

bool brushIsEdited(const BrushState& brush) {
  if (brush.brushLibrary.active >= brush.brushLibrary.presets.size()) return false;
  const BrushPreset& p = brush.brushLibrary.presets[brush.brushLibrary.active];
  // The five scalars `presetMatches()` still takes as parameters (its own
  // signature is unchanged -- only where a caller reads them from moved) now
  // come from `brush.model` rather than from five deleted `BrushState`
  // fields.
  return !presetMatches(p, brush.model.tip.diameterPx / 2.0f, brush.model.tip.hardness,
                        brush.model.tip.spacingPercent / 100.0f, brush.model.tip.roundness,
                        brush.model.tip.angleDeg, brush.load, brush.wetness, brush.links,
                        brush.grain);
}

bool StrokeSession::begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
                          std::string* errorOut, const BrushModel* model,
                          const DynamicInputs& hardwareInputs) {
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

  // Transfer Opacity/Flow (Part 2, `PsTransfer::opacity`/`.flow`), resolved
  // HERE -- before any of the three `*_.begin()` calls below read
  // `tip.opacity`, and before this function's own `if (haveModel_)` block
  // further down that copies the rest of the model's Variance objects. That
  // block runs AFTER these three calls (it always has -- see its own
  // comment), and Opacity is a per-STROKE ceiling that gets latched into
  // `rgb_`/`erase_`/`pigErase_`'s own members at exactly this point
  // (brush/RgbDeposit.hpp §2, brush/RgbErase.hpp §2, brush/PigmentErase.hpp
  // §2) -- there is no second chance to apply it once those calls have run.
  // Moving the whole model-copying block earlier was the other option this
  // task's own brief named; resolving inline here is the smaller change,
  // because nothing else in that block needs to run before those three
  // calls, only this.
  //
  // Flow has no single latch point the way Opacity does -- `BrushTip::flow`
  // is read fresh out of `tip_` every dab, by whichever route is running, so
  // there is nothing to bake the resolved value INTO here that would survive
  // `setTip()` rebuilding `tip_` from a fresh (Transfer-unaware)
  // `brushTipFor()` call on the stroke's very next frame. Its resolution
  // therefore lives in `transferFlowMul_` (a per-stroke CONSTANT, computed
  // once here) and is applied to `tip_.flow` fresh every dab in
  // `depositPending()` -- see that loop's own comment for the full argument.
  //
  // **No real stroke position exists yet here.** `begin()`'s own signature
  // has no x/y -- the identical reason `seed_` below is latched from the
  // stroke's first DAB position rather than here. So a Control-driven
  // Opacity/Flow Jitter (a brush whose `opVr`/`prVr` reads PenPressure, say)
  // sees `hardwareInputs` -- this frame's hardware sample, the only one
  // available -- but the JITTER component draws from a FIXED placeholder
  // seed (`0`) and a fixed dab index (`0`), never a real per-stroke random
  // draw. This is a PRE-EXISTING limitation of resolving anything at
  // `begin()`-time (nothing latched here has ever had real randomness to
  // draw from -- the ink and the erase/deposit ceiling already had exactly
  // this limitation before Transfer existed), not a new gap Transfer
  // introduces. Named plainly rather than left to be discovered, this
  // codebase's standing rule for a divergence.
  const bool haveTransferModel = model != nullptr;
  // The fixed placeholder this comment names. `0` rather than something
  // derived from `layerIndex`/`tool`/anything else in scope: those are not
  // positions either, and a seed built from them would look like real
  // per-stroke variation without being any.
  constexpr uint64_t kTransferSeed = 0;
  const float resolvedOpacity =
      haveTransferModel
          ? varianceScale(model->transfer.opacity, hardwareInputs, kTransferSeed, 0,
                          VarianceSite::Opacity) *
                tip.opacity
          : tip.opacity;
  transferFlowMul_ = haveTransferModel
                         ? varianceScale(model->transfer.flow, hardwareInputs, kTransferSeed, 0,
                                        VarianceSite::Flow)
                         : 1.0f;

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
    rgb_.begin(tip.linearRgb, resolvedOpacity, layer.alphaLocked);
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
    erase_.begin(resolvedOpacity);
  else
    erase_.end();

  // The Pigment erase route's own accumulator, latched from the same slider for
  // the same reason (brush/PigmentErase.hpp §2). Three `begin()`/`end()` pairs
  // rather than one switch because each one's `else` is the load-bearing half:
  // whichever route this stroke took, the other two must be left holding no
  // tiles, and an interrupted drag is exactly the case that reaches here with
  // one of them still live.
  if (route_ == StrokeRoute::PigmentErase)
    pigErase_.begin(resolvedOpacity);
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
  //
  // The model's own base values and Variance objects, copied out rather than
  // kept as a pointer -- see `StrokeSession.hpp`'s own member comment for why.
  // `haveModel_` false leaves every dab reading `tip_` unmodified, same as a
  // null `model` here always has.
  haveModel_ = model != nullptr;
  if (haveModel_) {
    baseDiameterPx_ = model->tip.diameterPx;
    baseAngleDeg_ = model->tip.angleDeg;
    baseRoundness_ = model->tip.roundness;
    sizeVariance_ = model->shape.size;
    angleVariance_ = model->shape.angle;
    roundnessVariance_ = model->shape.roundness;
    scatterVariance_ = model->scatter.scatter;
    // Scatter COUNT (Part 1) -- copied out alongside the five above, for the
    // identical reason (`StrokeSession.hpp`'s own member comment: nothing
    // guarantees the `BrushState`/`BrushPreset` `begin()` was called with
    // outlives the stroke).
    baseCount_ = model->scatter.count;
    countVariance_ = model->scatter.countJitter;
  }
  hardwareInputs_ = hardwareInputs;
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

    // **Size, Angle, Roundness and Scatter are resolved HERE, per dab, in
    // exactly one `varianceScale()`/`varianceOffset()` call each -- never a
    // stroke-begin base multiplied/added to by a second per-dab correction.**
    // This is `brush/Variance.hpp`'s own load-bearing invariant: a `Variance`
    // object's floor is applied ONCE, inside its formula, so composing two
    // partial resolutions the way the old `applyStrokeLocalCorrection()` did
    // for the matrix would apply that floor twice and reintroduce audit B6 in
    // a new shape. `dabTip` therefore does not start from `tip_.radius`/
    // `.angle`/`.roundness`/`.scatter` and correct them -- it REPLACES them
    // outright, from `baseDiameterPx_`/`baseAngleDeg_`/`baseRoundness_` and
    // this dab's own resolution.
    BrushTip dabTip = tip_;
    // Transfer FLOW (Part 2): `transferFlowMul_` is a per-STROKE constant,
    // resolved once at `begin()` -- see that function's own comment on why
    // it lives here as a multiplier applied fresh every dab, rather than
    // baked once into `tip_.flow` there. Identity (`* 1.0f`, bit-exact for
    // every finite float) whenever there is no model or the model's
    // Transfer Flow Variance is inert, which is what keeps this a no-op for
    // every brush this codebase already paints with.
    dabTip.flow = tip_.flow * transferFlowMul_;

    // **Size, Angle, Roundness and Scatter are resolved HERE, per dab, in
    // exactly one `varianceScale()`/`varianceOffset()` call each -- never a
    // stroke-begin base multiplied/added to by a second per-dab correction.**
    // This is `brush/Variance.hpp`'s own load-bearing invariant: a `Variance`
    // object's floor is applied ONCE, inside its formula, so composing two
    // partial resolutions the way the old `applyStrokeLocalCorrection()` did
    // for the matrix would apply that floor twice and reintroduce audit B6 in
    // a new shape. `dabTip` therefore does not start from `tip_.radius`/
    // `.angle`/`.roundness`/`.scatter` and correct them -- it REPLACES them
    // outright, from `baseDiameterPx_`/`baseAngleDeg_`/`baseRoundness_` and
    // this dab's own resolution.
    //
    // Scatter COUNT (Part 1) resolves alongside them below. `resolvedCount`
    // starts at `dabTip.count` -- `BrushTip::count`'s own identity, 1, when
    // there is no model -- exactly the identity every field above already
    // takes in that case.
    int32_t resolvedCount = dabTip.count;
    if (haveModel_) {
      // The six stroke-local signals, fresh every dab -- unchanged from the
      // old `local` this replaces, since Variance needs the identical inputs
      // the matrix did for VELOCITY/FADE/NOISE/RANDOM/DIRECTION/INITIAL
      // DIRECTION. Seeded from `hardwareInputs_` first so Pressure/Tilt/
      // Azimuth/Barrel (and their `has*` flags) reach a PenPressure/PenTilt/
      // Rotation Control -- at the FRAME granularity `begin()`/`setTip()`
      // latched them at, not resampled per dab (this codebase's own standing
      // rule; `dynamicInputsFor()`'s header is the argument for it).
      DynamicInputs local = hardwareInputs_;
      local.velocity = dynamicVelocity(stepDist, tip_.radius);
      local.fade = dynamicFade(distanceTravelled_);
      local.noise = dynamicNoiseAt(seed_, distanceTravelled_);
      local.random = dynamicRandomDraw(seed_, static_cast<uint32_t>(dabs_));
      local.direction = dynamicDirection(dx, dy);
      local.initialDirection = initialDirection_;

      const auto dabIndex = static_cast<uint32_t>(dabs_);
      dabTip.radius = (baseDiameterPx_ * 0.5f) *
                      varianceScale(sizeVariance_, local, seed_, dabIndex, VarianceSite::Size);
      dabTip.angle =
          baseAngleDeg_ +
          varianceOffset(angleVariance_, local, 180.0f, seed_, dabIndex, VarianceSite::Angle);
      dabTip.roundness =
          baseRoundness_ * varianceScale(roundnessVariance_, local, seed_, dabIndex,
                                         VarianceSite::Roundness);
      // Scatter's span is 2.0 -- two RADII, i.e. one DIAMETER -- because
      // `PsScatter::scatter`'s own comment says the file's jitter is "a
      // fraction of the DIAMETER" while `BrushTip::scatter` (this field) is a
      // multiplier of RADIUS (`applyPerDabScatter()`'s `magnitude = tip.scatter
      // * tip.radius`). A `v.jitter` of 1.0 (Photoshop's own 100%) must
      // therefore reach a magnitude of one full diameter -- two radii -- which
      // is exactly `span * jitter` at `span == 2.0`.
      dabTip.scatter = varianceOffset(scatterVariance_, local, 2.0f, seed_, dabIndex,
                                      VarianceSite::Scatter);

      // Scatter COUNT (Part 1, `PsScatter::count`/`countJitter`): a
      // MULTIPLICATIVE resolution, the same shape Size and Roundness use
      // above, scaling `baseCount_` (Photoshop's own `Cnt `) rather than
      // replacing it outright -- there is no "offset" reading for a dab
      // count the way there is for an angle. Rounded to the nearest integer
      // and clamped to [1, 16]: at least one dab must always land at a
      // nominal position (a count that resolved to 0 would be silent paint
      // loss on a brush whose Count Jitter control happens to sample a
      // dynamic source at 0 this dab), and 16 is comfortably above the
      // widest `Cnt ` this build has measured (5, across the 68 scattering
      // presets `brush/BrushModel.hpp` reports on) while still matching
      // Photoshop's own Scatter Count ceiling.
      const float countMul =
          varianceScale(countVariance_, local, seed_, dabIndex, VarianceSite::Count);
      resolvedCount =
          std::clamp(static_cast<int32_t>(std::lround(baseCount_ * countMul)), 1, 16);
    }
    dabTip.count = resolvedCount;
    // No further flooring here: `brush/Variance.hpp`'s `minimum` is already
    // the floor, applied inside `varianceScale()`'s own formula above. There
    // is no second, pixel-space floor left to apply downstream of it --
    // `BrushTip::sizeFloorPx` (and this exact `std::max()` call) is gone.
    lastDabRadius_ = dabTip.radius;

    // One deposit dispatch per SUB-DAB -- `resolvedCount` of them, all at
    // this ONE nominal position `p`. Each sub-dab draws its OWN scatter
    // offset (`applyPerDabScatter()`'s `subIndex` parameter, folded into the
    // seed before SCATTER's own random draw) so N dabs stamped at one
    // position do not all land on the exact same pixels -- the whole point
    // of Scatter Count.
    //
    // **Provably a no-op at `resolvedCount == 1`** -- every existing preset
    // before this feature, and most of the 68 scattering ones (`Cnt `
    // measured 1x21, 2x28, 3x18, 5x1): the loop runs exactly once, at
    // `subIndex == 0`, and `applyPerDabScatter()`'s own fold is the identity
    // there by construction. `app/selftest/ScatterCount.cpp` asserts this
    // bit-for-bit and sabotage-proves it.
    //
    // **`dabs_`/`seed_`'s own per-dab index advances once per NOMINAL
    // position, never once per sub-dab.** VELOCITY, FADE, NOISE, DIRECTION
    // and INITIAL DIRECTION are all properties of the stroke's own PATH --
    // consecutive dab POSITIONS, which do not change between sub-dabs
    // stamped at one position -- so advancing `dabs_` per sub-dab would move
    // those five signals for a reason that has nothing to do with them.
    // `distanceTravelled_`/`stepDist`, likewise, are computed once above this
    // loop, from consecutive PATH positions, not sub-dabs. Only RANDOM
    // (`dynamicRandomDraw(seed_, dabIndex)`, which nothing downstream reads
    // per sub-dab today) and SCATTER's own draw read a per-dab index, and
    // only the second actually varies within this loop, via `subIndex` alone.
    for (int32_t subIndex = 0; subIndex < resolvedCount; ++subIndex) {
      const Vec2 centre = applyPerDabScatter(p, dabTip, seed_, static_cast<uint32_t>(dabs_), dx,
                                             dy, static_cast<uint32_t>(subIndex));

      // The four routes differ in exactly this call, and each takes
      // `selection`. Everything around it -- the tile bookkeeping, the
      // counters, the revision bump and the single history entry -- is
      // shared, because none of it is a property of what a texel is made of
      // or of which direction the stroke moves it.
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
    }
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
