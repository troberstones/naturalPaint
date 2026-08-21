#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/LayerGeometry.hpp"
#include "core/LayerOps.hpp"

// core/LayerSetOps (PLAN.md "Phase 5 -- Stack it", step 11).
//
// PRD C12 (P0): "Multi-select layers; move, transform, group, delete and set
// properties as a set". PRD C13 (P1): "Align and distribute selected layers, to
// each other or to the canvas". PRD C15 (P2)'s linking half.
//
// ==========================================================================
// (1) WHAT C12 ASKS FOR, VERB BY VERB, AND WHICH TWO ARE REFUSED
// ==========================================================================
//
// C12 names five verbs. Three are built here and two are refused by name, in
// core/LayerComp.hpp's style -- stated where a reader will look for them rather
// than left to be discovered as an absence:
//
//   **move**       BUILT, in both senses the word has here. Reorder (Move Up /
//                  Move Down over a set) and translate (align/distribute, via
//                  core/LayerGeometry's new integer-pixel translate).
//   **delete**     BUILT, and it is the one with a real ordering hazard -- see
//                  section 3.
//   **set properties as a set**  BUILT: visibility, lock, clip, blend, opacity,
//                  colour label, link.
//
//   **transform**  **REFUSED.** There is no geometric transform of a layer
//                  anywhere in this codebase and this step did not add one.
//                  What it added is an *integer-pixel translate*, which is the
//                  degenerate case that needs no resampling; rotate, scale,
//                  skew and sub-pixel offset all do. core/LayerGeometry.hpp
//                  section 1 lists exactly what would have to exist first: a
//                  filter kernel choice, a premultiplied-alpha argument, and --
//                  on a Pigment layer -- a decision about whether a latent
//                  triple may be interpolated at all, which DESIGN-imaging.md
//                  §3 makes a document-level invariant rather than a coding
//                  detail. That is PLAN.md phase 6 ("Filter and transform it").
//
//   **group**      **REFUSED.** There is no `LayerKind::Group`: CONTEXT.md's
//                  seven kinds do not include one, `core::Layer::parent` has
//                  carried a group part name since Phase 4 and has never been
//                  acted on by anything, and io/NpaintFile.hpp already records
//                  that "groups have no native concept" in the format either --
//                  a group is a part with no image channels that this build
//                  cannot construct. Grouping a multi-selection is therefore
//                  not a selection feature at all: it is a new layer kind, a
//                  compositor that renders a group offscreen or folds it, a
//                  `parent` link that is finally honoured, and a writer that
//                  emits a channel-less part. Building a `parent` write here
//                  without the other three would produce documents whose layers
//                  claim membership in a group nothing composites -- a file
//                  that is wrong rather than incomplete.
//
// Both refusals are printed by `--selftest` on every run rather than living
// only here.
//
// ==========================================================================
// (2) THE SELECTION IS A SET OF **INDICES**, and why not ids
// ==========================================================================
//
// Step 12 gave `core::Layer` a stable `id` precisely so a *layer comp* could
// survive the stack changing underneath it, and core/Layer.hpp argues at length
// that an index would have been wrong there. This is the other case, and it
// goes the other way:
//
//   A comp persists. **A selection is consumed within one gesture.**
//
// `applyLayerSetOp()` receives a selection, validates every index against the
// document it was handed, and returns the selection the caller should adopt
// next. Nothing here outlives the call, so there is no window in which the
// stack can shift underneath a stored index -- the exact hazard ids exist for.
//
// And keying a selection by id would have a cost that is not hypothetical: ids
// are handed out **lazily**, by `normalizeLayerIds()`, which only
// `captureLayerComp()` calls. Every layer in a document that has never used a
// comp carries `id == 0`. A selection keyed by id would have to assign ids to
// the whole document the first time a user shift-clicked two rows, which would
// destroy step 12's stated property that "a document that never uses a comp
// carries zeros here for its whole life and nothing about it changes" -- and
// with it the byte-identity guarantee that property was built to make
// structural. A P2 convenience must not silently change what every save writes.
//
// The invariant is **sorted ascending and duplicate-free**, established by
// `makeLayerSelection()` and relied on by every ordering rule in section 3.
//
// ==========================================================================
// (3) ALL-OR-NOTHING, AND THE ORDER EACH VERB WALKS THE SET
// ==========================================================================
//
// **A set operation either applies to every member or to none of them.** No
// partial application, ever.
//
// The alternative -- apply to the members that accept and skip the ones that
// refuse -- was rejected on what it does to the user rather than on what it
// costs to write: "hide these five" that hides three produces a picture nobody
// asked for and nobody was told about, and the layer it silently skipped is by
// construction the one the user had a reason to protect. A refusal in this
// codebase is a sentence naming the layer (core/LayerOps.hpp); a partial apply
// is a sentence naming the layer *plus* a document that has already changed.
//
// **How atomicity is achieved is the load-bearing part, because it is what
// keeps the rules in one place.** The operation runs against a **copy of the
// `Document`**, one member at a time, through core/LayerOps' own setters; the
// first refusal discards the copy and returns that setter's own sentence. The
// rejected alternative was to *predict* the refusals -- check every member
// against a pre-flight predicate and only then mutate -- and that would have
// meant a second implementation of "is this layer locked", "does this clip have
// alpha below it", "is this half of a `Mix` pair". Two copies of a refusal rule
// is how a UI comes to grey out a control the model would have allowed, or
// worse, offer one it refuses.
//
// The copy is cheap **because of Phase 5 step 6**: `TileStoreOf`'s slots are
// `shared_ptr`s and its copy constructor shares them, so duplicating a
// `Document` costs one hash-map node per occupied tile (tens of bytes) rather
// than 128 KiB per tile. `--selftest` measures the copy against
// `unshareAll()` -- the deep copy this would have cost before step 6 -- and
// prints both. It is also exactly the cost model `core::History` already
// accepts on every single edit, so a set edit is not a new order of magnitude.
//
// **A successful set operation is ONE `recordLayerEdit()`**, hence one history
// entry and one undo. That is not a nicety: N entries for one gesture would
// make undo restore a state the user never saw -- three of five layers deleted
// -- which is a data-loss bug wearing an undo stack's clothes.
//
// **The order each verb walks the set, because three of them are order-
// sensitive and one of those is the classic bug:**
//
//   Delete      **descending**. Deleting {0,2,4} by walking upwards deletes 0,
//               which makes the old 2 the new 1 -- so the next delete takes the
//               wrong layer, and the third takes a third wrong one. Descending,
//               every index below the one being removed is still valid.
//               `--selftest` deletes {0,2,4} of five named layers and asserts
//               the two survivors by name, and runs the ascending version
//               beside it to print which layers it would have destroyed.
//   Duplicate   **descending**, same reason: a copy inserted at `i+1` shifts
//               everything above it.
//   Move Up     **descending**. Moving the topmost selected layer first leaves
//               room for the one under it.
//   Move Down   **ascending**, the mirror.
//   everything else   order-independent -- a property write touches one layer
//               and shifts nothing.
//
// ==========================================================================
// (4) LINKING: what a link propagates, which is one thing
// ==========================================================================
//
// **A link propagates geometry, and nothing else.** Aligning, distributing or
// translating any member of a link group moves the whole group, as one unit,
// by one delta. Reordering, deleting, hiding, locking, re-opacity-ing,
// re-blending and labelling do **not** propagate.
//
// The rule that decides it: a link must not become a second, invisible
// selection. A user who selects one row and presses Delete has said which layer
// to delete; if the link propagated deletion, the panel would be showing one
// highlighted row while the gesture consumed three. Geometry is different, and
// is the entire reason links exist in every editor that has them -- two layers
// are linked because they are *registered with each other*, and an operation
// that moved one and not the other would break precisely the relationship the
// user established. (This is Photoshop's split too, which matters only as
// evidence about what a user's hands already expect.)
//
// The *unit* is therefore the thing align and distribute operate on: each link
// group among the effective set is one unit with one bounding box and one
// delta; each unlinked selected layer is a unit of one. Two linked layers stay
// exactly as far apart after an align as before it, which is the property that
// makes the feature worth having and which `--selftest` asserts in pixels.
//
// **A group with fewer than two live members is not a link** -- core/Layer.hpp
// argues why the survivor of a deleted pair keeps its number rather than having
// it scrubbed. `linkedLayers()` below is the single place that turns a number
// into a set and it applies that rule, so no caller can see a dangling link.
namespace np {

// A multi-selection: indices into `Document::layers`, **sorted ascending and
// duplicate-free**. Section 2 argues indices over ids.
struct LayerSelection {
  std::vector<size_t> indices;

  bool empty() const noexcept { return indices.empty(); }
  size_t size() const noexcept { return indices.size(); }
  bool contains(size_t index) const noexcept;

  friend bool operator==(const LayerSelection&, const LayerSelection&) = default;
};

// The only way to build one: sorts and de-duplicates, so the invariant is a
// property of the type rather than of every call site. A panel that appends the
// same row twice on a shift-click cannot produce a selection that deletes a
// layer twice.
LayerSelection makeLayerSelection(std::vector<size_t> indices);

// Convenience for the single-selection case a panel starts in.
LayerSelection singleLayerSelection(size_t index);

// --- Links ------------------------------------------------------------------

// Every index in `doc` that shares `doc.layers[index]`'s link group, including
// `index` itself, ascending. **Returns just `{index}` when the layer is
// unlinked or is the only live member of its group** -- section 4's rule, in
// the one place that implements it.
std::vector<size_t> linkedLayers(const Document& doc, size_t index);

// True when `index` is in a group with at least one other live member, i.e.
// when a panel should draw a link badge on the row.
bool layerIsLinked(const Document& doc, size_t index);

// The group number `Link` will hand out next: one above the highest present.
//
// Max-plus-one rather than `Document::nextLayerId`'s stored counter, and the
// asymmetry is deliberate and safe. core/Document.hpp rejects max-plus-one for
// `Layer::id` because a **comp** stores an id and would silently re-target a
// re-issued one. Nothing outside `Document::layers` ever names a link group --
// no comp, no file attribute other than the layers' own, no panel state that
// outlives a frame -- so a number one above every number present cannot collide
// with anything that still refers to it, and the counter that would otherwise
// have to be persisted and undone does not need to exist.
uint64_t nextLinkGroupId(const Document& doc);

// The selection plus every live link partner of every member, sorted and
// de-duplicated. This is what align, distribute and translate act on;
// everything else acts on the selection as given (section 4).
LayerSelection expandSelectionByLinks(const Document& doc, const LayerSelection& sel);

// --- The commands -----------------------------------------------------------

// Every gesture the multi-selection offers, in menu order. Flat rather than a
// verb plus parameters, for `app::LayerCommand`'s reason: `allLayerSetCommands()`
// is what both the `Layer` menu and the LAYERS panel walk, so a command that is
// not in this enum is a command exactly one of them can reach, and that is the
// failure app/LayerEditor.hpp exists to have fixed once.
enum class LayerSetCommand {
  // Structure. Section 3 gives the order each walks the set.
  DeleteLayers,
  DuplicateLayers,
  MoveLayersUp,
  MoveLayersDown,
  // Flags, as a set. Show/Hide rather than one Toggle, because a toggle over a
  // mixed selection has no defensible meaning -- half the rows would invert and
  // the user would have to look to find out what happened.
  ShowLayers,
  HideLayers,
  LockLayers,
  UnlockLayers,
  ClipLayers,
  UnclipLayers,
  // PRD C15's linking half. Section 4.
  LinkLayers,
  UnlinkLayers,
  // PRD C15's colour labels. One command per label, because setting a label is
  // a gesture with no value attached in exactly `app::LayerCommand`'s sense --
  // the label *is* the menu item, the way "New Pigment Layer" is.
  LabelNone,
  LabelRed,
  LabelOrange,
  LabelYellow,
  LabelGreen,
  LabelBlue,
  LabelViolet,
  LabelGrey,
  // PRD C13. Twelve entries rather than an edge plus a target parameter, for
  // the flatness reason above: a menu walks this list.
  AlignSelectionLeft,
  AlignSelectionCenterX,
  AlignSelectionRight,
  AlignSelectionTop,
  AlignSelectionCenterY,
  AlignSelectionBottom,
  AlignCanvasLeft,
  AlignCanvasCenterX,
  AlignCanvasRight,
  AlignCanvasTop,
  AlignCanvasCenterY,
  AlignCanvasBottom,
  DistributeHorizontally,
  DistributeVertically,
};

const std::vector<LayerSetCommand>& allLayerSetCommands();

// Menu text: "Delete Layers", "Align Left (to selection)". Title case.
const char* layerSetCommandLabel(LayerSetCommand command) noexcept;

// Whether the command can be offered at all for this selection --
// `app::layerCommandAvailable()`'s rule exactly, one level up: **availability,
// not permission.** Unavailable means the gesture is meaningless for this
// selection (Move Up with the top layer in it, Align to selection with one
// layer, Distribute with two). Every other reason a command may not go ahead --
// a lock, a clip with no alpha below it, a `Mix` pair, an empty layer with no
// edges -- is a **refusal**, offered and answered with a sentence.
bool layerSetCommandAvailable(const Document& doc, LayerSetCommand command,
                              const LayerSelection& sel);

struct LayerSetOpResult {
  bool ok = false;
  // The refusing setter's own sentence, verbatim, or this file's for the
  // refusals it owns. Empty when `ok`.
  std::string error;
  // What went ahead but is worth saying: the rounding an align or a distribute
  // had to do, because core/LayerGeometry's translate is integer-only and a
  // centre can land on a half pixel. Empty when nothing was rounded.
  std::vector<std::string> warnings;
  // What a caller records; `app::recordLayerEdit()`'s input. Empty when !ok.
  std::string editLabel;
  // Where the selection lands. Section 3 gives each verb's answer.
  LayerSelection selection;
};

// Applies one command to the whole selection, atomically. Section 3 is the
// contract.
//
// `doc` is **unchanged** on refusal -- not "mostly unchanged", not "unchanged
// except for the members that had already gone through": the operation runs on
// a copy and the copy is discarded.
LayerSetOpResult applyLayerSetOp(Document& doc, LayerSetCommand command,
                                 const LayerSelection& sel);

// --- The two value-carrying set setters --------------------------------------
//
// app/LayerEditor.hpp's boundary, applied to a set: a `LayerSetCommand` is a
// gesture with no value attached, so the opacity slider and the blend dropdown
// are not commands. They are these, and they are here rather than in the panel
// for the one reason the commands are -- **the atomic trial and the single
// history entry**, which a panel looping `setLayerOpacity()` over N rows would
// get wrong in both directions at once.
LayerSetOpResult setLayerSetOpacity(Document& doc, const LayerSelection& sel, float opacity);
LayerSetOpResult setLayerSetBlend(Document& doc, const LayerSelection& sel, BlendMode mode);

}  // namespace np
