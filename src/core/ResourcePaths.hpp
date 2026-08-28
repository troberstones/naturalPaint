#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

// core/ResourcePaths -- the one place this codebase decides where its five
// runtime resources live.
//
// ==========================================================================
// The problem (docs/architecture-review.md P1-2, "The binary cannot leave
// the machine that built it")
// ==========================================================================
//
// `src/CMakeLists.txt`'s `target_compile_definitions` bakes five paths into
// the executable, every one of them an absolute path into the *build*
// machine's checkout:
//
//   NP_SHADER_DIR             shaders/
//   NP_MIXBOX_LUT              third_party/mixbox/shaders/mixbox_lut.png
//   NP_KEYMAP_DIR               keymaps/
//   NP_LUCIDE_TTF                 third_party/lucide/lucide.ttf
//   NP_LUCIDE_CODEPOINTS_JSON       third_party/lucide/codepoints.json
//
// Copy the binary anywhere else, or move the checkout, and every one of
// those five strings points at nothing. The failure modes differ and that is
// the worse part: `main.cpp`'s Mixbox LUT load is fatal (loud, at least), but
// `ui/Fonts.cpp`'s Lucide font check degrades to hand-drawn vectors -- the
// toolbox still renders, just with no icons, and nothing on a machine
// without a terminal attached ever explains why. That silent case is
// documented separately in this project's own notes as a known trap.
//
// ==========================================================================
// The fix: one resolver, three tiers, in this order
// ==========================================================================
//
// For every one of the five resources, in this order:
//
//   1. `NP_ASSET_DIR` environment variable, if set. Joined with the
//      resource's path relative to a source-tree-shaped root (e.g.
//      `$NP_ASSET_DIR/shaders`, `$NP_ASSET_DIR/third_party/lucide/lucide.ttf`).
//      This is for the golden harness and for tests: point a whole run at a
//      scratch resource tree without relinking anything. Checked first, and
//      wins outright when the path it names exists -- see resolveResourcePath()
//      below -- because an operator who set this override did so on purpose
//      and a silently-ignored override is its own bug.
//
//   2. The compile-time absolute path (`CMAKE_SOURCE_DIR` at configure time,
//      via the five `NP_*` macros) -- the source tree this binary was built
//      from, on the occasions there still is one.
//
//   3. Executable-relative: `<directory containing the running binary>/`
//      followed by the same relative layout as (1). This is what makes the
//      binary portable. `src/CMakeLists.txt` stages the five resources into
//      `build/src/` (next to `naturalPaint`) with a POST_BUILD copy, so this
//      tier is not aspirational -- a freshly built tree already has what this
//      lookup will find, and a binary copied elsewhere finds it too, so long
//      as its staged neighbours travel with it.
//
// (The `tier` id `resolveFromCandidates()` reports names the SOURCE, not the
// priority: 1 override, 2 staged-beside-the-binary, 3 compile-time. The
// order they are TRIED in is the numbered list above. Tying the ids to the
// source means a caller asking "where did this come from" does not also have
// to track the current precedence.)
//
// **Why the source tree before the staged copy, and not the reverse.** The
// reverse is the obvious answer and it is wrong, for a reason that only
// shows up when you run it. The one machine on which both of these tiers
// exist is the machine that built the binary, and on that machine the source
// tree is the copy someone is actually editing: `keymaps/default.json` is
// meant to be hand-edited, and `shaders/*.wgsl` are meant to be edited and
// reloaded live with Cmd+R (`reload_shaders`, bound in that same keymap;
// `src/CMakeLists.txt`'s own comment has promised exactly this since Phase
// 1). Checking the staged copy first breaks both, silently -- and that is
// measured rather than argued: appending unparseable text to
// `shaders/advect_water.wgsl` makes `--selftest` exit 1 with shader errors
// on a build without this module, and exit 0, quietly reading the stale
// staged copy, on a build that consults the staged copy first. Closing
// P1-2's silent failure by opening a new silent failure in the edit-reload
// loop is not a fix, it is a relocation.
//
// Off the build machine the compile-time path resolves to nothing, tier 3
// wins by default, and the binary is portable -- which is the entire job
// tier 3 exists for, and it does it exactly when it is needed.
//
// What this order costs, stated plainly: a DIFFERENT machine that happened
// to have a directory at the build machine's absolute source path would be
// preferred over the binary's own staged copy. That takes reproducing
// another user's home-directory layout, and `NP_ASSET_DIR` overrides it
// outright for anyone who has to care. Set against a breakage that would hit
// this project's own edit-reload loop every day, it is the better trade --
// but it IS a trade, not a free win.
//
// ==========================================================================
// Why _NSGetExecutablePath and not SDL_GetBasePath()
// ==========================================================================
//
// SDL3 (already linked) offers `SDL_GetBasePath()`, which would have worked
// here too -- SDL3's implementation does not require `SDL_Init()` to have run
// (it shells out to the same platform primitive underneath), so the
// "does --selftest break because SDL isn't up yet" concern this task raised
// does not actually apply on this codebase's current call graph: by the time
// any of the five resources is first loaded, `SDL_Init(SDL_INIT_VIDEO)` has
// already run (`main.cpp`, near the top). It was rejected anyway, for a
// reason that has nothing to do with initialization order: layering. Every
// other header under `src/core/` is free of SDL, wgpu and ImGui -- `core/` is
// the bottom of this codebase's dependency stack (see e.g. `core/Parallel.hpp`,
// `core/Document.hpp`), and `gfx/`, `ui/`, `app/` build on top of it, never
// the other way around. Taking an SDL dependency here to save one platform
// `#include` would be the first crack in that rule, for a module whose whole
// job is "where do files live" and has no principled reason to know SDL
// exists. `_NSGetExecutablePath` (`<mach-o/dyld.h>`) plus `realpath()` to
// resolve symlinks is the direct macOS primitive, unconditionally available
// the instant the process starts -- no subsystem, no init call, nothing to
// get the ordering of wrong, ever, for any future caller. `app/Memory.cpp`
// takes the same approach for RSS (raw `<mach/mach.h>`, no `#ifdef __APPLE__`
// guard) and this file follows that precedent: the project currently only
// builds for macOS, so the guard would be untestable dead code, not
// portability.
//
// ==========================================================================
// Testability: the pure resolver is separate from the real one
// ==========================================================================
//
// `resolveFromCandidates()` below takes every input as a parameter --
// override root, executable directory, compile-time absolute path, and even
// the `exists()` predicate -- and touches no global state. `--selftest`
// (`app/selftest/ResourcePaths.cpp`) calls it directly with a temp directory
// standing in for "the executable's directory" to prove tier 2 is reachable
// without ever writing next to the real binary, and with a deliberately
// nonexistent compile-time path to prove tier 3 is reachable (or not) on
// demand. `resolveResourcePath()` is the thin, real wrapper around it that
// production code calls -- it reads the real environment variable, asks the
// real OS for the real executable directory (cached: the executable does not
// move mid-process), and supplies the real `NP_*` macro for `kind`.
namespace np {

enum class ResourceKind {
  ShaderDir,
  MixboxLut,
  KeymapDir,
  LucideTtf,
  LucideCodepointsJson,
};

// A short label used only in the missing-resource report -- "shaders/",
// "third_party/mixbox/shaders/mixbox_lut.png", and so on.
const char* resourceLabel(ResourceKind kind);

// The path relative to a source-tree-shaped root: "shaders",
// "third_party/mixbox/shaders/mixbox_lut.png", "keymaps",
// "third_party/lucide/lucide.ttf", "third_party/lucide/codepoints.json".
// This is the exact layout `src/CMakeLists.txt` stages into `build/src/`,
// so it is also the layout an `NP_ASSET_DIR` override or a test's temp
// directory needs to reproduce.
std::string resourceRelativePath(ResourceKind kind);

// The compile-time absolute path baked in for `kind` -- i.e. the literal
// value of NP_SHADER_DIR / NP_MIXBOX_LUT / NP_KEYMAP_DIR / NP_LUCIDE_TTF /
// NP_LUCIDE_CODEPOINTS_JSON for that kind. Exposed so callers that used to
// reference the macro directly (and the tests that check tier 3 against the
// real value) have a non-macro way to get it.
std::string compileTimeAbsolutePath(ResourceKind kind);

// The three-tier decision, taking every input as a parameter and touching no
// global state -- see "Testability" above. `overrideRoot` and `exeDir` may
// both be empty, meaning "that tier does not apply" (skipped, not tried as
// an empty-string path). `exists` is injected so a test can fake filesystem
// state without touching a real disk; production always passes a thin
// wrapper over `std::filesystem::exists`.
//
// Returns the first candidate, in tier order, for which `exists()` is true.
// If none exist, returns a best-effort path anyway (the override candidate
// if `overrideRoot` was set, else the compile-time path) with `found` set to
// false -- callers that don't check `found` get the same non-empty string
// every prior direct macro reference did, so nothing that used to compile
// against `NP_SHADER_DIR` etc. unconditionally starts crashing on an empty
// path. Callers that DO check `found` (resolveResourcePath() does) can
// report the failure loudly instead of discovering it three calls later as
// a mysterious empty read.
struct ResolvedResource {
  std::string path;
  int tier = 0;          // 1, 2, or 3 -- which tier supplied `path`; 0 if none did
  bool found = false;    // whether `path` actually exists
  std::vector<std::string> tried;  // every candidate checked, in order, for reportResourceMissing()
};

ResolvedResource resolveFromCandidates(
    const std::string& overrideRoot, const std::string& exeDir,
    const std::string& compileTimeAbsolutePath, const std::string& relativePath,
    const std::function<bool(const std::string&)>& exists);

// The directory containing the currently running executable, resolved via
// `_NSGetExecutablePath()` + `realpath()` (symlinks resolved, so a symlinked
// launcher still finds the resources staged beside the real binary).
// Computed once and cached in a function-local static -- the executable does
// not move mid-process, and every one of the five resource loads in a
// typical run would otherwise repeat the same two syscalls. Returns "" if
// the OS call fails (buffer too small on the first try is handled; anything
// else is not expected to happen and is treated as "tier 2 unavailable, fall
// through").
const std::string& executableDir();

// Prints `label` and every path in `tried` to `out` (default `stderr`) --
// the loud version of "this resource could not be found in any tier". `out`
// is a parameter specifically so `--selftest` can pass an in-memory stream
// (`fmemopen`) and assert on the exact text without redirecting the process's
// real stderr out from under whichever section runs next. Called
// automatically by `resolveResourcePath()` whenever none of the three tiers
// exist; also callable directly by a callsite that wants to add its own
// context (ui/Fonts.cpp does, for the Lucide font specifically, since a
// missing icon font is this task's motivating case).
void reportResourceMissing(const char* label, const std::vector<std::string>& tried,
                           std::FILE* out = nullptr);

// The real resolver: reads `NP_ASSET_DIR` from the environment, asks
// executableDir() for tier 2, and looks up the real NP_* macro for `kind` as
// tier 3. Reports (see reportResourceMissing(), to real stderr) if none of
// the three tiers exist, then returns the same best-effort path
// resolveFromCandidates() would.
std::string resolveResourcePath(ResourceKind kind);

// Convenience accessors -- these are what call sites use in place of the
// five raw NP_* macros. Each is `resolveResourcePath(ResourceKind::X)` by
// another name, so `#include "core/ResourcePaths.hpp"` plus one of these is
// the whole migration for a call site that used to write `NP_SHADER_DIR`
// etc. directly.
std::string shaderDir();
std::string mixboxLutPath();
std::string keymapDir();
std::string lucideTtfPath();
std::string lucideCodepointsJsonPath();

}  // namespace np
