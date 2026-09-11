#pragma once

#include <cstdint>
#include <string>
#include <vector>

// core/Region -- document-level named rectangles (docs/ui.md §4a's Frame and
// Slice tools; PLAN.md gap-closing wave, track `region`). `PRD.md:105` lists
// "Slice / web export" as a non-goal; this is built anyway, lean, because the
// user asked for it after that line was written -- the gather is where
// PRD.md itself gets corrected, not this file.
//
// ==========================================================================
// (1) WHAT A REGION IS, AND WHY IT IS NOT `ops::DocumentRegion`
// ==========================================================================
//
// `ops/DocumentTransform.hpp` already declares a `DocumentRegion` -- a bare
// rectangle with no identity, used as an argument type for crops and resizes.
// This is a different thing: a **named, kinded, addressable** rectangle that
// persists as part of the document and that a user creates, selects, moves
// and deletes by name. Reusing the name would make every include site read
// as the wrong type's argument; `Region` is picked instead, matching how
// `core::LayerComp` did not become a second `Layer`.
//
// ==========================================================================
// (2) TWO KINDS SHARE ONE LIST, BECAUSE THEY SHARE EVERY OPERATION
// ==========================================================================
//
// `Tool::Frame` (Move's palette group, "Frame / Artboard" per docs/ui.md:254)
// and `Tool::Slice` (Crop's group) both draw a rectangle, both select and drag
// an existing one, both resize by corner, both delete with Backspace, and
// both export through the same per-region loop (`io/ExportRegions.hpp`). The
// only place the two kinds' behaviour differs at all is the overlay's colour
// and the export dialog's "Frames only / Slices only" filter -- both cosmetic
// -- so one list with a `kind` tag is the whole model, and a `Frame`-only
// list plus a `Slice`-only list would be two copies of every one of those
// operations kept in step by hand.
//
// ==========================================================================
// (3) WHERE THIS LIVES, AND WHY (the inside/outside test)
// ==========================================================================
//
// `ops/Pattern.hpp` §1 states the test this codebase already applies to
// "does a thing belong in `Document`, or is it session state beside it":
// **`core::HistoryEntry` holds a whole `Document` by value, so anything
// inside `Document` is restored by Undo, and anything outside it is not.**
// A defined pattern fails that test (it must survive the very undo that
// created the layer it will fill) and lives beside the session instead. A
// region passes it the other way: creating, deleting, moving and resizing one
// are exactly the kind of "the user made a thing" edits that Undo is for --
// there is no case where "undo this paint stroke" should also silently
// un-create a Frame the user drew five minutes later, which is what a
// session-side list would risk the moment the two lists' undo histories
// disagreed. So `regions` sits on `Document`, beside `comps` and `channels`,
// for the identical reason both of them do.
//
// **Persisted in `.npaint`** as a new `np:regions` attribute on part 0, the
// same hex-string-carrier convention `np:comps` and `np:ops` use
// (io/NpaintFile.hpp's "no working blob carrier" section) -- io/RegionSerial
// owns the encoding. Written only when the document has regions, so a
// document with none produces exactly the bytes it produced before this
// feature existed, matching every other optional attribute in that file.
//
// ==========================================================================
// (4) GEOMETRY EDITS: WHAT crop/canvas-size/image-size/transform DO TO A
//     REGION, AND WHY
// ==========================================================================
//
// `ops/DocumentTransform.hpp` already states the rule for every other kind of
// document content -- pixels, masks, the selection -- and a region is a
// fourth kind of content that has to follow the same document-geometry
// changes or it silently points at the wrong pixels after the very next
// crop. Three cases, decided here because `ops/DocumentTransform.cpp` is
// where the hook lives (see that file's crop/canvas-size and transform-
// document entry points):
//
//   **Crop and canvas size** (both route through `cropDocument()`, one
//   directly and one via the anchor-implied offset): a region translates by
//   the same `(-x, -y)` every layer's tile store does, and is then
//   **intersected with the new canvas**. A region that ends up with an empty
//   intersection is **removed**, not kept as a zero-size husk -- an empty
//   region is not a rectangle a user could see, drag or export, and keeping
//   it would mean every consumer (the overlay, the export dialog, the
//   options row) had to filter it out again. This differs from
//   `cropDocument()`'s own choice for pixel content, which is kept
//   off-canvas so an undo gives back what a crop hid -- and that is not
//   available here: a region has no tile store to preserve off-canvas state
//   in, only two numbers per axis, so "clip to the new canvas" is where the
//   rectangle's own definition already runs out. Undo still gives back the
//   *original* region exactly, because `core::History` snapshots the whole
//   `Document` (this member included) rather than this file computing an
//   inverse.
//
//   **Image size**: a region scales by the same `width_new / width_old` and
//   `height_new / height_old` factors as `resizeDocumentImage()` applies to
//   every pixel store, rounded to the nearest texel. No intersection step --
//   an image resize cannot move a region off the canvas, only stretch it
//   with the canvas.
//
//   **Rotate / flip the canvas** (`transformDocument()`): a region's four
//   corners are mapped by the same matrix as every layer and the selection,
//   and the result is the smallest integer rectangle containing them --
//   `regionTransformedRect()` below, which does not reuse
//   `ops::transformedRegion()`'s one-pixel rounding margin because that
//   margin exists for *resampled pixel content*, where a destination texel
//   can only be written when its centre maps back inside the source; a named
//   rectangle has no such sampling condition; it needs the tightest box that
//   still contains every mapped corner exactly. Intersected with the new
//   canvas afterwards, and removed if the intersection is empty, for the
//   crop case's own reason.
namespace np {

// Which of the two tools created a region. Also the export dialog's filter
// and the overlay's colour selector -- see this header's §2.
enum class RegionKind {
  Frame,
  Slice,
};

// "Frame" / "Slice" -- the name stem a default name is built from
// (`defaultNewRegionName()`), and the word used in refusal sentences.
const char* regionKindName(RegionKind kind) noexcept;

// One named, kinded rectangle in document pixel space, half-open like
// `ops::DocumentRegion`: `[x, x + width) x [y, y + height)`.
struct Region {
  // A stable identity, assigned once from `Document::nextRegionId` and never
  // reused -- `core::Layer::id`'s own reason: a reference captured before a
  // delete must not silently start naming an unrelated later region.
  uint64_t id = 0;

  RegionKind kind = RegionKind::Frame;

  // User-facing, and **unique across the whole list, regardless of kind** --
  // unlike a layer name and like `core::AlphaChannel::name`. Uniqueness is
  // what lets the export dialog's name template and the overlay's label be
  // read without a disambiguating index, and what lets a future "go to
  // region by name" not depend on enumeration order.
  // `uniqueRegionName()` (core/RegionOps.hpp) is how a caller gets a name
  // that keeps the property; every mutator in that file routes through it.
  std::string name;

  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool empty() const noexcept { return width == 0u || height == 0u; }

  friend bool operator==(const Region&, const Region&) = default;
};

}  // namespace np
