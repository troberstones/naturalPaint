#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"

// app/LayerThumbnail -- **the two small pictures in a layer row**: what the
// layer holds, and what its mask holds.
//
// ==========================================================================
// 0. Why this is a module and not twenty lines inside the draw function
// ==========================================================================
//
// `--selftest` cannot reach an ImGui dispatch site (reachability-audit F4),
// exactly as `ui/DabPicker.hpp` §1 says of the tip grid. A thumbnail written
// inside `drawLayersSection()` is a thumbnail with no assertions on it at
// all, and the two things most worth asserting about one are things a
// screenshot shows only if a human happens to look: **which byte a value
// becomes**, and **whether the picture is of the layer as it is now**. Both
// are pure functions of numbers here, and `app/selftest/LayerMask.cpp`
// drives them directly. The draw site calls these, uploads what they return,
// and decides nothing.
//
// ==========================================================================
// 1. The transfer function, which is DIFFERENT for the two thumbnails
// ==========================================================================
//
// **This is the whole reason the two builders are two functions rather than
// one with a channel count.** `ui/CanvasQuad.hpp` records what this
// application learned the expensive way: the swapchain is deliberately
// **non-sRGB**, so Dear ImGui's pipeline applies gamma 1.0 and every byte
// handed to `AddImage()` reaches the screen unchanged. Chrome bytes are sRGB
// and are therefore exactly right; a **linear-light** texture drawn that way
// is not -- linear 0.25 reaches the screen as byte 61 where it should be 137,
// with zero error at both endpoints, which is why no black-and-white test
// image ever caught it (`app/selftest/PresentTransfer.cpp` measured it).
//
// So:
//
//   * **A layer's tiles hold LINEAR light.** `layerContentThumbnail()`
//     therefore does the sRGB encode **on the CPU**, with `color/Space`'s own
//     `srgbEncode()`, and what it returns is sRGB bytes -- the same kind of
//     value every `atelierToken()` colour is. Those may go through
//     `AddImage()`, because by the time they are texture bytes they are not
//     linear any more. Uploading the linear values and letting `AddImage()`
//     have them would be the present-transfer defect, one thumbnail at a
//     time.
//   * **A mask holds COVERAGE.** `ui/DabPicker.hpp` §2 states the rule for
//     exactly this case: "coverage is an opacity, which is never
//     gamma-encoded -- so this is one of the cases that may go through
//     `AddImage()`". `layerMaskThumbnail()` therefore does **no** encode:
//     coverage 0.5 becomes byte 128, not the 188 an encode would give.
//
// The two constants in that last sentence are what `--selftest` asserts, in
// both directions, so an "obvious simplification" that gave the two builders
// one shared body would fail rather than merely look slightly wrong.
//
// **Alpha is not encoded either, in the layer thumbnail.** It is a coverage
// for the same reason a mask sample is, and the same rule applies to it. Only
// the three colour channels go through `srgbEncode()`.
//
// ==========================================================================
// 2. What each thumbnail is OF, and what it deliberately leaves out
// ==========================================================================
//
// The layer thumbnail shows **the layer's own content, alone**: not the
// composite below it, not its own opacity, not its blend mode, not its op
// stack, and -- the one worth stating plainly -- **not its mask**. The mask
// has its own thumbnail two pixels away, and a layer picture with the mask
// already multiplied in would make the pair say one thing twice while the
// question a user actually has ("what is on this layer, and what is the mask
// letting through") lost half its answer. This is also what Photoshop's pair
// shows.
//
// A layer with no pixels to show -- an Adjustment layer, a Group -- is
// `layerHoldsPixels() == false` and gets an entirely transparent thumbnail.
// The draw site paints its own "nothing here" mark rather than this module
// inventing one, because a placeholder is a design decision and this file
// has no opinions about the panel.
//
// **Premultiplied in, straight out.** `core::Tile` stores premultiplied
// (DESIGN-imaging.md §2) and so does `projectPigmentTexel()`; averaging
// premultiplied values is the correct filter (it is a linear operation on
// (colour*a, a)), and the un-premultiply happens once, on the average, at the
// end -- which is where `ui/DocumentTexture` does it too. Averaging straight
// colours weighted by nothing would let a nearly-transparent bright texel
// drag the average, which is the classic dark-fringe artefact seen from the
// other side.
//
// ==========================================================================
// 3. The cost, stated rather than hoped for
// ==========================================================================
//
// **Sampling is O(thumbnail), not O(document).** A thumbnail is not built by
// walking the layer's tiles -- a 2048x2048 layer is 256 tiles of 16 384
// texels, and doing that for twenty rows on every frame is not a cost this
// panel can carry. Instead each of the `kLayerThumbPx * kLayerThumbPx` output
// texels box-averages a fixed `kThumbSupersample^2` samples spread across the
// source footprint it stands for. So one thumbnail costs
// `24*24*4*4 = 9 216` samples whatever the document's size is, and each sample
// is one tile lookup plus one half decode.
//
// The consequence to be honest about: at 4x4 samples per output texel a large
// document is **point-sampled sparsely**, so fine detail aliases. That is the
// accepted trade -- `ui/DabPicker.hpp`'s thumbnails make the same call for the
// same reason, and a 24 px square is an index card, not a proof.
//
// **Twenty layers.** The cache below rebuilds only what the caller asks for,
// and the caller asks only for rows it is about to draw -- so the cost is
// bounded by the number of rows that fit in the panel (about ten), not by the
// stack's depth. On the frame after an edit that is ~10 rows x 2 thumbnails x
// 9 216 samples = ~184 000 samples; `--selftest` measures and prints the real
// number rather than leaving that as arithmetic.
//
// ==========================================================================
// 4. Invalidation, which is the part that must not be got wrong
// ==========================================================================
//
// **A thumbnail cache that does not invalidate shows a picture of the layer as
// it used to be, which is worse than showing nothing** -- a blank square says
// "I do not know", a stale one says something false with confidence.
//
// The key is `(DocumentId, OpenDocument::revision, layerIndex)`, and it is
// held **inside the object**, which is `core::SelectionBoundaryCache`'s shape
// and for its stated reason: the caller hands over the key and the cache
// decides whether to rebuild, so there is no companion pair of
// cached-key members beside it for a caller to forget to update.
//
// **Why the document id is in the key and not just the revision.**
// `AppState::cachedSelectionRevision`'s comment already argues this and it is
// argued the same way here: revisions start at 0 per document, so two open
// tabs sit at the same revision most of the time, and keying on the revision
// alone would show one tab's layer in the other tab's panel whenever the two
// numbers agreed -- which is the common case, not the rare one.
//
// **Why the whole cache is dropped when the key moves, rather than per row.**
// `revision` is document-wide, so it cannot say *which* layer changed; and a
// reorder, a delete or a merge changes which layer a given index names without
// changing anything about the layers themselves. Dropping everything is the
// only rule that is right in all three cases. It costs a rebuild of the
// visible rows on each edit, which §3 measures.
//
// **The key follows a LIVE stroke, and that was worth measuring rather than
// reasoning about.** `ui/MacPaintUI.cpp` says "`revision` is bumped by
// `recordEdit()` and nothing else", which reads as "a stroke moves it once, at
// pen-up" -- and that is not what happens. `StrokeSession::addPoint()` bumps
// `revision` directly on every frame that landed tiles, because "the revision
// is what invalidates ui/DocumentTexture's cache"; `end()` then bumps it once
// more along with the single history entry. `app/selftest/MaskTarget.cpp` §8
// asserts both halves of that (many bumps, one entry) precisely because the
// cost below depends on which it is.
//
// So a thumbnail tracks the pen instead of lagging a whole stroke behind it,
// and the price is that §3's number is a **per-frame** cost during a drag
// rather than a per-edit one. Measured on this build, it is ~0.8 ms for ten
// visible rows' worth of both thumbnails, inside PRD F3's 20 ms frame; if that
// ever stops being true the fix is a smaller cell or fewer samples, both of
// which are constants in this header, and not a looser key.
//
// **What the key does NOT catch.** Anything that writes tiles without moving
// the revision -- of which this repository has exactly one, `main.cpp`'s
// `buildDemoDocument()`, which calls `recordEdit()` by hand at the end for
// precisely this reason. Such a caller must move the revision or call
// `invalidate()`.
namespace np {

// The square cell, in texels. Small on purpose: the layers panel is about
// 322 px wide and a row is about 40 px tall (`ui/MacPaintUI.cpp`'s own
// `kLayerMaskChipW` comment records that the row is often clipped at that
// width), so the two thumbnails together may spend about 56 px of it. 24 is
// what fits a 40 px row with the panel's 4 px vertical padding and still
// divides evenly into an atlas page.
inline constexpr int kLayerThumbPx = 24;

// Samples per output texel, per axis. §3 derives the cost; 4 is the largest
// value that keeps one thumbnail under ten thousand samples.
inline constexpr int kThumbSupersample = 4;

// One thumbnail: `kLayerThumbPx` square, RGBA8, **straight** alpha.
//
// The document's aspect ratio is preserved and the picture is centred, with
// the letterbox margin left fully transparent -- the same choice
// `dabThumbnailRgba()` makes for a non-square tip, and for the same reason: a
// tall document stretched into a square would be a picture of a document that
// does not exist.
struct LayerThumbnail {
  std::vector<uint8_t> rgba;  // kLayerThumbPx * kLayerThumbPx * 4
  // The letterboxed rect the document actually occupies, in cell texels.
  int x = 0, y = 0, w = 0, h = 0;
  // How many source samples were taken. §3's cost claim, measured rather than
  // asserted; `--selftest` prints it.
  size_t samples = 0;
};

// The layer's own content, sRGB-encoded (§1), straight alpha.
//
// `layerIndex` out of range, a document with no area, or a layer that holds no
// pixels at all (an Adjustment layer, a Group) all give a fully transparent
// thumbnail with `w == h == 0` -- three different reasons, one answer, because
// the panel's answer to all three is the same mark.
LayerThumbnail layerContentThumbnail(const Document& doc, size_t layerIndex);

// The layer's mask, as coverage bytes with **no encode** (§1): 0 is black and
// hidden, 255 is white and revealed, alpha 255 throughout the letterboxed
// rect.
//
// A layer with no mask gives a fully transparent thumbnail with `w == h == 0`.
// That is not the same picture as a mask that reveals everything, which is a
// solid white square -- `core/Mask.hpp` separates absent, all-1.0 and all-0.0
// at length and this module keeps all three distinct, because the panel has to
// show the difference between "no mask" and "a mask that is doing nothing".
LayerThumbnail layerMaskThumbnail(const Document& doc, size_t layerIndex);

// Both thumbnails for one row, cached on `(DocumentId, revision, layerIndex)`
// (§4).
class LayerThumbnailCache {
 public:
  struct Row {
    LayerThumbnail content;
    LayerThumbnail mask;
  };

  // The row's pair at `(documentId, revision)`. Builds it if the key moved or
  // this row has not been asked for at this key; returns a reference that is
  // valid until the next call with a **different** key.
  const Row& rowFor(const Document& doc, size_t layerIndex, uint64_t documentId,
                    uint64_t revision);

  // How many rows this cache has actually built. **The hook the invalidation
  // assertion hangs on**, borrowed verbatim from
  // `core::SelectionBoundaryCache::extractionCount()` along with its reason: a
  // cache that never refreshes returns a plausible-looking picture forever and
  // passes every test that draws once, so `--selftest` checks both that this
  // number moves when the revision moves and that the picture it hands back
  // afterwards is the new one.
  size_t buildCount() const noexcept { return builds_; }

  // How many rows are resident. The memory claim, checkable: each row is two
  // 24x24 RGBA8 buffers, 4 608 bytes, so a twenty-layer document's whole cache
  // is under 100 KiB.
  size_t residentRows() const noexcept { return rows_.size(); }

  // Forces the next call to rebuild, whatever the key says. For a caller that
  // has mutated tiles behind the revision's back -- §4 names the one such path
  // in this repository -- and provided rather than pretending the situation
  // cannot arise, which is `SelectionBoundaryCache::invalidate()`'s reason
  // too.
  void invalidate() noexcept {
    rows_.clear();
    primed_ = false;
  }

 private:
  std::unordered_map<size_t, Row> rows_;
  uint64_t documentId_ = 0;
  uint64_t revision_ = 0;
  bool primed_ = false;
  size_t builds_ = 0;
};

}  // namespace np
