#include "flats/Segment.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "flats/Expand.hpp"
#include "flats/Morphology.hpp"

namespace np {

namespace {

// Union-find over region ids with path halving, shared by every merge pass.
struct UnionFind {
  std::vector<int32_t> parent;
  explicit UnionFind(int n) : parent(static_cast<size_t>(n) + 1) {
    std::iota(parent.begin(), parent.end(), 0);
  }
  int32_t find(int32_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
  }
};

int maxLabel(const FlatLabels& v) {
  int32_t m = 0;
  for (const int32_t x : v) m = std::max(m, x);
  return m;
}

// Adjacency tallies keyed on (a, b), kept in FIRST-CONTACT order.
//
// The source keeps these in a JavaScript Map, whose iteration order is
// insertion order, and every "merge into the best neighbour" pass below
// breaks ties by that order. An unordered_map would make the tie-break depend
// on hash layout, and the port would then differ from the reference by a few
// boundary pixels on any drawing with a tie -- which is exactly the kind of
// drift app/selftest/Flats.cpp's fixture hashes exist to catch. Rows are
// indexed by region id (ids are bounded by the label maximum), and `order`
// records the rows in the order they were first written.
template <class V>
struct OrderedTally {
  explicit OrderedTally(int maxId) : rows(static_cast<size_t>(maxId) + 1) {}
  std::vector<std::vector<std::pair<int32_t, V>>> rows;
  std::unordered_map<uint64_t, size_t> at;
  std::vector<int32_t> order;
  V& get(int32_t a, int32_t b) {
    const uint64_t k = (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b);
    auto it = at.find(k);
    if (it != at.end()) return rows[a][it->second].second;
    if (rows[a].empty()) order.push_back(a);
    rows[a].emplace_back(b, V{});
    at.emplace(k, rows[a].size() - 1);
    return rows[a].back().second;
  }
  bool has(int32_t a) const { return !rows[a].empty(); }
};

}  // namespace

FlatTrappedBallResult flatTrappedBall(const FlatMask& line, int w, int h, int maxGap, const FlatInk* ink,
                              bool attach) {
  const size_t n = static_cast<size_t>(w) * h;
  const FlatDist dist = flatDistanceTransform(line, w, h);
  FlatLabels core(n, 0);
  std::vector<int32_t> queue(n);
  int32_t next = 0;

  std::vector<int> radii;
  for (int r = std::max(1, maxGap); r >= 1; r = r > 4 ? r >> 1 : r - 1) radii.push_back(r);

  for (const int r : radii) {
    const int32_t thr = r * 3;
    size_t total = 0;
    for (size_t s = 0; s < n; s++) {
      if (core[s] || line[s] || dist[s] <= thr) continue;
      // flood one ball-fitting component, appending to the shared queue
      const int32_t id = ++next;
      size_t sp = total;
      core[s] = id;
      queue[total++] = static_cast<int32_t>(s);
      while (sp < total) {
        const int32_t p = queue[sp++];
        const int x = p % w;
        int32_t q = p - 1;
        if (x > 0 && !core[q] && !line[q] && dist[q] > thr) { core[q] = id; queue[total++] = q; }
        q = p + 1;
        if (x < w - 1 && !core[q] && !line[q] && dist[q] > thr) { core[q] = id; queue[total++] = q; }
        q = p - w;
        if (q >= 0 && !core[q] && !line[q] && dist[q] > thr) { core[q] = id; queue[total++] = q; }
        q = p + w;
        if (static_cast<size_t>(q) < n && !core[q] && !line[q] && dist[q] > thr) { core[q] = id; queue[total++] = q; }
      }
    }
    if (total) {
      FlatGrowOpts o;
      o.blocked = &line;
      o.maxCost = thr;
      o.seeds = queue.data();
      o.seedCount = total;
      flatGrowLabels(core, w, h, o);
    }
  }

  if (attach) {
    // attach every remaining free pixel to its nearest connected region;
    // ink-weighted so faint sub-threshold strokes still act as soft walls
    FlatGrowOpts o;
    o.blocked = &line;
    o.cost = ink;
    flatGrowLabels(core, w, h, o);
    next = flatLabelPockets(core, line, w, h);
  }

  return {std::move(core), next};
}

int flatLabelPockets(FlatLabels& core, const FlatMask& line, int w, int h) {
  const size_t n = static_cast<size_t>(w) * h;
  int32_t next = maxLabel(core);
  std::vector<int32_t> stack;
  for (size_t s = 0; s < n; s++) {
    if (core[s] || line[s]) continue;
    const int32_t id = ++next;
    core[s] = id;
    stack.clear();
    stack.push_back(static_cast<int32_t>(s));
    while (!stack.empty()) {
      const int32_t p = stack.back();
      stack.pop_back();
      const int x = p % w;
      const int32_t qs[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p - w, p + w};
      for (const int32_t q : qs) {
        if (q < 0 || static_cast<size_t>(q) >= n || core[q] || line[q]) continue;
        core[q] = id;
        stack.push_back(q);
      }
    }
  }
  return next;
}

FlatFinalizeResult flatFinalizeRegions(FlatLabels& core, FlatLabels& labels, int w, int h, int minArea) {
  const size_t n = static_cast<size_t>(w) * h;
  const int maxId = maxLabel(core);
  std::vector<int32_t> area(static_cast<size_t>(maxId) + 1, 0);
  for (size_t i = 0; i < n; i++) area[core[i]]++;

  // boundary lengths between expanded labels
  OrderedTally<int32_t> nbr(maxId);
  auto bump = [&](int32_t a, int32_t b) { nbr.get(a, b)++; };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int32_t a = labels[i];
      if (x < w - 1) {
        const int32_t b = labels[i + 1];
        if (a != b) { bump(a, b); bump(b, a); }
      }
      if (y < h - 1) {
        const int32_t b = labels[i + w];
        if (a != b) { bump(a, b); bump(b, a); }
      }
    }
  }

  // union-find, absorb smallest first
  UnionFind uf(maxId);
  std::vector<int32_t> ids(static_cast<size_t>(maxId));
  std::iota(ids.begin(), ids.end(), 1);
  std::stable_sort(ids.begin(), ids.end(), [&](int32_t a, int32_t b) { return area[a] < area[b]; });
  for (const int32_t s : ids) {
    if (area[uf.find(s)] >= minArea) continue;
    if (!nbr.has(s)) continue;
    int32_t best = 0, bw = -1;
    for (const auto& [b, wgt] : nbr.rows[s]) {
      const int32_t rb = uf.find(b);
      if (rb != uf.find(s) && wgt > bw) { bw = wgt; best = rb; }
    }
    if (best) {
      const int32_t rs = uf.find(s);
      area[best] += area[rs];
      uf.parent[rs] = best;
    }
  }

  // compact ids
  std::vector<int32_t> remap(static_cast<size_t>(maxId) + 1, 0);
  int K = 0;
  for (int32_t i = 1; i <= maxId; i++)
    if (uf.find(i) == i && area[i] > 0) remap[i] = ++K;
  for (int32_t i = 1; i <= maxId; i++) remap[i] = remap[uf.find(i)];
  for (size_t i = 0; i < n; i++) {
    core[i] = remap[core[i]];
    labels[i] = remap[labels[i]];
  }

  std::vector<int32_t> outArea(static_cast<size_t>(K) + 1, 0);
  for (size_t i = 0; i < n; i++) outArea[core[i]]++;
  std::vector<uint8_t> isBg(static_cast<size_t>(K) + 1, 0);
  for (int x = 0; x < w; x++) {
    isBg[labels[x]] = 1;
    isBg[labels[static_cast<size_t>(h - 1) * w + x]] = 1;
  }
  for (int y = 0; y < h; y++) {
    isBg[labels[static_cast<size_t>(y) * w]] = 1;
    isBg[labels[static_cast<size_t>(y) * w + w - 1]] = 1;
  }

  FlatFinalizeResult out;
  out.count = K;
  out.regions.reserve(static_cast<size_t>(K));
  for (int i = 1; i <= K; i++) out.regions.push_back({i, outArea[i], isBg[i] != 0});
  std::stable_sort(out.regions.begin(), out.regions.end(),
                   [](const FlatRegionInfo& a, const FlatRegionInfo& b) { return a.area > b.area; });
  return out;
}

bool flatMergeSlivers(FlatLabels& core, FlatLabels& labels, const FlatMask& line, int w, int h, int sliverW) {
  if (sliverW <= 0) return false;
  const size_t n = static_cast<size_t>(w) * h;
  const FlatDist ld = flatDistanceTransform(line, w, h);
  const int maxId = maxLabel(core);
  std::vector<int32_t> maxD(static_cast<size_t>(maxId) + 1, 0);
  for (size_t i = 0; i < n; i++)
    if (core[i] && ld[i] > maxD[core[i]]) maxD[core[i]] = ld[i];
  std::vector<uint8_t> thin(static_cast<size_t>(maxId) + 1, 0);
  bool any = false;
  for (int id = 1; id <= maxId; id++)
    if (maxD[id] > 0 && maxD[id] <= sliverW * 3) { thin[id] = 1; any = true; }
  if (!any) return false;

  // neighbour tallies for thin regions: open borders weighted heavily
  constexpr int32_t OPEN = 7;
  OrderedTally<int32_t> tally(maxId);
  auto bump = [&](int32_t a, int32_t b, int32_t wgt) {
    if (!thin[a]) return;
    tally.get(a, b) += wgt;
  };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int32_t a = labels[i];
      const int32_t qs[2] = {x < w - 1 ? static_cast<int32_t>(i + 1) : -1,
                             y < h - 1 ? static_cast<int32_t>(i + w) : -1};
      for (const int32_t q : qs) {
        if (q < 0) continue;
        const int32_t b = labels[q];
        if (a == b) continue;
        const int32_t wgt = ld[i] > OPEN && ld[q] > OPEN ? 20 : 1;
        bump(a, b, wgt);
        bump(b, a, wgt);
      }
    }
  }

  UnionFind uf(maxId);
  for (const int32_t id : tally.order) {
    int32_t best = 0, bw = -1;
    for (const auto& [b, wgt] : tally.rows[id]) {
      // prefer merging into a non-thin region when weights tie-ish
      const int32_t ww = wgt * (thin[b] ? 1 : 2);
      if (ww > bw) { bw = ww; best = b; }
    }
    if (best) {
      const int32_t ra = uf.find(id), rb = uf.find(best);
      if (ra != rb) uf.parent[ra] = rb;
    }
  }
  for (size_t i = 0; i < n; i++) {
    core[i] = uf.find(core[i]);
    labels[i] = uf.find(labels[i]);
  }
  return true;
}

std::optional<FlatDeclutterOpts> flatDeclutterOpts(int strength) {
  if (strength <= 0) return std::nullopt;
  const float s = static_cast<float>(std::min(100, strength));
  return FlatDeclutterOpts{static_cast<int>(100 + 12 * s), 2.0f + 0.03f * s, 0.32f - 0.0017f * s};
}

std::vector<uint8_t> flatBackgroundLut(const std::vector<FlatRegionInfo>& regions) {
  int mx = 0;
  for (const auto& r : regions) mx = std::max(mx, r.id);
  std::vector<uint8_t> lut(static_cast<size_t>(mx) + 1, 0);
  for (const auto& r : regions)
    if (r.isBg) lut[r.id] = 1;
  return lut;
}

bool flatDeclutter(FlatLabels& core, FlatLabels& labels, const FlatMask& line, int w, int h,
               const std::optional<FlatDeclutterOpts>& optsIn, const std::vector<uint8_t>* isBg) {
  if (!optsIn) return false;
  const FlatDeclutterOpts& opts = *optsIn;
  // Stroke half-width (px) at or above which a boundary counts as a contour --
  // a drawn edge between two areas -- rather than a hatch/detail mark. Fills
  // must never merge across one.
  constexpr int STRONG_PX = 2;
  const size_t n = static_cast<size_t>(w) * h;
  const FlatDist ld = flatDistanceTransform(line, w, h);
  const int maxId = maxLabel(core);
  if (maxId < 2) return false;
  auto bg = [&](int32_t id) { return isBg && static_cast<size_t>(id) < isBg->size() && (*isBg)[id]; };

  // per-region geometry, measured on `core` (the open-space part of a region;
  // `labels` also covers the pixels grown under the strokes, whose distance is
  // zero and would drag every mean down)
  std::vector<int32_t> area(static_cast<size_t>(maxId) + 1, 0);
  std::vector<double> sumD(static_cast<size_t>(maxId) + 1, 0), sumX(sumD), sumY(sumD);
  for (size_t i = 0; i < n; i++) {
    const int32_t id = core[i];
    if (!id) continue;
    area[id]++;
    sumD[id] += ld[i];
    sumX[id] += static_cast<double>(i % w);
    sumY[id] += static_cast<double>(i / w);
  }

  // summed-area table of the line mask, for O(1) local ink density
  const int S = w + 1;
  std::vector<double> ii(static_cast<size_t>(S) * (h + 1), 0.0);
  for (int y = 0; y < h; y++) {
    double run = 0;
    for (int x = 0; x < w; x++) {
      run += line[static_cast<size_t>(y) * w + x];
      ii[static_cast<size_t>(y + 1) * S + x + 1] = ii[static_cast<size_t>(y) * S + x + 1] + run;
    }
  }
  constexpr int R = 20;  // ~41px window: wide enough to see a hatch patch, not a whole limb
  auto density = [&](int x, int y) -> double {
    const int x0 = std::max(0, x - R), y0 = std::max(0, y - R);
    const int x1 = std::min(w, x + R), y1 = std::min(h, y + R);
    const double s = ii[static_cast<size_t>(y1) * S + x1] - ii[static_cast<size_t>(y0) * S + x1] -
                     ii[static_cast<size_t>(y1) * S + x0] + ii[static_cast<size_t>(y0) * S + x0];
    return s / std::max(1, (x1 - x0) * (y1 - y0));
  };

  std::vector<uint8_t> clutter(static_cast<size_t>(maxId) + 1, 0);
  bool any = false;
  for (int32_t id = 1; id <= maxId; id++) {
    if (!area[id] || bg(id)) continue;
    if (area[id] >= opts.maxArea) continue;
    if (sumD[id] / area[id] / 3.0 >= opts.maxMeanD) continue;  // chamfer units -> px
    const int cx = static_cast<int>(std::lround(sumX[id] / area[id]));
    const int cy = static_cast<int>(std::lround(sumY[id] / area[id]));
    if (density(cx, cy) <= opts.minDensity) continue;
    clutter[id] = 1;
    any = true;
  }
  if (!any) return false;

  // Shared boundaries, classified by WHAT separates the two regions. Growth
  // stops at the medial axis of the separating stroke, so the distance to the
  // nearest non-line pixel at a boundary is that stroke's half-width:
  //   open   - no stroke at all (an arbitrary cut through free space)
  //   thin   - a hatch/detail mark; absorbing it is the whole point
  //   strong - a contour. Merging across one lets a fill invade the area on the
  //            other side of a drawn line, which is never right.
  const FlatDist wd = flatDistanceTransform(flatInvertMask(line), w, h);
  struct B { int32_t total = 0, open = 0, thin = 0, strong = 0; };
  OrderedTally<B> nbr(maxId);
  auto bump = [&](int32_t a, int32_t b, int cls) {
    B& e = nbr.get(a, b);
    e.total++;
    if (cls == 0) e.open++;
    else if (cls == 1) e.thin++;
    else e.strong++;
  };
  constexpr int32_t OPEN = 7;  // chamfer units: > ~2.3px from any line
  auto classify = [&](size_t i, size_t q) -> int {
    if (ld[i] > OPEN && ld[q] > OPEN) return 0;
    return std::max(wd[i], wd[q]) / 3.0 >= STRONG_PX ? 2 : 1;
  };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int32_t a = labels[i];
      if (x < w - 1) {
        const int32_t b = labels[i + 1];
        if (a != b) { const int c = classify(i, i + 1); bump(a, b, c); bump(b, a, c); }
      }
      if (y < h - 1) {
        const int32_t b = labels[i + w];
        if (a != b) { const int c = classify(i, i + w); bump(a, b, c); bump(b, a, c); }
      }
    }
  }
  // A merge may not cross a predominantly-contour boundary, and needs some
  // genuine open/thin contact to justify it. Open contact counts far more.
  auto allowed = [](const B& e) { return e.strong <= 0.4 * e.total && e.open + e.thin > 0; };
  auto score = [](const B& e) { return static_cast<double>(e.open) * 20 + e.thin; };

  UnionFind uf(maxId);
  auto unite = [&](int32_t a, int32_t b) {
    const int32_t ra = uf.find(a), rb = uf.find(b);
    if (ra != rb) uf.parent[ra] = rb;
  };

  // pass 1: a hatched patch is many touching clutter cells -- collapse it first
  // (but never across a contour: two cells either side of a drawn edge are two
  // different areas, however cluttered the neighbourhood is)
  for (const int32_t a : nbr.order) {
    if (!clutter[a]) continue;
    for (const auto& [b, e] : nbr.rows[a])
      if (b && clutter[b] && allowed(e)) unite(a, b);
  }
  // pass 2: attach each collapsed patch to the real area it shades
  OrderedTally<B> tally(maxId);
  for (const int32_t a : nbr.order) {
    if (!a || !clutter[a]) continue;
    const int32_t ra = uf.find(a);
    for (const auto& [b, e] : nbr.rows[a]) {
      if (!b || clutter[b] || uf.find(b) == ra) continue;
      B& acc = tally.get(ra, uf.find(b));
      acc.total += e.total; acc.open += e.open; acc.thin += e.thin; acc.strong += e.strong;
    }
  }
  for (const int32_t ra : tally.order) {
    int32_t best = 0;
    double bw = 0;
    for (const auto& [b, e] : tally.rows[ra]) {
      if (!allowed(e)) continue;  // would cross a drawn line
      const double s = score(e) * (bg(b) ? 0.05 : 1.0);
      if (s > bw) { bw = s; best = b; }
    }
    // no acceptable host: this is a real enclosed area, so leave it alone
    if (best) unite(ra, best);
  }

  for (size_t i = 0; i < n; i++) {
    core[i] = uf.find(core[i]);
    labels[i] = uf.find(labels[i]);
  }
  return true;
}

void flatApplyMerges(FlatLabels& core, FlatLabels& labels, const std::vector<std::pair<int, int>>& merges) {
  int mx = maxLabel(core);
  for (const auto& [a, b] : merges) mx = std::max({mx, a, b});
  UnionFind uf(mx);
  for (const auto& [a, b] : merges) {
    const int32_t ra = uf.find(a), rb = uf.find(b);
    if (ra != rb) uf.parent[rb] = ra;
  }
  for (size_t i = 0; i < core.size(); i++) {
    core[i] = uf.find(core[i]);
    labels[i] = uf.find(labels[i]);
  }
}

}  // namespace np
