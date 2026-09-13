#pragma once

#include <string>

// app/Version -- `naturalPaint --version`.
//
// The semver comes from the top-level `project()` call; the git hash comes
// from a header generated at BUILD time (cmake/GenerateVersionHeader.cmake,
// wired in by src/CMakeLists.txt) rather than at configure time, so it
// cannot go stale between a commit and the next `cmake` invocation. Kept in
// its own tiny header/source pair, separate from the generated one, so the
// format `versionString()` produces is hand-written and reviewable rather
// than baked into a CMake string.

namespace np {

// "naturalPaint <semver> (<hash>)", e.g. "naturalPaint 0.1.0 (a1b2c3d)" or,
// with uncommitted changes, "naturalPaint 0.1.0 (a1b2c3d-dirty)". Outside a
// git checkout (a source tarball, or `git`/`.git` missing at build time) the
// parenthesised part reads "(unknown)" instead of a hash.
std::string versionString();

}  // namespace np
