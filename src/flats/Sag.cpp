#include "flats/Sag.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "flats/Membrane.hpp"

namespace np {

namespace {

constexpr int SUB = 8;       // sag quantisation, 1/8 px
constexpr float REL = 0.3f;  // also merge when the col is within 30% of the peak
// How much wider the opening has to be before an ENCLOSED basin is absorbed.
// Two basins that both reach the image frame are two lobes of the same open
// paper and merge at plain `wide` -- that is what keeps the background a
// single region. A basin that never reaches the frame is a drawn area, and
// horns, fingers and hair open onto the background through necks several
// stroke-gaps across, so absorbing them at `wide` loses them. Measured over
// the seven samples, separating the two cases cuts the area lost to the
// background by 2-8x (Lineart3 5.27% -> 0.67%) while the background stays one
// region.
constexpr int ENCLOSED = 3;

constexpr int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};

// Local maxima of the quantised field, grouped into plateaus. Each plateau is
// one seed: the deepest point of one valley. Returns peaks[id] = height.
std::vector<int32_t> seedMaxima(const std::vector<int32_t>& q, int w, int h, FlatLabels& seedOf) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<uint8_t> isMax(n, 0);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      if (!q[i]) continue;
      const int32_t v = q[i];
      uint8_t top = 1;
      for (int dy = -1; dy <= 1 && top; dy++) {
        const int ny = y + dy;
        if (ny < 0 || ny >= h) continue;
        for (int dx = -1; dx <= 1; dx++) {
          const int nx = x + dx;
          if ((!dx && !dy) || nx < 0 || nx >= w) continue;
          if (q[static_cast<size_t>(ny) * w + nx] > v) { top = 0; break; }
        }
      }
      isMax[i] = top;
    }
  }
  std::vector<int32_t> peaks{0};  // 1-based ids
  std::vector<int32_t> stack;
  for (size_t s = 0; s < n; s++) {
    if (!isMax[s] || seedOf[s]) continue;
    const int32_t id = static_cast<int32_t>(peaks.size());
    peaks.push_back(q[s]);
    seedOf[s] = id;
    stack.clear();
    stack.push_back(static_cast<int32_t>(s));
    while (!stack.empty()) {
      const int32_t p = stack.back();
      stack.pop_back();
      const int px = p % w, py = p / w;
      for (int dy = -1; dy <= 1; dy++) {
        const int ny = py + dy;
        if (ny < 0 || ny >= h) continue;
        for (int dx = -1; dx <= 1; dx++) {
          const int nx = px + dx;
          if ((!dx && !dy) || nx < 0 || nx >= w) continue;
          const int32_t nn = ny * w + nx;
          if (!isMax[nn] || seedOf[nn] || q[nn] != q[s]) continue;
          seedOf[nn] = id;
          stack.push_back(nn);
        }
      }
    }
  }
  return peaks;
}

// Follow the ridge downhill from a col until it meets ink; returns that pixel,
// or -1 if the walk runs out of budget or bottoms out in the open.
int32_t ridgeWalk(int32_t s, int dir, const std::vector<int32_t>& q, const FlatMask& line, int w, int h,
                  int budget) {
  int32_t p = s;
  int d = dir;
  for (int step = 0; step < budget; step++) {
    const int x = p % w, y = p / w;
    int32_t best = -1;
    int32_t bq = std::numeric_limits<int32_t>::max();
    int bd = d;
    for (int k = -2; k <= 2; k++) {  // stay roughly on course
      const int nd = (d + k + 8) % 8;
      const int nx = x + DX[nd], ny = y + DY[nd];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      const int32_t nn = ny * w + nx;
      if (line[nn]) return nn;  // reached the stroke
      if (q[nn] < bq) { bq = q[nn]; best = nn; bd = nd; }
    }
    if (best < 0 || bq >= q[p]) return -1;  // no way down: not a real leak
    p = best;
    d = bd;
  }
  return -1;
}

// A watershed line has to be JUSTIFIED by ink. Walking downhill from a col
// runs along the ridge (both basins are uphill), so if the artist left a
// break in a stroke the two walks arrive at the stroke tips that flank it --
// and the segment between them is the line they left out. If instead the
// walks wander off without finding ink, nothing in the drawing supports a
// boundary there: it is the seam between two lobes of one wide-open area, and
// the two basins belong together. This one test replaces an absolute width
// threshold, which cannot tell a hundred-pixel background waist from a wide
// silhouette break.
bool inkJustifies(int32_t p, const std::vector<int32_t>& q, const FlatMask& line, int w, int h, int budget) {
  const int x = p % w, y = p / w;
  int d0 = -1;
  int32_t bq = std::numeric_limits<int32_t>::max();
  for (int d = 0; d < 8; d++) {
    const int nx = x + DX[d], ny = y + DY[d];
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
    const int32_t nn = ny * w + nx;
    if (!line[nn] && q[nn] < bq) { bq = q[nn]; d0 = d; }
  }
  if (d0 < 0) return false;
  const int32_t a = ridgeWalk(p, d0, q, line, w, h, budget);
  if (a < 0) return false;
  const int32_t b = ridgeWalk(p, (d0 + 4) % 8, q, line, w, h, budget);
  if (b < 0) return false;
  // both tips found, and close enough together to be one break rather than two
  // unrelated strokes the walks happened to bump into
  return std::hypot(static_cast<double>(b % w - a % w), static_cast<double>(b / w - a / w)) <= budget;
}

struct UnionFind {
  std::vector<int32_t> parent;
  explicit UnionFind(int n) : parent(static_cast<size_t>(n) + 1) { std::iota(parent.begin(), parent.end(), 0); }
  int32_t find(int32_t i) {
    while (parent[i] != i) i = parent[i] = parent[parent[i]];
    return i;
  }
};

}  // namespace

FlatLabels flatSagWatershed(const std::vector<float>& sag, const FlatMask& line, int w, int h, float tauPx,
                    int maxGap) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<int32_t> q(n, 0);
  int32_t qmax = 0;
  for (size_t i = 0; i < n; i++) {
    if (line[i]) continue;
    const int32_t v = std::max<int32_t>(1, static_cast<int32_t>(std::lround(sag[i] * SUB)));
    q[i] = v;
    if (v > qmax) qmax = v;
  }

  FlatLabels core(n, 0);
  const std::vector<int32_t> peaks = seedMaxima(q, w, h, core);
  const int K = static_cast<int>(peaks.size()) - 1;

  // union-find over seeds, with each component's highest peak
  UnionFind uf(K);
  std::vector<int32_t> peak(static_cast<size_t>(K) + 1, 0);
  for (int i = 0; i <= K; i++) peak[i] = peaks[i];

  const int32_t tau = std::max<int32_t>(1, static_cast<int32_t>(std::lround(tauPx * SUB)));

  // Descending priority flood. Buckets are exact (the field is quantised), so
  // this is a linear-time watershed rather than a heap.
  std::vector<std::vector<int32_t>> buckets(static_cast<size_t>(qmax) + 1);
  for (size_t i = 0; i < n; i++)
    if (core[i]) buckets[q[i]].push_back(static_cast<int32_t>(i));

  for (int32_t c = qmax; c >= 1; c--) {
    std::vector<int32_t>& b = buckets[c];
    for (size_t k = 0; k < b.size(); k++) {  // b grows while we walk it
      const int32_t p = b[k];
      const int32_t a = core[p];
      if (!a) continue;
      const int x = p % w, y = p / w;
      for (int d = 0; d < 8; d++) {
        const int nx = x + DX[d], ny = y + DY[d];
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const int32_t nn = ny * w + nx;
        if (line[nn]) continue;
        // never slip diagonally between two ink pixels: a 1px stroke drawn on
        // the diagonal would otherwise let two areas touch straight through it
        if (DX[d] && DY[d] && line[y * w + nx] && line[ny * w + x]) continue;
        const int32_t o = core[nn];
        if (!o) {
          core[nn] = a;
          const int32_t nq = q[nn] < c ? q[nn] : c;  // never climb: keep the queue monotone
          buckets[nq].push_back(nn);                 // nq == c appends to b, walked below
          continue;
        }
        const int32_t ra = uf.find(a), rb = uf.find(o);
        if (ra == rb) continue;
        // First contact is the col. Later contacts sit lower, so they can only
        // be less mergeable -- re-evaluating them is harmless.
        // A basin that barely stands above where it meets its neighbour is a
        // bulge, a limb, a waist -- not a thing of its own.
        const int32_t lo = peak[ra] < peak[rb] ? peak[ra] : peak[rb];
        const int32_t pers = lo - c;
        if (pers < tau || pers < REL * lo) {
          uf.parent[rb] = ra;
          if (peak[rb] > peak[ra]) peak[ra] = peak[rb];
        }
      }
    }
    std::vector<int32_t>().swap(buckets[c]);
  }

  for (size_t i = 0; i < n; i++)
    if (core[i]) core[i] = uf.find(core[i]);

  // Two regions can only touch in open space -- the flood never crosses ink --
  // so every surviving adjacency marks a place a stroke failed to close. A
  // given pair may touch in several places (a silhouette broken twice, say),
  // and each has to answer for itself: one gap the artist clearly meant to
  // leave open must not excuse a second one they clearly meant to close. So
  // group the contact pixels into connected SITES and take the highest point
  // of each -- the col the fills would pour through there.
  constexpr int64_t KEY = 1048576;
  auto colSites = [&]() {
    std::unordered_map<int64_t, std::vector<int32_t>> byPair;
    std::vector<int64_t> order;  // insertion order, for determinism
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const int32_t i = y * w + x;
        const int32_t a = core[i];
        if (!a) continue;
        for (int d = 0; d < 4; d++) {  // right/down-right/down/down-left
          const int nx = x + DX[d], ny = y + DY[d];
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const int32_t nn = ny * w + nx;
          const int32_t o = core[nn];
          if (!o || o == a) continue;
          if (DX[d] && DY[d] && line[y * w + nx] && line[ny * w + x]) continue;
          const int64_t key = a < o ? static_cast<int64_t>(a) * KEY + o : static_cast<int64_t>(o) * KEY + a;
          auto it = byPair.find(key);
          if (it == byPair.end()) {
            byPair.emplace(key, std::vector<int32_t>{i});
            order.push_back(key);
          } else {
            it->second.push_back(i);
          }
        }
      }
    }
    std::vector<std::pair<int64_t, int32_t>> sites;  // [pair key, col pixel]
    std::vector<int32_t> stack;
    for (const int64_t key : order) {
      const std::vector<int32_t>& pts = byPair[key];
      std::unordered_set<int32_t> open(pts.begin(), pts.end());
      for (const int32_t s : pts) {
        if (!open.erase(s)) continue;
        int32_t best = s;
        stack.clear();
        stack.push_back(s);
        while (!stack.empty()) {
          const int32_t p = stack.back();
          stack.pop_back();
          if (q[p] > q[best]) best = p;
          const int px = p % w, py = p / w;
          for (int dy = -1; dy <= 1; dy++) {
            const int ny = py + dy;
            if (ny < 0 || ny >= h) continue;
            for (int dx = -1; dx <= 1; dx++) {
              const int nx = px + dx;
              if ((!dx && !dy) || nx < 0 || nx >= w) continue;
              const int32_t nn = ny * w + nx;
              if (open.erase(nn)) stack.push_back(nn);
            }
          }
        }
        sites.emplace_back(key, best);
      }
    }
    return sites;
  };

  // What each site is worth, by how wide the opening is:
  //   under minCol   a pinhole. No fill pours through it; ignore it entirely.
  //   up to `wide`   narrower than the trapped ball, so the two sides stay
  //                  separate on the same guarantee trapped-ball segmentation
  //                  gives -- a ball of radius r cannot pass a gap under 2r.
  //                  An enclosed basin gets ENCLOSED times the allowance (see
  //                  below), because a drawn area meets the background through
  //                  a neck, not through a stroke gap.
  //   over that      Now it matters whether there is ink to justify a wall: a
  //                  broken silhouette keeps its boundary (and beats
  //                  trapped-ball, which would have leaked), while the seam
  //                  between two lobes of open background does not.
  const int32_t minCol = 2 * SUB;
  const int32_t wide = 2 * maxGap * SUB;
  const int budget = std::max(6, 2 * maxGap + 4);

  // Which roots reach the image frame. The membrane is pinned at the frame
  // just as it is on ink, so a basin that runs off the edge is open paper; one
  // that never does is enclosed by the drawing.
  auto frameRoots = [&]() {
    std::vector<uint8_t> f(static_cast<size_t>(K) + 1, 0);
    auto mark = [&](int32_t i) {
      const int32_t r = core[i];
      if (r) f[uf.find(r)] = 1;
    };
    for (int x = 0; x < w; x++) { mark(x); mark((h - 1) * w + x); }
    for (int y = 0; y < h; y++) { mark(y * w); mark(y * w + w - 1); }
    return f;
  };

  // Merging changes who is adjacent to whom, so re-scan until it settles.
  for (int round = 0; round < 4; round++) {
    bool merged = false;
    const std::vector<uint8_t> frame = frameRoots();
    for (const auto& [key, p] : colSites()) {
      if (q[p] < minCol) continue;
      const int32_t ra = uf.find(static_cast<int32_t>(key / KEY)), rb = uf.find(static_cast<int32_t>(key % KEY));
      if (ra == rb) continue;
      const bool bothOpen = frame[ra] && frame[rb];
      if (q[p] <= wide * (bothOpen ? 1 : ENCLOSED)) continue;
      if (inkJustifies(p, q, line, w, h, budget)) continue;
      uf.parent[rb] = ra;
      if (peak[rb] > peak[ra]) peak[ra] = peak[rb];
      merged = true;
    }
    if (!merged) break;
    for (size_t i = 0; i < n; i++)
      if (core[i]) core[i] = uf.find(core[i]);
  }
  return core;
}

FlatSagResult flatSagSegment(const FlatMask& line, int w, int h, float tauPx, int maxGap) {
  FlatSagResult r;
  r.sag = flatMembraneSag(line, w, h);
  r.core = flatSagWatershed(r.sag, line, w, h, tauPx, maxGap);
  return r;
}

FlatSagView flatSagView(const std::vector<float>& sag) {
  FlatSagView v;
  for (const float s : sag) v.max = std::max(v.max, s);
  v.data.assign(sag.size(), 0);
  if (v.max > 0) {
    const float k = 255.f / std::log1p(v.max);
    for (size_t i = 0; i < sag.size(); i++) v.data[i] = static_cast<uint8_t>(k * std::log1p(sag[i]));
  }
  return v;
}

}  // namespace np
