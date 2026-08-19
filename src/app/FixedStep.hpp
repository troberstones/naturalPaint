#pragma once

namespace np {

// PRD H7 — fixed timestep.
//
// Before this, `main.cpp` called `PaintSim::frame()` exactly once per
// rendered UI frame, so total simulated time advanced at renderFPS * dt: a
// wash on a fast machine ran faster than the same wash on a slow one.
// `consumeFixedSteps()` below decouples the two — the render loop reports
// how much real time just elapsed and gets back how many kFixedDt-sized
// physics ticks to run before drawing this frame.
//
// kFixedDt matches the substep rate the solver was already effectively
// running at (PaintSim::substeps / inkSubsteps split a per-call dt into
// several smaller passes for numerical stability — that mechanism is
// unrelated and untouched; this constant is about how often a call happens
// at all, not what happens inside one).
constexpr float kFixedDt = 1.0f / 240.0f;          // seconds
constexpr float kFixedDtMs = kFixedDt * 1000.0f;   // milliseconds

// Two independent caps guard two different failure modes:
//
// - kMaxCatchUpMs bounds how much real elapsed time a single call may ADD
//   to the accumulator. Without it, one freak stall (a breakpoint, a window
//   drag, a spotlight GC pause) hands the accumulator a multi-second
//   backlog that then takes many frames to work off, so the sim visibly
//   "runs fast" for a while after every hitch. 250ms follows the usual
//   fixed-timestep convention (Fiedler, "Fix Your Timestep!") of giving one
//   hitch enough slack to be absorbed without letting it become a
//   standing debt.
constexpr float kMaxCatchUpMs = 250.0f;

// - kMaxStepsPerFrame bounds how many ticks a single call may REPORT,
//   independent of how large the accumulator is. Without it, draining a
//   legitimate backlog could itself make one frame's compute cost balloon,
//   which slows that frame, which grows the backlog further next time —
//   the death spiral the plan calls out. 8 ticks is 2x the ~4 ticks a
//   240Hz-equivalent target needs per frame at 60fps, enough headroom to
//   absorb ordinary frame-pacing jitter before falling behind.
constexpr int kMaxStepsPerFrame = 8;

// Advances the accumulator by the (capped) real time elapsed and reports
// how many fixed ticks to run, leaving the sub-tick remainder in `acc` for
// next time. Pure and stateless beyond `acc` — no engine, SDL, or GPU
// dependency — so it can be exercised directly by --selftest (see the
// accumulator determinism test in SelfTest.cpp) without a window or device.
//
// Parameterised rather than reading the constants above directly so the
// test can probe each cap in isolation (e.g. an unlimited step budget to
// prove the add-side cap alone, or a huge add-side cap to prove the
// step-side cap alone).
inline int consumeFixedSteps(float& acc, float realElapsedMs, float fixedDtMs,
                             float maxCatchUpMs, int maxSteps) {
  acc += realElapsedMs < maxCatchUpMs ? realElapsedMs : maxCatchUpMs;
  int steps = static_cast<int>(acc / fixedDtMs);
  if (steps > maxSteps) steps = maxSteps;
  acc -= static_cast<float>(steps) * fixedDtMs;
  return steps;
}

}  // namespace np
