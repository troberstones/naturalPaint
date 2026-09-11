#include "app/selftest/Support.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "app/PenAxes.hpp"
#include "app/PointerQueue.hpp"
#include "brush/StrokePath.hpp"

namespace np {
namespace {

// ---------------------------------------------------------------------------
// A model of SDL 3.2.24's pen core (events/SDL_pen.c) and of ImGui's input
// sequence numbers, feeding a real `PointerQueue` exactly the `PointerEvent`s
// main.cpp's `pointerEventFromSdl()` would build from what SDL pushes.
//
// What is modelled, because the queue's decisions depend on it:
//   * SDL's de-duplication: a position event only when the position changed
//     (SDL_pen.c:481), an axis event only when that axis's value changed
//     (:425), a touch event only when contact changed (:338-344);
//   * PEN_DOWN/PEN_UP carry SDL's CURRENT pen position, i.e. the previous
//     report's on a backend that sends Touch before Motion (:335-336);
//   * the pen's synthesised mouse events (SDL_PEN_MOUSEID) pushed right behind
//     PEN_DOWN (motion + left down), PEN_UP (left up) and every in-contact
//     PEN_MOTION (motion) (:375-384, :494-499);
//   * timestamps: a "stamped" backend gives every event of one report the
//     report's timestamp; an unstamped one passes 0 and `SDL_PushEvent()`
//     stamps each event with its own `SDL_GetTicksNS()` (SDL_events.c:1717),
//     modelled as a strictly increasing clock;
//   * ImGui sequence numbers: every mouse event (real or synthesised) becomes
//     one ImGui input event and so takes one number; pen events take none.
//     `PointerEvent::uiSeq` is the number BEFORE the event, as main.cpp reads
//     it. (ImGui's own duplicate filtering is not modelled: it can only skip
//     numbers, which no rule here depends on.)
// Pen buttons (SDL_EVENT_PEN_BUTTON_*) are not modelled: main.cpp does not
// translate them and nothing in the queue reads them.
struct FakeSdlPen {
  PointerQueue& q;
  bool stamped = true;
  uint64_t reportTs = 1'000'000'000;  // 1 s, then 5 ms per report (a 200 Hz tablet)
  uint64_t clock = 0;
  uint32_t uiSeq = 1;  // ImGui's InputEventsNextEventId starts at 1
  float x = 0.0f, y = 0.0f;
  bool touching = false;
  float axes[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // PointerAxis order; SDL_zero'd
  uint32_t lastSynthUpSeq = 0;

  void beginReport() {
    reportTs += 5'000'000;
    clock = reportTs;
  }
  uint64_t ts() { return stamped ? reportTs : ++clock; }
  void mouse(PointerEventKind kind, bool leftHeldOrLeft) {
    PointerEvent e{kind, ts(), uiSeq, x, y, PointerAxis::Other, 0.0f, leftHeldOrLeft, true};
    if (kind == PointerEventKind::MouseButtonUp) lastSynthUpSeq = uiSeq;
    ++uiSeq;
    q.push(e);
  }
  void touch(bool down) {
    if (down == touching) return;
    touching = down;
    q.push(PointerEvent{down ? PointerEventKind::PenDown : PointerEventKind::PenUp, ts(), uiSeq, x, y});
    if (down) {
      mouse(PointerEventKind::MouseMotion, false);
      mouse(PointerEventKind::MouseButtonDown, true);
    } else {
      mouse(PointerEventKind::MouseButtonUp, true);
    }
  }
  void motion(float nx, float ny) {
    if (nx == x && ny == y) return;
    x = nx;
    y = ny;
    q.push(PointerEvent{PointerEventKind::PenMotion, ts(), uiSeq, x, y});
    mouse(PointerEventKind::MouseMotion, touching);
  }
  void axis(PointerAxis a, float v) {
    const size_t i = static_cast<size_t>(a);
    if (axes[i] == v) return;
    axes[i] = v;
    q.push(PointerEvent{PointerEventKind::PenAxis, ts(), uiSeq, x, y, a, v});
  }
  // Every axis varies with the report, so a stale snapshot of ANY of them is
  // visible: x-tilt 20 + 10p and rotation 100p degrees, rounded to whole
  // degrees so the expected values below are exact integers.
  void allAxes(float p) {
    axis(PointerAxis::Pressure, p);
    axis(PointerAxis::Rotation, 10.0f * std::round(p * 10.0f));
    axis(PointerAxis::TiltX, 20.0f + std::round(p * 10.0f));
    axis(PointerAxis::TiltY, -10.0f);
  }
};

enum class Backend { MacOS, Wayland, Windows, X11 };

const char* backendName(Backend b) {
  switch (b) {
    case Backend::MacOS: return "macOS";
    case Backend::Wayland: return "Wayland";
    case Backend::Windows: return "Windows";
    case Backend::X11: return "X11";
  }
  return "?";
}

// One device report at (x, y), in contact or not, at pressure p -- in the
// ORDER that backend's SDL driver sends it (app/PointerQueue.hpp section 2
// has the file:line for each).
void report(FakeSdlPen& s, Backend b, float px, float py, bool contact, float p) {
  s.beginReport();
  switch (b) {
    case Backend::MacOS:  // Touch, Motion, (buttons), axes
      s.touch(contact);
      s.motion(px, py);
      s.allAxes(p);
      break;
    case Backend::Wayland:  // contact: Motion, Touch; lift: Touch, Motion; then axes
      if (contact && !s.touching) {
        s.motion(px, py);
        s.touch(true);
      } else {
        s.touch(contact);
        s.motion(px, py);
      }
      s.allAxes(p);
      break;
    case Backend::Windows:  // Touch(up)?, Motion, (buttons), axes, Touch(down)?
      if (!contact) s.touch(false);
      s.motion(px, py);
      s.allAxes(p);
      if (contact) s.touch(true);
      break;
    case Backend::X11:  // XI_Motion: Motion, axes; then XI_ButtonPress/Release: Touch alone
      s.motion(px, py);
      s.allAxes(p);
      if (contact != s.touching) {
        s.beginReport();
        s.touch(contact);
      }
      break;
  }
}

// A short earlier stroke and its lift, then hover to (hx, 100): what leaves
// the latest-axis pressure at the lift's 0 -- the stale value finding 1's
// opening samples carried.
void priorStrokeAndHover(FakeSdlPen& s, Backend b, float hx) {
  report(s, b, 40.0f, 40.0f, true, 0.5f);
  report(s, b, 44.0f, 40.0f, true, 0.5f);
  report(s, b, 44.0f, 40.0f, false, 0.0f);
  for (float hover = hx - 8.0f; hover <= hx; hover += 2.0f) report(s, b, hover, 100.0f, false, 0.0f);
}

// Drains exactly what one frame of the canvas would: ImGui has processed
// everything numbered below `bound`, the canvas claims (if it is not already
// painting `gesture`) and takes.
std::vector<PointerSample> frameTake(PointerQueue& q, uint32_t bound, uint64_t& gesture) {
  q.beginFrame(bound);
  if (gesture == 0) gesture = q.claimGesture();
  std::vector<PointerSample> out = q.takeForStroke(gesture);
  q.endFrame();
  return out;
}

PointerEvent mouseEv(PointerEventKind k, float x, uint32_t seq, bool left = true,
                     bool penSynth = false) {
  return PointerEvent{k, 0, seq, x, 100.0f, PointerAxis::Other, 0.0f, left, penSynth};
}

}  // namespace

bool runPointerQueueTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] pointer queue: axis association, gestures, barrel wrap\n");

  // ==========================================================================
  // 1. Axes arrive after the position: each backend's order, replayed. Pen
  //    down at (100,100) pressure 0.6, then motion reports at 0.6/0.7/0.8.
  //    SDL sends no pressure event for the second 0.6 (unchanged value), so
  //    the queued pressures must be exactly 0.6, 0.6, 0.7, 0.8 -- the down
  //    sample, then one per motion. Exact float equality: every value is an
  //    event's own number, stored and patched, never computed.
  // ==========================================================================
  {
    const float expected[4] = {0.6f, 0.6f, 0.7f, 0.8f};
    for (Backend b : {Backend::MacOS, Backend::Wayland, Backend::Windows, Backend::X11}) {
      PointerQueue q;
      FakeSdlPen s{q};
      s.stamped = b != Backend::X11;
      priorStrokeAndHover(s, b, 100.0f);
      uint64_t g = 0;
      frameTake(q, s.uiSeq, g);  // the prior stroke is over; nothing claims it
      g = 0;
      report(s, b, 100.0f, 100.0f, true, 0.6f);
      report(s, b, 102.0f, 100.0f, true, 0.6f);
      report(s, b, 104.0f, 100.0f, true, 0.7f);
      report(s, b, 106.0f, 100.0f, true, 0.8f);
      // The lift arrives in the same poll -- its pressure 0 must patch nothing.
      report(s, b, 106.0f, 100.0f, false, 0.0f);
      // ImGui processes up to (not including) the synthesised release.
      const std::vector<PointerSample> got = frameTake(q, s.lastSynthUpSeq, g);
      char line[160];
      std::printf("  [measured] %s: %zu samples, pressures", backendName(b), got.size());
      for (const PointerSample& ps : got) std::printf(" %.3f", static_cast<double>(ps.pressure));
      std::printf("\n");
      bool exact = got.size() == 4;
      for (size_t i = 0; exact && i < 4; ++i) exact = got[i].pressure == expected[i];
      std::snprintf(line, sizeof(line),
                    "1. %s order: queued pressures are exactly 0.6, 0.6, 0.7, 0.8", backendName(b));
      check(exact, line);
      std::snprintf(line, sizeof(line),
                    "1. %s order: the opening sample carries 0.6, not the lift's stale 0",
                    backendName(b));
      check(!got.empty() && got.front().pressure == 0.6f, line);
      // Contact at p = 0.6: x-tilt 26, rotation 60 (the lift left 20 and 0).
      std::snprintf(line, sizeof(line),
                    "1. %s order: the opening sample's tilt/rotation are its own (26, 60 deg)",
                    backendName(b));
      check(!got.empty() && got.front().tiltXDeg == 26.0f && got.front().tiltYDeg == -10.0f &&
                got.front().rotationDeg == 60.0f,
            line);
    }

    // macOS's contact that also MOVED: Touch queues the down at the OLD
    // position (SDL_pen.c:335-336), Motion the new one, both before any
    // axis, all under one timestamp -- rule (a) must patch BOTH.
    PointerQueue q;
    FakeSdlPen s{q};
    priorStrokeAndHover(s, Backend::MacOS, 98.0f);
    uint64_t g = 0;
    frameTake(q, s.uiSeq, g);
    g = 0;
    report(s, Backend::MacOS, 100.0f, 100.0f, true, 0.6f);
    const std::vector<PointerSample> got = frameTake(q, s.uiSeq, g);
    std::printf("  [measured] macOS moving contact: %zu samples at x", got.size());
    for (const PointerSample& ps : got)
      std::printf(" %.0f (p %.3f)", static_cast<double>(ps.x), static_cast<double>(ps.pressure));
    std::printf("\n");
    check(got.size() == 2 && got[0].x == 98.0f && got[1].x == 100.0f,
          "1. macOS moving contact: down at the old position, motion at the new");
    check(got.size() == 2 && got[0].pressure == 0.6f && got[1].pressure == 0.6f,
          "1. macOS moving contact: rule (a) patches BOTH samples of the report to 0.6");
  }

  // ==========================================================================
  // 2. Press-and-hold dot: down at 0.6, no motion, up. SDL sends no motion for
  //    an unchanged position, so the down sample is the ONLY one -- it must
  //    carry 0.6. Then: pressure-only reports while held (0.65, 0.7) belong to
  //    later reports and must NOT rewrite it.
  // ==========================================================================
  {
    for (Backend b : {Backend::MacOS, Backend::X11}) {
      PointerQueue q;
      FakeSdlPen s{q};
      s.stamped = b != Backend::X11;
      priorStrokeAndHover(s, b, 100.0f);
      uint64_t g = 0;
      frameTake(q, s.uiSeq, g);
      g = 0;
      report(s, b, 100.0f, 100.0f, true, 0.6f);
      const std::vector<PointerSample> dot = frameTake(q, s.uiSeq, g);
      char line[160];
      std::printf("  [measured] %s dot: %zu sample(s), pressure %.3f\n", backendName(b),
                  dot.size(), dot.empty() ? -1.0 : static_cast<double>(dot.front().pressure));
      std::snprintf(line, sizeof(line), "2. %s press-and-hold dot: one sample, carrying 0.6",
                    backendName(b));
      check(dot.size() == 1 && dot.front().pressure == 0.6f, line);

      // Same position, pressure only, the sample still QUEUED this time.
      PointerQueue q2;
      FakeSdlPen s2{q2};
      s2.stamped = b != Backend::X11;
      priorStrokeAndHover(s2, b, 100.0f);
      uint64_t g2 = 0;
      frameTake(q2, s2.uiSeq, g2);
      g2 = 0;
      report(s2, b, 100.0f, 100.0f, true, 0.6f);
      report(s2, b, 100.0f, 100.0f, true, 0.65f);
      report(s2, b, 100.0f, 100.0f, true, 0.7f);
      const std::vector<PointerSample> held = frameTake(q2, s2.uiSeq, g2);
      std::snprintf(line, sizeof(line),
                    "2. %s held dot: later pressure-only reports do not rewrite the sample",
                    backendName(b));
      check(held.size() == 1 && held.front().pressure == 0.6f, line);
    }
  }

  // ==========================================================================
  // 3. Scroll then click. ImGui 1.92.9b's trickling (measured headless:
  //    /private/tmp/np-w1-review/imgui/trickle.cpp, case A) processes a wheel
  //    event and then stops before a button change in the same frame. So the
  //    click's frame shows the canvas NOT down, and the next frame shows it
  //    down -- the stroke that begins then must receive the click's samples.
  // ==========================================================================
  {
    PointerQueue q;
    using K = PointerEventKind;
    // Frame N's poll: wheel (ImGui #1), press at 100 (#2), drag to 101 (#3).
    q.push(mouseEv(K::MouseButtonDown, 100.0f, 2));
    q.push(mouseEv(K::MouseMotion, 101.0f, 3));
    q.beginFrame(2);  // the wheel only
    check(q.claimGesture() == 0, "3. scroll-then-click: nothing claimable while ImGui holds the press");
    q.endFrame();
    q.beginFrame(4);  // frame N+1: the press and the motion
    const uint64_t g = q.claimGesture();
    const std::vector<PointerSample> got = q.takeForStroke(g);
    q.endFrame();
    std::printf("  [measured] scroll-then-click: stroke received %zu sample(s)\n", got.size());
    check(got.size() == 2 && got[0].x == 100.0f && got[1].x == 101.0f,
          "3. scroll-then-click: the stroke beginning next frame receives the click's samples");

    // The quick click: press AND release in frame N behind the wheel. ImGui
    // shows the press on N+1 and the release on N+2; the gesture has ENDED in
    // the queue by the time the canvas first sees it down, and must still be
    // claimed.
    PointerQueue q2;
    q2.push(mouseEv(K::MouseButtonDown, 100.0f, 2));
    q2.push(mouseEv(K::MouseButtonUp, 100.0f, 3));
    q2.beginFrame(2);
    q2.endFrame();
    q2.beginFrame(3);
    const uint64_t g2 = q2.claimGesture();
    const std::vector<PointerSample> quick = q2.takeForStroke(g2);
    q2.endFrame();
    q2.beginFrame(4);
    q2.endFrame();
    check(quick.size() == 1 && quick[0].x == 100.0f,
          "3. quick click behind a wheel event: claimed on the frame ImGui shows it down");
    check(q2.size() == 0 && q2.gestureCount() == 0,
          "3. quick click: nothing left once ImGui has shown the release");
  }

  // ==========================================================================
  // 4. Lift and re-touch in one frame (trickle.cpp case B). The release
  //    frame's poll holds [g1 s@210, g1 up, g2 down@300, g2 s@310]; ImGui
  //    processes pos 210 and the release, then stops before pos 300 (a button
  //    already changed this frame). Stroke 1 must get ONLY 210; stroke 2
  //    begins the next frame and gets 300 first.
  // ==========================================================================
  {
    PointerQueue q;
    using K = PointerEventKind;
    q.push(mouseEv(K::MouseButtonDown, 200.0f, 1));
    uint64_t g1 = 0;
    const std::vector<PointerSample> open1 = frameTake(q, 2, g1);
    q.push(mouseEv(K::MouseMotion, 210.0f, 2));
    q.push(mouseEv(K::MouseButtonUp, 210.0f, 3));
    q.push(mouseEv(K::MouseMotion, 300.0f, 4, /*left held=*/false));  // hover: never queued
    q.push(mouseEv(K::MouseButtonDown, 300.0f, 5));
    q.push(mouseEv(K::MouseMotion, 310.0f, 6));
    q.beginFrame(4);  // pos 210, release; pos 300 held back
    const std::vector<PointerSample> tail1 = q.takeForStroke(g1);  // the pen-up branch
    q.endFrame();
    q.beginFrame(6);  // pos 300, press; pos 310 held back
    const uint64_t g2 = q.claimGesture();
    const std::vector<PointerSample> open2 = q.takeForStroke(g2);
    q.endFrame();
    std::printf("  [measured] lift-and-retouch: stroke 1 tail %zu sample(s)", tail1.size());
    for (const PointerSample& ps : tail1) std::printf(" x=%.0f", static_cast<double>(ps.x));
    std::printf("; stroke 2 opening %zu sample(s)", open2.size());
    for (const PointerSample& ps : open2) std::printf(" x=%.0f", static_cast<double>(ps.x));
    std::printf("\n");
    check(open1.size() == 1 && g1 != 0, "4. setup: stroke 1 opens on its own press");
    check(tail1.size() == 1 && tail1[0].x == 210.0f,
          "4. lift-and-retouch: stroke 1 receives only its own g1 sample (no bridge)");
    check(g2 != 0 && g2 != g1, "4. lift-and-retouch: stroke 2 claims the NEW gesture");
    check(!open2.empty() && open2[0].x == 300.0f,
          "4. lift-and-retouch: stroke 2 receives the g2 down sample at 300 first");
    check(open2.size() == 2 && open2[1].x == 310.0f,
          "4. lift-and-retouch: ...then 310, nothing of g1");
  }

  // ==========================================================================
  // 5. A gesture the canvas never began a stroke for is never delivered to a
  //    later stroke -- including when ImGui is still trickling through it at
  //    the moment the next press arrives.
  // ==========================================================================
  {
    PointerQueue q;
    using K = PointerEventKind;
    // A click-drag on a panel, all in one poll, then a canvas press in the
    // SAME poll: [panel down #1, panel drag #2, panel up #3, canvas down #4,
    // canvas drag #5].
    q.push(mouseEv(K::MouseButtonDown, 10.0f, 1));
    q.push(mouseEv(K::MouseMotion, 11.0f, 2));
    q.push(mouseEv(K::MouseButtonUp, 11.0f, 3));
    q.push(mouseEv(K::MouseButtonDown, 500.0f, 4));
    q.push(mouseEv(K::MouseMotion, 501.0f, 5));
    q.beginFrame(2);  // panel press shown (canvas not hovered: no claim)
    q.endFrame();
    q.beginFrame(3);  // drag
    q.endFrame();
    q.beginFrame(4);  // panel release shown; canvas press still held back
    q.endFrame();
    q.beginFrame(6);  // canvas press shown: a stroke begins
    const uint64_t g = q.claimGesture();
    const std::vector<PointerSample> got = q.takeForStroke(g);
    q.endFrame();
    bool anyPanel = false;
    for (const PointerSample& ps : got) anyPanel = anyPanel || ps.x < 100.0f;
    std::printf("  [measured] unclaimed panel gesture: canvas stroke received %zu sample(s)\n",
                got.size());
    check(!anyPanel, "5. unclaimed panel gesture: never delivered to the later canvas stroke");
    check(got.size() == 2 && got[0].x == 500.0f,
          "5. ...and the canvas stroke still gets its own press, held while ImGui caught up");
    check(q.size() == 0, "5. nothing left queued afterwards");

    // A press on the canvas while panning (the canvas sees `down` but does
    // not begin), then the pan key released mid-drag: the stroke that begins
    // then must not receive the pan-period samples -- they were offered, in
    // window space, under a view that has since moved.
    PointerQueue qp;
    qp.push(mouseEv(K::MouseButtonDown, 50.0f, 1));
    qp.push(mouseEv(K::MouseMotion, 60.0f, 2));
    qp.beginFrame(3);
    qp.endFrame();  // panning: no claim
    qp.push(mouseEv(K::MouseMotion, 70.0f, 3));
    qp.beginFrame(4);
    const uint64_t gp = qp.claimGesture();  // pan released: the stroke begins now
    const std::vector<PointerSample> pan = qp.takeForStroke(gp);
    qp.endFrame();
    check(pan.size() == 1 && pan[0].x == 70.0f,
          "5. pan then paint in one drag: the stroke gets only post-pan samples");
  }

  // ==========================================================================
  // 6. The behaviours the wave-1 review found correct, pinned.
  // ==========================================================================
  {
    using K = PointerEventKind;
    PointerQueue q;
    q.push(mouseEv(K::MouseMotion, 1.0f, 1, /*left=*/true, /*penSynth=*/true));
    q.push(mouseEv(K::MouseButtonDown, 1.0f, 2, true, true));
    q.push(mouseEv(K::MouseMotion, 2.0f, 3, true, true));
    check(q.size() == 0, "6. pen-synthesised mouse motion and button-down queue nothing");
    q.push(mouseEv(K::MouseButtonUp, 2.0f, 4, true, true));
    check(q.size() == 0, "6. pen-synthesised button-up queues nothing");
    // The same duplicates while a REAL mouse gesture is open. Without this the
    // motion filter is untestable: the check above passes on the open-gesture
    // gate alone (a pen's synthesised press opens no mouse gesture), which is
    // how a sabotage deleting the filter first stayed green.
    PointerQueue m;
    m.push(mouseEv(K::MouseButtonDown, 1.0f, 1));
    m.push(mouseEv(K::MouseMotion, 2.0f, 2, true, /*penSynth=*/true));
    m.push(mouseEv(K::MouseButtonDown, 3.0f, 3, true, true));
    m.push(mouseEv(K::MouseMotion, 4.0f, 4, true, true));
    check(m.size() == 1, "6. pen-synthesised motion/press never join an open mouse gesture");

    PointerQueue t;  // touch-generated mouse: which == SDL_TOUCH_MOUSEID, not the pen's
    t.push(mouseEv(K::MouseButtonDown, 5.0f, 1));
    t.push(mouseEv(K::MouseMotion, 6.0f, 2));
    check(t.size() == 2, "6. touch-generated mouse events are kept (press + drag)");
    t.push(mouseEv(K::MouseButtonUp, 6.0f, 3));
    check(t.size() == 2, "6. mouse button-up is never queued");
    t.push(mouseEv(K::MouseButtonDown, 7.0f, 4, /*left=*/false));
    check(t.size() == 2, "6. a non-left button press is never queued");

    PointerQueue h;
    h.push(mouseEv(K::MouseMotion, 8.0f, 1, /*left held=*/false));
    h.push(PointerEvent{K::PenMotion, 1, 1, 9.0f, 9.0f});
    h.push(PointerEvent{K::PenAxis, 1, 1, 9.0f, 9.0f, PointerAxis::Pressure, 0.3f});
    check(h.size() == 0, "6. hover (mouse without left held, pen without contact) never queued");
    h.push(PointerEvent{K::PenDown, 2, 1, 9.0f, 9.0f});
    h.push(PointerEvent{K::PenUp, 3, 1, 9.0f, 9.0f});
    check(h.size() == 1, "6. pen: the down queues one sample, the up none");
    check(h.latestAxes().pressure == 0.3f,
          "6. pen: hover axes still update the latest-axis state the down snapshots");
  }

  // ==========================================================================
  // 7. Barrel rotation through its +-180 degree seam. The review's probe:
  //    rotations 172, 176, 179.5, -176.5, -172.5, -168.5 deg, 3 px apart, dab
  //    spacing 0.6 px. Between 179.5 (barrel 0.99861) and -176.5 (0.00972)
  //    the pen turned 4 degrees; every dab there must lie on that short arc.
  // ==========================================================================
  {
    const float rot[] = {172.0f, 176.0f, 179.5f, -176.5f, -172.5f, -168.5f};
    StrokePath path;
    std::vector<StrokeDab> dabs;
    for (int i = 0; i < 6; ++i) {
      StrokeSample s;
      s.pos = Vec2{100.0f + 3.0f * static_cast<float>(i), 50.0f};
      s.barrel = penBarrelNormalised(rot[i]);
      s.azimuth = s.barrel;  // same seam, same rule
      path.addPoint(s, 0.6f, dabs);
    }
    path.flush(0.6f, dabs);
    const float a = penBarrelNormalised(179.5f);
    const float b = penBarrelNormalised(-176.5f);
    // The arc's own endpoints, widened by 1e-6: `a + d*u` and its wrap by
    // 1.0 are three float operations on values <= ~1, each exact to within
    // half an ulp of 1.0 (6e-8), so 1e-6 is over five times the worst
    // accumulated rounding and still ~1e4 times smaller than the arc itself.
    const float tol = 1e-6f;
    size_t between = 0;
    bool onArc = true, azimuthOnArc = true, inRange = true;
    float worst = 0.0f;
    for (const StrokeDab& d : dabs) {
      inRange = inRange && d.barrel >= 0.0f && d.barrel <= 1.0f;
      if (d.pos.x <= 106.0f || d.pos.x >= 109.0f) continue;
      ++between;
      const bool on = d.barrel >= a - tol || d.barrel <= b + tol;
      if (!on) worst = std::max(worst, std::min(std::fabs(d.barrel - a), std::fabs(d.barrel - b)));
      onArc = onArc && on;
      azimuthOnArc = azimuthOnArc && (d.azimuth >= a - tol || d.azimuth <= b + tol);
    }
    std::printf("  [measured] barrel seam: %zu dab(s) between the two samples, worst off-arc "
                "distance %.4f\n",
                between, static_cast<double>(worst));
    check(between >= 3, "7. setup: several dabs land between the two seam samples");
    check(onArc, "7. barrel wrap: every in-between dab is on the short arc (>= 0.9986 or <= 0.0097)");
    check(azimuthOnArc, "7. azimuth wrap: same rule, same result");
    check(inRange, "7. every dab's barrel stays in [0,1]");

    // A barrel held at exactly +180 degrees (1.0, a value
    // `penBarrelNormalised()` really returns) stays 1.0 -- not 0.0.
    StrokePath held;
    std::vector<StrokeDab> heldDabs;
    for (int i = 0; i < 4; ++i) {
      StrokeSample s;
      s.pos = Vec2{10.0f * static_cast<float>(i), 0.0f};
      s.barrel = 1.0f;
      held.addPoint(s, 1.0f, heldDabs);
    }
    held.flush(1.0f, heldDabs);
    bool allOne = !heldDabs.empty();
    for (const StrokeDab& d : heldDabs) allOne = allOne && d.barrel == 1.0f;
    check(allOne, "7. a barrel held at +180 deg (1.0) interpolates to exactly 1.0, not 0.0");
  }

  // ==========================================================================
  // 8. Memory bound. 10 000 samples of a gesture that is never claimed and
  //    never ended, with ImGui never catching up, and 10 000 separate clicks
  //    likewise: the queue stays at its stated caps (app/PointerQueue.hpp
  //    section 4) and keeps the NEWEST input.
  // ==========================================================================
  {
    using K = PointerEventKind;
    PointerQueue q;
    q.push(mouseEv(K::MouseButtonDown, 0.0f, 1));
    for (int i = 1; i < 10000; ++i) q.push(mouseEv(K::MouseMotion, static_cast<float>(i), 1));
    std::printf("  [measured] 10000 unclaimed samples -> queue size %zu (cap %zu)\n", q.size(),
                PointerQueue::kMaxSamples);
    check(q.size() <= PointerQueue::kMaxSamples,
          "8. bound: 10 000 unclaimed, never-ended samples stay within kMaxSamples");
    q.beginFrame(2);
    const std::vector<PointerSample> kept = q.takeForStroke(q.claimGesture());
    check(!kept.empty() && kept.back().x == 9999.0f, "8. bound: the newest sample is the one kept");

    PointerQueue c;
    for (int i = 0; i < 10000; ++i) {
      c.push(mouseEv(K::MouseButtonDown, static_cast<float>(i), 1));
      c.push(mouseEv(K::MouseButtonUp, static_cast<float>(i), 1));
    }
    std::printf("  [measured] 10000 unprocessed clicks -> %zu gesture record(s), %zu sample(s)\n",
                c.gestureCount(), c.size());
    check(c.gestureCount() <= PointerQueue::kMaxGestures && c.size() <= PointerQueue::kMaxGestures,
          "8. bound: 10 000 clicks ImGui never processed stay within kMaxGestures records");
  }

  std::printf("[selftest] pointer queue %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
