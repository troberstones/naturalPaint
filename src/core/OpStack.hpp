#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ops/PointOps.hpp"

// core/OpStack (PLAN.md "Phase 3 -- Grade it", step 5; docs/operations.md §1
// and §1.3's four-letter op-class taxonomy; ADR-0004
// (docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)).
//
// An ordered stack of grading operations, plus the run-detection logic a
// later, unbuilt step (color/LutBake) needs: ADR-0004's "maximal run of
// adjacent point ops" that "bakes onto a 32^3 grid... in one compute
// dispatch." detectRuns() below produces exactly those run boundaries, each
// paired with its own ready-to-use, ordered list of ops::PointOp -- so
// color/LutBake's whole job, once it exists, is to consume what
// detectRuns() already computed, not to recompute run boundaries itself.
//
// *** Op is a kind tag plus fields only meaningful for that kind, not a
// std::variant -- matching core/Layer.hpp's own established idiom ***
// core/Layer.hpp is this module's structural precedent: `LayerKind` is a
// real enum with a value for every kind CONTEXT.md names, but only `RGB` has
// real behaviour wired up today, and `Layer` itself is "kind tag + fields
// only meaningful for that kind," not a std::variant<...>. `Op` follows the
// same shape for the same reason -- this codebase has no std::variant use
// anywhere yet, and introducing one here (for six point-op kinds, several
// carrying per-channel arrays) would be a bigger style deviation than
// reusing the fat-struct pattern Layer already established.
namespace np {

// docs/operations.md §1's own four-letter op-class taxonomy: A = parametric
// point (folded into the shaper + 3-D LUT, ~zero marginal cost per op), B =
// parametric spatial (its own ROI-bounded pass), C = recorded stroke
// (replayed from stored geometry), D = baked (copy-on-write into tiles).
// Only PointA has real, working ops anywhere in this codebase today
// (ops/PointOps.hpp's six functions) -- SpatialB/StrokeC/BakedD exist purely
// so OpClass is a real, general tag and detectRuns()'s run-boundary logic
// below is written against "is this class A," not hard-coded to "everything
// is always class A" in a way a later phase (once a real class-B/C/D op
// exists) would have to rewrite. Nothing in this codebase constructs a
// non-PointA Op with meaningful data yet -- a bare `Op{.opClass =
// SpatialB}` (or similar) is only ever built as a test fixture to exercise
// run-splitting, not a real spatial/stroke/baked op (none of those three
// classes has an implementation anywhere in this codebase).
enum class OpClass {
  PointA,
  SpatialB,
  StrokeC,
  BakedD,
};

// Which of ops/PointOps.hpp's six functions a PointA-classed Op wraps.
// Meaningful only when the owning Op's `opClass == OpClass::PointA` -- see
// Op::pointKind below.
enum class PointOpKind {
  Levels,
  Curves,
  Exposure,
  Saturation,
  Grayscale,
  ChannelMixer,
};

// One entry in an OpStack.
struct Op {
  // Which of the four op classes this entry is. Defaults to PointA since
  // that's the only class with real behaviour today -- see OpClass's own
  // doc comment above.
  OpClass opClass = OpClass::PointA;

  // Whether this op is included when the stack is evaluated. A *disabled*
  // op still occupies a slot in the stack (and, if it's PointA, still keeps
  // its run undivided -- see OpStack::detectRuns()'s doc comment for why),
  // it is simply excluded from the composed op list a run produces.
  bool enabled = true;

  // Which ops/PointOps.hpp function the params fields below select.
  // Meaningful only when `opClass == OpClass::PointA`; for any other class
  // this field (and every params field below) is left at its default value
  // and never read.
  PointOpKind pointKind = PointOpKind::Levels;

  // One field per PointOpKind, holding that op's params type from
  // ops/PointOps.hpp. Only the field matching `pointKind` is meaningful,
  // and only when `opClass == PointA` -- the exact "populated only for the
  // relevant kind" convention core/Layer.hpp documents for its own
  // `rgbTiles` field. Every field not selected by `pointKind` (or, for a
  // non-PointA entry, every field here) sits at its default-constructed
  // (neutral/identity) value and is never read by anything in this module.
  std::array<LevelsParams, 3> levels{};
  std::array<Curve, 3> curves{};
  ExposureParams exposure{};
  SaturationParams saturation{};
  GrayscaleParams grayscale{};
  ChannelMixerParams channelMixer{};
};

// One maximal contiguous run of OpClass::PointA entries, as detected by
// OpStack::detectRuns(). `[startIndex, endIndex)` is the run's half-open
// index range into the OpStack it came from -- endIndex is exclusive.
// `ops` holds only the *enabled* entries in the run, in stack order, each
// already turned into a callable ops::PointOp bound to that entry's own
// params -- ready to hand straight to ops::applyPointOpsPremultiplied() (or
// a future color/LutBake) with no further lookup needed. Note `ops.size()`
// can be smaller than `endIndex - startIndex`: a disabled PointA entry
// occupies a slot in the index range but contributes nothing to `ops` (see
// detectRuns()'s doc comment below).
struct OpRun {
  size_t startIndex = 0;
  size_t endIndex = 0;  // exclusive
  std::vector<PointOp> ops;
};

// An ordered stack of grading Ops, plus a version counter a future
// color/LutBake can compare across frames to decide whether to rebake.
class OpStack {
 public:
  size_t size() const noexcept { return ops_.size(); }

  // Bounds-checked (throws std::out_of_range on an invalid index, via
  // std::vector::at's own contract). A read-only accessor -- does not
  // touch version().
  const Op& at(size_t index) const { return ops_.at(index); }

  // Appends `op` to the end of the stack and returns its new index
  // (`size() - 1` after the append). Increments version().
  size_t add(Op op);

  // Removes the entry at `index`; every later entry shifts down by one.
  // Increments version().
  void remove(size_t index);

  // Moves the entry currently at `from` so it ends up at index `to` in the
  // resulting stack -- the standard "move an element to a new position"
  // semantics: every entry strictly between the old and new position shifts
  // by one to make room, everything else keeps its index. Increments
  // version().
  void reorder(size_t from, size_t to);

  // Replaces `index`'s enabled flag. Increments version().
  void setEnabled(size_t index, bool enabled);

  // Replaces `index`'s entire entry (e.g. a curve was re-authored, or the
  // op's class/kind itself changed). Increments version().
  void setOp(size_t index, Op op);

  // Monotonically increasing; bumped by every mutator above, including a
  // setEnabled()/setOp() call that happens not to change anything
  // observable (e.g. setEnabled(i, at(i).enabled), passing the value it
  // already had). This is deliberate: simplicity over micro-optimizing away
  // redundant version bumps. The only consumer is a future color/LutBake
  // comparing versions across frames to decide whether to rebake -- for
  // that consumer, an occasional spurious rebake from an over-eager bump is
  // cheap and harmless, whereas under-incrementing and missing a real
  // change would silently leave a stale bake on screen, a far worse and
  // much harder to notice failure. So every mutator bumps unconditionally,
  // without first checking whether the new value actually differs from the
  // old one.
  uint64_t version() const noexcept { return version_; }

  // Walks the stack once, grouping maximal contiguous runs of
  // OpClass::PointA entries -- a run boundary occurs at every non-PointA
  // entry, and implicitly at the stack's own start/end. An empty stack (or
  // one with no PointA entries at all) returns no runs.
  //
  // Non-obvious rule, stated plainly because it's the one genuinely subtle
  // design decision in this module and easy to get backwards: a *disabled*
  // PointA entry does NOT split a run. Op *class*, not *enabled state*,
  // defines a run boundary. A disabled op is the identity function on
  // whatever runs through it -- exactly the same as "not present at all"
  // from a LUT-baking point of view -- so treating it as a split point
  // would force an unnecessary run break (and an unnecessary extra LUT
  // bake) every time a user temporarily toggles an op off, which is
  // precisely the case a maximal-run collapse exists to make cheap. The
  // disabled entry still occupies its slot in the run's
  // `[startIndex, endIndex)` range; it is simply excluded from that run's
  // `.ops` list, contributing nothing to the composed chain -- the correct
  // behaviour for an identity function either way.
  std::vector<OpRun> detectRuns() const;

 private:
  std::vector<Op> ops_;
  uint64_t version_ = 0;
};

}  // namespace np
