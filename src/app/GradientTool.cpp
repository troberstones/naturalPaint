#include "app/GradientTool.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace np {

// The table § 4 of the header argues for. Order is the order the combo lists
// them, and it is deliberately the enum's own order so a reader comparing
// this against `ops/Gradient.hpp` can do it line by line.
const GradientSpreadRow kGradientSpreads[kGradientSpreadCount] = {
    {GradientSpread::Pad, "Clamp",
     "Outside the drag, the ramp's two end colours are held -- everything behind the start "
     "handle takes the first stop, everything past the end handle takes the last. The "
     "default, and the only mode that cannot surprise a short drag on a big canvas."},
    {GradientSpread::Repeat, "Repeat",
     "The ramp tiles outside the drag, restarting from the first stop each time. A hard seam "
     "wherever the last stop meets the first again, which is the point when the ramp is a "
     "stripe pattern and a defect when it is not."},
    {GradientSpread::Reflect, "Reflect",
     "The ramp tiles outside the drag, mirrored on every other repeat, so the last stop meets "
     "itself and there is no seam. Repeat's answer when the ramp's two ends do not match."},
};

// The count is not enough on its own -- `kToolMeta` (ui/AtelierChrome.cpp) is
// checked by exactly this kind of assert and still shipped rows in the wrong
// order, because a COUNT check passes on any permutation and on any duplicate.
// So `--selftest` walks the enum and asserts each value appears here exactly
// once; this assert is only the cheap half that fires at compile time.
static_assert(kGradientSpreadCount == 3,
              "kGradientSpreads must carry every GradientSpread; see the enum in "
              "ops/Gradient.hpp and app/selftest/GradientTool.cpp's coverage assertion");

const GradientKindRow kGradientKinds[kGradientKindCount] = {
    {GradientKind::Linear, "Linear",
     "The ramp runs along the drag and is constant across it -- every line perpendicular to "
     "the drag is one colour. The drag's start is the first stop, its end the last."},
    {GradientKind::Radial, "Radial",
     "The drag is a RADIUS, not a span: it starts at the centre and ends on the t=1 circle. "
     "Circular, never elliptical -- squashing one is a transform of the input point and "
     "belongs to the transform tools, not to a fourth kind here."},
    {GradientKind::Angular, "Angular",
     "The ramp sweeps once around the drag's start point, beginning along the drag and going "
     "CLOCKWISE on screen. The drag's length sets nothing -- only where the sweep begins. "
     "SPREAD does not apply: a sweep has no outside."},
};

static_assert(kGradientKindCount == 3,
              "kGradientKinds must carry every GradientKind; see the enum in "
              "ops/Gradient.hpp and app/selftest/GradientTool.cpp's coverage assertion");

const char* gradientKindLabel(GradientKind kind) {
  for (size_t i = 0; i < kGradientKindCount; ++i)
    if (kGradientKinds[i].kind == kind) return kGradientKinds[i].label;
  return "Linear";
}

bool gradientKindUsesSpread(GradientKind kind) {
  // Spelled as the positive list rather than `kind != Angular`, so a fourth
  // kind arriving in `ops/Gradient` has to be classified here on purpose. The
  // negative form would silently claim any new kind honours spread, which is
  // the answer that draws a live control over nothing.
  return kind == GradientKind::Linear || kind == GradientKind::Radial;
}

const char* gradientSpreadLabel(GradientSpread spread) {
  for (size_t i = 0; i < kGradientSpreadCount; ++i)
    if (kGradientSpreads[i].spread == spread) return kGradientSpreads[i].label;
  return "Clamp";
}

GradientStops resolveGradientPresetStops(const GradientPresetStops& spec,
                                         const std::array<float, 3>& foregroundLinear) {
  GradientStops stops;
  stops.colorStops.reserve(spec.colorStops.size());
  for (const GradientColorStopSpec& s : spec.colorStops) {
    const std::array<float, 3>& c = s.foreground ? foregroundLinear : s.color;
    stops.colorStops.push_back(ColorStop{s.position, c, s.midpoint});
  }
  stops.opacityStops.reserve(spec.opacityStops.size());
  for (const GradientOpacityStopSpec& s : spec.opacityStops)
    stops.opacityStops.push_back(OpacityStop{s.position, s.opacity, s.midpoint});
  return stops;
}

GradientStops gradientToolStops(const std::array<float, 4>& foregroundLinear,
                                const GradientPresetStops* custom) {
  if (custom != nullptr) {
    return resolveGradientPresetStops(
        *custom, {foregroundLinear[0], foregroundLinear[1], foregroundLinear[2]});
  }
  GradientStops stops;
  // The midpoint on every stop is 0.5 -- the linear interpolation
  // `gradientParameterAt()` degenerates to when the control is centred. This
  // build surfaces no midpoint control on the built-in default, so writing
  // anything else here would be a bias nothing in the UI could explain or
  // undo.
  const float r = foregroundLinear[0];
  const float g = foregroundLinear[1];
  const float b = foregroundLinear[2];
  stops.colorStops.push_back(ColorStop{0.0f, {r, g, b}, 0.5f});
  stops.colorStops.push_back(ColorStop{1.0f, {r, g, b}, 0.5f});
  stops.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
  stops.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});
  return stops;
}

float clampGradientStopPosition(float t) noexcept { return std::clamp(t, 0.0f, 1.0f); }

float clampGradientStopMidpoint(float m) noexcept {
  // The exact band `ops/Gradient.cpp`'s `applyMidpointSkew()` clamps to --
  // copied as a literal rather than shared through a header, because that
  // function is `ops/Gradient.cpp`'s own file-local `static`, and promoting
  // it to a header for one constant would be a bigger seam than restating a
  // number both files already state once each. `--selftest` (Gradient
  // Editor §C) asserts the two literals still agree.
  return std::clamp(m, 1e-3f, 1.0f - 1e-3f);
}

void sortGradientPresetStops(GradientPresetStops& stops) {
  std::stable_sort(stops.colorStops.begin(), stops.colorStops.end(),
                   [](const GradientColorStopSpec& a, const GradientColorStopSpec& b) {
                     return a.position < b.position;
                   });
  std::stable_sort(stops.opacityStops.begin(), stops.opacityStops.end(),
                   [](const GradientOpacityStopSpec& a, const GradientOpacityStopSpec& b) {
                     return a.position < b.position;
                   });
}

size_t addGradientColorStop(GradientPresetStops& stops, float position, bool foreground,
                            const std::array<float, 3>& color, float midpoint) {
  GradientColorStopSpec s;
  s.position = clampGradientStopPosition(position);
  s.foreground = foreground;
  s.color = color;
  s.midpoint = clampGradientStopMidpoint(midpoint);
  // Sorted insert via upper_bound rather than push_back-then-sort: the sort
  // is a stable_sort, and finding which of possibly several equal-position
  // stops is "the one just inserted" after it ran would need a second search
  // this avoids entirely -- `insertPoint()`'s own shape (`app/CurveEdit.cpp`).
  const auto it = std::upper_bound(
      stops.colorStops.begin(), stops.colorStops.end(), s.position,
      [](float t, const GradientColorStopSpec& c) { return t < c.position; });
  const size_t idx = static_cast<size_t>(it - stops.colorStops.begin());
  stops.colorStops.insert(it, s);
  return idx;
}

size_t addGradientOpacityStop(GradientPresetStops& stops, float position, float opacity,
                              float midpoint) {
  GradientOpacityStopSpec s;
  s.position = clampGradientStopPosition(position);
  s.opacity = std::clamp(opacity, 0.0f, 1.0f);
  s.midpoint = clampGradientStopMidpoint(midpoint);
  const auto it = std::upper_bound(
      stops.opacityStops.begin(), stops.opacityStops.end(), s.position,
      [](float t, const GradientOpacityStopSpec& o) { return t < o.position; });
  const size_t idx = static_cast<size_t>(it - stops.opacityStops.begin());
  stops.opacityStops.insert(it, s);
  return idx;
}

bool removeGradientColorStop(GradientPresetStops& stops, size_t index) {
  if (stops.colorStops.size() <= 2 || index >= stops.colorStops.size()) return false;
  stops.colorStops.erase(stops.colorStops.begin() + static_cast<ptrdiff_t>(index));
  return true;
}

bool removeGradientOpacityStop(GradientPresetStops& stops, size_t index) {
  if (stops.opacityStops.size() <= 2 || index >= stops.opacityStops.size()) return false;
  stops.opacityStops.erase(stops.opacityStops.begin() + static_cast<ptrdiff_t>(index));
  return true;
}

size_t moveGradientColorStop(GradientPresetStops& stops, size_t index, float position) {
  const GradientColorStopSpec moved = stops.colorStops.at(index);
  stops.colorStops.erase(stops.colorStops.begin() + static_cast<ptrdiff_t>(index));
  GradientColorStopSpec placed = moved;
  placed.position = clampGradientStopPosition(position);
  const auto it = std::upper_bound(
      stops.colorStops.begin(), stops.colorStops.end(), placed.position,
      [](float t, const GradientColorStopSpec& s) { return t < s.position; });
  const size_t idx = static_cast<size_t>(it - stops.colorStops.begin());
  stops.colorStops.insert(it, placed);
  return idx;
}

size_t moveGradientOpacityStop(GradientPresetStops& stops, size_t index, float position) {
  const GradientOpacityStopSpec moved = stops.opacityStops.at(index);
  stops.opacityStops.erase(stops.opacityStops.begin() + static_cast<ptrdiff_t>(index));
  GradientOpacityStopSpec placed = moved;
  placed.position = clampGradientStopPosition(position);
  const auto it = std::upper_bound(
      stops.opacityStops.begin(), stops.opacityStops.end(), placed.position,
      [](float t, const GradientOpacityStopSpec& s) { return t < s.position; });
  const size_t idx = static_cast<size_t>(it - stops.opacityStops.begin());
  stops.opacityStops.insert(it, placed);
  return idx;
}

std::optional<size_t> hitTestGradientStop(const std::vector<float>& positions, float t,
                                          float hitRadius) noexcept {
  std::optional<size_t> best;
  float bestDist = 0.0f;
  for (size_t i = 0; i < positions.size(); ++i) {
    const float d = std::fabs(positions[i] - t);
    if (d <= hitRadius && (!best.has_value() || d < bestDist)) {
      bestDist = d;
      best = i;
    }
  }
  return best;
}

namespace {
constexpr const char* kDefaultGradientPresetNameLiteral = "Foreground to Transparent";
}  // namespace

const char* defaultGradientPresetName() { return kDefaultGradientPresetNameLiteral; }

std::vector<GradientBuiltInPreset> builtInGradientPresets() {
  std::vector<GradientBuiltInPreset> presets;

  // Index 0, matching § 5's hard-coded default exactly (same positions, same
  // 0.5 midpoints) -- `--selftest` checks the two agree.
  GradientPresetStops fgToTransparent;
  fgToTransparent.colorStops.push_back(GradientColorStopSpec{0.0f, true, {0, 0, 0}, 0.5f});
  fgToTransparent.colorStops.push_back(GradientColorStopSpec{1.0f, true, {0, 0, 0}, 0.5f});
  fgToTransparent.opacityStops.push_back(GradientOpacityStopSpec{0.0f, 1.0f, 0.5f});
  fgToTransparent.opacityStops.push_back(GradientOpacityStopSpec{1.0f, 0.0f, 0.5f});
  presets.push_back({kDefaultGradientPresetNameLiteral, std::move(fgToTransparent)});

  // A second, fixed-colour built-in: proof that the picker's "not written to
  // disk" list is not just a re-statement of the one default, and a ramp a
  // user reaches for often enough that shipping it beats making everyone
  // author it themselves.
  GradientPresetStops blackToWhite;
  blackToWhite.colorStops.push_back(GradientColorStopSpec{0.0f, false, {0.0f, 0.0f, 0.0f}, 0.5f});
  blackToWhite.colorStops.push_back(GradientColorStopSpec{1.0f, false, {1.0f, 1.0f, 1.0f}, 0.5f});
  blackToWhite.opacityStops.push_back(GradientOpacityStopSpec{0.0f, 1.0f, 0.5f});
  blackToWhite.opacityStops.push_back(GradientOpacityStopSpec{1.0f, 1.0f, 0.5f});
  presets.push_back({"Black to White", std::move(blackToWhite)});

  return presets;
}

GradientGeometry gradientToolGeometry(const GradientToolState& tool, float x0, float y0,
                                      float x1, float y1) {
  GradientGeometry geom;
  geom.kind = tool.kind;
  geom.x0 = x0;
  geom.y0 = y0;
  geom.x1 = x1;
  geom.y1 = y1;
  // The spread is copied through unconditionally, INCLUDING for `Angular`,
  // which ignores it. Sanitising it to `Pad` here would be a second opinion
  // about a rule `gradientParameterAt()` already implements, and would also
  // silently forget the user's choice the moment they switched to Angular and
  // back -- the options bar disables the control for that kind rather than
  // rewriting the state behind it.
  geom.spread = tool.spread;
  return geom;
}

bool gradientDragIsUsable(float x0, float y0, float x1, float y1) {
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  return dx * dx + dy * dy >= 1.0f;
}


// ---------------------------------------------------------------------------
// § 3a. The session's handles (app/GradientTool.hpp)
// ---------------------------------------------------------------------------

std::vector<float> gradientEffectiveStopPositions(const GradientDrag& session,
                                                  const GradientStops& stops) {
  std::vector<float> positions;
  positions.reserve(stops.colorStops.size());
  for (const ColorStop& stop : stops.colorStops) positions.push_back(stop.position);
  // The override is only honoured when it still describes THIS ramp. Swapping
  // presets mid-session changes the stop count, and pasting old positions onto
  // a new ramp would silently rewrite a preset the user just chose.
  if (session.stopPositionOverride.size() == positions.size())
    positions = session.stopPositionOverride;
  return positions;
}

int gradientHandleAt(const GradientDrag& session, const GradientStops& stops, float x, float y,
                     float radius) noexcept {
  if (!session.active) return -1;

  // **Nearest hit wins outright here**, unlike the crop's corner-beats-edge
  // rule. Every handle on a ramp is a point on one line and stops can sit
  // arbitrarily close together, so there is no "more specific" class to prefer
  // -- the only defensible answer is the one the pointer is actually closest
  // to. This matters far more under touch: at a fingertip's 22px radius three
  // stops on a short ramp can all be in range at once, and a first-hit scan
  // would return whichever came first in the list rather than the one aimed at.
  int best = -1;
  float bestDist2 = radius * radius;
  const auto consider = [&](int handle, float px, float py) {
    const float dx = x - px;
    const float dy = y - py;
    const float d2 = dx * dx + dy * dy;
    // `<=` for the first candidate so an exact-radius hit counts, strict `<`
    // afterwards so ties resolve to the earlier handle and the answer is
    // stable rather than flickering with rounding.
    if (d2 <= bestDist2 && (best < 0 || d2 < bestDist2)) {
      best = handle;
      bestDist2 = d2;
    }
  };

  // Stops AT the ends are skipped entirely so the endpoints stay grabbable --
  // see `kGradientStopEdgeEpsilon`, which the band's drawing reads too.
  const std::vector<float> positions = gradientEffectiveStopPositions(session, stops);
  for (size_t i = 0; i < positions.size(); ++i) {
    const float t = positions[i];
    if (t <= kGradientStopEdgeEpsilon || t >= 1.0f - kGradientStopEdgeEpsilon) continue;
    consider(kGradientHandleStop + static_cast<int>(i),
             session.x0 + (session.x1 - session.x0) * t,
             session.y0 + (session.y1 - session.y0) * t);
  }
  consider(kGradientHandleStart, session.x0, session.y0);
  consider(kGradientHandleEnd, session.x1, session.y1);
  return best;
}

void gradientDragHandle(GradientDrag& session, const GradientStops& stops, int handle, float x,
                        float y) noexcept {
  if (handle == kGradientHandleStart) {
    session.x0 = x;
    session.y0 = y;
    return;
  }
  if (handle == kGradientHandleEnd) {
    session.x1 = x;
    session.y1 = y;
    return;
  }
  if (handle < kGradientHandleStop) return;

  const size_t index = static_cast<size_t>(handle - kGradientHandleStop);
  std::vector<float> positions = gradientEffectiveStopPositions(session, stops);
  if (index >= positions.size()) return;

  // A stop is a position ALONG the ramp, so the pointer is projected onto the
  // ramp's axis rather than followed freely -- dragging one sideways moves it
  // up or down the ramp, it does not pull it off the line. A degenerate ramp
  // has no axis to project onto and is left alone rather than dividing by zero.
  const float ax = session.x1 - session.x0;
  const float ay = session.y1 - session.y0;
  const float len2 = ax * ax + ay * ay;
  if (len2 < 1e-6f) return;
  float t = ((x - session.x0) * ax + (y - session.y0) * ay) / len2;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  positions[index] = t;
  session.stopPositionOverride = std::move(positions);
}

void gradientBeginDefine(GradientDrag& session, DocumentId doc, float x, float y) noexcept {
  session.active = true;
  session.defining = true;
  session.doc = doc;
  session.x0 = x;
  session.y0 = y;
  // The far end starts ON the near one, so a click that never drags is
  // degenerate and `gradientDragIsUsable()` refuses it -- rather than
  // inheriting the last session's endpoint and aiming a ramp somewhere the
  // pointer has not been.
  session.x1 = x;
  session.y1 = y;
  session.dragHandle = -1;
  // Positions come back from the ramp itself until a handle is moved again.
  session.stopPositionOverride.clear();
}

void gradientCancel(GradientDrag& session) noexcept {
  session.active = false;
  session.defining = false;
  session.doc = 0;
  session.dragHandle = -1;
  session.stopPositionOverride.clear();
}

}  // namespace np
