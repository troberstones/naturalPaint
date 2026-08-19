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

}  // namespace np
