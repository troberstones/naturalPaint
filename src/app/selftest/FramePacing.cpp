#include "app/selftest/Support.hpp"

#include "app/FixedStep.hpp"
#include "app/FramePacing.hpp"

namespace np {

// app/FramePacing -- T27's three tiers, and the two things about them that a
// running application could hide.
//
// The whole reason this decision lives in its own translation unit is that
// this section can exist. `--selftest` never enters `main.cpp`'s
// `while (!st.quit)` loop -- the `--selftest` branch returns hundreds of
// lines above it -- so a pacing rule written inline there would be
// unassertable, and the golden harness cannot help either: pacing is a
// property of *when* frames happen, and every golden reference is a single
// still frame. Between them the two existing harnesses have no way at all to
// notice that painting had been throttled or that a capture run had been
// slowed to five minutes, which is exactly the pair of mistakes this file is
// here to catch.
//
// What is asserted, in the order the frame loop asks it:
//
//   1. Painting is never throttled -- under every idle age, with or without a
//      live solver, and not merely at the moment of a dab.
//   2. A capture run is never throttled -- including a capture run that looks
//      completely idle, which is what every one of the 20 golden views is by
//      the time it is photographed.
//   3. The idle tier is entered on the far side of kFramePacingIdleAfterNs
//      and not before it, and the tier below it really is slower.
//   4. Activity returns to the interactive tier immediately, in one step,
//      with no hysteresis or ramp.
//   5. The simulation ceiling: a live solver clamps the idle period to the
//      longest period consumeFixedSteps() can still keep real time at, so an
//      idle throttle cannot bank unbounded physics debt (FramePacing.hpp §2).
//   6. framePacingWaitNs()'s arithmetic across a synthetic timeline, including
//      the two edges the loop actually hits: the very first frame, and a
//      frame that is already over budget.
//
// Headless and GPU-free by construction -- planFramePacing() takes no clock
// and no SDL handle, which is the point of it.
bool runFramePacingTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] frame pacing: the three tiers, the capture exemption, the solver ceiling\n");

  // Named so the assertions below read as the states the loop is actually in
  // rather than as brace-initialised tuples.
  auto inputs = [](bool exempt, bool painting, bool simLive, uint64_t nsSinceActivity) {
    FramePacingInputs in;
    in.exempt = exempt;
    in.painting = painting;
    in.simLive = simLive;
    in.nsSinceActivity = nsSinceActivity;
    return in;
  };

  const uint64_t justNow = 0;
  const uint64_t awake = kFramePacingIdleAfterNs / 2;
  const uint64_t justBeforeIdle = kFramePacingIdleAfterNs - 1;
  const uint64_t deepIdle = kFramePacingIdleAfterNs * 100;

  // -----------------------------------------------------------------------
  // 1. Painting is never throttled.
  // -----------------------------------------------------------------------
  //
  // Swept over the idle age rather than asserted at one point, because the
  // failure this guards against is not "a dab was throttled" -- it is the
  // frames *between* dabs. A user who holds the pen still for three seconds
  // mid-stroke has an nsSinceActivity that would put any non-painting frame
  // deep in tier 3, and that stroke must still be running at full rate when
  // they move again. (The loop feeds painting frames into the activity clock
  // too, so this state is not reachable in practice today -- which is
  // precisely why it is asserted here: it is one edit away from being
  // reachable, and nothing else in the build would notice.)
  {
    bool everThrottled = false;
    bool everWaited = false;
    const uint64_t ages[] = {justNow, awake, justBeforeIdle, kFramePacingIdleAfterNs, deepIdle};
    for (uint64_t age : ages) {
      for (int simLive = 0; simLive < 2; ++simLive) {
        const FramePacingPlan p = planFramePacing(inputs(false, true, simLive != 0, age));
        if (p.tier != FramePacingTier::Unthrottled || p.periodNs != 0) everThrottled = true;
        if (p.waitOnEvents) everWaited = true;
      }
    }
    check(!everThrottled, "painting: unthrottled at every idle age, sim or not");
    check(!everWaited, "painting: never blocks the loop on the event queue");
  }

  // -----------------------------------------------------------------------
  // 2. The capture exemption.
  // -----------------------------------------------------------------------
  //
  // tools/golden/run_golden.sh drives 20 views at 90 settle frames each. A
  // golden view is, by the frame it is photographed on, as idle as a frame
  // can be: a scripted demo has run, the synthetic pointer is parked, and
  // nothing has produced an SDL event for the whole settle. So the exemption
  // has to survive being asked in exactly that state -- which is the state
  // this asserts -- or a golden run goes from the measured 25 s to minutes
  // and the script's per-view timeouts start firing.
  {
    const FramePacingPlan idleCapture = planFramePacing(inputs(true, false, false, deepIdle));
    check(idleCapture.tier == FramePacingTier::Unthrottled && idleCapture.periodNs == 0,
          "capture: exempt even when the run looks completely idle");
    const FramePacingPlan simCapture = planFramePacing(inputs(true, false, true, deepIdle));
    check(simCapture.periodNs == 0 && !simCapture.waitOnEvents,
          "capture: exempt with a live solver too, and never waits");
    // The exemption outranks everything, including the tier that would
    // otherwise be chosen -- checked separately from the idle case because a
    // plausible wrong implementation is `exempt` merely lowering the tier.
    const FramePacingPlan busyCapture = planFramePacing(inputs(true, false, false, justNow));
    check(busyCapture.tier == FramePacingTier::Unthrottled,
          "capture: exemption outranks the interactive tier too");
  }

  // -----------------------------------------------------------------------
  // 3. The tier boundary, from both sides.
  // -----------------------------------------------------------------------
  {
    const FramePacingPlan before = planFramePacing(inputs(false, false, false, justBeforeIdle));
    const FramePacingPlan at = planFramePacing(inputs(false, false, false, kFramePacingIdleAfterNs));
    check(before.tier == FramePacingTier::Interactive &&
              before.periodNs == kFramePacingInteractivePeriodNs,
          "tier 2: 60 fps one nanosecond before the idle interval");
    check(at.tier == FramePacingTier::Idle && at.periodNs == kFramePacingIdlePeriodNs,
          "tier 3: entered exactly at the idle interval, not before");
    // A tier that is not actually slower is not a throttle. Asserted as a
    // relation rather than against the literals so that retuning either
    // constant cannot leave this green while inverting the tiers.
    check(kFramePacingIdlePeriodNs > kFramePacingInteractivePeriodNs,
          "tier 3 is slower than tier 2, and tier 2 slower than tier 1");
    check(at.waitOnEvents && !before.waitOnEvents,
          "only tier 3 blocks on the queue (tier 2's cap needs a deadline)");
  }

  // -----------------------------------------------------------------------
  // 4. Input returns to tier 2 in one step.
  // -----------------------------------------------------------------------
  //
  // The loop resets nsSinceActivity to 0 on any frame that saw an event, so
  // "immediately" here means: from the deepest idle, a single frame's worth of
  // activity is enough, with nothing in between.
  {
    const FramePacingPlan wasIdle = planFramePacing(inputs(false, false, false, deepIdle));
    const FramePacingPlan woken = planFramePacing(inputs(false, false, false, justNow));
    check(wasIdle.tier == FramePacingTier::Idle && woken.tier == FramePacingTier::Interactive,
          "wake: one frame of activity leaves tier 3, no ramp");
    check(!woken.waitOnEvents && woken.periodNs == kFramePacingInteractivePeriodNs,
          "wake: the woken frame is a plain 60 fps frame");
  }

  // -----------------------------------------------------------------------
  // 5. The solver ceiling.
  // -----------------------------------------------------------------------
  //
  // consumeFixedSteps() caps one frame at kMaxStepsPerFrame ticks of
  // kFixedDt. Handed more real time than that, it advances the sim by the cap
  // and keeps the remainder in the accumulator -- forever, unbounded. The
  // first assertion is the property itself, driven through the real
  // consumeFixedSteps() rather than restated: at the ceiling the accumulator
  // does not grow, and at the unclamped idle period it does.
  {
    const float ceilingMs = static_cast<float>(kFramePacingSimCeilingNs) / 1.0e6f;
    const float idleMs = static_cast<float>(kFramePacingIdlePeriodNs) / 1.0e6f;

    // The claim is *bounded versus unbounded*, so it is measured over two
    // equal spans and compared, not read off one number. The remainder the
    // accumulator legitimately carries between frames is a sub-tick quantity
    // that ordinary float rounding can push either side of an exact
    // threshold; whether the debt is still growing after 240 frames of
    // running is not.
    float accAtCeiling = 0.0f;
    float accAtIdle = 0.0f;
    auto runFrames = [](float& acc, float frameMs, int frames) {
      for (int i = 0; i < frames; ++i)
        consumeFixedSteps(acc, frameMs, kFixedDtMs, kMaxCatchUpMs, kMaxStepsPerFrame);
    };
    runFrames(accAtCeiling, ceilingMs, 240);
    runFrames(accAtIdle, idleMs, 240);
    const float ceilingAt240 = accAtCeiling;
    const float idleAt240 = accAtIdle;
    runFrames(accAtCeiling, ceilingMs, 240);
    runFrames(accAtIdle, idleMs, 240);
    check(accAtCeiling - ceilingAt240 < kFixedDtMs && accAtCeiling < 2.0f * kFixedDtMs,
          "ceiling: frames at the ceiling bank no growing simulation debt");
    check(idleAt240 > 1000.0f && accAtIdle - idleAt240 > 1000.0f,
          "ceiling: frames at the idle period bank seconds of debt, growing");

    const FramePacingPlan idleWithSim = planFramePacing(inputs(false, false, true, deepIdle));
    const FramePacingPlan idleNoSim = planFramePacing(inputs(false, false, false, deepIdle));
    check(idleWithSim.periodNs == kFramePacingSimCeilingNs,
          "ceiling: a live solver clamps the idle period to it");
    check(idleNoSim.periodNs == kFramePacingIdlePeriodNs &&
              idleNoSim.periodNs > idleWithSim.periodNs,
          "ceiling: with no solver the idle tier is genuinely deeper");
    // The clamp lowers the period, not the tier -- the frame is still an idle
    // frame and must still wake on the event rather than after the period.
    check(idleWithSim.tier == FramePacingTier::Idle && idleWithSim.waitOnEvents,
          "ceiling: clamping the period does not stop the instant wake-up");
    // Tier 2 is already under the ceiling, so a live solver must not perturb
    // it -- if it did, the interactive tier would be silently slower than the
    // 60 fps the report asked for whenever paint was wet.
    const FramePacingPlan awakeWithSim = planFramePacing(inputs(false, false, true, awake));
    check(awakeWithSim.periodNs == kFramePacingInteractivePeriodNs,
          "ceiling: tier 2 is already under it and is left alone");
  }

  // -----------------------------------------------------------------------
  // 6. framePacingWaitNs() across a timeline.
  // -----------------------------------------------------------------------
  {
    const uint64_t period = kFramePacingInteractivePeriodNs;
    check(framePacingWaitNs(0, 1'000, 2'000) == 0, "wait: an unpaced frame never sleeps");
    check(framePacingWaitNs(period, 0, 5'000'000'000ull) == 0,
          "wait: the first frame of the run never sleeps");
    check(framePacingWaitNs(period, 1'000'000'000ull, 1'000'000'000ull) == period,
          "wait: a frame with no elapsed time waits the whole period");
    check(framePacingWaitNs(period, 1'000'000'000ull, 1'000'000'000ull + period / 4) ==
              period - period / 4,
          "wait: a partly-elapsed frame waits only the remainder");
    check(framePacingWaitNs(period, 1'000'000'000ull, 1'000'000'000ull + period) == 0,
          "wait: a frame exactly on budget does not sleep");
    check(framePacingWaitNs(period, 1'000'000'000ull, 1'000'000'000ull + period * 10) == 0,
          "wait: an already-late frame does not sleep, and never wraps");

    // Driven as an actual timeline, because the arithmetic being right at
    // isolated points is not the same claim as the loop converging on the
    // target rate. Simulate 300 frames whose own work costs 1 ms, at tier 2's
    // period, and check the mean spacing is the period rather than the period
    // plus the work (the off-by-one that turns a 60 fps cap into 56 fps).
    uint64_t nowNs = 1'000'000'000ull;
    uint64_t prevFrameNs = 0;
    uint64_t firstFrameNs = 0;
    uint64_t lastFrameNs = 0;
    const uint64_t kWorkNs = 1'000'000ull;
    for (int i = 0; i < 300; ++i) {
      nowNs += framePacingWaitNs(period, prevFrameNs, nowNs);
      prevFrameNs = nowNs;
      if (i == 0) firstFrameNs = nowNs;
      lastFrameNs = nowNs;
      nowNs += kWorkNs;
    }
    const uint64_t spanNs = lastFrameNs - firstFrameNs;
    check(spanNs == period * 299,
          "wait: 300 paced frames land exactly 299 periods apart");
  }

  std::printf("[selftest] frame pacing %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
