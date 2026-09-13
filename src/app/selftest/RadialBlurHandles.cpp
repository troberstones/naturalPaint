#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>

#include "app/CanvasView.hpp"
#include "app/RadialBlurHandles.hpp"
#include "app/SelfTest.hpp"

namespace np {
namespace {

constexpr float kDeg = 3.14159265358979f / 180.0f;

float dist(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

// Canvas up is -y; positive angles turn toward +x. Written out here rather
// than borrowed, so a flipped convention in the module shows as a wrong value.
Vec2 canvasAt(Vec2 c, float r, float degrees) {
  return Vec2{c.x + r * std::sin(degrees * kDeg), c.y - r * std::cos(degrees * kDeg)};
}

float canvasAngleDegrees(Vec2 c, Vec2 p) { return std::atan2(p.x - c.x, -(p.y - c.y)) / kDeg; }

}  // namespace

bool runRadialBlurHandlesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const float docW = 800.0f, docH = 600.0f;
  const Vec2 docCentre{docW * 0.5f, docH * 0.5f};
  const CanvasView plainView;
  CanvasView busyView;
  busyView.zoom = 2.5f;
  busyView.rotation = 0.7f;
  busyView.mirrorX = true;
  const ViewTransform plain(plainView, docCentre, Vec2{640.0f, 400.0f});
  const ViewTransform busy(busyView, docCentre, Vec2{700.0f, 380.0f});

  std::printf("  -- A. the fixture view zooms, rotates and mirrors --\n");
  {
    const Vec2 a = busy.toScreen(docCentre);
    const Vec2 b = busy.toScreen(Vec2{docCentre.x + 1.0f, docCentre.y});
    check(std::fabs(dist(a, b) - 2.5f) < 1e-3f, "fixture: one texel is 2.5 screen px in the busy view");
    const Vec2 up = busy.toScreen(Vec2{docCentre.x, docCentre.y - 10.0f});
    check(std::fabs(up.x - a.x) > 1.0f, "fixture: canvas up is not screen up in the busy view");
  }

  std::printf("  -- B. shape: where the handles and guide are drawn --\n");
  {
    RadialBlurParams p{RadialBlurMethod::Spin, 210.0f, 330.0f, 0.0f, 8};
    const RadialBlurHandleShape spin0 = radialBlurHandleShape(p, busy);
    check(dist(spin0.center, busy.toScreen(Vec2{210.0f, 330.0f})) < 1e-3f,
          "shape: the centre handle is drawn on the blur centre");
    check(std::fabs(dist(spin0.amountHandle, spin0.center) - kRadialBlurGuidePx) < 0.05f &&
              dist(spin0.amountHandle, spin0.rest) < 1e-3f,
          "shape: Spin 0 puts the amount handle at rest, 72 screen px out");

    p.amount = -64.0f;
    const RadialBlurHandleShape spin = radialBlurHandleShape(p, busy);
    bool onRing = true;
    for (const Vec2& g : spin.guide)
      if (std::fabs(dist(g, spin.center) - kRadialBlurGuidePx) > 0.05f) onRing = false;
    check(onRing, "shape: every point of Spin's arc is 72 screen px from the centre");
    const Vec2 c{p.centerX, p.centerY};
    check(std::fabs(canvasAngleDegrees(c, busy.toCanvas(spin.guide.front())) + 32.0f) < 0.05f &&
              std::fabs(canvasAngleDegrees(c, busy.toCanvas(spin.guide.back())) - 32.0f) < 0.05f,
          "shape: Spin -64 draws its arc from -32 to +32 degrees, the sweep the engine takes");
    check(dist(spin.amountHandle, spin.guide.back()) < 1e-3f,
          "shape: Spin's amount handle is the arc's clockwise end");

    p.method = RadialBlurMethod::Zoom;
    p.amount = 0.5f;
    const RadialBlurHandleShape zoom = radialBlurHandleShape(p, busy);
    check(std::fabs(dist(zoom.guide.front(), zoom.center) - 36.0f) < 0.05f &&
              std::fabs(dist(zoom.guide.back(), zoom.center) - 108.0f) < 0.05f,
          "shape: Zoom 0.5 draws its segment from 0.5 to 1.5 of the 72 px radius");
    check(dist(zoom.amountHandle, zoom.guide.back()) < 1e-3f &&
              std::fabs(dist(zoom.rest, zoom.center) - kRadialBlurGuidePx) < 0.05f,
          "shape: Zoom's amount handle is the segment's outer end; rest is at 72 px");
  }

  std::printf("  -- C. picking --\n");
  {
    const RadialBlurParams p{RadialBlurMethod::Spin, 400.0f, 300.0f, 40.0f, 8};
    const RadialBlurHandleShape s = radialBlurHandleShape(p, plain);
    check(radialBlurHandleAt(s, s.center) == RadialBlurHandle::Center, "pick: the centre");
    check(radialBlurHandleAt(s, Vec2{s.amountHandle.x + 5.0f, s.amountHandle.y + 5.0f}) ==
              RadialBlurHandle::Amount,
          "pick: within the pick radius of the amount handle");
    check(radialBlurHandleAt(s, Vec2{s.center.x + kRadialBlurPickPx + 3.0f, s.center.y}) ==
              RadialBlurHandle::None,
          "pick: just outside the centre's pick radius is nothing");
    RadialBlurHandleShape overlap = s;
    overlap.amountHandle = overlap.center;
    check(radialBlurHandleAt(overlap, overlap.center) == RadialBlurHandle::Center,
          "pick: where both are in reach, the centre wins");
  }

  std::printf("  -- D. dragging the centre (busy view) --\n");
  {
    RadialBlurParams p{RadialBlurMethod::Spin, 210.0f, 330.0f, 20.0f, 8};
    const Vec2 grabbed = radialBlurHandleShape(p, busy).center;
    RadialBlurHandleDrag drag = beginRadialBlurHandleDrag(RadialBlurHandle::Center, p, busy, grabbed);
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(Vec2{123.0f, 456.0f}), docW, docH, &p);
    check(p.centerX == 123.0f && p.centerY == 456.0f,
          "centre: follows the pointer to the texel under it");

    const Vec2 off{5.0f, -4.0f};
    const Vec2 s = radialBlurHandleShape(p, busy).center;
    drag = beginRadialBlurHandleDrag(RadialBlurHandle::Center, p, busy, Vec2{s.x + off.x, s.y + off.y});
    const Vec2 target = busy.toScreen(Vec2{500.0f, 77.0f});
    updateRadialBlurHandleDrag(drag, busy, Vec2{target.x + off.x, target.y + off.y}, docW, docH, &p);
    check(p.centerX == 500.0f && p.centerY == 77.0f,
          "centre: grabbed off-centre, it keeps the grab offset instead of jumping");

    drag = beginRadialBlurHandleDrag(RadialBlurHandle::Center, p, busy, radialBlurHandleShape(p, busy).center);
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(Vec2{-50.0f, 700.0f}), docW, docH, &p);
    check(p.centerX == 0.0f && p.centerY == docH, "centre: clamped to the document");
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(Vec2{10.4f, 20.6f}), docW, docH, &p);
    check(p.centerX == 10.0f && p.centerY == 21.0f, "centre: lands on whole texels");
    check(p.amount == 20.0f && p.method == RadialBlurMethod::Spin,
          "centre: dragging it leaves the amount and method alone");
  }

  std::printf("  -- E. dragging Spin's amount (busy view) --\n");
  {
    bool roundTrip = true;
    for (const float a : {30.0f, -64.0f, 90.0f}) {
      RadialBlurParams p{RadialBlurMethod::Spin, 300.0f, 250.0f, a, 8};
      const Vec2 h = radialBlurHandleShape(p, busy).amountHandle;
      const RadialBlurHandleDrag drag = beginRadialBlurHandleDrag(RadialBlurHandle::Amount, p, busy, h);
      updateRadialBlurHandleDrag(drag, busy, h, docW, docH, &p);
      if (p.amount != a) roundTrip = false;
    }
    check(roundTrip, "spin: releasing the handle where it is drawn keeps 30, -64 and 90");

    const Vec2 c{300.0f, 250.0f};
    RadialBlurParams p{RadialBlurMethod::Spin, c.x, c.y, 10.0f, 8};
    RadialBlurHandleDrag drag = beginRadialBlurHandleDrag(RadialBlurHandle::Amount, p, busy, Vec2{});
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, 150.0f, 20.0f)), docW, docH, &p);
    check(p.amount == 40.0f,
          "spin: the pointer 20 degrees from canvas up sets a 40 degree sweep, at any distance");
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, 40.0f, -25.0f)), docW, docH, &p);
    check(p.amount == 50.0f, "spin: either side of up sets the same magnitude");
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, 90.0f, 170.0f)), docW, docH, &p);
    check(p.amount == kRadialBlurMaxSpinDegrees, "spin: clamped to the dialog's 90 degrees");

    p.amount = -10.0f;
    drag = beginRadialBlurHandleDrag(RadialBlurHandle::Amount, p, busy, Vec2{});
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, 60.0f, 15.0f)), docW, docH, &p);
    check(p.amount == -30.0f, "spin: a negative amount keeps its sign while its size is dragged");
  }

  std::printf("  -- F. dragging Zoom's amount (busy view) --\n");
  {
    bool roundTrip = true;
    for (const float a : {0.25f, -0.6f, 1.0f}) {
      RadialBlurParams p{RadialBlurMethod::Zoom, 300.0f, 250.0f, a, 8};
      const Vec2 h = radialBlurHandleShape(p, busy).amountHandle;
      const RadialBlurHandleDrag drag = beginRadialBlurHandleDrag(RadialBlurHandle::Amount, p, busy, h);
      updateRadialBlurHandleDrag(drag, busy, h, docW, docH, &p);
      if (p.amount != a) roundTrip = false;
    }
    check(roundTrip, "zoom: releasing the handle where it is drawn keeps 0.25, -0.6 and 1.0");

    const Vec2 c{300.0f, 250.0f};
    const float r = kRadialBlurGuidePx / busyView.zoom;  // 72 screen px in canvas texels
    RadialBlurParams p{RadialBlurMethod::Zoom, c.x, c.y, 0.1f, 8};
    const RadialBlurHandleDrag drag = beginRadialBlurHandleDrag(RadialBlurHandle::Amount, p, busy, Vec2{});
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, r * 1.4f, 0.0f)), docW, docH, &p);
    check(p.amount == 0.4f, "zoom: the pointer 1.4 radii up sets 0.4");
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, r * 2.5f, 0.0f)), docW, docH, &p);
    check(p.amount == kRadialBlurMaxZoom, "zoom: clamped to 1.0 past two radii");
    updateRadialBlurHandleDrag(drag, busy, busy.toScreen(canvasAt(c, r * 0.5f, 0.0f)), docW, docH, &p);
    check(p.amount == 0.0f, "zoom: inside the rest radius is 0");
  }

  std::printf("[selftest] radialBlurHandles %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
