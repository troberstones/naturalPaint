#include "gfx/ShaderLoader.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "core/ResourcePaths.hpp"

namespace np {
namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Recursive so includes can include; the guard set makes repeats no-ops the way
// `#pragma once` would.
std::string expand(const fs::path& root, const fs::path& file,
                   std::unordered_set<std::string>& seen) {
  std::string src = slurp(file);
  if (src.empty()) {
    std::fprintf(stderr, "[shader] cannot read %s\n", file.string().c_str());
    return {};
  }

  std::ostringstream out;
  std::istringstream in(src);
  std::string line;
  const std::string tag = "//#include";

  while (std::getline(in, line)) {
    auto pos = line.find(tag);
    if (pos == std::string::npos) {
      out << line << '\n';
      continue;
    }
    auto q1 = line.find('"', pos);
    auto q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) {
      std::fprintf(stderr, "[shader] malformed include in %s: %s\n",
                   file.string().c_str(), line.c_str());
      continue;
    }
    std::string rel = line.substr(q1 + 1, q2 - q1 - 1);

    // The one basis-selecting indirection in the whole shader tree:
    // composite.wgsl names a symbolic target, "include/pigment_basis.wgsl",
    // that does not exist on disk, rather than naming mixbox.wgsl directly --
    // exactly the same role NP_USE_MIXBOX plays on the C++ side
    // (core/Pigment.cpp, paint/Palette.cpp), and resolved by the same
    // compile-time flag so the two halves of one build cannot disagree about
    // which pigment model is linked in. `#if` rather than a runtime choice
    // because WGSL itself has no preprocessor and this loader's `#include` is
    // already the only preprocessing step it gets (see this file's own
    // top-of-header comment).
    if (rel == "include/pigment_basis.wgsl") {
#if defined(NP_USE_MIXBOX)
      rel = "include/mixbox.wgsl";
#else
      rel = "include/km2.wgsl";
#endif
    }

    if (!seen.insert(rel).second) continue;  // already pulled in
    out << expand(root, root / rel, seen) << '\n';
  }
  return out.str();
}

struct ScopeResult { bool failed = false; bool done = false; };

}  // namespace

std::string readShaderSource(std::string_view relativePath) {
  const fs::path root{shaderDir()};
  std::unordered_set<std::string> seen;
  return expand(root, root / fs::path(relativePath), seen);
}

WGPUShaderModule compileShader(WGPUDevice device, WGPUInstance instance,
                               std::string_view relativePath) {
  const std::string src = readShaderSource(relativePath);
  const std::string label(relativePath);
  if (src.empty()) {
    std::fprintf(stderr, "[shader] %s is empty or unreadable\n", label.c_str());
    return nullptr;
  }

  // An error scope turns a silent null module — WebGPU's most confusing failure
  // mode — into a definite yes/no plus naga's diagnostics.
  wgpuDevicePushErrorScope(device, WGPUErrorFilter_Validation);

  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(src.c_str(), src.size());

  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  desc.label = sv(label.c_str(), label.size());

  WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &desc);

  ScopeResult res;
  WGPUPopErrorScopeCallbackInfo pci = {};
  pci.mode = WGPUCallbackMode_AllowProcessEvents;
  pci.userdata1 = &res;
  pci.userdata2 = const_cast<char*>(label.c_str());
  pci.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type,
                    WGPUStringView message, void* ud1, void* ud2) {
    auto* r = static_cast<ScopeResult*>(ud1);
    if (type != WGPUErrorType_NoError) {
      r->failed = true;
      std::fprintf(stderr, "[shader] %s:\n%.*s\n", static_cast<const char*>(ud2),
                   svLen(message), message.data ? message.data : "");
    }
    r->done = true;
  };
  wgpuDevicePopErrorScope(device, pci);
  while (!res.done) wgpuInstanceProcessEvents(instance);

  if (res.failed || !mod) {
    if (mod) wgpuShaderModuleRelease(mod);
    return nullptr;
  }
  return mod;
}

}  // namespace np
