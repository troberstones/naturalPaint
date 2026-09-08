#include "flats/Morphology.hpp"

#include <algorithm>
#include <unordered_set>

namespace np {

FlatDist flatDistanceTransform(const FlatMask& mask, int w, int h) {
  constexpr int32_t INF = 1 << 29;
  const size_t n = static_cast<size_t>(w) * h;
  FlatDist d(n);
  for (size_t i = 0; i < n; i++) d[i] = mask[i] ? 0 : INF;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      int32_t v = d[i];
      if (v == 0) continue;
      if (x > 0 && d[i - 1] + 3 < v) v = d[i - 1] + 3;
      if (y > 0) {
        if (d[i - w] + 3 < v) v = d[i - w] + 3;
        if (x > 0 && d[i - w - 1] + 4 < v) v = d[i - w - 1] + 4;
        if (x < w - 1 && d[i - w + 1] + 4 < v) v = d[i - w + 1] + 4;
      }
      d[i] = v;
    }
  }
  for (int y = h - 1; y >= 0; y--) {
    for (int x = w - 1; x >= 0; x--) {
      const size_t i = static_cast<size_t>(y) * w + x;
      int32_t v = d[i];
      if (v == 0) continue;
      if (x < w - 1 && d[i + 1] + 3 < v) v = d[i + 1] + 3;
      if (y < h - 1) {
        if (d[i + w] + 3 < v) v = d[i + w] + 3;
        if (x < w - 1 && d[i + w + 1] + 4 < v) v = d[i + w + 1] + 4;
        if (x > 0 && d[i + w - 1] + 4 < v) v = d[i + w - 1] + 4;
      }
      d[i] = v;
    }
  }
  return d;
}

FlatMask flatSkeletonize(const FlatMask& mask, int w, int h) {
  FlatMask sk = mask;
  // Clear the frame so the 8-neighbourhood reads below never leave the buffer.
  for (int x = 0; x < w; x++) {
    sk[x] = 0;
    sk[static_cast<size_t>(h - 1) * w + x] = 0;
  }
  for (int y = 0; y < h; y++) {
    sk[static_cast<size_t>(y) * w] = 0;
    sk[static_cast<size_t>(y) * w + w - 1] = 0;
  }
  std::vector<int32_t> cand;
  for (size_t i = 0; i < sk.size(); i++)
    if (sk[i]) cand.push_back(static_cast<int32_t>(i));
  const int nbr[8] = {-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1};
  // "next" is a set in the source; a stamp buffer gives the same membership
  // without hashing every candidate.
  std::vector<uint32_t> stamp(sk.size(), 0);
  uint32_t gen = 0;
  std::vector<int32_t> next, del;
  for (int iter = 0; iter < 300 && !cand.empty(); iter++) {
    gen++;
    next.clear();
    bool removed = false;
    for (int phase = 0; phase < 2; phase++) {
      del.clear();
      for (const int32_t i : cand) {
        if (!sk[i]) continue;
        const int p2 = sk[i - w], p3 = sk[i - w + 1], p4 = sk[i + 1], p5 = sk[i + w + 1],
                  p6 = sk[i + w], p7 = sk[i + w - 1], p8 = sk[i - 1], p9 = sk[i - w - 1];
        const int B = p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9;
        if (B < 2 || B > 6) continue;
        int A = 0;
        if (!p2 && p3) A++;
        if (!p3 && p4) A++;
        if (!p4 && p5) A++;
        if (!p5 && p6) A++;
        if (!p6 && p7) A++;
        if (!p7 && p8) A++;
        if (!p8 && p9) A++;
        if (!p9 && p2) A++;
        if (A != 1) continue;
        if (phase == 0) {
          if ((p2 && p4 && p6) || (p4 && p6 && p8)) continue;
        } else {
          if ((p2 && p4 && p8) || (p2 && p6 && p8)) continue;
        }
        del.push_back(i);
      }
      for (const int32_t i : del) {
        sk[i] = 0;
        removed = true;
        for (const int d : nbr) {
          const int32_t q = i + d;
          if (sk[q] && stamp[q] != gen) {
            stamp[q] = gen;
            next.push_back(q);
          }
        }
      }
    }
    if (!removed) break;
    cand.swap(next);
  }
  return sk;
}

FlatMask flatInvertMask(const FlatMask& mask) {
  FlatMask inv(mask.size());
  for (size_t i = 0; i < mask.size(); i++) inv[i] = mask[i] ? 0 : 1;
  return inv;
}

FlatMask flatSmoothMask(const FlatMask& line, int w, int h, int r, int despeckle) {
  const size_t n = static_cast<size_t>(w) * h;
  FlatMask m;
  if (r > 0) {
    const FlatDist d1 = flatDistanceTransform(line, w, h);
    FlatMask dil(n, 0);
    for (size_t i = 0; i < n; i++)
      if (d1[i] <= 3 * r) dil[i] = 1;
    const FlatDist d2 = flatDistanceTransform(flatInvertMask(dil), w, h);
    m.assign(n, 0);
    for (size_t i = 0; i < n; i++)
      if (d2[i] > 3 * r || line[i]) m[i] = 1;
  } else {
    m = line;
  }
  if (despeckle > 0) {
    // remove tiny isolated line components (dust); fills flow under the dots anyway
    std::vector<uint8_t> seen(n, 0);
    std::vector<int32_t> stack, comp;
    for (size_t s = 0; s < n; s++) {
      if (!m[s] || seen[s]) continue;
      stack.clear();
      stack.push_back(static_cast<int32_t>(s));
      seen[s] = 1;
      comp.clear();
      comp.push_back(static_cast<int32_t>(s));
      while (!stack.empty()) {
        const int32_t p = stack.back();
        stack.pop_back();
        const int x = p % w, y = p / w;
        for (int dy = -1; dy <= 1; dy++) {
          for (int dx = -1; dx <= 1; dx++) {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
            const int32_t q = qy * w + qx;
            if (m[q] && !seen[q]) {
              seen[q] = 1;
              stack.push_back(q);
              if (static_cast<int>(comp.size()) <= despeckle) comp.push_back(q);
            }
          }
        }
      }
      if (static_cast<int>(comp.size()) <= despeckle)
        for (const int32_t p : comp) m[p] = 0;
    }
  }
  return m;
}

}  // namespace np
