#include "ui/PointerScale.hpp"

#include <algorithm>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace np {

float osPointerSizeScale() noexcept {
#if !defined(__APPLE__)
  // Windows scales custom cursors itself from the system cursor size, and X11
  // has `Xcursor.size`; neither is this project's platform today, and
  // returning 1.0 there is honest rather than a guess at an untested path.
  return 1.0f;
#else
  // Another application's preference domain, so `CFPreferencesCopyAppValue`
  // rather than the current-application convenience. The synchronize call is
  // what makes a change made in System Settings *while this process is
  // running* visible to it -- without it the value can be served from a cache
  // taken at first read, which would make the focus-gained re-read in
  // main.cpp silently useless.
  CFPreferencesAppSynchronize(CFSTR("com.apple.universalaccess"));
  const CFPropertyListRef value =
      CFPreferencesCopyAppValue(CFSTR("mouseDriverCursorSize"), CFSTR("com.apple.universalaccess"));
  if (value == nullptr) return 1.0f;  // never set, or not readable: normal.

  double size = 1.0;
  // Type-checked rather than cast: a preference file is user-writable, and a
  // string where a number belongs would otherwise be read as a pointer.
  const bool isNumber = CFGetTypeID(value) == CFNumberGetTypeID();
  if (isNumber) CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &size);
  CFRelease(value);
  if (!isNumber) return 1.0f;

  // Clamped at both ends. The low end because a value below 1 would make the
  // cursor smaller than the design and there is no UI that asks for that; the
  // high end because the canvas is `32 * scale * 2` pixels square at the
  // Retina alternate, and an unclamped preference file could ask for a cursor
  // larger than any texture the platform will accept.
  if (!(size > 0.0)) return 1.0f;  // also catches NaN
  return static_cast<float>(std::clamp(size, 1.0, 4.0));
#endif
}

}  // namespace np
