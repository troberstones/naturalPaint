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

// One raw pointer sample: a position plus the four hardware axes at that
// instant, already normalised to [0,1] -- the same four fields
// `app/PenAxes.hpp`'s conversions and `brush/Dynamics.hpp`'s `DynamicInputs`
// carry (pressure, tilt, azimuth, barrel). Track A (full-rate pen input):
// `app/AppState`'s per-frame `PointerSample` queue is converted to these at
// the canvas block's boundary -- through `penTiltNormalised()` etc., never
// re-derived here -- so this type and everything downstream of it never sees
// a raw SDL axis or a window coordinate.
//
// Defaults are a mouse's neutral reading: full pressure, no tilt, no
// rotation, at the pen's own rest orientation (barrel 0.5 -- the same value
// `penBarrelNormalised(0)` returns, which is also what a device that cannot
// report barrel rotation at all, a mouse included, shares that reading
// with). Matches `DynamicInputs`' own "a mouse at full pressure" defaults
// except for barrel, which is 0.5 here rather than that struct's bare 0.0 --
// see `DynamicInputs::barrel`'s comment for why its own default differs (it
// is read only when `hasBarrel` says a device is reporting one at all, which
// makes the float moot; this struct has no such flag, so its default has to
// be the honest "no rotation" reading on its own).
struct StrokeSample {
  Vec2 pos;
  float pressure = 1.0f;
  float tilt = 0.0f;
  float azimuth = 0.0f;
  float barrel = 0.5f;
};

// An emitted dab: a position along the walked curve plus the axes
// interpolated for it (StrokePath.cpp's own comment on `emitAlongSegment()`
// says how). Same four fields as `StrokeSample` -- a dab IS a sample located
// on the curve rather than at a raw input point, so a second, differently
// named struct would only be two spellings of one shape. Kept as a distinct
// name rather than used as `StrokeSample` directly at call sites, purely for
// readability: `std::vector<StrokeDab>& out` reads as "what this emits",
// `const StrokeSample&` as "what this consumes".
using StrokeDab = StrokeSample;

// Turns a stream of raw pointer samples into a stream of Dabs. CONTEXT.md:
// "One stamp of the brush tip, emitted every `spacing * radius` pixels of
// arc length along a Stroke -- never once per input event, and never scaled
// by frame time." ADR-0003 is what this exists to satisfy: deposition must
// depend on distance travelled, never on how much time or how many input
// events that distance was divided into.
//
// **Feed it one new sample per raw pointer EVENT, not once per render
// frame.** This header used to say the opposite -- "feed it one new sample
// per render frame" -- which was ADR-0003 obeyed for POSITION and quietly
// broken for every axis riding along with it: a tablet reporting at 133-200
// Hz into a 60 Hz frame had its pressure/tilt/azimuth/barrel samples
// collapsed to whichever one arrived last, so a single frame's worth of
// dabs (a fast stroke can emit dozens) all shared one pressure. Feeding this
// class every raw sample -- `app/AppState`'s per-frame `PointerSample`
// queue, drained by the canvas block -- and letting it interpolate the axes
// PER DAB rather than share one per frame is what fixes that without
// touching ADR-0003 at all: dab POSITIONS still come from the identical
// arc-length walk over the identical Catmull-Rom curve, at the identical
// spacing, regardless of how many samples that walk is fed from. A caller
// that still only has one sample a frame (the solver route, `app/BrushSheet`,
// every existing selftest) keeps working unchanged through the `Vec2`-only
// overloads below, which feed a neutral axis reading through the exact same
// geometry.
//
// It fits a centripetal Catmull-Rom curve through the last four samples,
// walks it, and appends however many dabs (0 or more) fall at `spacingPx`
// intervals since the last one. Sub-spacing leftover distance is carried
// across calls, so a slow stroke sampled densely and a fast stroke sampled
// sparsely still lay dabs down at the identical spatial spacing over the
// identical path -- the property that makes deposition speed-independent.
//
// **Each emitted dab's axes are linearly interpolated in the Catmull-Rom
// parameter `u` across the P1->P2 span it falls in**, between the two REAL
// samples that bound that span (P1 and P2 are always real recorded samples
// in both `addPoint()` and `flush()` -- only P0/P3, the curve's look-ahead/
// look-behind control points, are ever extrapolated, and extrapolated points
// carry no axis information anything reads). `u`, not an arc-length
// fraction: the two coincide exactly for the common case this matters most
// -- a straight or near-straight stroke with evenly spaced samples, where
// centripetal knot spacing is uniform and the curve's own parameterisation
// degenerates to linear (`StrokePath.cpp`'s `emitAlongSegment()` comment) --
// and elsewhere `u` is what the position walk already has in hand at every
// dab, at zero extra cost; recovering a true arc-length fraction instead
// would mean a second numerical integration of the SAME curve this file
// already walks once, for axes a user cannot perceive to sub-percent
// precision at any real pen speed. `app/selftest/StrokeInput.cpp` derives
// and asserts the bound between the two for a fixture where they are not
// identical.
//
// The one gesture that has no arc length to divide is a click that never
// moved, and it is handled at flush() rather than by any rule about spacing:
// a stationary stroke emits exactly one dab, at its own position and its own
// sampled axes (there is only ever one real sample to have read them from).
// That is not a violation of the distance-not-time rule above -- one dab is
// what a click deposits whether it was held for one frame or three hundred.
//
// Otherwise pure geometry: no notion of brush radius, pigment, or GPU state.
// The caller supplies `spacingPx` (already `spacing * radius`) and is
// responsible for turning each returned dab into an actual deposit.
class StrokePath {
 public:
  // Clears all history and leftover distance. Call at the start of every new
  // stroke (pen/mouse down) -- carrying leftover distance or point history
  // across strokes would let the end of one stroke bias the start of the
  // next.
  void reset();

  // Feeds one new raw sample (canvas texel space, plus its own axes) and
  // appends any dabs emitted between the previous sample and this one to
  // `out`. `spacingPx` is the arc-length spacing in pixels (spacing *
  // radius); it is read once per call, so it may change stroke-to-stroke or
  // even sample-to-sample (e.g. pressure-modulated radius) without needing
  // to be fixed for a whole stroke.
  void addPoint(const StrokeSample& sample, float spacingPx, std::vector<StrokeDab>& out);

  // Call once at stroke end (pen/mouse up), before the next reset(). The
  // emitter always lags one real sample behind so every segment it walks is
  // bounded by real data on both sides (see the .cpp for why); that means
  // the very last segment of a stroke -- between the second-to-last and the
  // last sample -- never gets walked by addPoint() alone, because no further
  // sample ever arrives to confirm its shape. flush() walks that final
  // segment, extrapolating the one missing control point the same way the
  // *start* of a stroke already has to.
  //
  // **A stroke that never moved emits exactly one dab here, at its own
  // position** -- a single click paints. It has to happen at flush() and not
  // in addPoint(), because "never moved" is only knowable once the stroke is
  // over: an addPoint() that stamped on the spot would double up the moment
  // a second sample arrived and the arc-length walk laid its own first dab.
  // The dab is emitted regardless of `spacingPx`, since spacing divides a
  // distance and this stroke has none. See the .cpp for what counts as "never
  // moved" and why the threshold is float noise rather than a tremor budget.
  //
  // Safe to call on a stroke with 0 samples (does nothing) or more than once
  // (a no-op after the first).
  void flush(float spacingPx, std::vector<StrokeDab>& out);

  // --- back-compatible overloads: position only, neutral axes ------------
  //
  // Thin wrappers over the two above, for every caller that has no axes to
  // offer -- the solver route (`ui/MacPaintUI.cpp`'s `st.strokePath`,
  // deliberately out of this track's scope), `app/BrushSheet.cpp`'s preview
  // sweep, and every selftest that predates this file's axis-carrying form.
  // Each wraps its sample/dab in a default-constructed
  // `StrokeSample`/`StrokeDab` (neutral axes) and copies only `.pos` back
  // out, so the geometry these run through -- the curve fit, the arc-length
  // walk, `leftover_`, `movedPx_` -- is the IDENTICAL code the axis-carrying
  // overloads call, not a second implementation. Dab positions for a given
  // sample sequence are therefore bit-identical to what this class produced
  // before this axis-carrying form existed; `app/selftest/StrokePath.cpp`
  // (`runStrokePathTest()`) is the existing guard and it is unchanged.
  void addPoint(float x, float y, float spacingPx, std::vector<Vec2>& out);
  void flush(float spacingPx, std::vector<Vec2>& out);

 private:
  void emitAlongSegment(const StrokeSample& S0, const StrokeSample& S1, const StrokeSample& S2,
                        const StrokeSample& S3, float spacingPx, std::vector<StrokeDab>& out);

  StrokeSample pts_[4];
  int numPts_ = 0;
  // Arc length walked since the last emitted dab, carried across addPoint()
  // calls (and into flush()) so spacing never resets at a render-frame
  // boundary.
  float leftover_ = 0.0f;
  // Total straight-line distance between consecutive RAW samples of this
  // stroke -- not the walked curve length, which pts_[4] cannot remember past
  // four samples. Only ever compared against a float-noise threshold in
  // flush() to answer one yes/no question ("did this stroke move at all?"),
  // so the chord underestimate a curved path gives is irrelevant: a path with
  // any curvature to underestimate has already moved.
  float movedPx_ = 0.0f;
};

}  // namespace np
