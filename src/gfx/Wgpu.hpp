#pragma once
#include <webgpu/webgpu.h>

#include <cstddef>
#include <cstring>

// wgpu-native ships the C API only. Rather than vendor a full C++ wrapper, we
// keep the raw handles — every GPU object here lives for the lifetime of the
// app, so there is nothing for RAII to actually manage — and add the two
// helpers that would otherwise be repeated on every line.

namespace np {

// WGPUStringView from a C literal.
inline WGPUStringView sv(const char* s) { return WGPUStringView{s, WGPU_STRLEN}; }
inline WGPUStringView sv(const char* s, size_t len) { return WGPUStringView{s, len}; }

// StringView -> printf-able length, for use with a "%.*s" pair. Views are not
// null-terminated in general.
inline int svLen(WGPUStringView v) {
  if (!v.data) return 0;
  return static_cast<int>(v.length == WGPU_STRLEN ? std::strlen(v.data) : v.length);
}

}  // namespace np
