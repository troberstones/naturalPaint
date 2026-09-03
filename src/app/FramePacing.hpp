#pragma once

#include <cstdint>

#include "app/FixedStep.hpp"

// app/FramePacing -- how long this frame is allowed to take, and how to wait.
//
// ==========================================================================
// 0. The report
// ==========================================================================
//
//   T27 "lets throttle the UI unless drawing to 60fps, and when nothing is
//        happening, throttle it further."
//
// Three tiers, and the report names all three: painting (uncapped), awake but
// not painting (60 fps), and nothing happening at all (lower still).
//
// **The report is not describing a hypothetical.** `gfx/Context.cpp`'s
// surface configuration uses `WGPUPresentMode_Fifo`, so presentation is
// vsync-locked -- which on a 60 Hz panel would already be the ceiling the
// report asks for and would make tier 2 a no-op. It is not the ceiling on the
// machine this was written on: 600 measured frames of a `--screenshot` run
// took 5.04 s, i.e. 8.4 ms per frame, ~119 fps, on a built-in Liquid Retina
// XDR (ProMotion) display. Twice the work, twice the power, for a UI that is
// showing a still image. So tier 2 is a real halving here and tier 3 is a
// further ~10x on top of it -- but on a 60 Hz external monitor tier 2 costs
// nothing and changes nothing, which is the correct behaviour for a cap.
//
// ==========================================================================
// 1. Why a pure function instead of four lines in the frame loop
// ==========================================================================
//
// `--selftest` has no window and never enters `main.cpp`'s `while (!st.quit)`
// loop at all (it returns from the `--selftest` branch hundreds of lines
// above it), so a pacing decision written inline in that loop would have
// exactly zero assertions on it forever -- and the golden harness cannot see
// it either, because pacing is a property of *when* frames happen, not of
// what any one of them contains.
//
// This is the same move `app/ToolSwitch` made for the same reason: lift the
// decision out of the UI block, leave the loop holding only the mechanical
// "wait this long, this way", and let the suite drive the decision directly
// across every tier boundary in the order the loop calls it.
//
// ==========================================================================
// 2. The simulation ceiling -- the constraint that picks the idle number
// ==========================================================================
//
// The obvious idle number is "very low", and it is wrong, because the frame
// loop is also the physics loop. `main.cpp` runs `PaintSim::frame()` on every
// render frame while `!st.paused`, through `consumeFixedSteps()`
// (app/FixedStep.hpp), which converts real elapsed milliseconds into
// `kFixedDt` (1/240 s) ticks and **caps a single frame at
// `kMaxStepsPerFrame` (8) ticks**.
//
// Eight ticks is 33.3 ms of simulated time. So:
//
//   * At a 33.3 ms frame period (30 fps) the solver exactly keeps real time.
//   * Below that it cannot. At a 166 ms period the accumulator is handed
//     166 ms and gives back 33.3 ms, so wet paint flows at one fifth speed
//     **and banks 133 ms of debt every frame, without bound** -- and
//     `consumeFixedSteps()` keeps that debt in `acc` rather than dropping it.
//     Wake after ten idle seconds and the solver then runs its maximum 8
//     ticks on every frame until seconds of banked debt drain: a
//     two-times-real-time fast-forward of the whole canvas if the wake was a
//     mouse move (tier 2's 16.7 ms frames), and four times if the user went
//     straight back to painting (tier 1, ~8.4 ms here).
//
// That is a far worse artefact than the power draw being fixed. So the plan
// clamps any period to `kFramePacingSimCeilingNs` whenever the solver is live
// (constructed and not paused), and the deep-idle tier applies at its full
// depth only in the state where there is no physics to fall behind: before
// the first stroke has constructed a `PaintSim` at all (`main.cpp` builds it
// lazily -- see `ensurePaintSim()`), or while paused.
//
// The ceiling is derived from `FixedStep.hpp`'s own constants rather than
// written out as 33 ms, so that raising `kMaxStepsPerFrame` raises this with
// it instead of leaving a stale literal that silently re-introduces the debt.
//
// ==========================================================================
// 3. Wait on the event queue, or sleep? -- both, for different tiers
// ==========================================================================
//
// The two mechanisms are not interchangeable and neither works for both
// tiers:
//
//   * **Tier 3 must block on the event queue** (`SDL_WaitEventTimeout(nullptr,
//     ms)`, which leaves the event in the queue for the loop's own
//     `SDL_PollEvent` drain below). A throttle the user can feel is worse
//     than no throttle, and at a 166 ms period a fixed sleep would put up to
//     166 ms of dead air between the mouse moving and the cursor following
//     it. Blocking on the queue wakes on the *first* input event instead, so
//     the observable wake-up latency is the scheduler's, not the tier's.
//
//   * **Tier 2 must NOT block on the event queue.** A moving pointer produces
//     motion events at the tablet's or trackpad's own rate, which is at or
//     above 120 Hz on this hardware; `SDL_WaitEventTimeout` would return
//     immediately every time and the 60 fps cap would simply not exist. Tier
//     2 therefore sleeps to the frame deadline. The cost is bounded and
//     understood: on a 120 Hz panel a capped frame starts at most 8.3 ms
//     later than it otherwise would, which is the price of the cap the report
//     asked for, and it is paid only on frames where the user is not
//     painting.
//
// Tier 1 does neither. Vsync is the only pacing a painting frame gets.
//
// ==========================================================================
// 4. Where the wait goes, and what must stay exempt
// ==========================================================================
//
// The loop performs the wait as the **first** thing in the iteration, before
// `frameStartNs` is sampled and before `SDL_PollEvent` is drained. Two
// separate things depend on that placement:
//
//   * `app/Latency` correlates `st.lastInputEventNs` (an SDL event timestamp)
//     against the post-present clock. Waiting *after* the poll would fold the
//     wait into every pen-to-photon sample and make `--latency` report the
//     throttle instead of the renderer. Waiting before the poll means the
//     events drained afterwards are ones that arrived during the wait, and
//     the measurement is untouched.
//   * `--frame-trace`'s `total_ms` is `presentNs - frameStartNs`, and
//     `frameStartNs` is sampled after the wait, so those numbers keep
//     describing the frame rather than the sleep.
//
// `exempt` is the screenshot path, and it is a correctness requirement of the
// golden harness rather than an optimisation. `tools/golden/run_golden.sh`
// drives 20 views at 90 settle frames each through `--screenshot`. Removing
// the exemption was measured, not reasoned about: a full `run_golden.sh
// check` went from 25.3 s to 43.0 s, and it is worth knowing exactly why it
// was not worse. 90 frames at tier 2 take 1.5 s, which is *shorter than
// `kFramePacingIdleAfterNs`* -- so an unexempt capture run never reaches tier
// 3 at all, and the damage is the 2x of the 60 fps cap rather than the 20x
// the idle tier would cost. Raise the settle count past ~120 frames and that
// stops being true. The exemption is what makes the harness's runtime
// independent of a constant in this file.
//
// `AppState::screenshotCliActive` is true for the entire life of a
// `--screenshot <path> [frames]` run (set once in `main()` before the loop),
// which is exactly the span that must not be paced. It is the only flag the
// loop feeds in: `AppState::requestScreenshot`, the in-session Save
// Screenshot action, is set and consumed inside a single iteration and so is
// always false where the pacing decision is taken -- see the call site for
// why testing it anyway would be an inert term rather than belt and braces.

namespace np {

enum class FramePacingTier {
  // The user is laying down paint, or this is a capture run. No frame budget
  // at all; vsync is the only limit.
  Unthrottled,
  // Awake -- input, a held widget, a stroke that just ended -- but not
  // painting. 60 fps ceiling.
  Interactive,
  // Nothing has happened for kFramePacingIdleAfterNs. As slow as section 2
  // allows.
  Idle,
};

// 60 fps, the number the report names.
constexpr uint64_t kFramePacingInteractivePeriodNs = 1'000'000'000ull / 60;

// 6 fps for the deep-idle tier.
//
// The argument for a number this low is that nothing on screen is moving in
// this state -- by construction, since section 2's clamp keeps a live solver
// out of it -- so the only thing a higher rate would buy is a faster response
// to input, and input does not wait for the period at all: tier 3 blocks on
// the event queue and wakes on the event itself. What is left is a redraw of
// an unchanged image, and 6 Hz is enough to keep an ImGui hover lerp or a
// tooltip fade from looking dead on the frames between the wake-up and the
// tier returning to Interactive.
//
// It is deliberately not 1 fps: SDL_WaitEventTimeout wakes on input but not
// on a GPU or window-server condition, and a one-second window between
// unforced redraws is long enough for a display reconfiguration or an
// occlusion change to be visible as a stale window.
constexpr uint64_t kFramePacingIdlePeriodNs = 1'000'000'000ull / 6;

// How long "nothing happening" has to last before tier 3.
//
// Two seconds, chosen against the failure mode rather than for the saving: a
// user who is mid-gesture but momentarily still -- pen down and stationary, a
// slider grabbed and held, a menu open under a motionless cursor -- must not
// be dropped into a tier while the gesture is live. The loop feeds all three
// of those into `nsSinceActivity` as activity, so this interval is only the
// backstop for whatever they miss, and two seconds is longer than any of them
// can plausibly be wrong for. Shorter would start trading real risk for
// milliwatts; much longer and the tier would rarely be reached at all.
constexpr uint64_t kFramePacingIdleAfterNs = 2'000'000'000ull;

// The longest frame period at which `consumeFixedSteps()` still advances the
// solver at real time: kMaxStepsPerFrame ticks of kFixedDt. 33.33 ms / 30 fps
// with today's constants. See section 2 -- crossing this does not merely slow
// the picture, it banks unbounded simulation debt.
constexpr uint64_t kFramePacingSimCeilingNs =
    static_cast<uint64_t>(static_cast<double>(kMaxStepsPerFrame) *
                          static_cast<double>(kFixedDt) * 1'000'000'000.0);

struct FramePacingInputs {
  // A `--screenshot` run (AppState::screenshotCliActive, true for the whole
  // run, which is what the golden harness uses) or a capture requested from
  // the menu this frame. Overrides everything below.
  bool exempt = false;
  // The user is painting: AppState::paintingThisFrame (down, hovered, inside
  // the canvas -- true on every frame of a stroke, including the ones between
  // dabs where the pointer has not moved) or AppState::strokeActive (the
  // stroke as a whole, which survives the pointer leaving the canvas).
  bool painting = false;
  // A PaintSim exists and is not paused, so this frame will step physics and
  // section 2's ceiling applies.
  bool simLive = false;
  // Since the last frame that saw an input event, a held widget, or painting.
  uint64_t nsSinceActivity = 0;
};

struct FramePacingPlan {
  FramePacingTier tier = FramePacingTier::Unthrottled;
  // The minimum wall-clock spacing between frame starts. 0 means no floor.
  uint64_t periodNs = 0;
  // True to spend the wait blocked on the event queue rather than asleep --
  // see section 3. Only ever true for Idle.
  bool waitOnEvents = false;
};

// The whole decision, given last frame's observations. Pure: no clock, no
// SDL, no state.
FramePacingPlan planFramePacing(const FramePacingInputs& in);

// How long the loop must wait before starting a frame whose budget is
// `periodNs`, given when the previous frame started. 0 for an unpaced frame,
// for a frame that is already late, and for the first frame of the run
// (`previousFrameStartNs == 0`). Separate from planFramePacing() because it
// is the only part that touches the clock, and separating them is what lets
// the suite drive a whole synthetic timeline through both.
uint64_t framePacingWaitNs(uint64_t periodNs, uint64_t previousFrameStartNs, uint64_t nowNs);

// "unthrottled" / "interactive" / "idle", for `--frame-trace`'s per-frame
// line. Pacing is the one property of a frame that a screenshot can never
// show and `--selftest` can never observe in situ, so the trace is the only
// place the running application can be asked which tier it actually chose.
const char* framePacingTierName(FramePacingTier tier);

}  // namespace np
