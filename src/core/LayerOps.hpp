#pragma once

#include <cstddef>
#include <string>

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
//             setLayerOpacity, setLayerName.
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
// There is deliberately no `setLayerBlend()`. PLAN.md Phase 5 step 2
// (`core/Blend`) owns the enumeration of blend modes, and a setter here would
// have to decide today which names are offerable -- exactly the guess
// core/Layer.hpp refuses to make by keeping the member a `std::string`.
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
// `addLayer()` is usually called with. RGB rather than Pigment for exactly the
// reason `Document::createBlank()` gives: a Pigment layer owns no storage yet,
// so a new one could not hold a pixel.
Layer makeRgbLayer(std::string name);

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
LayerOpResult moveLayer(Document& doc, size_t from, size_t to);

// Inserts a deep copy of the layer at `index` **directly above it**, at
// `index + 1`. Above rather than below because that is where every editor puts
// a duplicate and where the user's eye is.
//
// The copy is a real copy: `TileStore` is an `unordered_map` of uniquely-owned
// 128 KiB tiles today (copy-on-write is Phase 5 step 6), so duplicating an
// N-tile layer allocates N x 128 KiB. Stated rather than hidden, the same way
// `duplicateDocument()` states its own cost.
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

// Sets the user-facing name. Any string is accepted, including an empty one
// (which means "unnamed", core/Layer.hpp) and one that duplicates another
// layer's -- names are explicitly not unique.
LayerOpResult setLayerName(Document& doc, size_t index, std::string name);

}  // namespace np
