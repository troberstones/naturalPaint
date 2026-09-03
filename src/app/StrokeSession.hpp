#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "brush/BrushModel.hpp"
#include "brush/CloneStamp.hpp"
#include "brush/Deposit.hpp"
#include "brush/MaskPaint.hpp"
#include "brush/PencilDeposit.hpp"
#include "brush/PigmentErase.hpp"
#include "brush/RgbDeposit.hpp"
#include "brush/RgbErase.hpp"
#include "brush/Smudge.hpp"
#include "brush/StrokePath.hpp"
#include "brush/TonalBrush.hpp"
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
//   Pencil      RGB, with tiles, writable        PencilDeposit <- new; this
//                                                                row used to
//                                                                be a
//                                                                not-built
//                                                                refusal
//   Pencil      Pigment, with tiles, writable    None        <- and NOT
//                                                               CpuDeposit
//   Pencil      **no target at all**             None        <- and NOT
//   Dodge       RGB, with tiles, writable        TonalBrush  <- new; the two
//   Burn        RGB, with tiles, writable        TonalBrush     tonal rows are
//                                                               ONE route with
//                                                               a latched sign
//   Dodge/Burn  RGB, with tiles, ALPHA-locked    TonalBrush  <- and NOT a
//                                                               refusal, unlike
//                                                               the eraser
//   Dodge/Burn  Pigment, with tiles, writable    None        <- a decision,
//                                                               argued below
//   Dodge/Burn  **no target at all**             None        <- and NOT
//                                                               PaintSim
//   CloneStamp  RGB, with tiles, writable        CloneStamp  <- new; §1b
//   CloneStamp  Pigment, with tiles, writable    None        <- a refusal by
//                                                               name, §1b
//   CloneStamp  **no target at all**             None        <- and NOT
//                                                               PaintSim, §1b
//   Smudge      RGB, with tiles, writable        Smudge      <- new; the rows
//                                                               below it are
//                                                               this tool's
//                                                               four refusals
//   Smudge      Pigment, with tiles, writable    None        <- a refusal by
//                                                               name, and it
//                                                               names its
//                                                               condition
//   Smudge      RGB, **alpha-locked**            None
//   Smudge      **any layer, locked**            None
//   Smudge      Adjustment / Media / Text / ...  None
//   Smudge      **no target at all**             None        <- and NOT
//                                                               PaintSim, for
//                                                               the eraser's
//                                                               reason, worse
//   Brush       **any layer, locked**            None
//   DryBrush    **any layer, locked**            None
//   Eraser      **any layer, locked**            None
//   Pencil      **any layer, locked**            None
//   Brush       Adjustment / Media / Text / ...  None
//   DryBrush    Adjustment / Media / Text / ...  None
//   Eraser      Adjustment / Media / Text / ...  None
//   Pencil      Adjustment / Media / Text / ...  None
//   Dodge/Burn  **any layer, locked**            None
//   Dodge/Burn  Adjustment / Media / Text / ...  None
//   CloneStamp  Adjustment / Media / Text / ...  None
//   CloneStamp  **any layer, locked**            None
//   Brush       **no target at all**             PaintSim
//   DryBrush    **no target at all**             PaintSim
//   Water       anything                         PaintSim     (unchanged)
//   Eyedropper / Hand / Zoom                     None         (unchanged)
//
// Fourteen rows are decisions rather than bookkeeping.
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
// **The Pencil rows, and the two of them that are decisions.** Until this step
// `Tool::Pencil` sat in the not-built list below with the nineteen other
// name/icon/slot cells, so a drag with it reached no layer and said nothing.
// `brush/PencilDeposit` is the tool and its §0 is the whole argument for what
// separates it from a hard brush -- in one line: a hard *dab* is not a hard
// *mark*, because `flow < 1` plus overlapping dabs grades a stroke's rim
// whatever the tip's hardness, so the pencil thresholds its coverage AND
// ignores flow, and the second half is the one a slider could not have given.
//
//   * **A Pigment layer refuses, and this is the row that is a real decision
//     rather than a missing feature.** The tempting row is `CpuDeposit` -- it
//     never blocks the user, and the pencil would draw. It would draw a
//     *soft-edged* mark: `brush/Deposit` mixes latents weighted by the mass a
//     dab lays, has no per-stroke ceiling at all (which is why the OPACITY
//     slider is drawn disabled on that route), and therefore has nothing for
//     "one dab is the whole mark" to be a rule about. The tool would work,
//     produce marks, and not be a pencil -- which is the invisible
//     wrong-behaviour failure the locked row and the Eraser's `nullptr` row
//     were both written to prevent, in its third variant. The honest Pigment
//     row is a binary-mass engine beside `brush/PencilDeposit`, and unlike the
//     Pigment *erase* it is not blocked on plumbing: it is blocked on a
//     question nobody has answered, namely what graphite is in a Kubelka-Munk
//     medium (`paint/Palette` has no entry for it, and mass at `kMaxMass` in
//     whatever hue happens to be loaded is not it). Refusing by name is what
//     keeps that from rotting into a silent fallback.
//
//   * **`nullptr` is `None`, exactly as it is for the eraser and for the same
//     shape of reason.** `sim::PaintSim` is a fluid solver whose whole output
//     is diffusion, wet edges and granulation -- every one of them a way of
//     making a mark's boundary soft. A pencil sent there would produce the
//     softest-edged mark in the build, on the solver canvas rather than on a
//     layer, which is the tool doing the opposite of its defining property.
//
//   * **An alpha-locked RGB layer still takes it**, unlike the eraser. A
//     pencil is a deposit; `brush/RgbDeposit` §4.5's colour-only composite is
//     what it reuses (brush/PencilDeposit §4), so drawing inside existing
//     alpha is the feature rather than the refusal. A *locked* layer still
//     refuses, from the shared body, before the kind is looked at.
// **The Dodge and Burn rows, and the four of them that are decisions.** Both
// tools were name/icon/slot-only cells until this step -- they drew a cursor,
// took a keystroke, and no gesture either made reached any layer. They are
// **one route**, `StrokeRoute::TonalBrush`, because they are one operation
// whose only free variable is a sign; `brush/TonalBrush` §0 carries that
// argument and explains why it is the opposite call to the one the two erase
// routes made.
//   * **A Pigment layer REFUSES, and this row is a decision rather than a
//     gap.** A tonal shift is defined on a display-referred colour
//     (`brush/TonalBrush` §2), and a Pigment texel does not hold one: it holds
//     a Mixbox `Latent` premultiplied by mass, and CONTEXT.md's glossary is
//     explicit that "nothing outside the pigment module may assume a basis has
//     three weights plus a residual". Raising a latent to a power is not a
//     Kubelka-Munk mix of anything -- the result is not the latent of any
//     pigment -- so a "dodge" there would be an arbitrary rewrite of a physical
//     quantity that then propagates through every later mix. The two operations
//     that ARE meaningful on pigment already exist under their own names:
//     less mass is `brush/PigmentErase`, and a lighter paint is a different
//     `Latent`, which is the colour picker. Refusing by name leaves the UI free
//     to say which one the user wants; guessing would put an un-invertible edit
//     into the one storage in this build whose values mean something physical.
//   * **An ALPHA-LOCKED RGB layer takes the stroke**, and this is where the
//     tonal rows deliberately part company with the eraser's. `alphaLocked`
//     freezes one quantity -- the layer's alpha -- and `brush/TonalBrush` §1's
//     whole invariant is that the alpha channel is *copied*, not recomputed. So
//     the lock is satisfied by construction rather than by a check, exactly as
//     it is for `RgbDeposit`, and refusing here would make the flag block an
//     edit it has no quarrel with. The erase row refuses because removing alpha
//     is precisely what that route is for.
//   * **`nullptr` is `None`, not `PaintSim`**, for the eraser's reason with a
//     different verb: `sim::PaintSim` has no dodge step, so a tonal stroke sent
//     there would run the *paint* path with the brush's loaded pigment and
//     deposit colour where the user asked for a tonal shift. That is not a
//     missing feature, it is the tool doing something else entirely, and it is
//     the invisible wrong-target failure the locked row exists to prevent.
//   * **Locked, Adjustment, Media, Text, Flats and Strokes refuse**, and they
//     refuse through the shared body rather than by name -- a tonal op needs a
//     writable RGB store and none of them has one. ADR-0007's per-kind table is
//     about erasing and says nothing about tone; when a Media layer grows a
//     readback, this row is one of the ones that has to be answered again.
//
// ==========================================================================
// 1b. The CloneStamp rows, and the four of them that are decisions
// ==========================================================================
//
// `brush/CloneStamp` is the engine and its header carries the arithmetic and
// the source-snapshot hazard. What belongs here is the table, and four of its
// rows are decisions rather than bookkeeping.
//
//   * **A writable RGB layer is the one destination.** The clone reads and
//     writes premultiplied linear RGBA in one store, which is exactly what
//     `core::TileStore` holds.
//
//   * **A Pigment layer refuses, BY NAME.** This is not "not built yet
//     because nobody got to it". A clone at partial coverage is a *mixture* of
//     the source texel and the destination texel, and on a Pigment layer that
//     mixture is a Kubelka-Munk one -- two latents combined in proportion to
//     their masses, which is what `depositTexel()` owns and what a
//     four-channel lerp is not. The soft edge of a cloned dab, which is most
//     of the dab, would therefore be a *different operation* there rather than
//     the same one on different storage. That is a second module (a
//     `brush/PigmentClone`, the way `brush/PigmentErase` is a second module
//     and not a flag on `brush/RgbErase`), and until it exists the honest
//     answer is a refusal the UI can print. **Not** a silent no-op, and **not**
//     a straight copy of the seven channels -- the latter looks right at full
//     opacity and is wrong everywhere the tip falls off, which is the worst of
//     the three options because it is the one nobody would report.
//
//   * **`nullptr` is `None`, and this is the eraser's row read one step
//     further.** A brush with no target paints the solver canvas
//     legitimately. A clone stamp with no target has no *source*: what it
//     copies is a document's own texels, and `sim::PaintSim`'s dense canvas
//     texture is not one -- no tile store to sample, no alpha. Sent there the
//     tool would run the paint path with a brush tip and lay down the
//     FOREGROUND COLOUR, which is not a degraded clone; it is a different tool
//     wearing this one's name.
//
//   * **A locked layer refuses, and alpha lock does NOT** -- and the two rows
//     disagreeing is the point. `locked` is checked before the kind, as for
//     every other tool, so the message names the one thing a user can fix.
//     `alphaLocked` freezes the layer's alpha and permits colour to change,
//     which is the whole feature it exists for, so a route that *adds* colour
//     belongs on the permitted side: the clone is such a route, and
//     `brush/CloneStamp` §1 carries the colour-only composite that honours the
//     lock without ever un-premultiplying the source. Folding this into the
//     eraser's alpha-lock refusal would block a legitimate edit; folding the
//     eraser into this row would make `alphaLocked` decorative.
//
// **The unset source is a refusal in `begin()`, not a row in this table.**
// `strokeRouteFor()` is pure in `(tool, target)`, and the anchor is neither --
// it is session state on `AppState` (`CloneSourceState`, whose own comment
// argues why it cannot live on this class). Threading it in would make a pure
// routing question depend on a gesture, and would give `toolBeginsStroke()` --
// which probes this table with two synthetic layers and no `AppState` at all
// -- the wrong answer for the palette, greying the cell out until the user
// managed to Option+click a tool they could not select. So the table says "an
// RGB layer can take a clone", and `begin()` says "but this one has no source
// yet", in the same `errorOut` sentence every other refusal uses and in the
// same band. `cloneSourceRefusal()` below is that sentence.
// **The Smudge rows, and why five of the six are refusals.** `brush/Smudge` is
// the engine; `Tool::Smudge` sat in the not-built list below beside the rest of
// the name/icon/slot-only cells, so a drag with it reached nothing at all. It
// is the first route whose dabs are not independent -- it carries a colour from
// dab to dab (that header's §1) -- but the routing question it asks is the
// familiar one, and the answers differ from the eraser's in exactly two places.
//
//   * **An RGB layer is the only destination.** Smudge reads and writes the
//     same premultiplied quadruple `brush/RgbDeposit` and `brush/RgbErase` do,
//     and the whole of `brush/Smudge` §2 is arithmetic on that storage.
//
//   * **A Pigment layer refuses BY NAME, on a stated condition**, exactly as
//     the Pigment *erase* row did before `brush/PigmentErase` paid it off --
//     and it is a refusal rather than a silent fallthrough for the same reason
//     that one was: the layer kind `Document::createBlank()` makes is Pigment,
//     so this is the row a user is most likely to meet first, and it must say
//     why. The condition is not plumbing. A Pigment texel is a Kubelka-Munk
//     latent plus a mass (`core/Pigment.hpp`), and `brush/Smudge` §2's pick-up
//     is a coverage-weighted **arithmetic mean**. The mean of two latents is
//     not what mixing two paints means in this build: `depositTexel()` mixes
//     them with a *mass*-weighted lerp whose exactness at `m == 0` is what
//     makes `brush/Deposit` §1's idempotence-in-hue invariant assertable at
//     zero tolerance. A linear average would be a second, unproven mixing rule
//     sitting next to the one this application exists for, and it would be
//     wrong in the way that is hardest to see -- plausible colours, drifting
//     hue. The row opens when someone decides what the mass-weighted mean of a
//     footprint of latents is and asserts it, not before.
//
//   * **An alpha-locked RGB layer refuses**, the same way the eraser's row
//     does and for a sharper version of the same reason. `alphaLocked` freezes
//     the layer's alpha (core/Layer.hpp), and smudge moves alpha inseparably
//     from colour: `brush/Smudge` §5 derives why the write has to be one lerp
//     factor across all four premultiplied channels, and there is no way to
//     hold `dst.a` still inside that without un-premultiplying the finger --
//     a division by the finger's own alpha, which is exactly 0 whenever the
//     finger is empty. `brush/RgbDeposit` §4.5 could give the brush a
//     locked-alpha composite because its source colour is *straight* to begin
//     with; this route's is not. Refusing keeps the flag honest rather than
//     decorative, which is the standard the erase row already set.
//
//   * **`nullptr` is `None`, and this is the eraser's row with the argument
//     one notch stronger.** `sim::PaintSim` has no smudge step, so a smudge
//     sent there would run the *paint* path -- and unlike the eraser, which
//     would at least have added paint with no colour of its own, this one would
//     deposit the loaded foreground pigment. A tool whose entire promise is
//     "moves what is already there, introduces nothing new" would introduce a
//     colour the picture did not contain. Nothing to smudge, so nowhere to go.
//
//   * **Locked, and the storeless kinds**, refuse through the shared body
//     below with no case of their own -- which is the point of the shared body.
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
// the layer at that index must still route to **the same one** of §1's five
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
  PencilDeposit,  // brush/PencilDeposit, into the target layer's rgb tiles --
                  // the ALIASED mark. A separate route from RgbDeposit and not
                  // a flag on it because the two differ in what a *stroke*
                  // means, not only in a coverage: this one reads no flow at
                  // all, so one dab is the whole mark and the number of dabs
                  // that overlap a texel cannot change its value
                  // (brush/PencilDeposit §§0, 2)
  TonalBrush,    // brush/TonalBrush, moving the TONE of the target layer's rgb
                 // tiles without moving their alpha -- Dodge and Burn, which
                 // are ONE route with a sign latched at pen-down and not two,
                 // because the accumulator counts the same thing in the same
                 // units for both (brush/TonalBrush §0)
  CloneStamp,    // brush/CloneStamp, compositing texels read from a PRE-STROKE
                 // SNAPSHOT of the same rgb tiles at a fixed offset -- §1b. A
                 // route of its own and not a source mode on RgbDeposit,
                 // because the "ink" is a different value at every texel and
                 // is read out of the store being written (that header's §0)
  Smudge,        // brush/Smudge, moving colour that is ALREADY in the target
                 // layer's rgb tiles along the stroke -- the tip carries no ink
                 // and the layer is both source and destination. The first
                 // route in this table whose dabs are not independent of each
                 // other (that header's §1)
  PaintSim,      // sim::PaintSim's dense canvas texture, and only when there is
                 // no document layer to have aimed at -- see §1's last paragraph
  MaskPaint,     // brush/MaskPaint, into the target layer's MASK tiles rather
                 // than into any of its content stores -- the first route in
                 // this table whose destination is not decided by the (tool,
                 // layer) pair alone, because a layer that HAS a mask has two
                 // writable stores and only the user can say which one is meant
                 // (`LayerEditTarget` below). A route of its own and not a flag
                 // on `RgbDeposit`, because a mask sample is a scalar coverage
                 // with no privileged end (brush/MaskPaint §1): "paint" and
                 // "erase" are two directions of one lerp there rather than two
                 // arithmetics, so a flag would have to select a different
                 // value type and a different store as well as a sign
};

// Which of a layer's two writable stores a stroke is aimed at.
//
// **This is the third input to the routing table, and the first one that is
// not a property of the document.** `strokeRouteFor(tool, target)` is pure in a
// tool and a layer and stays that way; a layer that has a mask has two places a
// brush could legitimately write, and nothing about the layer or the tool says
// which. The user says, by clicking a thumbnail in the layer row, and that
// choice lives on `OpenDocument::maskIsEditTarget` beside `activeLayer` --
// session state rather than document content, for exactly the reason that
// member's own comment gives about `activeLayer`.
//
// `docs/testing-issues.md` T16 called this "the target concept" and named its
// absence as the deeper of the two reasons all three of its gestures were
// missing.
enum class LayerEditTarget {
  Content,  // the layer's own rgb or pigment tiles -- what every route above
            // `MaskPaint` writes, and the answer this build gave to every
            // stroke before that route existed
  Mask,     // the layer's `MaskTileStore`
};

const char* layerEditTargetName(LayerEditTarget target) noexcept;

// The target a stroke actually gets, given what the user asked for and what the
// layer can offer.
//
// **The whole reason this is a function is that the answer must be unambiguous
// when the layer has no mask.** "The mask is selected" plus a layer whose
// `mask` is `std::nullopt` is a state reachable in one gesture -- select the
// mask on one row, then click a different row -- and the wrong answer there is
// not a refusal, it is a live control over nothing: a brush that leaves no
// mark, with a chip lit, and nothing anywhere saying why. That is the defect
// class this codebase keeps naming; §6's last paragraph tells the same story
// about the paint bucket, which discarded a click on a Pigment layer with "no
// message, no history entry and no mark on the canvas".
//
// So **`Mask` comes back only when the layer really has one.** The panel
// resolves through this same function, so a chip cannot be lit over a store
// that is not being written.
LayerEditTarget resolveLayerEditTarget(bool maskRequested, const Layer* layer) noexcept;

// The routes that write a `Layer`, as one predicate, because four call
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
//
// **`TonalBrush` is in here for the identical reason, one step further out.**
// It neither adds paint nor removes it -- it changes the colour of paint that
// is already there, leaving the alpha bit-identical -- and every one of the
// four call sites still wants the same answer: it unshares a copy-on-write
// tile, moves the revision, dirties tiles for the incremental composite and
// owes exactly one history entry. That this predicate needed no new *concept*
// to admit a third family is the evidence that "writes a layer" was the right
// question to extract, rather than "deposits" or "deposits or erases".
// **The clone route is in here too, and it was worth asking rather than
// assuming.** It reads a layer as well as writing one, which none of the other
// four do -- but every one of the four questions this predicate answers is
// about the WRITE: does the session accept it, does the per-frame
// re-validation guard it, does the canvas branch take it, does the options bar
// accent it. A clone unshares a copy-on-write tile, moves the revision,
// dirties tiles for the incremental composite and owes exactly one history
// entry, same as a deposit.
// **Smudge is in here too, and "writes" is if anything more literal for it.**
// It neither adds nor removes paint on balance -- it moves it -- but every one
// of the four call sites is asking about the mechanics, not the intent: it
// unshares copy-on-write tiles, moves the revision, dirties tiles for the
// incremental composite and owes exactly one history entry, and it can allocate
// tiles a stroke passes over, which the erase deliberately cannot
// (brush/Smudge §§5-6).
//
// **`MaskPaint` is in here, and it is the first entry that writes a store the
// word "layer" does not obviously cover.** It writes `Layer::mask`, not
// `Layer::rgbTiles` or `Layer::pigmentTiles` -- but every one of the four call
// sites is asking about the mechanics of the write and gets the same answer:
// it unshares a copy-on-write tile, moves the revision, dirties tiles for the
// incremental composite (a mask tile changing changes the composite over
// exactly that tile) and owes exactly one history entry. Saying no here would
// make `begin()` refuse every mask stroke and the options bar grey a live one
// as though it went to the solver -- which is the specific drift this
// predicate was extracted to stop, arriving through a store rather than
// through a tool.
//
// **Two other predicates delegate to this one and both were re-asked rather
// than inherited**, because this one's own comment and `grainReachesRoute()`'s
// both warn that a route added without answering their questions leaves them
// correct and their call sites wrong. `grainReachesRoute()`: yes -- a mask dab
// computes a CPU coverage and `brush/MaskPaint.cpp` calls `grainCoverageAt()`
// on the same line of its per-texel loop every other layer-writing route does.
// `wetnessReachesSolver()`: no, and unchanged -- it names `PaintSim` alone, and
// a mask stroke does not touch the solver.
inline bool strokeRouteWritesLayer(StrokeRoute route) noexcept {
  return route == StrokeRoute::CpuDeposit || route == StrokeRoute::RgbDeposit ||
         route == StrokeRoute::RgbErase || route == StrokeRoute::PigmentErase ||
         route == StrokeRoute::PencilDeposit || route == StrokeRoute::TonalBrush ||
         route == StrokeRoute::CloneStamp || route == StrokeRoute::Smudge ||
         route == StrokeRoute::MaskPaint;
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
// would again be nothing to notice.
//
// **That fifth route arrived, and the separate claim is what got it
// answered.** `StrokeRoute::PencilDeposit` calls `grainCoverageAt()` on the
// same line of its per-texel loop the other four do -- and then thresholds the
// result (brush/PencilDeposit §1), so paper tooth reaches a pencil as a
// keep/drop speckle rather than as a grey. Delegating stays correct because
// the answer to the question was yes, not because nobody asked it. `app/selftest/StrokeSession.cpp` asserts
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
//
// **This form means "aimed at the layer's content"**, which is what every call
// site meant before masks were writable and what all but three of them still
// mean: the cursor's refusal shape, the options bar's route indicator,
// `toolBeginsStroke()`'s probe and `toolSurfaceFor()` are all asking about the
// tool, not about which of a layer's stores a particular document currently has
// selected.
StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept;

// The same table with the edit target as its third input (`LayerEditTarget`
// above).
//
// **`Content` DELEGATES to the two-argument form rather than repeating it.**
// That is the whole reason this is an overload and not a second table: §1
// exists to stop one question being answered in more than one place, and a
// three-argument copy of the same rows would be exactly the drift this file's
// own comments describe -- "the options bar's route indicator read 'goes to the
// solver' grey for a live RGB stroke for exactly as long as it had its own copy
// of the test". `app/selftest/LayerMask.cpp` asserts the delegation over every
// tool, so a row added to one and not the other fails rather than diverging.
//
// A `Mask` target on a layer with no mask cannot reach here: callers resolve
// through `resolveLayerEditTarget()` first, and this function answers `None`
// for that combination anyway rather than trusting them to.
StrokeRoute strokeRouteFor(Tool tool, const Layer* target, LayerEditTarget editTarget) noexcept;

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

// --- the Clone Stamp's source gesture (§1b, AppState::CloneSourceState) -----
//
// Two free functions rather than methods on `CloneSourceState`, for the reason
// `toolWritesRgbPixels()` below is a free function: `app/AppState.hpp` is a
// data header that every band of the chrome includes, and the *rules* about a
// gesture belong beside the routing table that refuses it -- which is here.
// They are the whole of what `ui/MacPaintUI.cpp`'s canvas block has to call,
// which is what keeps that edit to one small block in a function five other
// tools also live in.

// Option+click. Sets the anchor and **discards any offset already latched**,
// which is the load-bearing half: a new source means the next stroke must
// re-derive the vector from its own pen-down, and an offset that survived
// would leave the click looking like it worked while the copy carried on from
// the old source.
void setCloneAnchor(AppState::CloneSourceState& clone, Vec2 anchor) noexcept;

// Pen-down. Latches `offset = anchor - penDown` the first time it is called
// after an Option+click, and is the identity on every call after that (this
// build clones *aligned* -- `CloneSourceState`'s own comment). Returns whether
// the stroke has a usable source, so the caller does not have to read two
// flags to find out.
//
// **The latch is here and not in `StrokeSession::begin()`** because `begin()`
// has no position: its signature has no x/y at all, which is the same fact
// that forces `seed_` to be latched from the stroke's first DAB rather than at
// pen-down. The offset cannot wait that long -- `brush/CloneStamp::begin()`
// needs it to take the snapshot against -- so it is resolved one step earlier,
// where the pointer position actually exists.
bool latchCloneOffset(AppState::CloneSourceState& clone, Vec2 penDown) noexcept;

// The sentence a clone stroke refuses with when no Option+click has happened
// yet, or empty when it has. §1b's last paragraph is why this is a refusal in
// `begin()` rather than a row in `strokeRouteFor()`.
//
// **A clean, visible refusal is the whole requirement here**, and the reason
// is that the obvious failure mode is invisible: with no anchor the offset is
// (0,0), every texel's source is itself, and a full-opacity composite of a
// texel onto itself is a perfect no-op. The user would see a tool that draws
// nothing, with the palette cell lit, the cursor correct and not one word
// anywhere about why -- which is precisely the silent no-op
// `ui/AtelierChrome`'s `toolHasCanvasHandler()` tripwire exists to prevent one
// tier up. In the same voice and shape as the other refusals: what is wrong,
// and what to do about it.
std::string cloneSourceRefusal(const AppState::CloneSourceState& clone);

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
// **These predicates exist because a hand-maintained boolean shipped a
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
// Two of the gates already existed and are reused unchanged:
// `strokeRouteFor()` and `toolWritesRgbPixels()` above. Six of the seven are
// declared in this section; the seventh, `toolZoomsView()`, is declared with
// the feature it gates (`app/ZoomAndSize.hpp` §3), which is equally valid --
// what matters is that each one is the literal gate expression, not where it
// is written.

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

// Whether `tool` reads **colour** off the canvas rather than writing it: the
// eyedropper, and nothing else.
//
// **Deliberately still false for `Tool::Measure`, now that Measure is built.**
// The two share a palette group and a `ToolCursor::Sample` cursor, and
// `ui/ToolCursor.hpp`'s own enumerator comment groups them as "reading the
// canvas rather than writing it" -- so widening this predicate by one name is
// the one-line way to make `toolHasCanvasHandler(Tool::Measure)` go true, and
// it is wrong twice over. It is wrong at the gate: this predicate is not a
// description, it IS the expression `ui/MacPaintUI.cpp`'s eyedropper block is
// written with, so a Measure that satisfied it would have its clicks handed to
// `applyEyedropperPick()` and would silently reset the foreground colour on
// every drag. And it is wrong at the contract: an eyedropper pick is ONE point
// and produces a colour; a measurement is TWO points and produces a length and
// a heading, so the shared word "sample" is the only thing they actually have
// in common. `toolMeasuresCanvas()` below is the seventh gate instead.
bool toolSamplesCanvas(Tool tool) noexcept;

// Whether `tool` reads **geometry** off the canvas by dragging a line across
// it: the measure tool, and nothing else (`app/MeasureLine.hpp` §0).
//
// The seventh gate, and the argument for adding one rather than widening a
// neighbour is `toolSamplesCanvas()`'s comment directly above. The shape is
// `toolZoomsView()`'s (`app/ZoomAndSize.hpp` §3): one predicate per canvas
// block, declared beside the family it joins, absorbed into
// `toolHasCanvasHandler()` by name.
//
// It is declared HERE rather than in `app/MeasureLine.hpp` for the same
// reason `toolDrawsSelection()` is not in `core/SelectionShapes`: this file is
// where the tool-to-canvas-block family lives, and `app/MeasureLine.hpp` is
// deliberately `<cstdint>`-only geometry that does not know what a `Tool` is.
bool toolMeasuresCanvas(Tool tool) noexcept;

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
//
// `subIndex` (Part 1, Scatter Count) picks which of `resolvedCount` sub-dabs
// stamped at ONE nominal position this call is drawing an offset for --
// folded into the seed before SCATTER's own random draw, so each sub-dab
// lands independently rather than all of them piling onto the same offset.
// Defaulted to `0`, unlike `brush/Deposit.hpp`'s deliberately non-defaulted
// `Selection*` (whose header explains why THAT default would be a trap):
// omitting `subIndex` always means "the only dab at this position", which is
// the correct and only meaning for every one of this function's callers that
// predate Scatter Count, so a default here cannot silently produce a wrong
// answer the way a defaulted selection could. At `subIndex == 0` the fold is
// the identity (`app/StrokeSession.cpp`'s own definition proves it: the
// salt is multiplied by `subIndex`, which is exactly zero there), so every
// existing call site -- including `app/selftest/Scatter.cpp`'s, which does
// not pass this argument -- keeps reading bit-identically.
Vec2 applyPerDabScatter(Vec2 centre, const BrushTip& tip, uint64_t seed, uint32_t dabIndex,
                        float stepDx, float stepDy, uint32_t subIndex = 0) noexcept;

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
  // **Which of the five layer-writing routes runs is decided here, once**, by
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
  // For a **smudge** it latches the same `tip.opacity` as that route's
  // **strength** (brush/Smudge.hpp §3), one more reading of the one slider:
  // there it is how far the carried colour dominates the canvas, and the
  // stroke's carried colour is only meaningful against the strength it was
  // carried with, so the two are bound together at this one call for
  // `RgbEraseStroke`'s stated reason. The ink is not read here either, and for
  // a sharper version of the eraser's reason: a smudge that reached for
  // `tip.linearRgb` would introduce a colour the picture did not contain, which
  // is the one thing this tool promises not to do (that header's §7 on
  // Photoshop's Finger Painting, which is exactly that feature and is
  // deliberately not built).
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
  //
  // `clone`, read only on the `StrokeRoute::CloneStamp` route: where the
  // source is, and whether there is one at all (§1b). **Defaulted to `nullptr`
  // so no existing caller changes**, and a null one on the clone route is not
  // a silent fallback -- it refuses with `cloneSourceRefusal()`'s sentence,
  // exactly as an unanchored state does, because "the UI forgot to pass it"
  // and "the user has not set a source" have the same correct answer: do not
  // paint, and say so. Borrowed for the duration of `begin()` only: the offset
  // is copied into `brush/CloneStamp`'s own stroke object, so nothing here
  // holds a pointer into `AppState` past this call.
  bool begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
             std::string* errorOut, const BrushModel* model = nullptr,
             const DynamicInputs& hardwareInputs = DynamicInputs{},
             const AppState::CloneSourceState* clone = nullptr);

  // Which of §1's five layer-writing routes this stroke took. Meaningless before
  // `begin()` succeeds.
  StrokeRoute route() const noexcept { return route_; }

  // The clone route's snapshot and offset, exposed for the same reason
  // `dabCount()`/`texelsWritten()` are: they are internals with no other
  // observation point, and two of this tool's claims are about them -- that
  // the snapshot is taken and dropped at the right moments, and that the
  // offset the engine actually samples with is the rounded one
  // (brush/CloneStamp §3). Both are 0 on every other route.
  size_t cloneSnapshotTiles() const noexcept { return clone_.snapshotTiles(); }
  int32_t cloneOffsetX() const noexcept { return clone_.offsetX(); }
  int32_t cloneOffsetY() const noexcept { return clone_.offsetY(); }

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
  // The pencil route's own accumulator, latched ink and latched ceiling
  // (brush/PencilDeposit §2). A fourth member for the reason there are three
  // already: the invariant "exactly one of these is live" is checkable with
  // four objects and hidden by one object with a tag. It is `RgbStroke`'s
  // storage and `RgbStroke`'s composite, and still not `rgb_` with a flag --
  // the two disagree about what a stroke IS (a rate reaching a ceiling over
  // many dabs, against one dab that is the whole mark), which is the
  // difference `brush/PencilDeposit` §0 is entirely about.
  PencilStroke pencil_;
  // The tonal route's, likewise (brush/TonalBrush.hpp §3). A fourth member for
  // the same reason as the third, plus one that is this route's alone: it
  // latches a DIRECTION as well as a strength, so a shared accumulator would
  // have to be told which sign it was counting in and the one combination
  // §3 exists to make unspellable -- an accumulator from a Dodge read by a Burn
  // -- would become spellable again.
  TonalStroke tonal_;
  // The clone route's own pre-stroke source snapshot, integer offset, latched
  // ceiling and alpha accumulator (brush/CloneStamp §2). A fourth member for
  // the same reason there are three above: exactly one of them is ever live,
  // and the one thing this one holds that none of the others does -- a whole
  // second `TileStore` sharing tiles with the layer -- is the one it is most
  // important to be able to see is empty. `snapshotTiles()` is 0 for a stroke
  // that took any other route, which `--selftest` measures rather than
  // assumes.
  CloneStampStroke clone_;
  // The smudge route's carried colour and latched strength (brush/Smudge §1).
  // A fourth member for the same reason there are three above -- exactly one of
  // them is ever live, and each `begin()`'s `else` branch is what leaves the
  // other three holding nothing after an interrupted drag. It is also the
  // reason that header spends a whole section on where the state lives: unlike
  // the three above it is 16 bytes and a bool rather than a tile store, so a
  // file-static in `brush/Smudge.cpp` would have been small enough to look
  // harmless and would have leaked one document's colour into another's stroke.
  SmudgeStroke smudge_;
  // The mask route's per-stroke fraction accumulator, latched target coverage
  // and latched ceiling (brush/MaskPaint §3). A seventh member for the reason
  // there are six above -- exactly one of them is ever live, and each
  // `begin()`'s `else` branch is what leaves the others holding nothing after
  // an interrupted drag. The one thing this one holds that none of the others
  // does is a *destination* that is not a content store, which is why it is
  // the member `editTarget_` below has to agree with.
  MaskPaintStroke maskPaint_;
  // Which store this stroke was aimed at, latched at `begin()` alongside
  // `route_` and `tool_` and for the identical reason (§5): a target that moved
  // half way through a drag would leave the dabs already spent in one store and
  // the rest in another, under one history entry that names neither. It is
  // latched rather than re-read because `route_` alone does not pin it down --
  // `MaskPaint` implies `Mask`, but that is an implication a future route could
  // break, and the per-frame re-validation compares the target it was begun
  // with.
  LayerEditTarget editTarget_ = LayerEditTarget::Content;

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

  // Scatter COUNT's own base value and Variance (Part 1, `PsScatter::count`/
  // `countJitter`) -- copied out at `begin()` alongside the four above, and
  // resolved the identical single-call-per-dab way in `depositPending()`.
  // `baseCount_` defaults to 1, `BrushTip::count`'s own identity, so a
  // session begun with `haveModel_` false (or never resolving this member at
  // all) reads exactly the pre-existing single-dab-per-position behaviour.
  int32_t baseCount_ = 1;
  Variance countVariance_;

  // Transfer FLOW's own resolved multiplier (Part 2, `PsTransfer::flow`) --
  // latched ONCE at `begin()`, from `hardwareInputs` and a fixed placeholder
  // seed (`begin()`'s own comment says why: there is no real stroke position
  // yet), and applied to `tip_.flow` fresh every dab in `depositPending()`
  // rather than baked into `tip_.flow` here. Baking it in would not survive
  // `setTip()` rebuilding `tip_` from a fresh, Transfer-unaware
  // `brushTipFor()` call on the stroke's very next frame -- `depositPending()`
  // 's own comment on this member has the full argument. Defaults to 1.0f,
  // the multiplicative identity, so a session with no model or an inert
  // Transfer Flow Variance reads `tip_.flow` completely unmodified.
  //
  // Transfer OPACITY has no equivalent member: it is a per-stroke CEILING,
  // latched directly into `rgb_`/`erase_`/`pigErase_`'s own accumulators at
  // `begin()` (their `*_.begin()` calls take the resolved value directly),
  // which is immune to `setTip()` by construction -- `setTip()` never touches
  // any of those three objects, only `tip_`.
  float transferFlowMul_ = 1.0f;

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
