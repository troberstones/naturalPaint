#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "brush/Deposit.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/StrokePath.hpp"
#include "paint/Palette.hpp"
#include "core/Layer.hpp"
#include "core/Tile.hpp"

// app/StrokeSession -- **the lifecycle of one stroke that reaches a Layer.**
//
// `brush/Deposit` owns the arithmetic of a dab (read its §1 first; it is not
// repeated here). This module owns the three things that arithmetic cannot
// decide on its own: which tool routes here at all, how the in-flight stroke
// reaches the screen, and how a stroke of N dabs becomes exactly one undo
// step.
//
// ==========================================================================
// 1. Routing -- which strokes come here and which still go to sim::PaintSim
// ==========================================================================
//
// **`sim::PaintSim` stays.** It is the fluid simulation for the watercolour,
// ink and oil media, this step does not touch it, and nothing here is a
// replacement for it. What changes is that one case now has a second
// destination, and `strokeRouteFor()` is the whole table -- one function, so
// the UI, `--selftest` and this header cannot hold three versions of it:
//
//   tool        target layer                     route
//   ---------   ------------------------------   ----------------------------
//   Brush       Pigment, with tiles, writable    CpuDeposit
//   DryBrush    Pigment, with tiles, writable    CpuDeposit
//   Brush       RGB, with tiles, writable        RgbDeposit  <- new
//   DryBrush    RGB, with tiles, writable        RgbDeposit  <- new
//   Brush       **any layer, locked**            None
//   DryBrush    **any layer, locked**            None
//   Brush       Adjustment / Media / Text / ...  None        <- was PaintSim
//   DryBrush    Adjustment / Media / Text / ...  None        <- was PaintSim
//   Brush       **no target at all**             PaintSim
//   DryBrush    **no target at all**             PaintSim
//   Water       anything                         PaintSim     (unchanged)
//   Eyedropper / Hand / Zoom                     None         (unchanged)
//
// Four rows are decisions rather than bookkeeping.
//
// **`Water` never routes to a layer, on any layer kind.** The water tool
// deposits water and no pigment (`app/AppState`'s own comment on the
// enumerator). A Pigment tile has seven channels and not one of them is water
// -- `docs/document-format.md`'s `pig.c0 pig.c1 pig.c2 pig.m` plus
// `res.R res.G res.B` -- so a CPU deposit of "water" could only mean depositing
// zero mass, which is indistinguishable from not painting; and an RGB tile has
// nowhere to put wetness at all. Wetness is a solver state; it belongs to the
// medium that simulates it and it is one of the things the readback bridge, not
// this step, will have to carry into a document.
//
// **A locked layer refuses rather than falling through to PaintSim.** Falling
// through is the tempting row, because it never blocks the user -- but it would
// put paint on the *solver canvas* when the user aimed at a layer, which is the
// one failure mode a painter cannot see and cannot undo. Refusing matches
// `core/LayerOps`, whose every setter refuses on `Layer::locked`, and leaves
// the UI free to say why. **Visibility is deliberately not a refusal**, for the
// same reason `core/LayerOps` does not refuse on it: hiding a layer is a view
// decision, and `layerCoverage()` already makes a hidden layer contribute
// nothing.
//
// **A target that cannot take the stroke refuses too, for exactly that
// argument.** This table used to end with "everything else keeps today's
// behaviour, which is the solver canvas", and that fallthrough was the same
// invisible wrong-target defect the locked row had already been written to
// prevent -- one line below it. Aiming at an RGB layer painted the canvas
// texture; so did aiming at an Adjustment layer, a Media layer, a Text layer,
// or a Pigment layer whose store had not been allocated. Paint appeared, the
// document never saw it, undo did not remove it, and save did not keep it. The
// RGB row is now a real destination and the rest are refusals with a reason the
// UI can print.
//
// **`nullptr` still routes to PaintSim, and that row is not a fallthrough.**
// It is the *only* case where the solver canvas is the destination the user
// meant: no document is open, so there is no layer to have aimed at, and
// watercolour and oil legitimately paint the dense canvas texture -- which is
// what every medium demo and the whole of `sim::PaintSim` does today. The
// distinction this table now draws is between "there was no target" and "there
// was a target and it could not take the stroke", and only the first of those
// is the solver's.
//
// ==========================================================================
// 2. One stroke is ONE undo step
// ==========================================================================
//
// A 400-dab stroke that produced 400 history entries would make undo useless:
// PRD O2's panel would be 400 rows of "brush stroke" and Cmd+Z would rub out a
// quarter of a millimetre. ADR-0005's undo is **stroke-granular**, so:
//
//   * `begin()` records nothing. The pre-stroke state is already the entry
//     sitting at the cursor -- that is `OpenDocument::recordEdit()`'s stated
//     contract ("call it after the mutation; the pre-edit state is the entry
//     already sitting at the cursor"), and taking a second snapshot at
//     pen-down would put an entry in the panel for *starting* to paint.
//   * `addPoint()` deposits and records nothing.
//   * `end()` calls `recordEdit(label, EditKind::Content)` exactly once, with
//     a label naming the tool that made the stroke.
//
// **The tiles survive that because they are copy-on-write.** The entry at the
// cursor holds a `Document` whose stores share `shared_ptr` slots with the
// live one; `PigmentTileStore::getOrCreate()` unshares before the first write
// of each dab, so the pre-stroke entry keeps the pre-stroke bytes and undo is
// byte-identical rather than approximately right. `--selftest` `memcmp`s the
// raw half words rather than arguing it.
//
// **A stroke that deposited nothing records nothing.** A click that lands off
// the canvas, or on a zero-radius tip, leaves no entry and does not move the
// revision -- an undo step that undoes nothing is a worse defect than a
// missing one, because the user has to press Cmd+Z twice and cannot tell why.
//
// ==========================================================================
// 3. Live feedback, without a full recomposite
// ==========================================================================
//
// PRD F3 (**P0**): "Pen-to-photon latency under 20 ms; the in-progress stroke
// does not wait on a full document re-composite." **That 20 ms is end-to-end,
// not a compute budget** -- it is sensor to photon, so a 19 ms composite has
// spent all of it and left nothing for input handling, the upload, the render
// pass or the present.
//
// The model is the user's chosen one, *scratch over last composite*, and the
// mechanism it uses is the incremental composite that landed at `2262a37` for
// exactly this reason:
//
//   1. `addPoint()` deposits this frame's dabs and returns **this frame's**
//      tile set (not the stroke's), sorted in `documentDirtyTiles()`' own
//      (y, x) order so `ui/DocumentTexture` can upload it one tile band at a
//      time.
//   2. It bumps `OpenDocument::revision` when, and only when, it wrote
//      something. That is what invalidates `DocumentTexture`'s revision-keyed
//      cache; the texture then diffs its retained snapshot against the live
//      document, gets back the same tile set, and recomposites and re-uploads
//      **those tiles alone**.
//
// So the "scratch" is the layer's own tiles and the "last composite" is the
// texture already on the GPU. There is no second scratch buffer, no separate
// preview layer to reconcile at pen-up, and therefore nothing that can differ
// between what the stroke looked like and what the document holds.
//
// **The per-frame cost this actually spends, and where it goes**, is measured
// and printed by `--selftest` rather than reasoned about here. One number is
// worth predicting because it is the non-obvious one: `DocumentTexture` keeps
// a snapshot of the previous frame's document, so every touched tile is
// *shared* at the start of every frame and `getOrCreate()` copies it -- a
// 224 KiB memcpy per touched tile per frame, which is the price of the
// snapshot-diff being able to see the change at all.
//
// **The revision bump makes the document dirty mid-stroke, deliberately.** It
// is: pigment has been deposited and is not on disk. `app/Journal` treats a
// moved `revision` as due only **on its interval** and a moved
// `structuralRevision` as due immediately (ADR-0008), and a content edit moves
// only the first -- so a stroke costs at most one journal write per interval,
// not one per frame.
//
// ==========================================================================
// 4. What this is not
// ==========================================================================
//
// **The pen IS wired to this now.** This section used to say it was not, and
// that "a deposit needs a target layer, and this application has no concept of
// an active layer". Both halves of that missing decision were made:
//
//   * `OpenDocument::activeLayer` -- on the session record, per document, and
//     deliberately not in `core::History` (that header carries the argument).
//   * `brushTipFor()` below -- what the PIGMENT panel's swatch means as a
//     `Latent`, which is the colour and deliberately not the three physical
//     constants.
//
// `ui/MacPaintUI.cpp`'s canvas block decides the route before it constructs a
// `PaintSim`, so painting a Pigment layer allocates no solver fields, and
// `--pen-demo` drags a synthetic pointer through the real UI to prove that
// something *calls* this class rather than only that the class works.
//
// **Not wired: the other direction.** Nothing carries solver state back into a
// document, so a stroke on a Media layer, and wetness generally, still live
// only on the dense canvas texture. That is the readback bridge, and it is
// still owed.
//
// ==========================================================================
// 5. The target, and the one hazard this cannot close yet
// ==========================================================================
//
// `begin()` takes a layer **index**, because that is what a UI has, and every
// frame re-validates it: the layer count must be what it was at pen-down, and
// the layer at that index must still route to **the same one** of §1's two
// deposit routes it did then. A stroke whose target has gone away drops its
// remaining dabs rather than writing anywhere, which `--selftest` exercises by
// deleting the target layer mid-stroke.
//
// Comparing against the latched route rather than merely against "some deposit
// route" is what stops a stroke changing medium under the pen: a Pigment layer
// swapped for an RGB one at the same index and the same count would otherwise
// keep the same session going and start writing RGB texels with a pigment
// tip's latent -- or, worse, RGB texels through an accumulator that was never
// started.
//
// **A pure reorder that preserves the count is not detected**, and that is
// stated rather than hidden. The durable fix is to key the target by
// `Layer::id` -- exactly the hazard that member was added for -- and it cannot
// be done here today: `core/Layer.hpp` says "0 means not yet assigned, and it
// is what every layer this build creates starts with", because ids are handed
// out lazily by `core::normalizeLayerIds()`, which only `captureLayerComp()`
// calls. In a document that has never used a comp *every* layer has id 0 and
// an id-keyed lookup would match the wrong one; assigning ids here instead
// would make a brush stroke mutate `Document::nextLayerId`, which is a change
// to a documented invariant of a shared header rather than to this module.
//
// It costs nothing today because **no UI path can produce it**: a stroke is one
// pointer drag, during which no menu, panel button or keybinding runs. It stops
// being free the moment a script, a plugin or a second input source can edit
// the stack while the pen is down, and that is the step that should key by id.
namespace np {

// Where a stroke with this tool, on this layer, deposits. See §1.
enum class StrokeRoute {
  None,        // the tool does not paint, or the target refuses the edit
  CpuDeposit,  // brush/Deposit, into the target layer's pigment tiles
  RgbDeposit,  // brush/RgbDeposit, into the target layer's rgb tiles
  PaintSim,    // sim::PaintSim's dense canvas texture, and only when there is
               // no document layer to have aimed at -- see §1's last paragraph
};

// The two routes that write a `Layer`, as one predicate, because four call
// sites ask the same question -- `begin()`'s refusal, `depositPending()`'s
// per-frame re-validation, `ui/MacPaintUI`'s canvas branch, and the options
// bar's route indicator, which accents a route that reaches the user's layer
// and greys one that does not -- and a fifth route added later must reach all
// four or reach none. The indicator is the reason this is a predicate and not
// an `== CpuDeposit` at each site: it read "goes to the solver" grey for a
// live RGB stroke for exactly as long as it had its own copy of the test.
inline bool strokeRouteWritesLayer(StrokeRoute route) noexcept {
  return route == StrokeRoute::CpuDeposit || route == StrokeRoute::RgbDeposit;
}

const char* strokeRouteName(StrokeRoute route) noexcept;

// `target` is the layer the stroke is aimed at, or nullptr when there is none.
// Pure and total: every (tool, target) pair has an answer and §1's table is
// the whole of it.
StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept;

// The history label for a stroke made with `tool`, in the same noun form
// `core/LayerOps`' `editLabel` uses ("duplicate", not "Duplicated") so PRD
// O2's panel reads consistently down the column.
const char* strokeEditLabel(Tool tool) noexcept;

// The pen's brush state, as a tip -- the one mapping from what the UI holds to
// what `brush/Deposit` takes, so the interactive route and `--selftest` cannot
// disagree about what a given brush deposits.
//
// **What the PIGMENT panel's swatch means as a `Latent`**, which is the second
// half of the missing decision section 4 named. The swatch is a
// `paint::Pigment` with an sRGB triple and three physical constants, and the
// answer here is deliberately the narrow one: the colour goes through
// `MixboxLut::rgbToLatent()` and **the three constants do not travel**.
// Density, staining and granulation are properties the *solver* reads -- they
// decide how a wash settles, lifts and pools -- and `brush/Deposit` simulates
// none of that (its own section 1 says so: "no diffusion, no edge darkening,
// no granulation"). Carrying them into a tip that cannot use them would put
// three dead fields in the deposit path and imply a fidelity that is not
// there. They are not lost: they are still what the pigment *is*, and they
// become live for a Pigment layer when the solver readback bridge lands.
//
// `pressure` in [0,1] scales radius and flow by the same two curves the solver
// route uses, and honours `BrushState::pressureSize` / `pressureFlow`
// independently, so a pen configured one way behaves the same on both routes.
//
// Falls back to the pigment's own RGB projected through `latentToRgb()`'s
// inverse-free path when `lut` has not loaded -- a build with no LUT still
// paints, in the pigment's colour, rather than painting nothing.
BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut, float pressure);

// The same, against the WHOLE source set rather than pressure alone -- what a
// stroke that has tilt, azimuth, barrel and its own derived sources to hand
// should call. The scalar form above delegates here with everything but
// pressure left at its default, which is exactly a mouse.
BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut,
                     const DynamicInputs& inputs);

// This frame's live source sample, read off AppState.
//
// **Four of the eight are still at their defaults**, and deliberately so:
// VELOCITY, FADE, NOISE and RANDOM are derived from a stroke in progress
// rather than read off hardware, so they belong to whoever owns the dab loop,
// not to a per-frame snapshot of input state. The DYNAMICS matrix draws their
// rows anyway -- an empty cell being as informative as a filled one is the
// whole premise of that panel -- and its live gutter shows them holding
// still, which is the truth about them today.
//
// Pressure falls back to 1.0 when no pen has ever been seen, so a mouse
// paints at full strength rather than at whatever `penPressure` last held.
DynamicInputs dynamicInputsFor(const AppState& st) noexcept;

// --- The brush library, against the live brush ------------------------------
//
// These three live here rather than in brush/Library.hpp because they need
// `BrushState`, which is app/AppState.hpp's -- and AppState already includes
// the library for its member, so the dependency only runs one way.

// Load a preset into the live brush. Leaves the loaded pigment and the
// selected tool alone: a preset holds neither (brush/Library.hpp), so picking
// a brush must not repaint in another colour or switch tools underneath you.
void applyPresetToBrush(const BrushPreset& preset, BrushState& brush);

// Capture the live brush as a preset under `name`.
BrushPreset presetFromBrush(std::string name, const BrushState& brush);

// Whether the live brush still matches the preset it was picked from -- what
// the editor's EDITED badge shows. False when `active` is out of range, since
// a brush picked from nothing cannot have drifted from it.
bool brushIsEdited(const BrushState& brush);

// One stroke, from pen-down to pen-up.
//
// Deliberately shaped like the block in `ui/MacPaintUI.cpp` that already feeds
// `StrokePath` -- `begin()` where `strokePath.reset()` is, `addPoint()` where
// `strokePath.addPoint()` is, `end()` where `strokePath.flush()` is -- so
// wiring the pen is a routing branch and not a restructure.
class StrokeSession {
 public:
  // Pen-down. Returns false and fills `errorOut` (when non-null) without
  // touching the document if the target cannot be painted: no such layer, a
  // kind with no tile store to write (Adjustment, Media, Text, ...), a Pigment
  // or RGB layer whose store was never allocated, or a locked layer. `doc` must
  // outlive the stroke.
  //
  // **Which of the two deposit routes runs is decided here, once**, by
  // `strokeRouteFor()` and not by a second reading of the layer -- so the
  // session cannot start on one kind and continue on another, and the
  // per-frame re-validation in §5 is a comparison against this answer rather
  // than a fresh decision.
  //
  // For an RGB target this also latches the ink: `tip.linearRgb` and
  // `tip.opacity` are read once, here, because brush/RgbDeposit.hpp §2's
  // accumulator is only exact against a colour and a ceiling that hold still
  // for the whole stroke. `setTip()` below may still change radius, hardness,
  // spacing and flow mid-stroke; it deliberately does not change those two.
  //
  // Records no history entry and does not move the revision -- §2.
  bool begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
             std::string* errorOut);

  // Which of §1's two layer-writing routes this stroke took. Meaningless before
  // `begin()` succeeds.
  StrokeRoute route() const noexcept { return route_; }

  bool active() const noexcept { return doc_ != nullptr; }

  // Replace the tip mid-stroke, which is how **pressure** reaches a CPU
  // deposit.
  //
  // Frame granularity, deliberately, and it is parity rather than a
  // compromise: `ui/MacPaintUI`'s solver route sets one `sim.brushRadius` per
  // frame from the current pen pressure and every dab that frame shares it.
  // This is the same rule on the same schedule -- the UI rebuilds the tip from
  // this frame's pressure and calls this before `addPoint()`. Within a frame a
  // batch of dabs shares one tip, in both routes.
  //
  // `StrokePath` already takes its spacing per call, so a tip whose radius
  // changed also changes the spacing from that point on rather than keeping
  // pen-down's -- which is what "spacing is in radii" has to mean for a
  // pressure-sized brush.
  void setTip(const BrushTip& tip) noexcept { tip_ = tip; }
  const BrushTip& tip() const noexcept { return tip_; }

  // One raw pointer sample, in document texel coordinates. Deposits whatever
  // dabs `brush/StrokePath` emits for it and returns **this frame's** tile
  // set -- what live feedback must recomposite, sorted (y, x) and unique.
  // The reference is valid until the next call.
  //
  // A no-op returning an empty set when the session is not active, so a UI
  // that calls it on a frame the stroke ended does not have to guard.
  const std::vector<TileCoord>& addPoint(float x, float y);

  // Pen-up. Walks the final segment `addPoint()` always holds back (see
  // `StrokePath::flush()`), deposits it, records **exactly one** history entry
  // when the stroke deposited anything, and ends the session. Returns the
  // whole stroke's tile set, which stays valid after the session ends.
  const std::vector<TileCoord>& end();

  size_t dabCount() const noexcept { return dabs_; }
  size_t texelsWritten() const noexcept { return texels_; }

  // Every tile this stroke has written so far, sorted (y, x) and unique.
  // Mid-stroke this is the union of every frame; after `end()` it is the
  // stroke's own dirty set, which is what a single undo step covers.
  const std::vector<TileCoord>& strokeTiles() const noexcept { return strokeTiles_; }

  const std::string& label() const noexcept { return label_; }

 private:
  void depositPending();

  OpenDocument* doc_ = nullptr;
  size_t layerIndex_ = 0;
  // The layer count at pen-down. See section 5 above on why the target is
  // guarded by a count rather than by `Layer::id`.
  size_t layerCount_ = 0;
  BrushTip tip_{};
  std::string label_;
  // Latched at `begin()`, compared against on every frame. See `begin()`.
  StrokeRoute route_ = StrokeRoute::None;
  // The RGB route's per-stroke alpha accumulator and latched ink
  // (brush/RgbDeposit.hpp §§2-3). Idle -- and holding no tiles -- for a stroke
  // that took the pigment route, which is what `RgbStroke::active()` says.
  RgbStroke rgb_;

  StrokePath path_;
  std::vector<Vec2> pending_;
  std::vector<TileCoord> frameTiles_;
  std::vector<TileCoord> strokeTiles_;
  size_t dabs_ = 0;
  size_t texels_ = 0;
};

}  // namespace np
