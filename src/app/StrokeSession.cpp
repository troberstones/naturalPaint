#include "app/StrokeSession.hpp"

#include <algorithm>

namespace np {

const char* strokeRouteName(StrokeRoute route) noexcept {
  switch (route) {
    case StrokeRoute::None: return "none";
    case StrokeRoute::CpuDeposit: return "cpu-deposit";
    case StrokeRoute::PaintSim: return "paint-sim";
  }
  return "?";
}

StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept {
  switch (tool) {
    case Tool::Brush:
    case Tool::DryBrush:
      break;
    // Water deposits water and no pigment, and a Pigment tile has no water
    // channel -- see the header §1.
    case Tool::Water:
      return StrokeRoute::PaintSim;
    case Tool::Eyedropper:
    case Tool::Hand:
    case Tool::Zoom:
    case Tool::Count:
      return StrokeRoute::None;
  }

  // A Pigment layer with somewhere to put pigment is the only thing that
  // routes to the CPU deposit. Everything else keeps today's behaviour
  // exactly, which is the solver canvas.
  if (target == nullptr || target->kind != LayerKind::Pigment || !target->pigmentTiles)
    return StrokeRoute::PaintSim;
  if (target->locked) return StrokeRoute::None;
  return StrokeRoute::CpuDeposit;
}

const char* strokeEditLabel(Tool tool) noexcept {
  switch (tool) {
    case Tool::Brush: return "brush stroke";
    case Tool::DryBrush: return "dry brush stroke";
    case Tool::Water: return "water stroke";
    case Tool::Eyedropper:
    case Tool::Hand:
    case Tool::Zoom:
    case Tool::Count:
      break;
  }
  return "stroke";
}

BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut, float pressure) {
  // The same two curves ui/MacPaintUI's solver route applies, and they are
  // here rather than there so the two routes cannot drift apart. A pen
  // configured for size-only pressure must feel the same whichever layer kind
  // it lands on.
  const float p = pressure < 0.0f ? 0.0f : (pressure > 1.0f ? 1.0f : pressure);
  const float sizeMul = brush.pressureSize ? (0.25f + 0.75f * p) : 1.0f;
  const float flowMul = brush.pressureFlow ? (0.15f + 0.85f * p) : 1.0f;

  BrushTip tip;
  tip.radius = brush.radius * sizeMul;
  tip.hardness = brush.hardness;
  // `BrushState::load` is "pigment concentration" and ranges 0..2.5; a tip's
  // `flow` is "mass laid down per dab where coverage is 1" and is deliberately
  // not clamped to [0,1] (brush/Deposit.hpp: "a flow above 1 is a legitimate
  // one dab saturates the paper tip"). So this is the same number, scaled by
  // pressure, and not a remapping that would make the LOAD slider mean two
  // things.
  tip.flow = brush.load * flowMul;
  tip.spacing = brush.spacing;

  const std::vector<Pigment>& palette = defaultPalette();
  const size_t index =
      brush.pigment >= 0 && static_cast<size_t>(brush.pigment) < palette.size()
          ? static_cast<size_t>(brush.pigment)
          : 0;
  const Pigment& pigment = palette[index];
  if (lut.valid())
    tip.pigment = lut.rgbToLatent(pigment.rgb[0], pigment.rgb[1], pigment.rgb[2]);
  else
    // No LUT: `Latent::c` is Mixbox's own three weights and the fourth
    // (white) is derived, so a straight copy of the sRGB triple is not the
    // right latent -- but it is a colour in the right family, and a build
    // that never loaded the 512x512 PNG painting *something* beats one that
    // paints white. The LUT is loaded by main.cpp before any UI exists, so
    // this branch is for tests and for a broken install.
    tip.pigment.c = {pigment.rgb[0], pigment.rgb[1], pigment.rgb[2]};
  return tip;
}

bool StrokeSession::begin(OpenDocument& doc, size_t layerIndex, const BrushTip& tip, Tool tool,
                          std::string* errorOut) {
  if (errorOut != nullptr) errorOut->clear();
  const auto refuse = [&](std::string why) {
    if (errorOut != nullptr) *errorOut = std::move(why);
    return false;
  };

  if (layerIndex >= doc.document.layers.size())
    return refuse("stroke refused: layer " + std::to_string(layerIndex) +
                  " is out of range for a document with " +
                  std::to_string(doc.document.layers.size()) + " layers.");

  Layer& layer = doc.document.layers[layerIndex];
  // The route function is the single table (header §1); this refusal reads it
  // rather than re-deriving the same conditions, so the two cannot disagree
  // about which strokes a locked or non-Pigment layer accepts.
  const StrokeRoute route = strokeRouteFor(tool, &layer);
  if (route != StrokeRoute::CpuDeposit)
    return refuse(std::string("stroke refused: the ") + strokeEditLabel(tool) + " on layer " +
                  std::to_string(layerIndex) + " ('" + layer.name + "', " +
                  layerKindName(layer.kind) + (layer.locked ? ", locked" : "") +
                  ") routes to " + strokeRouteName(route) +
                  ", not to the CPU pigment deposit.");

  doc_ = &doc;
  layerIndex_ = layerIndex;
  layerCount_ = doc.document.layers.size();
  tip_ = tip;
  label_ = strokeEditLabel(tool);

  // Leftover arc length and point history from whatever stroke happened
  // before must not bleed into this one -- brush/StrokePath::reset()'s own
  // contract, and the same call ui/MacPaintUI already makes at pen-down.
  path_.reset();
  pending_.clear();
  frameTiles_.clear();
  strokeTiles_.clear();
  dabs_ = 0;
  texels_ = 0;
  return true;
}

void StrokeSession::depositPending() {
  frameTiles_.clear();
  if (pending_.empty()) return;

  Document& doc = doc_->document;
  // The target is re-validated on **every** frame, not just at pen-down: the
  // document is a public aggregate, and a stroke that outlived its layer would
  // otherwise be a dangling write -- or, worse, would deposit the rest of
  // itself into whatever slid into that index. Both are the same class of
  // silent, invisible mistake the header refuses for a locked target.
  //
  // The check is the layer *count* plus the layer's own kind, store and lock,
  // and the header says what that does and does not cover.
  if (doc.layers.size() != layerCount_ || layerIndex_ >= doc.layers.size()) {
    pending_.clear();
    return;
  }
  Layer& layer = doc.layers[layerIndex_];
  if (strokeRouteFor(Tool::Brush, &layer) != StrokeRoute::CpuDeposit) {
    pending_.clear();
    return;
  }
  PigmentTileStore& store = *layer.pigmentTiles;
  const StrokeDeposit deposited =
      depositDabs(store, tip_, pending_, doc.width, doc.height);
  dabs_ += deposited.dabs;
  texels_ += deposited.texels;
  frameTiles_ = deposited.tiles;

  if (!frameTiles_.empty()) {
    strokeTiles_.insert(strokeTiles_.end(), frameTiles_.begin(), frameTiles_.end());
    sortUniqueTiles(strokeTiles_);
  }
  pending_.clear();
}

const std::vector<TileCoord>& StrokeSession::addPoint(float x, float y) {
  frameTiles_.clear();
  if (doc_ == nullptr) return frameTiles_;

  path_.addPoint(x, y, tip_.spacingPx(), pending_);
  depositPending();

  // Live feedback, header §3: the revision is what invalidates
  // ui/DocumentTexture's cache, and it moves only when pigment actually
  // landed. No history entry -- that is pen-up's job, once.
  if (!frameTiles_.empty()) ++doc_->revision;
  return frameTiles_;
}

const std::vector<TileCoord>& StrokeSession::end() {
  if (doc_ == nullptr) return strokeTiles_;

  path_.flush(tip_.spacingPx(), pending_);
  depositPending();

  OpenDocument* doc = doc_;
  doc_ = nullptr;  // the session is over before the record, so a re-entrant
                   // caller cannot deposit into a half-ended stroke

  // Exactly one entry, and only for a stroke that put something down --
  // header §2.
  if (texels_ > 0) doc->recordEdit(label_, EditKind::Content);
  return strokeTiles_;
}

}  // namespace np
