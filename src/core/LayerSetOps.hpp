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
// C12 names five verbs. Four are built here (as of this step) and one is
// refused by name, in core/LayerComp.hpp's style -- stated where a reader
// will look for them rather than left to be discovered as an absence:
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
//   **group**      **BUILT** (PLAN.md Phase 5's C7/C12 follow-on). This
//                  section used to say "REFUSED", and the argument for the
//                  refusal was that grouping "is not a selection feature at
//                  all: it is a new layer kind, a compositor..., a `parent`
//                  link that is finally honoured, and a writer that emits a
//                  channel-less part" -- naming all four things this step had
//                  to build before a single `LayerSetCommand` could exist
//                  honestly. All four are now real: `LayerKind::Group`
//                  (core/Layer.hpp), `core::groupCoverage()`
//                  (core/Composite.hpp, a **pass-through** fold -- see its own
//                  comment for why not an isolated one), `Layer::parent`
//                  resolved against `Layer::groupTag` rather than left inert,
//                  and io/NpaintFile's Group part (one dummy channel, exactly
//                  Adjustment's `buildAdjustmentLayerPart()` shape -- a
//                  channel-less part is provably unwritable through this
//                  OpenImageIO, measured at the Adjustment step and true here
//                  for the identical reason). `GroupLayers`/`UngroupLayers`
//                  below are the two commands; see this file's own
//                  implementation for the span-splice that keeps nesting
//                  correct and the order-preservation `--selftest` proves.
//
// The one refusal that remains (`transform`, above) is printed by
// `--selftest` on every run rather than living only here.
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
//
// ==========================================================================
// (5) GROUPING: the span-splice, nesting, and why ungroup cannot reorder
// ==========================================================================
//
// **A layer's group membership is `Layer::parent`, and `GroupLayers` is the
// only thing in this build that writes a non-empty one.** core/Composite.hpp
// argues the pass-through/isolated choice; this section is the mechanics of
// building and undoing the link, which is where the "classic bug" lives (an
// ungroup that reverses its children).
//
// **`GroupLayers` moves layers; it does not merely tag them.** The selected
// layers are collected into one **contiguous run**, positioned where the
// topmost selected layer was, and the new Group layer is placed directly
// above that run (`doc.layers` is bottom-to-top, so "above" is the higher
// index -- the one nearer the top of the panel). Two consequences follow and
// both are deliberate:
//
//   * **A group's members are always exactly the contiguous run directly
//     below it.** This is an invariant every mutator here maintains, not
//     merely usually true -- `ungroupLayers()`'s own correctness rests on it,
//     because it lets "find this group's members" be a downward scan rather
//     than a document-wide search keyed on `parent` alone (which a stray,
//     non-contiguous same-tag entry from a hand-built fixture could fool).
//   * A selection is refused, by name, if any member already carries a
//     non-empty `parent` -- **grouping a layer that is already grouped is
//     refused**, rather than silently re-parenting it or silently leaving it
//     where it is. The fix the refusal names is "ungroup it first". This
//     keeps the contiguity invariant provable without a second, recursive
//     case: a selected **Group** layer's whole span (itself plus its own
//     contiguous members, however deeply those nest) moves as one opaque
//     block, so **nesting falls out of moving spans rather than needing its
//     own code path** -- grouping an existing group alongside an ordinary
//     layer nests it one level deeper, and nothing inside that span is
//     touched, read, or re-validated in the process.
//
// **Locked layers refuse `GroupLayers`**, core/LayerOps.hpp's own reason for
// refusing `moveLayer()`: grouping relocates a layer's position in the stack
// exactly as a move does, and "a locked layer's ... own place in the stack
// is frozen" would be a decoration if grouping could move one anyway.
//
// **The post-condition that cannot arise from a `moveLayer()` call, so it is
// checked here instead:** after the splice, a **clipped** layer must not have
// landed at index 0 -- `core::setLayerClipped()` and `moveLayer()` both
// refuse to create that state directly (core/Layer.hpp), and a span-splice
// that bypasses both must not become the back door into it. Checked once,
// after the whole scratch document is built, and refused by name if it
// happened -- the same "the atomic trial and the single history entry"
// discipline section 3 already applies to the delete/duplicate/move family.
//
// **Cycles cannot arise from `GroupLayers` alone.** A new group's tag does
// not exist until the group is created, so nothing already in the document
// can reference it, and the operation never re-parents an *existing* Group
// layer's own `parent` (it only ever sets it on a layer being newly wrapped).
// core/Composite.hpp's `groupAncestry()` is nonetheless written to survive a
// cycle it did not create -- a hand-built `Document`, or a `.npaint` this
// build did not write, is not bound by what this file's own operations would
// ever produce, and PRD I10 material can carry exactly that.
//
// **`UngroupLayers` is the mirror of `GroupLayers`' own extract-then-splice,
// and it is where the "classic bug" -- an ungroup that reverses or reorders
// its children -- actually lives.** It locates the group's contiguous member
// span (the same downward scan `GroupLayers` relies on), reparents each
// member's `parent` to the group's own `parent` (nesting-aware promotion -- a
// member of a nested group becomes a member of the **outer** group, not a
// top-level layer), then **extracts the whole span and reinserts it, in the
// same relative order, at the position the group occupied**. The extract and
// reinsert are not load-bearing for correctness on their own -- a group's
// members are already contiguous and already in their final relative order,
// so erasing just the one group entry in place would reach the same result
// -- but the explicit span move is what `GroupLayers` itself does, symmetric
// code for the symmetric operation, and it is deliberately kept rather than
// simplified away: it is the one place in this pair of commands where writing
// the member list in the wrong order (`rbegin()`/`rend()` instead of
// `begin()`/`end()`, the single-character mistake this docstring exists to
// name) reverses the document a user sees. `--selftest` asserts the ordering
// end to end -- group a non-contiguous selection, ungroup it, and check the
// members come back in the SAME relative order they went in, not reversed --
// and the sabotage proof breaks exactly that one call.
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
  // PRD C7/C12. Section 5 gives the span-splice, the locked/clipped guards,
  // and the ordering argument.
  GroupLayers,
  UngroupLayers,
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

// A Lucide codepoint for the eight selection-relative align/distribute
// commands (the icon toolbar `ui/MacPaintUI.cpp` draws, Figma's own
// presentation of the same operations), 0 for the other 26. PUA codepoints
// from `third_party/lucide`, verified against the vendored font's own cmap
// (`lucide.ttf`) rather than guessed. Returned as a raw codepoint, not a
// UTF-8 glyph string: the drawing side (`ui/MacPaintUI.cpp`'s
// `layerSetCommandIconButton()`) must render it through `drawToolGlyph()` at
// `ui/Fonts.hpp`'s fixed `kToolIconSizePx` -- the same reason
// `toolFlyoutRow()` there hand-draws instead of embedding the glyph in an
// `ImGui::SmallButton` label: a merged Lucide glyph is only proven to bake at
// that one fixed size, and drawing it as plain widget-label text at whatever
// size the ambient font happens to be is a different, unproven bake. A raw
// codepoint makes that the only path available, rather than a string that
// invites re-deriving the codepoint by hand at the call site (or worse,
// tempting a second `ImGui::SmallButton(glyphString)` call). `AlignCanvas*`
// intentionally shares no icon of its own: it is the same six operations
// against a different reference frame, and the toolbar's label ("to the
// selection's own bounds") is what disambiguates, not a second icon set six
// users would have to learn.
uint32_t layerSetCommandIconCodepoint(LayerSetCommand command) noexcept;

// The nonzero codepoints `layerSetCommandIconCodepoint()` can return, for the
// Lucide font merge (`installToolIconFont()`, `ui/Fonts.hpp`) -- the exact
// role `toolIconCodepoints()` (`ui/AtelierChrome.hpp`) plays for the tool
// palette, which is why this is a second, INDEPENDENT list rather than an
// addition to that one: `toolIconCodepoints()`'s whole contract is "walks
// `kToolMeta`", and `LayerSetCommand` is not a `Tool`. `main.cpp` merges both
// lists into one `installToolIconFont()` call.
const std::vector<uint32_t>& layerSetCommandIconCodepoints();

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
