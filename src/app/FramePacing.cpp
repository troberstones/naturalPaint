#include "app/FramePacing.hpp"

namespace np {

FramePacingPlan planFramePacing(const FramePacingInputs& in) {
  FramePacingPlan plan;

  // Tier 1. The exemption is checked first and unconditionally: a capture run
  // must not be paced even if it happens to look idle, which is exactly what
  // a golden view looks like (a scripted demo that settles and then sits
  // still for 90 frames). Painting is the report's own carve-out.
  if (in.exempt || in.painting) {
    plan.tier = FramePacingTier::Unthrottled;
    plan.periodNs = 0;
    plan.waitOnEvents = false;
    return plan;
  }

  if (in.nsSinceActivity < kFramePacingIdleAfterNs) {
    // Tier 2.
    plan.tier = FramePacingTier::Interactive;
    plan.periodNs = kFramePacingInteractivePeriodNs;
    // Never block on the queue here -- a moving pointer would return from the
    // wait instantly and the cap would not exist. FramePacing.hpp §3.
    plan.waitOnEvents = false;
  } else {
    // Tier 3.
    plan.tier = FramePacingTier::Idle;
    plan.periodNs = kFramePacingIdlePeriodNs;
    plan.waitOnEvents = true;
  }

  // The solver ceiling (FramePacing.hpp §2). Applied to whatever tier was
  // just chosen rather than only to Idle, so that lowering
  // kFramePacingInteractivePeriodNs below the ceiling one day cannot
  // reintroduce the debt through the other branch. It is a no-op on tier 2 at
  // today's constants (16.7 ms is already under 33.3 ms).
  //
  // The tier itself is NOT downgraded: the frame is still an idle frame, it
  // is simply not allowed to be as slow as an idle frame with no physics
  // running. `waitOnEvents` stays true with it, which is what keeps the
  // wake-up immediate in the case that matters most -- a canvas of wet paint
  // the user is about to touch again.
  if (in.simLive && plan.periodNs > kFramePacingSimCeilingNs)
    plan.periodNs = kFramePacingSimCeilingNs;

  return plan;
}

const char* framePacingTierName(FramePacingTier tier) {
  switch (tier) {
    case FramePacingTier::Unthrottled: return "unthrottled";
    case FramePacingTier::Interactive: return "interactive";
    case FramePacingTier::Idle: return "idle";
  }
  return "?";
}

uint64_t framePacingWaitNs(uint64_t periodNs, uint64_t previousFrameStartNs, uint64_t nowNs) {
  if (periodNs == 0) return 0;
  // The first iteration of the loop has no previous frame to space itself
  // from. Sleeping a whole period before the window's first frame would be a
  // visible startup stall for nothing.
  if (previousFrameStartNs == 0) return 0;
  // Defensive rather than load-bearing: SDL_GetTicksNS() is monotonic, so
  // `nowNs` cannot precede a value it produced earlier in the same run. The
  // branch exists so that a caller passing a synthetic timeline (the suite
  // does) cannot produce a wrap-around wait of ~584 years from the unsigned
  // subtraction below.
  if (nowNs <= previousFrameStartNs) return periodNs;
  const uint64_t elapsedNs = nowNs - previousFrameStartNs;
  if (elapsedNs >= periodNs) return 0;
  return periodNs - elapsedNs;
}

}  // namespace np
