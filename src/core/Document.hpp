#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "color/Space.hpp"
#include "core/Layer.hpp"
#include "core/LayerComp.hpp"

// core/Document (PLAN.md "Phase 2 -- See a file", step 4; CONTEXT.md
// Relationships: "A Document holds an ordered list of Layers and a Working
// space"). Canvas dimensions, a working space, an ordered layer list, and
// the one factory PLAN.md's step 5 (PRD C7) asks for: createBlank().
namespace np {

struct Document {
  int32_t width = 0;
  int32_t height = 0;
  WorkingSpace workingSpace;

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
