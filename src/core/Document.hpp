#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "color/Space.hpp"
#include "core/Channels.hpp"
#include "core/Layer.hpp"
#include "core/LayerComp.hpp"

// core/Document (PLAN.md "Phase 2 -- See a file", step 4; CONTEXT.md
// Relationships: "A Document holds an ordered list of Layers and a Working
// space"). Canvas dimensions, a working space, an ordered layer list, and
// the one factory PLAN.md's step 5 (PRD C7) asks for: createBlank().
namespace np {

// The pigment basis this build fits latents in (PRD C8: "the file records
// which pigment basis produced them"). core/Pigment's `latentToRgb()` is
// Mixbox's own 20-term polynomial over Mixbox's own three stored pigment
// weights, and paint/MixboxLut's 512x512 texture is the inverse of exactly
// that map -- so "mixbox-v1" is not a label chosen here, it names the model
// core/Pigment implements.
//
// Declared in `core/` rather than in io/NpaintFile because it is the default
// of the member below, and a default has to live where the member does.
// io/NpaintFile's `kNpaintPigmentBasis` -- the format's name for the same
// string, which docs/document-format.md's own example uses -- is defined *as*
// this constant rather than as a second spelling of "mixbox-v1", so the
// document's claim and the attribute the writer stamps cannot drift apart.
inline constexpr const char* kPigmentBasisMixbox = "mixbox-v1";

struct Document {
  int32_t width = 0;
  int32_t height = 0;
  WorkingSpace workingSpace;

  // **Which pigment basis this document's latents are fitted in** (PLAN.md
  // Phase 5 step 15; PRD C8 (P1), "Pigment latents survive save/load; the
  // file records which pigment basis produced them").
  //
  // Sits immediately below `workingSpace` because it is the same kind of fact
  // about the same pixels: a working space says what a Layer's RGB numbers
  // mean, and a pigment basis says what a Pigment layer's `pig.c0/c1/c2` mean.
  // Neither is recoverable from the numbers themselves, and a document that
  // has lost either one holds pixels nobody can interpret.
  //
  // **A `std::string` and not an enum**, and that is the whole point of the
  // field. An enum could only hold bases this build has heard of, and the
  // case the field exists for is the opposite one: a file written by a future
  // build declaring `km2-v1`, whose latents are perfectly good data this build
  // cannot interpret. The value is carried verbatim -- never mapped onto a
  // nearest known basis, never silently replaced with this build's -- exactly
  // the rule `Layer::blend` follows for a blend mode this build cannot
  // composite.
  //
  // **On `Document` and not on `app::OpenDocument`.** The basis describes the
  // pixel data, so it has to travel with the pixel data:
  //
  //  * `core::History` entries hold a whole `Document` **by value**
  //    (core/History.hpp), so undo restores this alongside the tiles it
  //    describes. On the session record it would sit outside every history
  //    entry, and an undo that gave back foreign latents would give them back
  //    under whatever label the session happened to be holding -- the numbers
  //    and the label disagreeing is precisely the failure this field exists to
  //    prevent, so putting it where undo cannot reach it would defeat it.
  //  * `saveNpaint(const Document&, ...)` takes a `Document`. A basis on the
  //    open-document record would have to be threaded through as a further
  //    argument to be stamped at all -- the same argument `comps` makes below.
  //
  // The cost of that placement is one `std::string` per history entry, and it
  // is a copy rather than a share. That is affordable for a measured reason
  // rather than an assumed one: "mixbox-v1" is 9 bytes, inside libc++'s
  // short-string capacity, so copying a history entry does **not** allocate
  // for this member; the entry grows by `sizeof(std::string)` and nothing
  // else. `--selftest` asserts that rather than trusting it.
  //
  // Never empty in a document this build produces. io/NpaintFile refuses to
  // save an empty one by name rather than writing a file that declares no
  // basis at all -- an absent `np:basis` and an empty one are indistinguishable
  // in an EXR header, because this OpenImageIO drops empty string attributes
  // (io/NpaintFile.hpp measures it).
  //
  // **The invariant every writer of latents owes this field**: code that
  // deposits new `PigmentTexel`s into a layer must check that this build's
  // basis is the document's before it does, or the document ends up holding
  // latents from two bases under one label. Nothing in the paint path checks
  // it today -- see io/NpaintFile.hpp's basis section for what that leaves
  // open and what closes it.
  std::string pigmentBasis = kPigmentBasisMixbox;

  // Ordered, **bottom to top**: index 0 is the bottom of the stack
  // (DESIGN-imaging.md §3's `Layer[]` diagram, and docs/document-format.md's
  // "Part order is layer order, bottom to top, after part 0" -- io/NpaintFile
  // writes `layers[0]` as the first layer part, so this is the file format's
  // order, not a local convention). core/Composite walks it front to back for
  // that reason; app/LayerPanel is the single place that reverses it for
  // presentation, because a layers panel shows the top layer first.
  //
  // Multi-layer is real as of Phase 5 step 1: `core/LayerOps` adds, removes,
  // reorders and duplicates entries here, and `core/Composite` composites
  // them with `over`. Starts empty; `createBlank()` below, `placeImageAsLayer()`
  // and opening a file populate it. Still an ordinary vector with no
  // invariants of its own -- see core/LayerOps.hpp on why the operations are
  // free functions rather than methods that would only half-encapsulate it.
  std::vector<Layer> layers;

  // **Layer comps** (PLAN.md Phase 5 step 12; PRD C14), in the order the panel
  // lists them. Named sets of per-layer state, restorable in one click.
  //
  // On `Document` and not on `app::OpenDocument`, which is the one placement
  // decision here and follows from C14's own wording -- "persisted in the
  // document". Two consequences make it the right one rather than the
  // convenient one:
  //
  //  * `core::History` entries hold a whole `Document` (core/History.hpp), so
  //    capturing, renaming, deleting and reordering a comp are undoable for
  //    free and **restoring one is itself an undoable edit**, which is what
  //    `History::restoreSnapshot()` already does one level up. A comp list on
  //    the session record would be outside every history entry, and undo would
  //    silently not cover it.
  //  * io/NpaintFile writes it as `np:comps` on part 0, and part 0 is where
  //    docs/document-format.md puts document-level attributes. A list that
  //    lived on the open-document record would have to be threaded through
  //    `saveNpaint()` as a fifth argument for no gain.
  //
  // Empty for every document this build has ever produced until a user
  // captures one, and io/NpaintFile writes no attribute at all for an empty
  // list -- so a document with no comps produces exactly the bytes it produced
  // before this member existed. `--selftest` asserts that against a file rather
  // than assuming it.
  std::vector<LayerComp> comps;

  // **Alpha channels** (PLAN.md Phase 7; PRD E13), and with them PRD E11's
  // saved selections -- a saved selection *is* a named channel, and
  // core/Channels.hpp is where that identity is argued.
  //
  // On `Document` for the two reasons `comps` gives above, and against a third
  // consideration that pulls the other way and must not win:
  //
  //  * `core::History` entries hold a whole `Document` by value, so Save
  //    Selection, rename and delete are undoable for free. That is **correct
  //    here and would be wrong for the active selection**, which is the whole
  //    distinction: saving a selection is a deliberate command that changes the
  //    document, so undo must remove the channel; drawing a marquee is a
  //    gesture, so undo must not touch it. `app::OpenDocument::selection` holds
  //    the active one and states the other half of the argument.
  //  * io/NpaintFile writes each channel as its own `S####` EXR part, the shape
  //    docs/document-format.md sketched before anything wrote one. A list on
  //    the session record would have to be threaded through `saveNpaint()` as a
  //    further argument for no gain.
  //
  // The consideration that loses: a channel is *bulk pixel data*, so a history
  // entry now carries the channel tiles too. It costs a refcount per tile and
  // not a copy -- `SelectionTileStore` is copy-on-write like every other tile
  // store (core/TileStore.hpp) -- so an undo of a paint stroke on a document
  // with twelve saved selections copies twelve pointers, not twelve masks.
  //
  // Empty for every document until a user saves a selection, and io/NpaintFile
  // writes **no part at all** for an empty list -- so a document with no
  // channels produces exactly the bytes it produced before this member existed,
  // and a file written before it existed loads with the list empty.
  // `--selftest` asserts both against files rather than assuming them.
  std::vector<AlphaChannel> channels;

  // The next value `core::normalizeLayerIds()` will hand out for `Layer::id`.
  //
  // A counter rather than "one above the highest id present", and that
  // difference is the whole point: the highest id present *falls* when the
  // topmost-numbered layer is deleted, so max-plus-one would re-issue a dead
  // layer's id to the next layer created -- and a comp captured before the
  // delete would then restore that layer's state onto an unrelated new layer.
  // Exactly `HistoryEntry::serial`'s rule ("monotonic within one History, never
  // reused") for exactly the same reason.
  //
  // Persisted inside `np:comps` and only there, so it costs nothing in a
  // document that has no comps. `normalizeLayerIds()` also raises it past any
  // id it finds, so a document whose counter was lost (loaded from a file whose
  // comps were stripped by another tool, say) still cannot re-issue a live id.
  uint64_t nextLayerId = 1;

  // **The next value `core::makeGroupLayer()` will hand out for
  // `Layer::groupTag`** (PLAN.md Phase 5's C7/C12 follow-on). A separate
  // counter from `nextLayerId` above, deliberately -- see `Layer::groupTag`'s
  // own comment for why grouping must not force every layer in a document to
  // acquire a `Layer::id`. Monotonic, never reused within one `Document`, and
  // assigned **eagerly** (every Group layer gets one immediately, unlike `id`,
  // which stays 0 until a comp is captured) -- so a document with a group
  // always has a nonzero counter here, and a document with none carries the
  // default `1` for its whole life and costs io/NpaintFile nothing to write
  // (there is no `np:*` attribute for this counter itself; each Group layer's
  // own `groupTag` is what persists, as `np:groupId`, and this counter is
  // reconstructed on load by advancing past every `groupTag` found -- the
  // identical "raise the counter past every id seen" rule
  // `core::normalizeLayerIds()` already applies to `nextLayerId`).
  uint64_t nextGroupId = 1;

  // Blank-document factory (PLAN.md Phase 2 step 5; PRD C7 (P0): "A document
  // can be created blank, not only opened from a file"). Builds a Document
  // of the given size and working space with exactly one layer.
  //
  // That layer is RGB-kind, not Pigment-kind -- even though CONTEXT.md names
  // Pigment as "the default kind for a new layer," the eventual,
  // ecosystem-wide domain default once Pigment layers are real (see
  // Layer::kind's default in core/Layer.hpp, which deliberately keeps that
  // default for exactly this future reason). Per Layer.hpp's own contract, a
  // Pigment-kind Layer has `rgbTiles == std::nullopt` and, today, no other
  // storage either: Pigment/Media need a different, 7-channel latent+mass
  // tile shape (DESIGN-imaging.md §2) that doesn't exist yet. A blank
  // document whose one layer cannot hold a single pixel would be useless for
  // "open a document and paint on it," the workflow this whole phase exists
  // to prove. This is a provisional, "ship 1" compromise, not a permanent
  // domain decision -- pending Phase 5 making Pigment layers real, at which
  // point createBlank()'s default may need revisiting; that revisit is not
  // this step's job.
  //
  // The RGB layer's TileStore is populated (`rgbTiles.emplace()`, matching
  // Layer.hpp's "populated only when kind == RGB" contract) but starts with
  // zero tiles allocated, regardless of `width`/`height`: PRD C2 (P0),
  // "Memory tracks content, not canvas dimensions -- tiles allocate only
  // where content exists." A freshly created blank canvas has no content
  // yet, so nothing here pre-fills a grid of tiles across the canvas --
  // core/TileStore.hpp's allocate-on-write design is what makes an untouched
  // region free, and pre-allocating here would silently reintroduce the
  // "memory tracks canvas size" bug that design exists to avoid.
  static Document createBlank(int32_t width, int32_t height, WorkingSpace space) {
    Document doc;
    doc.width = width;
    doc.height = height;
    doc.workingSpace = space;

    Layer layer;
    layer.kind = LayerKind::RGB;
    layer.rgbTiles.emplace();
    doc.layers.push_back(std::move(layer));

    return doc;
  }
};

}  // namespace np
