#pragma once

#include <cstddef>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

// core/Parallel -- the entire threading layer (docs/architecture-review.md
// **P0-3**: "one std::thread in the whole tree, and it is in a selftest.
// Application code: none... The machine has 16 cores. The app uses 1.").
//
// This header is the whole fix. Everything downstream of it -- ops/Blur.cpp,
// ops/Filters.cpp, and whatever P0-4's tile-major composite eventually does
// -- is a nested `ty`/`tx` loop over TileCoord flattened to one linear index
// and handed to `parallelFor()` below. The loop body does not change; only
// who runs it does.
//
// --- Why dispatch_apply and not a dependency ------------------------------
//
// P0-3 is explicit that this must not cost a dependency: no TBB, no OpenMP,
// no std::execution (libc++ on macOS does not ship a parallel STL backend at
// all -- `std::execution::par` silently degrades to serial, which would be
// worse than not trying: a call site that *looks* parallel and isn't).
// `dispatch_apply` is in `<dispatch/dispatch.h>`, part of libSystem, already
// linked into every macOS process including this one -- it costs a #include,
// nothing else. The cost, stated rather than hidden: this file only threads
// the Apple build. Every other platform gets the `#else` branch below, which
// is a plain serial loop -- correct, just not parallel. That is an
// intentional trade, not an oversight: naturalPaint ships on macOS today
// (docs/architecture-review.md's own "16 cores" is this machine's), and a
// second backend (e.g. a raw std::thread pool for Linux/Windows) is more
// code to keep thread-safe for a platform nothing here currently targets.
// The `#if defined(__APPLE__)` guard is exactly the seam a future port would
// widen.
//
// --- The one contract this header does NOT enforce -------------------------
//
// `body` is called once per index in `[0, n)`, from however many worker
// threads `dispatch_apply` decides to use (`DISPATCH_APPLY_AUTO`), with no
// ordering guarantee and no synchronization between calls. That makes
// `parallelFor` exactly as safe as the body handed to it, and no safer.
// **This codebase's one real hazard living behind that contract is
// `TileStoreOf::getOrCreate` (core/TileStore.hpp)**: it mutates a bare
// `std::unordered_map` and is not safe to call from two iterations at once
// (two threads racing to insert would corrupt the map's bucket chains, not
// merely overwrite each other's tile). The pattern every call site below
// uses is two phases: a short serial loop that reserves every destination
// tile the ROI touches (so every `getOrCreate()`/insertion happens on one
// thread, before any parallel work starts), followed by a `parallelFor` over
// the now-stable slots that only reads/writes pixels through the pointers
// the reservation phase already resolved -- never calling `getOrCreate`
// again inside the parallel body. **That hazard is not theoretical, and it
// was proven rather than argued**: moving `getOrCreate` from ops/Blur.cpp's
// reservation loop into its parallel body does not produce a few wrong
// pixels -- `--selftest` wedges, mid-`printf`, inside the first filter
// section that reaches the sabotaged call, which is what a thread walking a
// bucket chain another thread is rewriting looks like from the outside. A
// defect of that shape would not have shown up as a red assertion; it would
// have shown up as an application that occasionally stopped responding while
// blurring. See core/TileStore.hpp's own comment
// (search "the count lives in the shared_ptr's control block") for why the
// pointers handed out by that reservation phase stay valid even if the map
// rehashes afterward: `TileStoreOf::Slot` is a `std::shared_ptr<T>`, so
// `getOrCreate()`'s returned `T&` addresses the heap block the `shared_ptr`
// owns, not a map node -- an `unordered_map` rehash moves buckets and
// `shared_ptr` copies between them, never the pointee. (The general
// standard-library guarantee is narrower than that and would already be
// enough on its own: `unordered_map` promises references/pointers to
// existing elements survive insertion, and only erasure invalidates them --
// nothing on any path here erases a tile mid-op. The `shared_ptr` indirection
// is the belt to that guarantee's suspenders.) Reads of a *different*,
// read-only store (`TileStore::find()`, `const`) are safe to call
// concurrently from every worker for the same reason ordinary concurrent
// reads of an unmodified `unordered_map` are safe -- nothing in this file
// enforces that either; it falls out of no call site mutating the store
// being read.
//
// --- The grain, measured rather than guessed -------------------------------
//
// Below `grain` items, `parallelFor` just runs the loop serially. This
// matters because `dispatch_apply` is not free for a tiny `n`: crossing into
// the thread pool, even on Apple's implementation, costs enough that a
// two-tile blur (a stroke's dirty-tile band, the common case while painting)
// would pay dispatch overhead for no win.
//
// Measured, not assumed -- docs/architecture-review.md's own snippet only
// ventured "start around 8 tiles" as a guess, and this header's job is to
// replace that guess with a number. Two throwaway benchmarks (not checked
// into the tree; same shape as this file's real call sites) on the machine
// this was built on (Apple Silicon, 16 logical cores):
//
//   1. Pure dispatch_apply overhead, empty task body, `n` from 1 to 64:
//      unmeasurably small -- 0.01-0.06 microseconds per task including the
//      dispatch call itself. libdispatch's worker pool is already warm by
//      the time any of this code runs (SDL/ImGui/wgpu have all started
//      threads of their own), so there is no cold-start tax to amortize.
//   2. A cheap per-tile workload -- one arithmetic pass over a tile's
//      128*128*4 floats with no convolution, i.e. the FLOOR of what a real
//      filter body costs per tile (scatterAligned's combine lambda is this
//      shape; a real blur's convolveLine/boxLine passes cost more per tile,
//      not less) -- serial vs. dispatch_apply from n=1 to n=64. Serial cost
//      per tile was ~6.2 microseconds. Parallel broke even (speedup crossing
//      1.0x) at n=3-4 and reached ~2x by n=10-12.
//
// So the true crossover for the cheapest realistic tile body on this machine
// is 3-4 tiles, and every real op in this file (which does a convolution, a
// gather, or both) crosses over sooner than that. **kParallelForDefaultGrain
// is set to 8** -- roughly 2x the measured floor -- rather than the measured
// number itself, because a tight, otherwise-idle microbenchmark understates
// real conditions: thread-wake scheduling jitter under whatever else the
// process is doing (SDL's event pump, wgpu-native's own queues, a second
// filter already in flight) is not something a 9-repetitions-take-the-best
// loop captures, and this header would rather lose a fraction of a
// millisecond on an 8-tile ROI than have a stroke's small dirty-tile band
// occasionally pay dispatch overhead for zero win because the pool was busy
// when it asked. 8 also lines up with docs/architecture-review.md's own
// starting guess, which is a coincidence worth noting rather than a
// justification on its own -- the number above is the one that was actually
// measured.
//
// The measured numbers for this build's real ops (blur and one filter, at
// 1024^2 and 2048^2, serial vs. parallel) are printed by
// `app/selftest/Parallel.cpp`'s `[measured]` lines rather than duplicated
// here, so they can't drift out of sync with what the binary actually does.
namespace np {

inline constexpr size_t kParallelForDefaultGrain = 8;

// Runs `body(i)` for every `i` in `[0, n)`. Serial (a plain `for` loop, in
// index order) when `n < grain`; on Apple platforms, and only when `n >=
// grain`, dispatched across `libdispatch`'s worker pool via
// `dispatch_apply()`, which blocks the calling thread until every index has
// run -- so by the time `parallelFor` returns, all of `body`'s side effects
// have happened and are visible to the caller, exactly as a serial loop
// would leave them. There is no partial/async form: a caller that wanted to
// overlap `parallelFor` with other work would need a different primitive,
// and none of this codebase's call sites want that (every one is "parallel:
// then use the result").
//
// `body` must tolerate being called from multiple threads concurrently, with
// no ordering between calls and no synchronization supplied by this
// function -- see the file comment above for what that means for a body
// that touches a `TileStoreOf`. `body` is captured by reference into the
// dispatched block (`__block F&`, not a copy), which is sound only because
// the call is synchronous: `body` is guaranteed still alive for every
// invocation, since `parallelFor` itself has not returned yet.
template <class F>
void parallelFor(size_t n, size_t grain, F&& body) {
  if (n == 0) return;
  if (n < grain) {
    for (size_t i = 0; i < n; ++i) body(i);
    return;
  }
#if defined(__APPLE__)
  __block F& f = body;
  dispatch_apply(n, DISPATCH_APPLY_AUTO, ^(size_t i) {
    f(i);
  });
#else
  // Serial fallback (see the file comment on why this header only threads
  // the Apple build): correct, just not parallel. A caller cannot tell the
  // difference from `body`'s point of view -- indices still run exactly
  // once each, 0..n-1 -- only wall-clock time differs.
  for (size_t i = 0; i < n; ++i) body(i);
#endif
}

}  // namespace np
