#include "flats/Expand.hpp"

#include <algorithm>

namespace np {

void flatGrowLabels(FlatLabels& labels, int w, int h, const FlatGrowOpts& opts) {
  const FlatMask* blocked = opts.blocked;
  const FlatInk* cost = opts.cost;
  const int32_t maxCost = opts.maxCost;
  const size_t n = static_cast<size_t>(w) * h;
  constexpr int32_t INF = 0x7fffffff;
  FlatDist dist(n, INF);
  // Buckets are exact (integer chamfer costs), so a vector of vectors indexed
  // by cost is a linear-time priority queue. Freed as we go.
  std::vector<std::vector<int32_t>> buckets;
  auto push = [&](int32_t i, int32_t c) {
    if (static_cast<size_t>(c) >= buckets.size()) buckets.resize(static_cast<size_t>(c) + 1);
    buckets[static_cast<size_t>(c)].push_back(i);
  };

  if (opts.seeds) {
    for (size_t k = 0; k < opts.seedCount; k++) {
      const int32_t s = opts.seeds[k];
      dist[s] = 0;
      push(s, 0);
    }
  } else {
    for (size_t i = 0; i < n; i++)
      if (labels[i]) {
        dist[i] = 0;
        push(static_cast<int32_t>(i), 0);
      }
  }

  for (size_t c = 0; c < buckets.size(); c++) {
    // b may grow while we walk it (an entry at cost c pushes nothing at cost
    // c, since every step costs >= 3), so index access is safe here.
    for (size_t k = 0; k < buckets[c].size(); k++) {
      const int32_t p = buckets[c][k];
      if (dist[p] != static_cast<int32_t>(c)) continue;  // stale entry
      const int32_t id = labels[p];
      const int x = p % w, y = p / w;
      for (int dy = -1; dy <= 1; dy++) {
        const int qy = y + dy;
        if (qy < 0 || qy >= h) continue;
        for (int dx = -1; dx <= 1; dx++) {
          if (!dx && !dy) continue;
          const int qx = x + dx;
          if (qx < 0 || qx >= w) continue;
          const int32_t q = qy * w + qx;
          if (blocked && (*blocked)[q]) continue;
          // fixed pixel (labeled but not a seed): impassable
          if (labels[q] && dist[q] == INF) continue;
          // don't slip diagonally between two blocked pixels
          if (dx && dy && blocked && (*blocked)[y * w + qx] && (*blocked)[qy * w + x]) continue;
          // ink 255 adds ~23 units (≈8 px) -- crossing a faint stroke is expensive
          const int32_t nd = static_cast<int32_t>(c) + (dx && dy ? 4 : 3) +
                             (cost ? (static_cast<int32_t>((*cost)[q]) * 3) >> 5 : 0);
          if (nd > maxCost || nd >= dist[q]) continue;
          dist[q] = nd;
          labels[q] = id;
          push(q, nd);
        }
      }
    }
    std::vector<int32_t>().swap(buckets[c]);  // free as we go
  }
}

FlatLabels flatExpandLabels(const FlatLabels& core, int w, int h, const FlatInk* ink) {
  FlatLabels labels = core;
  FlatGrowOpts o;
  o.cost = ink;
  flatGrowLabels(labels, w, h, o);
  return labels;
}

}  // namespace np
