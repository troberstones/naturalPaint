#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

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
//
// § 2a. The AUTHORED ramp is not `ops/Gradient::GradientStops`
// -------------------------------------------------------------------------
// PRD D24's editor lets a colour stop be a fixed colour OR "Foreground",
// tracking the swatch at the moment the gradient is drawn -- that is the only
// way today's default (§ 5) stays expressible once a second ramp exists to
// choose instead of it. `ops/Gradient::ColorStop` has no room for that: its
// `color` is a concrete linear RGB triple, on purpose (`ops/Gradient.hpp` § 2
// -- the op is headless and has never heard of a foreground swatch). So the
// authored form is a parallel, tool-level struct that CAN say "follow the
// foreground", and `resolveGradientPresetStops()` below is the one place that
// turns one of those into the concrete `GradientStops` the op actually reads
// -- the same "one function" shape as § 1, one level up: an editor, a JSON
// file and the preview all hold a `GradientPresetStops`, and only the final
// hop to pixels ever substitutes a colour.
struct GradientColorStopSpec {
  float position = 0.0f;
  // When true, `color` is ignored and the resolved stop takes whatever
  // `resolveGradientPresetStops()` is handed as the foreground -- so a preset
  // saved with a Foreground stop still tracks the swatch after a save/load
  // round trip, exactly as today's built-in default does.
  bool foreground = false;
  std::array<float, 3> color{0.0f, 0.0f, 0.0f};
  float midpoint = 0.5f;
};

// No `foreground` flag here: an opacity value has no swatch to follow, so
// every opacity stop is already fully concrete.
struct GradientOpacityStopSpec {
  float position = 0.0f;
  float opacity = 1.0f;
  float midpoint = 0.5f;
};

// The authored ramp: what the stop editor edits, what a preset file holds,
// and what `GradientToolState::customStops` below stores. Mirrors
// `ops/Gradient::GradientStops`'s two-list shape (§ 1 of that header) for the
// same reason -- colour and opacity are positioned independently -- with
// colour stops widened to `GradientColorStopSpec`.
struct GradientPresetStops {
  std::vector<GradientColorStopSpec> colorStops;
  std::vector<GradientOpacityStopSpec> opacityStops;
};

// Turns an authored ramp into the concrete stops `renderGradient()` (and the
// swatch, and the live preview) actually read, substituting `foregroundLinear`
// for every colour stop marked `foreground`. Position and midpoint pass
// through unchanged; `ops/Gradient.hpp`'s sorted-ascending contract is the
// caller's to keep here exactly as it is everywhere else in this build (an
// editor sorts after a drag with `sortGradientStops()`; this function does not
// sort defensively, for the reason `ops/Gradient.hpp` gives at its own stop
// lists).
GradientStops resolveGradientPresetStops(const GradientPresetStops& spec,
                                         const std::array<float, 3>& foregroundLinear);

// ---------------------------------------------------------------------------
// § 2b. Editor operations -- PRD D24's stop editor, headless
// ---------------------------------------------------------------------------
//
// Every mutation the stop editor's UI performs is a pure function here, so
// `--selftest` exercises "add keeps stops sorted", "delete refuses below
// two", "move clamps to [0,1]" and "midpoint clamps" without a frame of
// ImGui -- `ui/MacPaintUI.cpp`'s dialog is a thin caller of these, per this
// codebase's own rule for where logic goes (put it in `app/`, keep the UI
// part thin).

// Clamps a stop's authored POSITION to the strip's own range. `ops/Gradient`
// itself does not require this -- `Repeat`/`Reflect` are defined for any
// real `t` (`ops/Gradient.hpp`'s own note on `ColorStop::position`) -- but
// the editor's strip only spans [0, 1], so a stop dragged past either end is
// held at it rather than dragged off the strip where nothing could select it
// again.
float clampGradientStopPosition(float t) noexcept;

// `applyMidpointSkew()`'s own clamp band (`ops/Gradient.cpp`), exposed here
// so the editor's midpoint control and the renderer agree on the same number
// rather than the editor inventing a second one that could drift from it.
float clampGradientStopMidpoint(float m) noexcept;

// Inserts a colour stop and re-sorts, so the caller's contract
// (`ops/Gradient.hpp`: "both lists must be sorted ascending") holds
// immediately rather than until the next edit.
void addGradientColorStop(GradientPresetStops& stops, float position, bool foreground,
                          const std::array<float, 3>& color, float midpoint = 0.5f);
void addGradientOpacityStop(GradientPresetStops& stops, float position, float opacity,
                            float midpoint = 0.5f);

// Removes the stop at `index`. Refuses -- leaving `stops` untouched and
// returning false -- when that would drop the list below two: a one-stop
// ramp has no span to interpolate across, and `GradientStops`'s own header
// makes the empty cases load-bearing (zero colour stops renders NOTHING,
// zero opacity stops is FULLY OPAQUE) rather than "an empty ramp", so two is
// the floor a ramp needs to mean anything at all.
bool removeGradientColorStop(GradientPresetStops& stops, size_t index);
bool removeGradientOpacityStop(GradientPresetStops& stops, size_t index);

// Stable-sorts both lists ascending by position -- `sortGradientStops()`'s
// contract (`ops/Gradient.hpp`), on the authored form. The editor calls this
// after every drag, exactly as `drawGradientMapDialog()` already does on its
// own, unwidened stop list.
void sortGradientPresetStops(GradientPresetStops& stops);

// Moves the stop at `index` to `position` (clamped to [0,1] by
// `clampGradientStopPosition()`), keeping the list sorted ascending --
// `movePoint()`'s own erase-then-reinsert shape (`app/CurveEdit.cpp`), one
// dimension over: a stop's only free coordinate is its position, so there is
// no second axis to carry through the splice. Returns the stop's index AFTER
// the move, which can differ from `index` when the move crossed a neighbour.
// `index` must be < the list's size (bounds-checked via `std::vector::at`,
// throws `std::out_of_range` on misuse, the same discipline `app/CurveEdit`
// holds its callers to).
size_t moveGradientColorStop(GradientPresetStops& stops, size_t index, float position);
size_t moveGradientOpacityStop(GradientPresetStops& stops, size_t index, float position);

// Nearest stop in `positions` to `t`, within `hitRadius` (inclusive) -- the
// hit-test the editor's drag/delete gestures start from, `hitTestPoint()`'s
// own contract (`app/CurveEdit.hpp`) on a 1-D strip instead of a 2-D plot.
// nullopt when nothing in `positions` (including an empty list) is within
// range. On a tie, the smaller index wins (a strict `<`, never `<=`, when
// replacing the running best) -- deterministic rather than
// iteration-order-dependent by accident. Takes a plain position list rather
// than a `GradientPresetStops` so the editor's colour row and opacity row
// share the one function despite hit-testing two different-typed lists.
std::optional<size_t> hitTestGradientStop(const std::vector<float>& positions, float t,
                                          float hitRadius) noexcept;

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

  // The chosen ramp, PRD D24's editor and presets. `hasCustomStops == false`
  // is "use the built-in Foreground-to-Transparent ramp" -- § 5 below -- and
  // is the only state that guarantees § 5's bit-identical promise, because it
  // takes the untouched original code path rather than a resolved copy of it.
  // `customStops` is otherwise ignored, so switching this flag off and back on
  // (picking the built-in preset, then re-picking a saved one) cannot lose the
  // edit sitting in it.
  bool hasCustomStops = false;
  GradientPresetStops customStops;

  // The name of the preset `customStops` was loaded from or last saved as,
  // empty for an edited-but-unsaved ramp or the built-in default. Purely a
  // label for the options-bar picker -- nothing downstream reads it, which is
  // § 5a's whole point: a renamed or deleted preset file cannot desync the
  // ramp actually in `customStops`.
  std::string presetName;
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
// **Foreground to transparent** is the BUILT-IN default, and used to be the
// only ramp this build could honestly offer: `docs/ui.md` deliberately has no
// BG half to the swatch (nothing fills with a background colour until PRD
// D25/D26), so "foreground to background" would still name a colour that
// does not exist. PRD D24's stop editor and presets (§ 2a, § 5a) are what let
// a user reach any OTHER ramp; this function's own default path is
// unchanged, which is what makes it the one thing every custom ramp is
// judged against rather than a second guess that happens to agree today.
//
// The default's colour stops hold ONE colour at both ends and the OPACITY
// stops do the fading -- which is exactly why `ops/Gradient` keeps the two
// lists independent, and is what stops the ramp darkening toward a
// transparent black that was never a stop. Getting this wrong is invisible on
// a white canvas and obvious on a dark one, which is the kind of bug that
// ships.
//
// `foregroundLinear` is STRAIGHT scene-linear RGBA, as `ColorStop` wants
// (`ops/Gradient.hpp` § 2). Its alpha is ignored: the opacity stops below own
// the ramp's alpha entirely, and letting a foreground alpha multiply into
// them would mean the swatch and the canvas disagreed the moment the colour
// panel grew an alpha slider.
//
// `custom`, when non-null, names the ramp instead: the built-in branch below
// is skipped ENTIRELY and the result is
// `resolveGradientPresetStops(*custom, {foregroundLinear[0..2]})`. That
// branch split -- rather than always resolving a `GradientPresetStops` that
// happens to equal the default when no preset is chosen -- is what makes
// "no custom gradient chosen is bit-identical to today's output" a fact about
// which CODE ran rather than a claim that two paths agree; `--selftest`
// checks the fact, not the claim.
GradientStops gradientToolStops(const std::array<float, 4>& foregroundLinear,
                                const GradientPresetStops* custom = nullptr);

// ---------------------------------------------------------------------------
// § 5a. Built-in presets
// ---------------------------------------------------------------------------
//
// PRD D24's picker offers these beside whatever `io/GradientPresetFile`'s
// library holds, and they are never written to that library: a fresh install
// has no `gradients/` directory at all, and a picker with nothing in it on
// first launch is the "engine capability with no control" gap this whole file
// exists to close (§ 3's comment on `GradientToolState::kind`, repeated one
// level up). "Foreground to Transparent" is § 5's own default, expressed as
// data so the picker's first row and `gradientToolStops(fg, nullptr)`'s
// hard-coded path describe the same ramp -- checked by `--selftest`, which is
// the same discipline `kGradientKinds`/`kGradientSpreads` (§ 4) already keep.
struct GradientBuiltInPreset {
  const char* name;
  GradientPresetStops stops;
};

// A function rather than a `static const` table: `GradientPresetStops` holds
// `std::vector`s, and a function returning a fresh one each call needs no
// static-initialisation-order reasoning at all, at the cost of an allocation
// no caller here makes more than once a frame.
std::vector<GradientBuiltInPreset> builtInGradientPresets();

// The label the picker shows for "no custom gradient chosen" -- the first
// entry `builtInGradientPresets()` returns, named so a caller does not have
// to know it is index 0.
const char* defaultGradientPresetName();

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
