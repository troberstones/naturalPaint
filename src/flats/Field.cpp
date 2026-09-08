#include "flats/Field.hpp"

#include <algorithm>
#include <cmath>

namespace np {

void flatArtStroke(FlatArt& a, int x0, int y0, int x1, int y1, int wpx) {
  const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(x1 - x0, y1 - y0))) * 2);
  for (int s = 0; s <= steps; s++) {
    const int cx = static_cast<int>(std::lround(x0 + (x1 - x0) * static_cast<double>(s) / steps));
    const int cy = static_cast<int>(std::lround(y0 + (y1 - y0) * static_cast<double>(s) / steps));
    for (int dy = 0; dy < wpx; dy++) {
      for (int dx = 0; dx < wpx; dx++) {
        const int x = cx + dx, y = cy + dy;
        if (x < 0 || y < 0 || x >= a.w || y >= a.h) continue;
        a.line[static_cast<size_t>(y) * a.w + x] = 1;
        a.ink[static_cast<size_t>(y) * a.w + x] = 255;
      }
    }
  }
}

void flatArtRect(FlatArt& a, int x0, int y0, int x1, int y1, int gap) {
  flatArtStroke(a, x0, y0, x1, y0);
  flatArtStroke(a, x1, y0, x1, y1);
  flatArtStroke(a, x1, y1, x0, y1);
  const int mid = (y0 + y1) / 2;
  if (gap > 0) {
    flatArtStroke(a, x0, y1, x0, mid + gap / 2);
    flatArtStroke(a, x0, mid - gap / 2, x0, y0);
  } else {
    flatArtStroke(a, x0, y1, x0, y0);
  }
}

FlatArt flatThreeBoxes(int w, int h) {
  FlatArt a = flatBlankArt(w, h);
  flatArtRect(a, 30, 30, 150, 150);
  flatArtRect(a, 180, 30, 300, 150);
  flatArtRect(a, 30, 180, 300, 260);
  return a;
}

FlatArt flatLeakyBox(int gap, int w, int h) {
  FlatArt a = flatBlankArt(w, h);
  flatArtRect(a, 100, 60, 300, 240, gap);
  return a;
}

FlatArt flatHatchedBox(int w, int h) {
  FlatArt a = flatBlankArt(w, h);
  flatArtRect(a, 60, 60, 340, 240);
  for (int x = 80; x < 330; x += 12) flatArtStroke(a, x, 70, x + 30, 230, 2);
  return a;
}

}  // namespace np
