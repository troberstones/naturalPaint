#pragma once

#include <string>

#include "brush/BrushModel.hpp"

// brush/BrushModelFields -- **the one place BrushModel's field list is
// written down**, and the walk every other module reads it through.
//
// A `BrushModel` is Photoshop's Brush Settings panel as a struct: 151 leaves
// once every `Variance` is expanded and the Dual Brush's own tip and scatter
// are counted as the second copies they are. Three things now need to walk
// that list -- writing it to `user-presets.txt` (brush/BrushModelIo),
// comparing two of them for the EDITED indicator and for round-trip proofs
// (brush/BrushModelDiff), and eventually drawing a control per field
// (Photoshop-shaped panels).
//
// **They walk THIS list or the list forks.** Two copies of a 151-field
// enumeration is not a hypothetical drift: it is three edits per new field,
// two of which a reviewer has to notice are missing. The pinned counts in
// both selftests would catch a forgotten field -- but they would catch it as
// "some count moved", in whichever module was edited second, which is a
// worse diagnostic than a compile error and a much worse one than a list
// that cannot disagree with itself.
//
// This file was extracted at integration time from two independently written
// visitors that had arrived at nearly the same decomposition. Their agreement
// is the evidence that this decomposition is the natural one; their existence
// as two files was the defect.
//
// ---------------------------------------------------------------------------
// Two shapes, one field list
// ---------------------------------------------------------------------------
//
//   `detail::visitBrushModel(a, b, visit)` walks TWO models in lockstep and
//   calls `visit(path, aLeaf, bLeaf)`, which returns whether to keep going.
//   That is the general form: comparison needs both sides, and early exit is
//   what makes `brushModelEqual()` cheaper than building a diff and asking
//   whether it is empty.
//
//   `visitBrushModelFields(m, fn)` is the one-model case, `fn(path, leaf)`,
//   built on the general form by passing the same model twice. `M` is
//   deduced, so a `const BrushModel&` gives const leaves and a mutable one
//   gives writable leaves **from the same template body** -- which is what
//   lets serialising (read) and parsing (write) share a walk without either
//   a `const_cast` or a second const-qualified copy of the list.

namespace np {

namespace detail {

// Variance -- Control, Jitter, Minimum, Fade Steps, and `present` (whether
// the key was in the file at all; Variance.hpp's own note on why that is a
// fifth fact, not a fourth, and a different one from "the file said Off").
// All five are visited: skipping `fadeSteps` because it only matters when
// `control == Fade` would silently swallow an edit to a Fade brush's fade
// length, and skipping `present` would erase exactly the "said nothing" vs
// "said Off" distinction the field exists to carry.
template <typename A, typename B, typename Visit>
bool visitVariance(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".control", a.control, b.control)) return false;
  if (!visit(prefix + ".jitter", a.jitter, b.jitter)) return false;
  if (!visit(prefix + ".minimum", a.minimum, b.minimum)) return false;
  if (!visit(prefix + ".fadeSteps", a.fadeSteps, b.fadeSteps)) return false;
  if (!visit(prefix + ".present", a.present, b.present)) return false;
  return true;
}

// PsTipShape. `dab.bitmap` is deliberately NOT one of these leaves: it is a
// `shared_ptr<const BrushTipBitmap>` resolved FROM `dab.id` at load time and
// never persisted (BrushModel.hpp's own comment on the field), so two models
// naming the same id are the same brush whether or not one of them happens
// to have its bitmap resolved yet -- mid-import versus after a picker click,
// say. Comparing the pointer would make "not loaded yet" register as an
// edit; comparing the pointee would need a deep bitmap comparison to restate
// a fact `id` already carries exactly.
template <typename A, typename B, typename Visit>
bool visitTipShape(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".dab.id", a.dab.id, b.dab.id)) return false;
  if (!visit(prefix + ".diameterPx", a.diameterPx, b.diameterPx)) return false;
  if (!visit(prefix + ".angleDeg", a.angleDeg, b.angleDeg)) return false;
  if (!visit(prefix + ".roundness", a.roundness, b.roundness)) return false;
  if (!visit(prefix + ".spacingPercent", a.spacingPercent, b.spacingPercent)) return false;
  if (!visit(prefix + ".hardness", a.hardness, b.hardness)) return false;
  if (!visit(prefix + ".spacingEnabled", a.spacingEnabled, b.spacingEnabled)) return false;
  if (!visit(prefix + ".flipX", a.flipX, b.flipX)) return false;
  if (!visit(prefix + ".flipY", a.flipY, b.flipY)) return false;
  if (!visit(prefix + ".computed", a.computed, b.computed)) return false;
  return true;
}

// PsScatter -- shared by BrushModel::scatter and PsDualBrush::scatter, so
// this one function contributes 13 leaves at TWO different prefixes
// ("scatter.*" and "dual.scatter.*"), not 13 total.
template <typename A, typename B, typename Visit>
bool visitScatter(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visitVariance(prefix + ".scatter", a.scatter, b.scatter, visit)) return false;
  if (!visit(prefix + ".bothAxes", a.bothAxes, b.bothAxes)) return false;
  if (!visit(prefix + ".count", a.count, b.count)) return false;
  if (!visitVariance(prefix + ".countJitter", a.countJitter, b.countJitter, visit)) return false;
  return true;
}

// PsShapeDynamics.
template <typename A, typename B, typename Visit>
bool visitShapeDynamics(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visitVariance(prefix + ".size", a.size, b.size, visit)) return false;
  if (!visitVariance(prefix + ".angle", a.angle, b.angle, visit)) return false;
  if (!visitVariance(prefix + ".roundness", a.roundness, b.roundness, visit)) return false;
  if (!visit(prefix + ".flipXJitter", a.flipXJitter, b.flipXJitter)) return false;
  if (!visit(prefix + ".flipYJitter", a.flipYJitter, b.flipYJitter)) return false;
  if (!visit(prefix + ".brushProjection", a.brushProjection, b.brushProjection)) return false;
  if (!visit(prefix + ".tiltScale", a.tiltScale, b.tiltScale)) return false;
  return true;
}

// PsTexture. `pattern.name` IS compared, unlike `dab.bitmap` above: a
// `PatternRef` carries no resolved/derived pointer, so `id` and `name` are
// both source-of-truth fields rather than one being cached off the other.
template <typename A, typename B, typename Visit>
bool visitTexture(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visit(prefix + ".pattern.id", a.pattern.id, b.pattern.id)) return false;
  if (!visit(prefix + ".pattern.name", a.pattern.name, b.pattern.name)) return false;
  if (!visit(prefix + ".invert", a.invert, b.invert)) return false;
  if (!visit(prefix + ".scalePercent", a.scalePercent, b.scalePercent)) return false;
  if (!visit(prefix + ".depth", a.depth, b.depth)) return false;
  if (!visit(prefix + ".minimumDepth", a.minimumDepth, b.minimumDepth)) return false;
  if (!visitVariance(prefix + ".depthJitter", a.depthJitter, b.depthJitter, visit)) return false;
  if (!visit(prefix + ".blend", a.blend, b.blend)) return false;
  if (!visit(prefix + ".brightness", a.brightness, b.brightness)) return false;
  if (!visit(prefix + ".contrast", a.contrast, b.contrast)) return false;
  if (!visit(prefix + ".eachTip", a.eachTip, b.eachTip)) return false;
  if (!visit(prefix + ".protectTexture", a.protectTexture, b.protectTexture)) return false;
  return true;
}

// PsDualBrush -- its own `enabled`/`blend`/`flip` plus a nested PsTipShape
// and a nested PsScatter, both walked through the same two functions above
// rather than re-listed here.
template <typename A, typename B, typename Visit>
bool visitDualBrush(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visitTipShape(prefix + ".tip", a.tip, b.tip, visit)) return false;
  if (!visit(prefix + ".blend", a.blend, b.blend)) return false;
  if (!visitScatter(prefix + ".scatter", a.scatter, b.scatter, visit)) return false;
  if (!visit(prefix + ".flip", a.flip, b.flip)) return false;
  return true;
}

// PsColorDynamics.
template <typename A, typename B, typename Visit>
bool visitColorDynamics(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visit(prefix + ".perTip", a.perTip, b.perTip)) return false;
  if (!visitVariance(prefix + ".foregroundBackground", a.foregroundBackground,
                     b.foregroundBackground, visit))
    return false;
  if (!visit(prefix + ".hueJitter", a.hueJitter, b.hueJitter)) return false;
  if (!visit(prefix + ".saturationJitter", a.saturationJitter, b.saturationJitter)) return false;
  if (!visit(prefix + ".brightnessJitter", a.brightnessJitter, b.brightnessJitter)) return false;
  if (!visit(prefix + ".purity", a.purity, b.purity)) return false;
  return true;
}

// PsTransfer -- four Variance members and nothing else; `wetness` and `mix`
// have no engine target today (PsTransfer's own comment) but they are still
// part of what the file, and the EDITED badge, must remember.
template <typename A, typename B, typename Visit>
bool visitTransfer(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".enabled", a.enabled, b.enabled)) return false;
  if (!visitVariance(prefix + ".opacity", a.opacity, b.opacity, visit)) return false;
  if (!visitVariance(prefix + ".flow", a.flow, b.flow, visit)) return false;
  if (!visitVariance(prefix + ".wetness", a.wetness, b.wetness, visit)) return false;
  if (!visitVariance(prefix + ".mix", a.mix, b.mix, visit)) return false;
  return true;
}

// PsToolOptions, including the tool preset's own four override Variances
// (parsed and carried, never applied -- PsToolOptions's own comment -- but
// still part of the model, so still part of the diff).
template <typename A, typename B, typename Visit>
bool visitToolOptions(const std::string& prefix, A& a, B& b, Visit& visit) {
  if (!visit(prefix + ".blendMode", a.blendMode, b.blendMode)) return false;
  if (!visit(prefix + ".opacity", a.opacity, b.opacity)) return false;
  if (!visit(prefix + ".flow", a.flow, b.flow)) return false;
  if (!visit(prefix + ".smoothing", a.smoothing, b.smoothing)) return false;
  if (!visit(prefix + ".pressureOverridesSize", a.pressureOverridesSize, b.pressureOverridesSize))
    return false;
  if (!visit(prefix + ".pressureOverridesOpacity", a.pressureOverridesOpacity,
             b.pressureOverridesOpacity))
    return false;
  if (!visit(prefix + ".useLegacy", a.useLegacy, b.useLegacy)) return false;
  if (!visitVariance(prefix + ".sizeOverride", a.sizeOverride, b.sizeOverride, visit))
    return false;
  if (!visitVariance(prefix + ".opacityOverride", a.opacityOverride, b.opacityOverride, visit))
    return false;
  if (!visitVariance(prefix + ".flowOverride", a.flowOverride, b.flowOverride, visit))
    return false;
  if (!visitVariance(prefix + ".colorOverride", a.colorOverride, b.colorOverride, visit))
    return false;
  return true;
}

// BrushModel itself -- the eight named panels, each already handled above,
// plus the checkbox tail and naturalPaint's own load/wetness. THIS is the
// one function a new top-level BrushModel field costs one line in; every
// nested struct's own field costs one line in the visitor function for
// THAT struct, above.
template <typename A, typename B, typename Visit>
bool visitBrushModel(A& a, B& b, Visit& visit) {
  if (!visitTipShape("tip", a.tip, b.tip, visit)) return false;
  if (!visitShapeDynamics("shape", a.shape, b.shape, visit)) return false;
  if (!visitScatter("scatter", a.scatter, b.scatter, visit)) return false;
  if (!visitTexture("texture", a.texture, b.texture, visit)) return false;
  if (!visitDualBrush("dual", a.dual, b.dual, visit)) return false;
  if (!visitColorDynamics("color", a.color, b.color, visit)) return false;
  if (!visitTransfer("transfer", a.transfer, b.transfer, visit)) return false;
  if (!visitToolOptions("options", a.options, b.options, visit)) return false;
  if (!visit("noise", a.noise, b.noise)) return false;
  if (!visit("wetEdges", a.wetEdges, b.wetEdges)) return false;
  if (!visit("airbrush", a.airbrush, b.airbrush)) return false;
  if (!visit("brushPose", a.brushPose, b.brushPose)) return false;
  if (!visit("load", a.load, b.load)) return false;
  if (!visit("wetness", a.wetness, b.wetness)) return false;
  return true;
}

}  // namespace detail

// The one-model case. `fn` is called as `fn(path, leaf)` and its return value
// is ignored -- a caller that wants to stop early is doing a comparison and
// should use the two-model form directly.
//
// Passing `m` as both sides is not a trick to apologise for: the general form
// deduces `A` and `B` independently, so both leaves are the same reference
// and the adaptor simply drops one. The alternative -- a second single-model
// template body -- is the forked field list this file exists to prevent.
template <typename M, typename Fn>
void visitBrushModelFields(M& m, Fn&& fn) {
  auto adapt = [&](const std::string& path, auto& a, auto&) {
    fn(path, a);
    return true;
  };
  detail::visitBrushModel(m, m, adapt);
}

}  // namespace np
