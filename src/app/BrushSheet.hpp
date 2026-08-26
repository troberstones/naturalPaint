#pragma once

// app/BrushSheet -- `--brush-sheet <file.abr> <out.png>`, a contact sheet of
// every imported preset painting Photoshop's own preview stroke.
//
// The rationale is in the .cpp: the stroke shape is copied from Photoshop's
// picker deliberately, so the two sheets can be laid side by side and any
// difference read as a difference in the BRUSH rather than in the path.
//
// Returns a process exit code: 0 on success, 1 if the file cannot be read, the
// import is refused, or the PNG cannot be written.

namespace np {

// `experiment` selects how the imported dynamics graph is INTERPRETED, so the
// same brushes can draw the same stroke with exactly one rule changed:
//   "as-imported"    -- the graph exactly as io/AbrBrushes produced it
//   "no-random-size" -- drop the RANDOM -> Size jitter link
//   "floor-once"     -- keep the jitter, but hang it off full size instead of
//                       carrying a second copy of the minimum-diameter floor
// nullptr means "as-imported".
int runBrushSheet(const char* abrPath, const char* outPath, const char* experiment);

}  // namespace np
