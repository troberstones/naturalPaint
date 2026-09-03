#include "text/Shaper.hpp"

// text/StubShaper -- the non-Apple fallback for text/Shaper.hpp.
//
// gfx/Context.cpp's `#error` for a Windows/Linux GPU surface is the wrong
// precedent to copy here: refusing to *build* is fine for a feature nothing
// else depends on yet, but text/Shaper.hpp's callers (app/selftest/
// TextShaper.cpp today, a Text tool later) need to link and run on every
// platform this project targets, CI included. So this file compiles
// everywhere, answers every call with a defined, non-crashing refusal, and
// leaves the door open for a real cross-platform backend (HarfBuzz +
// FreeType, per PRD K2) to replace it without touching the header or any
// caller.
namespace np {

bool shaperAvailable() noexcept { return false; }

const char* shaperUnavailableReason() noexcept {
  return "text shaping is only implemented for Apple platforms (CoreText); "
         "this build has no shaper";
}

ShapedText shapeText(std::string_view /*utf8*/, const TextStyle& /*style*/,
                     const TextFrame& /*frame*/, TextAlign /*align*/) {
  ShapedText result;
  result.ok = false;
  result.error = shaperUnavailableReason();
  return result;
}

bool glyphPath(uint32_t /*glyphId*/, const TextStyle& /*style*/, Path* /*out*/) {
  return false;
}

// No platform font catalog to enumerate on this build -- matches
// `shaperAvailable()`'s own false above, per text/Shaper.hpp's comment on
// what a font picker should do with an empty list.
std::vector<std::string> availableFontFamilies() { return {}; }

bool fontFamilyAvailable(std::string_view /*family*/) { return false; }

}  // namespace np
