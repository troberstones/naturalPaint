#pragma once

// app/PsdReport -- what actually survived importing a `.psd`.
//
// io/PsdImport.hpp's own closing section states the gap this closes: no
// Photoshop-authored file existed on the machine that module was written on,
// so every fixture `--selftest` runs through it was produced by a matching
// writer in the test code, and passing proved the reader agrees with its own
// author's reading of the specification rather than with Photoshop. That gap
// closes exactly once: when a real file is opened and someone compares the
// panel of the genuine document against what the reader built from its bytes.
//
// This flag is that comparison's left-hand side. It prints the imported layer
// stack -- order, name, blend key as mapped, opacity, visibility, clipping,
// rectangle and whether any pixel arrived -- so the answer to "did Photoshop's
// document survive?" is a diff against the application's own layer panel
// rather than an impression formed by scrolling a canvas.
//
// The same argument `--abr-report` already makes for `.abr`, for the same
// reason: a foreign binary format this project did not write, whose failures
// are overwhelmingly *silent* (a stack read upside down, every hidden layer
// shown, a blend key quietly downgraded) rather than loud. None of those show
// up as a crash, and all three are one glance from this table.
//
// Headless, GPU-free, read-only: it imports and prints, and writes no files.

namespace np {

// Imports `path` through io/PsdImport and prints the resulting layer stack.
// Returns 0 when the file imported, 1 when it did not (including the
// `noLayerData` case, which is reported by name -- it is not a parse failure,
// but it is not a layered import either, and the caller asked for one).
int runPsdReport(const char* path);

}  // namespace np
