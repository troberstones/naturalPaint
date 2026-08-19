#pragma once
#include <vector>

namespace np {

// A position in canvas texel space. Used for raw input samples, spline
// control points, and emitted dab positions alike -- all three are simply
// points along a stroke's path.
struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

// Turns a stream of raw pointer samples into a stream of Dabs. CONTEXT.md:
// "One stamp of the brush tip, emitted every `spacing * radius` pixels of
// arc length along a Stroke -- never once per input event, and never scaled
// by frame time." ADR-0003 is what this exists to satisfy: deposition must
// depend on distance travelled, never on how much time or how many input
// events that distance was divided into.
//
// Feed it one new sample per render frame via addPoint(); it fits a
// centripetal Catmull-Rom curve through the last four samples, walks it, and
// appends however many dabs (0 or more) fall at `spacingPx` intervals since
// the last one. Sub-spacing leftover distance is carried across calls, so a
// slow stroke sampled every frame and a fast stroke sampled once every few
// frames still lay dabs down at the identical spatial spacing over the
// identical path -- the property that makes deposition speed-independent.
//
// Pure geometry: no notion of brush radius, pigment, or GPU state. The
// caller supplies `spacingPx` (already `spacing * radius`) and is
// responsible for turning each returned position into an actual deposit.
class StrokePath {
 public:
  // Clears all history and leftover distance. Call at the start of every new
  // stroke (pen/mouse down) -- carrying leftover distance or point history
  // across strokes would let the end of one stroke bias the start of the
  // next.
  void reset();

  // Feeds one new raw sample (canvas texel space) and appends any dabs
  // emitted between the previous sample and this one to `out`. `spacingPx`
  // is the arc-length spacing in pixels (spacing * radius); it is read once
  // per call, so it may change stroke-to-stroke or even frame-to-frame (e.g.
  // pressure-modulated radius) without needing to be fixed for a whole
  // stroke.
  void addPoint(float x, float y, float spacingPx, std::vector<Vec2>& out);

  // Call once at stroke end (pen/mouse up), before the next reset(). The
  // emitter always lags one real sample behind so every segment it walks is
  // bounded by real data on both sides (see the .cpp for why); that means
  // the very last segment of a stroke -- between the second-to-last and the
  // last sample -- never gets walked by addPoint() alone, because no further
  // sample ever arrives to confirm its shape. flush() walks that final
  // segment, extrapolating the one missing control point the same way the
  // *start* of a stroke already has to. Safe to call on a stroke with 0 or 1
  // samples (does nothing) or more than once (a no-op after the first).
  void flush(float spacingPx, std::vector<Vec2>& out);

 private:
  void emitAlongSegment(Vec2 P0, Vec2 P1, Vec2 P2, Vec2 P3, float spacingPx,
                        std::vector<Vec2>& out);

  Vec2 pts_[4];
  int numPts_ = 0;
  // Arc length walked since the last emitted dab, carried across addPoint()
  // calls (and into flush()) so spacing never resets at a render-frame
  // boundary.
  float leftover_ = 0.0f;
};

}  // namespace np
