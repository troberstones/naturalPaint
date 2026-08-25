#include "app/StrokeSession.hpp"

#include <algorithm>

#include "color/Space.hpp"

namespace np {

const char* strokeRouteName(StrokeRoute route) noexcept {
  switch (route) {
    case StrokeRoute::None: return "none";
    case StrokeRoute::CpuDeposit: return "cpu-deposit";
    case StrokeRoute::RgbDeposit: return "rgb-deposit";
    case StrokeRoute::RgbErase: return "rgb-erase";
    case StrokeRoute::PaintSim: return "paint-sim";
  }
  return "?";
}

StrokeRoute strokeRouteFor(Tool tool, const Layer* target) noexcept {
  // The one bit that separates §1's two families of stroke: a deposit adds and
  // an erase removes, and every row below differs only in that. Carried as a
  // flag through one shared body rather than as a second copy of the target
  // tests, because the target half of the question -- locked before kind, a
  // store to write being part of the question, "no target" being its own row --
  // has exactly one right answer and two copies of it would drift.
  bool erasing = false;

  switch (tool) {
    case Tool::Brush:
    case Tool::DryBrush:
      break;
    // PRD F9/F10 (both P0) and ADR-0007. This case used to sit in the
    // not-built list below, so the eraser routed nowhere and did nothing at
    // all -- see the header §1's Eraser rows.
    case Tool::Eraser:
      erasing = true;
      break;
    // Water deposits water and no pigment, and a Pigment tile has no water
    // channel -- see the header §1.
    case Tool::Water:
      return StrokeRoute::PaintSim;
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    // The palette cells app/AppState.hpp's Tool comment says exist for
    // their name/icon/slot only -- none of them is built, so none of them can
    // route anywhere but here. Listed rather than caught by a `default:` so
    // that a real implementation moves its tool out of this list instead of
    // silently keeping the fallback -Wswitch would no longer force a look at.
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::CloneStamp:
    case Tool::PaintBucket:
    case Tool::Gradient:
    case Tool::Pencil:
    case Tool::Smudge:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      return StrokeRoute::None;
  }

  // **No target at all is the one case the solver canvas is right for**, and
  // it is checked first so that it reads as its own row rather than as the
  // remainder of the ones below it (header §1's last paragraph). No document is
  // open, so there is no layer the user could have aimed at, and watercolour
  // and oil paint the dense canvas texture legitimately.
  //
  // **Except for the eraser**, which is the one row where the two families part
  // company: `sim::PaintSim` has no alpha and no erase step, so an eraser sent
  // there would run the paint path and *add* pigment where the user asked for
  // its removal. Nothing to erase, so nowhere to go.
  if (target == nullptr) return erasing ? StrokeRoute::None : StrokeRoute::PaintSim;

  // Locked before kind, so a locked layer refuses for being locked whatever it
  // is made of -- and so the UI's "clear its Lock in LAYERS" message is the one
  // a user gets for the one problem they can actually fix.
  if (target->locked) return StrokeRoute::None;

  // A store to write is part of the question, not a precondition: `Layer.hpp`'s
  // "at most one of rgbTiles and pigmentTiles is engaged" allows a layer of
  // either kind with neither, and painting one has nowhere to go.
  //
  // **A Pigment layer takes a deposit and refuses an erase**, and the header §1
  // carries that argument at length: mass is the erasable quantity ADR-0007
  // names, but `depositDab(PigmentTileStore&, ...)` has no selection parameter,
  // so a Pigment erase built today would be the one tool on that layer kind that
  // either stopped at the marching ants alone or destroyed paint outside a
  // selection drawn to protect it. It refuses by name until the pigment route
  // has PRD E1's gate.
  if (target->kind == LayerKind::Pigment && target->pigmentTiles)
    return erasing ? StrokeRoute::None : StrokeRoute::CpuDeposit;
  if (target->kind == LayerKind::RGB && target->rgbTiles)
    return erasing ? StrokeRoute::RgbErase : StrokeRoute::RgbDeposit;

  // Everything left is a real target that cannot take a stroke: an Adjustment
  // layer (no tiles by construction), a Media/Strokes/Text/Flats layer (no
  // storage built yet), or a Pigment/RGB layer whose store was never allocated.
  // **`None`, not `PaintSim`** -- see §1. Falling through to the solver here is
  // what made "select an RGB layer and paint" put colour on the canvas texture
  // instead of on the layer, invisibly, one line below the locked row that
  // exists to prevent exactly that.
  return StrokeRoute::None;
}

const char* strokeEditLabel(Tool tool) noexcept {
  switch (tool) {
    case Tool::Brush: return "brush stroke";
    case Tool::DryBrush: return "dry brush stroke";
    case Tool::Water: return "water stroke";
    // **"erase", not "brush stroke"** -- a noun for what the edit did, in the
    // form `core/LayerOps`' `editLabel` uses. PRD O2's panel is scanned to find
    // an edit to undo, and a row that says "brush stroke" about a gesture that
    // took paint *off* is the one row a user cannot recognise.
    case Tool::Eraser: return "erase";
    case Tool::Eyedropper:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Hand:
    case Tool::Zoom:
    // Same not-built list as strokeRouteFor() above, and the same reason
    // it is spelled out rather than a `default:` -- none of these can begin
    // a stroke (strokeRouteFor() refuses them all), so none of them needs a
    // label of its own; they fall through to the generic one below.
    case Tool::Move:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Measure:
    case Tool::Frame:
    case Tool::CloneStamp:
    case Tool::PaintBucket:
    case Tool::Gradient:
    case Tool::Pencil:
    case Tool::Smudge:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Text:
    case Tool::Shape:
    case Tool::Slice:
    case Tool::Count:
      break;
  }
  return "stroke";
}

// --- the pixel-writing ops that are not strokes (header §6) ----------------

bool toolWritesRgbPixels(Tool tool) noexcept {
  return tool == Tool::PaintBucket || tool == Tool::Gradient;
}

PixelOpRefusal pixelOpRefusalFor(const Layer* target) noexcept {
  if (target == nullptr) return PixelOpRefusal::NoLayer;
  // Before the storage test, so a locked RGB layer refuses for the reason that
  // has a fix. See the header.
  if (target->locked) return PixelOpRefusal::Locked;
  // **`rgbTiles`, not `kind == RGB`.** core/Layer.hpp allows a layer of either
  // paint kind to carry neither store, and ops/FloodFill and ops/Gradient both
  // take a `TileStore&` -- so the question is whether there is one to write,
  // not what the kind tag says. `strokeRouteFor()` draws the same distinction
  // just above its own fallthrough, for the same reason.
  if (!target->rgbTiles.has_value()) return PixelOpRefusal::NoRgbStore;
  return PixelOpRefusal::None;
}

std::string pixelOpRefusalMessage(PixelOpRefusal reason, const Layer* target,
                                  const char* opName) {
  const std::string op = opName != nullptr ? opName : "fill";
  const std::string name = target != nullptr ? target->name : std::string();
  switch (reason) {
    case PixelOpRefusal::None:
      return {};
    case PixelOpRefusal::NoLayer:
      // No name to give, because there is nothing to name -- and the fix is a
      // different one from the other two, so it says so rather than reusing
      // "pick a layer in LAYERS" for a stack that may have none.
      return "no layer: the " + op +
             " has nothing to fill. Open a document, or add a layer in LAYERS.";
    case PixelOpRefusal::Locked:
      return "locked layer: \"" + name + "\" cannot take the " + op +
             ". Clear its Lock in LAYERS.";
    case PixelOpRefusal::NoRgbStore:
      // Names the KIND as well as the layer: "Pigment" is the answer to "why
      // not", and a user who has just made a layer from the NEW popup's first
      // entry has no other way in the chrome to find out which of the seven
      // kinds they picked.
      return "\"" + name + "\" is " +
             (target != nullptr ? layerKindName(target->kind) : "?") +
             " and has no RGB pixels for the " + op + ". Pick an RGB layer in LAYERS.";
  }
  return {};
}

BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut, float pressure) {
  DynamicInputs in;
  in.pressure = pressure;  // evaluateLinks() clamps; see linkContribution()
  return brushTipFor(brush, lut, in);
}

BrushTip brushTipFor(const BrushState& brush, const MixboxLut& lut,
                     const DynamicInputs& inputs) {
  // Resolved here rather than at each call site so the two routes cannot drift
  // apart. A pen configured for size-only pressure must feel the same
  // whichever layer kind it lands on -- which is the reason the two hardcoded
  // curves were pulled into one place before, and the reason the link set that
  // replaced them is read in one place now.
  const DynamicResult dyn = evaluateLinks(brush.links, inputs);
  const float sizeMul = dyn.at(DynamicTarget::Size);
  const float flowMul = dyn.at(DynamicTarget::Flow);

  BrushTip tip;
  tip.radius = brush.radius * sizeMul;
  tip.hardness = brush.hardness * dyn.at(DynamicTarget::Hardness);
  // **These two used to be dropped here**, and brush/Deposit.hpp §2b is the
  // whole account of what that cost: two sliders, two DYNAMICS columns, a
  // shipped preset and a Photoshop importer all describing a tip shape that
  // nothing painted. Each takes its own combine rule from the matrix rather
  // than a rule chosen here -- Roundness multiplies (a link scales the ratio)
  // and Angle adds (a link offsets the rotation), which is exactly what
  // `targetCombine()` says of each, and is why `evaluateLinks()` hands back
  // 1.0 and 0.0 respectively for an undriven target and these two lines are
  // therefore a no-op for a brush with no links.
  tip.roundness = brush.roundness * dyn.at(DynamicTarget::Roundness);
  tip.angle = brush.angle + dyn.at(DynamicTarget::Angle);
  // `BrushState::load` is "pigment concentration" and ranges 0..2.5; a tip's
  // `flow` is "mass laid down per dab where coverage is 1" and is deliberately
  // not clamped to [0,1] (brush/Deposit.hpp: "a flow above 1 is a legitimate
  // one dab saturates the paper tip"). So this is the same number, scaled by
  // pressure, and not a remapping that would make the LOAD slider mean two
  // things.
  tip.flow = brush.load * flowMul;
  tip.spacing = brush.spacing * dyn.at(DynamicTarget::Spacing);
  // Straight through, unscaled: there is no `DynamicTarget::Opacity` in
  // brush/Dynamics' twelve, and inventing one here rather than in the matrix
  // that draws them would give the DYNAMICS panel a target it cannot show. The
  // clamp to a legal alpha is `RgbStroke::begin()`'s, at the point of use.
  tip.opacity = brush.opacity;

  const std::vector<Pigment>& palette = defaultPalette();
  const size_t index =
      brush.pigment >= 0 && static_cast<size_t>(brush.pigment) < palette.size()
          ? static_cast<size_t>(brush.pigment)
          : 0;
  const Pigment& pigment = palette[index];

  // **The same swatch, decoded, for the other layer kind** (brush/Deposit.hpp's
  // `linearRgb`). Derived here, from the same `pigment` the latent below is
  // derived from, so the two representations of one load are produced by one
  // statement pair and cannot name different colours.
  //
  // `paint/Palette`'s `rgb` is display-referred sRGB and a document part is
  // scene-referred linear (DESIGN-imaging.md, PRD B6). This is exactly
  // `ui/MacPaintUI::foregroundLinearRgba()`'s decode, and it is spelled out
  // again rather than called because `app/` must not include `ui/` -- the
  // dependency runs the other way. `--selftest` asserts the two agree, which is
  // the guard that spelling it twice needs.
  tip.linearRgb = {srgbDecode(pigment.rgb[0]), srgbDecode(pigment.rgb[1]),
                   srgbDecode(pigment.rgb[2])};

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

DynamicInputs dynamicInputsFor(const AppState& st) noexcept {
  DynamicInputs in;
  in.pressure = st.penSeen ? st.penPressure : 1.0f;
  in.tilt = st.penTilt;
  in.azimuth = st.penAzimuth;
  in.barrel = st.penBarrel;
  return in;
}

void applyPresetToBrush(const BrushPreset& preset, BrushState& brush) {
  brush.radius = preset.radius;
  brush.hardness = preset.hardness;
  brush.spacing = preset.spacing;
  brush.roundness = preset.roundness;
  brush.angle = preset.angle;
  brush.load = preset.load;
  brush.wetness = preset.wetness;
  brush.links = preset.links;
}

BrushPreset presetFromBrush(std::string name, const BrushState& brush) {
  BrushPreset p;
  p.name = std::move(name);
  p.radius = brush.radius;
  p.hardness = brush.hardness;
  p.spacing = brush.spacing;
  p.roundness = brush.roundness;
  p.angle = brush.angle;
  p.load = brush.load;
  p.wetness = brush.wetness;
  p.links = brush.links;
  return p;
}

bool brushIsEdited(const BrushState& brush) {
  if (brush.brushLibrary.active >= brush.brushLibrary.presets.size()) return false;
  const BrushPreset& p = brush.brushLibrary.presets[brush.brushLibrary.active];
  return !presetMatches(p, brush.radius, brush.hardness, brush.spacing, brush.roundness,
                        brush.angle, brush.load, brush.wetness, brush.links);
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
  if (!strokeRouteWritesLayer(route))
    return refuse(std::string("stroke refused: the ") + strokeEditLabel(tool) + " on layer " +
                  std::to_string(layerIndex) + " ('" + layer.name + "', " +
                  layerKindName(layer.kind) + (layer.locked ? ", locked" : "") +
                  ") routes to " + strokeRouteName(route) +
                  ", which does not write a layer.");

  doc_ = &doc;
  layerIndex_ = layerIndex;
  layerCount_ = doc.document.layers.size();
  tip_ = tip;
  route_ = route;
  tool_ = tool;
  label_ = strokeEditLabel(tool);

  // The ink, latched for the whole stroke -- brush/RgbDeposit.hpp §2 on why the
  // colour and the ceiling may not move once the accumulator has started, and
  // §3 on the accumulator being allocated here and freed at `end()`.
  //
  // The `else` is not redundant. A pigment or erase stroke that follows an RGB
  // deposit must leave nothing of it behind, for exactly `StrokePath::reset()`'s
  // reason below: alpha carried across strokes would let the ceiling of the last
  // stroke cap the first dab of the next -- and a session begun without a
  // matching `end()` (a window blur, an interrupted drag) is the case that
  // reaches this line holding tiles.
  if (route_ == StrokeRoute::RgbDeposit)
    rgb_.begin(tip.linearRgb, tip.opacity);
  else
    rgb_.end();

  // The strength, latched with it and for the identical reason
  // (brush/RgbErase.hpp §2): a stroke whose floor moved half way through has no
  // well-defined floor. `tip.opacity` is the same slider the deposit route reads
  // as its ceiling -- one control, one meaning, "the fraction of the maximum
  // effect one stroke may reach" -- and `tip.linearRgb` is deliberately NOT
  // read, because an eraser that had a colour would be a brush painting the
  // background (ADR-0007's rejected model).
  //
  // The `else` carries the same weight as the one above, one direction further:
  // erasure carried across strokes would let the floor of the last stroke stop
  // the first dab of the next, so a second pass would refuse to cut deeper.
  if (route_ == StrokeRoute::RgbErase)
    erase_.begin(tip.opacity);
  else
    erase_.end();

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
  // Against the route latched at pen-down, not merely against "some deposit
  // route" -- header §5's second paragraph on why a Pigment layer swapped for
  // an RGB one at the same index must end the stroke rather than continue it.
  //
  // **Asked with `tool_`, the session's own tool, and not with a stand-in.**
  // This line used to pass `Tool::Brush` on the argument that Brush and DryBrush
  // agree on every target and nothing else could have begun a session. The
  // eraser retires that argument: Brush and Eraser give two *different* answers
  // about one unchanged RGB layer, so a stand-in would find the route "changed"
  // on the second frame of every erase and silently drop the rest of the drag --
  // a tool that works for one frame and then stops, which is harder to diagnose
  // than one that never works at all.
  if (strokeRouteFor(tool_, &layer) != route_) {
    pending_.clear();
    return;
  }

  // PRD E1 (P0): the active selection bounds every deposit and every erase.
  // Read live rather than latched at pen-down -- nothing can install a selection
  // during a pointer drag, and reading it here means a session that outlived one
  // somehow cannot write through a stale bound.
  //
  // Hoisted out of the call below now that two of the three routes take it, so
  // the deposit and the erase cannot end up reading the selection two different
  // ways. The pigment route still takes none, which is the gap
  // `strokeRouteFor()`'s Pigment-eraser row refuses over.
  const Selection* selection = doc_->selection.has_value() ? &*doc_->selection : nullptr;

  // The three routes differ in exactly this call. Everything around it -- the
  // dab stream, the tile bookkeeping, the counters, the revision bump and the
  // single history entry -- is shared, because none of it is a property of what
  // a texel is made of or of which direction the stroke moves it.
  const StrokeDeposit deposited =
      route_ == StrokeRoute::RgbErase
          ? erase_.eraseDabs(*layer.rgbTiles, tip_, pending_, doc.width, doc.height, selection)
      : route_ == StrokeRoute::RgbDeposit
          ? rgb_.depositDabs(*layer.rgbTiles, tip_, pending_, doc.width, doc.height, selection)
          : depositDabs(*layer.pigmentTiles, tip_, pending_, doc.width, doc.height);
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
  // The accumulator's whole life is one stroke (brush/RgbDeposit.hpp §3), and
  // it is dropped here rather than at the next `begin()` so an application
  // sitting idle after a long stroke is not holding 64 KiB per tile it painted.
  // Unconditional: `end()` on an idle accumulator is a no-op, and a branch here
  // would be one more place the routes could disagree about cleanup. Both are
  // dropped, not the one this stroke used -- exactly one of them was ever live,
  // and asking which at cleanup time is how the other one keeps its tiles after
  // an interrupted drag.
  rgb_.end();
  erase_.end();

  // Exactly one entry, and only for a stroke that put something down --
  // header §2.
  if (texels_ > 0) doc->recordEdit(label_, EditKind::Content);
  return strokeTiles_;
}

}  // namespace np
