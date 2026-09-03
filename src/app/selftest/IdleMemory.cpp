#include "app/selftest/Support.hpp"

namespace np {

// 1.4 / ADR-0001 bullet 5. The 40 MB ceiling is PLAN.md's phase-1 exit
// criterion; it has headroom above SDL/window/WebGPU-device baseline
// footprint (per ADR-0001's own amendment, this ceiling needs slack for
// allocator and driver-version noise, not a tight bound). idleRssBytes == 0
// means task_info() itself failed, which is a measurement failure, not a
// pass.
//
// ==========================================================================
// WHAT THIS CEILING CANNOT SEE, AND WHY THAT IS NOT A BUG IN IT
// ==========================================================================
//
// docs/testing-issues.md **T6** is the report "naturalPaint shows 500+ MB of
// RAM on open; it was supposed to be under 100 MB", and this assertion is the
// "<100 MB". Both figures are correct and they are different quantities, so
// state the boundary here rather than leave a future reader to rediscover it
// from a green suite:
//
//   * `idleRssBytes` is `currentResidentBytes()` -- MACH_TASK_BASIC_INFO's
//     `resident_size`, this task's resident pages, what `ps -o rss=` prints.
//   * Activity Monitor's "Memory" column is `phys_footprint`, a kernel ledger
//     that additionally charges this process for the IOAccelerator /
//     IOSurface regions the graphics driver maps in on its behalf. Those are
//     SM=SHM in `vmmap` and are not this task's resident pages at all, so no
//     ceiling on `resident_size` can ever bound them.
//
// Neither number contains the other, and the OpenImageIO allowance below is
// the cleanest illustration of the direction people forget. Those 29.5 MB are
// clean file-backed dylib pages: `resident_size` counts them -- which is the
// entire reason this ceiling had to grow by 32 MB -- and `phys_footprint`
// does not charge for them at all. Measured at this very capture point,
// 2026-09-02: resident 91.2 MB against a footprint of 38.5 MB. Later in the
// same process, once the GPU has submitted work, it is 143.7 MB against
// 622.3 MB. Do not assume an ordering.
//
// Measured on this machine, 2026-09-02, idle windowed launch, no document:
// `resident_size` 143.5 MB against `phys_footprint` 551 MB, of which 401 MB
// is "IOAccelerator (graphics)" -- 48 allocations of exactly 8 MiB that
// appear the moment the GPU executes its first command buffer, while
// `MTLDevice.currentAllocatedSize` (every Metal resource this build, wgpu,
// Dear ImGui and SDL have between them created) reads 34.8 MB. So it is not
// a texture, not a buffer, not the swapchain -- it is invariant across a 36x
// change in window area -- and it is not something a ceiling on this
// application's own allocation should be asked to catch.
//
// **The ceiling below is therefore deliberately left where it is.** It bounds
// the thing it can bound, and it bounds it honestly. What was missing was
// anybody saying so, which is what this comment and the second printed line
// below are for: the footprint is printed beside the RSS so the two numbers
// appear together, and it is printed rather than asserted because there is no
// budget it could be held against that would mean anything.
//
// `idleFootprintBytes` is captured at the same instant as `idleRssBytes` (see
// main.cpp), before any branch has constructed a PaintSim.
bool runIdleMemoryTest(size_t idleRssBytes, size_t idleFootprintBytes) {
  // 80 MB, not the original 40: measured on 2026-08-18 that SDL3's own video
  // subsystem init (window server connection, display enumeration,
  // keyboard/mouse/pen setup — see SDL_VideoInit()) costs ~57 MB before any
  // of this project's code runs, and WebGPU/Metal device creation adds only
  // ~5 MB more on top. 40 MB was never checked against that baseline. See
  // PLAN.md's Findings for the full breakdown; PRD.md A1 revised to match.
  constexpr size_t kIdleRssCeilingBytes = 80ull * 1024 * 1024;

  // --- The NP_USE_OIIO allowance, stated out loud rather than folded in ---
  //
  // **The 80 MB core ceiling above is unchanged and still binds the
  // dependency-free build.** What follows is a separate, additive,
  // separately-printed allowance that applies only to the NP_USE_OIIO=ON
  // configuration, and it exists because that configuration genuinely does
  // not fit under 80 MB. Measured, on the same machine, in the same session:
  //
  //   NP_USE_OIIO=OFF  idle RSS  63.3 MB   (under the 80 MB ceiling)
  //   NP_USE_OIIO=ON   idle RSS  91.8 MB   (over it, by 11.8 MB)
  //
  // The 28.5 MB difference was isolated to dyld, not to anything this code
  // does, with a standalone two-line program that reads RSS as its first
  // statement: linking nothing costs 1.03 MB resident, linking
  // libOpenImageIO + libOpenImageIO_Util (and their transitive OpenEXR,
  // Imath, OpenColorIO, libtiff, libpng, libjpeg-turbo and giflib
  // dependencies) costs 30.56 MB -- 29.5 MB paid before main() runs a line.
  // The same program then makes its first OpenImageIO call and RSS moves by
  // 0.02 MB, which independently confirms two things: io/Capabilities' lazy,
  // cached probe costs essentially nothing, and there is no eager
  // initialisation left to defer.
  //
  // That last point matters for what this allowance is NOT. PLAN.md step 6
  // ("Lazy OIIO init -- on first file open, not at startup, so PRD A2
  // holds") will not recover this: OpenImageIO's own initialisation is
  // already lazy and already free. The 29.5 MB is the dynamic loader mapping
  // and relocating the libraries, which only dlopen()-ing OpenImageIO on
  // first use could defer -- a different linkage architecture, not a tuning
  // change, and not in Phase 4 step 2/3's scope.
  //
  // So: 32 MB, slightly above the measured 29.5 MB so a future OpenImageIO
  // point release does not turn this into a flake, and deliberately not
  // "whatever makes 91.8 pass". If the OIIO build's idle RSS ever needs more
  // than this, that is a real regression and should fail here rather than
  // being accommodated again.
  constexpr size_t kOiioDylibAllowanceBytes = 32ull * 1024 * 1024;
  const bool oiio = oiioBackendCompiledIn();
  const size_t ceiling = kIdleRssCeilingBytes + (oiio ? kOiioDylibAllowanceBytes : 0);

  const double mb = static_cast<double>(idleRssBytes) / (1024.0 * 1024.0);
  const bool ok = idleRssBytes > 0 && idleRssBytes < ceiling;
  if (oiio) {
    // Printed as a sum, never as one number, so the allowance can never
    // read as if the core budget had quietly grown.
    std::printf("[selftest] idle RSS %.1f MB (ceiling 80 MB core + 32 MB OpenImageIO dylib "
                "allowance = %.0f MB; the OpenImageIO dylib chain costs 29.5 MB at load, "
                "measured) %s\n",
                mb, static_cast<double>(ceiling) / (1024.0 * 1024.0), ok ? "pass" : "FAIL");
  } else {
    std::printf("[selftest] idle RSS %.1f MB (ceiling 80 MB) %s\n", mb, ok ? "pass" : "FAIL");
  }
  // Printed, never asserted -- see the boundary note at the top of this file.
  // The point of putting it here is that the next person to read "idle RSS
  // 92.6 MB (ceiling 80 MB + 32 MB)" and then look at Activity Monitor sees
  // the reconciliation on the adjacent line instead of filing T6 again.
  std::printf("[selftest] idle phys_footprint %.1f MB (NOT a budget: this is the number "
              "Activity Monitor shows, and the gap above it is the graphics driver's, "
              "not this build's -- docs/testing-issues.md T6)\n",
              static_cast<double>(idleFootprintBytes) / (1024.0 * 1024.0));
  return ok;
}


}  // namespace np
