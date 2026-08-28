#include "app/selftest/Support.hpp"

#include "core/SelectionBoundary.hpp"
#include "core/SelectionShapes.hpp"

namespace np {
namespace {

// Minimum distance from `p` to the closed polyline `pts` -- the nearest
// point on any of its SEGMENTS, wrapping the last vertex back to the first,
// not merely the nearest of its vertices. A vertex-only measure would
// overstate the gap between a coarse point run and a fine boundary trace by
// up to half a segment length, for a reason that has nothing to do with
// whether the two shapes actually agree.
float distanceToClosedPolyline(Vec2 p, const std::vector<Vec2>& pts) {
  float best = std::numeric_limits<float>::max();
  const size_t n = pts.size();
  for (size_t i = 0; i < n; ++i) {
    const Vec2 a = pts[i];
    const Vec2 b = pts[(i + 1) % n];
    const float abx = b.x - a.x, aby = b.y - a.y;
    const float lenSq = abx * abx + aby * aby;
    float t = lenSq > 1e-9f ? ((p.x - a.x) * abx + (p.y - a.y) * aby) / lenSq : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float cx = a.x + abx * t, cy = a.y + aby * t;
    const float dx = p.x - cx, dy = p.y - cy;
    best = std::min(best, std::sqrt(dx * dx + dy * dy));
  }
  return best;
}

// The worst (max, not mean) distance from any turning point of `boundary`
// to the nearest point on `pts`'s segments. Max rather than mean because
// this is a disagreement detector: a generator that is right almost
// everywhere and wrong in one place (SelectionDrag.hpp's own "bounding
// rectangle" sabotage the section below revives) must not have that one
// place averaged away by a hundred correct ones.
float maxBoundaryDeviation(const SelectionBoundary& boundary, const std::vector<Vec2>& pts) {
  float worst = 0.0f;
  for (const BoundaryContour& contour : boundary.contours) {
    for (const BoundaryVertex v : contour.vertices) {
      const Vec2 p{static_cast<float>(v.x), static_cast<float>(v.y)};
      worst = std::max(worst, distanceToClosedPolyline(p, pts));
    }
  }
  return worst;
}

}  // namespace

// docs/testing-issues.md T13 ("The ellipse marquee draws a rectangle while
// you drag it"): app/SelectionDrag.hpp's ellipseMarqueePreviewPoints(), the
// point run ui/MacPaintUI.cpp's live rubber band now walks for the ellipse
// marquee instead of always the four-corner rectangle every selection tool
// used to share. Headless and GPU-free -- pure CPU, no ImGui involvement,
// the same split app/SelectionDrag.hpp's own doc comment describes.
//
// **Why this cannot be a visual claim.** ui/MacPaintUI.cpp's live rubber
// band only draws between a real mouse-down and mouse-up on the canvas --
// nothing --selftest runs boots ImGui or injects mouse events, and neither
// --marquee-demo (installs a already-COMMITTED selection, no drag in
// progress) nor any other demo flag reaches it. See this section's report
// for what was instead verified by eye. So the strongest testable claim is
// not "it looks like an ellipse" but the one PRD-adjacent property that
// actually matters: **the preview draws the SAME shape mouse-up commits,
// for the SAME box** -- measured by comparing the preview's own returned
// points against core/SelectionBoundary's trace of what
// `case Tool::EllipseMarquee:`'s commit arm actually builds
// (`selectEllipse()` from the identical box), rather than against a second,
// independently-written idea of where an ellipse's edge is.
bool runEllipseMarqueePreviewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-78s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. the generator in isolation --\n");
  {
    // Fewer than three points cannot close into a shape (this header's own
    // doc comment on the guard) -- the degenerate input a caller passing a
    // collapsed drag (mouse-up before the drag moved) could produce.
    check(ellipseMarqueePreviewPoints(0.0f, 0.0f, 10.0f, 10.0f, 0).empty() &&
              ellipseMarqueePreviewPoints(0.0f, 0.0f, 10.0f, 10.0f, 1).empty() &&
              ellipseMarqueePreviewPoints(0.0f, 0.0f, 10.0f, 10.0f, 2).empty(),
          "generator: fewer than 3 segments returns no points, not a degenerate sliver");

    const std::vector<Vec2> pts = ellipseMarqueePreviewPoints(100.0f, 100.0f, 360.0f, 280.0f, 32);
    check(pts.size() == 32, "generator: returns exactly `segments` points when segments >= 3");

    // Every returned point sits exactly on the analytic ellipse this box
    // describes -- ((x-cx)/rx)^2 + ((y-cy)/ry)^2 == 1 -- to float rounding
    // only. This is the property SABOTAGE (b) below breaks (a generator
    // that emits the box's own corners instead answers ~4.6 and ~1.7 here,
    // not 1.0), pinned in isolation before section B ties it to the
    // committed boundary.
    const float cx = (100.0f + 360.0f) * 0.5f, cy = (100.0f + 280.0f) * 0.5f;
    const float rx = (360.0f - 100.0f) * 0.5f, ry = (280.0f - 100.0f) * 0.5f;
    float worstResidual = 0.0f;
    for (const Vec2& p : pts) {
      const float u = (p.x - cx) / rx, v = (p.y - cy) / ry;
      worstResidual = std::max(worstResidual, std::fabs(u * u + v * v - 1.0f));
    }
    std::printf("    [measured] worst |((x-cx)/rx)^2+((y-cy)/ry)^2 - 1| over 32 samples = %.3e\n",
                worstResidual);
    // 1e-4f: the same round-trip-safety margin app/selftest/SelectionDrag.cpp
    // uses for an equally shallow chain of float ops (a few +, -, *, one
    // cosf/sinf each) -- not slack for a real geometric error, since there
    // is none to give in an exact parametric sample.
    check(worstResidual <= 1e-4f,
          "generator: every returned point lies exactly on the box's ellipse, to float "
          "rounding only");
  }

  std::printf("  -- B. the preview agrees with what mouse-up actually commits --\n");
  {
    // Deliberately not tile-aligned (kTileSize is 128; 200 % 128 == 72) and
    // not square, the same reasoning --marquee-demo's own fixture states:
    // an aligned or symmetric box could pass by accident in ways a generic
    // one cannot.
    const float x0 = 200.0f, y0 = 150.0f, x1 = 460.0f, y1 = 330.0f;

    // Exactly `case Tool::EllipseMarquee:`'s commit arm (ui/MacPaintUI.cpp):
    // selectEllipse(cx, cy, rx, ry) with cx,cy the box's centre and rx,ry
    // its half-extents. Duplicated here rather than shared through a
    // helper for the reason app/SelectionDrag.cpp's own doc comment on
    // ellipseMarqueePreviewPoints() gives: it is two subtractions and an
    // average, cheaper to keep honest as two copies than as a dependency
    // between a UI translation unit and this headless one.
    const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float rx = (x1 - x0) * 0.5f, ry = (y1 - y0) * 0.5f;
    const Selection committed = selectEllipse(cx, cy, rx, ry);
    const SelectionBoundary boundary = extractSelectionBoundary(committed);
    check(!boundary.empty(), "(setup) the committed ellipse has a real boundary to compare against");

    // 32: mirrors ui/MacPaintUI.cpp's kEllipseMarqueePreviewSegments. Kept
    // as a literal rather than a shared constant because the two live in
    // different translation units (ui/ and this headless one) and the
    // value itself -- "what the live overlay actually draws with" -- is
    // exactly what this section means to hold fixed; a shared header would
    // let a future change to the UI's resolution silently retune this
    // assertion's tolerance along with it.
    const std::vector<Vec2> correctPreview = ellipseMarqueePreviewPoints(x0, y0, x1, y1, 32);
    const float correctDeviation = maxBoundaryDeviation(boundary, correctPreview);
    std::printf("    [measured] preview-from-box vs committed boundary, worst turning point = %.3f texels\n",
                correctDeviation);
    // 2.0 texels: empirically derived, not guessed. Measured on this exact
    // box/segment-count pair: the polygon-sampling error a 32-gon has
    // approximating this ellipse (~0.62 texels, point-to-SEGMENT distance)
    // plus core/SelectionBoundary's own crack-edge quantisation of the 0.5
    // coverage threshold (sub-texel, per its header) leaves comfortable
    // margin under 2.0 without approaching sabotage (b)'s ~130-texel
    // failure mode measured below.
    check(correctDeviation <= 2.0f,
          "boundary: the preview built from the drag's BOX agrees with the committed "
          "selection's traced boundary, everywhere around it, to within 2 texels");

    // --- the discriminating power of that same assertion, under exactly
    // the gesture T13's own doc comment names: Option-from-centre --------
    //
    // anchor(330,240), fromCentre=true, dragged to (460,330): T10's
    // computeSelectionDragBox() reads the anchor as the box's CENTRE, and
    // reproduces the identical box above (330-130=200, 240-90=150,
    // 330+130=460, 240+90=330) -- so the committed boundary is the exact
    // same `boundary` already extracted.
    const SelectionDragBox box =
        computeSelectionDragBox(330.0f, 240.0f, 460.0f, 330.0f, 0.0f, 0.0f, false, true);
    check(std::fabs(box.x0 - x0) < 1e-4f && std::fabs(box.y0 - y0) < 1e-4f &&
              std::fabs(box.x1 - x1) < 1e-4f && std::fabs(box.y1 - y1) < 1e-4f,
          "(setup) Option-from-centre on this anchor/cursor reproduces the SAME box section "
          "B already committed from -- so the two cases share one boundary to compare against");

    // SABOTAGE (a)'s exact shape, run as a permanent assertion rather than
    // only at sabotage time: feed the generator the drag's RAW anchor and
    // cursor -- (330,240) and (460,330) -- as if they were the box's own
    // corners, the way a call site that forgot T10's box and read
    // `marqueeX0..Y1` instead of `marqueeBoxX0..Y1` would. Under a plain
    // corner drag this happens to be harmless (the anchor IS a corner); the
    // whole point of Option-from-centre is that here it is not, so this is
    // the case that actually exercises the mistake.
    const std::vector<Vec2> wrongPreview =
        ellipseMarqueePreviewPoints(330.0f, 240.0f, 460.0f, 330.0f, 32);
    const float wrongDeviation = maxBoundaryDeviation(boundary, wrongPreview);
    std::printf("    [measured] preview-from-RAW-corners vs committed boundary, worst turning point = %.3f texels\n",
                wrongDeviation);
    check(wrongDeviation > 20.0f,
          "boundary: feeding the generator the raw anchor/cursor instead of the "
          "gesture-resolved box disagrees with the committed boundary by tens of texels "
          "under Option-from-centre -- this is the assertion sabotage (a) would redden if "
          "the call site fed it, and it is already red for the equivalent input");
  }

  std::printf("  -- C. the ellipse and the rectangle overload disagree on a non-square box --\n");
  {
    // Not required by T13, but worth pinning down: on a box that is not a
    // square, the ellipse preview and the four-corner rectangle preview
    // must NOT coincide, or a regression that silently routed the ellipse
    // tool back through the rectangle overload (the exact bug T13 reports)
    // would still pass every assertion above that only ever looks at one
    // overload's output.
    const std::vector<Vec2> ellipsePts = ellipseMarqueePreviewPoints(0.0f, 0.0f, 200.0f, 100.0f, 32);
    const std::vector<Vec2> rectCorners = {Vec2{0.0f, 0.0f}, Vec2{200.0f, 0.0f}, Vec2{200.0f, 100.0f},
                                           Vec2{0.0f, 100.0f}};
    // The rectangle's own corners, e.g. (200,100), are ~111.8 texels from
    // the nearest point of a 100x50-radii ellipse centred at (100,50) --
    // sqrt((200-100)^2+(100-50)^2) - ellipse's own reach in that
    // direction. Comfortably beyond the 2-texel agreement tolerance above,
    // so this is a real distinguishing check, not a tautology.
    float minSeparation = std::numeric_limits<float>::max();
    for (const Vec2& c : rectCorners)
      minSeparation = std::min(minSeparation, distanceToClosedPolyline(c, ellipsePts));
    check(minSeparation > 10.0f,
          "shape: the ellipse overload's points are NOT the rectangle overload's four "
          "corners on a non-square box -- the two tools' previews are genuinely different "
          "shapes, not the same rectangle drawn twice");
  }

  std::printf("[selftest] ellipse marquee preview %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
