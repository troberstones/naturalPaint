#pragma once

#include <string>
#include <vector>

#include "brush/BrushModelFields.hpp"

namespace np {

// brush/BrushModelDiff -- the two questions `presetMatches()` and a
// round-trip test both need answered against the FULL model, not the 14
// scalars `presetMatches()` compares today: "are these the same brush" and,
// when they are not, "which of its ~151 leaf fields say so."
//
// **Why ~151, not the ~117 the model's own header estimates.** That count
// treats each of `Variance`'s five fields (`control`, `jitter`, `minimum`,
// `fadeSteps`, `present`) as four, and it counts `PsTipShape` and `PsScatter`
// once each even though `PsDualBrush` embeds a second copy of both (its own
// `tip`, its own `scatter`) -- so the model's own estimate is a lower bound,
// not this file's contract. `brushModelDiffPaths().size()` below is the
// actual number, pinned by app/selftest/BrushModelDiff.cpp so a field added
// without a visitor line here fails loudly instead of silently under-
// counting by one.
std::vector<std::string> brushModelDiff(const BrushModel& a, const BrushModel& b);

bool brushModelEqual(const BrushModel& a, const BrushModel& b) noexcept;

// Every path the two functions above can ever name, in the same stable
// order they walk -- the complete field list, independent of any two actual
// models. Exists because the selftest's "every field is reachable" section
// needs a ground truth to walk, and the honest way to get one is to ask this
// file for it rather than maintain a second, hand-written mirror that would
// drift from BrushModel exactly the way a missed visitor line would.
//
// A parallel unit (brush/BrushModelIo.hpp, someone else's file) builds its
// own equivalent for serialisation. Deliberately not shared -- see this
// file's .cpp for the full reason -- so this function exists here too,
// rather than this file depending on that one.
std::vector<std::string> brushModelDiffPaths();

// ---------------------------------------------------------------------------
// detail -- the one visitor everything above is built on.
//
// It has to live in the header, not the .cpp, because it is a template and
// its callers span two translation units: brush/BrushModelDiff.cpp (which
// instantiates it read-only, `A = B = const BrushModel`, to build the three
// functions above) and app/selftest/BrushModelDiff.cpp (which instantiates
// it with `A = BrushModel` to WRITE exactly the field a "one field at a
// time" test wants changed, and nothing else). Both instantiations walk the
// identical field list below; there is exactly one place where a field can
// be forgotten, and it is exactly one line long per field.
//
// `A` and `B` are deduced independently at every level, down to a `Variance`
// or a `DabRef`, rather than fixed to `BrushModel`/`const BrushModel` up
// front. That is what makes the mutable instantiation possible without a
// second copy of this file: the compiler infers `A = BrushModel&` (so
// `visit()` gets a writable leaf) when the selftest passes a non-const
// model, and `A = const BrushModel&` when brushModelDiff() passes two const
// ones, from ONE template body.
//
// `Visit` is called as `visit(path, aLeaf, bLeaf)` and returns whether the
// walk should continue. brushModelDiff() and brushModelDiffPaths() always
// return true (they want every leaf visited); brushModelEqual() returns
// false on the first mismatch, which is the whole reason it is cheaper than
// calling brushModelDiff() and checking `.empty()`.
//
// No stability promise on this namespace's shape beyond what the two
// callers above use -- it is `detail` because nothing outside
// brush/BrushModelDiff.cpp and its selftest should be reaching in here.
// ---------------------------------------------------------------------------
// The visitor this file is built on now lives in brush/BrushModelFields.hpp,
// because brush/BrushModelIo needs the identical walk and two copies of a
// 151-field list is three edits per new field with only a count assertion
// standing behind the two a reviewer forgets. That header carries the full
// reasoning; nothing about the functions above changed when it moved.

}  // namespace np
