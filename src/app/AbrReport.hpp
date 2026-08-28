#pragma once

// app/AbrReport -- `--abr-report <file.abr>`, a headless read-only dump of what
// survived importing a Photoshop brush library.
//
// The rationale is in the .cpp: it separates "the dynamics are misread" from
// "the tip never arrived" before anyone spends time tuning dynamics that were
// never the problem.
//
// Returns a process exit code: 0 on a successful import, 1 if the file cannot
// be opened or the parser refuses it.

namespace np {

int runAbrReport(const char* path);

// `--abr-keys <file.abr>` -- the raw evidence, one level below `runAbrReport()`.
//
// Prints the file's `8BIM` section table and then a census of EVERY descriptor
// key the `desc` block contains: the normalised path (list indices collapsed to
// `[]`, so `Brsh/0/useTexture` and `Brsh/49/useTexture` are one row), how many
// presets carry it, the descriptor types seen, and a value summary -- true/false
// counts for booleans, a histogram for enumerations, min/max for numbers.
//
// **This exists so that no claim about what an `.abr` contains has to be
// believed.** Every count this project has written down about Photoshop's
// brush format -- how many presets switch Texture on, which `bVTy` ordinals
// occur, what `textureBlendMode` values are real -- came from a throwaway
// script that no longer exists, and this codebase has already shipped two
// defects that began as a confident recollection (`AbrControl` in
// io/AbrBrushes.hpp, audit B9). An absence claim in particular rots: "no file
// uses ordinal 7" is true until a file does. This flag re-measures in a second,
// against whatever pack is in front of you, so the answer is current rather
// than remembered.
//
// Headless, GPU-free and read-only, exactly as `runAbrReport()` is.
int runAbrKeyCensus(const char* path);

}  // namespace np
