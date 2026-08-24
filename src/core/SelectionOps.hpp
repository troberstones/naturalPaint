#pragma once

// Boolean algebra over selection coverage -- PRD E7 (add, subtract, intersect)
// and the invert half of PRD E4.
//
// These are separate from core/SelectionMask because that file owns the *type*
// and the one constructor that makes a rectangle. This one owns what you do
// with two of them, and it is where the antialiasing question actually gets
// decided.
//
// --- Why max/min, and not saturating add -----------------------------------
//
// Coverage is a real number in [0,1], so "union" has no single right answer.
// A texel that is 0.5 covered by A and 0.5 covered by B could be anywhere from
// 0.5 selected (the two halves are the same half) to 1.0 selected (they are
// disjoint halves). The store does not record *which* half, so any rule here
// is a choice between defensible bounds:
//
//     max(a, b)        the lower bound -- assumes maximal overlap
//     a + b - a*b      independence
//     min(1, a + b)    the upper bound -- assumes disjointness
//
// This file uses the **fuzzy-set triple**: add is `max`, intersect is `min`,
// complement is `1 - a`, and subtract is defined as intersect-with-complement,
// `min(a, 1 - b)`. Three properties bought:
//
//   1. **Idempotence.** Shift-dragging the same rectangle twice must leave the
//      selection identical. Under saturating add it does not -- the second add
//      pushes every fractional edge texel toward 1 and the antialiased boundary
//      hardens a little each time. A user repeating a gesture because they were
//      not sure it registered would silently degrade their own selection edge.
//
//   2. **De Morgan closure.** `subtract(a, b) == intersect(a, invert(b))`
//      *exactly*, so subtract is not a third independently-invented formula
//      that can drift away from the other two. The selftest asserts the
//      identity rather than trusting it.
//
//   3. **Associativity**, so a chain of adds does not depend on the order the
//      user drew them in.
//
// The cost is confined to texels where **both** inputs are fractional -- that
// is, where two antialiased edges cross. Everywhere else the rules agree: an
// edge of B falling inside a fully-selected A gives 1.0 under every formula,
// and an edge of B falling outside A gives B's own coverage under every
// formula. So this is a decision about a sliver of texels, made for the three
// algebraic properties above rather than for a visual difference anyone can
// see.
//
// --- Tile sparsity ---------------------------------------------------------
//
// Each operation knows which tiles can possibly be non-zero, and never
// allocates outside that set:
//
//   add        union of both tile sets      (absent tile is 0; max(a,0) = a)
//   subtract   a subset of `base`'s tiles   (nothing new can become selected)
//   intersect  a subset of the intersection (min(a,0) = 0 outside either)
//   invert     dense over the document      -- see the warning on invertSelection()
//
// All four restore core/SelectionMask's constructor invariant on the way out:
// **no returned tile is entirely zero**. `selectRectangle()` maintains it by
// skipping zero writes; subtract and intersect can drive a whole tile to zero,
// so they drop it instead of leaving 16 KiB of "not selected" resident.

#include <cstdint>

#include "core/SelectionMask.hpp"

namespace np {

// How a newly-drawn selection combines with the one already installed.
//
// The order matters and is fixed by the UI's modifier convention, not by
// taste: `base` is what was already there, `addend` is what the user just
// drew. `Subtract` removes the new shape from the old one, which is what
// Option-dragging means everywhere -- the reverse would make the modifier read
// as "keep only what I just drew, minus everything else".
enum class SelectionCombine {
  Replace,    // no modifier -- the new shape, and the old one is discarded
  Add,        // Shift        -- max(a, b)
  Subtract,   // Option/Alt   -- min(a, 1 - b)
  Intersect,  // Shift+Option -- min(a, b)
};

// The modifier convention, as a function rather than as a conditional buried
// in the mouse handler: Shift adds, Option/Alt subtracts, both intersect,
// neither replaces.
//
// It lives here, next to the algebra it selects, for one reason: this mapping
// is a four-row table that every editor shares and that a user's hands know
// better than their conscious mind does, and if it ever inverts nobody will
// see a crash -- they will see an application that deletes their selection
// when they meant to extend it. Inline in ui/MacPaintUI it was unreachable by
// any test. Here it is one call, and --selftest asserts all four rows.
SelectionCombine selectionCombineFromModifiers(bool shift, bool alt) noexcept;

// The four rules above, on a single pair of coverages. Exposed because the
// per-texel loops below are not the only consumer -- the quick-mask path (PRD
// E12) will combine coverage that never sat in a tile store -- and because a
// selftest that checks the algebra should check the *same* arithmetic the
// stores run, not a re-typed copy of it.
float combineCoverage(float base, float addend, SelectionCombine op) noexcept;

// `base` combined with `addend`. Neither input is modified.
//
// `Replace` returns a copy of `addend` and ignores `base` entirely, so a call
// site that has already resolved the user's modifiers into a SelectionCombine
// can call this unconditionally instead of branching around it.
Selection combineSelections(const Selection& base, const Selection& addend,
                            SelectionCombine op);

// The complement over a `width` x `height` document: every texel becomes
// `1 - coverage`. PRD E4's invert.
//
// **This is the one operation here that is dense, and it is dense by
// definition.** A marquee in a corner of a 4K document occupies a handful of
// tiles; its inverse selects everything else, which is every tile the document
// has -- 1024 of them at 4096x4096, so 16 MiB of coverage. That is not an
// implementation shortfall to be optimised away later, it is what the answer
// is. A caller that inverts in a loop should expect to pay it each time.
//
// Texels outside the document are not represented at all: the complement is
// taken *within* the given bounds, so inverting twice returns the original
// clipped to the document. For a selection that was built inside the document
// -- which is all of them, because `selectRectangle()` is fed clamped corners
// -- that clip is a no-op and the round trip is exact.
//
// A `width` or `height` of zero yields a selection with no tiles.
Selection invertSelection(const Selection& selection, int32_t width, int32_t height);

}  // namespace np
