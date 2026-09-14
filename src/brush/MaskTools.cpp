#include "brush/MaskTools.hpp"

#include <algorithm>
#include <cmath>

#include "brush/Grain.hpp"
#include "brush/MaskPaint.hpp"

namespace np {
namespace {

struct DabTiles {
  PixelBounds b;
  TileCoord first{0, 0};
  TileCoord last{0, 0};
};

bool dabTiles(const BrushTip& tip, Vec2 centre, int32_t canvasW, int32_t canvasH, DabTiles& out) {
  out.b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (out.b.empty()) return false;
  out.first = tileCoordAt(PixelCoord{out.b.x0, out.b.y0});
  out.last = tileCoordAt(PixelCoord{out.b.x1, out.b.y1});
  return true;
}

// The dab's grained coverage times the selection at one texel; 0 skips it.
float dabWeightAt(const BrushTip& tip, Vec2 centre, int32_t x, int32_t y,
                  const SelectionTile* cover, const Selection* selection, PixelCoord local) {
  const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
  const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
  const float rawCov = dabCoverage(tip, dx, dy);
  if (!(rawCov > 0.0f)) return 0.0f;
  const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
  if (!(cov > 0.0f)) return 0.0f;
  const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
  if (!(sel > 0.0f)) return 0.0f;
  return cov * sel;
}

float selectionAt(const SelectionTile* cover, const Selection* selection, PixelCoord local) {
  return selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
}

}  // namespace

MaskTonalStep toneMaskTexel(float dst, float strokeTone, float weight, float strength,
                            TonalDirection direction) noexcept {
  MaskTonalStep out;
  out.coverage = dst;
  out.strokeTone = strokeTone;

  const float cap = std::clamp(strength, 0.0f, 1.0f);
  const float t0 = std::clamp(strokeTone, 0.0f, 1.0f);
  if (!(weight > 0.0f)) return out;
  if (!(cap > 0.0f)) return out;
  if (!(t0 < cap)) return out;

  float t1 = t0 + weight * (1.0f - t0);
  if (t1 > cap) t1 = cap;
  const float dt = t1 - t0;
  const float signedDt = direction == TonalDirection::Burn ? dt : -dt;
  const float gamma = std::exp2(signedDt * std::log2(kTonalFullGamma));
  const float next = maskCoverageClamp(tonalCurve(dst, gamma));
  if (next == dst) return out;

  out.coverage = next;
  out.strokeTone = t1;
  out.changed = true;
  return out;
}

void MaskTonalStroke::begin(float strength, TonalDirection direction) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  direction_ = direction;
  toned_ = StrokeAlphaStore{};
  active_ = true;
}

void MaskTonalStroke::end() noexcept {
  toned_ = StrokeAlphaStore{};
  active_ = false;
}

DepositCount MaskTonalStroke::toneDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre,
                                      int32_t canvasW, int32_t canvasH,
                                      const Selection* selection,
                                      std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f) || !(strength_ > 0.0f)) return count;
  DabTiles d;
  if (!dabTiles(tip, centre, canvasW, canvasH, d)) return count;

  for (int32_t ty = d.first.y; ty <= d.last.y; ++ty) {
    for (int32_t tx = d.first.x; tx <= d.last.x; ++tx) {
      const TileCoord coord{tx, ty};
      // An absent tile is coverage 1, a fixed point of the curve: nothing to do.
      const MaskTile* srcTile = store.find(coord);
      if (srcTile == nullptr) continue;
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        if (cover == nullptr) continue;
      }
      const PixelCoord org = tileOrigin(coord);
      const StrokeAlphaTile* toneRead = toned_.find(coord);
      MaskTile* dst = nullptr;
      StrokeAlphaTile* toneWrite = nullptr;
      for (int32_t y = std::max(d.b.y0, org.y); y <= std::min(d.b.y1, org.y + kTileSize - 1); ++y) {
        for (int32_t x = std::max(d.b.x0, org.x); x <= std::min(d.b.x1, org.x + kTileSize - 1);
             ++x) {
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
          const float w = dabWeightAt(tip, centre, x, y, cover, selection, local);
          if (!(w > 0.0f)) continue;
          const float sel = selectionAt(cover, selection, local);
          const float applied = toneRead != nullptr ? toneRead->at(local) : 0.0f;
          const MaskTonalStep step = toneMaskTexel(srcTile->readCoverage(local), applied,
                                                   tip.flow * w, strength_ * sel, direction_);
          if (!step.changed) continue;
          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (toneWrite == nullptr) {
            toneWrite = &toned_.getOrCreate(coord);
            toneRead = toneWrite;
          }
          toneWrite->set(local, step.strokeTone);
          dst->writeCoverage(local, step.coverage);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

void MaskSmudgeStroke::begin(float strength) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  finger_ = 1.0f;
  loaded_ = false;
  active_ = true;
}

void MaskSmudgeStroke::end() noexcept {
  finger_ = 1.0f;
  loaded_ = false;
  active_ = false;
}

DepositCount MaskSmudgeStroke::smudgeDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre,
                                         int32_t canvasW, int32_t canvasH,
                                         const Selection* selection,
                                         std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(strength_ > 0.0f)) return count;
  DabTiles d;
  if (!dabTiles(tip, centre, canvasW, canvasH, d)) return count;

  // Pick-up: the coverage-weighted mean under the dab, ignoring the selection
  // as brush/Smudge does (the finger reads what it passes over).
  double sumW = 0.0, sum = 0.0;
  for (int32_t ty = d.first.y; ty <= d.last.y; ++ty) {
    for (int32_t tx = d.first.x; tx <= d.last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const MaskTile* srcTile = store.find(coord);
      const PixelCoord org = tileOrigin(coord);
      for (int32_t y = std::max(d.b.y0, org.y); y <= std::min(d.b.y1, org.y + kTileSize - 1); ++y) {
        for (int32_t x = std::max(d.b.x0, org.x); x <= std::min(d.b.x1, org.x + kTileSize - 1);
             ++x) {
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
          const float w = dabWeightAt(tip, centre, x, y, nullptr, nullptr, local);
          if (!(w > 0.0f)) continue;
          sumW += w;
          sum += static_cast<double>(w) * maskCoverage(srcTile, local);
        }
      }
    }
  }
  if (!(sumW > 0.0)) return count;
  const float pick = static_cast<float>(sum / sumW);
  finger_ = loaded_ ? std::lerp(pick, finger_, strength_) : pick;
  loaded_ = true;
  if (!(tip.flow > 0.0f)) return count;

  for (int32_t ty = d.first.y; ty <= d.last.y; ++ty) {
    for (int32_t tx = d.first.x; tx <= d.last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const MaskTile* srcTile = store.find(coord);
      if (srcTile == nullptr && finger_ == 1.0f) continue;  // reveal onto reveal
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        if (cover == nullptr) continue;
      }
      const PixelCoord org = tileOrigin(coord);
      MaskTile* dst = nullptr;
      for (int32_t y = std::max(d.b.y0, org.y); y <= std::min(d.b.y1, org.y + kTileSize - 1); ++y) {
        for (int32_t x = std::max(d.b.x0, org.x); x <= std::min(d.b.x1, org.x + kTileSize - 1);
             ++x) {
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
          const float w = dabWeightAt(tip, centre, x, y, cover, selection, local);
          if (!(w > 0.0f)) continue;
          const float a = std::min(1.0f, tip.flow * w * strength_);
          const float before = maskCoverage(srcTile, local);
          if (!(a > 0.0f) || before == finger_) continue;
          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          dst->writeCoverage(local, std::lerp(before, finger_, a));
          ++count.texels;
        }
      }
    }
  }
  return count;
}

void MaskCloneStroke::begin(const MaskTileStore& source, Vec2 offset, float opacity) {
  source_ = source;
  offsetX_ = static_cast<int32_t>(std::lround(offset.x));
  offsetY_ = static_cast<int32_t>(std::lround(offset.y));
  opacity_ = std::clamp(opacity, 0.0f, 1.0f);
  applied_ = StrokeAlphaStore{};
  active_ = true;
}

void MaskCloneStroke::end() noexcept {
  applied_ = StrokeAlphaStore{};
  source_ = MaskTileStore{};
  active_ = false;
}

DepositCount MaskCloneStroke::cloneDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre,
                                       int32_t canvasW, int32_t canvasH,
                                       const Selection* selection,
                                       std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f) || !(opacity_ > 0.0f)) return count;
  DabTiles d;
  if (!dabTiles(tip, centre, canvasW, canvasH, d)) return count;

  for (int32_t ty = d.first.y; ty <= d.last.y; ++ty) {
    for (int32_t tx = d.first.x; tx <= d.last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        if (cover == nullptr) continue;
      }
      const PixelCoord org = tileOrigin(coord);
      const MaskTile* dstRead = store.find(coord);
      const StrokeAlphaTile* appliedRead = applied_.find(coord);
      MaskTile* dst = nullptr;
      StrokeAlphaTile* appliedWrite = nullptr;
      for (int32_t y = std::max(d.b.y0, org.y); y <= std::min(d.b.y1, org.y + kTileSize - 1); ++y) {
        for (int32_t x = std::max(d.b.x0, org.x); x <= std::min(d.b.x1, org.x + kTileSize - 1);
             ++x) {
          const int32_t sx = x + offsetX_;
          const int32_t sy = y + offsetY_;
          if (sx < 0 || sy < 0 || sx >= canvasW || sy >= canvasH) continue;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
          const float w = dabWeightAt(tip, centre, x, y, cover, selection, local);
          if (!(w > 0.0f)) continue;
          const float sel = selectionAt(cover, selection, local);
          const PixelCoord sp{sx, sy};
          const float src = maskCoverage(source_.find(tileCoordAt(sp)), tileLocalOffset(sp));
          const float applied = appliedRead != nullptr ? appliedRead->at(local) : 0.0f;
          const MaskPaintStep step = paintMaskTexel(maskCoverage(dstRead, local), applied,
                                                    tip.flow * w, opacity_ * sel, src);
          if (!step.changed) continue;
          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            dstRead = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (appliedWrite == nullptr) {
            appliedWrite = &applied_.getOrCreate(coord);
            appliedRead = appliedWrite;
          }
          appliedWrite->set(local, step.strokeApplied);
          dst->writeCoverage(local, step.coverage);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

}  // namespace np
