#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Surface;
struct SDL_Window;

// The app icon, set on the window at runtime: the unbundled binary has no other
// way to get a Dock (macOS), taskbar (X11/Windows) or xdg-toplevel (Wayland)
// icon. The 512 px PNG is compiled in rather than read from disk so it cannot
// go missing silently.
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

AppIconImage decodeAppIcon();

// Caller owns the surface (SDL_DestroySurface). nullptr on failure.
SDL_Surface* createAppIconSurface(std::string* error = nullptr);

// Never fatal: on false the app keeps the platform's generic icon.
bool installAppIcon(SDL_Window* window, std::string* error = nullptr);

// For --selftest: whether main.cpp's installAppIcon() call succeeded.
bool appIconInstalled() noexcept;

}  // namespace np
