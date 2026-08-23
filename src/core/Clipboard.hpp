#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"
#include "core/SelectionMask.hpp"

// core/Clipboard (PLAN.md "Phase 7 -- Select and paste"; PRD M1, M3, M4, M5,
// M8).
//
// --- M5 is the requirement that shapes this file -------------------------
//
// "The internal clipboard holds a copy-on-write tile reference, **not** a
// flattened buffer, and its cost appears in the status-bar figure." The PRD
// adds, unusually, a note explaining that this is "a Lightweight requirement,
// not a convenience": a 4K full-document copy is 68 MB at rgba16float, and
// "Photoshop ships a 'Purge -> Clipboard' command because it holds exactly
// this invisibly. A5 forbids it here."
//
// So the copy is not a memcpy of the selected region. Every tile the selection
// covers **completely** is shared with the source through
// `TileStoreOf::shareTileFrom()` -- one refcount increment, no bytes -- and
// only the tiles the selection's EDGE crosses are materialised, because those
// are the ones whose texels have to be weighted by coverage. A rectangular
// marquee over a large area therefore costs the perimeter, not the area.
//
// `sharedTileCount()` and `exclusiveBytes()` report the split, which is what
// PRD M5's "its cost appears in the status-bar figure" needs to be answerable.
//
// --- Two weighting rules, not one ----------------------------------------
//
// An RGB tile is premultiplied rgba16float, so weighting a texel by coverage
// scales all four channels together (core/SelectionMask.hpp's clear says why
// that is the correct, fringe-free hole).
//
// A Pigment texel is a **straight** latent plus a `mass` that is the alpha
// analogue, so weighting scales **mass alone and leaves the latent alone** --
// PRD F10's rule for the eraser, in its own words. Scaling the latent too
// would make a half-copied red stop being red rather than being less of it.
//
// This is why the two kinds are handled by separate code here rather than a
// template: the shared shape is the tile walk, and the part that differs is
// the only part that matters.
//
// --- What this file is NOT ------------------------------------------------
//
// **No OS pasteboard.** PRD M8 requires that an internal copy-paste "takes the
// internal path and never round-trips the pasteboard, so pigment latents
// survive", and the strongest way to satisfy that is for the internal path not
// to know the pasteboard exists. Nothing here serialises, encodes, or converts
// to a display space; a Pigment layer copied and pasted arrives with the same
// latents it left with, bit for bit, because the tiles are the same tiles.
// PRD M6 and M7 (decoding a pasted foreign image, writing display-encoded sRGB
// out) belong to the pasteboard bridge, which is a different file and reuses
// io/ImageDecode and io/Export rather than anything here.
namespace np {

// One clipboard payload: enough to reconstruct a layer, and nothing about
// where it came from beyond its name.
//
// Tiles keep their original `TileCoord` keys, in document space. That is what
// makes PRD M3's "paste in place preserves document coordinates" exact rather
// than approximate -- there is no offset to apply and therefore no resampling,
// so a copy-paste in place is bit-identical to the source.
struct Clipboard {
  LayerKind kind = LayerKind::RGB;
  std::optional<TileStore> rgbTiles;
  std::optional<PigmentTileStore> pigmentTiles;

  // The name of the layer this came from, so a pasted layer can be called
  // something a user recognises rather than "Layer 4".
  std::string sourceName;

  bool empty() const noexcept;

  // How many of the payload's tiles are still shared with the document they
  // were copied from -- the direct evidence for PRD M5. A full-document copy
  // should report every tile shared and therefore near-zero marginal cost.
  size_t sharedTileCount() const noexcept;

  // Bytes this payload holds that are NOT shared with anything else: the
  // honest answer to "what does the clipboard cost right now", and the figure
  // PRD M5 wants on the status bar. Shared tiles are excluded because they are
  // resident whether or not the clipboard exists.
  size_t exclusiveBytes() const noexcept;
};

// PRD M1's copy. Takes the part of `layer` that `selection` covers, weighting
// partially covered texels.
//
// A null selection copies the whole layer -- Select All is the default, the
// same reading `clearThroughSelection()` takes -- and that is the case PRD M5
// is really about, because it is the one that would otherwise be 68 MB.
//
// An empty (engaged but zero-coverage) selection copies nothing, and the
// result is `empty()`. A caller must be able to tell that from a successful
// copy, which is why `empty()` exists rather than a bool return.
Clipboard copyThroughSelection(const Layer& layer, const Selection* selection);

// PRD M1's cut: the copy above, then the coverage-weighted clear. The clear
// runs only if the copy produced something, so a cut through an empty
// selection leaves the layer untouched rather than being a clear with extra
// steps.
//
// Refuses (returning an empty Clipboard and touching nothing) on a locked
// layer -- the same routing rule app/StrokeSession and app/StrokeBake follow,
// because a cut is a destructive edit and a locked layer must not take one.
Clipboard cutThroughSelection(Layer& layer, const Selection* selection);

// PRD M3: paste creates a LAYER, in place. Inserts a new layer at `atIndex`
// holding the clipboard's tiles, and returns its index, or `std::nullopt` if
// the clipboard is empty or the index is out of range.
//
// The tiles are shared with the clipboard, not copied, so pasting the same
// payload five times costs five refcounts until one of them is painted on.
std::optional<size_t> pasteAsLayer(Document& doc, const Clipboard& clip, size_t atIndex);

// PRD M2's copy merged: composites the visible stack and takes what the
// selection covers.
//
// **This is the one copy that cannot be a reference, and that is inherent
// rather than a shortcut.** Everywhere else in this file a fully covered tile
// is shared with the source (PRD M5), because the pixels already exist
// somewhere. A merged copy's pixels did not exist until it composited them, so
// there is nothing to share and every tile it produces costs bytes. A caller
// showing PRD M5's status-bar figure will see copy-merged cost real memory
// where a plain copy costs none, and that is the truth rather than a
// regression.
//
// **The result is always an RGB layer, even when the stack was all Pigment.**
// Compositing projects latents through `latentToRgb()`, so a merged copy of a
// pigment stack is a picture of that paint, not the paint. PRD M8's "pigment
// latents survive" applies to `copyThroughSelection()`, which takes a layer's
// own tiles; it cannot apply here, because a composite of two pigment layers
// has no single latent to preserve. Copy the layer rather than the merge when
// the latents are what matters.
//
// `warningsOut`, when non-null, receives core/Composite's blend warnings --
// appended, never cleared, the same contract io/Export's flatten uses.
Clipboard copyMergedThroughSelection(const Document& doc, const Selection* selection,
                                     std::vector<std::string>* warningsOut = nullptr);

// PRD M4's "selection -> new layer": copy the selected part of `srcIndex` and
// paste it directly above as its own layer, in one act. Returns the new
// layer's index.
//
// Deliberately built from the two functions above rather than beside them, so
// there is one implementation of "what does a selection take from a layer" and
// this cannot drift from what copy does.
std::optional<size_t> selectionToNewLayer(Document& doc, size_t srcIndex,
                                          const Selection* selection);

}  // namespace np
