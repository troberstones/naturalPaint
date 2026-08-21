#include "app/selftest/Support.hpp"

namespace np {

// core/OpStack (Phase 3 step 5). See SelfTest.hpp for the full breakdown.
// Pure CPU bookkeeping plus calls into the already-tested ops/PointOps
// functions -- no PaintSim or gpu involvement anywhere in this function.
bool runOpStackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto nearRgb = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float tol) {
    return near(a[0], b[0], tol) && near(a[1], b[1], tol) && near(a[2], b[2], tol);
  };
  // Runs `v` through `ops` in order, exactly the way a future color/LutBake
  // (or ops::applyPointOpsPremultiplied()) would consume an OpRun's
  // composed list.
  auto runOps = [](const std::vector<PointOp>& ops, std::array<float, 3> v) {
    for (const PointOp& op : ops) v = op(v);
    return v;
  };

  // --- empty stack: no runs, version starts at 0 ---
  {
    OpStack stack;
    check(stack.size() == 0, "OpStack: a default-constructed stack is empty");
    check(stack.version() == 0, "OpStack: version() starts at 0");
    check(stack.detectRuns().empty(), "OpStack: detectRuns() on an empty stack returns no runs");
  }

  // --- every mutator increments version(); at()/size() do not ---
  {
    OpStack stack;
    Op probe;
    probe.pointKind = PointOpKind::Exposure;
    probe.exposure.stops = 0.5f;

    const size_t idx = stack.add(probe);
    check(stack.version() == 1, "OpStack: add() increments version() by exactly one, from 0");
    check(idx == 0, "OpStack: add() returns the new entry's index");

    const uint64_t vAfterAdd = stack.version();
    (void)stack.at(idx);
    (void)stack.size();
    check(stack.version() == vAfterAdd, "OpStack: at()/size() do not change version()");

    stack.setEnabled(idx, false);
    check(stack.version() == vAfterAdd + 1, "OpStack: setEnabled() increments version()");
    check(!stack.at(idx).enabled, "OpStack: setEnabled() actually applies");

    // Same value it already has -- still bumps, by design (see
    // OpStack.hpp's version() doc comment: erring toward over-bumping
    // rather than checking whether anything actually changed).
    const uint64_t vBeforeNoopSet = stack.version();
    stack.setEnabled(idx, false);
    check(stack.version() == vBeforeNoopSet + 1,
          "OpStack: setEnabled() bumps version() even when the value doesn't change");

    const uint64_t vBeforeSetOp = stack.version();
    Op replacement;
    replacement.pointKind = PointOpKind::Saturation;
    stack.setOp(idx, replacement);
    check(stack.version() == vBeforeSetOp + 1, "OpStack: setOp() increments version()");
    check(stack.at(idx).pointKind == PointOpKind::Saturation,
          "OpStack: setOp() actually replaces the entry's data");

    const size_t idx2 = stack.add(Op{});
    const uint64_t vBeforeReorder = stack.version();
    stack.reorder(0, static_cast<size_t>(idx2));
    check(stack.version() == vBeforeReorder + 1, "OpStack: reorder() increments version()");

    const size_t sizeBeforeRemove = stack.size();
    const uint64_t vBeforeRemove = stack.version();
    stack.remove(0);
    check(stack.version() == vBeforeRemove + 1, "OpStack: remove() increments version()");
    check(stack.size() == sizeBeforeRemove - 1, "OpStack: remove() actually shrinks the stack");
  }

  // --- an all-PointA stack collapses into one run, composed in order ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 0.0f;
    stack.add(exposureOp);
    stack.add(saturationOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1, "detectRuns: an all-PointA stack collapses into exactly one run");
    if (runs.size() == 1) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 2,
            "detectRuns: the one run spans the whole stack, {0,2}");
      check(runs[0].ops.size() == 2,
            "detectRuns: the run's composed op list has both entries");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> viaDirect =
          applySaturation(applyExposure(rgb, exposureOp.exposure), saturationOp.saturation);
      check(nearRgb(viaRun, viaDirect, kTol),
            "detectRuns: running a value through the composed run matches calling "
            "applyExposure() then applySaturation() directly, in that order");
    }
  }

  // --- a disabled PointA entry in the middle does not split the run ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op disabledSaturation;
    disabledSaturation.pointKind = PointOpKind::Saturation;
    disabledSaturation.saturation.scale = 0.0f;  // would collapse to luma if it ran
    disabledSaturation.enabled = false;
    Op channelMixerOp;
    channelMixerOp.pointKind = PointOpKind::ChannelMixer;
    channelMixerOp.channelMixer.matrix = {
        {{0.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}}};  // R<->B swap

    stack.add(exposureOp);
    stack.add(disabledSaturation);
    stack.add(channelMixerOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1, "detectRuns: a disabled PointA entry does not split the run");
    if (runs.size() == 1) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 3,
            "detectRuns: the run still spans all three indices, {0,3}, including the disabled "
            "entry's slot");
      check(runs[0].ops.size() == 2,
            "detectRuns: the run's composed op list has only the two enabled entries");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> viaSkippingDisabled =
          applyChannelMixer(applyExposure(rgb, exposureOp.exposure), channelMixerOp.channelMixer);
      check(nearRgb(viaRun, viaSkippingDisabled, kTol),
            "detectRuns: the composed run matches skipping the disabled saturation op entirely");
    }
  }

  // --- a real class boundary splits the stack into two runs ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    // Pure test fixture, per this module's own doc comment -- no real
    // class-B/C/D op exists anywhere in this codebase yet. Constructed only
    // to exercise detectRuns()'s run-splitting on a genuine class boundary.
    Op spatialPlaceholder;
    spatialPlaceholder.opClass = OpClass::SpatialB;
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 2.0f;

    stack.add(exposureOp);
    stack.add(spatialPlaceholder);
    stack.add(saturationOp);

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 2, "detectRuns: a non-PointA entry splits the stack into two runs");
    if (runs.size() == 2) {
      check(runs[0].startIndex == 0 && runs[0].endIndex == 1,
            "detectRuns: the first run is exactly the PointA entry before the boundary, {0,1}");
      check(runs[1].startIndex == 2 && runs[1].endIndex == 3,
            "detectRuns: the second run is exactly the PointA entry after the boundary, {2,3}");
      check(runs[0].ops.size() == 1 && runs[1].ops.size() == 1,
            "detectRuns: each run's composed op list has only its own side's op");

      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      check(nearRgb(runOps(runs[0].ops, rgb), applyExposure(rgb, exposureOp.exposure), kTol),
            "detectRuns: the first run's op is the exposure op");
      check(nearRgb(runOps(runs[1].ops, rgb), applySaturation(rgb, saturationOp.saturation), kTol),
            "detectRuns: the second run's op is the saturation op");
    }
  }

  // --- reorder(): order genuinely matters, and detectRuns() reflects it ---
  {
    // Exposure (a uniform scalar multiply) and Levels with gamma != 1 (a
    // nonlinear pow) do NOT commute -- unlike two purely linear ops (e.g.
    // Exposure+Saturation, which always agree regardless of order since
    // scalar multiplication commutes with any linear map -- see how the
    // all-PointA-stack case above deliberately avoided this trap), so this
    // pair genuinely exercises order.
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;  // x2

    Op levelsOp;
    levelsOp.pointKind = PointOpKind::Levels;
    LevelsParams lp;
    lp.gamma = 2.0f;
    levelsOp.levels = {lp, lp, lp};

    const std::array<float, 3> rgb{0.1f, 0.2f, 0.3f};
    const std::array<float, 3> exposureThenLevels =
        applyLevels(applyExposure(rgb, exposureOp.exposure), levelsOp.levels);
    const std::array<float, 3> levelsThenExposure =
        applyExposure(applyLevels(rgb, levelsOp.levels), exposureOp.exposure);
    check(!nearRgb(exposureThenLevels, levelsThenExposure, kTol),
          "reorder fixture sanity check: exposure-then-levels and levels-then-exposure "
          "genuinely disagree, so the reorder assertion below can't pass vacuously");

    OpStack stack;
    stack.add(exposureOp);  // index 0
    stack.add(levelsOp);    // index 1

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 1 && nearRgb(runOps(runs[0].ops, rgb), exposureThenLevels, kTol),
            "reorder: before reordering, the composed run matches exposure-then-levels");
    }

    const uint64_t versionBefore = stack.version();
    stack.reorder(1, 0);  // move Levels (index 1) to the front
    check(stack.version() == versionBefore + 1, "reorder(): increments version()");

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 1 && nearRgb(runOps(runs[0].ops, rgb), levelsThenExposure, kTol),
            "reorder: after moving Levels to index 0, the composed run matches "
            "levels-then-exposure");
    }
  }

  // --- remove(): run boundaries and the composed op list update correctly ---
  {
    OpStack stack;
    Op exposureOp;
    exposureOp.pointKind = PointOpKind::Exposure;
    exposureOp.exposure.stops = 1.0f;
    Op spatialPlaceholder;
    spatialPlaceholder.opClass = OpClass::SpatialB;  // test fixture only, see above
    Op saturationOp;
    saturationOp.pointKind = PointOpKind::Saturation;
    saturationOp.saturation.scale = 0.0f;

    stack.add(exposureOp);         // index 0, run A
    stack.add(spatialPlaceholder); // index 1, boundary
    stack.add(saturationOp);       // index 2, run B

    {
      const std::vector<OpRun> runs = stack.detectRuns();
      check(runs.size() == 2 && runs[0].startIndex == 0 && runs[0].endIndex == 1 &&
                runs[1].startIndex == 2 && runs[1].endIndex == 3,
            "remove fixture sanity check: two runs at the expected indices before removal");
    }

    // Remove the exposure entry at index 0. The placeholder shifts down to
    // index 0, saturation shifts down to index 1 -- and the removed
    // exposure op's effect is gone from the one remaining run.
    const uint64_t versionBefore = stack.version();
    stack.remove(0);
    check(stack.version() == versionBefore + 1, "remove(): increments version()");
    check(stack.size() == 2, "remove(): the stack shrinks by exactly one entry");

    const std::vector<OpRun> runs = stack.detectRuns();
    check(runs.size() == 1 && runs[0].startIndex == 1 && runs[0].endIndex == 2,
          "remove: the run after the removed entry shifts its indices down by one, {1,2}");

    if (runs.size() == 1) {
      const std::array<float, 3> rgb{0.2f, 0.6f, 0.9f};
      const std::array<float, 3> viaRun = runOps(runs[0].ops, rgb);
      const std::array<float, 3> saturationOnly = applySaturation(rgb, saturationOp.saturation);
      check(nearRgb(viaRun, saturationOnly, kTol),
            "remove: the composed op list no longer includes the removed exposure op's effect");
    }
  }

  std::printf("[selftest] op stack %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
