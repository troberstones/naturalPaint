#include "app/selftest/Support.hpp"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "core/Path.hpp"
#include "core/PathRaster.hpp"
#include "core/PathStroke.hpp"
#include "io/PsdVectorPath.hpp"

// io/PsdVectorCompose -- step 2 of docs/psd-vector-shapes.md.
//
// Headless, GPU-free. Every fixture below is a `PsdPathStream` built by hand
// in this file, not read from a decoder: `decodePsdPathRecords()` (step 1)
// is a separate module with its own suite, and hand-built streams let this
// file reach cases no sample PSD happens to contain (an unrecognised
// operation code; Intersect; Exclude mixed with Union).
//
// The strongest assertions here RASTERISE the composed path and read actual
// coverage at chosen points, rather than inspecting `path.rule`. A test that
// only checks the enum passes on a build that sets the fill rule correctly
// and then assembles the subpaths wrong -- e.g. forgetting to reverse a
// subtracted subpath produces a `NonZero` path whose "hole" is solid, and an
// enum-only assertion would not notice.
namespace np {
namespace {

// A closed square subpath, every handle coincident with its own anchor
// (core/Path.hpp section 1's straight-line encoding) so this is a genuine
// polygon rather than a curve that happens to look like one.
SubPath squareSubPath(float x0, float y0, float x1, float y1) {
  SubPath sub;
  sub.closed = true;
  for (const PathPoint& p : {PathPoint{x0, y0}, PathPoint{x1, y0}, PathPoint{x1, y1},
                              PathPoint{x0, y1}}) {
    Anchor a;
    a.pt = a.in = a.out = p;
    sub.anchors.push_back(a);
  }
  return sub;
}

PsdSubPath makeSubPath(SubPath sub, PsdPathOp op, bool opKnown = true, int16_t rawOp = 1) {
  PsdSubPath s;
  s.sub = std::move(sub);
  s.op = op;
  s.opKnown = opKnown;
  s.rawOp = rawOp;
  return s;
}

// Rasterise into a dense float image so a test can index a specific point.
// Mirrors app/selftest/PathRaster.cpp's own helper -- not shared with it,
// because that file is scoped to core/PathRaster's own claims and this one
// to io/PsdVectorCompose's, and the two would otherwise be a shared-bug risk
// for a five-line function.
std::vector<float> rasterizeToImage(const Path& path, int32_t w, int32_t h) {
  std::vector<float> img(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
  PathRasterScratch scratch;
  RasterClip clip{0, 0, w, h};
  rasterizePath(path, 0.05f, clip, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x)
                    img[static_cast<size_t>(y) * static_cast<size_t>(w) +
                        static_cast<size_t>(x)] = cov[x - x0];
                });
  return img;
}

float sampleAt(const std::vector<float>& img, int32_t w, int32_t h, int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
  return img[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
}

double sumOf(const std::vector<float>& img) {
  double s = 0.0;
  for (float v : img) s += v;
  return s;
}

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

bool runPsdVectorComposeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  std::printf("  -- A. A hole is actually a hole (Union+Subtract), and the test is\n");
  std::printf("        proven sensitive to the reversal that makes it one --\n");
  // ==========================================================================
  {
    // A 100x100 square (Union, subpath 0 -- so its literal op is irrelevant)
    // minus a concentric 20x20 square (Subtract).
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 100, 100), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(40, 40, 60, 60), PsdPathOp::Subtract));

    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "A1: Union+Subtract of a square and a concentric square composes (ok)");
    check(composed.path.rule == FillRule::NonZero, "A1: the composed path's fill rule is NonZero");
    check(pathIsFinite(composed.path), "A1: the composed path is finite");

    const std::vector<float> img = rasterizeToImage(composed.path, 100, 100);
    const float centre = sampleAt(img, 100, 100, 50, 50);
    const float ring = sampleAt(img, 100, 100, 10, 10);
    std::printf("  [measured] centre coverage %.4f, ring coverage %.4f\n", centre, ring);
    check(centre < 0.01f, "A2: rasterised centre (inside the subtracted square) is EMPTY");
    check(ring > 0.99f, "A2: rasterised ring (outside it, inside the outer square) is FILLED");

    // Sensitivity oracle: the SAME two subpaths, reversal skipped, prove the
    // test above actually depends on `composePsdSubPaths()` reversing the
    // subtracted subpath and not merely on the fill rule it chose. Under
    // NonZero, an unreversed inner square shares the outer square's winding
    // direction, so the two windings ADD inside it (2) rather than cancel
    // (0) -- both nonzero, so both fill.
    Path unreversed;
    unreversed.rule = FillRule::NonZero;
    unreversed.subpaths.push_back(squareSubPath(0, 0, 100, 100));
    unreversed.subpaths.push_back(squareSubPath(40, 40, 60, 60));  // NOT reversed
    const float centreUnreversed = sampleAt(rasterizeToImage(unreversed, 100, 100), 100, 100, 50, 50);
    std::printf("  [measured] centre coverage WITHOUT reversal: %.4f\n", centreUnreversed);
    check(centreUnreversed > 0.99f,
          "A3: the identical fixture WITHOUT reversal fills the centre -- this test is "
          "sensitive to the reversal, not just to the fill rule");
  }

  // ==========================================================================
  std::printf("  -- B. Exclude is XOR: overlap empty, the rest filled --\n");
  // ==========================================================================
  {
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 60, 60), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(30, 0, 90, 60), PsdPathOp::Exclude));

    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "B1: an Exclude pair composes (ok)");
    check(composed.path.rule == FillRule::EvenOdd, "B1: the composed path's fill rule is EvenOdd");

    const std::vector<float> img = rasterizeToImage(composed.path, 90, 60);
    const float overlap = sampleAt(img, 90, 60, 45, 30);
    const float leftOnly = sampleAt(img, 90, 60, 10, 30);
    const float rightOnly = sampleAt(img, 90, 60, 80, 30);
    std::printf("  [measured] overlap %.4f, left-only %.4f, right-only %.4f\n", overlap, leftOnly,
                rightOnly);
    check(overlap < 0.01f, "B2: the overlapping region is EMPTY under XOR");
    check(leftOnly > 0.99f && rightOnly > 0.99f, "B2: the non-overlapping regions are FILLED");
  }

  // ==========================================================================
  std::printf("  -- C. All-Union overlap does not punch a hole --\n");
  // ==========================================================================
  {
    // The case that silently breaks if EvenOdd is picked here by accident:
    // two overlapping same-winding squares must stay solid in the overlap.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 60, 60), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(30, 0, 90, 60), PsdPathOp::Union));

    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "C1: an all-Union pair composes (ok)");
    check(composed.path.rule == FillRule::NonZero, "C1: the composed path's fill rule is NonZero");

    const std::vector<float> img = rasterizeToImage(composed.path, 90, 60);
    const float overlap = sampleAt(img, 90, 60, 45, 30);
    std::printf("  [measured] overlapping region coverage: %.4f\n", overlap);
    check(overlap > 0.99f, "C2: the overlapping region of two same-winding squares is FILLED");
  }

  // ==========================================================================
  std::printf("  -- D. Intersect, and Exclude mixed with Union, compose exactly --\n");
  // ==========================================================================
  {
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 10, 10), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(5, 5, 15, 15), PsdPathOp::Intersect));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const std::vector<float> img = rasterizeToImage(composed.path, 16, 16);
    std::printf("  [measured] intersect area %.4f\n", sumOf(img));
    check(composed.ok && std::fabs(sumOf(img) - 25.0) < 0.05,
          "D1: Union then Intersect of [0,10]^2 and [5,15]^2 imports the 5x5 overlap, area 25");
    check(sampleAt(img, 16, 16, 7, 7) > 0.99f && sampleAt(img, 16, 16, 2, 2) < 0.01f &&
              sampleAt(img, 16, 16, 12, 12) < 0.01f,
          "D1: the overlap is FILLED, and what only one square covers is EMPTY");
    bool named = false;
    for (const std::string& w : composed.warnings)
      if (contains(w, "Intersect") && contains(w, "straight segments")) named = true;
    check(named, "D1: a warning names Intersect and says its curves became straight segments");
  }
  {
    // (A xor B) union C = 150 + 100 = 250.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 10, 10), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(5, 5, 15, 15), PsdPathOp::Exclude));
    stream.subpaths.push_back(makeSubPath(squareSubPath(20, 20, 30, 30), PsdPathOp::Union));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const std::vector<float> img = rasterizeToImage(composed.path, 31, 31);
    std::printf("  [measured] exclude-then-union area %.4f\n", sumOf(img));
    check(composed.ok && std::fabs(sumOf(img) - 250.0) < 0.05,
          "D2: Union, Exclude, Union folds left to right: (A xor B) + C = 250");
    check(sampleAt(img, 31, 31, 7, 7) < 0.01f && sampleAt(img, 31, 31, 2, 2) > 0.99f &&
              sampleAt(img, 31, 31, 25, 25) > 0.99f,
          "D2: the excluded overlap is EMPTY; A's own part and C are FILLED");
    bool named = false;
    for (const std::string& w : composed.warnings)
      if (contains(w, "Exclude")) named = true;
    check(named, "D2: a warning names the Exclude mix");
  }

  // ==========================================================================
  std::printf("  -- E. Empty stream, a single subpath, and an unrecognised op code --\n");
  // ==========================================================================
  {
    const PsdPathStream empty;
    const PsdComposedPath composed = composePsdSubPaths(empty);
    check(composed.ok, "E1: an empty stream composes (ok == true, nothing to refuse)");
    check(composed.path.subpaths.empty(), "E1: the composed path has no subpaths");
    check(pathIsFinite(composed.path), "E1: the (empty) composed path is finite");
  }
  {
    // A single subpath's own operation is never read (decision 1) -- so an
    // UNKNOWN op code on the only subpath still composes, because there is
    // nothing for it to combine with.
    PsdPathStream stream;
    stream.subpaths.push_back(
        makeSubPath(squareSubPath(0, 0, 40, 40), PsdPathOp::Union, /*opKnown=*/false,
                    /*rawOp=*/17));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "E2: a single subpath composes (ok) even with an unrecognised op code");
    check(composed.path.rule == FillRule::NonZero, "E2: a lone subpath's fill rule is NonZero");
    const double area = sumOf(rasterizeToImage(composed.path, 40, 40));
    std::printf("  [measured] lone-subpath area: expected 1600.0, got %.3f\n", area);
    check(std::fabs(area - 1600.0) < 1.0, "E2: the lone subpath's full area is filled, not "
                                          "treated as a hole with nothing to subtract from");
  }
  {
    // The same unrecognised op code, on a SECOND subpath, is a real vote the
    // layer can't be composed without: decision 3 refuses the whole layer
    // rather than guessing Union or silently dropping the subpath.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 40, 40), PsdPathOp::Union));
    stream.subpaths.push_back(
        makeSubPath(squareSubPath(5, 5, 10, 10), PsdPathOp::Union, /*opKnown=*/false,
                    /*rawOp=*/17));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(!composed.ok, "E3: a non-first subpath with an unrecognised op code refuses the layer");
    check(contains(composed.refusal, "17"), "E3: the refusal names the unrecognised code (17)");
  }

  // ==========================================================================
  std::printf("  -- F. MergeWithPrevious: a letter and its counter, exactly the case\n");
  std::printf("        the header names -- inherits Union, is NEVER reversed --\n");
  // ==========================================================================
  {
    // The realistic, documented use of MergeWithPrevious: one drawn figure,
    // two subpaths, no boolean relationship between them at all. The hole is
    // authored directly as OPPOSITE winding (the font/SVG convention), not
    // produced by reversal on this side -- so the inner subpath is built
    // with its corners in the reverse order from `squareSubPath()`'s, and is
    // never passed through `reverseSubPath()` because its effective op
    // resolves to Union (inherited from subpath 0), which this module never
    // reverses.
    SubPath counter;
    counter.closed = true;
    for (const PathPoint& p : {PathPoint{30, 70}, PathPoint{70, 70}, PathPoint{70, 30},
                                PathPoint{30, 30}}) {  // Reverse order from squareSubPath().
      Anchor a;
      a.pt = a.in = a.out = p;
      counter.anchors.push_back(a);
    }

    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 100, 100), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(counter, PsdPathOp::MergeWithPrevious));

    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "F1: a Union figure plus its MergeWithPrevious counter composes (ok)");
    check(composed.path.rule == FillRule::NonZero, "F1: the composed path's fill rule is NonZero");

    const std::vector<float> img = rasterizeToImage(composed.path, 100, 100);
    const float outerRing = sampleAt(img, 100, 100, 10, 10);
    const float counterHole = sampleAt(img, 100, 100, 50, 50);
    std::printf("  [measured] outer ring %.4f, counter (hole) %.4f\n", outerRing, counterHole);
    check(outerRing > 0.99f, "F2: the outer ring is FILLED");
    check(counterHole < 0.01f, "F2: the counter is a HOLE, purely from its authored opposite "
                               "winding -- this module reverses nothing here");
  }

  // Note what this section does NOT claim: a MergeWithPrevious subpath
  // chained after a SUBTRACT subpath (rather than after Union, as above) is
  // not something any sample file exercises. This module's policy for that
  // case -- inherit Subtract, and so be reversed along with it -- is this
  // track's own extrapolation of "inherits the previous operation", not a
  // claim verified against a real render.

  // ==========================================================================
  std::printf("  -- G. Known-answer geometry: Apple's App Icon Shape, a full-canvas\n");
  std::printf("        rectangle minus an inner blob -- corners filled, centre empty --\n");
  // ==========================================================================
  {
    // docs/psd-vector-shapes.md and this track's known-answers.txt: subpath 1
    // is the exact 1024x1024 document rectangle (op 1); subpath 2 is a
    // 44-knot squircle (op 2) this track cannot decode (that is
    // `decodePsdPathRecords()`, a different module). The geometry that
    // matters for THIS module -- a big Union rectangle minus an inner
    // Subtract blob -- is exactly reproducible by hand; the stand-in below
    // is an octagon approximating the squircle's rounded-square silhouette,
    // not the real 44 knots, so pixel counts are NOT compared against the
    // real file's 57,136 -- only the qualitative claim the doc states in
    // words: the four corners are filled and the centre is not.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 1024, 1024), PsdPathOp::Union));

    SubPath blob;
    blob.closed = true;
    const float c = 512.0f, r = 420.0f, cut = 160.0f;  // An octagon inscribed near the canvas.
    const std::vector<PathPoint> octagon = {
        {c - r + cut, c - r}, {c + r - cut, c - r}, {c + r, c - r + cut},
        {c + r, c + r - cut}, {c + r - cut, c + r}, {c - r + cut, c + r},
        {c - r, c + r - cut}, {c - r, c - r + cut}};
    for (const PathPoint& p : octagon) {
      Anchor a;
      a.pt = a.in = a.out = p;
      blob.anchors.push_back(a);
    }
    stream.subpaths.push_back(makeSubPath(blob, PsdPathOp::Subtract));

    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "G1: rectangle-minus-blob composes (ok)");
    check(pathIsFinite(composed.path), "G1: the composed path is finite");

    const std::vector<float> img = rasterizeToImage(composed.path, 1024, 1024);
    const float centre = sampleAt(img, 1024, 1024, 512, 512);
    const float cornerTL = sampleAt(img, 1024, 1024, 10, 10);
    const float cornerTR = sampleAt(img, 1024, 1024, 1013, 10);
    const float cornerBL = sampleAt(img, 1024, 1024, 10, 1013);
    const float cornerBR = sampleAt(img, 1024, 1024, 1013, 1013);
    std::printf("  [measured] centre %.4f, corners %.4f %.4f %.4f %.4f\n", centre, cornerTL,
                cornerTR, cornerBL, cornerBR);
    check(centre < 0.01f, "G2: the centre (inside the subtracted blob) is EMPTY");
    check(cornerTL > 0.99f && cornerTR > 0.99f && cornerBL > 0.99f && cornerBR > 0.99f,
          "G2: all four corners (outside the blob, inside the rectangle) are FILLED");
  }

  // ==========================================================================
  std::printf("  -- H. A real decoded fixture: Apple's PNG/1 circle (single subpath,\n");
  std::printf("        all-Union) composes and rasterises to its true area --\n");
  // ==========================================================================
  {
    // Knot coordinates from this track's known-answers.txt, already decoded
    // (by the other track's step-1 module, not by anything here) into
    // document space: a circle, centre (512, 357), r = 256, printed there as
    // (in.x,in.y) (pt.x,pt.y) (out.x,out.y) per knot. Reproduced verbatim
    // rather than resynthesised, so this fixture is a real file's geometry
    // and not a shape merely believed to be a circle.
    SubPath circle;
    circle.closed = true;
    struct Knot { PathPoint in, pt, out; };
    const std::vector<Knot> knots = {
        {{370.6151f, 101.0000f}, {512.0000f, 101.0000f}, {653.3849f, 101.0000f}},
        {{768.0000f, 215.6151f}, {768.0000f, 357.0000f}, {768.0000f, 498.3849f}},
        {{653.3849f, 613.0000f}, {512.0000f, 613.0000f}, {370.6151f, 613.0000f}},
        {{256.0000f, 498.3849f}, {256.0000f, 357.0000f}, {256.0000f, 215.6151f}},
    };
    for (const Knot& k : knots) {
      Anchor a;
      a.in = k.in;
      a.pt = k.pt;
      a.out = k.out;
      circle.anchors.push_back(a);
    }

    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(circle, PsdPathOp::Union));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "H1: the real PNG/1 circle fixture composes (ok)");
    check(pathIsFinite(composed.path), "H1: the composed path is finite");

    const double area = sumOf(rasterizeToImage(composed.path, 1024, 1024));
    const double expected = 3.14159265358979 * 256.0 * 256.0;
    std::printf("  [measured] circle area: expected ~%.1f (pi r^2), got %.3f\n", expected, area);
    // A 4-knot cubic circle approximation (the standard 0.5523 magic
    // constant) is not an exact circle; this tolerance is generous enough to
    // pass that construction and tight enough to fail a wrong radius or a
    // hole where there should be a disc.
    check(std::fabs(area - expected) < expected * 0.01,
          "H2: rasterised area matches pi*r^2 within 1%");
  }

  // --- I. The one-fill-rule shortcut is checked exactly ------------------
  //
  // Each unsound fixture is paired with the same subpaths hand-built into the
  // shortcut's compound, proving the shortcut really does draw it wrong -- so
  // a pass means the check caught something, not that nothing was there.
  auto shortcutOf = [](std::vector<std::pair<SubPath, bool>> subs) {
    Path p;
    for (auto& [sub, subtract] : subs) {
      if (subtract) reverseSubPath(sub);
      p.subpaths.push_back(sub);
    }
    return p;
  };
  auto warnedExact = [](const PsdComposedPath& c) {
    for (const std::string& w : c.warnings)
      if (contains(w, "computed exactly")) return true;
    return false;
  };
  {
    // A hole on TWO layers of fill: union 6000, minus 200 = 5800.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 60, 60), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(40, 0, 100, 60), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(45, 20, 55, 40), PsdPathOp::Subtract));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const std::vector<float> img = rasterizeToImage(composed.path, 100, 60);
    const Path naive = shortcutOf({{squareSubPath(0, 0, 60, 60), false},
                                   {squareSubPath(40, 0, 100, 60), false},
                                   {squareSubPath(45, 20, 55, 40), true}});
    std::printf("  [measured] doubly covered: area %.3f, hole %.4f, shortcut's hole %.4f\n",
                sumOf(img), sampleAt(img, 100, 60, 50, 30),
                sampleAt(rasterizeToImage(naive, 100, 60), 100, 60, 50, 30));
    check(sampleAt(rasterizeToImage(naive, 100, 60), 100, 60, 50, 30) > 0.99f,
          "I1 premise: the shortcut compound re-fills a hole that sits on two fills");
    check(composed.ok && sampleAt(img, 100, 60, 50, 30) < 0.01f &&
              std::fabs(sumOf(img) - 5800.0) < 0.1,
          "I1: the import leaves that hole EMPTY, area 6000 - 200 = 5800");
    check(warnedExact(composed), "I1: and warns that the outline was computed exactly");
  }
  {
    // A subtraction over NO fill: a bounding-box test has nothing to compare
    // it with, and the shortcut paints the reversed square solid.
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 40, 40), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(squareSubPath(60, 60, 80, 80), PsdPathOp::Subtract));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const std::vector<float> img = rasterizeToImage(composed.path, 80, 80);
    const Path naive =
        shortcutOf({{squareSubPath(0, 0, 40, 40), false}, {squareSubPath(60, 60, 80, 80), true}});
    check(sampleAt(rasterizeToImage(naive, 80, 80), 80, 80, 70, 70) > 0.99f,
          "I2 premise: the shortcut fills a subtracted square that has no fill beneath it");
    check(composed.ok && sampleAt(img, 80, 80, 70, 70) < 0.01f &&
              std::fabs(sumOf(img) - 1600.0) < 0.1 && warnedExact(composed),
          "I2: the import leaves it EMPTY (area 1600) and warns");
  }
  {
    // A hole authored with the OPPOSITE winding: the shortcut's reversal turns
    // it back into fill.
    SubPath reversedHole = squareSubPath(40, 40, 60, 60);
    reverseSubPath(reversedHole);
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 100, 100), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(reversedHole, PsdPathOp::Subtract));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const std::vector<float> img = rasterizeToImage(composed.path, 100, 100);
    const Path naive =
        shortcutOf({{squareSubPath(0, 0, 100, 100), false}, {reversedHole, true}});
    check(sampleAt(rasterizeToImage(naive, 100, 100), 100, 100, 50, 50) > 0.99f,
          "I3 premise: the shortcut fills a subtracted hole authored with opposite winding");
    check(composed.ok && sampleAt(img, 100, 100, 50, 50) < 0.01f &&
              std::fabs(sumOf(img) - 9600.0) < 0.1 && warnedExact(composed),
          "I3: the import leaves it EMPTY (area 10000 - 400 = 9600) and warns");
  }
  {
    // The sound case keeps the shortcut, and with it the curves: a circular
    // hole inside a square comes back with its handles, and no warning.
    SubPath hole;
    hole.closed = true;
    const float k = 0.5522847498f * 20.0f;
    const PathPoint pts[4] = {{50, 30}, {30, 50}, {10, 30}, {30, 10}};
    const PathPoint tan[4] = {{0, k}, {-k, 0}, {0, -k}, {k, 0}};
    for (int j = 0; j < 4; ++j) {
      Anchor a;
      a.pt = pts[j];
      a.in = {pts[j].x - tan[j].x, pts[j].y - tan[j].y};
      a.out = {pts[j].x + tan[j].x, pts[j].y + tan[j].y};
      hole.anchors.push_back(a);
    }
    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(squareSubPath(0, 0, 60, 60), PsdPathOp::Union));
    stream.subpaths.push_back(makeSubPath(hole, PsdPathOp::Subtract));
    const PsdComposedPath composed = composePsdSubPaths(stream);
    const bool curved = composed.path.subpaths.size() == 2 &&
                        composed.path.subpaths[1].anchors.size() == 4 &&
                        (composed.path.subpaths[1].anchors[0].in.x !=
                             composed.path.subpaths[1].anchors[0].pt.x ||
                         composed.path.subpaths[1].anchors[0].in.y !=
                             composed.path.subpaths[1].anchors[0].pt.y);
    check(composed.ok && composed.warnings.empty() && curved,
          "I4: a circular hole in one fill keeps the curve-preserving compound, and warns of "
          "nothing");
  }

  // ==========================================================================
  std::printf("  -- K. sawOpenSubPath decided: an open subpath's fill closes it, its\n");
  std::printf("        stroke genuinely does not -- both correct, so neither warns --\n");
  // ==========================================================================
  {
    // Three sides of a square, OPEN on the fourth (the bottom, y=0): every
    // handle coincident with its own anchor, so these are straight edges and
    // the only variable between the two shapes built from this is `closed`.
    auto threeSidedSquare = [](bool closed) {
      SubPath sub;
      sub.closed = closed;
      for (const PathPoint& p :
           {PathPoint{0, 0}, PathPoint{0, 100}, PathPoint{100, 100}, PathPoint{100, 0}}) {
        Anchor a;
        a.pt = a.in = a.out = p;
        sub.anchors.push_back(a);
      }
      return sub;
    };

    PsdPathStream stream;
    stream.subpaths.push_back(makeSubPath(threeSidedSquare(/*closed=*/false), PsdPathOp::Union));
    stream.sawOpenSubPath = true;  // what decodePsdPathRecords() would have set (step 1)
    const PsdComposedPath composed = composePsdSubPaths(stream);
    check(composed.ok, "K1: an open subpath composes (ok)");
    bool anyOpenWarning = false;
    for (const std::string& w : composed.warnings)
      if (w.find("open") != std::string::npos || w.find("Open") != std::string::npos)
        anyOpenWarning = true;
    check(!anyOpenWarning,
          "K1: composing it warns of nothing naming 'open' -- there is nothing wrong to report");

    // FILL: PathRaster closes the missing fourth side implicitly, so the
    // WHOLE square fills, not just the three drawn sides.
    const float fillCentre = sampleAt(rasterizeToImage(composed.path, 100, 100), 100, 100, 50, 50);
    std::printf("  [measured] open subpath's FILL at centre: %.4f\n", fillCentre);
    check(fillCentre > 0.99f,
          "K2: the fill of an open subpath closes implicitly -- the centre is FILLED, matching "
          "SVG's and Photoshop's own rule for filling an open subpath");

    // STROKE: core/PathStroke must NOT invent the missing fourth side. Same
    // four anchors, only `closed` differs, so this isolates the flag rather
    // than comparing two different shapes.
    StrokeStyle style;
    style.width = 10.0f;
    Path openPath;
    openPath.subpaths.push_back(threeSidedSquare(/*closed=*/false));
    Path closedPath;
    closedPath.subpaths.push_back(threeSidedSquare(/*closed=*/true));
    const Path openStroke = strokePath(openPath, style, 0.25f);
    const Path closedStroke = strokePath(closedPath, style, 0.25f);
    const float openBottom = sampleAt(rasterizeToImage(openStroke, 100, 100), 100, 100, 50, 0);
    const float closedBottom = sampleAt(rasterizeToImage(closedStroke, 100, 100), 100, 100, 50, 0);
    std::printf("  [measured] stroke coverage at the missing 4th side's midpoint: open %.4f, "
                "closed %.4f\n",
                openBottom, closedBottom);
    check(openBottom < 0.01f,
          "K3: the OPEN subpath's stroke draws nothing along the side it never drew -- "
          "core/PathStroke does not close it");
    check(closedBottom > 0.99f,
          "K3: the IDENTICAL anchors marked CLOSED do stroke that side -- proving the assertion "
          "above tests `closed`, not some other difference in the geometry");
  }

  std::printf("[selftest] psd vector compose %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
