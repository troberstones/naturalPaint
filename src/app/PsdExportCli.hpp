#pragma once

// app/PsdExportCli -- `--psd-export <out.psd>`, the headless hook that lets an
// EXTERNAL reader look at what io/PsdExport writes.
//
// **Why this exists at all, given --selftest already round-trips the file.**
// docs/psd-export.md's verification section makes the argument and it is the
// whole reason this flag is here: a round trip through our own code is
// symmetric in exactly the places most likely to be wrong. Invert the channel
// order, the sRGB encode, or the premultiply convention in *both* directions
// and every assertion in app/selftest/PsdExport.cpp stays green while the file
// is wrong everywhere it will actually be opened. Only a reader this project
// did not write can say otherwise, and psd-tools 1.18.0 is the one already in
// use as this project's PSD oracle (io/PsdImport.hpp records the comparison it
// was used for on the reading side).
//
// So this writes two files from ONE document:
//
//   <out.psd>       through io/PsdExport -- the code under test
//   <out.psd>.png   through io/Export's existing 8-bit sRGB PNG path
//
// and the external check is that psd-tools' reading of the first agrees with
// PIL's reading of the second, pixel for pixel. That is a materially better
// oracle than "psd-tools opened it without throwing": the PNG comes from a
// stb_image_write encoder and an `encodeLinearImage()` quantiser that PRD B6's
// own suite already exercises, and it shares nothing with the PSD path below
// the flatten. Agreement therefore covers the sRGB transfer function, the
// straight-alpha convention, the channel order, the planar layout and the RLE
// framing all at once -- and disagreement localises to whichever of them the
// diff points at.
//
// The document is synthesised here rather than opened from a file, on purpose.
// It is deterministic, it is checked into nothing, and it is shaped to make the
// failures that matter visible: a non-square, non-tile-multiple canvas so a
// width/height transposition or a row-stride error cannot hide; a region where
// the composite alpha is fractional so a premultiplied writer diverges; and a
// gradient across both axes so a channel swap or a plane duplication shows up
// as a wrong colour rather than a wrong number.
//
// Headless and GPU-free. Not part of `--selftest` -- it writes files, and the
// reader that judges them is a Python process outside this binary.

namespace np {

// Writes `outPath` (a PSD) and `outPath + ".png"` (the independent reference)
// from one synthesised document, prints what was written and any warnings, and
// returns 0 on success or 1 on any refusal.
// `layered` selects `writeLayeredPsd()` over `writeFlattenedPsd()` -- the same
// fixture through both tiers, so a difference between the two files is a
// difference in the writers rather than in what was written.
int runPsdExportDemo(const char* outPath, bool layered);

}  // namespace np
