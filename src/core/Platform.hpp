#pragma once

// core/Platform -- NP_PLATFORM_MACOS / NP_PLATFORM_IOS, for the handful of
// `__APPLE__` sites that actually mean "macOS" rather than "any Apple OS".
//
// `__APPLE__` is true on iOS too, so every existing `#if defined(__APPLE__)`
// in this codebase was, until the iOS spike (docs/ios-spike-plan.md), an
// accidental "macOS" guard that had never been exercised on anything else.
// The spike's survey went through all of them and found most are genuinely
// Apple-wide and correctly stay `__APPLE__`: dispatch_apply
// (core/Parallel.hpp), mach task_info (app/Memory.cpp), mach-o dyld
// (core/ResourcePaths.cpp), CoreText (text/CoreTextShaper.mm), the Metal
// surface (gfx/Context.cpp), F_FULLFSYNC, `st_mtimespec`, and the
// `$HOME/Library/Application Support/...` paths, which resolve the same way
// inside an iOS app's sandboxed container.
//
// Only the three AppKit `.mm` files reach for something iOS genuinely does
// not have (NSMenu, NSEvent trackpad monitors, NSTouch) -- those, and only
// those, need this finer distinction.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define NP_PLATFORM_IOS 1
#define NP_PLATFORM_MACOS 0
#else
#define NP_PLATFORM_IOS 0
#define NP_PLATFORM_MACOS 1
#endif
#else
#define NP_PLATFORM_IOS 0
#define NP_PLATFORM_MACOS 0
#endif
