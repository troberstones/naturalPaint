#include "io/PsdVectorPath.hpp"

#include <string>
#include <utility>
#include <vector>

#include "core/PathBoolean.hpp"

// io/PsdVectorCompose -- step 2 of docs/psd-vector-shapes.md: fold a
// `PsdPathStream`'s per-subpath boolean operations into one `Path`.
//
// Two routes to the same region. The EXACT one folds the subpaths left to
// right with core/PathBoolean, which is Photoshop's own meaning and handles
// all four operations, but flattens curves. The SHORTCUT is one compound path
// with one fill rule (subtracted subpaths reversed under NonZero, or EvenOdd
// for all-Exclude), which keeps every curve editable but is only right when
// each subtracted region sits on exactly one layer of fill. So every layer the
// shortcut can express is also folded exactly, and the shortcut is kept only
// when the two regions match.
//
// Decisions the wire format forces:
//
// 1. Subpath 0's operation is never read: nothing precedes it to combine
//    with, and a literal Subtract there would import a filled shape as nothing.
// 2. `MergeWithPrevious` marks the later subpaths of one drawn figure (a
//    letter and its counter). It joins its predecessor's group as authored
//    winding and inherits that group's operation; it is never an operation of
//    its own.
// 3. A non-first subpath with an unrecognised operation code refuses the
//    whole layer by name rather than guessing.
// 4. `sawOpenSubPath` changes nothing: core/PathRaster fills an open contour
//    as closed and core/PathStroke caps it, which is what Photoshop does too.
namespace np {

namespace {

// Below this many square document pixels, the shortcut and the exact fold are
// the same region up to flattening noise.
constexpr double kSameRegionArea = 0.25;

PathBooleanOp booleanFor(PsdPathOp op) {
  switch (op) {
    case PsdPathOp::Subtract: return PathBooleanOp::Difference;
    case PsdPathOp::Intersect: return PathBooleanOp::Intersect;
    case PsdPathOp::Exclude: return PathBooleanOp::Xor;
    case PsdPathOp::Union:
    case PsdPathOp::MergeWithPrevious: break;
  }
  return PathBooleanOp::Union;
}

Path foldExactly(const PsdPathStream& stream, const std::vector<PsdPathOp>& effective) {
  std::vector<std::pair<PsdPathOp, Path>> groups;
  for (size_t i = 0; i < stream.subpaths.size(); ++i) {
    if (i == 0 || stream.subpaths[i].op != PsdPathOp::MergeWithPrevious)
      groups.push_back({effective[i], Path{}});
    groups.back().second.subpaths.push_back(stream.subpaths[i].sub);
  }
  Path acc = std::move(groups[0].second);
  for (size_t g = 1; g < groups.size(); ++g)
    acc = pathBoolean(booleanFor(groups[g].first), acc, groups[g].second);
  return acc;
}

}  // namespace

PsdComposedPath composePsdSubPaths(const PsdPathStream& stream) {
  PsdComposedPath result;
  const size_t n = stream.subpaths.size();
  if (n == 0) {
    result.ok = true;
    return result;
  }

  std::vector<PsdPathOp> effective(n);
  effective[0] = PsdPathOp::Union;  // decision 1
  for (size_t i = 1; i < n; ++i) {
    const PsdSubPath& s = stream.subpaths[i];
    if (s.op == PsdPathOp::MergeWithPrevious) {
      effective[i] = effective[i - 1];  // decision 2, chained
    } else if (!s.opKnown) {
      result.ok = false;
      result.refusal = "a subpath's path operation code (" + std::to_string(s.rawOp) +
                        ") matches none of PSD's four booleans or merge-with-previous, so "
                        "this layer can't be composed";
      return result;
    } else {
      effective[i] = s.op;
    }
  }

  bool sawUnion = false, sawSubtract = false, sawExclude = false, sawIntersect = false;
  for (size_t i = 1; i < n; ++i) {
    switch (effective[i]) {
      case PsdPathOp::Union: sawUnion = true; break;
      case PsdPathOp::Subtract: sawSubtract = true; break;
      case PsdPathOp::Exclude: sawExclude = true; break;
      case PsdPathOp::Intersect: sawIntersect = true; break;
      case PsdPathOp::MergeWithPrevious: break;
    }
  }
  const bool mixedExclude = sawExclude && (sawSubtract || sawUnion);
  result.ok = true;

  if (!sawIntersect && !mixedExclude) {
    Path shortcut;
    shortcut.rule = sawExclude ? FillRule::EvenOdd : FillRule::NonZero;
    for (size_t i = 0; i < n; ++i) {
      SubPath sub = stream.subpaths[i].sub;
      // EvenOdd ignores winding; under NonZero the reversal is what cancels.
      if (!sawExclude && effective[i] == PsdPathOp::Subtract) reverseSubPath(sub);
      shortcut.subpaths.push_back(std::move(sub));
    }
    if (n == 1) {
      result.path = std::move(shortcut);
      return result;
    }
    Path exact = foldExactly(stream, effective);
    if (pathBooleanArea(pathBoolean(PathBooleanOp::Xor, shortcut, exact)) <= kSameRegionArea) {
      result.path = std::move(shortcut);
      return result;
    }
    result.path = std::move(exact);
    result.warnings.push_back(
        "a single fill rule over this layer's subpaths would not match Photoshop (a "
        "subtracted or excluded region is not covered exactly once by the fill beneath it), "
        "so its outline was computed exactly and its curves import as straight segments");
    return result;
  }

  result.path = foldExactly(stream, effective);
  result.warnings.push_back(
      std::string(sawIntersect ? "this layer uses Intersect"
                               : "this layer mixes Exclude with Union/Subtract") +
      ", so its outline was computed exactly and its curves import as straight segments");
  return result;
}

}  // namespace np
