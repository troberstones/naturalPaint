#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "brush/Deposit.hpp"
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
//   Brush       Pigment, writable                CpuDeposit  <- new
//   DryBrush    Pigment, writable                CpuDeposit  <- new
//   Brush       Pigment, **locked**              None        <- refused
//   DryBrush    Pigment, **locked**              None        <- refused
//   Brush       RGB / Adjustment / none / ...    PaintSim     (unchanged)
//   DryBrush    RGB / Adjustment / none / ...    PaintSim     (unchanged)
//   Water       anything                         PaintSim     (unchanged)
//   Eyedropper / Hand / Zoom                     None         (unchanged)
//
// Two rows are decisions rather than bookkeeping.
//
// **`Water` never routes here, on any layer.** The water tool deposits water
// and no pigment (`app/AppState`'s own comment on the enumerator). A Pigment
// tile has seven channels and not one of them is water -- `docs/document-
// format.md`'s `pig.c0 pig.c1 pig.c2 pig.m` plus `res.R res.G res.B` -- so a
// CPU deposit of "water" could only mean depositing zero mass, which is
// indistinguishable from not painting. Wetness is a solver state; it belongs
// to the medium that simulates it and it is one of the things the readback
// bridge, not this step, will have to carry into a document.
//
// **A locked Pigment layer refuses rather than falling through to PaintSim.**
// Falling through is the tempting row, because it never blocks the user -- but
// it would put paint on the *solver canvas* when the user aimed at a layer,
// which is the one failure mode a painter cannot see and cannot undo. Refusing
// matches `core/LayerOps`, whose every setter refuses on `Layer::locked`, and
// leaves the UI free to say why. **Visibility is deliberately not a refusal**,
// for the same reason `core/LayerOps` does not refuse on it: hiding a layer is
// a view decision, and `layerCoverage()` already makes a hidden layer
// contribute nothing.
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
// the layer at that index must still route to the CPU deposit (Pigment, with a
// store, unlocked). A stroke whose target has gone away drops its remaining
// dabs rather than writing anywhere, which `--selftest` exercises by deleting
// the target layer mid-stroke.
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
  PaintSim,    // sim::PaintSim's dense canvas texture, exactly as before
};

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

// One stroke, from pen-down to pen-up.
//
// Deliberately shaped like the block in `ui/MacPaintUI.cpp` that already feeds
// `StrokePath` -- `begin()` where `strokePath.reset()` is, `addPoint()` where
// `strokePath.addPoint()` is, `end()` where `strokePath.flush()` is -- so
// wiring the pen is a routing branch and not a restructure.
class StrokeSession {
 public:
  // Pen-down. Returns false and fills `errorOut` (when non-null) without
  // touching the document if the target cannot be painted: no such layer, not
  // a Pigment layer, no pigment store, or locked. `doc` must outlive the
  // stroke.
  //
  // Records no history entry and does not move the revision -- §2.
  bool begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
             std::string* errorOut);

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

  StrokePath path_;
  std::vector<Vec2> pending_;
  std::vector<TileCoord> frameTiles_;
  std::vector<TileCoord> strokeTiles_;
  size_t dabs_ = 0;
  size_t texels_ = 0;
};

}  // namespace np
