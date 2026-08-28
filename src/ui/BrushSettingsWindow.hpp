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
// calls them one per tab. Neither surface owns a control the other lacks.
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

// One tab per group of brush settings.
//
// **These are naturalPaint's groups as they exist today, not Photoshop's
// panel list.** The ABR work (docs and the branch plan) is heading toward
// Brush Tip Shape / Shape Dynamics / Scattering / Texture / Dual Brush /
// Colour Dynamics / Transfer, and every one of those is a tab this enum can
// grow. It has not grown them yet because `brush/BrushModel` is populated by
// the importer and not yet authoritative over the painted stroke -- a tab
// full of controls that read a struct nothing paints from would be the
// "built and wired to nothing" defect the reachability audit exists to catch,
// dressed up as progress.
enum class BrushSettingsTab : uint8_t {
  // Radius, hardness, spacing, roundness, angle, and the dab grid. Photoshop
  // calls this Brush Tip Shape and it is the same set.
  TipShape,

  // Load, water and opacity -- what the stroke carries and how much of it
  // lands. Named for what it is rather than "Transfer": Photoshop's Transfer
  // panel is flow and opacity jitter, which this build reads from a `.abr`
  // but does not yet paint from, and a tab named after a panel it only half
  // implements would claim more than it does.
  Paint,

  // Paper grain. Named as Photoshop names it, because after the texture work
  // it IS the same feature: a scanned height field under the coverage.
  Texture,

  // The source->target link matrix and its editor.
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
// Defined in `ui/MacPaintUI.cpp` -- see §2. Each draws exactly the controls
// the docked BRUSH column has always drawn for that group, in the same order,
// with the same captions and the same disabled-with-a-reason treatment.

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
// This is the only group that needs the distinction. The other three open on
// content already.
void drawBrushTextureGroup(AppState& st, bool ownPage);
void drawBrushDynamicsGroup(AppState& st);

// --- The window ------------------------------------------------------------

// Draws the window when `st.showBrushSettings` is set, and clears that flag
// when the user closes it -- so the Window menu's tick and the window's own
// close button are the same state and cannot disagree.
//
// Call once per frame, outside any other window, beside `ShowDemoWindow()`.
void drawBrushSettingsWindow(AppState& st, GpuContext& gpu, const MixboxLut& lut);

}  // namespace np
