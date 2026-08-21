#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Layer.hpp"

// core/LayerComp (PLAN.md "Phase 5 -- Stack it", step 12: "**Layer comps** --
// named sets of visibility, position and properties, restorable in one click
// and **persisted in the document** as an `np:comps` blob on part 0 (PRD C14).
// Cheap here because the layer model is fresh in hand; expensive later").
//
// PRD C14 (P1): "**Layer comps**: named, saved sets of layer visibility,
// position and properties, restorable in one click, persisted in the document".
//
// This header owns the *type* and the two decisions that are really decisions.
// core/LayerCompOps.hpp owns capture, restore and the list operations;
// io/CompSerial owns the on-disk encoding. The split is core/Layer.hpp's and
// core/LayerOps.hpp's, for the same reason.
//
// ==========================================================================
// (1) A CORRECTION TO THIS STEP'S SPECIFICATION: **position cannot be
//     delivered, because a layer has none**
// ==========================================================================
//
// C14 asks for "visibility, position and properties". Two of the three are
// here. The third is not, and it is not deferred for want of time -- it has
// nothing to attach to:
//
//   **`core::Layer` has no offset, origin, transform or position field of any
//   kind.** A layer's pixels live in `TileStore`/`PigmentTileStore` keyed by
//   `TileCoord`, which is an absolute position in the document's own
//   coordinate frame (core/TileStore.hpp). There is no per-layer origin that
//   those coordinates are relative to, so there is no number to capture and
//   nothing a restore could write back.
//
// Inventing an `offsetX/offsetY` here to make the wording come true would be
// the worst available answer: it would add a member that nothing composites
// through, nothing moves, no tool sets and no file carries, and then claim C14
// was met because a comp round-trips two zeros. This codebase's convention is
// the opposite one -- core/History.hpp opens with a numbered correction to its
// own step's wording, core/LayerOps.hpp is blunt about the exact scope of
// `locked`, and io/NpaintFile.hpp lists what it refuses to write. So:
//
//   **Comps capture visibility and properties. Position is not applicable,
//   because the layer model has no position.** `--selftest` prints that line
//   on every run rather than leaving it in a header.
//
// What would have to exist first, stated so the gap is closable rather than
// merely admitted: a per-layer transform on `Layer` that `core/Composite`
// samples through (today every walk indexes tiles directly, so a nonzero
// offset would be ignored by the compositor as well as by the file), an
// `np:offset` or `np:transform` attribute in docs/document-format.md, and a
// move tool to set it. That is a phase 6 "Filter and transform it" feature,
// not a field.
//
// ==========================================================================
// (2) WHICH PROPERTIES ARE IN A COMP -- and why the boundary is where it is
// ==========================================================================
//
// **A property that is in a comp is a property that clicking a comp silently
// overwrites.** That is the whole test, and it is why this list is short and
// why each exclusion has a reason rather than a shrug.
//
// IN -- four, all four of them appearance:
//
//   visible   PRD C14 names it first, and it is the property comps exist for:
//             "which of these variants is showing" is a set of eye icons.
//   opacity   the canonical appearance number. A comp that restored which
//             layers show but not how much would not restore the picture.
//   blend     ditto, and it is per-layer appearance in exactly the same sense.
//             Carried and restored **as the stored string**, so a value this
//             build has never heard of (`linear-burn`, PRD I10's value-level
//             carry, core/Layer.hpp) survives a capture and a restore
//             untouched -- but see restoreLayerComp(), which cannot *set* a
//             mode `core::blendModeFromName()` does not know and says so with
//             the name rather than substituting Normal.
//   clipped   PRD C9. Whether a layer shows through the alpha of the one below
//             is precisely the kind of switch a comp is for -- "the texture
//             pass clipped to the figure" versus "loose over everything" is
//             one flag and two pictures.
//
// OUT, each for a stated reason:
//
//   **the mask** (`Layer::mask` engaged, or its samples). Not restorable in
//   either direction. Restoring "had no mask" onto a layer that has one means
//   calling `removeLayerMask()`, which **discards** the mask (core/LayerOps.hpp
//   is explicit that it never applies it) -- so clicking a comp would destroy
//   authored coverage with no way back except undo. Restoring "had a mask"
//   onto a layer that lost it cannot invent the samples. A property a restore
//   can only half-perform is worse than one it does not claim.
//
//   **`locked`.** A lock is a *working* state, not an appearance: it exists to
//   stop edits reaching a layer. A comp that unlocked a layer the user locked
//   to protect it would defeat the guard by a click on an unrelated control,
//   and a comp that locked one would silently start refusing edits. Note the
//   consequence, which is deliberate and is reported rather than worked
//   around: **a restore honours the lock**, so a locked layer keeps its
//   opacity, blend and clip and `LayerCompRestoreReport` names it.
//
//   **`name`.** Names are how a user finds a layer in the panel. A comp that
//   renamed rows on restore would make the layers panel change words when the
//   picture changes, which is disorienting and is not what any editor's comps
//   do. The name at capture *is* stored on the entry below -- but only so the
//   report can say "the layer that was called 'Sky' is gone" instead of "id 7",
//   and nothing ever writes it back.
//
//   **`ops`, the per-layer op stack.** The closest call, because it is
//   genuinely appearance and genuinely per-layer. Excluded on size and on
//   granularity: an op stack is *unbounded content* (on an Adjustment layer it
//   is the layer's entire content, core/Layer.hpp), so a comp would become a
//   full copy of authored work rather than a set of switches, the `np:comps`
//   payload would grow without limit, and a restore would silently overwrite a
//   grade a user spent time on -- with the whole restore as the only undo
//   granularity. `core::History` already holds whole-document states for that
//   job, and holds them better. The unblocking condition is the one Photoshop
//   uses: comps capture layer *styles*, which are a bounded, named parameter
//   set; when this codebase has one, it belongs here.
//
//   **stack order.** A comp captures per-layer state, not the order of the
//   stack. Restoring an order would mean reordering layers created since the
//   capture into positions the comp has no opinion about, and `moveLayer()`
//   has its own refusals (a clipped layer cannot land at index 0). Out of
//   scope for the same reason position is: the operation exists, the *meaning*
//   of applying it to a changed stack does not.
//
// ==========================================================================
// (3) Not built here: export
// ==========================================================================
//
// PLAN.md step 13 ("Export comps to files, and layers to files"; PRD I16/I17)
// is a separate step and is not started. What this type owes it is that
// nothing here makes it hard: step 13 is "set a document state, composite,
// write through Export As presets", and `restoreLayerComp()` is the first of
// those three verbs with a `Document&` in and a `LayerOpResult` out. An
// exporter loops comps, restores each into a scratch copy of the document, and
// hands it to io/Export -- no part of that needs a second entry point, and no
// part of this type is shaped around a panel.
namespace np {

// One layer's captured state inside a comp.
//
// Deliberately flat and by value rather than a `Layer` with most of it
// ignored: what a comp captures is a decision (section 2), and a struct that
// held a whole `Layer` would make that decision invisible and would carry
// tiles.
struct LayerCompEntry {
  // `Layer::id`. Never 0 in an entry this build captured -- `captureLayerComp()`
  // assigns ids before it reads anything.
  uint64_t layerId = 0;

  // The four captured properties. Section 2 argues the boundary.
  bool visible = true;
  bool clipped = false;
  float opacity = 1.0f;
  std::string blend = kDefaultBlendName;

  // The layer's name **at the moment of capture**, stored for the refusal and
  // for `exrheader`, and never written back onto a layer. See section 2's
  // `name` exclusion: this is what lets a report say "'Sky' is no longer in
  // this document" rather than naming an integer a user has never seen.
  std::string nameAtCapture;

  friend bool operator==(const LayerCompEntry&, const LayerCompEntry&) = default;
};

// One named comp.
struct LayerComp {
  std::string name;

  // In the document's own bottom-to-top order at the moment of capture, which
  // is the order `Document::layers` is in (core/Document.hpp). The order is not
  // meaningful to a restore -- entries are matched by `layerId` -- but keeping
  // it means the payload and any `exrheader` dump read in stack order.
  std::vector<LayerCompEntry> layers;

  // **False for a comp record this build could not decode**, in which case
  // `unrecognised` holds the record's bytes verbatim and `name`/`layers` are
  // empty. io/OpSerial's `OpClass::Unknown` rule, one level up: a comp written
  // by a newer build survives a load/save through this one **in its original
  // position in the list** rather than being dropped (PRD I10), and
  // `restoreLayerComp()` refuses it by name rather than applying an empty comp
  // that would look like a comp that does nothing.
  bool known = true;
  std::vector<uint8_t> unrecognised;

  friend bool operator==(const LayerComp&, const LayerComp&) = default;
};

}  // namespace np
