#pragma once

#include <cstddef>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/TonalBrush.hpp"
#include "core/Mask.hpp"
#include "core/SelectionMask.hpp"

// brush/MaskTools: Dodge/Burn, Smudge and Clone Stamp over a layer mask's
// scalar coverage (T16). Brush, Pencil and Eraser are brush/MaskPaint's lerp.
// Each mirrors its content-store engine's dab loop and accumulator, with an
// absent mask tile read as 1.0 (core/Mask.hpp) instead of transparent black.
namespace np {

struct MaskTonalStep {
  float coverage = 1.0f;
  float strokeTone = 0.0f;
  bool changed = false;
};
// `toneRgbTexel()`'s accumulator and exponent applied to one coverage. A mask
// sample is not gamma-encoded, so the curve acts on it directly: Dodge raises
// coverage (reveals), Burn lowers it (hides); 0 and 1 are fixed points.
MaskTonalStep toneMaskTexel(float dst, float strokeTone, float weight, float strength,
                            TonalDirection direction) noexcept;

class MaskTonalStroke {
 public:
  void begin(float strength, TonalDirection direction) noexcept;
  void end() noexcept;
  bool active() const noexcept { return active_; }

  DepositCount toneDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                       int32_t canvasH, const Selection* selection,
                       std::vector<TileCoord>* touchedOut);

 private:
  TonalDirection direction_ = TonalDirection::Dodge;
  float strength_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore toned_;
};

// brush/Smudge's pick-up and lay-down with a one-channel finger. There is no
// "empty finger" case: an untouched mask is coverage 1, not nothing.
class MaskSmudgeStroke {
 public:
  void begin(float strength) noexcept;
  void end() noexcept;
  bool active() const noexcept { return active_; }
  float finger() const noexcept { return finger_; }
  bool loaded() const noexcept { return loaded_; }

  DepositCount smudgeDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                         int32_t canvasH, const Selection* selection,
                         std::vector<TileCoord>* touchedOut);

 private:
  float strength_ = 1.0f;
  float finger_ = 1.0f;
  bool loaded_ = false;
  bool active_ = false;
};

// brush/CloneStamp within one mask: each texel lerps toward the pen-down
// snapshot's coverage at the whole-texel source offset, under the per-stroke
// opacity ceiling (`paintMaskTexel()` with a per-texel target). A source
// outside the canvas copies nothing.
class MaskCloneStroke {
 public:
  void begin(const MaskTileStore& source, Vec2 offset, float opacity);
  void end() noexcept;
  bool active() const noexcept { return active_; }
  int32_t offsetX() const noexcept { return offsetX_; }
  int32_t offsetY() const noexcept { return offsetY_; }

  DepositCount cloneDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                        int32_t canvasH, const Selection* selection,
                        std::vector<TileCoord>* touchedOut);

 private:
  MaskTileStore source_;
  int32_t offsetX_ = 0;
  int32_t offsetY_ = 0;
  float opacity_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore applied_;
};

// Heal on a mask: a membrane fill. The dab's bounding rectangle plus a
// one-texel rim is taken from the live mask, the rim is held fixed and the
// interior solved to Laplace's equation (ops/Poisson's `harmonicFill()`), so
// specks and holes vanish and a gradient across the dab survives. Each texel
// then lerps toward the fill under the soft tip and the per-stroke opacity
// ceiling, as Clone Stamp does. No source is needed.
class MaskHealStroke {
 public:
  void begin(float opacity) noexcept;
  void end() noexcept;
  bool active() const noexcept { return active_; }

  DepositCount healDab(MaskTileStore& store, const BrushTip& tip, Vec2 centre, int32_t canvasW,
                       int32_t canvasH, const Selection* selection,
                       std::vector<TileCoord>* touchedOut);

 private:
  float opacity_ = 1.0f;
  bool active_ = false;
  StrokeAlphaStore applied_;
};

}  // namespace np
