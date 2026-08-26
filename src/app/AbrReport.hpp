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

}  // namespace np
