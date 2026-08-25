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
                   const BrushLinkSet& links) {
  return preset.radius == radius && preset.hardness == hardness && preset.spacing == spacing &&
         preset.roundness == roundness && preset.angle == angle && preset.load == load &&
         preset.wetness == wetness && linkSetsEqual(preset.links, links);
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
  // **Every field is left at BrushPreset's default, and that is load-bearing
  // rather than lazy**: those defaults are `BrushState`'s defaults, and this
  // is `active = 0`, so a freshly launched app has a live brush that matches
  // the preset it claims to be on. Set anything here that BrushState does not
  // also default to -- 4a's own slider reads 24 px, for instance -- and the
  // editor opens showing EDITED on a brush nobody has touched, which trains
  // people to ignore the one badge the LIBRARY pane's discard warning relies
  // on. --selftest asserts a default AppState is not edited.
  BrushPreset round;
  round.name = "Round Bristle 03";
  round.links = defaultBrushLinks();
  lib.presets.push_back(round);

  // A broad flat, which is what roundness and angle are FOR: an elliptical tip
  // held at an angle. Pressure drives flow but not size, because a flat wash
  // brush laid harder puts down more water, not a wider mark.
  BrushPreset wash;
  wash.name = "Flat Wash";
  wash.radius = 46.0f;
  wash.hardness = 0.12f;
  wash.spacing = 0.12f;
  wash.roundness = 0.28f;
  wash.angle = 35.0f;
  wash.load = 0.7f;
  wash.wetness = 2.4f;
  addLink(wash.links, makeLink(DynamicSource::Pressure, DynamicTarget::Flow, 0.2f, 1.0f));
  // Tilt widens it, which is the flat's whole behaviour: rolled onto its edge
  // it draws thin, laid down it draws its full width.
  addLink(wash.links, makeLink(DynamicSource::Tilt, DynamicTarget::Roundness, 0.28f, 1.0f));
  lib.presets.push_back(wash);

  // A dry bristle, where the interest is in the scatter and the spacing rather
  // than in pressure -- the marks break up because the dabs do.
  BrushPreset dry;
  dry.name = "Dry Bristle";
  dry.radius = 18.0f;
  dry.hardness = 0.85f;
  dry.spacing = 0.55f;
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
  BrushPreset liner;
  liner.name = "Detail Liner";
  liner.radius = 5.0f;
  liner.hardness = 0.95f;
  liner.spacing = 0.08f;
  liner.load = 1.4f;
  liner.wetness = 0.6f;
  addLink(liner.links, makeLink(DynamicSource::Pressure, DynamicTarget::Size, 0.08f, 1.0f,
                                EasingPreset::SCurve));
  addLink(liner.links, makeLink(DynamicSource::Pressure, DynamicTarget::Flow, 0.4f, 1.0f));
  lib.presets.push_back(liner);

  return lib;
}

}  // namespace np
