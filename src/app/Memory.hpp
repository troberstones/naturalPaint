#pragma once
#include <cstddef>

namespace np {

// Current resident set size, in bytes -- macOS only (this project already
// assumes macOS; see SDL_WINDOW_METAL in main.cpp). Backed by task_info() +
// MACH_TASK_BASIC_INFO, which reports the task's *current* resident_size.
// Deliberately not getrusage()'s ru_maxrss: that field is the process's peak
// RSS, monotonically non-decreasing for the life of the process, so it can
// never observe memory being freed (a mode switch releasing ink/oil fields,
// PaintSim not existing yet) -- exactly the things 1.4 / ADR-0001 need to
// measure. Returns 0 if task_info() fails.
size_t currentResidentBytes();

// Current *phys_footprint*, in bytes -- the number Activity Monitor prints in
// its "Memory" column and the number `footprint(1)` totals. Backed by
// task_info() + TASK_VM_INFO. Returns 0 if task_info() fails or if the kernel
// is too old to carry the field (it needs TASK_VM_INFO_REV1_COUNT).
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

}  // namespace np
