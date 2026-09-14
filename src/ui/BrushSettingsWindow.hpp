#pragma once

// ui/BrushSettingsWindow -- Window > Brush Settings, laid out like Photoshop's
// Brush Settings panel.
//
// A list of panels down the left, each with its own switch; the selected
// panel's page to its right, in Photoshop's order and units; the test stroke
// pinned along the bottom, so every edit is judged against the same picture.
// What the list and the pages hold is data in ui/BrushPanelLayout, which is
// what the selftests read -- `--selftest` cannot open a window.
//
// It is an ImGui window rather than a second OS window. A second SDL window
// would need its own non-sRGB WebGPU surface (ui/CanvasQuad on why that is
// easy to get wrong), a second owner for the pointer (ui/ToolCursor) and a
// second set of key state for ui/MacNativeMenu.
//
// The docked BRUSH column shows a subset of the same controls. The groups both
// surfaces draw are declared here and defined in ui/MacPaintUI.cpp, because
// they reach that file's own helpers: the test stroke's GPU cache, the dab
// library scan, and the user-preset save.

namespace np {

struct GpuContext;
class MixboxLut;
struct AppState;

// Which brush this is, whether it has been edited, and Save / Revert. Above
// the list and the page, because a Save button on one page is invisible from
// the others.
void drawBrushPresetHeader(AppState& st);

// The docked column's tip group: the test stroke, the shape sliders in radii,
// and the tip grid.
void drawBrushTipShapeGroup(AppState& st, GpuContext& gpu, const MixboxLut& lut);

// Load, Water, Opacity and Flow.
void drawBrushPaintGroup(AppState& st);

// naturalPaint's procedural paper grain. `ownPage` drops the collapsing header,
// which on a page of its own would only hide the controls behind a click.
void drawBrushTextureGroup(AppState& st, bool ownPage);

// The shelved link matrix; drawn only under --advanced-dynamics.
void drawBrushDynamicsGroup(AppState& st);

// The dab grid for the brush's own tip, or for the Dual Brush's second tip.
// Picking a cell changes the tip and nothing else; "Use native size" and "Use
// its spacing" are separate presses. Returns true when the model changed.
bool drawBrushTipPicker(AppState& st, GpuContext& gpu, bool dual);

// One test stroke with the current brush, and its dab count.
void drawBrushPreviewStroke(AppState& st, GpuContext& gpu, const MixboxLut& lut);

// Draws the window while `st.showBrushSettings` is set, and clears it when the
// window is closed, so the Window menu's tick and the close button agree.
void drawBrushSettingsWindow(AppState& st, GpuContext& gpu, const MixboxLut& lut);

}  // namespace np
