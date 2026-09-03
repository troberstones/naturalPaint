#pragma once

#include <array>
#include <cstddef>

#include "ops/Gradient.hpp"

namespace np {

// ===========================================================================
// app/GradientTool -- the gradient TOOL's settings, and the one place its
// ramp and its geometry are built.
// ===========================================================================
//
// `ops/Gradient` is the renderer: give it a geometry, a stop list and a
// region and it writes texels. It has no opinion about where any of those
// came from, which is right for an op and useless for a tool -- the tool has
// to answer "which ramp, aimed how" every frame of a drag AND once more at
// pen-up, and it has to answer identically both times.
//
// § 1. Why this file exists at all: one answer, not two agreeing ones
// -------------------------------------------------------------------------
// The gradient tool draws its ramp in three places:
//
//   (a) the options bar swatch, so the user can see what they are about to
//       pull before they pull it;
//   (b) the live drag preview composited over the canvas;
//   (c) the pixels `renderGradient()` actually writes at pen-up.
//
// Before this file, (c) built its stops inline at the call site and (a) and
// (b) did not exist. Adding them by writing the same six lines twice more
// would have made the swatch a PICTURE of the gradient rather than the
// gradient: the two would agree until the day one of them changed, and the
// failure mode is a preview that lies -- the single worst thing a preview can
// do, because it is trusted precisely when the user cannot check it.
//
// This is the argument `app/FilterOps.cpp` makes for `computePixelFilter()`
// being one function shared by preview and commit rather than two functions
// that currently match. Same defect class, same fix: the three readers above
// call `gradientToolStops()` and `gradientToolGeometry()`, and a change to
// the ramp is a change to one function body.
//
// § 2. What is NOT here
// -------------------------------------------------------------------------
// No colour lookup. `foregroundLinearRgba()` lives in `ui/MacPaintUI.hpp`
// because the foreground is a UI concept (it depends on whether the colour
// panel is in RGB or pigment mode), and `app/` does not depend on `ui/`. So
// the foreground arrives here as a parameter. `ui/MacPaintUI.hpp`'s
// `currentGradientStops(const BrushState&)` is the one-line adapter that
// pairs the two, and it is what all three readers above actually call --
// so § 1's "one function" survives the layering split rather than being
// quietly reintroduced as two.

// ---------------------------------------------------------------------------
// § 3. The tool's own settings
// ---------------------------------------------------------------------------
//
// One field today. It is a struct rather than a bare `GradientSpread` on
// `AppState` for the reason `EyedropperState` is one: a tool's settings are a
// group, and the second setting (PRD's radial/angular kinds, a reverse
// toggle, a dither flag) should widen a struct that already exists rather
// than scatter a second loose field beside the first.
struct GradientToolState {
  // Which function from a document position to the ramp parameter -- the
  // shape of the gradient. `ops/Gradient` has implemented all three since it
  // was written; until 2026-09-02 the tool hard-coded `Linear` and there was
  // no way to reach the other two, which is the plain kind of gap this
  // field closes: engine capability with no control.
  //
  // Linear is the default because it is the one a drag most obviously means,
  // and because it is what every gradient tool opens on.
  GradientKind kind = GradientKind::Linear;

  // What happens OUTSIDE the drag -- behind the start handle and past the end
  // one. `Pad` (the ramp's end colours, held) is `ops/Gradient`'s own
  // documented default and stays this build's, because it is the only one of
  // the three that cannot surprise: a user who drags a short gradient across
  // a big canvas and has not thought about spread means "fill the rest with
  // the ends", never "tile my ramp forty times".
  GradientSpread spread = GradientSpread::Pad;
};

// ---------------------------------------------------------------------------
// § 3a. The gesture, and why it is not `marqueeDragging`
// ---------------------------------------------------------------------------
//
// **The gradient tool used to borrow `AppState::marqueeDragging` and the four
// `marqueeX0..Y1` handles, and that is the whole reason the tool did
// nothing.** Not a figure of speech: the selection-tool switch in
// `ui/MacPaintUI.cpp` ends in an `else` arm that clears `marqueeDragging` on
// every frame a NON-selection tool is active -- so it exists to cancel a
// selection drag abandoned by a tool change, and it did that correctly. The
// gradient is not a selection tool, so it took that arm every frame: a
// gradient drag was set on the frame of pen-down and wiped at the top of the
// very next frame, and the pen-up commit could never run. The tool had a
// route, an implemented flag, a passing `toolHasCanvasHandler()` and a green
// suite, and drew nothing on a canvas -- which is the reachability defect
// class `docs/reachability-audit.md` is about, arriving through a shared
// mutable flag rather than through a hand-written list.
//
// The same sharing produced a second, independent defect at the other end of
// the frame: the rubber-band draw's `else if (marqueeDragging || ...)` arm
// treated any non-marquee drag as a lasso, so a gradient drag drew the stale
// outline of whatever lasso had been drawn last.
//
// Two unrelated bugs, one cause, one fix: the gesture gets its own state.
// Nothing else in the build writes these fields, so nothing else can clear
// them, and `marqueeX0..Y1`'s own documented claim -- "only ever written by
// the two marquee tools" -- becomes true again.
struct GradientDrag {
  bool active = false;
  // Document texels. Pen-down is t=0 and the live pointer is t=1 (§ 6).
  float x0 = 0.0f, y0 = 0.0f;
  float x1 = 0.0f, y1 = 0.0f;
};

// ---------------------------------------------------------------------------
// § 4. The vocabulary seam
// ---------------------------------------------------------------------------
//
// The engine's word for "hold the end colours" is `Pad`; the word the options
// bar shows is CLAMP. Two vocabularies for one concept is a real cost, and it
// is paid deliberately: `Pad` is what CSS, SVG and every gradient renderer
// this build could be compared against call it, and renaming the enum would
// make `ops/Gradient` read unlike its own reference material -- while CLAMP
// is what a painter reading a toolbar expects, and is the word this tool was
// specified in.
//
// The cost is contained by there being exactly ONE table where the two meet.
// A label added here without a spread, or a spread added to `ops/Gradient`
// without a row here, is caught by `--selftest` rather than by a combo that
// silently offers two of three modes -- which is the `kToolMeta` failure
// (`ui/AtelierChrome.cpp`) in miniature, and that one shipped.
struct GradientSpreadRow {
  GradientSpread spread;
  const char* label;
  const char* tip;
};
inline constexpr size_t kGradientSpreadCount = 3;
extern const GradientSpreadRow kGradientSpreads[kGradientSpreadCount];

// The kinds get the same treatment for the same reason. Here the two
// vocabularies happen to agree -- Linear, Radial and Angular are what the
// enum calls them and what a painter would call them -- so the table exists
// for the OTHER half of § 4's argument: it is the one list the combo walks,
// so a kind added to `ops/Gradient` and not to this table is caught by
// `--selftest` rather than by a picker that silently offers three of four.
struct GradientKindRow {
  GradientKind kind;
  const char* label;
  const char* tip;
};
inline constexpr size_t kGradientKindCount = 3;
extern const GradientKindRow kGradientKinds[kGradientKindCount];

const char* gradientKindLabel(GradientKind kind);

// **Whether SPREAD means anything for this kind.** False for `Angular` only:
// that kind wraps its parameter into [0, 1) by construction, so there is
// nothing outside the range for a spread mode to pad, tile or mirror.
//
// Precisely: on [0, 1) all three modes are the identity -- `Pad` clamps
// nothing, `Repeat`'s `t - floor(t)` is `t`, and `Reflect`'s triangle wave
// has not turned yet. `gradientParameterAt()` also returns before reaching
// the spread switch, but that early return is not what makes this true; it
// pins the one boundary where the modes would disagree (a `t` that rounds to
// exactly 1.0, where `Repeat` answers 0 and the other two answer 1).
// Recorded because a sabotage removing the early return changed no rendered
// texel, and it would be easy to read that as the guard being pointless.
//
// This exists as a function rather than as an `if (kind == Angular)` in the
// options bar because that would be the chrome RESTATING a fact about the op
// -- and a restatement is a second copy that can fall out of step with the
// first. `--selftest` asserts the two agree by rendering: for `Angular`, all
// three spreads must produce identical pixels. The options bar draws the
// SPREAD combo disabled, with the reason, on the kinds this answers false
// for; `docs/ui.md`'s rule is that no dead control looks live.
bool gradientKindUsesSpread(GradientKind kind);

// The label for a spread, by lookup in the table above rather than by a
// second switch. Returns "Clamp" for anything unrecognised -- unreachable
// while the static_assert in the .cpp holds, and the safe answer if it ever
// stops holding, because Pad is the default the rest of the file assumes.
const char* gradientSpreadLabel(GradientSpread spread);

// ---------------------------------------------------------------------------
// § 5. The ramp
// ---------------------------------------------------------------------------
//
// **Foreground to transparent**, which is the only default this build can
// honestly offer: `docs/ui.md` deliberately has no BG half to the swatch
// (nothing fills with a background colour until PRD D25/D26), so "foreground
// to background" would name a colour that does not exist.
//
// The colour stops hold ONE colour at both ends and the OPACITY stops do the
// fading -- which is exactly why `ops/Gradient` keeps the two lists
// independent, and is what stops the ramp darkening toward a transparent
// black that was never a stop. Getting this wrong is invisible on a white
// canvas and obvious on a dark one, which is the kind of bug that ships.
//
// `foregroundLinear` is STRAIGHT scene-linear RGBA, as `ColorStop` wants
// (`ops/Gradient.hpp` § 2). Its alpha is ignored: the opacity stops below own
// the ramp's alpha entirely, and letting a foreground alpha multiply into
// them would mean the swatch and the canvas disagreed the moment the colour
// panel grew an alpha slider.
GradientStops gradientToolStops(const std::array<float, 4>& foregroundLinear);

// ---------------------------------------------------------------------------
// § 6. The aim
// ---------------------------------------------------------------------------
//
// The drag's two handles, in document texels: pen-down is t=0 and pen-up is
// t=1, which is the mapping the tool was specified with and the one
// `ops/Gradient` already takes.
//
// This is three assignments and a copy of one field, and it is a function
// anyway for § 1's reason: the preview builds a geometry every frame and the
// commit builds one at pen-up, and the field that is easy to forget in the
// second copy is `spread` -- the one whose omission is invisible until
// someone picks a non-default mode and watches the preview stop matching the
// result. A forgotten field in one of two hand-written copies is not a
// hypothetical here; `ui/MacPaintUI.cpp`'s gradient block was written that
// way and hard-coded `GradientKind::Linear` with no way to reach the other
// two kinds at all.
GradientGeometry gradientToolGeometry(const GradientToolState& tool, float x0, float y0,
                                      float x1, float y1);

// ---------------------------------------------------------------------------
// § 7. When a drag is a gradient
// ---------------------------------------------------------------------------
//
// A click with no drag has no direction, and a zero-length gradient is not a
// fill -- it is an undefined ramp. Ignored rather than guessed at.
//
// Shared by the preview and the commit **because the alternative is a preview
// that shows a ramp the commit then refuses**: two thresholds that both say
// "about a texel" differ somewhere, and the somewhere is a drag that painted
// itself onto the canvas and then vanished at pen-up with nothing said. One
// threshold cannot have that gap.
//
// One texel squared, so the comparison is exact in float and needs no sqrt.
bool gradientDragIsUsable(float x0, float y0, float x1, float y1);

}  // namespace np
