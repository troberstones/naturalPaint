#include "flats/Model.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "flats/Expand.hpp"
#include "flats/Ink.hpp"
#include "flats/Morphology.hpp"

namespace np {

namespace {

int iround(double v) { return static_cast<int>(std::lround(v)); }

// FNV-1a over raw bytes, the same shape core/VectorShape's content hash uses.
struct Hasher {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= b[i];
      h *= 1099511628211ull;
    }
  }
  template <class T> void pod(const T& v) { bytes(&v, sizeof v); }
  void str(const std::string& s) { pod(s.size()); bytes(s.data(), s.size()); }
  void poly(const FlatPolyline& p) { pod(p.size()); bytes(p.data(), p.size() * sizeof(float)); }
};

}  // namespace

bool operator==(const FlatParams& a, const FlatParams& b) noexcept {
  return a.lineThreshold == b.lineThreshold && a.colourReject == b.colourReject &&
         a.smoothing == b.smoothing && a.skeletonize == b.skeletonize && a.gapSize == b.gapSize &&
         a.sheet == b.sheet && a.closeTightGaps == b.closeTightGaps && a.minRegion == b.minRegion &&
         a.sliverWidth == b.sliverWidth && a.declutter == b.declutter &&
         a.autoMergeLeaks == b.autoMergeLeaks && a.paletteSize == b.paletteSize &&
         a.completionField == b.completionField;
}

bool FlatEdits::empty() const noexcept {
  return bridges.empty() && carves.empty() && mergeStrokes.empty() && mergePairs.empty() &&
         deleteMarks.empty() && shapeFills.empty() && groups.empty() && recolors.empty() &&
         notes.empty();
}

uint64_t flatsContentHash(const FlatsContent& c) noexcept {
  Hasher hs;
  const FlatParams& p = c.params;
  hs.pod(p.lineThreshold); hs.pod(p.colourReject); hs.pod(p.smoothing); hs.pod(p.skeletonize);
  hs.pod(p.gapSize); hs.pod(p.sheet); hs.pod(p.closeTightGaps); hs.pod(p.minRegion);
  hs.pod(p.sliverWidth); hs.pod(p.declutter); hs.pod(p.autoMergeLeaks); hs.pod(p.paletteSize);
  hs.pod(p.completionField);
  const FlatEdits& e = c.edits;
  hs.pod(e.bridges.size());
  for (const auto& b : e.bridges) { hs.pod(b.id); hs.poly(b.pts); hs.pod(b.erase); }
  hs.pod(e.carves.size());
  for (const auto& v : e.carves) { hs.pod(v.id); hs.pod(v.x); hs.pod(v.y); }
  hs.pod(e.mergeStrokes.size());
  for (const auto& m : e.mergeStrokes) { hs.pod(m.id); hs.poly(m.pts); }
  hs.pod(e.mergePairs.size());
  for (const auto& m : e.mergePairs) { hs.pod(m.id); hs.pod(m.ax); hs.pod(m.ay); hs.pod(m.bx); hs.pod(m.by); }
  hs.pod(e.deleteMarks.size());
  for (const auto& d : e.deleteMarks) { hs.pod(d.id); hs.pod(d.x); hs.pod(d.y); }
  hs.pod(e.shapeFills.size());
  for (const auto& s : e.shapeFills) { hs.pod(s.id); hs.poly(s.pts); hs.pod(s.color); hs.str(s.name); }
  hs.pod(e.groups.size());
  for (const auto& g : e.groups) { hs.pod(g.id); hs.str(g.name); hs.poly(g.path); }
  hs.pod(e.recolors.size());
  for (const auto& r : e.recolors) { hs.pod(r.id); hs.pod(r.x); hs.pod(r.y); hs.pod(r.slot); hs.pod(r.color); }
  hs.pod(e.notes.size());
  for (const auto& n : e.notes) { hs.pod(n.id); hs.pod(n.x); hs.pod(n.y); hs.str(n.name); hs.pod(n.visible); }
  hs.pod(c.palette.size());
  for (const auto& sw : c.palette) {
    hs.pod(sw.has_value());
    if (sw) hs.pod(*sw);
  }
  return hs.h;
}

// ---------------------------------------------------------------- colours

namespace {

double hash01(int x, int y, int s) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
               static_cast<uint32_t>(s) * 1442695041u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<double>(h ^ (h >> 16)) / 4294967296.0;
}

}  // namespace

FlatRgb flatHslToRgb(float h, float s, float l) {
  const float c = (1 - std::fabs(2 * l - 1)) * s;
  const float x = c * (1 - std::fabs(std::fmod(h / 60.f, 2.f) - 1));
  const float m = l - c / 2;
  float r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; }
  else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; }
  else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; }
  else { r = c; b = x; }
  return {static_cast<uint8_t>(iround((r + m) * 255)), static_cast<uint8_t>(iround((g + m) * 255)),
          static_cast<uint8_t>(iround((b + m) * 255))};
}

std::array<float, 3> flatRgbToHsl(FlatRgb rgb) {
  const float r = rgb[0] / 255.f, g = rgb[1] / 255.f, b = rgb[2] / 255.f;
  const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (mx + mn) / 2;
  if (mx == mn) return {0.f, 0.f, l};
  const float d = mx - mn;
  const float s = d / (1 - std::fabs(2 * l - 1));
  float h = mx == r ? 60 * std::fmod((g - b) / d, 6.f) : mx == g ? 60 * ((b - r) / d + 2) : 60 * ((r - g) / d + 4);
  if (h < 0) h += 360;
  return {h, s, l};
}

FlatRgb flatAnchorColor(int x, int y) {
  return flatHslToRgb(static_cast<float>(hash01(x, y, 1) * 360), static_cast<float>(0.45 + 0.2 * hash01(x, y, 2)),
                      static_cast<float>(0.68 + 0.12 * hash01(x, y, 3)));
}

FlatRgb flatPaletteColor(int i) {
  const float h = std::fmod(i * 137.508f, 360.f);
  const float s = 0.45f + 0.2f * ((i * 7) % 3) / 2.f;
  const float l = 0.68f + 0.12f * ((i * 5) % 2);
  return flatHslToRgb(h, s, l);
}

// ---------------------------------------------------------------- geometry

bool flatPointInPoly(float x, float y, const FlatPolyline& p) {
  bool inside = false;
  if (p.size() < 6) return false;
  for (size_t i = 0, j = p.size() - 2; i < p.size(); j = i, i += 2) {
    if ((p[i + 1] > y) != (p[j + 1] > y) &&
        x < (p[j] - p[i]) * (y - p[i + 1]) / (p[j + 1] - p[i + 1]) + p[i])
      inside = !inside;
  }
  return inside;
}

float flatDistToSeg(float px, float py, float x1, float y1, float x2, float y2) {
  const float dx = x2 - x1, dy = y2 - y1;
  const float den = dx * dx + dy * dy;
  const float t = std::max(0.f, std::min(1.f, ((px - x1) * dx + (py - y1) * dy) / (den > 0 ? den : 1.f)));
  return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
}

// ---------------------------------------------------------------- evaluation

int FlatEvaluation::root(int id) const noexcept {
  while (id > 0 && static_cast<size_t>(id) < fills.size() && fills[id].parent != id) id = fills[id].parent;
  return id;
}

std::vector<int32_t> FlatEvaluation::rootLut() const {
  std::vector<int32_t> lut(fills.size(), 0);
  for (size_t i = 1; i < fills.size(); i++) lut[i] = root(static_cast<int>(i));
  return lut;
}

std::vector<int> FlatEvaluation::roots() const {
  std::vector<int> out;
  for (size_t i = 1; i < fills.size(); i++)
    if (fills[i].parent == static_cast<int>(i)) out.push_back(static_cast<int>(i));
  std::stable_sort(out.begin(), out.end(), [&](int a, int b) { return fills[a].area > fills[b].area; });
  return out;
}

int FlatEvaluation::fillAt(float x, float y) const noexcept {
  const int ix = static_cast<int>(x), iy = static_cast<int>(y);
  if (ix < 0 || iy < 0 || ix >= w || iy >= h || labels.empty()) return 0;
  return root(labels[static_cast<size_t>(iy) * w + ix]);
}

std::vector<std::array<int, 2>> FlatEvaluation::anchors() const {
  // Every root at once, in two passes over the image rather than two per
  // region: shift-clicking a shared colour recolours dozens of fills in one
  // go, and doing this per region would have made that a multi-second stall.
  const std::vector<int32_t> lut = rootLut();
  const size_t K = fills.size();
  std::vector<double> n(K, 0), sx(K, 0), sy(K, 0);
  for (size_t i = 0; i < labels.size(); i++) {
    const int32_t r = lut[labels[i]];
    if (!r) continue;
    n[r] += 1;
    sx[r] += static_cast<double>(i % w);
    sy[r] += static_cast<double>(i / w);
  }
  std::vector<double> bd(K, std::numeric_limits<double>::infinity());
  std::vector<std::array<int, 2>> out(K, std::array<int, 2>{-1, -1});
  for (size_t i = 0; i < labels.size(); i++) {
    const int32_t r = lut[labels[i]];
    if (!r) continue;
    const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
    const double dx = x - sx[r] / n[r], dy = y - sy[r] / n[r], d = dx * dx + dy * dy;
    if (d < bd[r]) { bd[r] = d; out[r] = {x, y}; }
  }
  return out;
}

FlatMask flatBridgeMask(const FlatEdits& edits, int w, int h) {
  FlatMask mask(static_cast<size_t>(w) * h, 0);
  auto clearDisc = [&](float x0, float y0, float x1, float y1) {
    const float R = kFlatEraseRadius;
    const int ylo = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - R)));
    const int yhi = std::min(h - 1, static_cast<int>(std::ceil(std::max(y0, y1) + R)));
    const int xlo = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - R)));
    const int xhi = std::min(w - 1, static_cast<int>(std::ceil(std::max(x0, x1) + R)));
    for (int y = ylo; y <= yhi; y++)
      for (int x = xlo; x <= xhi; x++)
        if (flatDistToSeg(static_cast<float>(x), static_cast<float>(y), x0, y0, x1, y1) <= R)
          mask[static_cast<size_t>(y) * w + x] = 0;
  };
  for (const FlatBridgeStroke& s : edits.bridges) {
    const FlatPolyline& p = s.pts;
    if (p.size() < 2) continue;
    auto step = [&](float x0, float y0, float x1, float y1) {
      if (s.erase) clearDisc(x0, y0, x1, y1);
      else flatMarkLine(mask, w, h, x0, y0, x1, y1);
    };
    // A dab is a single point; give it a zero-length segment so it still marks.
    if (p.size() == 2) step(p[0], p[1], p[0], p[1]);
    for (size_t i = 2; i + 1 < p.size(); i += 2) step(p[i - 2], p[i - 1], p[i], p[i + 1]);
  }
  return mask;
}

FlatMask flatLineMask(const FlatInk& ink, int w, int h, const FlatsContent& content, bool includeBridges) {
  const FlatParams& p = content.params;
  FlatMask line = flatSmoothMask(flatThresholdInk(ink, p.lineThreshold), w, h, p.smoothing);
  if (includeBridges && !content.edits.bridges.empty()) {
    const FlatMask bridges = flatBridgeMask(content.edits, w, h);
    for (size_t i = 0; i < line.size(); i++)
      if (bridges[i]) line[i] = 1;
  }
  if (p.skeletonize) line = flatSkeletonize(line, w, h);
  return line;
}

namespace {

// The worker's `segment()`: seal tight closures, then the sheet or the ball,
// then line-centre expansion.
struct Segmented {
  FlatLabels core, labels;
  FlatSegs closures;
  FlatSagView sag;
};

Segmented segment(FlatMask line, const FlatInk& ink, int w, int h, const FlatParams& p) {
  Segmented s;
  if (p.closeTightGaps) {
    s.closures = flatTightClosures(line, w, h, p.gapSize);
    for (size_t i = 0; i + 3 < s.closures.size(); i += 4)
      flatMarkLine(line, w, h, s.closures[i], s.closures[i + 1], s.closures[i + 2], s.closures[i + 3]);
  }
  if (p.sheet > 0) {
    FlatSagResult r = flatSagSegment(line, w, h, p.sheet, p.gapSize);
    s.core = std::move(r.core);
    s.sag = flatSagView(r.sag);
  } else {
    s.core = flatTrappedBall(line, w, h, p.gapSize, &ink).core;
  }
  s.labels = flatExpandLabels(s.core, w, h, &ink);
  return s;
}

}  // namespace

FlatEvaluation flatEvaluate(const uint8_t* rgba8, int w, int h, const FlatsContent& content) {
  return flatEvaluateInk(flatExtractInk(rgba8, w, h, content.params.colourReject), w, h, content);
}

FlatEvaluation flatEvaluateInk(FlatInk ink, int w, int h, const FlatsContent& content) {
  const FlatParams& p = content.params;
  FlatEvaluation e;
  e.w = w;
  e.h = h;
  e.ink = std::move(ink);
  e.line = flatLineMask(e.ink, w, h, content);

  Segmented s = segment(e.line, e.ink, w, h, p);
  e.core = std::move(s.core);
  e.labels = std::move(s.labels);
  e.closures = std::move(s.closures);
  e.sag = std::move(s.sag);

  // The worker's 'flat' handler: finalise, slivers, declutter, fronts.
  FlatFinalizeResult fr = flatFinalizeRegions(e.core, e.labels, w, h, p.minRegion);
  if (flatMergeSlivers(e.core, e.labels, e.line, w, h, p.sheet > 0 ? 0 : p.sliverWidth))
    fr = flatFinalizeRegions(e.core, e.labels, w, h, p.minRegion);
  std::vector<uint8_t> bg = flatBackgroundLut(fr.regions);
  if (flatDeclutter(e.core, e.labels, e.line, w, h, flatDeclutterOpts(p.declutter), &bg))
    fr = flatFinalizeRegions(e.core, e.labels, w, h, p.minRegion);
  const FlatOrientation orient = flatOrientationField(e.ink, w, h);
  const int maxBridge = flatMaxBridge(p.gapSize);
  bg = flatBackgroundLut(fr.regions);
  FlatFrontsResult fronts =
      flatAnalyzeFronts(e.labels, e.line, w, h, orient, maxBridge, &bg, p.sheet <= 0 && p.autoMergeLeaks);
  if (!fronts.merges.empty()) {
    flatApplyMerges(e.core, e.labels, fronts.merges);
    fr = flatFinalizeRegions(e.core, e.labels, w, h, p.minRegion);
    bg = flatBackgroundLut(fr.regions);
    // fronts changed after merging: recompute suggestions on the final labels
    fronts = flatAnalyzeFronts(e.labels, e.line, w, h, orient, maxBridge, &bg, false);
  }
  {
    std::vector<FlatPolyline> paths = flatBridgePaths(fronts.segs, &orient, e.line, w, h);
    const FlatBridgeSelection sel = flatSelectBridges(paths, e.labels, e.line, w, h);
    for (const int k : sel.keep) e.suggestions.push_back(std::move(paths[k]));
  }

  // The fill table: colour from the anchor, background hidden by default,
  // as autoFlats' runFlat builds it.
  e.fills.assign(static_cast<size_t>(fr.count) + 1, FlatFill{});
  for (const FlatRegionInfo& r : fr.regions) {
    FlatFill& f = e.fills[r.id];
    f.id = r.id;
    f.parent = r.id;
    f.area = r.area;
    f.isBg = r.isBg;
    f.visible = !r.isBg;
    f.name = "Fill " + std::to_string(r.id);
  }
  flatApplyPalette(e, p.paletteSize);
  flatReplayEdits(e, content);
  return e;
}

std::vector<std::vector<int>> flatRegionAdjacency(const FlatEvaluation& e) {
  const std::vector<int32_t> lut = e.rootLut();
  std::vector<std::vector<int>> adj(e.fills.size());
  auto add = [&](int a, int b) {
    auto& v = adj[a];
    if (std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
  };
  for (int y = 0; y < e.h; y++) {
    for (int x = 0; x < e.w; x++) {
      const size_t i = static_cast<size_t>(y) * e.w + x;
      const int a = lut[e.labels[i]];
      if (!a) continue;
      if (x < e.w - 1) { const int b = lut[e.labels[i + 1]]; if (b && b != a) { add(a, b); add(b, a); } }
      if (y < e.h - 1) { const int b = lut[e.labels[i + e.w]]; if (b && b != a) { add(a, b); add(b, a); } }
    }
  }
  return adj;
}

void flatApplyPalette(FlatEvaluation& e, int K) {
  const std::vector<int> roots = e.roots();
  if (K <= 0) {
    const std::vector<std::array<int, 2>> at = e.anchors();
    for (const int r : roots)
      if (at[r][0] >= 0) e.fills[r].color = flatAnchorColor(at[r][0], at[r][1]);
    return;
  }
  const std::vector<std::vector<int>> adj = flatRegionAdjacency(e);
  std::vector<int> idx(e.fills.size(), -1);
  for (const int r : roots) {
    std::vector<uint8_t> used(static_cast<size_t>(K), 0);
    for (const int nb : adj[r])
      if (idx[nb] >= 0) used[idx[nb]] = 1;
    int pick = -1;
    for (int c = 0; c < K; c++)
      if (!used[c]) { pick = c; break; }
    if (pick < 0) {
      // every palette entry clashes (K too small here): take the one used by
      // the fewest neighbours so the collision is least visible
      std::vector<int> count(static_cast<size_t>(K), 0);
      for (const int nb : adj[r])
        if (idx[nb] >= 0) count[idx[nb]]++;
      pick = 0;
      for (int c = 1; c < K; c++)
        if (count[c] < count[pick]) pick = c;
    }
    idx[r] = pick;
    e.fills[r].color = flatPaletteColor(pick);
  }
}

// ---------------------------------------------------------------- edits

FlatMergeOutcome flatApplyMergeStroke(FlatEvaluation& e, const FlatPolyline& pts) {
  FlatMergeOutcome out;
  if (e.labels.empty()) { out.reason = "no fills yet"; return out; }
  int start = 0;
  std::vector<int> crossed;
  auto visit = [&](float x, float y) {
    const int id = e.fillAt(x, y);
    if (!id) return;
    if (!start) start = id;
    else if (id != start && std::find(crossed.begin(), crossed.end(), id) == crossed.end()) crossed.push_back(id);
  };
  for (size_t i = 0; i + 3 < pts.size(); i += 2) {
    const float x1 = pts[i], y1 = pts[i + 1], x2 = pts[i + 2], y2 = pts[i + 3];
    const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(x2 - x1, y2 - y1))));
    for (int s = 0; s <= steps; s++) visit(x1 + (x2 - x1) * s / steps, y1 + (y2 - y1) * s / steps);
  }
  if (pts.size() == 2) visit(pts[0], pts[1]);
  if (!start) { out.reason = "stroke missed every fill"; return out; }
  FlatFill& a = e.fills[start];
  if (a.isBg) { out.reason = "start the stroke on a fill, not the background"; return out; }
  for (const int cid : crossed) {
    FlatFill& r = e.fills[cid];
    if (r.isBg) continue;
    r.parent = start;
    a.area += r.area;
    out.merged.push_back(cid);
  }
  out.into = start;
  if (out.merged.empty()) out.reason = "stroke crossed no other fills";
  return out;
}

void flatApplyMergePair(FlatEvaluation& e, const FlatMergePair& p) {
  const int a = e.fillAt(p.ax, p.ay), b = e.fillAt(p.bx, p.by);
  // Silent when either point has landed on ink, off the image, or in the
  // same region as the other: a re-flat can legitimately have joined them
  // already, and there is nothing to do about that but leave it alone.
  if (!a || !b || a == b) return;
  e.fills[b].parent = a;
  e.fills[a].area += e.fills[b].area;
}

bool flatApplyShapeFill(FlatEvaluation& e, const FlatShapeFill& sf) {
  const FlatPolyline& p = sf.pts;
  if (p.size() < 6 || e.labels.empty()) return false;
  float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
  for (size_t i = 0; i < p.size(); i += 2) {
    x0 = std::min(x0, p[i]); x1 = std::max(x1, p[i]);
    y0 = std::min(y0, p[i + 1]); y1 = std::max(y1, p[i + 1]);
  }
  const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), iy0 = std::max(0, static_cast<int>(std::floor(y0)));
  const int ix1 = std::min(e.w - 1, static_cast<int>(std::ceil(x1))), iy1 = std::min(e.h - 1, static_cast<int>(std::ceil(y1)));
  const int id = static_cast<int>(e.fills.size());
  FlatFill reg;
  reg.id = id;
  reg.parent = id;
  reg.color = sf.color;
  reg.name = sf.name;
  e.fills.push_back(reg);
  const std::vector<int32_t> lut = e.rootLut();
  int area = 0;
  for (int y = iy0; y <= iy1; y++) {
    for (int x = ix0; x <= ix1; x++) {
      if (!flatPointInPoly(x + 0.5f, y + 0.5f, p)) continue;
      const size_t i = static_cast<size_t>(y) * e.w + x;
      // Label every pixel, ink included, so the shape reads as a fill under the
      // line art like any other. Area is a count of FREE pixels only, which is
      // the convention flatFinalizeRegions uses.
      if (!e.line[i]) {
        const int prev = lut[e.labels[i]];
        if (prev && prev != id) e.fills[prev].area--;
        area++;
      }
      e.labels[i] = id;
    }
  }
  if (!area) { e.fills.pop_back(); return false; }
  e.fills[id].area = area;
  return true;
}

void flatAssignGroups(FlatEvaluation& e, const std::vector<FlatGroup>& groups) {
  constexpr double GROUP_COVER = 0.25;  // fraction of a fill that must fall inside the lasso
  for (FlatFill& f : e.fills) f.group = 0;
  if (groups.empty() || e.labels.empty()) return;
  const std::vector<int32_t> lut = e.rootLut();
  std::vector<double> total(e.fills.size(), 0);
  for (size_t i = 0; i < e.labels.size(); i++)
    if (const int32_t r = lut[e.labels[i]]) total[r] += 1;
  for (const FlatGroup& g : groups) {
    if (g.path.size() < 6) continue;
    // The polygon interior PLUS the drawn path itself (3px wide), so a stroke
    // dragged through fills selects them just as an enclosing loop does.
    float x0 = std::numeric_limits<float>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (size_t i = 0; i < g.path.size(); i += 2) {
      x0 = std::min(x0, g.path[i]); x1 = std::max(x1, g.path[i]);
      y0 = std::min(y0, g.path[i + 1]); y1 = std::max(y1, g.path[i + 1]);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0)) - 2), iy0 = std::max(0, static_cast<int>(std::floor(y0)) - 2);
    const int ix1 = std::min(e.w - 1, static_cast<int>(std::ceil(x1)) + 2), iy1 = std::min(e.h - 1, static_cast<int>(std::ceil(y1)) + 2);
    std::vector<double> inside(e.fills.size(), 0);
    for (int y = iy0; y <= iy1; y++) {
      for (int x = ix0; x <= ix1; x++) {
        const float cx = x + 0.5f, cy = y + 0.5f;
        bool in = flatPointInPoly(cx, cy, g.path);
        if (!in) {
          for (size_t i = 0; i + 1 < g.path.size() && !in; i += 2) {
            const size_t j = (i + 2 < g.path.size()) ? i + 2 : 0;
            if (flatDistToSeg(cx, cy, g.path[i], g.path[i + 1], g.path[j], g.path[j + 1]) <= 1.5f) in = true;
          }
        }
        if (!in) continue;
        if (const int32_t r = lut[e.labels[static_cast<size_t>(y) * e.w + x]]) inside[r] += 1;
      }
    }
    for (size_t r = 1; r < e.fills.size(); r++) {
      if (!inside[r]) continue;
      FlatFill& f = e.fills[r];
      if (f.isBg || f.deleted) continue;  // never swallow the background
      if (inside[r] >= GROUP_COVER * std::max(1.0, total[r])) f.group = static_cast<int>(g.id);
    }
  }
}

bool flatCarveAt(FlatEvaluation& e, int cx, int cy, int r) {
  if (cx < 0 || cy < 0 || cx >= e.w || cy >= e.h || e.core.empty()) return false;
  const int w = e.w, h = e.h;
  const size_t n = static_cast<size_t>(w) * h;
  const int32_t idx = cy * w + cx;
  // Carve within the ROOT fill under the click: core ids are pre-merge, so
  // resolve every core pixel through the root table first.
  const std::vector<int32_t> lut = e.rootLut();
  const int32_t rootTarget = e.fillAt(static_cast<float>(cx), static_cast<float>(cy));
  if (!rootTarget) return false;
  const int newId = static_cast<int>(e.fills.size());
  auto rootCore = [&](size_t i) -> int32_t {
    const int32_t c = e.core[i];
    return c == newId ? newId : c ? lut[c] : 0;
  };
  // blocked = line or other regions
  FlatMask blocked(n, 0);
  for (size_t i = 0; i < n; i++)
    if (e.line[i] || (e.core[i] && rootCore(i) != rootTarget)) blocked[i] = 1;
  const FlatDist dist = flatDistanceTransform(blocked, w, h);
  const int32_t thr = r * 3;
  // BFS from the click through target pixels to the nearest ball-fitting pixel
  std::vector<int32_t> queue(n);
  std::vector<uint8_t> seen(n, 0);
  size_t qt = 0, head = 0;
  int32_t seed = -1;
  if (e.line[idx] || rootCore(idx) != rootTarget) return false;
  queue[qt++] = idx;
  seen[idx] = 1;
  while (head < qt) {
    const int32_t p = queue[head++];
    if (dist[p] > thr) { seed = p; break; }
    const int x = p % w;
    const int32_t qs[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p - w, p + w};
    for (const int32_t q : qs) {
      if (q < 0 || static_cast<size_t>(q) >= n || seen[q] || e.line[q] || rootCore(q) != rootTarget) continue;
      seen[q] = 1;
      queue[qt++] = q;
    }
  }
  if (seed < 0) return false;
  // flood the ball-fitting component within the target
  qt = 0;
  head = 0;
  std::vector<int32_t> taken;
  e.core[seed] = newId;
  queue[qt++] = seed;
  taken.push_back(seed);
  while (head < qt) {
    const int32_t p = queue[head++];
    const int x = p % w;
    const int32_t qs[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p - w, p + w};
    for (const int32_t q : qs) {
      if (q < 0 || static_cast<size_t>(q) >= n || e.core[q] == newId || e.line[q] || rootCore(q) != rootTarget || dist[q] <= thr) continue;
      e.core[q] = newId;
      queue[qt++] = q;
      taken.push_back(q);
    }
  }
  // dilate back by r within the target
  size_t levelEnd = qt;
  int steps = r;
  head = 0;
  while (steps-- > 0 && head < qt) {
    while (head < levelEnd) {
      const int32_t p = queue[head++];
      const int x = p % w;
      const int32_t qs[4] = {x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, p - w, p + w};
      for (const int32_t q : qs) {
        if (q < 0 || static_cast<size_t>(q) >= n || e.core[q] == newId || e.line[q] || rootCore(q) != rootTarget) continue;
        e.core[q] = newId;
        queue[qt++] = q;
        taken.push_back(q);
      }
    }
    levelEnd = qt;
  }
  FlatFill reg;
  reg.id = newId;
  reg.parent = newId;
  reg.area = static_cast<int>(taken.size());
  reg.name = "Fill " + std::to_string(newId);
  e.fills[rootTarget].area -= reg.area;
  e.fills.push_back(reg);
  // Re-grow the labels so the new fill also reaches the stroke centres.
  FlatLabels rootedCore(n, 0);
  for (size_t i = 0; i < n; i++) rootedCore[i] = e.core[i] == newId ? newId : rootCore(i);
  e.labels = flatExpandLabels(rootedCore, w, h, &e.ink);
  // labels are now root ids; make the root table agree (every fill is now its
  // own root, merges having been folded into the labels).
  for (FlatFill& f : e.fills) f.parent = f.id;
  e.core = std::move(rootedCore);
  e.fills[newId].color = flatAnchorColor(cx, cy);
  return true;
}

void flatReplayEdits(FlatEvaluation& e, const FlatsContent& content) {
  const FlatEdits& ed = content.edits;
  if (e.labels.empty()) return;
  // Carves first: they create the fills that later edits may name.
  for (const FlatCarve& c : ed.carves) flatCarveAt(e, iround(c.x), iround(c.y), content.params.gapSize);
  // Merges next, because they decide which roots exist.
  for (const FlatMergeStroke& s : ed.mergeStrokes) flatApplyMergeStroke(e, s.pts);
  for (const FlatMergePair& p : ed.mergePairs) flatApplyMergePair(e, p);
  // Deletion is rebuilt from scratch rather than carried over, so removing a
  // mark genuinely un-deletes.
  for (FlatFill& f : e.fills) f.deleted = false;
  for (const FlatDeleteMark& m : ed.deleteMarks)
    if (const int r = e.fillAt(m.x, m.y)) e.fills[r].deleted = true;
  // Shapes stamp last: they overwrite the label map, so they must land on the
  // regions as they finally are rather than on ones a later merge would change.
  for (const FlatShapeFill& sf : ed.shapeFills) flatApplyShapeFill(e, sf);
  // Colours last of all, so they land on the regions as they finally are --
  // including the ones a shape fill just stamped over the top.
  for (const FlatRecolor& rc : ed.recolors) {
    const int r = e.fillAt(rc.x, rc.y);
    if (!r) continue;
    FlatFill& f = e.fills[r];
    f.swatch = rc.slot;
    // A live swatch link: the palette's current colour wins over the one
    // recorded at the time (PRD N6). A slot that has since been emptied falls
    // back to the recorded colour.
    if (rc.slot >= 0 && static_cast<size_t>(rc.slot) < content.palette.size() && content.palette[rc.slot])
      f.color = *content.palette[rc.slot];
    else
      f.color = rc.color;
  }
  for (const FlatFillNote& nt : ed.notes) {
    const int r = e.fillAt(nt.x, nt.y);
    if (!r) continue;
    if (!nt.name.empty()) e.fills[r].name = nt.name;
    e.fills[r].visible = nt.visible;
  }
  flatAssignGroups(e, ed.groups);
}

std::vector<FlatMergePair> flatClusterSmall(const FlatEvaluation& e, int maxArea) {
  std::vector<FlatMergePair> out;
  if (e.labels.empty()) return out;
  const int w = e.w, h = e.h;
  const std::vector<int32_t> lut = e.rootLut();
  const FlatDist ld = flatDistanceTransform(e.line, w, h);
  std::vector<int32_t> area(e.fills.size(), 0);
  for (size_t i = 0; i < e.labels.size(); i++)
    if (!e.line[i]) area[lut[e.labels[i]]]++;
  // open-border lengths between root pairs
  std::unordered_map<uint64_t, int32_t> open;
  auto key = [](int32_t a, int32_t b) { return (static_cast<uint64_t>(static_cast<uint32_t>(a)) << 32) | static_cast<uint32_t>(b); };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int32_t a = lut[e.labels[i]];
      const size_t qs[2] = {x < w - 1 ? i + 1 : SIZE_MAX, y < h - 1 ? i + w : SIZE_MAX};
      for (const size_t q : qs) {
        if (q == SIZE_MAX) continue;
        const int32_t b = lut[e.labels[q]];
        if (a == b || ld[i] <= 7 || ld[q] <= 7) continue;
        open[key(a, b)]++;
        open[key(b, a)]++;
      }
    }
  }
  const std::vector<std::array<int, 2>> at = e.anchors();
  for (size_t id = 1; id < e.fills.size(); id++) {
    const FlatFill& f = e.fills[id];
    if (f.parent != static_cast<int>(id) || !area[id] || area[id] >= maxArea || f.isBg) continue;
    int32_t best = 0, bw = 3;  // need at least 4 open border px
    for (size_t b = 1; b < e.fills.size(); b++) {
      if (b == id || e.fills[b].isBg) continue;
      auto it = open.find(key(static_cast<int32_t>(id), static_cast<int32_t>(b)));
      if (it != open.end() && it->second > bw) { bw = it->second; best = static_cast<int32_t>(b); }
    }
    if (best && at[id][0] >= 0 && at[best][0] >= 0) {
      FlatMergePair p;
      p.ax = static_cast<float>(at[best][0]); p.ay = static_cast<float>(at[best][1]);
      p.bx = static_cast<float>(at[id][0]); p.by = static_cast<float>(at[id][1]);
      out.push_back(p);
    }
  }
  return out;
}

FlatEditRef flatEditAt(const FlatEdits& edits, float x, float y, float reach) {
  FlatEditRef best;
  float bd = reach;
  auto polyDist = [&](const FlatPolyline& p, bool closed) {
    float d = std::numeric_limits<float>::infinity();
    if (p.size() == 2) return std::hypot(p[0] - x, p[1] - y);
    for (size_t i = 0; i + 3 < p.size(); i += 2) d = std::min(d, flatDistToSeg(x, y, p[i], p[i + 1], p[i + 2], p[i + 3]));
    if (closed && p.size() >= 6) {
      d = std::min(d, flatDistToSeg(x, y, p[p.size() - 2], p[p.size() - 1], p[0], p[1]));
      // A group's interior counts as a hit but scores just worse than any line.
      if (d > reach && flatPointInPoly(x, y, p)) d = reach * 0.99f;
    }
    return d;
  };
  auto consider = [&](float d, int kind, uint32_t id) {
    if (d < bd) { bd = d; best = {kind, id}; }
  };
  for (const auto& b : edits.bridges) consider(polyDist(b.pts, false), 1, b.id);
  for (const auto& m : edits.mergeStrokes) consider(polyDist(m.pts, false), 2, m.id);
  for (const auto& m : edits.mergePairs) consider(flatDistToSeg(x, y, m.ax, m.ay, m.bx, m.by), 3, m.id);
  for (const auto& d : edits.deleteMarks) consider(std::hypot(d.x - x, d.y - y), 4, d.id);
  for (const auto& s : edits.shapeFills) consider(polyDist(s.pts, true), 5, s.id);
  for (const auto& g : edits.groups) consider(polyDist(g.path, true), 6, g.id);
  for (const auto& c : edits.carves) consider(std::hypot(c.x - x, c.y - y), 7, c.id);
  return best;
}

bool flatRemoveEdit(FlatEdits& edits, FlatEditRef ref) {
  auto erase = [&](auto& v) {
    for (auto it = v.begin(); it != v.end(); ++it)
      if (it->id == ref.id) { v.erase(it); return true; }
    return false;
  };
  switch (ref.kind) {
    case 1: return erase(edits.bridges);
    case 2: return erase(edits.mergeStrokes);
    case 3: return erase(edits.mergePairs);
    case 4: return erase(edits.deleteMarks);
    case 5: return erase(edits.shapeFills);
    case 6: return erase(edits.groups);
    case 7: return erase(edits.carves);
    default: return false;
  }
}

void flatRenderRgba8(const FlatEvaluation& e, uint8_t* out) {
  const size_t n = static_cast<size_t>(e.w) * e.h;
  std::memset(out, 0, n * 4);
  if (e.labels.empty()) return;
  const std::vector<int32_t> lut = e.rootLut();
  for (size_t i = 0; i < n; i++) {
    const int32_t r = lut[e.labels[i]];
    if (!r) continue;
    const FlatFill& f = e.fills[r];
    if (!f.visible || f.deleted) continue;
    out[i * 4] = f.color[0];
    out[i * 4 + 1] = f.color[1];
    out[i * 4 + 2] = f.color[2];
    out[i * 4 + 3] = 255;
  }
}

}  // namespace np
