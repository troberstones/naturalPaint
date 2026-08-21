#include "app/selftest/Support.hpp"

namespace np {

bool runAccumulatorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Synthetic "frame time" sequence in ms: a mix of typical paces
  // (~8ms/120Hz, ~16ms/60Hz, ~33ms/30Hz) plus one deliberate 500ms stall
  // (a breakpoint, a window drag) to exercise the catch-up cap.
  const float frameTimesMs[] = {8.3f,  16.7f, 8.3f,  33.3f, 16.7f,
                                500.0f, 16.7f, 8.3f,  33.3f, 16.7f};
  constexpr int n = static_cast<int>(sizeof(frameTimesMs) / sizeof(frameTimesMs[0]));
  constexpr int kStallIdx = 5;

  // --- determinism: two independent accumulators fed the identical input
  // sequence must produce the identical step-count sequence and end in the
  // identical state. This is the property that matters for the interactive
  // path: --diag/--selftest never touch main.cpp's accumulator (they call
  // PaintSim::frame() directly, no wall clock involved), so this is the
  // only place that property is actually checked. ---
  float accA = 0.0f, accB = 0.0f;
  int stepsA[n], stepsB[n];
  for (int i = 0; i < n; ++i) {
    stepsA[i] = consumeFixedSteps(accA, frameTimesMs[i], kFixedDtMs, kMaxCatchUpMs,
                                  kMaxStepsPerFrame);
    stepsB[i] = consumeFixedSteps(accB, frameTimesMs[i], kFixedDtMs, kMaxCatchUpMs,
                                  kMaxStepsPerFrame);
  }
  bool sameSteps = true;
  for (int i = 0; i < n; ++i)
    if (stepsA[i] != stepsB[i]) sameSteps = false;
  check(sameSteps && accA == accB,
        "two accumulators fed identical input produce identical output");

  // --- the 500ms stall must clamp to kMaxStepsPerFrame (8), not
  // floor(500 / (1000/240)) = 120. ---
  check(stepsA[kStallIdx] == kMaxStepsPerFrame,
        "500ms stall clamps to the per-frame step cap, not floor(500/dt)=120");

  // --- the two caps are independent. kMaxCatchUpMs bounds how much real
  // time a single call may ADD to the accumulator; kMaxStepsPerFrame
  // separately bounds how many ticks a single call may REPORT. Isolate the
  // add-side cap: with a tick so coarse that even a maxCatchUpMs-sized
  // addition is still under one tick, and an effectively unlimited step
  // budget, a single huge stall should leave acc at exactly kMaxCatchUpMs —
  // not at the raw, far larger, stall duration. ---
  {
    float acc = 0.0f;
    const float coarseDtMs = kMaxCatchUpMs * 10.0f;
    const int steps =
        consumeFixedSteps(acc, /*realElapsedMs=*/5000.0f, coarseDtMs, kMaxCatchUpMs,
                          /*maxSteps=*/1000);
    check(steps == 0, "coarse tick + huge stall: no whole tick has banked yet");
    check(acc == kMaxCatchUpMs,
          "add-side cap alone bounds acc growth, independent of the step cap");
  }

  // --- and isolate the step-side cap: even with a generous add-side cap
  // that would bank far more than kMaxStepsPerFrame ticks in one shot, the
  // reported step count still holds at the cap. ---
  {
    float acc = 0.0f;
    const int steps = consumeFixedSteps(acc, /*realElapsedMs=*/5000.0f, kFixedDtMs,
                                        /*maxCatchUpMs=*/5000.0f, kMaxStepsPerFrame);
    check(steps == kMaxStepsPerFrame,
          "step-side cap holds even when the add-side cap alone would allow more");
  }

  std::printf("[selftest] accumulator %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
