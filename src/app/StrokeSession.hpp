#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "brush/BrushModel.hpp"
#include "brush/Deposit.hpp"
#include "brush/PigmentErase.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/RgbErase.hpp"
#include "brush/StrokePath.hpp"
#include "brush/Variance.hpp"
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
//   Brush       RGB, with tiles, writable        RgbDeposit
//   DryBrush    RGB, with tiles, writable        RgbDeposit
//   Eraser      RGB, with tiles, writable        RgbErase
//   Eraser      Pigment, with tiles, writable    PigmentErase <- new; this row
//                                                               used to be a
//                                                               refusal by name
//   Eraser      **no target at all**             None        <- and NOT
//                                                               PaintSim
//   Brush       **any layer, locked**            None
//   DryBrush    **any layer, locked**            None
//   Eraser      **any layer, locked**            None
//   Brush       Adjustment / Media / Text / ...  None
//   DryBrush    Adjustment / Media / Text / ...  None
//   Eraser      Adjustment / Media / Text / ...  None
//   Brush       **no target at all**             PaintSim
//   DryBrush    **no target at all**             PaintSim
//   Water       anything                         PaintSim     (unchanged)
//   Eyedropper / Hand / Zoom                     None         (unchanged)
//
// Seven rows are decisions rather than bookkeeping.
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
// **The Eraser rows, and the three of them that are decisions.** PRD F9 and
// F10 are both **P0** and ADR-0007 specifies them: the eraser is the brush with
// a negative deposit step, inheriting the whole dynamics matrix, removing alpha
// on RGB, Mass on Pigment with the Latent left untouched, deposit on Media, dab
// records on Strokes and the mask on the parametric kinds. Until this step
// `Tool::Eraser` sat in the not-built `None` list below and the tool did
// **nothing at all** -- it drew a cursor, it took a keystroke, and no gesture it
// made reached any layer or produced any message. `brush/RgbErase` is the RGB
// row of ADR-0007's table and `StrokeRoute::RgbErase` is how a stroke gets to
// it.
//
//   * **`nullptr` is `None` for the eraser, and this is the one place its rows
//     do not simply follow the brush's.** "No document open" routes a brush to
//     `PaintSim` because watercolour and oil legitimately paint the dense canvas
//     texture -- but `sim::PaintSim` has no alpha and no erase, so an eraser
//     sent there would run the *paint* path with a brush tip and add pigment
//     where the user asked for its removal. That is not a missing feature, it is
//     the tool doing the opposite of its name, and it is exactly the invisible
//     wrong-target failure the locked row exists to prevent.
//
//   * **A Pigment layer takes an erase now, and the refusal that stood here is
//     spent.** This row used to read `None` and the paragraph under it said
//     why: ADR-0007 defines the row completely -- mass is "the Pigment analogue
//     of alpha" (core/Pigment.hpp, docs/document-format.md), scaling it is a
//     valid operation on a latent because mass is linear, and `depositTexel()`'s
//     §1(ii) already handles the zero-mass texel an eraser leaves behind ("a
//     stale hue at zero coverage, which PRD F10's eraser deliberately creates")
//     -- so the arithmetic was never the blocker. **The selection was.**
//     `depositDab(PigmentTileStore&, ...)` took no `Selection` at all, so a
//     Pigment erase built then would have been "the only thing on that layer
//     kind that stopped at the ants, or would destroy paint outside a selection
//     drawn to protect it, with one undo step covering the stroke". The refusal
//     was conditional on one thing and named it.
//
//     `brush/Deposit` §4 is that gate, and both halves landed together as one
//     decision exactly as this paragraph asked: the pigment deposit is bounded
//     by the active selection, and `brush/PigmentErase` is bounded by the same
//     rule in the same shape. `depositDab()` now takes a `Selection*` and
//     `depositPending()` passes `doc_->selection` down all four routes rather
//     than three.
//
//     **The two erase routes stay two modules**, for `brush/RgbErase` §0's
//     reason applied one level up: they share the dab stream, the falloff, the
//     footprint, the tile loop and the accumulator *type*, and differ in what
//     one dab does to one texel -- four premultiplied channels against one
//     straight mass beside an untouched latent -- and in what a texel emptied of
//     paint is allowed to hold (`brush/PigmentErase` §3: at mass 0 a stale hue
//     is well-formed here and malformed there, because one storage is
//     premultiplied and the other is not). A `bool pigment` on `RgbEraseStroke`
//     would put both conventions inside one loop whose §1 argument describes
//     only one of them.
//
//   * **Media, Strokes, Text, Flats and Adjustment refuse for their own
//     reasons**, all of which ADR-0007 states and none of which is built:
//     erasing a Strokes layer deletes dab records rather than pixels
//     (PRD F11, a structural edit), erasing a Media layer removes the dry
//     deposit and not the film or the saturation (which is Blot, PRD F12, P2),
//     and erasing a parametric kind paints its mask, since there are no pixels
//     to remove. They already refuse for having no writable store; naming them
//     here is what keeps that from reading like an accident.
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
// the layer at that index must still route to **the same one** of §1's three
// layer-writing routes it did then, **for the same tool**. A stroke whose target
// has gone away drops its remaining dabs rather than writing anywhere, which
// `--selftest` exercises by deleting the target layer mid-stroke.
//
// Comparing against the latched route rather than merely against "some deposit
// route" is what stops a stroke changing medium under the pen: a Pigment layer
// swapped for an RGB one at the same index and the same count would otherwise
// keep the same session going and start writing RGB texels with a pigment
// tip's latent -- or, worse, RGB texels through an accumulator that was never
// started. Re-asking **with the session's own tool** is the other half of that,
// and it became load-bearing with the eraser: Brush and Eraser give two
// different answers about one unchanged RGB layer, so a re-validation that
// assumed the brush would find every erase stroke's route "changed" on its
// second frame and silently drop the rest of the drag.
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
  None,          // the tool does not paint, or the target refuses the edit
  CpuDeposit,    // brush/Deposit, into the target layer's pigment tiles
  RgbDeposit,    // brush/RgbDeposit, into the target layer's rgb tiles
  RgbErase,      // brush/RgbErase, taking alpha back OUT of the target layer's
                 // rgb tiles -- ADR-0007's RGB row
  PigmentErase,  // brush/PigmentErase, taking MASS back out of the target
                 // layer's pigment tiles and leaving the latent alone --
                 // ADR-0007's Pigment row. The two erase rows are two routes
                 // and not one with a flag, because the storage conventions
                 // differ in what an emptied texel may hold (§1)
  PaintSim,      // sim::PaintSim's dense canvas texture, and only when there is
                 // no document layer to have aimed at -- see §1's last paragraph
};

// The four routes that write a `Layer`, as one predicate, because four call
// sites ask the same question -- `begin()`'s refusal, `depositPending()`'s
// per-frame re-validation, `ui/MacPaintUI`'s canvas branch, and the options
// bar's route indicator, which accents a route that reaches the user's layer
// and greys one that does not -- and a route added later must reach all four or
// reach none. The indicator is the reason this is a predicate and not an
// `== CpuDeposit` at each site: it read "goes to the solver" grey for a live RGB
// stroke for exactly as long as it had its own copy of the test.
//
// **Both erase routes are in here, and "writes" is the right word for them.** A
// route that removes paint still unshares a copy-on-write tile, still moves the
// revision, still dirties tiles for the incremental composite and still owes
// exactly one history entry -- every one of the four call sites wants the same
// answer for it as for a deposit. A predicate that meant "adds paint" would
// leave the options bar greying a live erase as though it went to the solver,
// which is the specific drift this predicate was extracted to stop.
inline bool strokeRouteWritesLayer(StrokeRoute route) noexcept {
  return route == StrokeRoute::CpuDeposit || route == StrokeRoute::RgbDeposit ||
         route == StrokeRoute::RgbErase || route == StrokeRoute::PigmentErase;
}

// Reachability audit B2: `BrushState::wetness` (the WET slider, drawn in both
// `ui/AtelierChrome.cpp`'s options bar and `ui/MacPaintUI.cpp`'s BRUSH panel)
// reaches exactly one place -- `applyToolToBrush()`'s write to
// `sim::PaintSim::brushWater`, called only on the route that paints the
// solver canvas rather than a document layer. Extracted the same way
// `strokeRouteWritesLayer()` above was: two UI call sites asked "does WET do
// anything right now" with their own copy of `route == StrokeRoute::PaintSim`
// before this existed, which is exactly the shape of drift that predicate's
// own comment warns about -- a third call site (this one's test) is what
// makes the duplication worth closing before it happens again rather than
// after.
inline bool wetnessReachesSolver(StrokeRoute route) noexcept {
  return route == StrokeRoute::PaintSim;
}

// Does the PAPER GRAIN group do anything on this route?
//
// **This predicate exists because the answer to it was already wrong once.**
// `brush/Grain`'s `grainCoverageAt()` was called from `depositDab()` alone,
// so the BRUSH panel greyed the whole group out on every other route and said
// why. The texture work then added the call to `brush/RgbDeposit.cpp`,
// `brush/RgbErase.cpp` and `brush/PigmentErase.cpp` -- and the UI's private
// copy of the old answer stayed behind, leaving a working control greyed out
// over a sentence explaining that it could not work. An RGB layer is what
// File > New gives you, so that was most strokes.
//
// It delegates to `strokeRouteWritesLayer()` because the two agree today:
// grain modifies a CPU-computed coverage, and every route that computes one
// writes a layer. **It is a separate name rather than a call to that
// predicate at the UI site, because it is a separate claim** -- a fifth
// layer-writing route added without a `grainCoverageAt()` call would make
// `strokeRouteWritesLayer()` still correct and this still wrong, and there
// would again be nothing to notice. `app/selftest/StrokeSession.cpp` asserts
// the agreement route by route, so adding a route means answering the
// question for grain rather than inheriting an answer.
//
// The solver route is the real exclusion and keeps the group honest: it has
// no CPU coverage for grain to modify at all (brush/BrushModel.hpp on the
// editor-versus-solver divergence generally).
inline bool grainReachesRoute(StrokeRoute route) noexcept {
  return strokeRouteWritesLayer(route);
}

const char* strokeRouteName(StrokeRoute route) noexcept;

// `target` is the layer the stroke is aimed at, or nullptr when there is none.
// Pure and total: every (tool, target) pair has an answer and §1's table is
// the whole of it.
StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept;

// The history label for a stroke made with `tool`, in the same noun form
// `core/LayerOps`' `editLabel` uses ("duplicate", not "Duplicated") so PRD
// O2's panel reads consistently down the column.
//
// **An erase is labelled "erase", not "brush stroke"**, and that is a
// requirement rather than a nicety: PRD O2's panel is a list of nouns a user
// scans to find the edit they want back, and a column of identical "brush
// stroke" rows in which some of them actually took paint off is a panel that
// cannot be read. It also names the route honestly at the one place a route
// name survives the session -- `ui/MacPaintUI`'s pen-up line prints the label
// and the route together.
const char* strokeEditLabel(Tool tool) noexcept;

// ==========================================================================
// 6. The pixel-writing ops that are NOT strokes -- the bucket and the gradient
// ==========================================================================
//
// **Why this lives in the stroke file.** It does not describe a stroke, and on
// that reading it does not belong here. It is here anyway because §1 exists to
// stop *one* question -- "can this tool put colour on this layer?" -- being
// answered in more than one place, and the paint bucket and the gradient ask
// exactly that question about exactly the same `Layer` members. Splitting them
// into a second header would recreate the drift §1's own comment describes
// ("the options bar's route indicator read 'goes to the solver' grey for a live
// RGB stroke for exactly as long as it had its own copy of the test"), only in
// a file nobody would think to look at when adding a third fill tool.
//
// **They are not rows in `strokeRouteFor()`'s table**, and that is deliberate
// rather than an omission to be corrected later. That table answers where a
// *stroke* deposits, and both of these tools are listed there as `None` because
// neither begins a stroke -- `StrokeSession::begin()` must go on refusing them.
// The two questions merely rhyme; folding them into one enum would make
// `StrokeRoute::None` mean "no stroke route" in one caller and "cannot be
// filled" in another.
//
// **RGB only, and the reason is ops/FloodFill's, not this file's.** Both ops
// take a `core::TileStore` and write straight linear RGBA into it. A Pigment
// layer stores latents premultiplied by mass (core/Pigment.hpp) and
// ops/FloodFill.hpp §4 states outright that it does not sample a
// `PigmentTileStore`, because "similar colour" between two latents is a
// question about Kubelka-Munk space that nothing in this build has decided.
// Filling one with a straight RGBA would be writing the wrong *kind* of value
// into it -- not a slightly wrong colour, a meaningless one.
//
// **The defect this closes.** `ui/MacPaintUI.cpp`'s canvas block used to spell
// this predicate inline as one `usable` bool and put it *inside the click
// condition*, so a bucket click on a Pigment layer -- which is the kind
// `CONTEXT.md` makes the default for a new layer, and the first entry in the
// LAYERS panel's own NEW popup -- evaluated to false and the click was
// discarded with no message, no history entry and no mark on the canvas. That
// is the same invisible wrong-target failure §1's last paragraph was written
// about, arriving through the one tool in the build that had not been given a
// refusal. The brush had had one since the RGB route landed; the bucket and the
// gradient had not.
enum class PixelOpRefusal {
  None,        // the layer can take the fill
  NoLayer,     // no document, or a document with no layer to have aimed at
  Locked,      // the layer is locked -- the one of the three a user can fix
  NoRgbStore,  // the kind holds no RGB tiles, or its store was never allocated
};

// Whether `tool` is one of the pixel-writing ops this section covers -- the
// paint bucket and the gradient, and nothing else.
//
// A predicate rather than the `tool == A || tool == B` the canvas block and the
// options bar would each otherwise spell, for `strokeRouteWritesLayer()`'s
// stated reason: a third fill tool must reach both call sites or neither.
bool toolWritesRgbPixels(Tool tool) noexcept;

// Why a fill cannot write `target`, or `None` when it can. `nullptr` is a legal
// argument and means "there is no target", which is its own answer rather than
// an error.
//
// **Locked is tested before storage**, exactly as `strokeRouteFor()` orders its
// own two refusals and for the same reason: a locked RGB layer must refuse for
// being locked, so the message a user gets names the one problem they can
// actually carry out a fix for.
PixelOpRefusal pixelOpRefusalFor(const Layer* target) noexcept;

// The same answer as a bool, for the call sites that only need the gate --
// `strokeRouteWritesLayer()`'s counterpart, and named to rhyme with it.
inline bool pixelOpWritesLayer(const Layer* target) noexcept {
  return pixelOpRefusalFor(target) == PixelOpRefusal::None;
}

// ==========================================================================
// 6b. Which tools the canvas actually listens to
// ==========================================================================
//
// **These four predicates exist because a hand-maintained boolean shipped a
// lie for two whole phases.** `ui/AtelierChrome`'s `kToolMeta` carries an
// `implemented` flag; `Tool::Eyedropper` had it set to `true` with **no canvas
// handler anywhere**, and nothing in the build could tell. The palette made
// the cell clickable and highlighted it *because* of the flag;
// `toolCursorOnTarget()` withheld the `Refuse` cursor *because* of the flag and
// handed out a bespoke `ToolCursor::Sample` pointer; and then the click landed
// in `ui/MacPaintUI.cpp`'s canvas block and nothing consumed it. Every tier of
// the chrome said live except the one that acts.
//
// The fix is not a second hand-written table saying which tools have handlers
// -- that could drift in exactly the same way. **Each of these predicates is
// the literal gate the corresponding block in the canvas is written with**, so
// a tool that stops being handled stops passing the predicate, and the
// completeness check in `ui/AtelierChrome` (`toolHasCanvasHandler()`) reddens.
// Two of the five gates already existed and are reused unchanged:
// `strokeRouteFor()` and `toolWritesRgbPixels()` above.

// Whether `tool` can begin a stroke on *anything*.
//
// Asked of `strokeRouteFor()` itself, against the two layer kinds §1 says can
// take one plus the no-target case, rather than restated as a second table --
// so a tool whose row in that table changes cannot disagree with this. Not
// `noexcept`: the two probe Layers it builds hold a `std::string` and an
// `optional<TileStore>`.
bool toolBeginsStroke(Tool tool);

// Whether `tool` builds a `Selection` by gesture: the five of PRD E3.
//
// This was an inline `selectionTool` bool inside the canvas block, and it is
// out here so the completeness check reads the same expression the handler is
// gated on rather than a copy of it.
bool toolDrawsSelection(Tool tool) noexcept;

// Whether `tool` reads colour off the canvas rather than writing it: the
// eyedropper, and today nothing else. `Tool::Measure` shares its palette group
// and its `ToolCursor::Sample` cursor but has no handler and is not
// `toolImplemented()`, so it is correctly false here.
bool toolSamplesCanvas(Tool tool) noexcept;

// Whether `tool` moves the view by dragging on the canvas: the hand, and today
// nothing else.
//
// **Not `Tool::Zoom`.** Zoom is `toolImplemented() == true` and has a bespoke
// `ToolCursor::Zoom`, and zooming works only from the scroll wheel and the View
// menu -- both of which are tool-independent and fire whatever tool is
// selected. Selecting the Zoom tool and clicking the canvas does nothing at
// all. That is the same defect the eyedropper had, still live, and
// `ui/AtelierChrome`'s completeness check records it as a named exception
// rather than letting it look like an accident.
bool toolPansView(Tool tool) noexcept;

// The sentence the options bar shows, in the same shape and the same voice as
// `ui/MacPaintUI.cpp`'s stroke refusals: what is wrong, which layer it is wrong
// about **by name**, and -- only when there is one -- what to do about it.
//
// `opName` is the op in the same noun form the history entry uses ("paint
// bucket", "gradient"), so a refusal and the entry it did not create name the
// same thing. Passed in rather than switched on a `Tool` here because
// `toolName()` is `ui/AtelierChrome`'s and `app/` does not include `ui/`.
//
// **The three reasons produce three visibly different sentences**, which is a
// requirement and not a nicety. "Locked" and "no RGB store" both present to a
// user as "the bucket did nothing", and only the first has a switch in LAYERS
// that fixes it; telling someone to clear a lock they never set is worse than
// telling them nothing at all. `--selftest` asserts the two are distinguishable
// rather than merely non-empty.
//
// Empty for `PixelOpRefusal::None` -- there is nothing to say when it worked.
std::string pixelOpRefusalMessage(PixelOpRefusal reason, const Layer* target,
                                  const char* opName);

// ==========================================================================
// 7. The foreground colour (PRD Q10, PRD L4)
// ==========================================================================
//
// The foreground colour, in **display-referred sRGB** -- whichever of
// `BrushState`'s two colour representations `BrushState::colorMode` currently
// selects.
//
// This is the whole of the union, and it is one function rather than a
// conditional at each call site for the reason this codebase keeps
// rediscovering: four places derive a colour from the brush (`brushTipFor()`'s
// `linearRgb`, `brushTipFor()`'s `Latent`, `main.cpp`'s solver uniform,
// `ui/MacPaintUI`'s `foregroundLinearRgba()`), and a fifth representation
// arriving with only three of them updated is how a build ends up painting one
// colour and filling another.
//
// **sRGB and not linear**, matching `paint::Pigment::rgb` and
// `BrushState::rgb` -- see that field's comment for why the encoding is the
// dangerous part. Callers that write a document part decode; callers that draw
// a swatch or feed `MixboxLut` do not.
//
// An out-of-range `pigment` index in PIGMENT mode yields black, the same answer
// `foregroundLinearRgba()` gives, rather than reading past the palette.
std::array<float, 3> foregroundSrgb(const BrushState& brush) noexcept;

// The name to show for the foreground colour: the pigment's own name in
// PIGMENT mode, or "Custom RGB" in RGB mode.
//
// A pigment has a name and an arbitrary triple does not, and a swatch tooltip
// that went on saying "Ultramarine Blue" after the eyedropper picked a grey off
// a photograph would be the chrome lying about what the next stroke will lay
// down -- the exact failure `strokeRouteFor()`'s options-bar indicator exists
// to prevent, one control over.
const char* foregroundName(const BrushState& brush) noexcept;

// **The three physical constants always follow `BrushState::pigment`, in both
// modes**, and this is the one honest asymmetry in the design above.
//
// A `Latent` is a colour and can be derived from any RGB triple; density,
// staining and granulation cannot. They are measurements of a real paint
// (`paint/Palette.cpp`, Curtis et al. 1997 Table 1), and there is no function
// from three floats to "how does this settle out of suspension". So an RGB
// foreground changes what colour the solver deposits and leaves *how it
// behaves* at whatever pigment is selected -- which is a real limitation and is
// why the COLOR panel says it in words rather than leaving it to be discovered
// by a wash that granulates unexpectedly.
//
// The alternative, snapping a picked colour to the nearest palette pigment so
// the constants always match, was rejected: an eyedropper exists to reproduce a
// colour exactly, and one that silently answered "Burnt Sienna" to a sampled
// #7f3f00 would be wrong in the one way the tool must never be wrong.
const Pigment& foregroundPhysicalConstants(const BrushState& brush) noexcept;

// The pen's brush state, as a tip -- the one mapping from what the UI holds to
// what `brush/Deposit` takes, so the interactive route and `--selftest` cannot
// disagree about what a given brush deposits.
//
// **What the COLOR panel's foreground means as a `Latent`**, which is the second
// half of the missing decision section 4 named. In PIGMENT mode the foreground
// is a `paint::Pigment` with an sRGB triple and three physical constants, and
// the answer here is deliberately the narrow one: the colour goes through
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
// Falls back to the foreground's own RGB projected through `latentToRgb()`'s
// inverse-free path when `lut` has not loaded -- a build with no LUT still
// paints, in the foreground's colour, rather than painting nothing.
//
// **An RGB foreground reaches a stroke through here, on both layer kinds.**
// `tip.linearRgb` is `srgbDecode(foregroundSrgb(brush))`, which is what
// `brush/RgbDeposit` writes, and `tip.pigment` is
// `rgbToLatent(foregroundSrgb(brush))`, which is what `brush/Deposit` writes --
// so a picked colour paints an RGB layer exactly and a Pigment layer through
// the RGB->latent map docs/ui.md §3.3 explicitly permits ("it maps through
// RGB->latent, with the caveat ... that the decomposition is plausible rather
// than true"). Neither path needed a new branch: they already went through one
// sRGB triple, and the change is only *which* triple.
BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut, float pressure);

// The same, against the WHOLE source set rather than pressure alone -- what a
// stroke that has tilt, azimuth, barrel and its own derived sources to hand
// should call. The scalar form above delegates here with everything but
// pressure left at its default, which is exactly a mouse.
BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut,
                     const DynamicInputs& inputs);

// This frame's live HARDWARE source sample, read off AppState.
//
// **Six of the ten are still at their struct defaults here, and that is
// no longer a gap.** VELOCITY, FADE, NOISE, RANDOM, DIRECTION and INITIAL
// DIRECTION are properties of a stroke in progress -- how fast it moved, how
// far it has travelled, a value that should drift smoothly or be redrawn
// fresh, which way it is currently heading, which way it was originally
// headed -- not of a pen sampled once per render frame before a stroke's
// geometry even exists. `AppState` has nowhere a "this frame's raw pointer
// position" could live before this function runs (the canvas block's local
// `tx, ty` are not written back to `AppState` until after it), so this
// function structurally cannot resolve them, and does not try to. They are
// resolved once per DAB instead, inside `StrokeSession`'s own deposit loop,
// from data the session already owns: consecutive dab positions for
// VELOCITY and DIRECTION (INITIAL DIRECTION reads the same step vector
// DIRECTION does, but only once -- see brush/Dynamics.hpp's own section),
// cumulative arc length for FADE, and the stroke's own seed for NOISE and
// RANDOM (`brush/Dynamics.hpp`'s `dynamicVelocity()`, `dynamicFade()`,
// `dynamicNoiseAt()`, `dynamicRandomDraw()`, `dynamicDirection()`).
//
// The 0.0 this function still hands back for all six is therefore a
// truthful idle reading, not a placeholder -- "not moving" (Velocity),
// "just started" (Fade) and "at the seed's own resting sample" (Noise) are
// exactly what 0.0 means, the same way Pressure's 1.0 fallback and Tilt's 0.0
// are the truthful readings for "no pen has ever reported in." DIRECTION's
// and INITIAL DIRECTION's 0.0 are the same idle reading one step further --
// "no heading yet" / "no heading LATCHED yet" -- the same answer a stroke
// that has not moved gets from VELOCITY, since a direction only exists once
// two dab positions do. RANDOM alone has no such resting value (see
// `sourceDisplay()`'s em-dash treatment of it), which is a property of
// RANDOM, not of this function.
//
// Pressure falls back to 1.0 when no pen has ever been seen, so a mouse
// paints at full strength rather than at whatever `penPressure` last held.
DynamicInputs dynamicInputsFor(const AppState& st) noexcept;

// SCATTER's own axis (reachability audit B5). `centre` is the dab's
// pre-scatter position; `seed`/`dabIndex` are the stroke's own per-dab draw,
// identically to every other stroke-local source; `stepDx`/`stepDy` is the
// SAME step vector `depositPending()` already computes for DIRECTION --
// `(p - prevDab)`, zero on the stroke's first dab. `tip.scatter` is the
// resolved magnitude, in radii (`BrushTip::scatter`'s own comment); the
// identity (`tip.scatter == 0.0f`) returns `centre` unchanged and spends no
// draw. See `app/StrokeSession.cpp`'s own definition for the geometry:
// perpendicular-to-the-stroke by default (`tip.scatterBothAxes == false`,
// Photoshop's own default), full-circle isotropic when it is set.
Vec2 applyPerDabScatter(Vec2 centre, const BrushTip& tip, uint64_t seed, uint32_t dabIndex,
                        float stepDx, float stepDy) noexcept;

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
  // **Which of the three layer-writing routes runs is decided here, once**, by
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
  // For **either** erase it latches `tip.opacity` as the **strength**, the same
  // slider and the same units (brush/RgbErase.hpp §2, brush/PigmentErase.hpp
  // §2), for the identical reason: a stroke whose floor moved half way through
  // has no well-defined floor. Neither the ink nor the loaded latent is read at
  // all -- an eraser that reached for a colour would be a brush painting the
  // background, which ADR-0007 exists to reject, and on a Pigment layer would
  // deposit white, which under Kubelka-Munk is opaque paint.
  //
  // **The tool is latched too**, and that is not bookkeeping: §5's per-frame
  // re-validation asks `strokeRouteFor()` the same question again, and the
  // answer depends on the tool as well as the layer now that Brush and Eraser
  // give different answers about the same RGB layer. Re-asking with a stand-in
  // tool would make every erase stroke drop its dabs from the second frame on.
  //
  // Records no history entry and does not move the revision -- §2.
  //
  // `model`, latched alongside the tool and the route: the brush's own
  // Photoshop-shaped model (brush/BrushModel.hpp), read again per DAB inside
  // the deposit loop to resolve Size/Angle/Roundness/Scatter through
  // `brush/Variance`'s `varianceScale()`/`varianceOffset()` -- each exactly
  // ONCE per dab per site, which is the whole invariant `brush/Variance.hpp`
  // exists to make structurally true (its floor is applied once, inside its
  // own formula; a stroke-begin base times a second per-dab correction would
  // apply it twice, reintroducing audit B6 in a new shape). This replaces the
  // old `BrushLinkSet* strokeLocalLinks` parameter -- the matrix is shelved
  // (`ui/DynamicsMatrixPanel.hpp`) and nothing in the paint path reads
  // `BrushState::links`/`BrushPreset::links` any more.
  //
  // **Defaulted to `nullptr` so a caller that only has a bare `BrushTip`
  // compiles unchanged** -- a session begun without a model simply gets
  // `tip_` unmodified at every dab, exactly the old null-`strokeLocalLinks`
  // identity path this replaces. `app/selftest/VarianceConsumption.cpp` is
  // what exercises the non-null path against a real stroke.
  //
  // `hardwareInputs`, latched with it: the Pressure/Tilt/Azimuth/Barrel
  // sample (and its `has*` availability flags) that built `tip` -- what the
  // per-dab loop feeds `varianceScale()`/`varianceOffset()` so a
  // PenPressure/PenTilt/Rotation Control still reads the pen, at the SAME
  // frame granularity `dynamicInputsFor()`'s own header describes (this
  // codebase does not resample pressure per dab, and this does not change
  // that). Defaults to a plain `DynamicInputs{}` -- a mouse at full pressure,
  // which is the neutral reading for every caller that does not care.
  bool begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
             std::string* errorOut, const BrushModel* model = nullptr,
             const DynamicInputs& hardwareInputs = DynamicInputs{});

  // Which of §1's four layer-writing routes this stroke took. Meaningless before
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
  //
  // `hardwareInputs`, replaced alongside it -- `begin()`'s own comment on the
  // identical parameter. Defaults to `DynamicInputs{}` so a caller that only
  // ever passes a bare tip (this codebase's other three, non-interactive
  // `setTip()` call sites) keeps reading a neutral mouse sample, which is
  // what they were already getting before this parameter existed.
  void setTip(const BrushTip& tip, const DynamicInputs& hardwareInputs = DynamicInputs{}) noexcept {
    tip_ = tip;
    hardwareInputs_ = hardwareInputs;
  }
  const BrushTip& tip() const noexcept { return tip_; }

  // PRESSURE SMOOTHING (brush/Dynamics.hpp's `dynamicPressureEma()`, from
  // PaintCopilot §3.2): this stroke's own exponential moving average over
  // the once-per-FRAME raw pressure sample, before it drives `brushTipFor()`.
  //
  // **This is the state `dynamicPressureEma()`'s own header comment says
  // belongs to a stroke's owner, not to that pure function.** The caller
  // (`ui/MacPaintUI.cpp`'s canvas block, `app/BrushSheet.cpp`'s per-sample
  // loop) calls this once per frame/sample with the RAW pressure it would
  // otherwise have fed `brushTipFor()` directly, and feeds `brushTipFor()`
  // the SMOOTHED result this returns instead.
  //
  // The first call after `begin()` returns `rawPressure` unchanged -- there
  // is no previous smoothed value to blend from yet, and manufacturing one
  // (starting the filter at 0, say) would make every stroke's opening dabs
  // fade in from nothing regardless of how hard the stroke started, which
  // is a soft-start artefact the paper's jitter filter was never meant to
  // add. Every call after the first is the plain recursion.
  //
  // **Must be called AFTER `begin()`, never before, on a stroke's first
  // painting frame.** `begin()` resets the latch below; a caller that reads
  // this frame's smoothed pressure before calling `begin()` (to build the
  // very tip `begin()` itself is handed) would still be blending against
  // the PREVIOUS stroke's last smoothed value, which is exactly the
  // cross-stroke leak the design brief for this feature calls out as "the
  // reset...least likely to be noticed." `ui/MacPaintUI.cpp`'s canvas block
  // avoids this by building `begin()`'s own bootstrap tip from the raw
  // sample and only calling `smoothPressure()` afterwards, once
  // `g_stroke.active()` is true -- the same tip is then rebuilt and handed
  // to `setTip()` on that same frame, so the bootstrap tip is live for zero
  // frames, never painted.
  float smoothPressure(float rawPressure) noexcept;

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

  // **The most recently deposited dab's own radius, AFTER `BrushTip::
  // sizeFloorPx` has been applied** -- `tip()` above is deliberately not
  // this: it returns `tip_`, the tip as `setTip()`/`begin()` left it, which
  // is only the HARDWARE half of a Multiply product on Size and is never
  // floored (`brush/Deposit.hpp`'s own comment on why). A dab's actual
  // radius also folds in whatever stroke-local correction
  // `depositPending()`'s own per-dab loop resolved, and floors the RESULT
  // of that -- a number `tip_`/`tip()` alone cannot answer, and the number
  // `docs/reachability-audit.md` **B6** is actually about.
  //
  // Exists for exactly one reason: `applyStrokeLocalCorrection()` has
  // internal linkage (StrokeSession.cpp's own anonymous namespace) and
  // `depositPending()` is private, so nothing outside this class can observe
  // the per-dab product `app/selftest/MultiplyFloor.cpp` needs to check
  // without painting a stroke and measuring its footprint back out of tile
  // data -- imprecise, and a test of `dabCoverage()`'s falloff shape as much
  // as of the floor. This is the direct number instead, the same discipline
  // `dabCount()`/`texelsWritten()` above already apply to two other
  // internals a caller has no other way to see. 0.0f before any dab has
  // ever been deposited by this session.
  float lastDabRadius() const noexcept { return lastDabRadius_; }

 private:
  void depositPending();

  OpenDocument* doc_ = nullptr;
  size_t layerIndex_ = 0;
  // The layer count at pen-down. See section 5 above on why the target is
  // guarded by a count rather than by `Layer::id`.
  size_t layerCount_ = 0;
  BrushTip tip_{};
  // See `lastDabRadius()`'s own comment above -- written once per dab, at the
  // end of `depositPending()`'s per-dab loop, after the floor has been
  // applied.
  float lastDabRadius_ = 0.0f;
  std::string label_;
  // Latched at `begin()`, compared against on every frame. See `begin()`.
  StrokeRoute route_ = StrokeRoute::None;
  // Latched with it, and re-asked with rather than assumed -- §5's last
  // paragraph on why a stand-in tool stopped being good enough at the eraser.
  Tool tool_ = Tool::Brush;
  // The RGB deposit route's per-stroke alpha accumulator and latched ink
  // (brush/RgbDeposit.hpp §§2-3). Idle -- and holding no tiles -- for a stroke
  // that took the pigment or the erase route, which is what
  // `RgbStroke::active()` says.
  RgbStroke rgb_;
  // The RGB erase route's per-stroke *erasure* accumulator and latched strength
  // (brush/RgbErase.hpp §2). A separate object rather than a mode on `rgb_`
  // because the two accumulators count different quantities -- alpha added
  // against fraction removed -- and exactly one of them is ever live, which is
  // an invariant two members make checkable and one member with a flag hides.
  RgbEraseStroke erase_;
  // The Pigment erase route's, likewise (brush/PigmentErase.hpp §2). A third
  // member rather than a shared one for the same reason there are two erase
  // modules: `E` means the same *number* on both routes, but the loop that
  // consumes it reads a different store and writes a different texel type, and
  // a session holding one accumulator would need a tag to say which loop last
  // touched it. Idle objects cost 24 bytes each and hold no tiles --
  // `accumulatorTiles()` is 0 -- which `--selftest` measures rather than
  // assumes.
  PigmentEraseStroke pigErase_;

  StrokePath path_;
  std::vector<Vec2> pending_;
  std::vector<TileCoord> frameTiles_;
  std::vector<TileCoord> strokeTiles_;
  size_t dabs_ = 0;
  size_t texels_ = 0;

  // --- VELOCITY, FADE, NOISE, RANDOM, DIRECTION, INITIAL DIRECTION's own
  //     per-stroke state ------------------------------------------------
  //
  // Latched/reset at `begin()`, updated once per dab inside `depositPending()`
  // -- never inside `addPoint()` directly, because `addPoint()` can hand
  // `path_` several samples that resolve to zero, one or several dabs, and
  // these six are stroke-DAB-local, not stroke-SAMPLE-local (brush/
  // Dynamics.hpp's own section comment on why RANDOM must be a fresh draw per
  // dab and not per input event).

  // Whether `begin()`'s caller passed a `BrushModel` -- see `begin()`'s own
  // comment. False leaves every dab of this stroke reading `tip_` unmodified,
  // exactly the old null-`strokeLocalLinks_` identity path this replaced.
  bool haveModel_ = false;

  // Photoshop's own base Size/Angle/Roundness (`PsTipShape::diameterPx`/
  // `angleDeg`/`roundness`) and their Variance objects (`PsShapeDynamics`),
  // plus Scatter's own magnitude Variance (`PsScatter::scatter`) -- copied out
  // of the brush's `BrushModel` at `begin()` rather than kept as a pointer
  // into it, because nothing guarantees the `BrushState`/`BrushPreset` `begin()`
  // was called with outlives the stroke.
  //
  // **Resolved ENTIRELY inside the per-dab loop in `depositPending()`, in ONE
  // call each to `varianceScale()`/`varianceOffset()` per dab -- never a
  // stroke-begin base times a separate per-dab correction.** That is
  // `brush/Variance.hpp`'s own load-bearing invariant: its floor is applied
  // once, inside the formula, so two calls composed onto one product would
  // apply the floor twice and reintroduce audit B6 in a new shape.
  float baseDiameterPx_ = 0.0f;
  float baseAngleDeg_ = 0.0f;
  float baseRoundness_ = 1.0f;
  Variance sizeVariance_;
  Variance angleVariance_;
  Variance roundnessVariance_;
  Variance scatterVariance_;

  // The hardware sample `begin()`/`setTip()` latched -- see either's own
  // comment. Read by the per-dab loop below to resolve a
  // PenPressure/PenTilt/Rotation Control, alongside the six stroke-local
  // signals (Velocity, Fade, Noise, Random, Direction, Initial Direction)
  // that loop already computes fresh every dab.
  DynamicInputs hardwareInputs_;

  // The stroke's seed (brush/Dynamics.hpp's `strokeSeedFromStart()`), latched
  // from the FIRST dab position this stroke deposits -- not at `begin()`,
  // which has no position to hand it yet.
  uint64_t seed_ = 0;
  bool seedLatched_ = false;

  // `smoothPressure()`'s own state, the `seed_`/`seedLatched_` shape applied
  // to a smoothed VALUE instead of an identity -- see that method's own
  // comment. Reset at `begin()`, exactly like `seed_` above, so a new stroke
  // never blends against the previous one's last reading.
  float smoothedPressure_ = 0.0f;
  bool pressureSmoothLatched_ = false;

  // The previous dab's position, for VELOCITY's step distance and DIRECTION's
  // (and INITIAL DIRECTION's) step vector (the same `(p - prevDab)`
  // difference, kept as components instead of collapsed to a length), and
  // whether one exists yet -- false only before the stroke's first dab,
  // which is `dynamicVelocity()`'s and `dynamicDirection()`'s shared "no
  // previous position" case.
  float prevDabX_ = 0.0f, prevDabY_ = 0.0f;
  bool havePrevDab_ = false;

  // Cumulative arc length since the stroke's first dab, for FADE and for
  // NOISE's lattice query. Distance between DABS, not between raw pointer
  // samples -- ADR-0003's own distance-not-events rule applied to this
  // measurement too, since a ramp measured in raw samples would run at a
  // different physical length depending on the render frame rate.
  float distanceTravelled_ = 0.0f;

  // INITIAL DIRECTION's own latch, the `seed_`/`seedLatched_` shape applied
  // to a resolved VALUE instead of an identity -- brush/Dynamics.hpp's
  // "INITIAL DIRECTION" section is the argument; this is where it lives.
  // Latched one dab LATER than `seed_` (at the first dab `havePrevDab_` is
  // true for, not the stroke's very first dab), because unlike a stroke's
  // start position -- always available, even for a single-dab stroke -- a
  // HEADING needs two positions, and the first dab alone never has a second
  // one to difference against.
  float initialDirection_ = 0.0f;
  bool initialDirectionLatched_ = false;
};

}  // namespace np
