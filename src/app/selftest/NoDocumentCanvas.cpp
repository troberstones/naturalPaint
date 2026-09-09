#include "app/selftest/Support.hpp"

#include "app/NoDocumentCanvas.hpp"

namespace np {

namespace {

// Blocks until every command already submitted to `gpu.queue` has finished --
// the identical pattern `PaintSim::shutdown()` itself uses before releasing
// anything the queue might still be touching (sim/PaintSim.cpp's own comment
// on why: destroying a texture underneath unflushed work is a validation
// error that aborts the process). Used here for a second reason `shutdown()`
// does not need it for: docs/testing-issues.md T6 measured that a chunk of
// `phys_footprint` -- roughly 400 MB on this machine, in fixed 8 MiB
// allocations -- appears the INSTANT a process's first command buffer
// executes, regardless of what that buffer contains. `ensurePaintSim()`
// alone never submits anything, so a footprint sampled right after it
// returns is taken BEFORE that one-time cost has landed -- and
// `shutdown()`'s own flush-and-wait would then be what pays it, burying any
// real saving from releasing this sim's own textures under a driver cost
// that had nothing to do with them. Paying it here, before the "before"
// sample, is what isolates the measurement below to this sim's own bytes.
void waitForGpuIdle(GpuContext& gpu) {
  if (gpu.queue == nullptr || gpu.instance == nullptr) return;
  wgpuQueueSubmit(gpu.queue, 0, nullptr);
  struct DoneState { bool done = false; } state;
  WGPUQueueWorkDoneCallbackInfo ci = {};
  ci.mode = WGPUCallbackMode_AllowProcessEvents;
  ci.userdata1 = &state;
  ci.callback = [](WGPUQueueWorkDoneStatus, void* ud1, void*) {
    static_cast<DoneState*>(ud1)->done = true;
  };
  wgpuQueueOnSubmittedWorkDone(gpu.queue, ci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);
}

}  // namespace

// docs/testing-issues.md T5, reversed 2026-09-08. The short-term half that
// landed in 8140912 explicitly kept "painting the bare canvas is a supported
// workflow" as the one thing it must not break -- app/ToolSurface.hpp's own
// table listed Brush, Water and Dry Brush as surviving with no document open,
// through `strokeRouteFor(t, nullptr) == PaintSim`. This task reverses that:
// with zero open documents there is no canvas at all -- nothing drawn,
// nothing paintable, no simulation alive -- and `sim::PaintSim` is torn down
// the moment the last document closes rather than left standing for nobody.
//
// Two halves, pinned separately because they can drift independently: the
// CPU-side predicate the canvas block and `ensurePaintSim()`'s call site now
// gate on (`DocumentSession::empty()` -- there is no separate
// `canvasPresent()` wrapper, because that predicate already IS the pure one
// and a second name for the same fact is exactly the drift risk
// docs/testing-issues.md keeps warning about), and the GPU-side teardown
// actually giving the process's memory back rather than merely nulling a
// pointer.
//
// Owns a PRIVATE PaintSim, constructed the same way --selftest's shared one
// is (`ensurePaintSim()`), rather than borrowing the shared `*s` the
// sections around this one take by reference: this section's whole point is
// to shut one down, and every GPU section after this one in main.cpp's `ok`
// chain still needs the shared sim alive.
//
// Part B calls `np::releaseSolverWhenNoDocuments()` (app/NoDocumentCanvas.hpp)
// directly -- the exact function main.cpp's frame loop calls, not a
// look-alike built out of `PaintSim::shutdown()` and `unique_ptr::reset()`
// inline. An earlier version of this section called those two directly,
// which meant the frame loop's own hook could be sabotaged (e.g. turned into
// a no-op) with this section still green, because it was never actually
// calling the code under test. That gap is what B0/B1 below close, alongside
// the pre-existing GPU teardown measurement in B2, which now runs through
// the same function.
bool runNoDocumentCanvasTest(GpuContext& gpu, const MixboxLut& lut) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] no-document canvas: T5 reversed -- no document means no canvas, not a "
      "paintable one belonging to nobody\n");

  // -----------------------------------------------------------------------
  // A. The predicate the UI gates on: DocumentSession::empty()
  // -----------------------------------------------------------------------
  {
    DocumentSession session;
    check(session.empty(), "no canvas: a session with zero documents is empty()");

    session.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "probe"));
    check(!session.empty(), "canvas present: adding one document flips empty() false");

    std::string err;
    check(session.close(0, /*discardUnsavedChanges=*/false, &err),
          "closing the only (clean, blank) document succeeds");
    check(session.empty(), "no canvas again: closing the last document empties the session");
  }

  // -----------------------------------------------------------------------
  // B. The GPU-side teardown actually gives memory back
  // -----------------------------------------------------------------------
  {
    std::unique_ptr<PaintSim> localSim;
    // 1024x1024, matching the shared --selftest sim's own size: big enough
    // that PaintSim::kFieldBytesPerTexel (197 B/texel in Watercolour,
    // sim/PaintSim.hpp) prices this construction at roughly 206 MB, which
    // stays comfortably above the floor below even once the one-time driver
    // allocation `waitForGpuIdle()` isolates is no longer part of the delta.
    constexpr uint32_t kW = 1024, kH = 1024;
    PaintSim* s = ensurePaintSim(localSim, gpu, kW, kH, lut);
    check(s != nullptr, "a private PaintSim constructs via ensurePaintSim(), same call the "
                        "shared --selftest one uses");
    if (s) {
      // Pay the one-time "first command buffer" driver cost T6 measured
      // BEFORE sampling "before" -- see waitForGpuIdle()'s own comment. An
      // idle frame (brushActive == 0) is enough to force a real submission;
      // it deposits nothing and moves nothing.
      s->frame(gpu, SimParams{});
      waitForGpuIdle(gpu);

      // -----------------------------------------------------------------
      // B0. A document open: releaseSolverWhenNoDocuments() must refuse.
      // -----------------------------------------------------------------
      {
        DocumentSession oneDoc;
        oneDoc.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "probe"));
        const bool releasedWithDoc = releaseSolverWhenNoDocuments(localSim, oneDoc);
        check(!releasedWithDoc,
              "releaseSolverWhenNoDocuments() returns false with a document open");
        check(localSim != nullptr,
              "...and leaves the sim alive when it refuses");
      }

      // -----------------------------------------------------------------
      // B1. A null sim: must report false and must not crash.
      // -----------------------------------------------------------------
      {
        std::unique_ptr<PaintSim> nullSim;
        DocumentSession emptySession;
        const bool releasedNullSim = releaseSolverWhenNoDocuments(nullSim, emptySession);
        check(!releasedNullSim, "releaseSolverWhenNoDocuments() returns false on a null sim");
        check(nullSim == nullptr, "...and does not construct one");
      }

      // -----------------------------------------------------------------
      // B2. The GPU-side teardown actually gives memory back.
      // -----------------------------------------------------------------
      DocumentSession emptySession;
      const size_t beforeShutdown = currentFootprintBytes();
      const bool released = releaseSolverWhenNoDocuments(localSim, emptySession);
      const size_t afterShutdown = currentFootprintBytes();
      check(released,
            "releaseSolverWhenNoDocuments() returns true with an empty document session");
      std::printf("  [measured] process footprint before this sim's shutdown(): %.2f MiB\n",
                  static_cast<double>(beforeShutdown) / (1024.0 * 1024.0));
      std::printf("  [measured] process footprint after this sim's shutdown():  %.2f MiB\n",
                  static_cast<double>(afterShutdown) / (1024.0 * 1024.0));

      // The floor. `kFieldBytesPerTexel` prices this sim's own fields alone
      // (canvas_/paper_/selection_ included, sim/PaintSim.cpp's
      // releaseFields()) at ~51.6 MB; 8 MiB is a sixth of that, chosen to
      // stay well clear of whatever the allocator does not hand straight
      // back to the OS while still being far above noise -- a process
      // footprint drifts by kilobytes between two samples, not megabytes.
      constexpr size_t kFloorBytes = 8ull * 1024ull * 1024ull;
      const bool droppedPastFloor =
          beforeShutdown > afterShutdown && (beforeShutdown - afterShutdown) > kFloorBytes;
      check(droppedPastFloor,
            "shutdown() drops the process footprint past the 8 MiB floor -- it frees the "
            "textures rather than only nulling the pointer to them");
      check(localSim == nullptr,
            "reset() leaves the unique_ptr null -- the next ensurePaintSim() on it constructs "
            "a fresh sim rather than reusing a shut-down one");
    }
  }

  std::printf("[selftest] no-document canvas %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
