#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Tile.hpp"

// core/DirtyTiles -- **what changed**, at tile granularity, between two
// snapshots of one document.
//
// ==========================================================================
// What this closes
// ==========================================================================
//
// `OpenDocument::revision` is a whole-document counter. It says *that*
// something changed and never *what*, so ui/DocumentTexture's cache -- keyed
// on `(id, revision, width, height)` -- has exactly two answers: the frame is
// free, or the whole canvas is recomposited. Its own section of `--selftest`
// measures the second one at **22 ms for a 1024x1024 document and 89 ms for a
// 2048x2048 one**, against a cache hit's 1.0 ns.
//
// **PRD F3 (P0) is the requirement, and its second clause is this module in
// one sentence**: "Pen-to-photon latency under 20 ms; **the in-progress
// stroke does not wait on a full document re-composite**." A 22 ms composite
// exceeds the whole pen-to-photon budget on its own before the solver, the
// upload or the present have run; an 89 ms one exceeds it more than four
// times over. F3 does not merely make this desirable, it forbids the
// alternative by name.
//
// **This is not the same claim as "60 fps", and that distinction is worth
// keeping.** F3's 20 ms is pen-to-photon -- input event to displayed frame --
// not a compute budget, so a composite that fits inside 20 ms of CPU time has
// still spent the entire budget and left nothing for anything else. Any "% of
// budget" figure this codebase prints against F3 should be read that way.
// (Both of those places once attributed a 16.67 ms frame budget to PRD A1.
// **A1 is the no-document-open memory requirement and the PRD sets no frame
// budget at all** -- the citation was invented. Both now cite F3.)
//
// The second consumer is the reason this lands now rather than later: the
// layers panel makes toggling visibility, opacity and blend constant, and
// **the stroke bridge would pay a full recomposite on every dab**.
//
// This header answers the localisation question. core/Composite's
// `compositeDocumentTilesPremultiplied()` is the other half (it composites a
// tile set instead of a canvas), and ui/DocumentTexture is the caller that
// puts the two together and uploads only the rectangles that moved.
//
// ==========================================================================
// 1. How a change is localised: copy-on-write slot identity
// ==========================================================================
//
// The dirty set is derived from **tile identity against a held snapshot**, and
// nothing else. `TileStoreOf` (core/TileStore.hpp) holds each tile in a
// `std::shared_ptr<T>` slot; `find()` returns that slot's address. Copying a
// store shares every slot. So:
//
//     the snapshot's find(c) == the current store's find(c)
//         <=>  the two stores still refer to the SAME tile object.
//
// `documentDirtyTiles(before, after)` walks both stores per layer and marks a
// coordinate dirty when the two addresses differ, when the coordinate exists
// in one store and not the other, or when either store's presence itself
// changed. That is three cheap pointer comparisons per occupied tile and no
// texel is read at all.
//
// **Three alternatives were considered and each is worse for a stated
// reason.**
//
//   a dirty set maintained by the mutation path
//       core/TileShare.hpp already refused this, in these words: "a dirty set
//       has to be *cleared* by something, and the thing that would clear it
//       is a journal flush or a history commit, neither of which exists at a
//       granularity finer than the document. A set nothing clears is a set
//       that is always full." That is still true, and there is now a second
//       reason: the *display* is not the only consumer. A journal, an
//       incremental save and a second view would each want their own clear
//       point, so a single set on the store would have to become a set per
//       consumer -- which is exactly what "each consumer holds its own
//       snapshot" already is, without a write barrier that has to know how
//       many consumers exist.
//
//   a per-layer revision counter
//       Cheap, but it is `OpenDocument::revision`'s problem moved down one
//       level: it says *that* a layer changed, so a one-dab edit still
//       recomposites that layer's every tile. It also cannot see the case
//       that matters most here -- a stroke crossing two tiles of one layer --
//       and it would need a mutator to bump it, which `TileStoreOf` has no
//       hook for (`getOrCreate()` hands out a `T&`; the write happens later,
//       through the reference, outside the store's sight).
//
//   hashing tile contents
//       Complete, and 128 KiB of hashing per occupied tile per frame -- for a
//       256-tile document that is 32 MiB read per frame to discover that
//       nothing changed, which is the cost this module exists to remove.
//
// ==========================================================================
// 2. What makes the set COMPLETE, precisely
// ==========================================================================
//
// A missed dirty tile is a stale pixel that nothing ever corrects, so this
// is the load-bearing paragraph of the module.
//
// **Claim.** Let `S` be a snapshot of a document -- a `Document` copy, so
// every tile slot is shared with the live document at the moment it is taken.
// Then for every coordinate `c` of every store, either the live store's
// `find(c)` still returns S's pointer *and* the tile's bytes are unchanged
// since the snapshot, or the pointers differ (or `c` is present in only one
// of the two).
//
// **Proof.** There are exactly two ways to obtain a writable tile from a
// `TileStoreOf`, and core/TileStore.hpp enumerates them and closes the set --
// `find()` is const-only and iteration is const-only, precisely so that this
// enumeration can be closed. Both ways (`getOrCreate`, `findForWrite`) pass
// through `unshare()`, which replaces the slot with a fresh copy whenever
// `use_count() > 1`. The snapshot is an owner, so at snapshot time every
// coordinate it holds has `use_count() >= 2`. Therefore the **first** barrier
// call after the snapshot must copy, and the live store's pointer differs
// from S's from that moment on. Later writes to the same coordinate may
// happen in place -- the pointer is already different, so the tile is already
// in the dirty set. A coordinate created after the snapshot is absent from S
// and present in the live store, and is caught by the presence test. A
// coordinate removed after the snapshot is caught by the same test in the
// other direction (`TileStoreOf` has no erase, so this cannot happen today;
// the test is there so it never becomes a hole).
//
// **The one leak, named rather than hidden.** The proof depends on every
// write passing the barrier *after* the snapshot was taken. core/TileStore.hpp
// already states the one caller rule that can break that: "a `T&` obtained
// from a barrier stays a live write handle to the tile it named, so copying
// the store *after* taking the reference and writing *through* it afterwards
// writes into a tile the copy now shares. Take the reference, write, then
// copy -- never the other order." A write through such a stale reference
// mutates the snapshot too, so no pointer moves and the tile is missed.
//
// Two things make that unreachable here rather than merely unlikely, and
// `--selftest` demonstrates the leak on a hand-built fixture so that the
// argument is a measured one:
//
//   * **The snapshot is taken by the code that composites**, at the moment it
//     composites (ui/DocumentTexture::viewFor), never by the caller and never
//     early. There is no window in which a caller holds a reference across it.
//   * Every writer in this build holds its reference for the duration of one
//     texel or one tile fill with no store copy in between -- the enumeration
//     is in core/TileStore.hpp and it is five call sites long.
//
// **What is NOT part of the claim, and does not need to be:** that the
// revision counter moved. If it did not, ui/DocumentTexture never asks this
// module anything -- it returns the cached view, which is the pre-existing,
// documented trap ("writing tiles directly, without going through
// `recordEdit()`"). This module does not widen it and does not narrow it.
//
// ==========================================================================
// 3. Which changes are NOT tile-local
// ==========================================================================
//
// Tile identity localises a change to *storage*. It cannot localise a change
// to a layer's **properties**, because those act on every texel the layer
// covers rather than on a tile someone painted in -- and getting that wrong
// the optimistic way puts a stale rectangle on screen that nothing repairs.
// So the classification is explicit, and the conservative direction is always
// "recomposite more": a wrong full recomposite costs 22 ms once, a wrong
// incremental one is a bug that never heals.
//
// **Every member of `Layer` is accounted for below.** A member is either
// compared here (a difference forces a full recomposite) or it is named as
// one the compositor provably never reads. That table is the maintenance
// contract of this module: giving core/Composite a new input means adding it
// here.
//
//   kind          compared -- it selects the whole projection path.
//   visible       compared -- PRD C3, whole-layer coverage -- but narrowed to
//                 that layer's own occupied tiles rather than the whole
//                 canvas when the layer holds pixels and is not an Adjustment
//                 layer, per §4's "no op reads a neighbour". Left at a full
//                 recomposite for an Adjustment layer (its stack reaches
//                 everything below it, which is not its own footprint) and
//                 for a layer entangled in a Pigment `Mix` pairing (the pair
//                 composites over the union of BOTH layers' tiles, which
//                 neither layer's own footprint covers -- see
//                 `documentDirtyTiles()`'s own comment on why that case is not
//                 narrowed instead of guessed at).
//   opacity       compared -- likewise, and the layer editor's hottest knob;
//                 narrowed under the identical conditions as `visible`.
//   blend         compared -- resolved once per layer; narrowed under the
//                 identical conditions, with one more: a blend of `mix` is
//                 itself what makes a Pigment pairing exist at all, so a
//                 change into or out of it is exactly the entanglement case
//                 above and stays a full recomposite.
//   clipped       compared -- changes which layer composites which. Narrowed
//                 to the layer's own tiles UNION its clip base's (found via
//                 `clipRuns()`, checked on both `before` and `after` since the
//                 flag is flipping) under the same holds-pixels/not-Adjustment
//                 condition -- core/Composite.hpp §17: "the base's tiles are
//                 the clipping run's whole extent", so nothing outside that
//                 union can move. Also stays a full recomposite when the flag
//                 flip changes a Pigment `Mix` pairing's eligibility
//                 (`blendModeAvailableForLayer()` refuses a pair when either
//                 half is clipped), for the identical reason `blend` does.
//   ops           compared **structurally**, entry by entry, not by
//                 `OpStack::version()`. Version is monotonic per stack and
//                 would be a complete detector for one layer over time, but
//                 a *reorder* puts a different layer's stack at an index, and
//                 two different stacks can carry the same version. Structural
//                 comparison has no such hole and an op stack is a handful of
//                 PODs.
//   rgbTiles      presence compared; contents diffed by slot identity.
//   pigmentTiles  presence compared; contents diffed by slot identity.
//   mask          presence compared; contents diffed by slot identity. The
//                 presence test is separate from the content diff because
//                 `std::nullopt` and an empty mask store are different things
//                 (core/Mask.hpp) and the compositor takes different paths.
//   name          **not compared, and the one member that needs a caveat.**
//                 No *texel* depends on it, so a rename is an empty edit: the
//                 revision moves, the dirty set is empty, nothing is
//                 recomposited and nothing is uploaded. But the compositor
//                 does read it -- `unimplementedBlendWarning()` and its two
//                 siblings name the layer in their sentences -- so a caller
//                 collecting warnings must still recompute them on an empty
//                 edit. ui/DocumentTexture does exactly that, with an empty
//                 tile set (see `compositeDocumentTilesPremultiplied()`), for
//                 the cost of one pass over the layer list and no tile access
//                 at all. `--selftest` asserts the bit-identity and the zero
//                 tile count rather than leaving either to be inferred.
//   locked        **not compared.** Enforced by core/LayerOps' refusals only;
//                 the compositor never mentions it.
//   parent        **not compared.** Still carried and never acted on
//                 (core/Layer.hpp); this build creates no groups. **If groups
//                 are ever composited, this member joins the compared list.**
//
// And at the document level:
//
//   width/height  compared -- they decide the texture, not just its contents.
//   workingSpace  compared, although today no compositor path reads it: it is
//                 one float array per document and the conservative direction
//                 is free here.
//   layers.size() compared -- an add or a remove shifts every index, so index
//                 `i` would be comparing two different layers.
//
// **Reordering.** There is no layer identity to compare, so a reorder is
// caught by the per-index property comparison: swapping two layers that
// differ in any compared property forces a full recomposite. Two layers that
// agree on *every* compared property are interchangeable except for their
// tiles, and the per-index tile diff then reports exactly the coordinates
// where the two stacks' contents differ -- which is exactly where the
// composite moved, because a texel's composite depends only on that texel's
// storage in each layer (§4). `--selftest` runs a reorder of two layers whose
// properties are identical and one of layers whose properties are not, and
// asserts bit-identity for both.
//
// ==========================================================================
// 4. Why a tile is the right unit at all
// ==========================================================================
//
// Because **the composite is texel-local given the layer properties.** Every
// branch of core/Composite's walk reads storage at document coordinate `p`
// and writes the accumulator at `p`: `blendInto()` writes the coordinate it
// was handed, an Adjustment layer transforms `out[p]` from `out[p]`, a mixed
// pair reads both layers at `p`, and a clipping group folds its members at
// the base's own `p`. No op in this build reads a neighbour -- `OpClass`
// `SpatialB` has no implementation anywhere (core/OpStack.hpp), which is the
// single assumption that would have to be revisited if it ever gains one.
//
// So the composited texel at `p` is a function of the layer properties and of
// each store's texel at `p`; grouping `p` by tile is then free, because tiles
// are what the stores are keyed by and what the barrier copies.
//
// ==========================================================================
// 5. Cost, and the one it adds
// ==========================================================================
//
// The diff is O(occupied tiles) pointer comparisons and allocates one vector.
// What it *adds* is the snapshot: a `Document` copy, measured by `--selftest`'s
// `cow tiles` section at 11 us and +0.0 MiB for a 256-tile / 32.0 MiB
// document, and one 128 KiB tile copy the first time each tile is written
// after a snapshot.
//
// **That tile copy is very nearly free, because it is already being paid.**
// `OpenDocument::recordEdit()` appends a `core::Document` copy to `History` on
// every edit (app/DocumentLifecycle.hpp), so every tile in an edited document
// already has `use_count() >= 2` and the next write already copies. A second
// snapshot moves the count from 2 to 3 and changes nothing about how many
// copies happen. `--selftest` measures the first-touch cost with and without
// this module's snapshot and prints both.
namespace np {

// Why a change could not be localised. Named rather than boolean because the
// answer is what a reader of a slow frame needs, and because `--selftest`
// asserts the *specific* reason rather than merely that a full recomposite
// happened -- an assertion on "it was full" would pass for the wrong reason.
enum class FullRecompositeReason {
  None,
  NoPreviousComposite,
  CanvasSizeChanged,
  WorkingSpaceChanged,
  LayerCountChanged,
  LayerKindChanged,
  LayerVisibilityChanged,
  LayerOpacityChanged,
  LayerBlendChanged,
  LayerClipChanged,
  LayerOpsChanged,
  LayerMaskPresenceChanged,
  LayerStoragePresenceChanged,
};

const char* fullRecompositeReasonName(FullRecompositeReason reason) noexcept;

// One sentence naming what forced a full recomposite, the layer it happened
// on, and what the alternative would have cost -- the io/Export refusal style
// applied to a performance decision, so a slow frame can say why it was slow.
// Empty when `reason == None`.
std::string fullRecompositeExplanation(FullRecompositeReason reason, size_t layerIndex);

struct DocumentDirtyTiles {
  // True when the change is not tile-local and the whole canvas must be
  // recomposited. `tiles` is then empty and meaningless -- callers must test
  // this first.
  bool everything = false;
  FullRecompositeReason reason = FullRecompositeReason::None;
  // The layer that forced `everything`, when the reason is a per-layer one.
  // `layers.size()` of the *after* document otherwise.
  size_t layerIndex = 0;

  // The tiles whose composited texels may have moved, ascending by (y, x) so
  // that two runs over the same edit produce the same order and the upload
  // walks the texture in rows. Unique. Not clipped to the canvas: a store may
  // hold a tile that lies entirely outside it, and core/Composite clips.
  std::vector<TileCoord> tiles;

  bool empty() const noexcept { return !everything && tiles.empty(); }
};

// What moved between two snapshots of the **same** document.
//
// `before` must be a snapshot taken by copying `after`'s document (directly or
// transitively) -- that is what §2's completeness argument rests on. Handing
// it two unrelated documents is not an error and not detectable here; it
// simply reports every tile of both as dirty, which is conservative and slow
// rather than wrong.
DocumentDirtyTiles documentDirtyTiles(const Document& before, const Document& after);

// Every tile coordinate the canvas rectangle touches, ascending by (y, x) --
// the dirty set a full recomposite is equivalent to. Empty for a non-positive
// canvas.
std::vector<TileCoord> canvasTiles(const Document& doc);

// How many tiles the canvas rectangle touches, without building the vector.
size_t canvasTileCount(const Document& doc) noexcept;

// **Whether to recomposite the whole canvas even though the change WAS
// localised**, because at this many dirty tiles the incremental path stops
// winning.
//
// --- The crossover was predicted at about a third of the canvas, and it is
//     not there -------------------------------------------------------------
//
// The incremental cost is `S + n*t`: a per-call setup `S` (the pairing pass,
// the clip pass, and one `layerPointOps()` per layer -- all O(layers) and none
// of it per tile) plus `t` per dirty tile. The full cost is `S + canvasTiles*t`
// **plus** allocating and zeroing a canvas of floats, packing a canvas of f16
// and uploading it. So the crossover has no reason to sit *below*
// `canvasTiles`, and measurement says it does not: `--selftest` fits both
// curves on a 1024x1024 two-layer document and lands at **107-109% of the
// canvas**, end to end as well as for the composite alone. At every dirty
// count from 1 tile to all 64, the incremental path is the cheaper one.
//
// So the fraction is **1.0**: the full path is taken only when the dirty set
// already covers the canvas -- where the two do the same compositing work and
// the full path is one `wgpuQueueWriteTexture` instead of one per tile.
// Setting it lower would have been the conservative-looking choice and would
// have cost real time: at half a canvas of dirty tiles the measured
// incremental update is 10.8 ms against the full path's 22.8 ms.
//
// **This is a performance constant and not a correctness one**, which is why
// it can be set from a measurement at all. Both paths produce identical bytes
// (core/Composite.hpp's region section, asserted at zero tolerance across ten
// kinds of edit), so a wrong answer here is a slow frame in one direction and
// a slightly slower one in the other -- never a stale pixel. The direction
// that *would* be a stale pixel is a dirty set that is too small, and that is
// §2's problem, not this one's.
//
// A fraction of the canvas rather than an absolute tile count because both
// terms scale with the canvas: a 16-tile document and a 256-tile document have
// different answers in tiles and the same answer in fractions. `--selftest`
// re-measures the crossover on every run and prints it beside this constant,
// so a machine or a document that moves it says so.
inline constexpr double kFullRecompositeTileFraction = 1.0;

bool preferFullRecomposite(size_t dirtyTiles, size_t canvasTiles) noexcept;

}  // namespace np
