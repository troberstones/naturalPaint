#include "app/selftest/Support.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "ui/AppIcon.hpp"

namespace np {

bool runAppIconTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // 1. The compiled-in PNG decodes, at the size the Dock needs.
  const AppIconImage img = decodeAppIcon();
  check(img.error.empty(), "app icon: embedded PNG decodes");
  check(img.width == 512 && img.height == 512, "app icon: embedded PNG is 512 x 512");
  check(img.rgba.size() == 512u * 512u * 4u, "app icon: decoded buffer is 512*512*4 bytes");

  // 2. A stale generated include would otherwise ship an old icon, all green.
  {
    std::ifstream f(NP_APP_ICON_PNG, std::ios::binary);
    const std::vector<unsigned char> onDisk((std::istreambuf_iterator<char>(f)),
                                            std::istreambuf_iterator<char>());
    const bool same = !onDisk.empty() && onDisk.size() == appIconPngSize() &&
                      std::equal(onDisk.begin(), onDisk.end(), appIconPngData());
    check(same, "app icon: embedded bytes == icons/linux/hicolor 512 px PNG");
  }

  // 3. The artwork's pixels: an opaque white ground, the brush's brown at the
  //    centre (measured 139 105 50; +-16 survives a re-export).
  if (img.rgba.size() == 512u * 512u * 4u) {
    auto px = [&](int x, int y) { return &img.rgba[(static_cast<size_t>(y) * 512u + x) * 4u]; };
    const unsigned char* c = px(0, 0);
    check(c[0] >= 250 && c[1] >= 250 && c[2] >= 250 && c[3] == 255,
          "app icon: corner (0,0) is the painted opaque white ground");
    const unsigned char* m = px(256, 256);
    std::printf("    centre (256,256) = %d %d %d %d\n", m[0], m[1], m[2], m[3]);
    check(std::abs(m[0] - 139) <= 16 && std::abs(m[1] - 105) <= 16 && std::abs(m[2] - 50) <= 16 &&
              m[3] == 255,
          "app icon: centre (256,256) is the brush's brown");
  } else {
    check(false, "app icon: corner (0,0) is the painted opaque white ground");
    check(false, "app icon: centre (256,256) is the brush's brown");
  }

  // 4. The surface handed to SDL, row by row through its pitch.
  {
    std::string why;
    SDL_Surface* s = createAppIconSurface(&why);
    check(s != nullptr, "app icon: createAppIconSurface() succeeds");
    bool same = false;
    if (s != nullptr) {
      same = s->w == img.width && s->h == img.height && s->format == SDL_PIXELFORMAT_RGBA32;
      const auto* p = static_cast<const unsigned char*>(s->pixels);
      for (int y = 0; same && y < s->h; ++y)
        same = std::equal(img.rgba.begin() + static_cast<std::ptrdiff_t>(y) * s->w * 4,
                          img.rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * s->w * 4,
                          p + static_cast<size_t>(y) * static_cast<size_t>(s->pitch));
      SDL_DestroySurface(s);
    }
    check(same, "app icon: surface is RGBA32, 512 x 512, pixels == decode");
  }

  // 5. main.cpp installs it on the real window before the suite runs.
  check(appIconInstalled(), "app icon: installAppIcon() succeeded on the window");

  std::printf("[selftest] app icon %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
