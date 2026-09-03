#include "app/selftest/Support.hpp"

#include <cmath>

#include "core/PathFlatten.hpp"
#include "io/SvgPath.hpp"

namespace np {

// io/SvgPath -- SVG's text-to-geometry grammars: the `d` path mini-language,
// `transform`, lengths/units, `viewBox`/`preserveAspectRatio`, and the basic
// shapes as paths. Pure parsing, headless and GPU-free; writes no files. See
// app/SelfTest.hpp for the full list of what this section proves.
bool runSvgPathTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto near = [](float a, float b, float tol = 1.0e-4f) { return std::fabs(a - b) <= tol; };
  auto nearPt = [&](PathPoint a, PathPoint b, float tol = 1.0e-4f) {
    return near(a.x, b.x, tol) && near(a.y, b.y, tol);
  };

  // ==========================================================================
  // 1. The number lexer -- where real files break a naive implementation.
  // ==========================================================================
  {
    std::vector<float> nums;
    check(parseSvgNumberList("1.5.5", &nums) && nums.size() == 2 && near(nums[0], 1.5f) &&
              near(nums[1], 0.5f),
          "lexer: '1.5.5' is two numbers, 1.5 and .5 (second '.' closes the first)");

    check(parseSvgNumberList("10-5", &nums) && nums.size() == 2 && near(nums[0], 10.0f) &&
              near(nums[1], -5.0f),
          "lexer: '10-5' is two numbers, 10 and -5 (sign closes the first)");

    check(parseSvgNumberList("1e3", &nums) && nums.size() == 1 && near(nums[0], 1000.0f),
          "lexer: '1e3' is one number");

    check(parseSvgNumberList("1.5e-3", &nums) && nums.size() == 1 && near(nums[0], 0.0015f),
          "lexer: '1.5e-3' is one number, exponent sign does not split it");

    check(parseSvgNumberList(".5 -.5 +5", &nums) && nums.size() == 3 && near(nums[0], 0.5f) &&
              near(nums[1], -0.5f) && near(nums[2], 5.0f),
          "lexer: '.5', '-.5', '+5' all lex");

    check(parseSvgNumberList("1,2 3,4", &nums) && nums.size() == 4,
          "lexer: comma and whitespace are both valid separators");
  }

  // ==========================================================================
  // 2. Every command letter, both cases; implicit lineto after M/m.
  // ==========================================================================
  {
    Path p;
    size_t err = 0;
    check(parseSvgPathData("M0,0 L10,0 H20 V10 Z", &p, &err) && p.subpaths.size() == 1 &&
              p.subpaths[0].closed && p.subpaths[0].anchors.size() == 4,
          "commands: M L H V Z (uppercase) parse into a closed 4-anchor subpath");
    check(nearPt(p.subpaths[0].anchors[1].pt, {10.0f, 0.0f}) &&
              nearPt(p.subpaths[0].anchors[2].pt, {20.0f, 0.0f}) &&
              nearPt(p.subpaths[0].anchors[3].pt, {20.0f, 10.0f}),
          "commands: H moves only x, V moves only y");

    check(parseSvgPathData("m0,0 l10,0 h0 v10 z", &p, &err) && p.subpaths.size() == 1 &&
              p.subpaths[0].closed,
          "commands: m l h v z (lowercase/relative) parse");

    check(parseSvgPathData("M0,0 L10,10 20,20 30,30", &p, &err) && p.subpaths[0].anchors.size() == 4,
          "commands: L repeats without a new letter (three linetos from one L)");

    check(parseSvgPathData("M0,0 M10,10 20,20", &p, &err) && p.subpaths.size() == 2 &&
              p.subpaths[1].anchors.size() == 2,
          "commands: implicit lineto after M -- the second pair is an L, not a second M");
    check(nearPt(p.subpaths[1].anchors[0].pt, {10.0f, 10.0f}) &&
              nearPt(p.subpaths[1].anchors[1].pt, {20.0f, 20.0f}),
          "commands: the implicit lineto after M lands where an L would");

    check(parseSvgPathData("m0,0 m10,10 5,5", &p, &err) && p.subpaths.size() == 2 &&
              nearPt(p.subpaths[1].anchors[1].pt, {15.0f, 15.0f}),
          "commands: implicit lineto after m is relative, matching m's own case");

    check(parseSvgPathData("M0,0 C1,1 2,2 3,3", &p, &err) && p.subpaths[0].anchors.size() == 2,
          "commands: C parses");
    check(parseSvgPathData("M0,0 Q1,1 2,2", &p, &err) && p.subpaths[0].anchors.size() == 2,
          "commands: Q parses");
    check(parseSvgPathData("M0,0 A5,5 0 0,1 10,0", &p, &err),
          "commands: A parses");
  }

  // ==========================================================================
  // 3. Quadratic elevation: sample the elevated cubic against the quadratic.
  // ==========================================================================
  {
    Path p;
    size_t err = 0;
    const PathPoint p0{0.0f, 0.0f}, q{10.0f, 20.0f}, p2{20.0f, 0.0f};
    check(parseSvgPathData("M0,0 Q10,20 20,0", &p, &err), "Q: parses for the elevation check");
    PathPoint cubic[4] = {p.subpaths[0].anchors[0].pt, p.subpaths[0].anchors[0].out,
                          p.subpaths[0].anchors[1].in, p.subpaths[0].anchors[1].pt};
    bool allMatch = true;
    for (float t : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
      const float u = 1.0f - t;
      const PathPoint quadAt{u * u * p0.x + 2.0f * u * t * q.x + t * t * p2.x,
                              u * u * p0.y + 2.0f * u * t * q.y + t * t * p2.y};
      const PathPoint cubicPt = cubicAt(cubic, t);
      if (!nearPt(quadAt, cubicPt, 1.0e-3f)) allMatch = false;
    }
    check(allMatch, "Q->cubic elevation matches the quadratic at several t");
  }

  // ==========================================================================
  // 4. S/T reflection, with and without a preceding matching command.
  // ==========================================================================
  {
    Path p;
    size_t err = 0;
    // C ends with c2 = (10,10) at current = (10,0); a following S must
    // reflect c2 through current: (2*10-10, 2*0-10) = (10,-10).
    check(parseSvgPathData("M0,0 C0,10 10,10 10,0 S20,10 20,0", &p, &err),
          "S: parses after a preceding C");
    check(nearPt(p.subpaths[0].anchors[1].out, {10.0f, -10.0f}),
          "S: reflects the preceding C's c2 through the current point");

    check(parseSvgPathData("M0,0 L10,0 S20,10 20,0", &p, &err),
          "S: parses after a non-curve command");
    check(nearPt(p.subpaths[0].anchors[1].out, {10.0f, 0.0f}),
          "S: with no preceding C/S, the implicit control point is the current point");

    // Q ends with q1 = (10,10) (the quadratic control point itself, not its
    // elevated cubic handles) at current = (20,0); a following T reflects
    // that: (2*20-10, 2*0-10) = (30,-10).
    check(parseSvgPathData("M0,0 Q10,10 20,0 T30,0", &p, &err),
          "T: parses after a preceding Q");
    const PathPoint p0{20.0f, 0.0f};
    const PathPoint reflectedQ1{30.0f, -10.0f};
    const PathPoint expectedC1{p0.x + (2.0f / 3.0f) * (reflectedQ1.x - p0.x),
                                p0.y + (2.0f / 3.0f) * (reflectedQ1.y - p0.y)};
    check(nearPt(p.subpaths[0].anchors[1].out, expectedC1),
          "T: reflects the preceding Q's own quadratic control point (not the elevated cubic one)");

    check(parseSvgPathData("M0,0 L20,0 T30,0", &p, &err),
          "T: parses after a non-curve command");
    check(nearPt(p.subpaths[0].anchors[1].out, p.subpaths[0].anchors[1].pt),
          "T: with no preceding Q/T, the implicit control point is the current point");
  }

  // ==========================================================================
  // 5. Arcs: exact endpoint, zero-radius fallback, the fromOut lifetime rule.
  // ==========================================================================
  {
    Path p;
    size_t err = 0;
    check(parseSvgPathData("M0,0 A50,50 0 1,1 100,0", &p, &err) && p.subpaths[0].anchors.size() > 2,
          "arc: a large-arc sweep produces multiple cubic pieces");
    check(nearPt(p.subpaths[0].anchors.back().pt, {100.0f, 0.0f}, 1.0e-2f),
          "arc: the final anchor lands exactly on the stated endpoint");

    check(parseSvgPathData("M0,0 A0,10 0 0,1 10,0", &p, &err) && p.subpaths[0].anchors.size() == 2,
          "arc: a zero radius degenerates to a single straight line, not a curve");
    check(nearPt(p.subpaths[0].anchors[1].pt, {10.0f, 0.0f}) &&
              nearPt(p.subpaths[0].anchors[1].in, p.subpaths[0].anchors[1].pt),
          "arc: the zero-radius fallback line has straight (coincident) handles");

    // Two arcs back-to-back exercise appending pieces across a call boundary
    // without the fromOut/vector aliasing hazard io/SvgPath.hpp documents.
    check(parseSvgPathData("M0,0 A10,10 0 0,1 20,0 A10,10 0 0,1 40,0", &p, &err) &&
              nearPt(p.subpaths[0].anchors.back().pt, {40.0f, 0.0f}, 1.0e-2f),
          "arc: two consecutive arcs both land correctly");
  }

  // ==========================================================================
  // 6. The SVG 1.1 8.3.1 error contract: false, offset, valid prefix.
  // ==========================================================================
  {
    Path p;
    size_t err = 999;
    const std::string_view bad = "M 0 0 L 1 1 X 5 5";
    check(!parseSvgPathData(bad, &p, &err), "errors: an unknown command letter returns false");
    check(err == bad.find('X'), "errors: errorOffset points at the offending byte");
    check(p.subpaths.size() == 1 && p.subpaths[0].anchors.size() == 2 && !p.subpaths[0].closed,
          "errors: the valid two-anchor prefix (M, L) survives in *out");
    check(nearPt(p.subpaths[0].anchors[0].pt, {0.0f, 0.0f}) &&
              nearPt(p.subpaths[0].anchors[1].pt, {1.0f, 1.0f}),
          "errors: the prefix's anchors are exactly the ones before the error");

    check(!parseSvgPathData("", &p, &err), "errors: empty input is refused");
    check(!parseSvgPathData("   \t\n", &p, &err), "errors: whitespace-only input is refused");
    check(!parseSvgPathData("-", &p, &err), "errors: a lone '-' is refused");
    check(!parseSvgPathData(".", &p, &err), "errors: a lone '.' is refused");
    check(!parseSvgPathData("NaN 0 0", &p, &err), "errors: 'NaN' is refused (not M/m)");
    check(!parseSvgPathData("M inf 0", &p, &err), "errors: an 'inf' spelling is refused");
    check(!parseSvgPathData("L 0 0", &p, &err),
          "errors: a path not starting with a moveto is refused");
    check(!parseSvgPathData("M 0 0 L 5", &p, &err),
          "errors: a command missing an argument is refused");
    check(!parseSvgPathData("M 1e400 0", &p, &err),
          "errors: a syntactically valid number that overflows to infinity is refused");
  }

  // ==========================================================================
  // 7. transform: composition order and the rotation sign convention.
  // ==========================================================================
  {
    Mat3 m;
    check(parseSvgTransform("translate(10,0) scale(2)", &m),
          "transform: 'translate(10,0) scale(2)' parses");
    // Hand arithmetic: scale is applied first (rightmost), then translate --
    // (1,0) -> scale -> (2,0) -> translate -> (12,0).
    Point2 mapped = mat3MapPoint(m, Point2{1.0f, 0.0f});
    check(near(mapped.x, 12.0f) && near(mapped.y, 0.0f),
          "transform: composition applies the rightmost function first (leftmost last)");

    check(parseSvgTransform("rotate(90)", &m), "transform: 'rotate(90)' parses");
    mapped = mat3MapPoint(m, Point2{1.0f, 0.0f});
    check(near(mapped.x, 0.0f) && near(mapped.y, 1.0f),
          "transform: rotate(90) turns (1,0) to (0,1) -- right to down, clockwise on screen");

    check(parseSvgTransform("matrix(1,0,0,1,5,7)", &m), "transform: 'matrix(...)' parses");
    mapped = mat3MapPoint(m, Point2{2.0f, 3.0f});
    check(near(mapped.x, 7.0f) && near(mapped.y, 10.0f),
          "transform: matrix(a b c d e f) maps (x,y) to (ax+cy+e, bx+dy+f)");

    check(!parseSvgTransform("", &m), "transform: an empty attribute is refused");
    check(!parseSvgTransform("frobnicate(1)", &m), "transform: an unknown function name is refused");
  }

  // ==========================================================================
  // 8. Units: every SvgUnit resolved at the stated 96 dpi basis.
  // ==========================================================================
  {
    SvgLengthContext ctx;
    ctx.fontSizePx = 16.0f;
    ctx.xHeightPx = 8.0f;
    ctx.percentBasisPx = 200.0f;

    SvgLength len;
    check(parseSvgLength("10", &len) && len.unit == SvgUnit::User &&
              near(resolveSvgLength(len, ctx), 10.0f),
          "units: a unitless number is SvgUnit::User and resolves unchanged");
    check(parseSvgLength("10px", &len) && len.unit == SvgUnit::Px &&
              near(resolveSvgLength(len, ctx), 10.0f),
          "units: px resolves unchanged");
    check(parseSvgLength("1in", &len) && near(resolveSvgLength(len, ctx), 96.0f),
          "units: 1in == 96px");
    check(parseSvgLength("1pt", &len) && near(resolveSvgLength(len, ctx), 96.0f / 72.0f),
          "units: 1pt == 96/72 px");
    check(parseSvgLength("1pc", &len) && near(resolveSvgLength(len, ctx), 16.0f),
          "units: 1pc == 16px");
    check(parseSvgLength("1cm", &len) && near(resolveSvgLength(len, ctx), 96.0f / 2.54f),
          "units: 1cm == 96/2.54 px");
    check(parseSvgLength("1mm", &len) && near(resolveSvgLength(len, ctx), 96.0f / 25.4f),
          "units: 1mm == 96/25.4 px");
    check(parseSvgLength("2em", &len) && near(resolveSvgLength(len, ctx), 32.0f),
          "units: em multiplies the context font size");
    check(parseSvgLength("2ex", &len) && near(resolveSvgLength(len, ctx), 16.0f),
          "units: ex multiplies the context x-height");
    check(parseSvgLength("50%", &len) && len.unit == SvgUnit::Percent &&
              near(resolveSvgLength(len, ctx), 100.0f),
          "units: percent is a fraction of the context basis");

    check(!parseSvgLength("10zz", &len), "units: an unrecognised unit suffix is refused");
    check(!parseSvgLength("", &len), "units: an empty length is refused");
  }

  // ==========================================================================
  // 9. viewBox and preserveAspectRatio.
  // ==========================================================================
  {
    SvgViewBox box;
    check(parseSvgViewBox("0 0 100 50", &box) && near(box.width, 100.0f) && near(box.height, 50.0f),
          "viewBox: parses four numbers");
    check(!parseSvgViewBox("0 0 -100 50", &box), "viewBox: a negative width is refused");
    check(parseSvgViewBox("0 0 0 50", &box) && near(box.width, 0.0f),
          "viewBox: a zero width parses (it disables rendering, it is not a parse error)");

    SvgPreserveAspectRatio par;
    check(parseSvgPreserveAspectRatio("xMidYMid meet", &par) &&
              par.align == SvgAlign::XMidYMid && par.meetOrSlice == SvgMeetOrSlice::Meet,
          "preserveAspectRatio: 'xMidYMid meet' parses");
    check(parseSvgPreserveAspectRatio("xMinYMax slice", &par) &&
              par.align == SvgAlign::XMinYMax && par.meetOrSlice == SvgMeetOrSlice::Slice,
          "preserveAspectRatio: 'xMinYMax slice' parses");
    check(parseSvgPreserveAspectRatio("none", &par) && par.align == SvgAlign::None,
          "preserveAspectRatio: 'none' parses");
    check(parseSvgPreserveAspectRatio("defer xMidYMid", &par) && par.align == SvgAlign::XMidYMid &&
              par.meetOrSlice == SvgMeetOrSlice::Meet,
          "preserveAspectRatio: a leading 'defer' is accepted and ignored, meet defaults");
    check(!parseSvgPreserveAspectRatio("bogus", &par),
          "preserveAspectRatio: an unrecognised align token is refused");

    parseSvgViewBox("0 0 100 50", &box);
    SvgPreserveAspectRatio meet;
    meet.align = SvgAlign::XMidYMid;
    meet.meetOrSlice = SvgMeetOrSlice::Meet;
    Mat3 m = svgViewBoxTransform(box, 200.0f, 200.0f, meet);
    Point2 mapped = mat3MapPoint(m, Point2{0.0f, 0.0f});
    check(near(mapped.x, 0.0f) && near(mapped.y, 50.0f),
          "viewBox: XMidYMid meet scales by the smaller axis and centres the other");
    mapped = mat3MapPoint(m, Point2{100.0f, 50.0f});
    check(near(mapped.x, 200.0f) && near(mapped.y, 150.0f),
          "viewBox: XMidYMid meet maps the viewBox's far corner inside the meet-scaled box");

    SvgPreserveAspectRatio slice = meet;
    slice.meetOrSlice = SvgMeetOrSlice::Slice;
    m = svgViewBoxTransform(box, 200.0f, 200.0f, slice);
    mapped = mat3MapPoint(m, Point2{0.0f, 0.0f});
    check(near(mapped.x, -100.0f) && near(mapped.y, 0.0f),
          "viewBox: slice scales by the LARGER axis ratio, overflowing the other axis");

    SvgPreserveAspectRatio none;
    none.align = SvgAlign::None;
    m = svgViewBoxTransform(box, 200.0f, 200.0f, none);
    mapped = mat3MapPoint(m, Point2{100.0f, 50.0f});
    check(near(mapped.x, 200.0f) && near(mapped.y, 200.0f),
          "viewBox: SvgAlign::None scales each axis independently to fill exactly");

    SvgViewBox degenerate{0.0f, 0.0f, 0.0f, 50.0f};
    m = svgViewBoxTransform(degenerate, 200.0f, 200.0f, meet);
    mapped = mat3MapPoint(m, Point2{5.0f, 5.0f});
    check(near(mapped.x, 5.0f) && near(mapped.y, 5.0f),
          "viewBox: a zero-width box returns identity rather than dividing by zero");
  }

  // ==========================================================================
  // 10. Shapes: rect rx/ry clamping and defaulting; sanity on the others.
  // ==========================================================================
  {
    Path rectPlain = svgRectPath(0.0f, 0.0f, 20.0f, 10.0f, -1.0f, -1.0f);
    check(rectPlain.subpaths.size() == 1 && rectPlain.subpaths[0].anchors.size() == 4,
          "svgRectPath: neither rx nor ry specified (both negative) gives a plain 4-anchor rect");

    Path rectOneGiven = svgRectPath(0.0f, 0.0f, 20.0f, 20.0f, 5.0f, -1.0f);
    check(rectOneGiven.subpaths[0].anchors.size() == 8,
          "svgRectPath: rx given, ry not -- rounded, ry takes rx's value");
    // Top edge runs from (rx,0) to (w-rx,0) = (5,0) to (15,0) when ry also
    // resolves to 5.
    check(nearPt(rectOneGiven.subpaths[0].anchors[0].pt, {5.0f, 0.0f}) &&
              nearPt(rectOneGiven.subpaths[0].anchors[1].pt, {15.0f, 0.0f}),
          "svgRectPath: the defaulted ry matches rx in the corner geometry");

    Path rectClamped = svgRectPath(0.0f, 0.0f, 20.0f, 10.0f, 100.0f, 100.0f);
    // rx clamps to w/2=10, ry clamps to h/2=5.
    check(nearPt(rectClamped.subpaths[0].anchors[0].pt, {10.0f, 0.0f}) &&
              nearPt(rectClamped.subpaths[0].anchors[2].pt, {20.0f, 5.0f}),
          "svgRectPath: rx clamps to w/2 and ry clamps to h/2");

    check(svgRectPath(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f).subpaths.empty(),
          "svgRectPath: a non-positive width renders nothing");

    Path ellipse = svgEllipsePath(0.0f, 0.0f, 10.0f, 5.0f);
    check(ellipse.subpaths.size() == 1 && ellipse.subpaths[0].closed &&
              ellipse.subpaths[0].anchors.size() == 4,
          "svgEllipsePath: four anchors, closed");
    check(nearPt(ellipse.subpaths[0].anchors[0].pt, {10.0f, 0.0f}) &&
              nearPt(ellipse.subpaths[0].anchors[2].pt, {-10.0f, 0.0f}),
          "svgEllipsePath: right and left anchors sit at +-rx");
    check(svgEllipsePath(0.0f, 0.0f, 0.0f, 5.0f).subpaths.empty(),
          "svgEllipsePath: a non-positive radius renders nothing");

    Path line = svgLinePath(0.0f, 0.0f, 10.0f, 10.0f);
    check(line.subpaths.size() == 1 && !line.subpaths[0].closed &&
              line.subpaths[0].anchors.size() == 2,
          "svgLinePath: two anchors, open");

    Path poly = svgPolyPath({0.0f, 0.0f, 10.0f, 0.0f, 10.0f, 10.0f}, true);
    check(poly.subpaths.size() == 1 && poly.subpaths[0].closed &&
              poly.subpaths[0].anchors.size() == 3,
          "svgPolyPath: three whole pairs, closed as requested");
    Path polyOdd = svgPolyPath({0.0f, 0.0f, 10.0f, 0.0f, 10.0f}, false);
    check(polyOdd.subpaths[0].anchors.size() == 2,
          "svgPolyPath: a trailing unpaired coordinate is dropped, not crashed on");
    check(svgPolyPath({0.0f, 0.0f}, false).subpaths.empty(),
          "svgPolyPath: fewer than two complete pairs renders nothing");
  }

  return ok;
}

}  // namespace np
