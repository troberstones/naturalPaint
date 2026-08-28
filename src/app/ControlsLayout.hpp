#pragma once

#include <cstddef>
#include <vector>

// app/ControlsLayout (UI detour step 3, problems 1 and 1b; docs/ui.md §2's
// right-hand stack, "a docked column of collapsing headers").
//
// The two decisions the right-hand controls column makes that can be wrong
// without a screenshot showing it, and therefore the two `--selftest` can
// check: **which section comes first and which of them start open**, and
// **how much room a labelled control gives its label**. The ImGui chrome --
// `CollapsingHeader`, the sliders, the tooltips -- stays in ui/MacPaintUI.cpp,
// the same split app/LayerPanel and app/CurveEdit already document.
//
// --- 1. The order, and the default open set -------------------------------
//
// Until this step the column was one unbroken scroll in this order: BRUSH,
// PIGMENT, WATER/OIL/INK, BOARD TILT, GRID, SOLVER, LAYERS, HISTORY, GRADE.
// Six simulation-parameter sections sat above everything Phase 5 built, so at
// the default window size a screenshot of the running application contained no
// LAYERS panel and no HISTORY panel at all -- the document was on the canvas
// (UI detour step 2) and the only controls that edit it were below the fold.
//
// The rule this file encodes, and the one the ordering test asserts:
//
//   **A section that describes the open document comes before a section that
//   tunes the solver, and the document sections are the only ones that start
//   open.**
//
// The asymmetry is deliberate and is not "layers matter more than paint". A
// simulation section is a *tuning* surface: a handful of numbers that are set
// once for a medium and then left, and whose effect is judged by painting, not
// by reading the panel. LAYERS and HISTORY are the opposite -- they are the
// document's own structure, they change constantly while working, and every
// one of their controls acts on what is on the canvas right now. Being open by
// default is worth a column of screen space to the second kind and not to the
// first. Anything a user does open stays open: ImGui persists a header's state
// for the session, so this is a starting point, not a policy the panel keeps
// re-imposing.
//
// GRADE sits between the two groups because it is neither: it previews the
// canvas through a session-level op stack, which is a view state rather than
// part of the document (the per-layer op stacks in LAYERS are the document's).
// It starts closed, because a closed grading section is exactly what "grading
// never turns on just because the stack below is non-empty" looks like in the
// column.
//
// --- 2. The label column --------------------------------------------------
//
// Dear ImGui puts a widget's label to the *right* of the widget, and the
// controls column is a fixed-width docked panel, so any label longer than the
// gap between the slider's right edge and the panel's is silently clipped by
// the window. Before this step the column read "Granulatio", "Edge darke",
// "Paper slop" and "Working ti" -- four controls whose names were cut off in
// the middle of a word.
//
// The fix is to stop letting the label take whatever is left: every labelled
// control in the column draws its label first, at the left, and the widget
// fills the remainder. `layoutLabelledControl()` below is that arithmetic, and
// it holds one invariant, which is the thing worth testing:
//
//   **The widget never starts before the label ends.** The column only ever
//   grows, and it grows *within the frame that measures a wider label*, so a
//   label added tomorrow cannot clip even on the first frame it is drawn.
//
// The cost of that rule is that the frame which first sees a wider label draws
// the controls above it against the narrower column -- one frame of
// misalignment, never a truncated word. The alternative (measure everything,
// then draw) would need the whole label set as data in this file, which is the
// same string written twice and the exact way a renamed slider goes back to
// clipping.
//
// The floor is the other half: when the panel is so narrow that the remainder
// would be unusable, the label goes on its own line and the widget takes the
// full width. That is the one case where a label is *not* in the column, and
// it is still never clipped.
namespace np {

// The sections of the right-hand controls column, in no particular order --
// `controlsSections()` below owns the order. One enumerator per section that
// has a collapsing header; a section drawn conditionally on the paint mode
// (OIL / INK / WATER) is one enumerator, `Medium`, because exactly one of the
// three is ever on screen and they occupy one slot in the column.
enum class ControlsSection {
  // **The tool palette and the tool options bar are panels now.** Both were
  // welded bands in the chrome -- a 52 px column on the left edge and a 46 px
  // strip under the tab strip -- until the user's instruction: *"move the
  // brush setting and the tool pallet to dockable panels as well, this makes
  // the UI modular and customizable."* They are `ControlsSection`s so that
  // one model, one persistence format, one PANELS menu and one set of docks
  // covers every panel in the application, rather than covering eleven of
  // them and leaving two special cases welded to the window.
  //
  // Both carry the `Tool` role and lead the list, which is not a new rule but
  // the existing one applied honestly: `Tool` is "what the next stroke will
  // be", and a tool palette is the most literal possible instance of that.
  //
  // They differ from every other section in one way worth naming here,
  // because it is the thing a reader will wonder about: **neither draws a
  // collapsing header of its own by default in a horizontal dock**, because
  // an options bar with a 26 px header above it in a 46 px band leaves 20 px
  // for the controls. See ui/MacPaintUI.cpp's panel frame for how a panel
  // that is the only occupant of a thin dock is drawn headerless.
  Tools,
  Options,
  // docs/ui.md section 3.3 and PRD **L4** (P0): "The colour panel has RGB and
  // PIGMENT modes; PIGMENT selects physical constants, not just a colour."
  // First of the docked sections because the design's own diagram puts COLOR
  // first -- see `ControlsSectionRole::Tool` below for why that outranks the
  // document-sections-lead rule this list used to open with.
  Color,
  Layers,
  History,
  // PLAN.md Phase 5 step 12 / PRD C14. A `Document` section like the two above
  // it and for the same reason: a comp is part of the open document (it is
  // persisted in the file, core/Document.hpp), it changes while working, and
  // every one of its controls acts on what is on the canvas right now. Third
  // rather than second because a comp is a saved state *of* the layer stack and
  // a history row is an edit *to* it, so LAYERS reads before either.
  Comps,
  Grade,
  // C2 (docs/reachability-audit.md; PRD D2, P0): a per-channel/luminance
  // distribution of the open document's composite, read from core/Histogram
  // .hpp's `computeHistogram()` -- a built and tested engine that had no UI
  // caller at all before this section. A `View` role like GRADE just above
  // it and for the same reason: what it draws is a fresh read of the canvas,
  // never anything `Document` persists, so it earns no place among the
  // document sections above.
  Histogram,
  // The brush library (brush/Library.hpp) -- which brush, as opposed to what
  // that brush is. Immediately above BRUSH, because picking is what you do
  // before editing and the editor's header names what the pane above chose.
  BrushLibrary,
  Brush,
  Pigment,
  Medium,
  BoardTilt,
  Grid,
  Solver,
};

// What a section is *about*, which is what decides where it sits and whether
// it starts open. See this header's section 1.
enum class ControlsSectionRole {
  // What the *next stroke* will be: the colour it lays down and the brush that
  // lays it. Ahead of the document sections, which is a reversal of the
  // ordering this file shipped with -- and it is docs/ui.md section 2's
  // ordering, not a preference. The diagram's right-hand column reads COLOR /
  // BRUSH SET. / LAYERS / CHANNELS, and the reason is the same one that put
  // LAYERS above the simulation parameters in the first place: a control
  // touched every stroke outranks one touched every few minutes. A layer is
  // selected occasionally; a colour is chosen constantly.
  Tool,
  // The open document itself: its layer stack and its undo history. These edit
  // what is on the canvas.
  Document,
  // A view of the canvas that is not part of the document.
  View,
  // Parameters of the paint simulation and of the canvas furniture. Set
  // occasionally, judged by painting rather than by reading.
  Simulation,
};

struct ControlsSectionSpec {
  ControlsSection section;
  ControlsSectionRole role;
  // The header text, in the column's existing all-caps idiom.
  const char* title;
  // Whether the header starts open in a fresh session.
  bool defaultOpen;
};

// Every section, in the order the column draws them. The list is data rather
// than a sequence of calls in the draw function precisely so the ordering rule
// above can be asserted headlessly -- a reordering that buried LAYERS again
// would fail `--selftest` rather than only being visible in a screenshot.
const std::vector<ControlsSectionSpec>& controlsSections();

// The spec for one section. Every enumerator has exactly one, which the test
// asserts; the returned reference is to a static list and outlives any caller.
const ControlsSectionSpec& controlsSectionSpec(ControlsSection section);

// --- The label column -----------------------------------------------------

// The gap between the end of a label and the start of its widget, px.
inline constexpr float kControlsLabelGapPx = 10.0f;

// The narrowest a labelled widget is allowed to get before its label is moved
// to its own line instead. A slider narrower than this is not a control, it is
// a decoration -- roughly 90 px is where a 0..1 slider still resolves to about
// a hundred distinct positions under the mouse.
inline constexpr float kControlsMinWidgetPx = 90.0f;

// Where one labelled control's two halves go.
struct LabelledControlLayout {
  // The label is drawn on the line above the widget rather than beside it, and
  // `labelColumn` is not used. Happens only when the panel is too narrow for
  // both.
  bool labelOnOwnLine = false;
  // The x offset, from the start of the content area, at which the widget
  // begins. Always at least `labelPx + kControlsLabelGapPx`.
  float labelColumn = 0.0f;
  // The width to give the widget.
  float widgetWidth = 0.0f;
};

// Lays out one labelled control, widening `column` in place if this label
// needs more room than any label before it.
//
// `labelPx` is the measured width of the label text and `availPx` the content
// width the panel has left. `column` is the caller's running label-column
// width for the panel; it only ever grows, so the widths are stable after the
// first frame that has drawn every control once.
LabelledControlLayout layoutLabelledControl(float& column, float labelPx, float availPx);

// How far one mouse-wheel unit scrolls the right-hand controls column.
//
// Dear ImGui's own answer is `5 * fontSize` -- five lines, 65 px at this
// build's 13 px UI font -- and it is the right answer for a window of prose.
// It is the wrong one here. The controls column is a stack of collapsing
// SECTIONS (`controlsSections()`), so the unit a user navigates it in is a
// section, not a line: at 65 px a notch, crossing a fully expanded column
// takes dozens of them, which is the "slow scrolling" this function exists to
// fix.
//
// A quarter of the visible height per notch, which is the settings-panel
// convention, and bounded at both ends rather than left to run free:
//
//  - **Never slower than ImGui's own step.** On a short column a quarter page
//    can be less than five lines, and scrolling must not get *worse* than the
//    default it replaces.
//  - **Never more than two thirds of the view**, which is ImGui's own
//    `max_step` rule (imgui.cpp's mouse-wheel block) reused rather than
//    reinvented. A notch that scrolls past a whole screenful loses the
//    reader's place completely.
//
// Proportional rather than a fixed pixel count deliberately: the column's
// height is the window's, and a constant tuned on a tall display would be a
// page and a half on a short one.
float controlsWheelScrollStep(float innerHeightPx, float fontSizePx) noexcept;

}  // namespace np
