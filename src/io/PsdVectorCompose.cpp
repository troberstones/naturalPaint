#include "io/PsdVectorPath.hpp"

#include <algorithm>
#include <string>
#include <vector>

// io/PsdVectorCompose -- step 2 of docs/psd-vector-shapes.md: fold a
// `PsdPathStream`'s per-subpath boolean operations into the one thing this
// codebase can draw, a single compound `Path` with a single `FillRule`.
//
// Four decisions this file makes, each forced by a case the wire format
// allows and the header's own table does not spell out completely:
//
// 1. Subpath 0's operation is never read. An operation describes how a NEW
//    subpath combines with what has already been accumulated, and nothing
//    precedes the first one -- so whatever the file wrote there (every
//    sample seen is `Union`, but nothing guarantees that) describes no real
//    combination. A first subpath that happened to read `Subtract` would,
//    taken literally, subtract from an empty accumulator and vanish -- a
//    normal filled shape imported as nothing. So subpath 0 always folds in
//    UNREVERSED, and it never casts a vote in the family decision below.
//
// 2. `MergeWithPrevious` (-1) is Photoshop's marker for "the second and
//    later subpaths of one drawn figure" -- a letter and its counter, one
//    shape, two subpaths. The counter's hole is already present as opposite
//    winding in the geometry, not as a boolean relationship to referee, so a
//    merged subpath casts no vote of its own either: it inherits the
//    resolved operation of the subpath immediately before it (chained, so a
//    run of several merges all resolve to whatever real operation started
//    the chain) and is reversed, or not, exactly as that operation would be.
//
// 3. An empty stream composes to an empty, `ok = true` path (nothing to
//    refuse). A single subpath is just subpath 0's case above: always
//    Union, always unreversed, regardless of what its own `op` or
//    `opKnown` says, because there is nothing for it to combine with. A
//    non-first subpath whose `opKnown` is false refuses the WHOLE layer by
//    name (rather than guessing Union or dropping the subpath silently) --
//    this module's whole reason to exist is refusing rather than guessing.
//
// 4. `sawOpenSubPath` changes nothing here. `core/PathRaster.cpp` fills a
//    contour as though it were closed regardless of its own `closed` flag
//    (its own comment: "An open contour is filled as if closed"), so an
//    open subpath composes exactly like a closed one. Whether an open path
//    in a *fill* is a modelling mistake worth a warning is the caller's
//    question, decided with the layer name this function never sees.
namespace np {

namespace {

// Control-point bounds of one subpath in isolation -- a conservative
// superset of the curve it actually draws (`pathControlBounds()`'s own
// guarantee), used only by the heuristic below.
PathBounds subPathControlBounds(const SubPath& sub) {
  Path single;
  single.subpaths.push_back(sub);
  return pathControlBounds(single);
}

// The rectangle intersection of two bounds, `valid == false` when they don't
// overlap (or either input doesn't). Shared by the pairwise checks below so
// "does A overlap B" and "does C overlap the region where A and B overlap"
// are the same primitive.
PathBounds intersectBounds(const PathBounds& a, const PathBounds& b) {
  PathBounds r;
  if (!a.valid || !b.valid) return r;
  const float minX = std::max(a.minX, b.minX);
  const float minY = std::max(a.minY, b.minY);
  const float maxX = std::min(a.maxX, b.maxX);
  const float maxY = std::min(a.maxY, b.maxY);
  if (minX < maxX && minY < maxY) {
    r.valid = true;
    r.minX = minX;
    r.minY = minY;
    r.maxX = maxX;
    r.maxY = maxY;
  }
  return r;
}

}  // namespace

PsdComposedPath composePsdSubPaths(const PsdPathStream& stream) {
  PsdComposedPath result;
  const size_t n = stream.subpaths.size();

  // Decision 3, empty case: nothing to draw and nothing to refuse. A
  // default-constructed `Path` has no subpaths, so `pathIsFinite()` and
  // `pathIsEmpty()` are both true of it vacuously.
  if (n == 0) {
    result.ok = true;
    return result;
  }

  // Resolve every subpath's EFFECTIVE operation: decisions 1 and 2 above,
  // applied in one forward pass so a chain of several `MergeWithPrevious`
  // subpaths all resolve to whatever real operation started the chain.
  std::vector<PsdPathOp> effective(n);
  effective[0] = PsdPathOp::Union;  // Decision 1 -- never actually voted on.

  bool unknownOpSeen = false;
  int16_t unknownRaw = 0;
  for (size_t i = 1; i < n; ++i) {
    const PsdSubPath& s = stream.subpaths[i];
    if (s.op == PsdPathOp::MergeWithPrevious) {
      effective[i] = effective[i - 1];
    } else if (!s.opKnown) {
      unknownOpSeen = true;
      unknownRaw = s.rawOp;
      effective[i] = PsdPathOp::Union;  // Placeholder; refused below anyway.
    } else {
      effective[i] = s.op;
    }
  }

  if (unknownOpSeen) {
    result.ok = false;
    result.refusal = "a subpath's path operation code (" + std::to_string(unknownRaw) +
                      ") matches none of PSD's four booleans or merge-with-previous, so "
                      "this layer can't be composed";
    return result;
  }

  // The vote: subpaths 1..n-1 only -- subpath 0 is decision 1's neutral base
  // and never contributes an operation to referee.
  bool sawUnion = false, sawSubtract = false, sawExclude = false, sawIntersect = false;
  for (size_t i = 1; i < n; ++i) {
    switch (effective[i]) {
      case PsdPathOp::Union: sawUnion = true; break;
      case PsdPathOp::Subtract: sawSubtract = true; break;
      case PsdPathOp::Exclude: sawExclude = true; break;
      case PsdPathOp::Intersect: sawIntersect = true; break;
      case PsdPathOp::MergeWithPrevious: break;  // Resolved away above.
    }
  }

  if (sawIntersect) {
    result.ok = false;
    result.refusal = "this layer uses Intersect, which no single fill rule over reversed "
                      "subpaths can express";
    return result;
  }
  if (sawExclude && (sawSubtract || sawUnion)) {
    result.ok = false;
    result.refusal = "this layer mixes Exclude with Union/Subtract on one path, and only "
                      "one fill rule can apply to it";
    return result;
  }

  // What remains is exactly the header's two sound families: all-Union
  // (trivially including "no votes at all", i.e. a single subpath), or
  // Union+Subtract; or, separately, all-Exclude.
  const bool allExclude = sawExclude;
  result.path.rule = allExclude ? FillRule::EvenOdd : FillRule::NonZero;

  for (size_t i = 0; i < n; ++i) {
    SubPath sub = stream.subpaths[i].sub;
    // EvenOdd's XOR does not care about winding direction, so a reversal
    // would change nothing except which side of each edge is "outside" --
    // Subtract only needs reversing under NonZero, where winding sign is
    // the thing that cancels a hole into existence.
    if (!allExclude && effective[i] == PsdPathOp::Subtract) reverseSubPath(sub);
    result.path.subpaths.push_back(std::move(sub));
  }
  result.ok = true;

  // Soundness heuristic, Union+Subtract only. Reversal is exact wherever a
  // subtracted region is covered by exactly one layer of Union coverage; it
  // OVER-subtracts (nonzero winding cancels twice and un-cancels) wherever
  // that region is covered TWICE -- by two overlapping Union pieces
  // underneath it, or by a second Subtract subpath overlapping it. Telling
  // that apart exactly is the boolean-ops pass this codebase does not have;
  // this is a cheap, CONSERVATIVE stand-in using control-point bounding
  // boxes (a superset of the true curve -- `pathControlBounds()`'s own
  // guarantee), so it can warn on a layer that in fact renders fine (boxes
  // touch, curves don't), but it never misses a real double-coverage.
  if (sawSubtract) {
    std::vector<PathBounds> unionBoxes, subtractBoxes;
    for (size_t i = 0; i < n; ++i) {
      const PathBounds b = subPathControlBounds(stream.subpaths[i].sub);
      if (effective[i] == PsdPathOp::Subtract)
        subtractBoxes.push_back(b);
      else
        unionBoxes.push_back(b);
    }
    bool doubledCoverage = false;
    for (size_t i = 0; i < unionBoxes.size() && !doubledCoverage; ++i) {
      for (size_t j = i + 1; j < unionBoxes.size() && !doubledCoverage; ++j) {
        const PathBounds overlap = intersectBounds(unionBoxes[i], unionBoxes[j]);
        if (!overlap.valid) continue;
        for (const PathBounds& hole : subtractBoxes) {
          if (intersectBounds(overlap, hole).valid) {
            doubledCoverage = true;
            break;
          }
        }
      }
    }
    for (size_t i = 0; i < subtractBoxes.size() && !doubledCoverage; ++i) {
      for (size_t j = i + 1; j < subtractBoxes.size() && !doubledCoverage; ++j) {
        if (intersectBounds(subtractBoxes[i], subtractBoxes[j]).valid) doubledCoverage = true;
      }
    }

    if (doubledCoverage) {
      result.warnings.push_back(
          "this layer's Union+Subtract may not match Photoshop exactly: a subtracted "
          "region appears to be covered more than once (by overlapping fill pieces, or "
          "by another subtracted region), which can make winding reversal re-fill part "
          "of a hole");
    }
  }

  return result;
}

}  // namespace np
