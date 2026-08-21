#include "app/selftest/Support.hpp"

namespace np {

// 1.4 / ADR-0001 bullets 2 and 3. Not a tautology: allocFields()
// unconditionally builds the shared water/pigment/deposit set (needed by all
// three media), but allocInkFields()/allocOilFields() are only ever reached
// from setMode(Ink)/setMode(Oil) — a sim fresh out of init() has never taken
// either path, so the first half checks that fact holds rather than
// assuming it. The second half exercises setMode() across all three media
// and checks the *outgoing* medium's fields actually get freed, not just
// the incoming one's allocated — this is what makes ADR-0001's per-mode
// residency table (watercolour/ink/oil each quoted independently) true of a
// session that visits more than one medium, rather than only of a session
// that visits exactly one.
bool runFieldAllocationTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  check(!sim.inkFieldsAllocated(),
        "ink lattice absent -- no ink content has ever been painted");
  check(!sim.oilFieldsAllocated(),
        "oil brush grid absent -- no oil content has ever been painted");

  sim.setMode(gpu, PaintMode::Ink);
  check(sim.inkFieldsAllocated(), "ink lattice allocated after switching to Ink");
  sim.setMode(gpu, PaintMode::Oil);
  check(!sim.inkFieldsAllocated(), "ink lattice freed after switching away to Oil");
  check(sim.oilFieldsAllocated(), "oil brush grid allocated after switching to Oil");
  sim.setMode(gpu, PaintMode::Watercolor);
  check(!sim.oilFieldsAllocated(),
        "oil brush grid freed after switching away to Watercolour");
  check(!sim.inkFieldsAllocated(), "ink lattice still absent (never revisited)");

  std::printf("[selftest] field allocation %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
