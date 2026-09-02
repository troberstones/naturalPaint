#include "brush/Library.hpp"

#include <cmath>

namespace np {
namespace {

BrushLink makeLink(DynamicSource source, DynamicTarget target, float lo, float hi,
                   EasingPreset preset = EasingPreset::Linear) {
  BrushLink l;
  l.source = source;
  l.target = target;
  l.rangeLo = lo;
  l.rangeHi = hi;
  if (preset != EasingPreset::Linear) l.curve = easingCurve(preset);
  return l;
}

bool curvesEqual(const Curve& a, const Curve& b) {
  // An empty curve and an explicit linear one behave identically
  // (`linkContribution()` treats empty as a pass-through), so they must
  // compare equal or a brush saved through the LINEAR chip would read as
  // edited against a default-constructed link that means the same thing.
  const Curve linear = easingCurve(EasingPreset::Linear);
  const Curve& ea = a.empty() ? linear : a;
  const Curve& eb = b.empty() ? linear : b;
  if (ea.size() != eb.size()) return false;
  for (size_t i = 0; i < ea.size(); ++i) {
    if (ea[i].x != eb[i].x || ea[i].y != eb[i].y) return false;
  }
  return true;
}

bool linksEqual(const BrushLink& a, const BrushLink& b) {
  return a.source == b.source && a.target == b.target && a.rangeLo == b.rangeLo &&
         a.rangeHi == b.rangeHi && a.invert == b.invert && a.enabled == b.enabled &&
         curvesEqual(a.curve, b.curve);
}

}  // namespace

bool linkSetsEqual(const BrushLinkSet& a, const BrushLinkSet& b) {
  // `multiplyFloor` is not part of any `BrushLink`, so the per-cell walk
  // below cannot see it -- checked first, and directly, or two sets that
  // differ ONLY in their Minimum Diameter floor would compare equal, which
  // would make `presetMatches()` (below) tell the Brush Editor a live brush
  // still matches a saved preset it has actually drifted from. Nothing in
  // this build's UI edits `multiplyFloor` today (only `io/AbrBrushes.cpp`'s
  // importer and `app/UserBrushLibrary.cpp`'s file reader ever write it), so
  // this is dormant rather than reachable right now -- but a correctness
  // function that is only accidentally correct is the exact kind of gap
  // this codebase's own audit exists to name rather than leave for later.
  for (size_t t = 0; t < kDynamicTargetCount; ++t)
    if (a.multiplyFloor[t] != b.multiplyFloor[t]) return false;
  if (a.links.size() != b.links.size()) return false;
  // Matched by CELL rather than by position: `addLink()` replaces in place and
  // `removeLink()` erases, so two sets that hold the same links can hold them
  // in different orders. Sizes are equal and cells are unique, so a match for
  // every link of `a` is a bijection -- no need to walk `b` as well.
  for (const BrushLink& la : a.links) {
    const size_t at = findLink(b, la.source, la.target);
    if (at == kNoLink) return false;
    if (!linksEqual(la, b.links[at])) return false;
  }
  return true;
}

bool presetMatches(const BrushPreset& preset, float radius, float hardness, float spacing,
                   float roundness, float angle, float load, float wetness,
                   const BrushLinkSet& links, const GrainParams& grain) {
  // Radius/hardness/spacing/roundness/angle now live on `preset.model`
  // (`brush/Library.hpp`'s own comment on why the five scalars this function
  // used to compare directly are gone) -- projected back into the same units
  // this function's own parameters have always taken, so every caller
  // (`app/StrokeSession.cpp`'s `brushIsEdited()`) keeps passing the exact
  // values it always did, just read from `brush.model` now instead of from
  // five deleted `BrushState` fields.
  return preset.model.tip.diameterPx / 2.0f == radius &&
         preset.model.tip.hardness == hardness &&
         preset.model.tip.spacingPercent / 100.0f == spacing &&
         preset.model.tip.roundness == roundness && preset.model.tip.angleDeg == angle &&
         preset.load == load && preset.wetness == wetness &&
         linkSetsEqual(preset.links, links) && grainParamsEqual(preset.grain, grain);
}

std::string uniquePresetName(const BrushLibrary& lib, const std::string& wanted) {
  const auto taken = [&](const std::string& n) {
    for (const BrushPreset& p : lib.presets)
      if (p.name == n) return true;
    return false;
  };
  if (!taken(wanted)) return wanted;
  for (int i = 2; i < 10000; ++i) {
    const std::string candidate = wanted + " " + std::to_string(i);
    if (!taken(candidate)) return candidate;
  }
  return wanted;
}

BrushLibrary defaultBrushLibrary() {
  BrushLibrary lib;

  // The design's own brush, and the one whose settings 4a photographs.
  //
  // **Every field is left at BrushPreset's/BrushModel's default, and that is
  // load-bearing rather than lazy**: those defaults are `BrushState`'s
  // defaults too, and this is `active = 0`, so a freshly launched app has a
  // live brush that matches the preset it claims to be on. Set anything here
  // that `BrushState`/`BrushModel` does not also default to -- 4a's own
  // slider reads 24 px, for instance -- and the editor opens showing EDITED
  // on a brush nobody has touched, which trains people to ignore the one
  // badge the LIBRARY pane's discard warning relies on. --selftest asserts a
  // default AppState is not edited.
  //
  // **`model.tip.hardness` is the one field this brush DOES set explicitly**,
  // and that is the one place `BrushModel`'s own default (1.0, a hard disc --
  // Photoshop's `Hrdn` is present on computed tips only, brush/BrushModel.hpp
  // §comment) disagrees with what this preset has always painted (0.35, the
  // old `BrushPreset::radius`-family default). Left at `BrushModel`'s default
  // here, this brush would silently harden -- so it is named, not implied.
  BrushPreset round;
  round.name = "Round Bristle 03";
  round.builtin = true;
  round.model.tip.hardness = 0.35f;
  round.links = defaultBrushLinks();
  lib.presets.push_back(round);

  // A broad flat, which is what roundness and angle are FOR: an elliptical tip
  // held at an angle. Pressure drives flow but not size, because a flat wash
  // brush laid harder puts down more water, not a wider mark.
  //
  // **The Pressure -> Flow and Tilt -> Roundness links below no longer paint
  // anything.** The matrix is shelved (`ui/DynamicsMatrixPanel.hpp`) and
  // `app/StrokeSession::brushTipFor()` does not read `BrushLinkSet` any more
  // -- only an imported `.abr` preset's `BrushModel` carries real per-dab
  // dynamics now, and this hand-authored built-in was never given one. The
  // links stay (so the shelved matrix editor, `--advanced-dynamics`, still
  // has something real to show for this preset) but this brush now paints
  // as a plain, undriven flat: still 92 px, still 12% hardness, still tilted
  // 35 degrees, just without Pressure moving its flow or Tilt moving its
  // roundness mid-stroke. A real, stated behaviour change for this one
  // built-in, not a silent one -- see this migration's own report for the
  // full accounting.
  BrushPreset wash;
  wash.name = "Flat Wash";
  wash.builtin = true;
  wash.model.tip.diameterPx = 92.0f;
  wash.model.tip.hardness = 0.12f;
  wash.model.tip.spacingPercent = 12.0f;
  wash.model.tip.roundness = 0.28f;
  wash.model.tip.angleDeg = 35.0f;
  wash.load = 0.7f;
  wash.wetness = 2.4f;
  addLink(wash.links, makeLink(DynamicSource::Pressure, DynamicTarget::Flow, 0.2f, 1.0f));
  // Tilt widens it, which is the flat's whole behaviour: rolled onto its edge
  // it draws thin, laid down it draws its full width.
  addLink(wash.links, makeLink(DynamicSource::Tilt, DynamicTarget::Roundness, 0.28f, 1.0f));
  lib.presets.push_back(wash);

  // A dry bristle, where the interest is in the scatter and the spacing rather
  // than in pressure -- the marks break up because the dabs do.
  //
  // **Its three links (Pressure -> Size, Random -> Scatter, Random -> Flow)
  // are the identical dead-link case `wash` above documents.** This preset
  // now paints a plain 36 px / 85% hardness / 55% spacing round dab with no
  // per-dab jitter at all -- it used to be the one built-in whose marks broke
  // up from scatter and thinned under a light touch, and it no longer does
  // either.
  BrushPreset dry;
  dry.name = "Dry Bristle";
  dry.builtin = true;
  dry.model.tip.diameterPx = 36.0f;
  dry.model.tip.hardness = 0.85f;
  dry.model.tip.spacingPercent = 55.0f;
  dry.load = 0.55f;
  dry.wetness = 0.25f;
  addLink(dry.links, makeLink(DynamicSource::Pressure, DynamicTarget::Size, 0.55f, 1.0f));
  addLink(dry.links, makeLink(DynamicSource::Random, DynamicTarget::Scatter, 0.0f, 0.45f));
  addLink(dry.links, makeLink(DynamicSource::Random, DynamicTarget::Flow, 0.35f, 1.0f));
  lib.presets.push_back(dry);

  // A liner: small, hard, and steeply pressure-sensitive, so the line can be
  // hairline or full width within one stroke. The S curve is what puts the
  // useful part of that range in the middle of the pressure the hand actually
  // uses rather than at the extremes.
  //
  // **Its Pressure -> Size link is the identical dead-link case above.** This
  // preset now paints a constant 10 px line regardless of pressure -- the one
  // built-in whose whole point was that pressure range, now inert for the
  // same reason `wash`/`dry` are above.
  BrushPreset liner;
  liner.name = "Detail Liner";
  liner.builtin = true;
  liner.model.tip.diameterPx = 10.0f;
  liner.model.tip.hardness = 0.95f;
  liner.model.tip.spacingPercent = 8.0f;
  liner.load = 1.4f;
  liner.wetness = 0.6f;
  addLink(liner.links, makeLink(DynamicSource::Pressure, DynamicTarget::Size, 0.08f, 1.0f,
                                EasingPreset::SCurve));
  addLink(liner.links, makeLink(DynamicSource::Pressure, DynamicTarget::Flow, 0.4f, 1.0f));
  lib.presets.push_back(liner);

  return lib;
}

}  // namespace np
