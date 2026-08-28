#pragma once

#include <string>
#include <vector>

#include "brush/BrushModelFields.hpp"

namespace np {

// brush/BrushModelIo -- the text format for a `BrushModel`, and the ONE
// visitor every field of it passes through to get there.
//
// **Why one visitor and not ~117 branches.** `BrushModel` is Photoshop's
// panel shape: ~57 scalar/enum/string leaves plus 15 `Variance` members, each
// of which is itself 5 leaves (see brush/Variance.hpp). Hand-writing a
// to-string and a from-string branch per leaf is ~234 places a leaf's name
// has to be typed correctly twice, and BrushModel.hpp's own history is
// exactly this failure mode -- `multiplyFloor[Roundness]` imported and never
// applied (audit B7's sibling) because a field existed in the struct and
// nowhere else. `visitBrushModelFields()` below is the one place a leaf's
// path is spelled out; `BrushModelIo.cpp`'s `brushModelToLines()`,
// `brushModelApplyLine()` and `brushModelFieldPaths()` are each a single call
// to it with a different `fn`, at ~20 lines apiece. Adding a field to
// `BrushModel` means adding exactly one call here, in the one spot that
// matches its type; the three functions in the .cpp do not change shape.
//
// **Paths are the C++ member name, joined by `.`.** Not a shorter invented
// name: a path is a persisted key, and the member name is the one spelling
// that already exists in BrushModel.hpp. `dual.tip.roundness` is
// `BrushModel::dual` (a `PsDualBrush`) `.tip` (its own `PsTipShape`)
// `.roundness` -- read the path back against the struct and it names the
// member chain exactly.

// **The walk lives in brush/BrushModelFields.hpp**, not here. This file had
// its own copy of it until integration, and so did brush/BrushModelDiff --
// two independently written enumerations of the same 151 leaves, which is
// three edits per new field and two chances to forget one. That header also
// deduces constness, so `brushModelToLines()` no longer needs the
// `const_cast` that reusing a mutable-only walk for reading required.


// Every non-default field of `m`, as "<path> <value>" strings WITHOUT the
// leading keyword. A field equal to a default-constructed `BrushModel`'s own
// value is omitted, so a default model produces zero lines -- a saved preset
// costs bytes proportional to what an artist actually changed, not to the
// panel's full size.
std::vector<std::string> brushModelToLines(const BrushModel& m);

// Applies one "<path> <value>" string to `m`. Returns false for an unknown
// path or an unparseable value and leaves `m` untouched either way -- a
// preset that half-applies is worse than one that is refused outright,
// because the failure is silent until the artist notices a setting that
// never changed.
bool brushModelApplyLine(BrushModel& m, const std::string& line);

// Every path this build knows, in the visitor's own order. Exists for
// `--selftest`: its count is what `runBrushModelIoTest()` pins to a literal,
// so a field added to `BrushModel` without a matching call in
// `visitBrushModelFields()` fails a test instead of silently shipping
// unreachable by any save.
std::vector<std::string> brushModelFieldPaths();

}  // namespace np
