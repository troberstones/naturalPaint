#pragma once

#include <array>
#include <cstdint>
#include <vector>

// core/StrokesContent -- what a `LayerKind::Strokes` layer holds (PLAN.md
// phase 8; PRD C1, C11, D6, F11).
//
// ==========================================================================
// 1. A document dab is not a solver dab, and that is the whole point
// ==========================================================================
//
// `brush/StrokePath` already emits dabs, and it emits them INTO a deposition:
// a `BrushTip` plus a position handed to `depositDab()`, spent the instant it
// is computed and recoverable afterwards only as the texels it changed.
// io/NpaintFile.hpp said so in the sentence that reserved this file's format
// carrier -- "brush/StrokePath emits dabs into the *solver*, not into a
// document".
//
// A `DabRecord` is the other thing: a dab the DOCUMENT keeps. It is stored
// rather than spent, so it can be re-evaluated (PRD C11), re-serialised
// (`np:dabs`), individually DELETED (PRD F11) and -- the property phase 8 is
// really named for -- re-read against whatever lies beneath the layer at the
// moment of evaluation (PRD D6). None of those four is expressible about a
// texel that has already been written.
//
// **It is deliberately NOT `BrushTip`.** A `BrushTip` carries a bitmap
// pointer, a dual tip, a pigment load, a spacing and a scatter -- stroke
// authoring state, none of which a stored dab needs, two of which are
// non-owning pointers that could not survive a save, and all of which would
// have to be versioned in the file format for ever. What a stored dab needs
// is its footprint, its colour policy and its identity, which is what is
// below. The SHAPE fields are named to match `BrushTip`'s exactly
// (`radius`/`hardness`/`roundness`/`angle`), because `strokesRasterize()`
// builds a `BrushTip` out of them and calls `dabCoverage()` -- there is one
// dab-shape function in this build, exactly as core/TextContent.hpp section 1
// insists there is one rasteriser.
//
// ==========================================================================
// 2. Samples-only-from-below
// ==========================================================================
//
// `DabColorSource::Below` is PLAN.md phase 8's "samples-only-from-below" rule and
// PRD D6's requirement ("clone and heal, which stay correct when layers
// beneath them are regraded") stated as one field. Such a dab carries no
// colour of its own: it reproduces the composite of the layers BENEATH its
// own layer, sampled at `(x + sourceDx, y + sourceDy)`. A grade added to a
// layer underneath therefore changes what the dab reproduces on the next
// evaluation, without the dab record itself changing at all.
//
// `DabColorSource::BelowHealed` is the other half of that same sentence --
// "clone AND HEAL" -- and it samples from below TWICE: once at the offset for
// the texture, and once straight down for the illumination the texture is
// corrected to. Both reads move under a regrade, so a recorded heal tracks
// one for the same reason a recorded clone tracks one.
//
// **Below, not "the whole document".** Sampling the full composite would
// include the Strokes layer's own output, so a dab would read its own result
// and every evaluation would be a feedback loop with no fixed point. Below is
// the only reading under which evaluation is a pure function of the document.
//
// This is why the kind cannot store pixels and be done with it: pixels are
// exactly the thing that does not track a regrade.
//
// ==========================================================================
// 3. Coordinates and colour follow core/'s conventions, not the brush's
// ==========================================================================
//
// `x`/`y` are DOCUMENT texel coordinates of the dab centre, in floats,
// because a dab lands between texels and rounding it at record time would
// quantise a stroke to the pixel grid. `rgba` is LINEAR and STRAIGHT, the
// same convention `VectorShape`'s `Paint` uses and for that header's stated
// reason; `strokesRasterize()` premultiplies on the way into a `Tile`, which
// is where every other producer in this build premultiplies too.

namespace np {

// Where one dab's colour comes from. Section 2.
//
// **`DabColorSource`, not `DabSource`.** app/DabLibrary already owns a
// `DabSource`, and it answers a completely different question -- where a
// stored TIP BITMAP came from (an `.abr`, a GIMP brush, an imported image).
// Two enums a header apart, both spelled `DabSource`, would be a name
// collision waiting on the first translation unit that included both, and
// the reader who hit it would have no way to tell which one a bare `Ink` or
// `Abr` belonged to. Named for what it selects instead.
enum class DabColorSource : uint8_t {
  // The record's own `rgba`. A recorded paint stroke.
  Ink,
  // The composite BENEATH this layer, at `(x + sourceDx, y + sourceDy)`. A
  // recorded CLONE -- the thing PRD D6 asks to stay correct under a regrade.
  Below,
  // The same sample, CORRECTED so that it carries the illumination of the
  // composite beneath this layer at the dab's OWN place: `ops/Poisson`'s
  // `healPatch()` over the dab's bounding box, with the source patch read at
  // the offset and the Dirichlet rim read straight down. A recorded HEAL.
  //
  // **A third enumerator rather than a `bool healed` beside `source`.** The
  // two are not independent bits: `rgba` is read for `Ink` and neither
  // offset field is, `sourceDx`/`sourceDy` are read for both of the others,
  // and a `healed` flag set on an `Ink` dab would name a solve with no source
  // to solve from. One field whose value selects which of the other fields
  // mean anything is the shape this record already has; a second field would
  // make three legal states and one nonsense one, and `io/StrokesSerial`
  // would have to decide what the nonsense one deserialises to.
  //
  // **And it is a stored SOURCE POLICY, not a stored correction.** Baking the
  // solved patch into `rgba` would be storing pixels, which is the one thing
  // core/StrokesContent exists to avoid -- the correction is a function of
  // what lies beneath, so it has to be recomputed when what lies beneath
  // changes or PRD D6 holds once and never again. `brush/StrokesLayer` §1b
  // carries the evaluation and the two decisions it makes.
  BelowHealed,
};

// One stored dab.
//
// Plain aggregate with defaults, `Layer`'s own shape: there is no invariant
// here a constructor could enforce that `strokesRasterize()` does not have to
// re-check anyway (a radius of zero covers nothing, a NaN position covers
// nothing), and every other content struct in core/ is an aggregate.
struct DabRecord {
  // Layer-local and stable across a save (`StrokesContent::nextDabId` hands
  // them out), for `VectorShape::id`'s reason: a selection, an undo entry or
  // a future Fills/Strokes panel row has to survive a reopen, and an index
  // into `dabs` does not survive a deletion.
  uint64_t id = 0;

  // The stroke this dab belonged to. PRD F8 makes one stroke one undo step,
  // and a future "delete the whole stroke I just grazed" gesture needs the
  // grouping -- recording it costs eight bytes and cannot be recovered later.
  // Zero means "no stroke", which is what a dab placed programmatically has.
  uint64_t strokeId = 0;

  // Document texels, the dab CENTRE. Section 3 on why they are floats.
  float x = 0.0f;
  float y = 0.0f;

  // `BrushTip`'s four shape fields, with `BrushTip`'s defaults and
  // `BrushTip`'s meanings -- see section 1 on why the names match exactly.
  // `radius` is the semi-MAJOR axis whatever `roundness` holds.
  float radius = 0.0f;
  float hardness = 0.35f;
  float roundness = 1.0f;
  float angle = 0.0f;

  // The dab's own opacity ceiling, in [0,1]. One number rather than
  // `BrushTip`'s flow/opacity pair: a stored dab has no stroke around it for
  // a per-stroke ceiling to be a ceiling OVER, so the two collapse into the
  // one quantity that survives -- how much of the source this dab lays down.
  float flow = 1.0f;

  // Linear, straight. Read only when `source == DabColorSource::Ink`; section 3.
  std::array<float, 4> rgba = {0.0f, 0.0f, 0.0f, 1.0f};

  DabColorSource source = DabColorSource::Ink;

  // The offset from the dab centre to the point sampled beneath, in document
  // texels. Read only when `source == DabColorSource::Below`. Zero is legal and
  // means "straight down" -- a heal in place, which is a different operation
  // from a clone and not an unset value.
  float sourceDx = 0.0f;
  float sourceDy = 0.0f;
};

// The whole content of a `LayerKind::Strokes` layer.
struct StrokesContent {
  // Paint order, first to last -- `VectorShape`'s "bottom to top" one
  // dimension down. Evaluation composites them in this order, so the vector
  // IS the z-order and a reorder is a move within it.
  std::vector<DabRecord> dabs;

  // Hands out `DabRecord::id`, exactly as `Layer::nextShapeId` hands out
  // `VectorShape::id` and for that member's stated reason. Serialised, so a
  // reopened layer cannot mint an id a live undo entry still names.
  uint64_t nextDabId = 1;
};

// The half-open texel rectangle one dab can possibly cover: `[x0, x1) x
// [y0, y1)`. Empty (x1 <= x0) for a dab that covers nothing -- a
// non-positive, infinite or NaN radius, or a non-finite centre.
//
// **The bound is the CIRCUMSCRIBING square of the major axis, not the
// ellipse's own tight box.** A tight box would have to re-derive
// `dabCoverage()`'s rotation, which is exactly the duplication section 1
// exists to avoid; the difference is at most a factor of two in area on a
// heavily rotated narrow tip, and every consumer of this rectangle
// (`StrokesIndex`, the rasteriser's scan, PRD F11's erase) re-tests coverage
// or distance per texel anyway. An over-estimate is therefore slower and
// never wrong; an under-estimate would silently clip dabs.
struct DabBounds {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
  bool empty() const noexcept { return x1 <= x0 || y1 <= y0; }
};
DabBounds dabRecordBounds(const DabRecord& dab) noexcept;

// A content hash over every field of every dab, plus `nextDabId`.
//
// `flatsContentHash()`'s and `textContentHash()`'s job for this kind, and it
// has that pair's two consumers: core/DirtyTiles pass 1 (without which a
// Strokes edit is invisible until something else dirties the canvas -- see
// that file for the defect this exact absence caused for Flats) and
// brush/StrokesLayer's evaluation cache. One function, so the two cannot
// disagree about what "changed" means.
//
// Bit patterns, not values: a dab nudged by a float's last bit is a change,
// and `-0.0f` and `0.0f` hashing differently is harmless where hashing them
// the same would be a missed edit.
uint64_t strokesContentHash(const StrokesContent& content) noexcept;

// The hash of the FIRST `n` dabs alone, `nextDabId` excluded.
//
// This is what makes checkpoint tiles possible (brush/StrokesLayer §2): a
// checkpoint taken after `n` dabs is reusable exactly when the first `n` dabs
// are still the same dabs, and this is that question asked in one number.
// `strokesContentHash()` cannot answer it -- it covers the whole list -- and
// comparing the prefixes element by element would cost the very replay the
// checkpoint exists to avoid.
uint64_t strokesPrefixHash(const StrokesContent& content, size_t n) noexcept;

// ==========================================================================
// The spatial index (PLAN.md phase 8: "spatial index over dab bounds")
// ==========================================================================
//
// A uniform grid of `kCellSize`-square buckets holding the indices of every
// dab whose `dabRecordBounds()` overlaps the bucket. Built from a
// `StrokesContent` and read-only afterwards.
//
// **A grid rather than a tree**, and the argument is the data rather than a
// preference. Dabs on a Strokes layer are a stroke's worth of near-identical
// discs laid a fraction of a radius apart: they are dense, they are uniform
// in size, and they arrive in spatial order. That is the distribution a
// uniform grid is exactly right for and the one a k-d tree or a BVH gains
// least on, and a grid has no rebalancing, no allocation per node and a build
// that is one pass over the dabs. If a Strokes layer ever holds geometry with
// a thousandfold size range, this is the file to revisit -- nothing outside
// it depends on the structure.
//
// **`kCellSize` is the tile size** (core/Tile.hpp's 64), not a tuned
// constant. A repair covering a region asks this index a question whose
// answer feeds a tile-store write, so a query rectangle is nearly always
// tile-aligned already, and matching the two means a whole-tile query touches
// exactly one bucket. A dab larger than one cell simply lands in several,
// which the build handles by iterating its bounds.
class StrokesIndex {
 public:
  static constexpr int kCellSize = 64;

  StrokesIndex() = default;
  explicit StrokesIndex(const StrokesContent& content);

  // The indices into `content.dabs` of every dab whose bounds overlap the
  // half-open rectangle `[x0, x1) x [y0, y1)`, ASCENDING and without
  // duplicates -- so a caller can composite them in paint order without
  // sorting, which is the order every caller wants.
  //
  // **Exactly those, not "those plus whatever shared a cell with them".** The
  // grid narrows the candidates and then each candidate's stored bounds are
  // re-tested against the rectangle, which is why `bounds_` exists. The
  // looser contract was tried first and is not worth having: a caller that
  // has to re-derive the bounds to know whether an answer is really an answer
  // is doing the index's job, and the two derivations would drift.
  //
  // It is still a BROAD phase in the remaining sense: an index returned here
  // has overlapping BOUNDS, not necessarily overlapping COVERAGE, because the
  // bounds circumscribe the ellipse. Callers re-test per texel, which they
  // had to do anyway.
  std::vector<size_t> query(int x0, int y0, int x1, int y1) const;

  // How many buckets the grid actually allocated, and how many dabs it
  // indexed. For `--selftest`: an index that quietly degenerated to one
  // bucket holding everything would answer every query correctly and be a
  // linear scan wearing a grid's name, which is precisely the failure this
  // class exists to prevent and precisely the one no correctness assertion
  // can see.
  size_t bucketCount() const noexcept { return buckets_.size(); }
  size_t dabCount() const noexcept { return dabCount_; }

  // How many dab entries the grid holds in total -- the sum of every
  // bucket's size, which exceeds `dabCount()` by exactly the multi-cell
  // spill. Also for `--selftest`, and for the same reason.
  size_t entryCount() const noexcept;

 private:
  // Keyed on the cell coordinate rather than a dense array over the canvas:
  // a Strokes layer's dabs occupy the few cells a hand drew through, and a
  // dense grid over a 4K canvas would allocate 4096 buckets to use nine of
  // them. Sparse costs a hash per lookup and nothing else.
  //
  // `int64_t` key = `(cellY << 32) | (uint32_t)cellX`, so negative cells
  // (a dab hanging off the top-left of the canvas, which is legal --
  // core/Tile.hpp's coordinates are signed) key correctly.
  std::vector<std::pair<int64_t, std::vector<size_t>>> buckets_;
  // One entry per dab, parallel to `content.dabs`, so `query()` can make its
  // promise exact without holding the content. An empty rectangle here is a
  // dab that covers nothing and is therefore in no bucket at all.
  std::vector<DabBounds> bounds_;
  size_t dabCount_ = 0;

  const std::vector<size_t>* bucket(int64_t key) const noexcept;
};

// ==========================================================================
// PRD F11 -- "erasing a Strokes layer deletes the dab records it covers,
// not pixels"
// ==========================================================================
//
// Deletes from `content` every dab the erasing disc at `(cx, cy)` of radius
// `r` covers, in one pass, and appends the deleted records to `removedOut`
// when it is non-null (the caller wants them to know which tiles to redraw,
// and an undo entry wants them to put back). Returns how many were deleted.
//
// **"Covers" means the disc contains the dab's CENTRE, and that is the
// decision this function makes.** The alternative -- delete any dab whose
// bounds or coverage the disc touches at all -- makes a single grazing
// contact remove a dab whose visible mark is mostly somewhere else, so a
// small eraser dragged along the edge of a stroke deletes the whole stroke.
// Centre-containment is the rule under which the eraser removes what it is
// over and nothing else, and it is the rule a user can predict from the
// cursor. It also makes the operation idempotent in the obvious way: erasing
// the same disc twice deletes nothing the second time.
//
// **The eraser's own falloff is deliberately not consulted.** A dab record is
// deleted or it is not; there is no partial deletion, so a soft eraser's rim
// has nothing to express. Softening the ERASE would mean scaling the covered
// dabs' `flow`, which is a pixel-like edit to the records and exactly the
// "erase paints pixels" behaviour F11 exists to replace.
//
// The index is rebuilt by the caller if it needs one afterwards; this
// function does not hold one, because deleting from the middle of `dabs`
// invalidates every index above the deletion and rebuilding is one pass.
size_t eraseDabsUnderDisc(StrokesContent& content, float cx, float cy, float r,
                          std::vector<DabRecord>* removedOut);

}  // namespace np
