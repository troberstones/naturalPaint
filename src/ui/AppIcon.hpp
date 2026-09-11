#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Window;

// ui/AppIcon -- the application's own icon, set on the window at runtime.
//
// ==========================================================================
// (1) Why at runtime, and why compiled in
// ==========================================================================
//
// This build ships as a bare executable (`build/src/naturalPaint`), not an
// .app bundle, an installed package or an .exe with resources -- so none of
// the ways an operating system normally finds an app's icon apply to the way
// it is actually run. `SDL_SetWindowIcon()` is the one call that reaches all
// three platforms from the running process:
//
//   * macOS: SDL's Cocoa backend sets `NSApp.applicationIconImage`, which is
//     the **Dock** icon -- otherwise the generic "exec" icon for an
//     unbundled binary.
//   * Linux/X11: `_NET_WM_ICON`, which the taskbar and Alt-Tab read.
//   * Linux/Wayland: `xdg-toplevel-icon-v1` where the compositor offers it;
//     where it does not, the compositor looks the icon up from the desktop
//     entry matching the app id instead (icons/linux/naturalPaint.desktop,
//     named for SDL_GetAppID()'s default: the executable's name).
//   * Windows: the title bar and taskbar. The .exe's Explorer icon comes
//     from icons/windows/naturalPaint.rc.in instead.
//
// **The PNG is compiled into the binary** (src/CMakeLists.txt generates
// `AppIconPng.inc` from icons/linux/hicolor/512x512/apps/naturalPaint.png),
// not read through core/ResourcePaths like the shaders and the keymap: those
// are read from disk so they can be edited without a rebuild, and an icon
// has no such workflow -- but it does have a failure the others do not. A
// missing shader is a loud error on the first frame; a missing icon is a
// generic Dock tile nobody reports. Compiled in, it cannot go missing.
//
// 512 px because the macOS Dock draws up to 256 pt, i.e. 512 px on Retina at
// full magnification; every other consumer scales it down.
//
// ==========================================================================
// (2) The seam --selftest uses
// ==========================================================================
//
// `decodeAppIcon()` and `createAppIconSurface()` need no window and no video
// subsystem, so app/selftest/AppIcon.cpp calls the real ones.
// `installAppIcon()` is the only part that needs the real `SDL_Window`, and
// main.cpp records its outcome through `appIconInstalled()` so the suite can
// assert the running window actually accepted it.
namespace np {

// The embedded PNG, exactly as compiled in.
const unsigned char* appIconPngData() noexcept;
size_t appIconPngSize() noexcept;

struct AppIconImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4, straight (unpremultiplied) alpha
  std::string error;          // non-empty exactly when decoding failed
};

// The embedded PNG decoded to RGBA8 through the stb_image this binary already
// links (paint/Palette.cpp owns the implementation).
AppIconImage decodeAppIcon();

// An SDL surface over a decoded icon, owning a copy of its pixels -- the
// caller destroys it with SDL_DestroySurface(). nullptr on failure, with the
// reason in `*error` when `error` is non-null.
SDL_Surface* createAppIconSurface(std::string* error = nullptr);

// Sets the window's (and on macOS the Dock's) icon. Returns false, with the
// reason, if the icon could not be decoded or SDL refused it -- never fatal,
// the app runs with the platform's generic icon. Also records the outcome
// for appIconInstalled().
bool installAppIcon(SDL_Window* window, std::string* error = nullptr);

// Whether installAppIcon() has succeeded in this process.
bool appIconInstalled() noexcept;

}  // namespace np
