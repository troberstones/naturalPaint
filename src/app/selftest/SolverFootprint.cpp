#include "app/selftest/Support.hpp"

#include "app/DocumentPresets.hpp"  // kMaxDocumentPresetDimension
#include "app/Memory.hpp"           // the two quantities, and why there are two
#include "app/ZoomAndSize.hpp"      // paintSimDimensionsFor(), kPaintSimMaxTexels
#include "ui/AtelierChrome.hpp"     // kResidentBudgetBytes, the 512 MB denominator

namespace np {

// docs/testing-issues.md **T6** -- "500+ MB at startup, against a documented
// <100 MB".
//
// T6's own history is why this section is shaped the way it is. The entry has
// been wrong twice, in two different directions, and both times because a
// number that was *derived* was reported as if it had been *measured*:
//
//   * T7 (closed) explained T6's 482 MB by counting PaintSim's textures in a
//     header -- 13 ping-pongs at 1024x1024 -- without checking whether the
//     object existed. It did not; the solver is lazily constructed and has
//     been all along.
//   * `app/ZoomAndSize.hpp`'s own budget rationale then quoted "176 bytes per
//     texel, 272 with ink". Measured 2026-09-02 against
//     `MTLDevice.currentAllocatedSize`, the real figures are **197 and 293**:
//     the 176 counted the seven ping-pong fields and silently omitted the
//     five singles -- `paper_`, `selection_`, `canvas_`, `grayscale_` and
//     `graded_` -- which together are another 21 B/texel. Every "3.3 GB" and
//     "292 GB" in `app/selftest/CanvasDimensions.cpp`'s assertion prose was
//     12% low for the same reason.
//
// So the property this section pins is not "the solver is small". It is
// **that the documented cost and the real cost cannot drift apart again**:
// `PaintSim::fieldTextureBytes()` interrogates the live textures, the
// `k*BytesPerTexel` constants are what the budget check reasons from, and the
// first two assertions below hold one against the other. A future field added
// to `allocFields()` changes the first and not the second, and fails here.
//
// The third property is the one the brief for this track cared about most:
// **the solver's size is a function of the document, never of the window.**
// A simulation that got more expensive when the user maximised their window
// would be a genuine defect, and it is worth an assertion even though the
// answer today is "it already is document-sized" -- `paintSimDimensionsFor()`
// takes no display, window or zoom argument at all, and the check below is
// that the bytes follow from the solver's own texel count and nothing else.
//
// **What this section deliberately does NOT claim.** It says nothing about
// the ~400 MB of `IOAccelerator (graphics)` that dominates what Activity
// Monitor shows for the windowed process. Measured 2026-09-02, that is 48
// allocations of exactly 8 MiB that appear when the first command buffer
// executes on the GPU, and `MTLDevice.currentAllocatedSize` is 34.8 MB at
// that instant -- so none of it is a texture or buffer anybody asked for, it
// is invariant across a 36x change in window area, and there is nothing in
// this process's own allocation for an assertion to hold it against. T6 has
// the full measurement. This section covers the part that IS ours.
//
// Needs a live solver (`fieldTextureBytes()` asks real textures), so it takes
// the same `PaintSim&` `runFieldAllocationTest()` does and reads its actual
// `width()`/`height()` rather than assuming the 1024x1024 that `--selftest`
// happens to construct.
bool runSolverFootprintTest(GpuContext& gpu, PaintSim& sim) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] solver footprint: the field set's real byte cost, and that it "
              "follows the document rather than the window\n");

  const uint64_t texels = static_cast<uint64_t>(sim.width()) * sim.height();
  check(texels > 0, "the solver under test has a non-zero texel count");

  // --- 1. the live cost equals the documented cost -------------------------
  //
  // Watercolour, freshly constructed: the base field set only. If Ink or Oil
  // fields are somehow live here the arithmetic below would be checking a
  // different question, so establish that first rather than assume it.
  const bool baseOnly = !sim.inkFieldsAllocated() && !sim.oilFieldsAllocated();
  check(baseOnly, "the sim under test holds the base field set only (no ink, no oil)");

  const uint64_t base = sim.fieldTextureBytes();
  const uint64_t expectedBase = PaintSim::kFieldBytesPerTexel * texels;
  check(baseOnly && base == expectedBase,
        "the live field textures cost exactly kFieldBytesPerTexel (197) each");

  // --- 2. ink's increment is exactly what the budget assumes ---------------
  //
  // Measured 2026-09-02: allocInkFields() moves the Metal device's total
  // allocation by 96.00 MiB at 1024x1024, to the byte. Three ping-pongs of
  // RGBA32Float is 2 x 3 x 16.
  sim.setMode(gpu, PaintMode::Ink);
  const uint64_t withInk = sim.fieldTextureBytes();
  check(sim.inkFieldsAllocated() &&
            withInk - base == PaintSim::kInkFieldBytesPerTexel * texels,
        "switching to Ink adds exactly kInkFieldBytesPerTexel (96) each");

  // Oil's brush grid is kBrushGrid x kBrushGrid, NOT canvas-sized, and the
  // per-texel constants deliberately exclude it. Assert that -- if oil ever
  // becomes canvas-sized the constants above stop describing the worst case
  // and the budget check silently under-counts.
  sim.setMode(gpu, PaintMode::Oil);
  const uint64_t withOil = sim.fieldTextureBytes();
  check(sim.oilFieldsAllocated() && withOil - base < 4ull * 1024 * 1024,
        "Oil's brush grid is a fixed 64x64 set, not canvas-sized (< 4 MiB)");

  sim.setMode(gpu, PaintMode::Watercolor);
  check(sim.fieldTextureBytes() == base,
        "returning to Watercolour frees both optional sets back to the base");

  // --- 3. the cost follows the DOCUMENT, and nothing else -----------------
  //
  // `paintSimDimensionsFor()` has no window, display, zoom or DPI parameter,
  // so the strongest runtime statement available is that the byte cost is a
  // pure function of the texel count that function returns. Two documents of
  // the same area cost the same; the fallback the caller passes only matters
  // when the document is over budget.
  {
    OpenDocument portrait = makeBlankOpenDocument(800, 1200, WorkingSpace{}, "portrait");
    // Two wildly different "window" fallbacks. An in-budget document must
    // ignore both -- the same solver either way, because the window is not an
    // input to this question.
    const CanvasDimensions tiny = paintSimDimensionsFor(&portrait, 320, 200);
    const CanvasDimensions huge = paintSimDimensionsFor(&portrait, 5120, 2880);
    check(tiny.w == huge.w && tiny.h == huge.h && tiny.w == 800.0f && tiny.h == 1200.0f,
          "an in-budget document sizes the solver identically for any window");
  }

  // --- 4. no reachable document can put the solver over the 512 MB budget --
  //
  // This is the assertion the corrected per-texel figure exists for. The cap
  // is expressed in texels (`kPaintSimMaxTexels`) and the budget in bytes
  // (`kResidentBudgetBytes`), so nothing connects them unless something like
  // this does -- raising the texel cap without re-measuring the byte cost is
  // exactly the change that would sail past every other section in the suite.
  //
  // Worst case is base + ink: 197 + 96 = 293 B/texel. At the cap that is
  // 293 MiB, 60% of the 512 MB budget -- which is why the cap is not a round
  // number and should not become one without a measurement.
  {
    const uint64_t worstPerTexel =
        PaintSim::kFieldBytesPerTexel + PaintSim::kInkFieldBytesPerTexel;
    const uint64_t atCap = worstPerTexel * kPaintSimMaxTexels;
    check(atCap < kResidentBudgetBytes,
          "base + ink at kPaintSimMaxTexels stays inside the 512 MB budget");

    // Sweep the sizes a user can actually reach, including the preset maximum
    // that would otherwise ask the driver for hundreds of gigabytes.
    const int32_t dims[][2] = {{1, 1},         {800, 1200},   {1024, 1024},
                               {1025, 1024},   {4000, 3000},  {8192, 8192},
                               {kMaxDocumentPresetDimension, kMaxDocumentPresetDimension}};
    bool allInside = true;
    for (const auto& d : dims) {
      OpenDocument doc = makeBlankOpenDocument(d[0], d[1], WorkingSpace{}, "sweep");
      const CanvasDimensions c = paintSimDimensionsFor(&doc, 1024, 1024);
      const uint64_t t = static_cast<uint64_t>(c.w) * static_cast<uint64_t>(c.h);
      if (worstPerTexel * t >= kResidentBudgetBytes) allInside = false;
    }
    check(allInside,
          "every document size up to the 32768 preset maximum stays in budget");
  }

  // --- 5. the two memory quantities, and that they are not the same one ----
  //
  // `app/Memory.hpp` explains at length why both exist. The assertion is that
  // they genuinely differ -- a build where they returned the same number would
  // mean `currentFootprintBytes()` had silently degraded to the RSS the status
  // bar already shows, which is precisely the confusion T6 is about.
  //
  // **The inequality below is local to this point in the run, not a law**, and
  // saying so is the whole reason it is written out rather than assumed. By
  // here a WebGPU device exists and has submitted work, so the driver's
  // ledger charges dominate and the footprint is several times the RSS
  // (measured 2026-09-02: 143.7 MB resident against 622.3 MB footprint). At
  // the idle capture in `main.cpp`, before the GPU has executed anything, the
  // ordering is the OTHER WAY -- 91.2 MB resident against 38.5 MB footprint,
  // because phys_footprint does not charge for the clean file-backed pages of
  // the OpenImageIO dylib chain that resident_size counts. An assertion
  // written as "the footprint is always the larger" would be false there, and
  // was, until this run printed both numbers side by side.
  //
  // Note the direction this can fail in. It is NOT a budget: no ceiling is
  // asserted here, and the printed footprint below is emphatically not an
  // answer to T6. `--selftest` is not the headless process T6's early text
  // called it -- `main()` gives it a real SDL window and a real WebGPU
  // adapter (it prints `[gpu] adapter: Apple M4 Max`), so it carries the same
  // ~400 MB driver arena the windowed application does, plus the ~198 MB
  // solver it constructs eagerly and the GUI never does. It simply never runs
  // an ImGui frame. Measured 2026-09-02: 48 IOAccelerator regions of exactly
  // 8 MiB in this process, the same 48 the windowed one has.
  {
    const size_t rss = currentResidentBytes();
    const size_t fp = currentFootprintBytes();
    check(rss > 0, "currentResidentBytes() reports a real reading");
    check(fp > 0, "currentFootprintBytes() reports a real reading");
    check(fp > rss,
          "with a live GPU device the footprint dominates the RSS (not a law)");
    std::printf("  [measured] resident %.1f MB, phys_footprint %.1f MB -- neither is a "
                "budget; the gap is the graphics driver's, not this build's (T6)\n",
                rss / (1024.0 * 1024.0), fp / (1024.0 * 1024.0));
  }

  std::printf("[selftest] solver footprint %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
