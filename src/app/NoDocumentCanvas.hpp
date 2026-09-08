#pragma once

#include <memory>

#include "app/DocumentLifecycle.hpp"
#include "sim/PaintSim.hpp"

// app/NoDocumentCanvas -- the pure half of main.cpp's per-frame "no document
// means no canvas" hook (docs/testing-issues.md T5, reversed 2026-09-08).
//
// main.cpp's frame loop used to inline this check directly against the
// live `sim`/`st.documents` globals, which meant --selftest could only ever
// exercise a LOOK-ALIKE of the hook (a private PaintSim and a private
// DocumentSession, app/selftest/NoDocumentCanvas.cpp) rather than the real
// thing -- sabotaging the actual hook left that section green. Pulling the
// logic out into a free function main.cpp calls closes that gap: the
// selftest can now call the exact function the frame loop calls.
namespace np {

// If `sim` is non-null and `documents` is empty, shuts the solver down and
// resets `sim` to null, then returns true. Otherwise leaves `sim` untouched
// (including a `sim` that is already null) and returns false. Idempotent by
// construction: once this returns true, `sim` is null, so every subsequent
// call on the same (now-empty) session returns false without doing
// anything -- there is nothing left to shut down.
bool releaseSolverWhenNoDocuments(std::unique_ptr<PaintSim>& sim,
                                   const DocumentSession& documents);

}  // namespace np
