#pragma once
#include <cstddef>

namespace np {

// Current resident set size, in bytes. On macOS, backed by task_info() +
// MACH_TASK_BASIC_INFO, which reports the task's *current* resident_size.
// Deliberately not getrusage()'s ru_maxrss: that field is the process's peak
// RSS, monotonically non-decreasing for the life of the process, so it can
// never observe memory being freed (a mode switch releasing ink/oil fields,
// PaintSim not existing yet) -- exactly the things 1.4 / ADR-0001 need to
// measure. On Linux, backed by /proc/self/statm's resident-pages field,
// which is the same quantity by the same definition (see Memory.cpp's
// `#else` branch). Returns 0 on failure, on either platform.
size_t currentResidentBytes();

// Current *phys_footprint*, in bytes -- the number Activity Monitor prints in
// its "Memory" column and the number `footprint(1)` totals. Backed by
// task_info() + TASK_VM_INFO. Returns 0 if task_info() fails or if the kernel
// is too old to carry the field (it needs TASK_VM_INFO_REV1_COUNT).
//
// Linux has no equivalent kernel ledger -- no /proc field both adds GPU
// driver charges the way phys_footprint's IOAccelerator/IOSurface line does
// and omits clean file-backed pages the way phys_footprint does. On Linux
// this is approximated as RssAnon + RssShmem from /proc/self/status: the
// resident memory that is not a clean file-backed mapping, which mirrors
// the *omission* half above (excluding the shared-library pages the
// OpenImageIO allowance below is about) but not the *addition* half -- it
// cannot see any GPU driver memory, so a widening gap between this and
// currentResidentBytes() does not mean on Linux what it means on macOS
// below. Returns 0 on failure there too.
//
// WHY BOTH EXIST, because getting this wrong is what T6 in
// docs/testing-issues.md is about. resident_size above and phys_footprint here
// are different quantities, and -- this is the part that is easy to get wrong
// -- **neither one contains the other**:
//
//   * resident_size counts the task's resident pages, including clean
//     file-backed ones: the dynamic loader's mappings of every dylib the
//     process links. It is what `ps -o rss=` prints.
//   * phys_footprint is a *ledger* of what the kernel charges this process
//     for. It ADDS memory resident_size never sees -- most importantly the
//     IOAccelerator / IOSurface regions the graphics driver maps in on our
//     behalf, which are SM=SHM in vmmap and are not this task's resident
//     pages at all -- and it OMITS clean file-backed pages, on the grounds
//     that the system can evict them at no cost to anyone.
//
// Both directions are observable in this build, on this machine, 2026-09-02:
//
//   at the idle capture in main.cpp, before the GPU has executed anything:
//       resident 91.2 MB  >  footprint 38.5 MB
//       (the gap is largely the OpenImageIO dylib chain -- 29.5 MB of clean
//        mapped pages, the very cost that forced --selftest's idle ceiling up
//        by 32 MB, and which the footprint does not charge us for at all)
//
//   the same process later, once a WebGPU device has submitted work:
//       resident 143.7 MB  <  footprint 622.3 MB
//
//   an idle windowed launch with no document:
//       resident 143.5 MB, footprint 551 MB, of which 401 MB is
//       "IOAccelerator (graphics)"
//
// So a budget expressed against resident_size is structurally incapable of
// noticing GPU allocation, which is exactly how a green <100 MB assertion and
// a user's 500+ MB Activity Monitor reading were both correct at the same
// time. And a budget expressed against phys_footprint would be structurally
// incapable of noticing a dependency being linked in.
//
// Rule of thumb for which to use: resident_size to measure *our own* heap --
// document tiles, history, an ink field allocated or freed -- because it is
// the quantity those things actually move. phys_footprint whenever the claim
// is about what the user will see. Never assume an ordering between them.
size_t currentFootprintBytes();

// --- Idle dependency-chain accounting (Linux only; see IdleMemory.cpp) ---
//
// app/selftest/IdleMemory.cpp's ceiling on macOS is 80 MB core plus a single
// constant (29.5 MB, measured once with a standalone probe) for the
// OpenImageIO dylib chain, because Apple's OpenImageIO pulls in a couple of
// dozen dylibs and that number is stable across point releases. Neither half
// of that holds on Linux: Ubuntu's libopenimageio-dev links OpenImageIO
// against GDAL, which alone pulls in on the order of 250 further shared
// libraries (curl, Kerberos, PostgreSQL, HDF5, netCDF, GEOS, ...) whose
// total resident size is a property of *this distribution's package graph*,
// not of this build -- a constant would go stale the next time any of those
// two hundred fifty packages does. And Mesa's software Vulkan device
// (lavapipe) costs on the order of 80 MB resident on its own -- LLVM's JIT
// backend, the loader, and every ICD the loader probes while enumerating
// physical devices, none of which exists in the macOS/Metal build at all.
//
// So both allowances are measured, not guessed: this struct is a sum of
// resident bytes across this process's own memory mappings (/proc/self/
// smaps), bucketed by what mapped each chain in, computed once and cached.
//
// The "once" matters as much as the "measured": IdleMemory.cpp's assertion
// is not checked until deep in --selftest's sequence -- after PaintSim
// exists, after strokes have been drawn, after files have round-tripped
// through OpenImageIO -- while `idleRssBytes` (main.cpp) is captured right
// after gpu.init(), before any of that. A dependency-chain snapshot taken
// when IdleMemory.cpp's check actually runs would be measuring a process
// that has since done real GPU and file-I/O work (confirmed: idle capture
// ~208 MB resident with these libraries alone accounting for ~144 MB of it;
// the same process mid-suite carries 200+ MB of Vulkan-allocated buffer
// memory on top, none of which existed yet at the idle instant). main.cpp's
// capture point cannot be moved (out of scope here; see the Linux build
// report), so instead this is captured as a side effect of the FIRST call
// to currentResidentBytes() in the process's life -- which main.cpp's own
// idle-RSS line already makes, before anything else does -- and cached for
// idleDependencyChainBytes() to hand back later. If that ordering assumption
// is ever violated (something starts calling currentResidentBytes() earlier
// than main.cpp's idle line), this degrades to measuring whatever the new
// first caller sees, which is either the same instant or an even calmer one
// -- it cannot silently start measuring a LATER, noisier point without that
// new call site being an active regression in its own right.
//
// macOS does not need this: its own version of "measured, not guessed" for
// this ceiling was the standalone two-line probe program described in
// IdleMemory.cpp, run once by hand, not at test time -- so this struct is
// always zero-valued there.
struct IdleDependencyChainBytes {
  // Sum of resident bytes for the Vulkan loader, every ICD it probed while
  // enumerating devices (only one of which -- lvp, lavapipe -- is ever
  // actually selected), LLVM's JIT backend (lavapipe's rasterizer is
  // LLVM-generated machine code), and Mesa's Gallium/EGL/GLX glue that comes
  // along with any of that being loaded at all.
  size_t vulkanDriverBytes = 0;
  // Sum of resident bytes for every OTHER shared library beyond the core
  // runtime (libc/libstdc++/SDL/X11/the executable itself) -- on this
  // distribution, OpenImageIO's own transitive dependency graph, GDAL and
  // everything GDAL pulls in among them.
  size_t oiioChainBytes = 0;
};

// See the struct's doc comment above for what this measures and why it is
// captured once, on the first call to currentResidentBytes(), rather than
// at the point this accessor itself is called.
IdleDependencyChainBytes idleDependencyChainBytes();

}  // namespace np
