#pragma once
#include <string>
#include <string_view>

#include "gfx/Wgpu.hpp"

namespace np {

// WGSL has no preprocessor, so we resolve `//#include "path"` ourselves.
// Shaders live on disk rather than baked into the binary specifically so the
// solver can be tweaked and reloaded without a rebuild.
std::string readShaderSource(std::string_view relativePath);

// Returns null and logs the diagnostics on failure; callers keep the previously
// compiled pipeline so a typo mid-session doesn't kill the app.
WGPUShaderModule compileShader(WGPUDevice device, WGPUInstance instance,
                               std::string_view relativePath);

}  // namespace np
