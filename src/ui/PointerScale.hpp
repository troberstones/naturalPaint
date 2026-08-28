#pragma once

// ui/PointerScale -- how big the user has asked their pointer to be.
//
// ==========================================================================
// Why this file exists, which is a fact about macOS and not a design choice
// ==========================================================================
//
// `ui/ToolCursor.hpp` §7 carried an objection it could not answer from inside
// itself: a custom bitmap cursor does not grow when the user enlarges their
// pointer in System Settings ▸ Accessibility ▸ Display ▸ Pointer size, so a
// user who needs a large pointer gets a normal-sized lasso sitting inside an
// enlarged arrow everywhere else. That objection is **true and it is not
// fixable by asking the OS more politely**: macOS applies the pointer-size
// setting to the cursors it draws itself, and applies nothing at all to an
// `NSCursor` an application built from its own image. There is no
// `NSCursor` API that opts in.
//
// So an application that wants to honour the setting has to read it and
// rasterise its own cursors bigger. That is what this file is: the reading
// half. `ui/ToolCursor.cpp` is the rasterising half.
//
// **This is separate from the Retina/backing-scale question, which SDL
// already answers.** A cursor also has to be crisp on a 2x display, and that
// is a different mechanism: `SDL_AddSurfaceAlternateImage()` attaches a
// higher-resolution alternate to a cursor surface, and SDL's Cocoa backend
// turns the base plus its alternates into one multi-representation `NSImage`
// whose POINT size is the base surface's size (`Cocoa_CreateImage()` in
// `SDL_cocoavideo.m`), which is exactly the multi-scale image AppKit picks a
// representation out of per display. Backing scale is therefore SDL's job and
// accessibility scale is ours; the two multiply, and `ui/ToolCursor.cpp`'s
// `create()` is where they meet.
//
// ==========================================================================
// The key, and why a `defaults` key rather than an API
// ==========================================================================
//
// `com.apple.universalaccess` / `mouseDriverCursorSize`, a float where 1.0 is
// "Normal" and the slider's far end is 4.0. There is no public framework call
// for it -- `CGSGetCursorScale` exists but is SPI in CoreGraphics' private
// surface, and using SPI to read a preference that CoreFoundation will hand
// over cleanly is the worse trade. `CFPreferencesCopyAppValue()` on another
// application's domain works for a non-sandboxed app; it returns null if that
// ever stops being true, and null is handled as "no opinion, use 1.0".
//
// Verified on this machine at the time of writing: the setting reads
// **2.0724**, i.e. the developer running this code is a user of the very
// accessibility feature the objection is about. That is not a hypothetical.
//
// **What is deliberately NOT done here:** no notification is registered.
// macOS does post distributed notifications for some universal-access
// changes, but the set is undocumented and version-dependent, and the cost of
// being wrong is a cursor that is the wrong size until the app is restarted.
// Instead `main.cpp` re-reads on window focus gained -- which is precisely
// when a user coming back from System Settings returns -- and rebuilds only
// if the number actually moved. Polling a preference once per focus change is
// free; guessing at a notification name is not.
namespace np {

// The user's pointer-size multiplier: 1.0 for a normal pointer, larger when
// they have enlarged it. Clamped to [1.0, 4.0].
//
// Returns exactly 1.0 on every platform but macOS, and on macOS whenever the
// preference is absent or unreadable -- "no opinion" and "normal" are the same
// answer here, which is why this returns a float rather than an optional: no
// caller has anything different to do with the two cases.
float osPointerSizeScale() noexcept;

}  // namespace np
