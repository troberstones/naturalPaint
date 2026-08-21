#pragma once

#include <cstddef>
#include <string>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"

// core/LayerOps (PLAN.md "Phase 5 -- Stack it", step 1: "Multiple layers in
// `Document`, with reorder, visibility, lock, opacity"; PRD C4: "Layers behave
// as in Photoshop: reorder, group, opacity, blend mode, per-layer mask,
// visibility, lock").
//
// --- Where these live, and why not on `Document` --------------------------
//
// `core::Document` is a plain aggregate: public members, one static factory,
// no invariants it enforces. Everything that already mutates a document --
// io/ImageIO's `placeImageAsLayer()`, io/NpaintFile's loader -- does so by
// touching `doc.layers` directly, and both are correct to. Adding member
// functions here would have made `Document` half-encapsulated: a `removeLayer()`
// method that refuses a locked layer, sitting next to a public `layers` vector
// that lets any caller `erase()` the same layer with no check at all. That is
// the worst of both, because it *reads* like an enforced invariant and is not
// one.
//
// So these are free functions in their own translation unit, and the honest
// claim they make is narrow and exact: **`locked` is enforced by these
// operations, not by `Layer`.** A caller that reaches past them into
// `doc.layers` is not defeating a guard, it is declining to use one -- the same
// relationship `app/CurveEdit`'s mutators have with the `Curve` vector they
// keep sorted, and the same one `core/OpStack`'s bounds-checked mutators have
// with the stack. `ui/MacPaintUI`'s layers panel goes through here for every
// change it makes, which is what makes the lock real where a user can reach it.
//
// --- What `locked` means, stated once -------------------------------------
//
// core/Layer.hpp shipped `locked` inert, with the note that "nothing edits
// layers through a checked path yet, so nothing consults this". This step is
// where it gets teeth, and the honest scope of those teeth is worth being blunt
// about: **there is still no pixel-edit path to a layer.** `sim::PaintSim` owns
// one dense GPU texture, a stroke reaches no `Layer::rgbTiles`, and no tool
// writes a texel into a document. So "locked layers reject edits" cannot mean
// "a brush refuses to paint on it" today, because no brush can paint on any
// layer, locked or not. Inventing a fake refusal path to demonstrate the lock
// would be worse than a narrow real one.
//
// What it means here, and all it means here:
//
//   **A locked layer's content and its own place in the stack are frozen; only
//   its visibility and its lock itself can still be changed.**
//
//   Refused:  removeLayer, moveLayer (of the locked layer), duplicateLayer's
//             *effect on the source* is nil so it is allowed -- see below --,
//             setLayerOpacity, setLayerName, setLayerBlend, addLayerMask,
//             removeLayerMask.
//   Allowed:  setLayerVisible. Hiding a locked layer changes nothing about the
//             layer, and a lock that also freezes the eye icon is the one
//             behaviour every editor with a lock agrees is wrong (Photoshop,
//             Krita and GIMP all let a locked layer be hidden).
//   Allowed:  setLayerLocked, in both directions. A lock that cannot be
//             removed is not a lock, it is a bug.
//   Allowed:  duplicateLayer of a locked layer -- it reads the source and
//             changes nothing about it. The copy inherits `locked`, matching
//             Photoshop, so duplicating is not a way to launder the lock off a
//             layer in one step.
//   Allowed:  addLayer next to a locked layer, and moveLayer of a *different*
//             layer past it. Both change the locked layer's index without
//             changing the layer. Locking one layer must not freeze the whole
//             document; the alternative rule ("nothing may move past a locked
//             layer") is not a rule any editor implements and would make a
//             locked bottom layer un-stackable-on.
//
// `setLayerBlend()` arrived with PLAN.md Phase 5 step 2, which is where the
// enumeration it needs came from. Step 1 left it out on the grounds that a
// setter "would have to decide today which names are offerable"; core/Blend
// now decides, so the setter takes a `BlendMode` rather than a string and the
// guess is gone. See its declaration below for why the *member* is still a
// string even though the *setter* is typed.
//
// --- Ordering ------------------------------------------------------------
//
// Every index below is an index into `Document::layers`, which is ordered
// **bottom to top**: index 0 is the bottom layer, `size() - 1` the top. That is
// the file format's order (docs/document-format.md: "Part order is layer order,
// bottom to top, after part 0") and core/Composite walks it in the same
// direction. The layers *panel* presents the same vector top-first;
// app/LayerPanel.hpp owns that single mapping and nothing else in the codebase
// reverses an index.
//
// --- Dirty tracking -------------------------------------------------------
//
// Nothing here touches `app::OpenDocument::recordEdit()`, because `core/` does
// not depend on `app/` (core/Layer.hpp: "app/ depends on core/, never the
// reverse"). Each result instead carries the `editLabel` such a caller should
// record, in the same noun form app/DocumentLifecycle.hpp already asks for
// ("place image as layer", "duplicate"). `app::recordLayerEdit()` is the one
// function that takes a result from here and turns it into a structural edit on
// an open document -- see app/DocumentLifecycle.hpp. Every one of these
// operations is `EditKind::Structural`: each changes the shape or the metadata
// of the layer stack, which is precisely what ADR-0008 wants journalled at once
// rather than on a timer.
namespace np {

// The shape every operation returns. `error` is non-empty exactly when `!ok`
// and always names the specific thing refused (the index, the layer, the
// value), matching io/Export's and io/NpaintFile's refusal style rather than
// inventing a third one.
struct LayerOpResult {
  bool ok = false;
  std::string error;
  // What a caller should record as the edit's label when `ok`. Empty when not.
  std::string editLabel;
  // The index the operation left the affected layer at: the insertion point for
  // addLayer/duplicateLayer, the destination for moveLayer, the touched layer
  // for the setters. `Document::layers.size()` (i.e. one past the end) after a
  // removeLayer that emptied the document. Meaningless when `!ok`.
  size_t index = 0;
};

// A default name for a newly created layer: "Layer N", where N is one above
// the highest number already used by an existing "Layer N" name, so adding,
// deleting and adding again does not hand out a name already on screen.
//
// core/Layer.hpp deferred this decision here on purpose -- "inventing a default
// naming scheme ('Layer 1', 'Background') is a UI decision that belongs with
// the layer panel in Phase 5, not with the data member". This is that decision,
// kept out of the panel itself so it is testable without ImGui. Note what it is
// *not*: it does not enforce uniqueness (names are explicitly not unique,
// docs/document-format.md) and it never renames an existing layer.
std::string defaultNewLayerName(const Document& doc);

// A new, empty RGB layer with `name` and this build's defaults -- the layer
// `addLayer()` is usually called with.
//
// Still RGB rather than Pigment after PLAN.md Phase 5 step 3, and the reason
// changed rather than went away. It used to be "a Pigment layer owns no
// storage yet, so a new one could not hold a pixel"; a Pigment layer now owns
// real latent-times-mass storage, projects, blends, mixes and saves. What it
// still cannot do is receive a brush stroke -- `sim::PaintSim` owns one dense
// texture and no stroke reaches a `Layer` of any kind -- so a new Pigment
// layer would be a layer a user could add and then not paint on, while an RGB
// one is at least what `Document::createBlank()`, image import and `.npaint`
// load all put content into. PRD's principle 3 ("Pigment by default") lands
// when the brush does; `Layer::kind`'s own default is already Pigment and was
// deliberately left there for that day.
Layer makeRgbLayer(std::string name);

// A new, empty Pigment layer: `pigmentTiles` engaged with zero tiles
// allocated, matching makeRgbLayer()'s shape and core/Layer.hpp's "populated
// only for the matching kind" contract, and PRD C2's "tiles allocate only
// where content exists".
Layer makePigmentLayer(std::string name);

// A new Adjustment layer (PLAN.md Phase 5 step 5, PRD C1, C5): **no tile
// storage of any kind, and an empty op stack**. That is not an incomplete
// version of the other two factories -- an Adjustment layer holds no pixels by
// definition (CONTEXT.md), and core/Composite treats one whose stack is empty
// as an exact no-op, so a freshly added adjustment layer is invisible until
// its stack has something in it. That is the same choice Phase 3 step 8 made
// for a newly added op ("adding an op should never itself change what's on
// screen, only enabling it should"), one level up.
//
// It is the one kind whose whole content is `Layer::ops`, which is why PLAN.md
// step 5 had to give `np:ops` a working carrier (io/OpSerial) before this
// factory could exist without handing a user a layer that a save would empty.
Layer makeAdjustmentLayer(std::string name);

// Inserts `layer` at `index`, shifting everything from `index` up. `index ==
// layers.size()` appends (i.e. puts it on top); a larger index is refused by
// name rather than clamped, because a clamped out-of-range insert is
// indistinguishable from a correct one at the call site.
LayerOpResult addLayer(Document& doc, size_t index, Layer layer);

// Removes the layer at `index`.
//
// Removing the **last** layer is allowed, and that is a decision rather than an
// oversight: a zero-layer document is representable (io/NpaintFile loads one and
// says so), `placeImageAsLayer()` already targets one, and PRD C16 is explicit
// that "a new document's base layer is an ordinary layer with alpha -- there is
// no special locked Background". A refusal here would reintroduce exactly the
// privileged bottom layer C16 rules out.
LayerOpResult removeLayer(Document& doc, size_t index);

// Moves the layer at `from` to `to`, both indices into the list as it stands
// *before* the move (so `moveLayer(doc, 0, 2)` on a three-layer document puts
// the bottom layer on top). `from == to` succeeds and changes nothing, but
// still reports `ok` -- a no-op reorder is not an error, and a caller that
// records an edit for it is recording something that genuinely happened as far
// as the user's gesture is concerned.
//
// **Refuses to move a `clipped` layer to index 0** (PLAN.md Phase 5 step 9),
// by name and with the numbers. That is the one reorder that would create a
// state `setLayerClipped()` refuses to create directly -- a clipped bottom
// layer, with no alpha below it to be clipped by -- and letting a *reorder* be
// the back door into it would make the setter's refusal decorative. Silently
// clearing the flag instead was the alternative and is worse: a drag would
// then change a layer's properties as a side effect, which is not what a drag
// means. Nothing else about clipping restricts a move: the base a clipped
// layer clips to is derived from position (core/Layer.hpp), so every other
// reorder legitimately re-parents it, which is exactly what dragging a clipped
// layer around a stack is for.
LayerOpResult moveLayer(Document& doc, size_t from, size_t to);

// Inserts a deep copy of the layer at `index` **directly above it**, at
// `index + 1`. Above rather than below because that is where every editor puts
// a duplicate and where the user's eye is.
//
// The copy is a real copy: `TileStore` is an `unordered_map` of uniquely-owned
// 128 KiB tiles today (copy-on-write is Phase 5 step 6), so duplicating an
// N-tile layer allocates N x 128 KiB -- N x 224 KiB for a Pigment layer, plus
// M x 32 KiB for a mask with M occupied tiles, since the mask is duplicated
// with the layer like every other member. Stated rather than hidden, the same
// way `duplicateDocument()` states its own cost.
//
// The copy's name gets a " copy" suffix when the source had a name, and is left
// empty when it did not -- inventing "Layer 3 copy" for an unnamed layer would
// be making up the part the user never wrote.
LayerOpResult duplicateLayer(Document& doc, size_t index);

// Sets visibility. Allowed on a locked layer; see this header's lock section.
LayerOpResult setLayerVisible(Document& doc, size_t index, bool visible);

// Sets opacity. Refuses a value outside [0,1] by name, and refuses NaN, rather
// than clamping: io/NpaintFile already refuses to *save* an out-of-range
// opacity by name (PRD I11), and a setter that silently clamped would let a UI
// put a value on screen that the file would then reject.
LayerOpResult setLayerOpacity(Document& doc, size_t index, float opacity);

// Sets the lock. Allowed in both directions on a locked layer.
LayerOpResult setLayerLocked(Document& doc, size_t index, bool locked);

// Sets the blend mode (PRD C4's "blend mode"). Refused on a locked layer --
// which blend a layer uses is part of how that layer looks, and a lock that
// froze content but not blending would be a lock in name only.
//
// **Takes a `BlendMode`, writes a `std::string`.** That asymmetry is the whole
// of this step's resolution of the enum question core/Layer.hpp posed, and
// core/Blend.hpp argues it: the member stays a string so PRD I10's verbatim
// carry of a name this build has never heard of is a structural property
// rather than machinery, and the type safety an enum member would have bought
// is bought here instead, at the only path a UI can take. There is no
// string-taking overload, deliberately -- one would be the hole the typed
// setter exists to close.
//
// Refuses a mode PRD L5 does not allow on this layer -- i.e. `Mix` on anything
// but a Pigment layer sitting directly on another Pigment layer -- by name,
// through the same `blendModeAvailableForLayer()` predicate the dropdown
// filters with, so the panel and the model cannot drift. The refusal is real
// rather than defensive: it is what stops "the dropdown never offers it" from
// being the only thing that makes L5 true.
LayerOpResult setLayerBlend(Document& doc, size_t index, BlendMode mode);

// --- The mask's lifecycle (PLAN.md Phase 5 step 4; PRD C4) ----------------
//
// These two are the whole of what a user can do to a mask in this build, and
// the honest reason is the one core/Layer.hpp and core/Mask.hpp both give:
// **nothing can paint one**. A stroke reaches `sim::PaintSim`'s dense texture
// and no `Layer` of any kind, so a mask's *content* can only come from a
// `.npaint` or from a test. Add and remove are the parts that do not need a
// brush, and they are real rather than placeholders -- `addLayerMask()`
// followed by a save writes a `mask` channel, and a load brings it back.
//
// Both are refused on a locked layer, for `setLayerBlend()`'s reason: which
// parts of a layer show is part of how that layer looks, and a lock that froze
// content but not masking would be a lock in name only.

// Gives the layer a mask that **reveals everything**: an engaged store with
// zero tiles, which costs no allocation at all because an unallocated mask
// tile means 1.0 (core/Mask.hpp). Photoshop's "Add Layer Mask -> Reveal All".
//
// Refused when the layer already has one, rather than replacing it: there is
// one mask slot per layer and a silent replacement would discard every texel
// in the old mask.
//
// **"Hide All" is deliberately absent.** Its mask is all-0.0, which is real
// content -- it would have to allocate one 32 KiB tile per canvas tile (8 MiB
// for a 2048x2048 document) holding nothing but the same number, which is
// precisely what PRD C2 ("memory tracks content, not canvas dimensions") is
// about. It becomes cheap either with a per-store default value or with the
// selection store phase 7 brings, and neither is this step's to invent.
LayerOpResult addLayerMask(Document& doc, size_t index);

// Removes the layer's mask, **discarding** it. Refused when there is none.
//
// Discard, never "apply": applying a mask bakes its coverage into the layer's
// own alpha (or, on a Pigment layer, its mass -- PRD F10's quantity), which is
// a destructive edit with a different name and a different undo entry. It is
// not built here and is not what this function does.
LayerOpResult removeLayerMask(Document& doc, size_t index);

// --- Clipping (PLAN.md Phase 5 step 9; PRD C9) ---------------------------

// Sets `Layer::clipped` -- whether the layer is clipped by the alpha of the
// layer below it. Refused on a locked layer, for `setLayerBlend()`'s reason:
// whether a layer is clipped is part of how that layer looks.
//
// **Three refusals, each with the numbers**, and each is a state
// core/Composite would otherwise have to approximate and warn about:
//
//   the bottom layer   index 0 has nothing below it. PRD C9 clips a layer by
//                      "the alpha of the layer below it", so there is no such
//                      alpha. Refused rather than accepted-and-ignored, which
//                      would leave a flag on screen that does nothing.
//   no alpha below     the nearest layer below that is not itself clipped
//                      holds no pixels -- an Adjustment layer, or one of the
//                      inert kinds. Same reason: no alpha to clip by. It is
//                      **not** resolved by clipping to something further down;
//                      core/Composite.hpp §12 says why.
//   a mixed pair       the layer, or the layer directly below it, is currently
//                      half of a `Mix` pair (PRD L5). A mix composites two
//                      layers as one unit and a clip makes one of them the
//                      alpha for the other; core/Composite.hpp §15 derives why
//                      both cannot hold. Clearing the blend first is the fix,
//                      and the refusal says so.
//
// Un-clipping (`clipped == false`) is never refused for any of the three --
// only the lock applies -- because none of them is a reason a layer may not
// stop being clipped.
//
// Setting the value it already has succeeds and changes nothing, matching
// `setLayerVisible()` and `setLayerLocked()`: a no-op the user asked for is
// not an error.
LayerOpResult setLayerClipped(Document& doc, size_t index, bool clipped);

// Sets the user-facing name. Any string is accepted, including an empty one
// (which means "unnamed", core/Layer.hpp) and one that duplicates another
// layer's -- names are explicitly not unique.
LayerOpResult setLayerName(Document& doc, size_t index, std::string name);

// --- The layer's own op stack (UI detour step 3; PRD C1, C5) --------------
//
// `Layer::ops` has been real since PLAN.md Phase 5 step 3 and persisted since
// step 5: core/Composite runs it over the layer's own pixels, and over
// everything already accumulated beneath when the layer is an Adjustment. What
// it has never had is a checked path a user could reach -- an op could only get
// into a layer from a `.npaint` or from a test. These five are that path.
//
// **Five functions rather than one `setLayerOps(OpStack)`**, and the reason is
// the journal rather than the arithmetic. A wholesale replace can honestly
// label itself only "edit ops"; `core::History`'s rows are read by a human (PRD
// O2), and "add Curves to layer 2" and "delete op 0 of layer 2" are different
// things to want back. Each of these mirrors exactly one `core::OpStack`
// mutator, so there is no second op-stack semantics here to drift from that
// one -- the ordering rule, the version bump and the run-detection consequences
// are all still core/OpStack's.
//
// Each adds this file's two standing guards -- the layer index, and the lock,
// refused for `setLayerBlend()`'s reason (an op stack is part of how a layer
// looks, and on an Adjustment layer it is the entire content of the layer) --
// plus the one guard `core::OpStack` deliberately does not have: **a bounds
// check on the op index that returns a sentence instead of throwing.**
// `OpStack::remove()`/`reorder()`/`setEnabled()`/`setOp()` all index through
// `std::vector::at`, so a stale op row from a panel whose stack shrank under it
// is a `std::out_of_range` escaping into a draw loop. Every other refusal a UI
// can provoke in this file is a sentence; this makes that one too.
//
// A note on what is *not* refused: an op stack on a kind that ignores it. Every
// kind carries an `OpStack` (core/Layer.hpp), core/Composite evaluates it for
// every kind that has pixels, and the inert kinds have no pixels to grade
// today but will. Refusing here would encode "which kinds are implemented in
// this build" as a rule about documents, which is exactly what PRD I10 says
// not to do.

// Appends `op` to the end of the layer's stack -- the top of it, evaluated
// last. `index` is the layer; the op lands at `doc.layers[index].ops.size()`
// as it stood before the call.
LayerOpResult addLayerOp(Document& doc, size_t index, Op op);

// Removes the op at `opIndex` from the layer's stack; every later op shifts
// down by one.
LayerOpResult removeLayerOp(Document& doc, size_t index, size_t opIndex);

// Moves the op at `from` so it ends up at `to`, `core::OpStack::reorder()`'s
// semantics exactly. `from == to` succeeds and changes nothing, matching
// `moveLayer()`.
LayerOpResult moveLayerOp(Document& doc, size_t index, size_t from, size_t to);

// Replaces the op at `opIndex` wholesale -- what a params edit is, since
// `core::OpStack` hands out no mutable reference to an entry.
LayerOpResult setLayerOp(Document& doc, size_t index, size_t opIndex, Op op);

// Enables or disables the op at `opIndex` without otherwise touching it.
// Setting the value it already has succeeds and changes nothing, matching
// `setLayerVisible()`.
LayerOpResult setLayerOpEnabled(Document& doc, size_t index, size_t opIndex, bool enabled);

}  // namespace np
