#include "flats/Ink.hpp"

#include <algorithm>
#include <cmath>

namespace np {

FlatInk flatExtractInk(const uint8_t* d, int w, int h, float satTol) {
  const size_t n = static_cast<size_t>(w) * h;
  FlatInk ink(n, 0);
  for (size_t i = 0; i < n; i++) {
    const size_t o = i * 4;
    const int a = d[o + 3];
    if (a == 0) continue;
    const int r = d[o], g = d[o + 1], b = d[o + 2];
    // composite over white
    const double af = a / 255.0;
    const double L = (0.299 * r + 0.587 * g + 0.114 * b) * af + 255.0 * (1.0 - af);
    double dark = (255.0 - L) / 255.0;
    const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double sat = (mx - mn) / 255.0;
    if (sat > satTol) dark *= std::max(0.0, 1.0 - (sat - satTol) / 0.25);
    ink[i] = static_cast<uint8_t>(std::lround(dark * 255.0));
  }
  return ink;
}

FlatMask flatThresholdInk(const FlatInk& ink, float thr) {
  FlatMask line(ink.size(), 0);
  const double t = static_cast<double>(thr) * 255.0;
  for (size_t i = 0; i < ink.size(); i++)
    if (ink[i] > t) line[i] = 1;
  return line;
}

}  // namespace np
