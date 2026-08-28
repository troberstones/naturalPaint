#include "core/ResourcePaths.hpp"

#include <mach-o/dyld.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace np {
namespace {

namespace fs = std::filesystem;

// Wraps `std::filesystem::exists()` as the default predicate
// `resolveResourcePath()` hands to `resolveFromCandidates()`. A tiny
// indirection, but it is what lets --selftest inject a fake one instead.
bool pathExists(const std::string& p) {
  std::error_code ec;
  return !p.empty() && fs::exists(p, ec);
}

std::string joinRoot(const std::string& root, const std::string& relativePath) {
  return (fs::path(root) / relativePath).string();
}

}  // namespace

const char* resourceLabel(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::ShaderDir: return "shaders/ (NP_SHADER_DIR)";
    case ResourceKind::MixboxLut: return "third_party/mixbox/shaders/mixbox_lut.png (NP_MIXBOX_LUT)";
    case ResourceKind::KeymapDir: return "keymaps/ (NP_KEYMAP_DIR)";
    case ResourceKind::LucideTtf: return "third_party/lucide/lucide.ttf (NP_LUCIDE_TTF)";
    case ResourceKind::LucideCodepointsJson:
      return "third_party/lucide/codepoints.json (NP_LUCIDE_CODEPOINTS_JSON)";
  }
  return "(unknown resource)";
}

std::string resourceRelativePath(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::ShaderDir: return "shaders";
    case ResourceKind::MixboxLut: return "third_party/mixbox/shaders/mixbox_lut.png";
    case ResourceKind::KeymapDir: return "keymaps";
    case ResourceKind::LucideTtf: return "third_party/lucide/lucide.ttf";
    case ResourceKind::LucideCodepointsJson: return "third_party/lucide/codepoints.json";
  }
  return "";
}

std::string compileTimeAbsolutePath(ResourceKind kind) {
  switch (kind) {
    case ResourceKind::ShaderDir: return NP_SHADER_DIR;
    case ResourceKind::MixboxLut: return NP_MIXBOX_LUT;
    case ResourceKind::KeymapDir: return NP_KEYMAP_DIR;
    case ResourceKind::LucideTtf: return NP_LUCIDE_TTF;
    case ResourceKind::LucideCodepointsJson: return NP_LUCIDE_CODEPOINTS_JSON;
  }
  return "";
}

ResolvedResource resolveFromCandidates(
    const std::string& overrideRoot, const std::string& exeDir,
    const std::string& compileTimeAbsolutePathValue, const std::string& relativePath,
    const std::function<bool(const std::string&)>& exists) {
  ResolvedResource result;

  if (!overrideRoot.empty()) {
    const std::string candidate = joinRoot(overrideRoot, relativePath);
    result.tried.push_back(candidate);
    if (exists(candidate)) {
      result.path = candidate;
      result.tier = 1;
      result.found = true;
      return result;
    }
  }

  // The source tree BEFORE the staged copy beside the executable, and the
  // `tier` ids below deliberately keep naming their SOURCE (2 = staged,
  // 3 = compile-time) rather than their priority, so a caller reasoning
  // about where a file came from does not have to track this ordering too.
  //
  // The only machine on which both of these exist is the machine that built
  // the binary, and there the source tree is the authoritative copy: it is
  // the one a developer edits. `keymaps/default.json` is meant to be
  // hand-edited, and `shaders/*.wgsl` are meant to be edited and reloaded
  // live with Cmd+R (`reload_shaders`, bound in `keymaps/default.json`;
  // `src/CMakeLists.txt`'s own comment has promised this since Phase 1).
  // Checking the staged copy first silently breaks both -- proven, not
  // supposed: appending unparseable text to `shaders/advect_water.wgsl`
  // made `--selftest` exit 1 with shader errors before this module existed,
  // and exit 0 with the staged copy winning after it. Trading the P1-2
  // silent failure for a new silent failure in the edit-reload loop is not
  // a fix.
  //
  // Off the build machine the compile-time path resolves to nothing, so the
  // staged copy wins by default and the binary is portable, which is the
  // whole point of tier 2 -- it is load-bearing exactly when it needs to be.
  // The cost of this order is the narrow case the reverse order was chosen
  // for: a DIFFERENT machine that happens to have a directory at the build
  // machine's absolute source path would be read in preference to the
  // binary's own staged copy. That requires reproducing another user's home
  // directory layout, and `NP_ASSET_DIR` overrides it outright when it
  // matters. A guaranteed daily breakage is the worse trade.
  result.tried.push_back(compileTimeAbsolutePathValue);
  if (exists(compileTimeAbsolutePathValue)) {
    result.path = compileTimeAbsolutePathValue;
    result.tier = 3;
    result.found = true;
    return result;
  }

  if (!exeDir.empty()) {
    const std::string candidate = joinRoot(exeDir, relativePath);
    result.tried.push_back(candidate);
    if (exists(candidate)) {
      result.path = candidate;
      result.tier = 2;
      result.found = true;
      return result;
    }
  }

  // Nothing exists in any tier. Best-effort return so a caller that doesn't
  // check `found` still gets a non-empty string, same as every direct NP_*
  // macro reference this resolver replaces did unconditionally.
  result.path = !overrideRoot.empty() ? joinRoot(overrideRoot, relativePath)
                                      : compileTimeAbsolutePathValue;
  result.tier = 0;
  result.found = false;
  return result;
}

const std::string& executableDir() {
  static const std::string cached = [] {
    // `_NSGetExecutablePath` wants the buffer size in bytes and returns -1
    // (and writes the required size into `size`) if it was too small --
    // the two-call pattern the man page documents.
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::string{};

    // `_NSGetExecutablePath` can itself return a path containing a symlink
    // component (e.g. `/tmp` on macOS is itself a symlink to `/private/tmp`,
    // which is exactly the case this project's own worktrees live under --
    // see the memory note on `/private/tmp/np-portable`). `realpath()`
    // resolves all of them, which matters here specifically because the
    // resolver joins this directory with a *relative* path to find staged
    // resources, and a symlinked component that doesn't match the staged
    // layout on the far side of the link would make an existing file look
    // absent.
    char resolved[PATH_MAX];
    if (::realpath(buf.data(), resolved) == nullptr) return std::string{};

    return fs::path(resolved).parent_path().string();
  }();
  return cached;
}

void reportResourceMissing(const char* label, const std::vector<std::string>& tried,
                           std::FILE* out) {
  std::FILE* stream = (out != nullptr) ? out : stderr;
  std::fprintf(stream,
              "[resources] could not find %s in any of %zu location(s) tried:\n", label,
              tried.size());
  for (const std::string& candidate : tried) {
    std::fprintf(stream, "[resources]   %s\n", candidate.c_str());
  }
}

std::string resolveResourcePath(ResourceKind kind) {
  const char* overrideEnv = std::getenv("NP_ASSET_DIR");
  const ResolvedResource resolved =
      resolveFromCandidates(overrideEnv != nullptr ? overrideEnv : std::string{}, executableDir(),
                            compileTimeAbsolutePath(kind), resourceRelativePath(kind), pathExists);
  if (!resolved.found) {
    reportResourceMissing(resourceLabel(kind), resolved.tried, stderr);
  }
  return resolved.path;
}

std::string shaderDir() { return resolveResourcePath(ResourceKind::ShaderDir); }
std::string mixboxLutPath() { return resolveResourcePath(ResourceKind::MixboxLut); }
std::string keymapDir() { return resolveResourcePath(ResourceKind::KeymapDir); }
std::string lucideTtfPath() { return resolveResourcePath(ResourceKind::LucideTtf); }
std::string lucideCodepointsJsonPath() {
  return resolveResourcePath(ResourceKind::LucideCodepointsJson);
}

}  // namespace np
