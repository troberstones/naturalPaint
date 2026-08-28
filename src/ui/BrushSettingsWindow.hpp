#pragma once

#include <cstddef>
#include <cstdint>

// ui/BrushSettingsWindow -- **the brush settings as a window of their own**,
// one tab per group, opened from Window > Brush Settings.
//
// ==========================================================================
// 1. Why this is an ImGui window and not a second OS window
// ==========================================================================
//
// "A new window" has two readings, and this file takes the cheaper one on
// purpose. A genuine second `SDL_Window` -- which is what ImGui's
// `ImGuiConfigFlags_ViewportsEnable` would give, and the docking branch IS
// what `cmake/Dependencies.cmake` vendors -- is not a flag flip in this
// build:
//
//   * every viewport needs its own WebGPU surface and swapchain, and this
//     application's surface is **deliberately non-sRGB** (ui/CanvasQuad's
//     header on why, and on what silently goes wrong when a linear texture
//     is drawn through the wrong path). A second surface is a second place
//     to get that wrong, in a way that looks like a colour bug rather than a
//     windowing one.
//   * `ImGuiConfigFlags_NoMouseCursorChange` (main.cpp) makes ui/ToolCursor
//     the single owner of the pointer, and per-viewport cursor ownership is
//     a question that has an owner today precisely because there is one
//     window.
//   * the native menu bar (ui/MacNativeMenu) is built against one window's
//     key state.
//
// None of those is unsolvable and none is in scope here. A floating ImGui
// window is modeless, draggable, resizable, closable and stays in front of
// the canvas, which is what the request actually asks for; if it should later
// leave the frame, that is a viewport-enablement job with the three items
// above as its checklist, and this file's contents do not change.
//
// ==========================================================================
// 2. One implementation, two surfaces -- and why the bodies are NOT here
// ==========================================================================
//
// The docked BRUSH column and this window show the same controls. Written
// twice they would drift within a release, and the way a painter meets the
// drift is that the panel and the window disagree about their own brush --
// exactly the failure ui/MenuModel.hpp exists to prevent for menu actions.
//
// So the groups are drawn by the functions declared at the bottom of this
// file, `drawBrushSection()` calls them in column order, and this window
// calls them one per tab. Neither surface owns a control the other lacks --
// **for the ORIGINAL four groups.** The six Photoshop-shaped panels this
// window's tabs grew to (Shape Dynamics, Scattering, Dual Brush, Color
// Dynamics, Transfer, Tool Options) are window-only: `drawBrushSection()`
// still calls exactly the same five functions it always did, because the
// docked column is a fixed 322 px strip that was already competing a dozen
// sections for space (`drawBrushTextureGroup()`'s own comment) and growing
// it to eleven is a layout project of its own, not a consequence of giving
// `BrushModel`'s other panels a control surface at all. This is a deliberate
// asymmetry, stated here rather than left for the invariant above to imply
// it does not exist.
//
// **They are declared here and defined in `ui/MacPaintUI.cpp`, deliberately**
// -- the same trade `performMenuAction()` already makes, for the same reason
// and with the same limits. Each group reaches that file's own file-local
// helpers: `drawTestStroke()` (a GPU preview with its own cached textures),
// `ensureDabLibraryScanned()`, `revealDabFolder()`, `saveUserBrushLibrary()`,
// `ensureUserBrushLibraryLoaded()`, and the `ctlSlider()` family with its
// shared widest-label column. Hoisting five helpers and a label-layout global
// out of an 11,000-line file to satisfy a header would be a much larger and
// much riskier change than declaring six functions, and it would be a change
// to code this window does not otherwise touch.
//
// What this file owns is therefore the *window*: which groups exist, what
// they are called, what order they come in, and the frame around them.
//
// ==========================================================================
// 3. The tab list is data, because --selftest cannot open a window
// ==========================================================================
//
// docs/reachability-audit.md F4: `--selftest` cannot reach an ImGui dispatch
// site. A tab strip written as a run of `BeginTabItem()` calls is a tab strip
// with no assertions on it -- a group silently dropped in a later edit is
// invisible until a painter goes looking for a control that is no longer
// anywhere. The table below is a pure function of nothing, so the suite can
// assert that every enumerator has a row, that no two rows share a label, and
// that the count is what it should be. `drawBrushSettingsWindow()` walks the
// table; it invents no tab of its own.

namespace np {

// One tab per panel of `brush/BrushModel` -- Photoshop's own eight, in
// Photoshop's own order, plus the shelved link-matrix editor at the end.
//
// **`brush/BrushModel` IS authoritative over the painted stroke now** (Phase
// B/C, landed on this branch before this file's tabs grew to match): Shape
// Dynamics' Size/Angle/Roundness Variances, Scattering's jitter and Count,
// Transfer's Opacity/Flow, and Tool Options' blend-mode id each reach
// `app/StrokeSession` today, not just the importer. This enum used to say
// otherwise -- "not yet authoritative... a tab full of controls that read a
// struct nothing paints from" -- and that sentence became false out from
// under this file rather than being edited when it did; it is corrected
// here rather than left standing. (Texture, Dual Brush and Color Dynamics
// are carried and persisted but not yet read at paint time; each tab below
// says so where it matters, in the same disabled-with-a-reason idiom the
// rest of this codebase already uses rather than by staying unbuilt.)
//
// `ui/BrushFieldPresentation.hpp` is what makes growing this enum to eight
// (nine, with Dynamics) tabs safe against the failure the OLD comment above
// was guarding against: every one of `BrushModel`'s 151 leaves is either in
// that file's presentation table (a tab below draws it) or its omission
// table (a one-line reason it does not), and `--selftest`
// (`runBrushPanelBindingTest()`) asserts the two account for all 151 with no
// overlap and no stale entry -- a leaf now cannot go from "the importer
// fills it in" to "no control anywhere" without turning the suite red.
enum class BrushSettingsTab : uint8_t {
  // Radius, hardness, spacing, roundness, angle, and the dab grid.
  TipShape,

  // Size/Angle/Roundness jitter, tilt scale, flip jitter. Wired -- see the
  // header above.
  ShapeDynamics,

  // Scatter jitter, Both Axes, Count and its own jitter. Wired.
  Scattering,

  // Pattern, scale, depth, blend mode, brightness/contrast. **This is
  // `BrushModel::texture` (Photoshop's imported panel), a different struct
  // from the paper-tooth PAPER GRAIN section this tab also carries
  // (`st.brush.grain`, wired since before this phase) -- see
  // `drawBrushTextureGroup()`'s own comment on why one tab holds both.
  Texture,

  // A second tip, its own scatter, and how its coverage combines with the
  // primary tip's. Net new: no hand-written body existed for any of this
  // before (naturalPaint-ui-design-gaps' own finding -- "8 of 12 Runny
  // Inkers stamp a second tip we have no way to render").
  DualBrush,

  // Foreground/Background jitter, Hue/Saturation/Brightness jitter, Purity.
  // Shown and editable -- persists to the model and the saved preset -- but
  // carries a standing banner: `StrokeSession::brushTipFor()` passes the
  // HSV identity regardless of what is set here (its own comment, "future
  // work, not this commit's"), and only 1 of 101 presets measured uses this
  // panel at all.
  ColorDynamics,

  // Opacity/Flow jitter -- wired. (Wetness/Mix are on
  // `ui/BrushFieldPresentation`'s omission list: no engine target, per
  // `PsTransfer`'s own comment.)
  Transfer,

  // Blend mode id, opacity, flow, smoothing, the two pressure-override
  // flags, Use Legacy -- plus naturalPaint's `noise`/`wetEdges`/`airbrush`/
  // `brushPose` checkbox tail, which has no Photoshop-panel prefix of its
  // own and lands here for lack of a better one (see
  // `drawBrushToolOptionsGroup()`'s own comment). No `enabled` field on this
  // one struct, unlike the six panels above it -- Tool Options has no
  // off switch in Photoshop either.
  ToolOptions,

  // The shelved source->target link matrix and its editor
  // (`--advanced-dynamics`). Kept as a tab of its own, last, rather than
  // folded fully behind the flag with no slot at all: today it still shows a
  // live "N LINKS" count with the flag off, and a painter who has used it
  // before should still be able to find it. See `ui/DynamicsMatrixPanel.hpp`
  // for what would have to happen for it to drive a stroke again.
  Dynamics,

  Count,
};

// How many real tabs there are.
constexpr size_t kBrushSettingsTabCount = static_cast<size_t>(BrushSettingsTab::Count);

// The static half of one tab: what it is called and what it says about
// itself. Pure data, so `--selftest` can read every row without a window.
struct BrushSettingsTabSpec {
  BrushSettingsTab tab = BrushSettingsTab::TipShape;

  // Shown on the tab itself. Short, because a tab strip that wraps is a tab
  // strip whose last group is hard to find.
  const char* label = "";

  // Shown on hover. Says what the group is FOR, in the same voice the docked
  // column's captions use -- a tab called "Paint" is not self-explanatory and
  // the tooltip is the only place the window can say so.
  const char* tooltip = "";
};

// The row for one tab. Indexed by the enum; `--selftest` asserts that every
// row carries its own id, which is the failure that puts the Dynamics
// controls under the Texture tab and is invisible on inspection.
const BrushSettingsTabSpec& brushSettingsTabSpec(BrushSettingsTab tab) noexcept;

// The enumerator's own spelling, for `--selftest` output. Never shown to a
// user -- labels are data on the spec.
const char* brushSettingsTabName(BrushSettingsTab tab) noexcept;

// --- The groups ------------------------------------------------------------
//
// Defined in `ui/MacPaintUI.cpp` -- see §2. The first four draw exactly the
// controls the docked BRUSH column has always drawn for that group, in the
// same order, with the same captions and the same disabled-with-a-reason
// treatment. The six after them (`drawBrushShapeDynamicsGroup()` through
// `drawBrushToolOptionsGroup()`) are window-only -- §2's asymmetry -- and are
// each built on `ui/BrushFieldPresentation`'s table: an `Enabled` checkbox
// drawn by hand where the panel has one, then a generic walk of
// `visitBrushModelFields()` filtered to that panel's own path prefix, each
// leaf's control chosen by the table row it looks up.

struct GpuContext;
class MixboxLut;
struct AppState;

// The preset header: which brush this is, whether it has been edited, and
// Save / Save As New / Revert. **Not a tab.** It names and persists whatever
// the tabs are editing, so it belongs above them rather than inside one of
// them -- a Save button on a tab is a Save button that is invisible from the
// other three.
void drawBrushPresetHeader(AppState& st);

void drawBrushTipShapeGroup(AppState& st, GpuContext& gpu, const MixboxLut& lut);
void drawBrushPaintGroup(AppState& st);
// `ownPage` is true when this group is the whole of a tab and false when it is
// one section of the docked column, and it controls exactly one thing: whether
// the "PAPER GRAIN" collapsing header is drawn at all.
//
// **A tab whose entire content is a collapsed accordion reads as an empty
// tab**, which is how it first looked. In the column the header earns its
// place -- it is one of a dozen sections competing for a 322 px strip, and it
// is collapsed by default because grain is off by default and a section that
// opens on controls doing nothing reads as broken. On a page of its own
// neither argument survives: nothing is competing for the space, and the tab
// label already says what the page is, so the header would be a second title
// hiding the controls behind a click.
//
// **Also gates `BrushModel::texture`'s own 16 leaves** (`ui/
// BrushFieldPresentation`'s table), appended after PAPER GRAIN -- a
// DIFFERENT struct (`st.brush.grain`) that this same tab has always shown.
// Drawn only when `ownPage` for the identical column-space reason PAPER
// GRAIN's own header is: the docked column has no room for 16 more controls,
// so the generic section is window-only, same as the five tabs below.
void drawBrushTextureGroup(AppState& st, bool ownPage);
void drawBrushDynamicsGroup(AppState& st);

// The six window-only, presentation-table-driven groups -- see the comment
// above this block and each panel's own comment on `BrushSettingsTab`.
void drawBrushShapeDynamicsGroup(AppState& st);
void drawBrushScatteringGroup(AppState& st);
void drawBrushDualBrushGroup(AppState& st);
void drawBrushColorDynamicsGroup(AppState& st);
void drawBrushTransferGroup(AppState& st);
void drawBrushToolOptionsGroup(AppState& st);

// --- The window ------------------------------------------------------------

// Draws the window when `st.showBrushSettings` is set, and clears that flag
// when the user closes it -- so the Window menu's tick and the window's own
// close button are the same state and cannot disagree.
//
// Call once per frame, outside any other window, beside `ShowDemoWindow()`.
void drawBrushSettingsWindow(AppState& st, GpuContext& gpu, const MixboxLut& lut);

}  // namespace np
