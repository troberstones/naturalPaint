#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/StrokeSession.hpp"

namespace np {

// Photoshop's Build-up (`BrushModel::airbrush`, `Rpt `): holding the pen still
// keeps laying dabs at the nib, so paint piles up where it waits.
bool runAirbrushBuildUpTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr uint64_t kSecond = 1'000'000'000ull;
  constexpr int32_t kPen = 64;

  BrushTip tip;
  tip.radius = 10.0f;
  tip.hardness = 1.0f;
  tip.flow = 0.1f;
  tip.opacity = 1.0f;
  tip.linearRgb = {0.6f, 0.3f, 0.1f};
  tip.spacing = 0.25f;

  BrushModel base;
  base.tip.diameterPx = 20.0f;
  base.tip.roundness = 1.0f;
  base.scatter.count = 1;

  struct Held {
    size_t atFirstTick = 0;
    size_t afterHold = 0;
    float alpha = 0.0f;
  };
  // One point, then `ticks` calls `step` apart, the first at one second.
  auto hold = [&](bool airbrush, bool placePen, int ticks, uint64_t step) {
    OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "airbrush");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("r")));
    BrushModel model = base;
    model.airbrush = airbrush;
    StrokeSession s;
    std::string err;
    Held h;
    if (!s.begin(od, 1, tip, Tool::Brush, &err, &model, DynamicInputs{})) return h;
    if (placePen) s.addPoint(static_cast<float>(kPen), static_cast<float>(kPen));
    s.airbrushTick(kSecond);
    h.atFirstTick = s.dabCount();
    for (int i = 1; i <= ticks; ++i) s.airbrushTick(kSecond + step * static_cast<uint64_t>(i));
    h.afterHold = s.dabCount();
    const PixelCoord pc{kPen, kPen};
    if (const Tile* t = od.document.layers[1].rgbTiles->find(tileCoordAt(pc)))
      h.alpha = t->readPixel(tileLocalOffset(pc))[3];
    s.end();
    return h;
  };

  // Half a second at 60 frames a second.
  const Held on = hold(true, true, 30, kSecond / 60);
  const Held off = hold(false, true, 30, kSecond / 60);
  std::printf("    [measured] held 0.5 s: Build-up on %zu dabs, alpha %.3f; off %zu dabs, alpha %.3f\n",
              on.afterHold, static_cast<double>(on.alpha), off.afterHold,
              static_cast<double>(off.alpha));
  check(on.atFirstTick == 0, "airbrush: the first tick only starts the clock");
  check(on.afterHold >= 14 && on.afterHold <= 15,
        "airbrush: half a second held lays Build-up's rate worth of dabs at the pen");
  check(on.alpha > 0.5f, "airbrush: and they pile paint up where the pen waits");
  check(off.afterHold == 0 && off.alpha == 0.0f,
        "airbrush: a brush without Build-up lays nothing while the pen is held");

  const Held stalled = hold(true, true, 1, 10 * kSecond);
  std::printf("    [measured] one tick after a 10 s stall: %zu dabs\n", stalled.afterHold);
  check(stalled.afterHold > 0 && stalled.afterHold <= kAirbrushMaxDabsPerTick,
        "airbrush: a stalled frame lays a few dabs, not the whole stall's worth");

  const Held noPen = hold(true, false, 30, kSecond / 60);
  check(noPen.afterHold == 0, "airbrush: nothing is laid before the stroke has a position");

  // A held dab has no step of its own to take a heading from. Straight down,
  // so both headings compared below are the identical float.
  struct Angles {
    float afterMove = 0.0f;
    float afterHold = 0.0f;
    size_t movedDabs = 0;
    size_t heldDabs = 0;
  };
  auto angles = [&](VarianceControl control, bool dwellFirst) {
    OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "airbrush-angle");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(), makeRgbLayer("r")));
    BrushModel model = base;
    model.airbrush = true;
    model.shape.enabled = true;
    model.shape.angle.control = control;
    StrokeSession s;
    std::string err;
    Angles a;
    if (!s.begin(od, 1, tip, Tool::Brush, &err, &model, DynamicInputs{})) return a;
    uint64_t t = kSecond;
    s.addPoint(40.0f, 20.0f);
    if (dwellFirst) {
      s.airbrushTick(t);
      for (int i = 0; i < 15; ++i) s.airbrushTick(t += kSecond / 60);
    }
    for (int i = 1; i <= 6; ++i) s.addPoint(40.0f, 20.0f + 12.0f * static_cast<float>(i));
    a.afterMove = s.lastDabAngle();
    a.movedDabs = s.dabCount();
    s.airbrushTick(t += kSecond / 60);
    for (int i = 0; i < 15; ++i) s.airbrushTick(t += kSecond / 60);
    a.afterHold = s.lastDabAngle();
    a.heldDabs = s.dabCount() - a.movedDabs;
    s.end();
    return a;
  };

  const Angles byDirection = angles(VarianceControl::Direction, false);
  std::printf("    [measured] Angle on Direction: %.2f moving, %.2f after %zu held dabs\n",
              static_cast<double>(byDirection.afterMove), static_cast<double>(byDirection.afterHold),
              byDirection.heldDabs);
  check(byDirection.heldDabs > 0 && byDirection.afterHold == byDirection.afterMove,
        "airbrush: a held dab keeps the heading of the last dab that moved");

  const Angles plain = angles(VarianceControl::InitialDirection, false);
  const Angles dwelt = angles(VarianceControl::InitialDirection, true);
  std::printf("    [measured] Angle on Initial Direction: %.2f, %.2f after dwelling at the start\n",
              static_cast<double>(plain.afterMove), static_cast<double>(dwelt.afterMove));
  check(dwelt.movedDabs > plain.movedDabs && dwelt.afterMove == plain.afterMove,
        "airbrush: dwelling before moving does not latch the initial direction");

  std::printf("[selftest] airbrush build-up %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
