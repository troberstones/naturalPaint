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
    case StrokeRoute::PencilDeposit: return "pencil-deposit";
    // One name for both tools, deliberately: the options bar prints this beside
    // the tool's own label, so "Dodge -> tonal-brush" and "Burn -> tonal-brush"
    // together say the true thing -- one engine, two directions. A pair of
    // names would suggest two routes to re-validate against, which is exactly
    // the comparison `depositPending()` makes every frame.
    case StrokeRoute::TonalBrush: return "tonal-brush";
    case StrokeRoute::CloneStamp: return "clone-stamp";
    case StrokeRoute::Smudge: return "smudge";
    case StrokeRoute::PaintSim: return "paint-sim";
    // Named for the STORE it writes and not for the tool that reaches it, like
    // every other row here: the options bar prints this beside the tool's own
    // label, so "Brush -> mask-paint" says the one thing a user needs when a
    // brush stroke is not appearing where they expected it to.
    case StrokeRoute::MaskPaint: return "mask-paint";
  }
  return "?";
}

const char* layerEditTargetName(LayerEditTarget target) noexcept {
  switch (target) {
    case LayerEditTarget::Content: return "content";
    case LayerEditTarget::Mask: return "mask";
  }
  return "?";
}

LayerEditTarget resolveLayerEditTarget(bool maskRequested, const Layer* layer) noexcept {
  // Header: `Mask` only when there really is one. A null layer, or one whose
  // `mask` is absent, resolves to `Content` however the flag is set.
  //
  // **The flag deliberately survives a click onto a maskless row**, which is
  // why this resolution happens at every read rather than once at the click:
  // the flag records what the user last asked for, so clicking back onto a
  // masked layer restores their choice instead of silently forgetting it. The
  // cost of that is exactly this function -- every reader must resolve rather
  // than read the flag -- and the panel resolves through it too, so the chip
  // cannot be lit over a store nothing is writing.
  if (!maskRequested) return LayerEditTarget::Content;
  if (layer == nullptr) return LayerEditTarget::Content;
  if (!layer->mask.has_value()) return LayerEditTarget::Content;
  return LayerEditTarget::Mask;
}

StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept {
  // What separates §1's families of stroke: a deposit adds, an erase removes,
  // and a tonal stroke moves the colour of what is already there without
  // touching how much of it there is. Carried as flags through one shared body
  // rather than as three copies of the target tests, because the target half of
  // the question -- locked before kind, a store to write being part of the
  // question, "no target" being its own row -- has exactly one right answer and
  // three copies of it would drift.
  //
  // **Two bools rather than an enum**, and the reason is that the two are not
  // parallel questions: `erasing` selects between two arithmetics that reach
  // *both* raster layer kinds, while `tonal` selects a family that reaches only
  // one and answers `None` everywhere else. An enum would suggest a third
  // symmetric arm to fill in on the Pigment row, which is precisely the row the
  // header argues must stay a refusal.
  bool erasing = false;
  // The second bit, and it is not a third value of the first: a pencil neither
  // adds at a rate nor removes, it *stamps*. Carried the same way `erasing` is
  // and for the same reason -- the target half of the question (locked before
  // kind, a store to write being part of the question, "no target" being its
  // own row) has one right answer, and a third copy of it would drift.
  bool pencil = false;
  bool tonal = false;
  // The second bit, and a third family: a clone neither adds the brush's ink
  // nor removes paint -- it composites texels read from a pre-stroke snapshot
  // of the same store (§1b, brush/CloneStamp). Carried the same way `erasing`
  // is, and for the same reason: the *target* half of the question -- locked
  // before kind, a store to write being part of the question, "no target"
  // being its own row -- has one right answer, and a third copy of it would
  // drift from the other two.
  bool cloning = false;
  // The second bit, and it is deliberately NOT a third value of the first one.
  // `erasing` selects between "adds" and "removes" on a target the two families
  // agree about; smudge answers a different question on four of the six rows
  // (Pigment, alpha-locked, and the two PaintSim ones), so folding it into a
  // three-state enum would put three families through one set of target tests
  // that only two of them share. Two bools, one shared body, and the rows that
  // differ are spelled out where they differ -- header §1's Smudge paragraphs.
  bool smudging = false;

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
    // brush/PencilDeposit, and the header §1's Pencil rows. This case used to
    // sit in the not-built list below, so a drag with the pencil reached no
    // layer and said nothing at all about why.
    case Tool::Pencil:
      pencil = true;
      break;
    // Both of these used to sit in the not-built list below, so Dodge and Burn
    // routed nowhere and did nothing at all. One route for the two, with the
    // direction latched by `StrokeSession::begin()` off the same `tool_` this
    // switch reads -- see the header §1's Dodge/Burn rows and brush/TonalBrush
    // §0.
    case Tool::Dodge:
    case Tool::Burn:
      tonal = true;
      break;
    // §1b. This case used to sit in the not-built list below with the other
    // palette cells, so the clone stamp routed nowhere and did nothing at all.
    case Tool::CloneStamp:
      cloning = true;
      break;
    // brush/Smudge. This case used to sit in the not-built list below too, so
    // a drag with the smudge tool reached no layer, wrote no texel, produced no
    // message and recorded nothing -- see the header §1's Smudge rows.
    case Tool::Smudge:
      smudging = true;
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
    // **Built, and `None` is its answer rather than its placeholder.** Measure
    // drags a ruler and writes nothing at all (app/MeasureLine.hpp §0), so it
    // has no target layer, no locked-layer refusal and nothing for §1's table
    // to route -- it is here for the same reason the eyedropper and the hand
    // are, not for the reason the block below is. Moved out of that block when
    // it shipped, which is exactly the move that block's comment asks for.
    case Tool::Measure:
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
    case Tool::Frame:
    case Tool::PaintBucket:
    case Tool::Gradient:
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
  // **And except for the pencil**, for the same shape of reason one step
  // over: `sim::PaintSim`'s entire output is diffusion, wet edges and
  // granulation, so a tool whose defining property is that its mark has no
  // soft edge (brush/PencilDeposit §0) would produce the softest mark in the
  // build there -- on the canvas texture rather than on a layer.
  //
  // **And except for the tonal tools, for the same shape of reason.** The
  // solver has no dodge step either, so a Dodge sent there would run the paint
  // path with the brush's loaded pigment and deposit colour where the user
  // asked for a tonal shift -- the tool doing something else entirely, silently,
  // on the one surface where nothing in the document records it.
  //
  // **And except for the clone stamp**, for the same argument read one step
  // further (§1b): the solver's dense canvas texture is not a document's tile
  // store, so there is nothing to *sample*, and a clone sent there would lay
  // down the foreground colour instead. Nothing to copy, so nowhere to go.
  //
  // **And except for the smudge**, which is the eraser's row with the argument
  // one notch stronger: the solver has no smudge step either, so a smudge sent
  // there would run the paint path and deposit the loaded FOREGROUND pigment --
  // a tool whose whole promise is "introduces nothing new" introducing a colour
  // the picture did not contain.
  //
  // **One block, and it used to be two.** A union merge left a second
  // `if (target == nullptr)` immediately below this one carrying the smudge
  // paragraph above and the narrower condition `(erasing || smudging)`. It was
  // dead -- the first block returns on every path -- but dead in the dangerous
  // direction: its condition is a strict SUBSET of this one, so deleting or
  // reordering the wrong one of the pair would have sent the Pencil, Dodge,
  // Burn and Clone Stamp to `StrokeRoute::PaintSim` with no document open, and
  // each of those four is a tool that would then have painted the solver
  // canvas with the loaded pigment while looking like it had worked. The
  // paragraphs are merged here and the condition kept at the wider of the two.
  // `app/selftest/LayerMask.cpp` asserts all five refusals against a null
  // target by name, which is what makes the merge stick: re-narrowing the
  // condition fails four assertions rather than passing silently.
  if (target == nullptr)
    return (erasing || pencil || tonal || cloning || smudging) ? StrokeRoute::None
                                                              : StrokeRoute::PaintSim;

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
  if (target->kind == LayerKind::Pigment && target->pigmentTiles) {
    // **The pencil refuses this row rather than falling back to `CpuDeposit`**
    // -- header §1's Pencil rows carry the argument. `brush/Deposit` has no
    // per-stroke ceiling at all, so "one dab is the whole mark" has nothing to
    // be a rule about there, and a pencil routed here would work, draw
    // soft-edged marks, and be a pencil in name only.
    if (pencil) return StrokeRoute::None;
  //
  // **A Pigment layer refuses the tonal tools, and that row is a decision.**
  // Header §1's Dodge/Burn bullets carry it in full: a Pigment texel holds a
  // Mixbox `Latent` premultiplied by mass, not a display-referred colour, and
  // raising a latent to a power is not a Kubelka-Munk mix of anything. The two
  // meaningful operations already have names -- less mass is
  // `brush/PigmentErase`, a lighter paint is a different `Latent` -- so this
  // refuses rather than guessing between them. Checked inside the Pigment arm
  // and not before it, so a Pigment layer with no store still refuses for the
  // reason it refuses everything else.
    if (tonal) return StrokeRoute::None;
    // **The clone refuses a Pigment layer, by name** -- header §1b. A cloned
    // dab is mostly falloff, and at partial coverage the operation there is a
    // Kubelka-Munk mixture of two latents weighted by mass (`depositTexel()`),
    // not a lerp of four premultiplied channels. That is a `brush/PigmentClone`
    // this build does not have, and copying the seven channels straight would
    // look right at full opacity and be wrong at every soft edge -- the version
    // nobody would report.
    if (cloning) return StrokeRoute::None;
  //
  // **The smudge refuses this row by name, on a stated condition** -- header
  // §1's Smudge paragraphs. `brush/Smudge` §2's pick-up is a coverage-weighted
  // ARITHMETIC mean, and the arithmetic mean of a footprint of Kubelka-Munk
  // latents is not what mixing those paints means here: `depositTexel()` mixes
  // with a MASS-weighted lerp whose exactness at `m == 0` is what makes
  // `brush/Deposit` §1's idempotence-in-hue invariant assertable at zero
  // tolerance. This row opens when someone decides what the mass-weighted mean
  // of a footprint of latents is and asserts it -- the same shape of
  // conditional refusal the Pigment ERASE row carried until `brush/Deposit` §4
  // paid off the condition it named.
    if (smudging) return StrokeRoute::None;
    return erasing ? StrokeRoute::PigmentErase : StrokeRoute::CpuDeposit;
  }
  if (target->kind == LayerKind::RGB && target->rgbTiles) {
    // **The tonal route is checked before the alpha lock, and that ordering is
    // the decision.** `alphaLocked` freezes the layer's alpha;
    // `brush/TonalBrush` §1's whole invariant is that the alpha channel is
    // *copied*, not recomputed, so the lock is satisfied by construction and a
    // refusal here would block an edit the flag has no quarrel with. This is
    // `RgbDeposit`'s position on the same flag, reached by the same argument,
    // and deliberately not the eraser's -- that route exists to remove alpha,
    // which is exactly what the lock is for.
    if (tonal) return StrokeRoute::TonalBrush;
    // Alpha lock refuses the ERASER, and only the eraser. `StrokeRoute::RgbErase`
    // exists to take alpha OUT of the layer (brush/RgbErase.hpp §0), which is
    // exactly the quantity `alphaLocked` freezes (core/Layer.hpp's own comment
    // on the member; brush/RgbDeposit.hpp §4.5 derives the rule) -- letting the
    // erase through would make the flag decorative. `RgbDeposit` is NOT
    // affected: painting on an alpha-locked layer is the whole feature, not a
    // refusal, and `brush/RgbDeposit.cpp`'s `depositRgbTexel()` is where the
    // colour-only composite actually happens, not here.
    // The alpha lock above deliberately does NOT catch the pencil: a pencil is
    // a deposit, it reuses `depositRgbTexel()`'s colour-only composite
    // unchanged (brush/RgbDeposit §4.5, brush/PencilDeposit §4), and drawing
    // inside existing alpha is the feature rather than the refusal.
    if (pencil) return StrokeRoute::PencilDeposit;
    // **Alpha lock does NOT refuse the clone**, and the disagreement with the
    // line above it is the decision (§1b): a clone adds colour, which is
    // exactly what an alpha-locked layer still permits, and
    // `brush/CloneStamp` §1 honours the freeze with a colour-only composite
    // that needs no un-premultiply. Refusing here would block a legitimate
    // edit; letting the ERASE through above would make the flag decorative.
    if (cloning) return StrokeRoute::CloneStamp;
    //
    // **And it refuses the SMUDGE, for a sharper version of the same
    // argument.** Smudge moves alpha inseparably from colour --
    // `brush/Smudge` §5 derives why the write has to be one lerp factor across
    // all four premultiplied channels -- so there is no smudge composite that
    // holds `dst.a` still without un-premultiplying the finger, a division by
    // the finger's own alpha which is exactly 0 whenever the finger is empty.
    // `RgbDeposit` could be given §4.5's locked composite because its source
    // colour is STRAIGHT to begin with; this route's is not.
    if ((erasing || smudging) && target->alphaLocked) return StrokeRoute::None;
    if (smudging) return StrokeRoute::Smudge;
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

StrokeRoute strokeRouteFor(Tool tool, const Layer* target, LayerEditTarget editTarget) noexcept {
  // **Delegation, not a second table** -- the header says why, and it is this
  // file's own standing warning: "the options bar's route indicator read 'goes
  // to the solver' grey for a live RGB stroke for exactly as long as it had its
  // own copy of the test". Every `Content` answer in the build comes from the
  // one function above, including this one.
  if (editTarget == LayerEditTarget::Content) return strokeRouteFor(tool, target);

  // From here the caller has said "the mask", and the rows below are the whole
  // of what that means. They are short because a mask is one store of one
  // scalar, so most of the two-argument form's structure -- which of two
  // content stores, whether a store was ever allocated, what a latent means --
  // has no analogue here.

  // A `Mask` target on a layer that has no mask does not reach a route.
  // `resolveLayerEditTarget()` is what callers are supposed to go through, and
  // it never produces this combination; answered here anyway rather than
  // trusting them to, because the cost of being wrong is a brush that draws
  // nothing with a chip lit and no message -- the exact failure this file's §6
  // records the paint bucket having shipped with.
  if (target == nullptr || !target->mask.has_value()) return StrokeRoute::None;

  // Locked before anything else, exactly as the content table does it and for
  // its stated reason: a locked layer refuses for being locked whatever is
  // being aimed at, so the UI's "clear its Lock in LAYERS" message is the one a
  // user gets for the one problem they can actually fix. A mask is part of the
  // layer -- `io/NpaintFile` writes it into the layer's own part -- so the lock
  // covers it.
  if (target->locked) return StrokeRoute::None;

  // **`alphaLocked` deliberately does NOT refuse here, and the disagreement is
  // the decision.** That flag freezes the layer's own alpha channel
  // (core/Layer.hpp), and a mask is not that channel: it is a separate store
  // that multiplies the layer's coverage at composite time (core/Mask.hpp's
  // first sentence). Refusing would block the gesture the flag exists to make
  // possible -- trim a layer's visible extent without touching its pixels --
  // and it is the same argument `RgbDeposit` wins on the row above.

  switch (tool) {
    // **The brush paints a mask, and it is the only tool that does.**
    // brush/MaskPaint §1: a mask sample has no privileged end, so "paint" and
    // "erase" are two directions of one lerp toward the ink's coverage --
    // painting black hides, painting white reveals -- and an eraser aimed here
    // would be a second control over a number the brush already reaches both
    // ends of. DryBrush travels with Brush as it does on every other row: the
    // two differ in the dynamics that shape a dab, not in where the dab lands.
    case Tool::Brush:
    case Tool::DryBrush:
      return StrokeRoute::MaskPaint;

    // The refusals, and each is a real question deferred rather than an
    // oversight. `--selftest` asserts every one of them by name, so opening a
    // row means answering its question rather than deleting a case label.
    //
    //   * **Eraser** -- has no meaning here that is not already spelled "paint
    //     white" (§1 above). Photoshop's answer is "paints with the background
    //     colour", which is a decision about the background swatch and not
    //     about masks; it can be made later without changing this module.
    //   * **Pencil** -- `brush/PencilDeposit` §1 thresholds its coverage to an
    //     aliased keep/drop. A hard-edged mask is a legitimate thing to want,
    //     but the threshold lives inside that module's own texel step against a
    //     premultiplied RGBA texel, so it is a `brush/MaskPencil` rather than a
    //     parameter.
    //   * **Dodge/Burn** -- `brush/TonalBrush` §0 counts a tonal shift of a
    //     *colour*. Dodging a coverage is either a gamma on it or a different
    //     ceiling, and those are two different features wearing one name.
    //   * **Clone Stamp** -- would need a snapshot of a `MaskTileStore`, which
    //     `brush/CloneStamp`'s snapshot type is not.
    //   * **Smudge** -- `brush/Smudge` §2's pick-up is a coverage-weighted mean
    //     of premultiplied RGBA; the scalar analogue is well defined but its
    //     "finger has no alpha" degenerate case is not the same one, so it is a
    //     derivation rather than a substitution.
    //   * **Water** and everything else -- these route nowhere on a content
    //     store either, and a mask does not give them a destination they were
    //     otherwise missing.
    case Tool::Eraser:
    case Tool::Pencil:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::CloneStamp:
    case Tool::Smudge:
    case Tool::Water:
    case Tool::Move:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::Eyedropper:
    case Tool::PaintBucket:
    case Tool::Gradient:
    case Tool::Hand:
    case Tool::Zoom:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      return StrokeRoute::None;
  }
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
    // **"pencil stroke", not "brush stroke"** -- the same requirement the
    // eraser's label above states. A pencil mark and a brush mark are the two
    // edits in this build a user is most likely to have made one after the
    // other over the same pixels, and a history column that called both of them
    // "brush stroke" is a column that cannot be read.
    case Tool::Pencil: return "pencil stroke";
    // **Two labels for one route**, and this is the one place the two tools
    // must NOT be folded together. `strokeRouteName()` says "tonal-brush" for
    // both because a route is what re-validation compares; a history row is
    // what a user reads to find the edit they want back, and "dodge" and
    // "burn" are opposite edits. A shared "tonal adjust" would make the panel
    // unable to distinguish a lightening pass from the darkening pass the user
    // made to correct it -- the identical complaint the eraser's own row is
    // written about, one level finer.
    case Tool::Dodge: return "dodge";
    case Tool::Burn: return "burn";
    // A noun for what the edit did, in the same form as the rows above it, and
    // distinct from "brush stroke" for the identical PRD O2 reason: a user
    // scanning the history panel for the retouch they want back cannot find it
    // in a column of identical brush rows, and a clone is exactly the kind of
    // edit that gets undone selectively.
    case Tool::CloneStamp: return "clone stamp";
    // Likewise its own noun rather than the generic "stroke": PRD O2's panel is
    // scanned to find an edit to undo, and a smudge is the row a user most
    // wants to find -- it is the one edit in the paint family that destroys
    // information (the colour it dragged away is not recoverable from the
    // result) and therefore the one whose undo step has to be recognisable.
    case Tool::Smudge: return "smudge";
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    // Built, and deposits nothing -- see strokeRouteFor()'s own note on this
    // arm. A measurement records no history entry at all (app/MeasureLine.hpp
    // §0), so it never reaches a label.
    case Tool::Measure:
    // Same not-built list as strokeRouteFor() above, and the same reason
    // it is spelled out rather than a `default:` -- none of these can begin
    // a stroke (strokeRouteFor() refuses them all), so none of them needs a
    // label of its own; they fall through to the generic one below.
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Frame:
    case Tool::PaintBucket:
    case Tool::Gradient:
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

// --- the Clone Stamp's source gesture (header §1b) -------------------------

void setCloneAnchor(AppState::CloneSourceState& clone, Vec2 anchor) noexcept {
  clone.anchor = anchor;
  clone.haveAnchor = true;
  // **The load-bearing line.** A new source means the next stroke re-derives
  // the vector from its own pen-down; an offset that survived the click would
  // leave the gesture looking like it worked while the copy carried on from the
  // old source -- a tool ignoring the user's most explicit instruction,
  // silently.
  clone.haveOffset = false;
  clone.offset = Vec2{0.0f, 0.0f};
}

bool latchCloneOffset(AppState::CloneSourceState& clone, Vec2 penDown) noexcept {
  if (!clone.haveAnchor) return false;
  if (!clone.haveOffset) {
    // `source = pointer + offset`, so `offset = anchor - penDown` puts the
    // stroke's first dab exactly on the anchor -- which is what an Option+click
    // followed by a stroke starting in the same place has to mean, and the one
    // sign convention this whole feature has to agree on.
    clone.offset = Vec2{clone.anchor.x - penDown.x, clone.anchor.y - penDown.y};
    clone.haveOffset = true;
  }
  // Aligned: every call after the first is the identity. See
  // `AppState::CloneSourceState` for why the non-aligned variant is not built.
  return true;
}

std::string cloneSourceRefusal(const AppState::CloneSourceState& clone) {
  if (clone.haveOffset) return {};
  // One sentence for both states -- no anchor at all, and an anchor whose
  // offset nothing has latched yet -- because they present to a user as the
  // same thing and have the same fix. Named in the gesture's own words
  // ("Option-click"), since a modifier that does nothing visible until it is
  // held is not something anyone discovers by trying.
  return "clone stamp: no source set. Option-click the canvas to set the clone source, "
         "then paint.";
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

// **Deliberately not one more name inside `toolSamplesCanvas()` above**, even
// though Measure shares the eyedropper's palette group and its
// `ToolCursor::Sample`. The header carries the argument; the short form is
// that these two predicates gate two DIFFERENT blocks of `ui/MacPaintUI.cpp`'s
// canvas, so a Measure that satisfied the eyedropper's gate would have every
// ruler drag handed to `applyEyedropperPick()` and would silently overwrite
// the foreground colour.
bool toolMeasuresCanvas(Tool tool) noexcept { return tool == Tool::Measure; }

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

  // --- the smudge's own block (brush/Smudge.hpp §3b) ----------------------
  //
  // **The single site that resolves it**, and the reason `SmudgeParams` sits on
  // `BrushState` rather than beside the flood tools' blocks: both halves land
  // here, in one place, off the one mapping `smudgeToolParamsFor()`. Spelling
  // `brush.tool == Tool::Smudge` here instead would be a second copy of the
  // options bar's own gate, and the way two copies stop agreeing is a row of
  // live controls over a struct no stroke reads.
  //
  // `tip.smudgeStrength` is left at its own default -- the same 0.5 -- for
  // every other tool. It is read by exactly one route, so what it holds while
  // a brush is selected changes nothing; setting it unconditionally from a
  // block that only the smudge edits would be the more surprising of the two.
  //
  // **The bitmap override is deliberately last of the two and deliberately
  // conditional on the POINTER, not on the id.** An id naming a dab the
  // library can no longer resolve (a folder moved out from under the app)
  // leaves `tipBitmap` null, and falling through to the brush's own tip is the
  // only answer that still smudges: substituting an empty bitmap would give a
  // tip with no coverage anywhere, which on this route is indistinguishable
  // from the tool being broken.
  if (const SmudgeParams* smudge = smudgeToolParamsFor(brush, brush.tool)) {
    tip.smudgeStrength = smudge->strength;
    if (smudge->tipBitmap) tip.bitmap = smudge->tipBitmap;
  }

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
  //
  // **This decode is NOT clamped, and since T25a that is load-bearing rather
  // than incidental.** `BrushState::rgb` is scene-referred and may exceed 1.0
  // (app/AppState.hpp); `srgbDecode()` is unclamped and monotonic, so an
  // over-range foreground arrives here as an over-range linear triple and is
  // deposited into HALF texels that can hold it. An RGB-layer stroke is
  // therefore one of the routes that keeps the picked value, which is exactly
  // what the COLOR panel's over-range badge promises the user.
  tip.linearRgb = {srgbDecode(shiftedRgb[0]), srgbDecode(shiftedRgb[1]),
                   srgbDecode(shiftedRgb[2])};

  // **The pigment half is the route that clamps, and it is the same clamp on
  // both arms below.** `rgbToLatent()` does it internally (paint/Palette.cpp
  // carries the physical argument: there is no reflectance above total
  // reflectance), so the `valid()` arm needs nothing here. The fallback arm
  // did need it: it copies the sRGB triple verbatim, so an over-range
  // foreground used to walk straight into `Latent::c` and give a Pigment
  // layer a weight above 1 that `mixLatents()` would then lerp against real
  // ones. Two arms of one branch disagreeing about whether pigment is
  // bounded is the sort of divergence nobody looks for, and it only exists
  // in a build with no LUT -- i.e. in tests and in a broken install, the two
  // places least likely to have it noticed.
  if (lut.valid())
    tip.pigment = lut.rgbToLatent(shiftedRgb[0], shiftedRgb[1], shiftedRgb[2]);
  else
    // No LUT: `Latent::c` is Mixbox's own three weights and the fourth
    // (white) is derived, so a straight copy of the sRGB triple is not the
    // right latent -- but it is a colour in the right family, and a build
    // that never loaded the 512x512 PNG painting *something* beats one that
    // paints white. The LUT is loaded by main.cpp before any UI exists, so
    // this branch is for tests and for a broken install.
    tip.pigment.c = clampToDisplayRange(shiftedRgb);
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
  // **`!= Pigment`, not `== Rgb`.** `ColorMode` had two members for its whole
  // life and this line was written as the two-way question it then was; when
  // `Munsell` arrived it compiled clean and routed a whole picker's output
  // into the pigment branch below -- a panel that looks entirely live while
  // every stroke lays down `defaultPalette()[pigment]`. There is no
  // diagnostic for that, so `--selftest`'s Munsell section asserts this
  // function's answer in Munsell mode rather than trusting the reading.
  // Written as "everything that is not a pigment is the triple" so the next
  // member added is right by default instead of wrong by default.
  if (brush.colorMode != ColorMode::Pigment) return brush.rgb;
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
  // The same `!= Pigment` rule as `foregroundSrgb()` above, for the same
  // reason -- this one is only a status string, so getting it wrong would
  // have been the quieter half of the same bug. "Munsell page" rather than a
  // notation like "5R 5/8": the hue is a CIELUV angle and the mapping to
  // Munsell hue is a renotation table that does not exist yet, so naming a
  // hue family here would be a confident wrong answer
  // (docs/munsell-picker.md, "The hue control").
  if (brush.colorMode == ColorMode::Munsell) return "Munsell page";
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
                          const DynamicInputs& hardwareInputs,
                          const AppState::CloneSourceState* clone) {
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
  // **The edit target is resolved HERE, from the document this call was handed,
  // rather than passed in by the caller.** `begin()` already derives the layer
  // from `layerIndex` and the document; the store within that layer is the same
  // kind of fact, and reading it here means every existing call site --
  // `ui/MacPaintUI`'s canvas block, `main.cpp`'s demos, the selftests -- reaches
  // the mask route without a signature change and without any of them being
  // able to pass a target that disagrees with the panel the user is looking at.
  //
  // Resolved, not read: `maskIsEditTarget` is a request, and a request for a
  // mask on a layer that has none is `Content` (`resolveLayerEditTarget()`).
  const LayerEditTarget editTarget = resolveLayerEditTarget(doc.maskIsEditTarget, &layer);
  // The route function is the single table (header §1); this refusal reads it
  // rather than re-deriving the same conditions, so the two cannot disagree
  // about which strokes a locked or non-Pigment layer accepts.
  const StrokeRoute route = strokeRouteFor(tool, &layer, editTarget);
  if (!strokeRouteWritesLayer(route))
    return refuse(std::string("stroke refused: the ") + strokeEditLabel(tool) + " on layer " +
                  std::to_string(layerIndex) + " ('" + layer.name + "', " +
                  layerKindName(layer.kind) + (layer.locked ? ", locked" : "") +
                  (layer.alphaLocked ? ", alpha-locked" : "") + ", target " +
                  layerEditTargetName(editTarget) + ") routes to " + strokeRouteName(route) +
                  ", which does not write a layer.");

  // **The clone's second precondition, and the reason it is here rather than in
  // `strokeRouteFor()`** -- header §1b's last paragraph. The route table is pure
  // in `(tool, target)` and this is neither; it is a gesture the user has or has
  // not made. Refused before anything is latched, so a stroke with no source
  // does not begin at all: with the offset at (0,0) every texel's source would
  // be itself, the composite would be a perfect no-op, and the tool would look
  // broken with nothing anywhere saying why.
  //
  // A null `clone` takes the same branch as an unanchored one deliberately --
  // "the UI forgot to pass it" and "the user has not set a source" have the same
  // correct answer.
  if (route == StrokeRoute::CloneStamp) {
    const AppState::CloneSourceState empty{};
    const std::string why = cloneSourceRefusal(clone != nullptr ? *clone : empty);
    if (!why.empty()) return refuse(why);
  }

  doc_ = &doc;
  layerIndex_ = layerIndex;
  layerCount_ = doc.document.layers.size();
  tip_ = tip;
  route_ = route;
  editTarget_ = editTarget;
  tool_ = tool;
  // **"mask stroke", not "brush stroke", for a stroke that went into a mask**,
  // and this is `strokeEditLabel()`'s own requirement one step further out. That
  // function's comment says a column of identical "brush stroke" rows in which
  // some of them actually took paint *off* is a panel that cannot be read, and
  // PRD O2's panel is scanned to find an edit to undo. A stroke that changed no
  // pixel of the layer and trimmed its visibility instead is at least as far
  // from "brush stroke" as an erase is -- undoing the wrong one of the two is
  // exactly the mistake the label exists to prevent.
  //
  // Not a row in `strokeEditLabel()` itself, because that function is pure in
  // the tool and the target store is not a property of the tool. Decided here,
  // where both are in hand, which is the same place the route was.
  label_ = route == StrokeRoute::MaskPaint ? "mask stroke" : strokeEditLabel(tool);

  // Transfer Opacity/Flow (Part 2, `PsTransfer::opacity`/`.flow`), resolved
  // HERE -- before any of the four `*_.begin()` calls below read
  // `tip.opacity`, and before this function's own `if (haveModel_)` block
  // further down that copies the rest of the model's Variance objects. That
  // block runs AFTER these three calls (it always has -- see its own
  // comment), and Opacity is a per-STROKE ceiling that gets latched into
  // `rgb_`/`erase_`/`pigErase_`/`tonal_`'s own members at exactly this point
  // (brush/RgbDeposit.hpp §2, brush/RgbErase.hpp §2, brush/PigmentErase.hpp
  // §2, brush/TonalBrush.hpp §3) -- there is no second chance to apply it once
  // those calls have run.
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
  // the same reason (brush/PigmentErase.hpp §2). Four `begin()`/`end()` pairs
  // rather than one switch because each one's `else` is the load-bearing half:
  // whichever route this stroke took, the other three must be left holding no
  // tiles, and an interrupted drag is exactly the case that reaches here with
  // one of them still live.
  if (route_ == StrokeRoute::PigmentErase)
    pigErase_.begin(resolvedOpacity);
  else
    pigErase_.end();

  // The pencil's ink and ceiling, latched from the same two fields the RGB
  // deposit reads and for the same reason (brush/PencilDeposit §2): the
  // accumulator is only correct against the colour and ceiling it was started
  // with. `layer.alphaLocked` travels with them, exactly as it does for
  // `rgb_` -- a lock cleared or set mid-drag must not change which composite
  // the dabs already spent are read back through.
  //
  // A fourth `begin()`/`end()` pair rather than a switch, for the reason the
  // three above give: each one's `else` is the load-bearing half, because
  // whichever route this stroke took the other three must be left holding no
  // tiles, and an interrupted drag is exactly the case that reaches here with
  // one of them still live.
  if (route_ == StrokeRoute::PencilDeposit)
    pencil_.begin(tip.linearRgb, resolvedOpacity, layer.alphaLocked);
  else
    pencil_.end();
  // The tonal route's accumulator, latched from the same slider for the same
  // reason again (brush/TonalBrush.hpp §3) -- and with one thing the other
  // three do not latch: the DIRECTION, read off `tool_` here and nowhere else.
  // Deciding it once, at pen-down, from the tool the session already validated
  // its route against is what makes a stroke that changed tool mid-drag
  // impossible to express: `depositPending()` re-asks `strokeRouteFor(tool_,
  // ...)` every frame with this same `tool_`, so a Dodge cannot become a Burn
  // without ending the stroke.
  // `Tool::Burn` explicitly rather than "not Dodge": this is the one line that
  // turns a tool into a sign, and a default that swallowed a future third tonal
  // tool into Dodge is exactly the silent wrong answer §0 argues one engine
  // must not make easy.
  if (route_ == StrokeRoute::TonalBrush)
    tonal_.begin(resolvedOpacity,
                 tool == Tool::Burn ? TonalDirection::Burn : TonalDirection::Dodge);
  else
    tonal_.end();
  // The clone route's own snapshot, offset and ceiling (brush/CloneStamp §2).
  // A fourth `begin()`/`end()` pair for the same reason there are three: each
  // one's `else` is the load-bearing half, and this one's is the most so --
  // the snapshot shares tiles with the layer, so a clone stroke followed by a
  // brush stroke that left it live would hold the whole previous target at
  // twice its size for as long as the application ran.
  //
  // **The store is snapshotted here, before any dab**, which is the ordering
  // core/TileStore.hpp requires of a copy ("take the reference, write, then
  // copy -- never the other order"). `layer.rgbTiles` is engaged on this route
  // by construction: `strokeRouteFor()` only answers `CloneStamp` for an RGB
  // layer whose store exists.
  //
  // `layer.alphaLocked` is latched with it, exactly as the RGB deposit latches
  // it, so a lock toggled mid-drag cannot change which composite the dabs
  // already spent were read back through.
  if (route_ == StrokeRoute::CloneStamp)
    clone_.begin(*layer.rgbTiles, clone != nullptr ? clone->offset : Vec2{0.0f, 0.0f},
                 resolvedOpacity, layer.alphaLocked);
  else
    clone_.end();
  // The smudge route's carried colour and its strength, latched for the stroke
  // (brush/Smudge.hpp §3): strength is how far the finger dominates the canvas,
  // and a stroke whose dominance moved half way through would have been picking
  // up under one rule and putting down under another. A fourth
  // `begin()`/`end()` pair rather than a fifth arm of a switch, for the reason
  // the three above are pairs: the `else` is the load-bearing half. It matters
  // more here than anywhere else, because what this engine holds after an
  // interrupted drag is a COLOUR -- a smudge_ left loaded across a `begin()`
  // would lay the previous stroke's paint down before the new stroke had picked
  // anything up, which is invisible until the two strokes are different
  // colours.
  //
  // **`tip.smudgeStrength`, NOT `resolvedOpacity`** -- brush/Smudge.hpp §3b.
  // This line used to read the same slider as the four `begin()`s above it, on
  // the argument that a strength and a stroke ceiling are one quantity; they
  // are not, and the price of pretending so was that the smudge inherited
  // `BrushState::opacity`'s default of 1, which is the single value at which
  // the tool provably never fades. The field it reads now has its own default
  // (0.5) and its own control, and there is deliberately no Transfer variance
  // applied to it: `opVr` is an opacity dynamic and this is not an opacity.
  if (route_ == StrokeRoute::Smudge)
    smudge_.begin(tip.smudgeStrength);
  else
    smudge_.end();

  // The mask route's target coverage and ceiling, latched together for the
  // reason every pair above is latched together (brush/MaskPaint §3): the
  // accumulator counts a fraction of the way to *this* target under *this*
  // ceiling, so a stroke whose target moved half way through has no
  // well-defined destination and its accumulator is a fraction of nothing.
  //
  // `tip.linearRgb` and `resolvedOpacity` -- the same two fields
  // `rgb_.begin()` above reads, with the same meanings. The colour becomes a
  // coverage through `maskTargetForInk()` (brush/MaskPaint §2), which is where
  // the "50 % grey means 50 % coverage" decision lives; the opacity is the
  // per-stroke ceiling exactly as it is on every other route, so the OPACITY
  // slider does the same thing to a mask stroke that it does to a paint stroke,
  // which is what keeps the BRUSH panel's caption honest.
  //
  // The `else` carries the weight the other six do: whichever route this stroke
  // took, the rest must be left holding no tiles, and an interrupted drag is
  // exactly the case that reaches here with one of them still live.
  if (route_ == StrokeRoute::MaskPaint)
    maskPaint_.begin(maskTargetForInk(tip.linearRgb), resolvedOpacity);
  else
    maskPaint_.end();

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
  //
  // **Asked with `editTarget_`, the target LATCHED at pen-down, and not with a
  // live read of `doc_->maskIsEditTarget`.** A live read would make a click on
  // the mask thumbnail during a drag redirect the rest of the stroke into a
  // different store under one history entry that names neither, which is the
  // same class of half-and-half edit §5 refuses for a layer swapped at the same
  // index. Latched, the target that moved is a route that no longer matches and
  // the stroke stops -- and it stops for the reason it should, because
  // `strokeRouteFor()` with the old target on a layer whose mask has since been
  // removed answers `None`.
  if (strokeRouteFor(tool_, &layer, editTarget_) != route_) {
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

      // The five routes differ in exactly this call, and each takes
      // `selection`. Everything around it -- the tile bookkeeping, the
      // counters, the revision bump and the single history entry -- is
      // shared, because none of it is a property of what a texel is made of
      // or of which direction the stroke moves it.
      const DepositCount c =
          // **The mask route is first and the only one that does not name a
          // content store.** `*layer.mask` rather than `*layer.rgbTiles` or
          // `*layer.pigmentTiles` -- and the engagement of that optional is
          // guaranteed by the re-validation above, which re-asks
          // `strokeRouteFor()` with `editTarget_` and stops the stroke if the
          // answer moved (a mask removed mid-drag is exactly that).
          route_ == StrokeRoute::MaskPaint
              ? maskPaint_.paintDab(*layer.mask, dabTip, centre, doc.width, doc.height, selection,
                                    &frameTiles_)
          : route_ == StrokeRoute::TonalBrush
              ? tonal_.toneDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height, selection,
                               &frameTiles_)
          : route_ == StrokeRoute::Smudge
              ? smudge_.smudgeDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height,
                                  selection, &frameTiles_)
          : route_ == StrokeRoute::RgbErase
              ? erase_.eraseDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height, selection,
                                &frameTiles_)
          : route_ == StrokeRoute::PigmentErase
              ? pigErase_.eraseDab(*layer.pigmentTiles, dabTip, centre, doc.width, doc.height,
                                   selection, &frameTiles_)
          : route_ == StrokeRoute::PencilDeposit
              ? pencil_.drawDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height, selection,
                                &frameTiles_)
          // The clone reads its source from the snapshot `begin()` took, not
          // from `*layer.rgbTiles` -- which is why the destination store is the
          // only one named here even though the route touches two.
          : route_ == StrokeRoute::CloneStamp
              ? clone_.cloneDab(*layer.rgbTiles, dabTip, centre, doc.width, doc.height,
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
  // would be one more place the routes could disagree about cleanup. All four
  // are dropped, not the one this stroke used -- exactly one of them was ever
  // live, and asking which at cleanup time is how the other three keep their
  // tiles after an interrupted drag.
  rgb_.end();
  erase_.end();
  pigErase_.end();
  pencil_.end();
  tonal_.end();
  // And the clone's snapshot with them -- the largest of the four to leave
  // behind, since it shares a tile with the layer for every tile the layer had
  // at pen-down.
  clone_.end();
  // The fourth, unconditionally with the rest. This one frees no tiles -- its
  // whole state is 16 bytes and a bool -- but dropping it here is what stops an
  // application sitting idle between strokes from holding a colour that the
  // next `begin()` is then responsible for clearing. Two places that must both
  // be right is one more than one place that must be.
  smudge_.end();
  // And the mask route's, with the rest and unconditionally, for the reason the
  // six above give: exactly one of them was ever live, and asking which at
  // cleanup time is how the others keep their tiles after an interrupted drag.
  maskPaint_.end();

  // Exactly one entry, and only for a stroke that put something down --
  // header §2.
  if (texels_ > 0) doc->recordEdit(label_, EditKind::Content);
  return strokeTiles_;
}

}  // namespace np
