#include "ui/MacPaintUI.hpp"

#include "app/StrokeSession.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/DabPicker.hpp"
#include "ui/DynamicsMatrixPanel.hpp"
#include "ui/FileDialog.hpp"
#include "ui/LabelledControl.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/DockLayout.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/NewDocumentDialog.hpp"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <functional>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "app/BrushLibraryFile.hpp"
#include "app/BrushRowIcon.hpp"
#include "app/CloseDecision.hpp"
#include "app/CompPanel.hpp"
#include "app/PanelLayout.hpp"
#include "app/ControlsLayout.hpp"
#include "app/CurveEdit.hpp"
#include "app/DabPreview.hpp"
#include "app/StrokePreview.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/AdjustmentOps.hpp"
#include "app/FilterOps.hpp"
#include "app/HistoryPanel.hpp"
#include "app/ImportImage.hpp"
#include "app/Journal.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "app/MoveTool.hpp"  // Tool::Move
#include "app/OpenAnyFile.hpp"
#include "app/QuitSequence.hpp"
#include "app/SelectionDrag.hpp"
#include "app/Snapping.hpp"
#include "app/UserBrushLibrary.hpp"
#include "app/ViewTransform.hpp"
#include "app/WheelInput.hpp"
#include "app/ZoomAndSize.hpp"
#include "color/LutBake.hpp"  // kMaxCurvePointsPerChannel
#include "core/Histogram.hpp"
#include "core/LayerCompOps.hpp"
#include "core/LayerOps.hpp"
#include "core/SelectionRefine.hpp"
#include "imgui.h"
#include "io/ExportAs.hpp"
#include "color/Space.hpp"
#include "ops/Feather.hpp"
#include "ops/FloodFill.hpp"
#include "ops/Gradient.hpp"
#include "io/ExportStates.hpp"
#include "brush/BrushModelFields.hpp"
#include "ui/BrushFieldPresentation.hpp"
#include "ui/BrushSettingsWindow.hpp"
#include "ui/CanvasQuad.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/Fonts.hpp"
#include "ui/MacNativeMenu.hpp"
#include "app/TouchGestureSession.hpp"
#include "ui/MacTrackpadGestures.hpp"
#include "ui/MacTrackpadTouch.hpp"
#include "ui/MenuModel.hpp"
#include "ui/ToolCursor.hpp"
#include "ui/TransformCompositeSplit.hpp"
#include "ui/TransformPreviewTexture.hpp"

namespace np {
namespace {

// The layout constants this file used to invent are now docs/ui.md section
// 2's, in ui/AtelierLayout: a 44 px single-column palette (this file had a
// 78 px 2-wide-grid palette of 30 px cells before the supplied design's
// single column) and a 322 px right column (this file had 300 px, itself
// widened from 268 px by UI detour step 3 to stop the longest label clipping
// -- 322 keeps that fix and takes the design's number).
//
// There is no file-scope `kToolSize` any more. The user's correction ("make
// the toolbar fit without scrolling ... the buttons are too large") replaced
// the fixed 36px cell with one computed per frame from the palette band's
// live height (`atelierToolCellSize()`, ui/AtelierLayout.hpp/.cpp) -- a
// value that can legitimately differ between two calls in the same frame if
// the window was just resized is not a `constexpr`, and stashing it in a
// file-static mutable instead is exactly the shape of the bug that caused
// the clipping regression this replaces: a per-frame value masquerading as
// a constant. `toolButton()` and `moreToolsButton()` below now take the
// cell size as an explicit parameter, computed once per palette draw and
// threaded through, so there is nowhere for a stale size to hide.
//
// `kSwatchStripH` is gone with the band it measured: the design has no
// full-width swatch strip along the bottom of the window. That row is where
// the status bar goes, and the pigment well it held has moved into the COLOR
// section of the right-hand column, which is where docs/ui.md section 3.3 puts
// colour selection.
// `kControlsW` is gone the same way, and for a sharper version of the same
// reason: it aliased `kRightColumnW`, the right-hand column's fixed 322 px,
// and the right-hand column is not fixed any more. A panel's width is its
// dock's extent, which the user drags and `app/PanelLayout` persists, so a
// compile-time constant standing in for it would be exactly the "per-frame
// value masquerading as a constant" this comment already warns about -- one
// revision later and one axis over. Its only reader was the label-column log
// line, which now reports `bands.rightDock.w`: the number that is actually
// on screen.
//
// Peak gravity, in the same cells-per-step units as the rest of the velocity field.
constexpr float kMaxTilt = 0.50f;
// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// Screen-space px, not document-space -- the ruler strip's on-screen size is
// a layout constant like the band heights above, and the snap radius is
// defined to *feel* constant on screen regardless of zoom (converted to a
// document-space threshold per-frame; see the canvas block below).
constexpr float kRulerThickness = 20.0f;
constexpr float kSnapThresholdPx = 8.0f;

// Free Transform's gizmo, in SCREEN pixels -- converted to document space by
// the live zoom at the point of use, so a handle stays the same size under
// the finger at any magnification. The hit radius is deliberately larger than
// the drawn square (`kTransformHandleDrawPx`): a 4 px target is a target you
// miss, and missing here does not merely do nothing -- before this gizmo
// claimed the mouse it would have started a brush stroke.
constexpr float kTransformHandleHitPx = 9.0f;
constexpr float kTransformHandleDrawPx = 7.0f;
// How far above the top edge the rotate affordance sits. The same 24 px
// app/TransformSession.hpp names as its own default, restated here in screen
// units because that header has no notion of zoom and says so.
constexpr float kTransformRotateReachPx = 24.0f;

// ui/DocumentTexture: the **visible** documents, composited and uploaded once
// per revision each, drawn over the paper by the canvas block and reported on
// by the layers panel. File-scope for exactly that reason -- two places in
// this file need the same instance, and it is neither app state
// (app/AppState.hpp's own rule: transient UI-owned state stays in ui/) nor
// something a caller of drawUI() has any use for.
//
// A pool of `kVisibleDocumentCap` slots rather than the single texture this
// was before PLAN.md Phase 5 step 14, and that is PRD **A6** (P0) rather than
// a generalisation for its own sake -- ui/DocumentTexture.hpp's decision 5
// carries the argument, including why two visible documents through one
// instance would recomposite the whole canvas twice a frame.
//
// It owns GPU objects and is deliberately never released: gfx/Wgpu.hpp's
// convention is that every GPU object here lives for the process, and a
// destructor running after the device is gone would be worse than the leak it
// prevents. See DocumentTexture::release().
DocumentTexturePool g_documentTextures;

// ui/TransformPreviewTexture: T14's live-pixel Free Transform preview.
// File-scope for the same reason `g_documentTextures` is -- the begin block
// (where a session's crop is uploaded, once) and the draw block (where it is
// read every frame the session is active) are two different places in this
// same file, several hundred lines apart by the same deliberate input-before-
// tools/drawing-after-tools split app/TransformSession.hpp's own header
// documents. It is not app state for the identical reason: transient,
// ui/-owned, of no use to any caller of drawUI().
TransformPreviewTexture g_transformPreview;

// ===== Tool::Move -- BEGIN (app/MoveTool.hpp) ==============================
// The Move tool's whole per-drag state. File-scope for `g_transformPreview`'s
// own reason: the gesture is serviced in two places in this file that are a
// few hundred lines apart, and it is transient ui/ state of no use to any
// caller of drawUI(). Kept out of `AppState` deliberately -- nothing headless
// needs it; the part that IS headless and testable lives in app/MoveTool.
//
// `g_moveCommitPending` is why the pen-up and the commit are two frames. The
// canvas composite for a frame is built near the top of the canvas block,
// with the moving layer hidden (ui/TransformCompositeSplit); committing from
// the input code further down would write the pixels AFTER that texture was
// built, so the layer would be missing from the picture for exactly one frame
// while the preview quad that was standing in for it is retired in the same
// breath. Deferring to the top of the next frame -- before the composite
// decision -- means the frame that first shows the committed pixels is the
// same frame that stops hiding the layer.
bool g_moveDragging = false;
bool g_moveCommitPending = false;
float g_moveStartX = 0.0f;
float g_moveStartY = 0.0f;
float g_moveDx = 0.0f;
float g_moveDy = 0.0f;
// ===== Tool::Move -- END ===================================================

// Which arrangement the canvas band is in and which document is in the
// unfocused pane (PRD **A5**). ui/AtelierChrome.hpp owns the shape and the
// rule; this is where the session's copy lives.
//
// In ui/ rather than on `AppState` for that header's own rule -- it is view
// state, not document state: it survives no save, it is not a property of any
// document, and the focused pane is by construction the session's own active
// document, so nothing outside this file has to ask.
AtelierSplitState g_split;

// The in-flight CPU stroke (app/StrokeSession), file-scope for the same reason
// `g_documentTexture` is: it has to outlive one call of `drawUI()`, because a
// stroke spans frames. Exactly one exists, which is also the invariant -- two
// pens are not a thing this application has.
StrokeSession g_stroke;
// Why the brush is not painting, when it is not. Shown in the options bar
// rather than in a log line: a locked target makes the brush silently stop
// working, which is the failure a user cannot diagnose from the canvas.
std::string g_strokeRefusal;

// What the canvas wants the pointer to be this frame, or empty when the pointer
// is not over it. `canvasCursorRequest()` at the bottom of this file is the
// accessor, and main.cpp's `SystemCursorTable::apply()` is the only reader.
//
// **Cleared at the top of every `drawUI()`, not at the point of use.** The
// canvas block is one branch among several -- a frame with no document open, or
// one where the pointer sits over a panel, never reaches it -- so clearing it
// only where it is set would leave the previous frame's crosshair standing
// while the pointer was somewhere else entirely. That is precisely the stale
// cursor that suppressing the ImGui backend was meant to make impossible, so
// the reset is unconditional and lives where nothing can skip it.
std::optional<SDL_SystemCursor> g_canvasCursor;

// Which TOOL's bitmap cursor this frame wants -- ui/ToolCursor.hpp §7, whose
// bitmaps are keyed by `Tool` rather than by intent so that all twenty-eight
// tools are distinguishable rather than the handful an intent enum collapses
// them to. Only set alongside `g_canvasCursor` at the one call site below
// that answers a genuine tool question (`toolCursorOnTarget()`), and only
// when that question came back as something other than `Refuse`: a refused
// gesture must show the slashed circle, so leaving this `nullopt` there is
// what routes it to `sdlCursorFor(Refuse)` rather than to the tool's own
// icon. The guide-drag and pan/rotate branches set `g_canvasCursor` directly
// to an SDL shape that is no tool's cursor at all (see their own comments on
// why), so this stays `nullopt` through those too.
//
// Cleared on the same unconditional schedule as `g_canvasCursor`, for the
// same reason: a stale request surviving a frame the canvas branch never runs
// would be exactly the stale-cursor bug §6 already fixed once.
std::optional<Tool> g_canvasBitmapTool;

// --- The layer editor's shared state (UI detour step 3, problem 2) --------
//
// The `Layer` menu and the LAYERS panel are two views of one editor, so they
// share one selection and one refusal line: a command issued from the menu
// must move the panel's selection, and a refusal provoked from the menu has to
// be legible somewhere, which is the panel's own error line. File-scope for
// the same reason `g_documentTexture` is -- two places in this file need the
// same instance, and app/AppState.hpp's rule is that transient UI state stays
// in ui/.
//
// **The primary row is not here.** It moved to `OpenDocument::activeLayer`,
// because it is the layer the *brush* paints into as well as the one whose
// properties this panel edits, and those must be the same layer or the panel
// is lying about what a stroke will hit. That header carries the argument for
// where it lives; what stays here is everything that is genuinely panel state.
//
// The multi-selection below is still an index set into `Document::layers`
// (bottom-first), never panel rows, so it keeps meaning the same layers across
// a reorder. app/LayerPanel owns the one reversal between the two and nothing
// here reverses anything.
struct LayerEditorUiState {
  std::string lastError;
  // What a *successful* merge had to say (PLAN.md Phase 5 step 10). Kept apart
  // from `lastError` and drawn in a different colour because it is a different
  // claim: the operation went ahead, and something that used to be adjustable
  // is not any more. core/Merge.hpp §3 is why those are warnings rather than
  // refusals.
  std::vector<std::string> lastWarnings;
  // **The multi-selection** (PLAN.md Phase 5 step 11, PRD C12). Indices into
  // `Document::layers`, never panel rows, exactly as `selected` is; `selected`
  // stays the *primary* row -- the one whose opacity slider, blend dropdown and
  // name field the panel draws -- and is kept a member of this set.
  //
  // Two pieces of state rather than one, because they answer different
  // questions. "Which layer's properties am I editing" has exactly one answer
  // and always has, and every single-selection assertion in `--selftest` is
  // about that one. Collapsing them would have made `selected` mean "the first
  // of the set", which is a different layer after every gesture that reorders.
  LayerSelection selection = singleLayerSelection(0);
  // **The shift-click range anchor** (bug fix, user-reported): a THIRD piece
  // of state, distinct from both of the above. `selected` is reassigned on
  // every click including shift-clicks (by design, per the comment above), so
  // using it as the range start meant each shift-click re-anchored from the
  // row the PREVIOUS shift-click landed on -- row1, shift+row2 selected 1-2 as
  // intended, but shift+row3 then selected 2-3 instead of 1-3, because
  // `selected` had already moved to 2. This tracks the last row clicked
  // WITHOUT shift, which is what a contiguous range is supposed to extend
  // from across any number of shift-clicks in a row.
  size_t shiftAnchor = 0;
  // **The panel filter** (PRD C15). app/LayerPanel.hpp states the rule it
  // follows: it changes which rows are drawn and nothing else, the selection
  // survives it, and a command acts only on the rows that are visible.
  LayerFilter filter;
  char filterBuf[64] = "";

  // **Inline rename** (double-click a row's title). `renaming` is the model
  // index whose title is currently an edit field instead of a Selectable;
  // `nullopt` the rest of the time, which is most frames. The buffer is
  // seeded once, the moment a double-click sets `renaming`, and is only ever
  // written back to the layer on Enter -- `IsItemDeactivated()` without a
  // commit (a click elsewhere, Tab, Escape) just clears `renaming` and drops
  // the typed text, which is "cancel" without a second code path for it.
  std::optional<size_t> renaming;
  char renameFieldBuf[128] = "";
  // `SetKeyboardFocusHere()` must run on the exact frame the field is first
  // drawn, not on the frame the double-click set `renaming` -- so this is
  // consumed (set false) the first time the field is drawn after it is set.
  bool renameFieldFocusPending = false;

  // **Which groups are collapsed** (PRD C7's UI half). Keyed on
  // `Layer::groupTag`, the same stable string a member's `parent` names.
  //
  // Session-only, deliberately -- NOT round-tripped through `.npaint`. Three
  // reasons, in order of how much they alone would settle it: (1) it is view
  // state, the same category `g_layers.filter` and `g_layers.selection`
  // already are, and neither of those is saved either -- a `.npaint` that
  // remembered which rows a search box had hidden would be a strange
  // precedent to set for this one. (2) round-tripping it would mean a new
  // `np:` attribute on the Group part, a writer, a reader, an older-build
  // degradation story and a place in the format table (docs/document-format.md)
  // -- real cost for a control that changes nothing about the picture. (3) a
  // tag is per-session already: it is never persisted as a *reference* from
  // outside `Document::layers` (core/LayerSetOps.hpp's own argument for why
  // link-group numbers need no counter to undo), so keying UI state on it
  // outlives exactly one session's worth of grouping, which is what this
  // state is for. A tag whose group has since been ungrouped, or that
  // belongs to a document that has since closed, simply never matches
  // anything in `layerHiddenByCollapsedGroup()` again -- nothing prunes it
  // and nothing needs to.
  std::set<std::string> collapsedGroups;
};
LayerEditorUiState g_layers;

// The one path from a gesture to an edit, whichever control issued it. Every
// mutation lands in `app::recordLayerEdit()` inside `applyLayerCommand()`, so
// it bumps the document's revision (which is what makes ui/DocumentTexture
// recomposite and the canvas change), appends a history entry and marks the
// document structurally dirty.
void runLayerCommand(AppState& st, LayerCommand command) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    g_layers.lastError =
        "layer command refused: no document is open. File > New Document makes one.";
    return;
  }
  const LayerEditResult r = applyLayerCommand(*od, command, od->activeLayer);
  setActiveLayer(*od, r.selected);
  g_layers.selection = singleLayerSelection(r.selected);
  g_layers.lastError = r.ok ? std::string() : r.error;
  g_layers.lastWarnings = r.warnings;
}

// The same path for a gesture over the whole selection (PLAN.md Phase 5
// step 11). Two things happen here and nowhere else, both of them
// app/LayerPanel.hpp's stated filter rule:
//
//   the selection is **restricted to what the filter lets the user see**
//   before the command is applied, so a row hidden by a search box is never
//   deleted, moved or aligned by a gesture aimed at the rows on screen;
//   a restriction that empties the set **refuses with the count**, rather than
//   leaving a button that appears to do nothing.
void runLayerSetCommand(AppState& st, LayerSetCommand command) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    g_layers.lastError =
        "layer command refused: no document is open. File > New Document makes one.";
    return;
  }
  const LayerSelection visible =
      restrictSelectionToFilter(od->document, g_layers.selection, g_layers.filter);
  if (visible.empty() && !g_layers.selection.empty()) {
    g_layers.lastError =
        std::string(layerSetCommandLabel(command)) + " refused: all " +
        std::to_string(g_layers.selection.size()) +
        " selected layer(s) are hidden by the panel filter, so this would have acted on "
        "rows you cannot see. Clear the filter, or select a visible row.";
    return;
  }
  const LayerSetEditResult r = applyLayerSetCommand(*od, command, visible);
  g_layers.lastError = r.ok ? std::string() : r.error;
  g_layers.lastWarnings = r.warnings;
  if (!r.ok) return;
  g_layers.selection = r.selection;
  setActiveLayer(*od, r.selection.empty() ? 0 : r.selection.indices.front());
}


// Icons are drawn rather than loaded so there is no asset to ship and they stay
// crisp at any DPI. Chunky strokes, no anti-aliased flourishes — the originals
// were 1-bit and read better for it.
void drawToolIcon(ImDrawList* dl, Tool t, ImVec2 c, float s, ImU32 col) {
  const float th = std::max(1.5f, s * 0.11f);
  switch (t) {
    case Tool::Brush: {
      dl->AddLine(ImVec2(c.x + s * 0.30f, c.y - s * 0.42f),
                  ImVec2(c.x - s * 0.06f, c.y + s * 0.06f), col, th);
      dl->AddCircleFilled(ImVec2(c.x - s * 0.20f, c.y + s * 0.22f), s * 0.24f, col, 16);
      break;
    }
    case Tool::Water: {
      // Teardrop: apex up, round belly.
      ImVec2 pts[3] = {ImVec2(c.x, c.y - s * 0.46f),
                       ImVec2(c.x + s * 0.34f, c.y + s * 0.12f),
                       ImVec2(c.x - s * 0.34f, c.y + s * 0.12f)};
      dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
      dl->AddCircleFilled(ImVec2(c.x, c.y + s * 0.12f), s * 0.34f, col, 20);
      break;
    }
    case Tool::DryBrush: {
      for (int i = -2; i <= 2; ++i) {
        const float x = c.x + i * s * 0.16f;
        dl->AddLine(ImVec2(x, c.y - s * 0.34f),
                    ImVec2(x + s * 0.06f, c.y + s * 0.38f), col, th * 0.8f);
      }
      break;
    }
    case Tool::Eyedropper: {
      dl->AddLine(ImVec2(c.x + s * 0.32f, c.y - s * 0.36f),
                  ImVec2(c.x - s * 0.12f, c.y + s * 0.10f), col, th * 1.6f);
      ImVec2 tip[3] = {ImVec2(c.x - s * 0.34f, c.y + s * 0.40f),
                       ImVec2(c.x - s * 0.24f, c.y - s * 0.02f),
                       ImVec2(c.x + s * 0.02f, c.y + s * 0.22f)};
      dl->AddTriangleFilled(tip[0], tip[1], tip[2], col);
      break;
    }
    case Tool::Marquee: {
      // A dashed rectangle -- the marching ants themselves, at rest. Drawn as
      // eight segments rather than an AddRect with a stroke pattern because
      // ImDrawList has no dash support, and the same eight-segment shape is
      // what drawMarchingAnts() animates on the canvas.
      const float x0 = c.x - s * 0.36f, x1 = c.x + s * 0.36f;
      const float y0 = c.y - s * 0.30f, y1 = c.y + s * 0.30f;
      const float dx = (x1 - x0) / 5.0f;
      const float dy = (y1 - y0) / 3.0f;
      for (int i = 0; i < 5; i += 2) {
        dl->AddLine(ImVec2(x0 + dx * i, y0), ImVec2(x0 + dx * (i + 1), y0), col, th);
        dl->AddLine(ImVec2(x0 + dx * i, y1), ImVec2(x0 + dx * (i + 1), y1), col, th);
      }
      for (int i = 0; i < 3; i += 2) {
        dl->AddLine(ImVec2(x0, y0 + dy * i), ImVec2(x0, y0 + dy * (i + 1)), col, th);
        dl->AddLine(ImVec2(x1, y0 + dy * i), ImVec2(x1, y0 + dy * (i + 1)), col, th);
      }
      break;
    }
    case Tool::Hand: {
      dl->AddRectFilled(ImVec2(c.x - s * 0.26f, c.y - s * 0.06f),
                        ImVec2(c.x + s * 0.26f, c.y + s * 0.40f), col);
      for (int i = 0; i < 3; ++i) {
        const float x = c.x - s * 0.24f + i * s * 0.20f;
        dl->AddRectFilled(ImVec2(x, c.y - s * 0.40f),
                          ImVec2(x + s * 0.13f, c.y), col);
      }
      break;
    }
    case Tool::Zoom: {
      dl->AddCircle(ImVec2(c.x - s * 0.08f, c.y - s * 0.10f), s * 0.28f, col, 20, th);
      dl->AddLine(ImVec2(c.x + s * 0.10f, c.y + s * 0.10f),
                  ImVec2(c.x + s * 0.38f, c.y + s * 0.38f), col, th * 1.4f);
      break;
    }
    default: break;
  }
}

// Draws one Lucide glyph, centred at `c`, at the spec's fixed 15px
// (ui/Fonts.hpp's kToolIconSizePx) -- independent of whatever size the
// ambient UI text face is running at, which `AddText(ImFont*, size, ...)`
// gives for free by taking the size as an argument rather than reading it
// off a pushed font. Returns false (drawing nothing) when the codepoint is 0
// or the merged font cannot draw it, which is the caller's cue to fall back
// to drawToolIcon()'s hand-drawn vectors -- see ui/Fonts.cpp's
// installToolIconFont() for why that can happen on a machine where the
// vendored TTF somehow failed to load.
bool drawToolGlyph(ImDrawList* dl, uint32_t codepoint, ImVec2 c, ImU32 col) {
  ImFont* font = uiFonts().text;
  if (font == nullptr || codepoint == 0u) return false;
  ImFontBaked* baked = font->GetFontBaked(kToolIconSizePx);
  if (baked == nullptr || baked->FindGlyphNoFallback(static_cast<ImWchar>(codepoint)) == nullptr)
    return false;
  const std::string glyph = encodeUtf8(codepoint);
  const ImVec2 ts = font->CalcTextSizeA(kToolIconSizePx, FLT_MAX, 0.0f, glyph.c_str());
  dl->AddText(font, kToolIconSizePx, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), col,
              glyph.c_str());
  return true;
}

// One cell of docs/ui.md section 2's single-column palette. `t` is always
// drawn -- the ~20 cells toolImplemented() says are not built yet are not
// skipped, they are drawn **inert**: dimmed, un-clickable (InvisibleButton
// still reports a click; this simply never acts on it), and never take the
// accent "selected" fill, because `st.brush.tool` can never equal one of
// them if nothing ever assigns it. Only the tooltip differs in kind rather
// than degree -- toolTooltip() appends "Not built yet." -- so a user who
// hovers finds out why the cell did nothing, rather than assuming it is
// broken.
bool toolButton(AppState& st, Tool t, float cellSize) {
  ImGui::PushID(static_cast<int>(t));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(cellSize, cellSize);
  const bool implemented = toolImplemented(t);
  const bool clickedRaw = ImGui::InvisibleButton("##tool", size);
  const bool clicked = clickedRaw && implemented;
  const bool selected = implemented && st.brush.tool == t;
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = selected                    ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                    : (hovered && implemented)  ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                                : ImGui::GetColorU32(ImGuiCol_Button);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg);
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
              ImGui::GetColorU32(ImGuiCol_Border));

  // Selected tools invert, the way MacPaint's did. A not-built-yet tool is
  // never selected (see above), so its icon is always the dimmed, halved-
  // alpha secondary-text colour -- the one visual cue that survives even a
  // screenshot with no cursor in it: "this cell is present but off."
  const ImU32 fg = selected ? IM_COL32(20, 22, 24, 255)
                   : implemented
                       ? ImGui::GetColorU32(ImGuiCol_Text)
                       : (atelierToken(kTextSecondary) & 0x00FFFFFFu) | IM_COL32(0, 0, 0, 110);
  const ImVec2 c(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
  if (!drawToolGlyph(dl, toolIconCodepoint(t), c, fg))
    drawToolIcon(dl, t, c, size.x * 0.62f, fg);  // graceful fallback -- see drawToolGlyph()'s comment

  // Separate from `hovered`, which also picks the cell's fill colour above --
  // gating that on the tooltip's own stationary+delay timer would make the
  // cell itself feel laggy to hover, not just its tooltip.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    const std::string tip = toolTooltip(t);
    ImGui::SetTooltip("%s", tip.c_str());
  }
  if (clicked) st.brush.tool = t;
  ImGui::PopID();
  return clicked;
}

// The "..." overflow cell that ends docs/ui.md section 2's palette (group 5:
// Hand, Zoom, More). It is not a Tool -- there is no overflow menu behind it
// yet, so like every not-built-yet cell above it draws inert, but unlike
// them it has no `st.brush.tool` value to compare against and no
// toolTooltip() to borrow, hence its own small function rather than a
// twenty-first entry manufactured in `enum class Tool` for one disabled
// button.
void moreToolsButton(float cellSize) {
  ImGui::PushID("more");
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(cellSize, cellSize);
  ImGui::InvisibleButton("##more", size);
  const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(ImGuiCol_Button));
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(ImGuiCol_Border));

  const ImU32 fg = (atelierToken(kTextSecondary) & 0x00FFFFFFu) | IM_COL32(0, 0, 0, 110);
  const ImVec2 c(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
  if (!drawToolGlyph(dl, kMoreIconCodepoint, c, fg)) {
    // literal-dots fallback: no vector for this one in drawToolIcon(), and
    // three periods need no glyph at all to read as "more".
    dl->AddText(ImVec2(c.x - 8.0f, c.y - ImGui::GetTextLineHeight() * 0.5f), fg, "...");
  }
  if (hovered) ImGui::SetTooltip("More tools\nNot built yet -- no overflow menu exists yet.");
  ImGui::PopID();
}

// docs/ui.md section 2's palette, nested into Photoshop-style flyout
// groups (ui/AtelierChrome.hpp's `kToolGroups`) rather than one cell per
// `Tool` -- the user's own instruction: "nest similar tools into a flyout
// to conserve space like photoshop." Display order and the four
// design-group rules both live in that table now (it is metadata, free of
// ImGui, the same split `kToolMeta` draws); what follows here is only how
// a group's cell and its flyout are drawn.

// Photoshop's own press-and-hold window is roughly a third of a second --
// long enough that a quick click-to-select never accidentally opens the
// flyout, short enough that a deliberate hold does not feel laggy.
constexpr float kFlyoutHoldSeconds = 0.35f;

// A flyout row's height and the icon gutter it reserves on the left --
// fixed regardless of the live palette cell size (`cellSize`), the same
// way Photoshop's own flyout menus do not grow or shrink with toolbar
// zoom. Row *width* is not fixed here -- see toolGroupButton() for why it
// is computed per popup instead.
constexpr float kFlyoutRowH = 24.0f;
constexpr float kFlyoutIconGutter = 24.0f;
constexpr float kFlyoutPadX = 10.0f;

// One row of a group's flyout: icon + name, Photoshop's own flyout
// layout. Hand-drawn like toolButton() rather than an ImGui::Selectable
// with a glyph embedded in its text label, so it renders through the
// exact same drawToolGlyph()/kToolIconSizePx path the palette cells
// themselves use -- a merged Lucide glyph is only proven
// (app/selftest/Fonts.cpp's Part D) to bake at kToolIconSizePx; drawing it
// via plain text at whatever font size ImGui's popup happens to be using
// would be a different, unproven bake.
//
// Returns true on click regardless of toolImplemented() -- toolButton()'s
// own clickedRaw/clicked split, so the caller (not this function) decides
// what a click on a not-yet-built member means.
bool toolFlyoutRow(Tool member, bool isCurrent, float rowW) {
  const bool implemented = toolImplemented(member);
  ImGui::PushID(static_cast<int>(member));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(rowW, kFlyoutRowH);
  const bool clicked = ImGui::InvisibleButton("##row", size);
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (isCurrent || hovered) {
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                      isCurrent ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                                : ImGui::GetColorU32(ImGuiCol_ButtonHovered));
  }

  // The exact fg-colour rule toolButton() uses -- dimmed for a not-built
  // tool, inverted for the current pick, plain text otherwise -- repeated
  // here rather than factored out, because toolButton() computes it inline
  // from state (selected/hovered/implemented) this function does not share
  // (a flyout row is never "selected" in toolButton()'s sense; `isCurrent`
  // is a different, group-local idea).
  const ImU32 fg = !implemented
                       ? (atelierToken(kTextSecondary) & 0x00FFFFFFu) | IM_COL32(0, 0, 0, 110)
                   : isCurrent ? IM_COL32(20, 22, 24, 255)
                               : ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 iconCenter(p.x + kFlyoutIconGutter * 0.5f, p.y + size.y * 0.5f);
  if (!drawToolGlyph(dl, toolIconCodepoint(member), iconCenter, fg))
    drawToolIcon(dl, member, iconCenter, kFlyoutIconGutter * 0.31f, fg);

  const std::string label = toolName(member);
  const ImVec2 textPos(p.x + kFlyoutIconGutter, p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f);
  dl->AddText(textPos, fg, label.c_str());

  // Separate from `hovered`, which also picks the row's fill above -- gating
  // that on the tooltip's own stationary+delay timer would make the row
  // itself feel laggy to hover, not just its tooltip.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
    const std::string tip = toolTooltip(member);
    ImGui::SetTooltip("%s", tip.c_str());
  }
  ImGui::PopID();
  return clicked;
}

// One cell of the palette's nested flyout groups. Draws exactly one
// `Tool` -- the group's *current* member, read from
// `st.toolGroupCurrent[groupIndex]` -- through the existing toolButton(),
// so every rule that function already enforces (disabled dimming,
// accent-only-if-selected, "Not built yet." tooltip, the
// drawToolGlyph()/drawToolIcon() fallback) applies to a grouped cell
// exactly as it does to an ungrouped one, with no duplicated logic. What
// this function adds on top of a single tool is only what a *group*
// needs: a small corner triangle when it has more than one member, and
// the flyout itself, opened by right-click (BeginPopupContextItem(), the
// established pattern this file already uses for the LAYERS row context
// menu) or a ~350ms press-and-hold.
//
// `forceOpen` is --flyout-demo's hook (AppState::openToolFlyoutDemo) --
// see that field's own comment for why a screenshot needs it.
void toolGroupButton(AppState& st, int groupIndex, float cellSize, bool forceOpen) {
  // Lazily sized and filled on first use -- AppState::toolGroupCurrent's
  // own comment says why this header cannot size it in advance.
  if (st.toolGroupCurrent.size() != static_cast<size_t>(kToolGroupCount)) {
    st.toolGroupCurrent.resize(static_cast<size_t>(kToolGroupCount));
    for (int g = 0; g < kToolGroupCount; ++g)
      st.toolGroupCurrent[static_cast<size_t>(g)] = toolGroupDefaultMember(g);
  }

  const ToolGroup& group = kToolGroups[groupIndex];
  Tool& current = st.toolGroupCurrent[static_cast<size_t>(groupIndex)];

  ImGui::PushID(groupIndex);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  toolButton(st, current, cellSize);
  // toolButton() leaves its own InvisibleButton as ImGui's "last item" --
  // exactly what BeginPopupContextItem() below needs to detect a
  // right-click, and what IsItemActive()/MouseDownDuration need for
  // press-and-hold.

  if (group.memberCount > 1) {
    // Bottom-right corner triangle -- Photoshop's own "this cell hides
    // more" mark. Small enough not to compete with the glyph beside it;
    // drawn last, over the cell's own bottom-right corner.
    //
    // Colour follows the same selected/not split toolButton() itself
    // draws its icon with -- ImGuiCol_Border (the first colour tried here)
    // reads at nearly the same luminance as the cell's own idle
    // background and border, which made the badge functionally invisible
    // on a screenshot rather than merely subtle. kTextPrimary against the
    // idle/hovered background and the dark on-accent ink against the
    // selected one both hold real contrast in both states.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool selected = toolImplemented(current) && st.brush.tool == current;
    const ImU32 triColor = selected ? IM_COL32(20, 22, 24, 255) : atelierToken(kTextPrimary);
    const float s = std::max(5.0f, cellSize * 0.28f);
    const ImVec2 corner(p.x + cellSize, p.y + cellSize);
    dl->AddTriangleFilled(ImVec2(corner.x - s, corner.y), corner,
                          ImVec2(corner.x, corner.y - s), triColor);

    // Anchored to the cell's right edge, per the design brief -- only
    // takes effect the frame the popup opens (ImGui ignores
    // SetNextWindowPos on a popup already open), so this does not fight a
    // user who has since scrolled or dragged it.
    ImGui::SetNextWindowPos(ImVec2(p.x + cellSize + 4.0f, p.y));

    const bool heldLongEnough =
        ImGui::IsItemActive() &&
        ImGui::GetIO().MouseDownDuration[ImGuiMouseButton_Left] > kFlyoutHoldSeconds;
    if (heldLongEnough || forceOpen) ImGui::OpenPopup("##toolFlyout");

    if (ImGui::BeginPopupContextItem("##toolFlyout")) {
      // Width fits the longest member name in *this* group, not a
      // guessed constant -- group 10 (Brush/Pencil/Water/DryBrush) and
      // group 1 (Move/Frame) do not need the same width, and a fixed one
      // wide enough for the longest name in the whole table would waste
      // space on every shorter group's flyout.
      float rowW = 60.0f;
      for (int m = 0; m < group.memberCount; ++m)
        rowW = std::max(rowW, ImGui::CalcTextSize(toolName(group.members[m])).x);
      rowW += kFlyoutIconGutter + kFlyoutPadX;

      for (int m = 0; m < group.memberCount; ++m) {
        const Tool member = group.members[m];
        if (toolFlyoutRow(member, member == current, rowW)) {
          current = member;                                     // display state always updates
          if (toolImplemented(member)) st.brush.tool = member;  // selection only if real
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::EndPopup();
    }
  }
  ImGui::PopID();
}

// Tilt is a direction and a steepness, so it gets a pad you drag rather than two
// numbers. The dot is where the low corner of the board is; distance from centre
// is how steep. Matches screen orientation: drag down, washes run down.
bool tiltPad(AppState& st, float size) {
  ImGui::PushID("tilt");
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
  const float r = size * 0.5f - 4.0f;

  ImGui::InvisibleButton("##pad", ImVec2(size, size));
  const bool active = ImGui::IsItemActive();
  bool changed = false;

  if (active) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    float dx = (m.x - c.x) / r;
    float dy = (m.y - c.y) / r;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.0f) { dx /= len; dy /= len; }
    st.sim.tiltX = dx * kMaxTilt;
    st.sim.tiltY = dy * kMaxTilt;
    changed = true;
  }
  // Double-click lays the board flat again.
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    st.sim.tiltX = 0.0f;
    st.sim.tiltY = 0.0f;
    changed = true;
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                    ImGui::GetColorU32(ImGuiCol_ChildBg));
  dl->AddRect(p, ImVec2(p.x + size, p.y + size),
              ImGui::GetColorU32(ImGuiCol_Border));
  const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border);
  dl->AddLine(ImVec2(c.x, p.y + 3), ImVec2(c.x, p.y + size - 3), grid);
  dl->AddLine(ImVec2(p.x + 3, c.y), ImVec2(p.x + size - 3, c.y), grid);
  dl->AddCircle(c, r, grid, 32);

  const float tx = st.sim.tiltX / kMaxTilt;
  const float ty = st.sim.tiltY / kMaxTilt;
  const ImVec2 knob(c.x + tx * r, c.y + ty * r);
  if (tx != 0.0f || ty != 0.0f) {
    dl->AddLine(c, knob, IM_COL32(80, 165, 185, 255), 2.0f);
  }
  dl->AddCircleFilled(knob, 5.0f, IM_COL32(235, 235, 225, 255), 16);

  ImGui::PopID();
  return changed;
}

void applyToolToBrush(AppState& st) {
  switch (st.brush.tool) {
    case Tool::Water:
      st.sim.brushPigment = 0.0f;
      st.sim.brushWater = st.brush.wetness * 1.4f;
      st.sim.brushHardness = 0.15f;
      break;
    case Tool::DryBrush:
      st.sim.brushPigment = st.brush.load * 1.3f;
      st.sim.brushWater = st.brush.wetness * 0.15f;
      st.sim.brushHardness = 0.9f;
      break;
    case Tool::Brush:
    default:
      st.sim.brushPigment = st.brush.load;
      st.sim.brushWater = st.brush.wetness;
      st.sim.brushHardness = st.brush.model.tip.hardness;
      break;
  }
}

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD
// Q5-Q7). ------------------------------------------------------------

// A "nice" ruler step (the 1-2-5-10-20-50-100... progression every ruler --
// Photoshop, Illustrator, browser devtools -- uses) such that consecutive
// major ticks land at least ~50 screen px apart, so labels stay readable at
// any zoom instead of crowding together.
float rulerStep(float zoom) {
  static constexpr float kSteps[] = {1,    2,    5,    10,   20,    25,   50,    100,
                                     200,  250,  500,  1000, 2000,  5000, 10000, 20000};
  for (float s : kSteps)
    if (s * zoom >= 50.0f) return s;
  return kSteps[(sizeof(kSteps) / sizeof(kSteps[0])) - 1];
}

// Thin strips along the top and left of the canvas window with document-
// space tick marks and coordinate labels, reusing the exact same
// ViewTransform every other document-space thing on the canvas goes
// through -- not a second, independently derived screen<->document mapping.
//
// Only meaningful at view.rotation == 0: a ruler measures a single
// screen-aligned document axis, which stops being a coherent idea once that
// axis isn't screen-aligned any more. PLAN.md step 12 explicitly allows a
// simple fallback here rather than solving rotated-ruler UX -- this draws a
// plain, dimmed, tickless band at any non-zero rotation instead of ticks
// that would no longer correspond to anything.
void drawRulers(ImDrawList* dl, const ViewTransform& xform, const CanvasView& view,
                ImVec2 canvasPos, ImVec2 paintOrigin, ImVec2 avail, float thickness) {
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_ChildBg);

  const ImVec2 topMin(paintOrigin.x, canvasPos.y);
  const ImVec2 topMax(paintOrigin.x + avail.x, canvasPos.y + thickness);
  const ImVec2 leftMin(canvasPos.x, paintOrigin.y);
  const ImVec2 leftMax(canvasPos.x + thickness, paintOrigin.y + avail.y);

  dl->AddRectFilled(canvasPos, topMax, bg);  // corner + top, one rect
  dl->AddRectFilled(leftMin, leftMax, bg);

  constexpr float kRotationEps = 1e-4f;
  if (std::fabs(view.rotation) > kRotationEps) {
    const ImU32 dim = IM_COL32(0, 0, 0, 70);
    dl->AddRectFilled(topMin, topMax, dim);
    dl->AddRectFilled(leftMin, leftMax, dim);
    return;
  }

  const ImU32 tick = ImGui::GetColorU32(ImGuiCol_Border);
  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

  // Document-space extent currently visible along each ruler, found by
  // inverting the strip's own screen-space endpoints through toCanvas() --
  // min/max rather than a direct left->right read, since a mirrored view
  // can reverse which screen edge maps to the smaller document coordinate.
  // At rotation == 0 the transform is diagonal (see ViewTransform.cpp's
  // algebra), so the y/x coordinate fed in below for the "other" axis is
  // arbitrary -- it cancels out.
  const float cx0 = xform.toCanvas(Vec2{topMin.x, topMin.y}).x;
  const float cx1 = xform.toCanvas(Vec2{topMax.x, topMin.y}).x;
  const float cy0 = xform.toCanvas(Vec2{leftMin.x, leftMin.y}).y;
  const float cy1 = xform.toCanvas(Vec2{leftMin.x, leftMax.y}).y;
  const float xMin = std::min(cx0, cx1), xMax = std::max(cx0, cx1);
  const float yMin = std::min(cy0, cy1), yMax = std::max(cy0, cy1);

  const float step = rulerStep(view.zoom);
  char label[32];

  for (float x = std::floor(xMin / step) * step; x <= xMax + step; x += step) {
    const Vec2 s = xform.toScreen(Vec2{x, 0.0f});
    if (s.x < topMin.x - 1.0f || s.x > topMax.x + 1.0f) continue;
    dl->AddLine(ImVec2(s.x, topMax.y - thickness * 0.6f), ImVec2(s.x, topMax.y), tick, 1.0f);
    std::snprintf(label, sizeof(label), "%.0f", x);
    dl->AddText(ImVec2(s.x + 2.0f, topMin.y + 2.0f), text, label);
  }
  for (float y = std::floor(yMin / step) * step; y <= yMax + step; y += step) {
    const Vec2 s = xform.toScreen(Vec2{0.0f, y});
    if (s.y < leftMin.y - 1.0f || s.y > leftMax.y + 1.0f) continue;
    dl->AddLine(ImVec2(leftMax.x - thickness * 0.6f, s.y), ImVec2(leftMax.x, s.y), tick, 1.0f);
    std::snprintf(label, sizeof(label), "%.0f", y);
    dl->AddText(ImVec2(leftMin.x + 2.0f, s.y + 2.0f), text, label);
  }
}

// PRD Q7: grid overlay, spacing/subdivisions from AppState's own sliders
// (this file's ##controls panel). Lines are found the same way rulers place
// ticks -- document-space positions from gridLinePositions(), mapped to
// screen through the same ViewTransform -- so the overlay tracks pan, zoom,
// mirror and rotation exactly like the canvas image itself.
void drawGridOverlay(ImDrawList* dl, const ViewTransform& xform, float texW, float texH,
                     float spacing, int subdivisions, float zoom) {
  if (!(spacing > 0.0f) || subdivisions < 1) return;
  const float minor = spacing / static_cast<float>(std::max(1, subdivisions));
  if (!(minor > 0.0f)) return;
  // Too dense to read (or draw efficiently) at the current zoom -- fall
  // back to major-only lines rather than flooding the canvas with
  // sub-pixel-spaced minor lines.
  const bool minorVisible = minor * zoom >= 4.0f;
  const auto xs = gridLinePositions(spacing, minorVisible ? subdivisions : 1, 0.0f, texW);
  const auto ys = gridLinePositions(spacing, minorVisible ? subdivisions : 1, 0.0f, texH);
  const ImU32 minorCol = IM_COL32(255, 255, 255, 22);
  const ImU32 majorCol = IM_COL32(255, 255, 255, 55);

  for (float x : xs) {
    const ImU32 col = isMajorGridLine(x, spacing) ? majorCol : minorCol;
    const Vec2 a = xform.toScreen(Vec2{x, 0.0f});
    const Vec2 b = xform.toScreen(Vec2{x, texH});
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
  }
  for (float y : ys) {
    const ImU32 col = isMajorGridLine(y, spacing) ? majorCol : minorCol;
    const Vec2 a = xform.toScreen(Vec2{0.0f, y});
    const Vec2 b = xform.toScreen(Vec2{texW, y});
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
  }
}

// PRD E6's marching ants.
//
// Two passes rather than one: a solid dark line, then a light dashed line over
// it with the gaps left open. That is what makes the boundary legible on both
// a white canvas and dark paint -- a single-colour outline disappears against
// one or the other, which is the reason real editors animate two tones rather
// than drawing one dotted line.
//
// The phase advances with wall-clock time, so the ants crawl. `ImGui::GetTime()`
// rather than a frame counter, so the speed is the same whether the app is
// running at 30 or 120 fps.
//
// **The outline is the selection's TRUE boundary**, traced by
// core/SelectionBoundary from the coverage itself: closed contours round every
// island, an inner contour round every hole, and a corner wherever the shape
// turns. It used to be the selection's bounding box, which was exact while
// `selectRectangle()` was the only constructor and became a lie the day PRD E3's
// lasso, polygon lasso and wand landed -- every selection, whatever its shape,
// drew as a rectangle, and PRD E7's Shift-add drew two islands as one box round
// both. core/SelectionBoundary.hpp's header carries the full argument, including
// which threshold counts as inside and why marching squares was not used.
//
// **The dash phase runs along ARC LENGTH, across every segment and every
// contour**, which is what makes the ants crawl round a corner rather than
// restart at each one. `phaseAccum` is threaded through the segment drawing for
// exactly that: it carries the leftover of the previous segment's last dash
// into the next segment's first. The one place it cannot be continuous is a
// closed contour's seam, where the loop's perimeter is not generally a whole
// number of dash periods -- there is no phase that makes both ends of a loop
// agree, and every editor has that same seam.
constexpr ImU32 kAntDark = IM_COL32(0, 0, 0, 190);
constexpr ImU32 kAntLight = IM_COL32(255, 255, 255, 230);
constexpr float kAntDash = 6.0f;

// The animated phase for this frame, in screen pixels along the boundary.
// Read once per frame by each caller and threaded through, rather than sampled
// per segment, so two segments of the same outline cannot land on two different
// clock readings.
float marchingAntPhase() {
  return static_cast<float>(std::fmod(ImGui::GetTime() * 18.0, kAntDash * 2.0));
}

// One segment of an outline, both passes, advancing `phaseAccum` by the
// segment's screen length. `phaseAccum` is "how far into the current dash
// period the segment starts", so a segment whose predecessor ended mid-dash
// begins mid-dash.
void drawAntSegment(ImDrawList* dl, ImVec2 a, ImVec2 b, float& phaseAccum) {
  dl->AddLine(a, b, kAntDark, 1.0f);
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (!(len > 0.0f)) return;
  const float ux = dx / len, uy = dy / len;
  constexpr float kPeriod = kAntDash * 2.0f;
  for (float t = -phaseAccum; t < len; t += kPeriod) {
    const float s = std::max(t, 0.0f);
    const float e = std::min(t + kAntDash, len);
    if (e <= s) continue;
    dl->AddLine(ImVec2(a.x + ux * s, a.y + uy * s), ImVec2(a.x + ux * e, a.y + uy * e),
                kAntLight, 1.0f);
  }
  phaseAccum = std::fmod(phaseAccum + len, kPeriod);
}

// A closed or open run of document-space points, drawn as ants.
//
// The transform is applied per point rather than per contour, because the view
// can be rotated and mirrored (app/ViewTransform) and a boundary is not
// axis-aligned on screen even when it is axis-aligned in the document.
void drawAntPolyline(ImDrawList* dl, const ViewTransform& xform,
                     const std::vector<Vec2>& pts, bool closed, float& phaseAccum) {
  if (pts.size() < 2) return;
  const size_t last = closed ? pts.size() : pts.size() - 1;
  Vec2 prev = xform.toScreen(pts[0]);
  for (size_t i = 0; i < last; ++i) {
    const Vec2 next = xform.toScreen(pts[(i + 1) % pts.size()]);
    drawAntSegment(dl, ImVec2(prev.x, prev.y), ImVec2(next.x, next.y), phaseAccum);
    prev = next;
  }
}

// The committed selection's outline: every contour of it.
//
// A hole's contour is drawn exactly like an island's, deliberately -- nothing
// here distinguishes the two, and a version that drew only outer contours would
// show a ring selection as a filled disc, which is a picture that says the
// opposite of what the selection does.
//
// The per-frame cost is one dark line plus its dashes per TURNING POINT, not
// per boundary texel -- core/SelectionBoundary collapses collinear runs, so a
// full-canvas selection is four segments. There is deliberately no cap on that
// count: a wand result on a noisy photograph can have thousands of turns, and
// drawing only the first N of them would show a boundary that stops partway
// round, which is a worse lie than a slow frame. If that ever becomes the
// bottleneck the answer is to simplify the contour at the current zoom, not to
// truncate it.
void drawMarchingAnts(ImDrawList* dl, const ViewTransform& xform,
                      const SelectionBoundary& boundary) {
  float phase = marchingAntPhase();
  std::vector<Vec2> pts;
  for (const BoundaryContour& contour : boundary.contours) {
    pts.clear();
    pts.reserve(contour.vertices.size());
    for (const BoundaryVertex v : contour.vertices)
      pts.push_back(Vec2{static_cast<float>(v.x), static_cast<float>(v.y)});
    drawAntPolyline(dl, xform, pts, /*closed=*/true, phase);
  }
}

// The in-progress gesture's preview: an axis-aligned rectangle in document
// texel space, drawn from the drag's own corners.
//
// **Deliberately a separate entry point from the one above**, and not a
// degenerate case of it. This draws a shape that has no `Selection` behind it
// yet -- the rubber band exists only between mouse-down and mouse-up -- and
// building a real selection per frame to outline it would allocate tiles 120
// times a second to answer a question four floats already answer. The two must
// not be confused in the other direction either: once the gesture commits, the
// thing on screen is the selection's own boundary and not the drag rectangle.
void drawMarchingAnts(ImDrawList* dl, const ViewTransform& xform, float x0, float y0,
                      float x1, float y1) {
  float phase = marchingAntPhase();
  const std::vector<Vec2> pts = {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}};
  drawAntPolyline(dl, xform, pts, /*closed=*/true, phase);
}

// docs/testing-issues.md T13 ("The ellipse marquee draws a rectangle while
// you drag it"): the SAME kind of preview as the overload just above, for
// the SAME box, but the ellipse it is inscribed in rather than its four
// corners -- so the ellipse marquee's live rubber band finally draws the
// shape mouse-up commits instead of always the bounding box every tool
// used to share.
//
// A distinct overload rather than a flag on the one above: the extra
// `segments` argument is what forces the caller to pick, at compile time,
// which shape it means -- a boolean would let a call site silently mean
// the wrong one and still compile. The points themselves are not built
// here: app/SelectionDrag.hpp's `ellipseMarqueePreviewPoints()` is the same
// function app/selftest/EllipseMarqueePreview.cpp calls to assert this
// preview agrees with what `case Tool::EllipseMarquee:`'s commit arm
// builds, so there is exactly one place this shape's arithmetic lives.
void drawMarchingAnts(ImDrawList* dl, const ViewTransform& xform, float x0, float y0,
                      float x1, float y1, int segments) {
  float phase = marchingAntPhase();
  const std::vector<Vec2> pts = ellipseMarqueePreviewPoints(x0, y0, x1, y1, segments);
  drawAntPolyline(dl, xform, pts, /*closed=*/true, phase);
}

// The resolution the ellipse preview above is walked at. 32, matching the
// brush cursor ring a few hundred lines down (`dl->AddCircle(mouse, ...,
// 32, ...)`) -- this codebase's own existing precedent for a circular
// canvas-space overlay drawn at whatever zoom the view happens to be at,
// reused here rather than picked afresh.
constexpr int kEllipseMarqueePreviewSegments = 32;

// One guide, drawn across the full canvas extent (0..texW or 0..texH) --
// not an unbounded "infinite" line -- through the same ViewTransform as
// everything else. Bounding it to the canvas extent is a deliberate
// simplification: this app has no pasteboard-beyond-the-canvas concept for
// a line to usefully extend into.
void drawGuideLine(ImDrawList* dl, const ViewTransform& xform, GuideOrientation orientation,
                   float position, float texW, float texH, ImU32 col) {
  Vec2 a, b;
  if (orientation == GuideOrientation::Horizontal) {
    a = xform.toScreen(Vec2{0.0f, position});
    b = xform.toScreen(Vec2{texW, position});
  } else {
    a = xform.toScreen(Vec2{position, 0.0f});
    b = xform.toScreen(Vec2{position, texH});
  }
  dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
}

// Screen-space point-to-segment distance, for right-click-to-delete guide
// picking below -- a guide can be dragged/rotated on screen (mirror,
// rotation), so "distance to the line's on-screen endpoints" is the only
// correct hit test, not a document-space one.
float distancePointToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
  const ImVec2 ab(b.x - a.x, b.y - a.y);
  const float lenSq = ab.x * ab.x + ab.y * ab.y;
  float t = lenSq > 1e-6f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lenSq : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const ImVec2 proj(a.x + ab.x * t, a.y + ab.y * t);
  const float dx = p.x - proj.x, dy = p.y - proj.y;
  return std::sqrt(dx * dx + dy * dy);
}

// PLAN.md Phase 3 step 8 ("Op-stack UI -- reorder, toggle, delete, and a
// curve widget operating in the shaper domain"). ---------------------------

// An op's name is `core::opDisplayName()` -- this file's private copy of that
// switch was deleted by UI detour step 3, which gave core/LayerOps the same
// question to answer for a journal label. Two copies would have been a row
// saying "Curves" and an undo entry saying something else about the same op.

// The one PLAN.md step 8 names explicitly ("a curve widget operating in the
// shaper domain"). ImGui glue around app/CurveEdit.hpp's pure geometry/
// list-mutation helpers -- that header's own doc comment: "the plot, the
// click/drag/right-click handling and the spline draw itself are UI...
// everything here is what that UI calls into." Edits `curve` in place;
// returns true on any frame it actually changed (a point added, moved or
// removed), which the caller uses to decide whether to write the containing
// Op back through OpStack::setOp() -- this function never touches OpStack
// itself, matching every other per-kind editor in drawGradeSection() below.
//
// Axes are plain [0,1]x[0,1] shaper-domain space (ADR-0004) -- Curve control
// points are already shaper-domain coordinates by contract, so nothing here
// calls color::shaperEncode/Decode; see app/CurveEdit.hpp's own header
// comment for why plotting needs no colour-domain conversion at all.
// `plotSize` is a parameter rather than a constant because the LINK editor in
// the BRUSH SETTINGS panel (design turn 4a) draws the same widget at 104 px in
// a 322 px column, and that design's note is explicit that it is the same
// widget as the grading stack's -- so it is this function at another size, not
// a second implementation that would drift.
//
// **External linkage, breaking out of this file's anonymous namespace for
// this one function** -- the identical reason `ctlSlider()` above does, and
// for the identical caller: `ui/DynamicsMatrixPanel.cpp`'s `drawLinkEditor()`
// draws its own 104 px curve plot with this exact function, per this
// function's own comment above ("the grading stack's own widget rather than
// a second one"). Reopened immediately after this function's closing brace.
}  // namespace

bool drawCurveWidget(Curve& curve, float plotSize = 200.0f) {
  const float kPlotSize = plotSize;
  constexpr float kHitRadiusPx = 8.0f;
  constexpr int kSamples = 64;

  bool changed = false;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  // Scoped by the caller's own PushID(opIndex) further up the ID stack, so
  // each Curves op in the stack keeps its own drag state independently.
  const ImGuiID dragKey = ImGui::GetID("curveDragIdx");
  int dragIdx = storage->GetInt(dragKey, -1);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 plotMax(origin.x + kPlotSize, origin.y + kPlotSize);
  dl->AddRectFilled(origin, plotMax, IM_COL32(20, 20, 22, 255));

  // The spline itself, sampled across shaper-domain x via the same
  // evalCurve() the grading pipeline (ops/PointOps.cpp, color/LutBake) runs
  // -- the plotted line is never a separate approximation of what grading
  // will actually do.
  ImVec2 prevPt{};
  for (int s = 0; s <= kSamples; ++s) {
    const float cx = static_cast<float>(s) / static_cast<float>(kSamples);
    const float cy = evalCurve(curve, cx);
    float px = 0.0f, py = 0.0f;
    curveToPlot(cx, cy, kPlotSize, px, py);
    const ImVec2 pt(origin.x + px, origin.y + py);
    if (s > 0) dl->AddLine(prevPt, pt, IM_COL32(255, 200, 90, 255), 1.5f);
    prevPt = pt;
  }
  // **Two endpoint knots are always shown and draggable, even before `curve`
  // has any real control points.** `evalCurve()`'s degenerate-case contract --
  // fewer than 2 points is identity (ops/PointOps.hpp) -- is what
  // `applyCurves()` relies on to skip the shaper encode/decode round trip
  // entirely for a genuinely untouched channel, landing an EXACT no-op rather
  // than one only correct to float tolerance; that has to stay intact for
  // grading, so this is display/hit-test-only synthesis, never a write to
  // `curve` by itself. The plotted SPLINE already draws correctly with zero
  // real points (evalCurve returns `x` unchanged, a flat diagonal); it was
  // only the KNOT DOTS that were missing before a first point existed, and
  // still wrong-looking with exactly one (a dot with no bend in the line
  // through it) -- this is that, not a change to what gets graded.
  const bool synthesizeEndpoints = curve.size() < 2;
  const size_t dotCount = synthesizeEndpoints ? 2 : curve.size();
  for (size_t i = 0; i < dotCount; ++i) {
    const CurvePoint pt = synthesizeEndpoints
                              ? CurvePoint{static_cast<float>(i), static_cast<float>(i)}
                              : curve[i];
    float px = 0.0f, py = 0.0f;
    curveToPlot(pt.x, pt.y, kPlotSize, px, py);
    dl->AddCircleFilled(ImVec2(origin.x + px, origin.y + py), 4.0f,
                        IM_COL32(240, 240, 235, 255));
  }
  dl->AddRect(origin, plotMax, ImGui::GetColorU32(ImGuiCol_Border));

  ImGui::InvisibleButton("##curvePlot", ImVec2(kPlotSize, kPlotSize));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float mx = mouse.x - origin.x;
  const float my = mouse.y - origin.y;

  // Click-down: grab an existing point within radius, or plant a new one at
  // the click location and grab that -- mirrors this file's own
  // st.pendingGuide "click-down starts a drag, IsMouseDown continues it"
  // idiom (see the canvas block's ruler drag-to-create above) rather than a
  // from-scratch input pattern. Capped at kMaxCurvePointsPerChannel
  // (color/LutBake.hpp) -- color::LutBake's GPU kernel truncates any longer
  // curve to its first 16 points when baking (with a stderr warning), so
  // letting the widget accept more here would let the plotted spline
  // silently diverge from what grading actually bakes; existing points
  // beyond the cap (none can exist, since insertion is capped) would still
  // be movable/deletable if they somehow did.
  if (ImGui::IsItemActivated()) {
    // The two synthetic endpoint dots drawn above are not real points until
    // the user actually touches this widget -- promote them here, once, on
    // the first activation while `curve` is still degenerate (fewer than 2
    // points). From this line on `curve` is an honest 2-point identity curve
    // and behaves exactly like any other; the hit-test and click below still
    // resolve normally against it, whether the click landed on one of these
    // two (a grab) or elsewhere (adding a third real point, same as always).
    if (curve.size() < 2) {
      curve.assign({CurvePoint{0.0f, 0.0f}, CurvePoint{1.0f, 1.0f}});
      changed = true;
    }
    const auto hit = hitTestPoint(curve, mx, my, kPlotSize, kHitRadiusPx);
    if (hit) {
      dragIdx = static_cast<int>(*hit);
    } else if (curve.size() < static_cast<size_t>(kMaxCurvePointsPerChannel)) {
      float cx = 0.0f, cy = 0.0f;
      plotToCurve(mx, my, kPlotSize, cx, cy);
      dragIdx = static_cast<int>(insertPoint(curve, cx, cy));
      changed = true;
    }
    storage->SetInt(dragKey, dragIdx);
  }
  if (dragIdx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    float cx = 0.0f, cy = 0.0f;
    plotToCurve(mx, my, kPlotSize, cx, cy);
    dragIdx = static_cast<int>(movePoint(curve, static_cast<size_t>(dragIdx), cx, cy));
    storage->SetInt(dragKey, dragIdx);
    changed = true;
  } else if (dragIdx >= 0) {
    dragIdx = -1;
    storage->SetInt(dragKey, dragIdx);
  }

  // Right-click deletes the point under the cursor, if any -- PLAN.md step
  // 8's own "double-click (or right-click, your call)" wording, decided in
  // favour of right-click: a double-click's second click also fires as an
  // ordinary left click one event earlier (Dear ImGui's own documented
  // behaviour -- "note that a double-click will also report
  // IsMouseClicked() == true"), which would insert-then-immediately-delete
  // a point when double-clicking empty plot area under the click-to-add
  // handler just above. Right-click has no such overlap with the left-click
  // gestures, so it needs no special-casing around it.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const auto hit = hitTestPoint(curve, mx, my, kPlotSize, kHitRadiusPx);
    // Never drop below 2 real points via delete -- the same invariant the
    // first activation above establishes. Below 2, `evalCurve()`'s degenerate
    // case takes over and the plotted knot dot(s) stop matching what the
    // spline actually does (see the comment above the dot-drawing loop):
    // exactly the bug this whole change exists to close, so a delete cannot
    // reopen it by taking the curve back under 2.
    if (hit && curve.size() > 2) {
      removePoint(curve, *hit);
      changed = true;
      // Indices may have shifted under the removal -- abandon any
      // in-progress drag rather than risk moving the wrong point next frame.
      dragIdx = -1;
      storage->SetInt(dragKey, dragIdx);
    }
  }

  // Separate from `hovered`, which also gates the right-click delete above
  // -- delaying that shared bool would delay the delete gesture itself, not
  // just the tooltip.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("Click empty area: add point\nDrag a point: move it\n"
                      "Right-click a point: delete it");

  ImGui::TextDisabled("%d / %d points", static_cast<int>(curve.size()),
                      kMaxCurvePointsPerChannel);
  return changed;
}

namespace {

// --- One op stack, edited ------------------------------------------------
//
// The rest of PLAN.md step 8 (add / list / toggle / reorder / delete, plus the
// per-kind inline editors, deliberately minimal outside Curves), generalised
// by UI detour step 3 over **which** stack it is editing. Two are:
//
//   * `AppState::opStack` -- the session-level grading preview in the GRADE
//     section. UI state; mutated directly, and undo knows nothing about it.
//   * `Layer::ops` -- a layer's own non-destructive stack (PLAN.md Phase 5
//     steps 3 and 5, composited by core/Composite for every kind and the
//     entire content of an Adjustment layer). *Document* state; every mutation
//     goes through core/LayerOps and `app::recordLayerEdit()`, so it lands in
//     history and moves the revision ui/DocumentTexture caches the composite
//     by. A layer op edited around that funnel would not reach the screen.
//
// Those two mutation paths are the whole difference between the two, so they
// are what the caller binds, and every row, every params editor and the curve
// widget are written once. The alternative -- a second copy of this loop for
// layers -- would have been a second curve widget inside a week.
struct OpStackBinding {
  const OpStack* stack = nullptr;
  std::function<void(Op)> add;
  std::function<void(size_t)> remove;
  std::function<void(size_t, size_t)> move;
  std::function<void(size_t, bool)> setEnabled;
  std::function<void(size_t, Op)> setOp;
};

void drawOpStackEditor(const OpStackBinding& bound) {
  const OpStack& stack = *bound.stack;

  // 1. Add -- PLAN.md step 8 item 1. The new op's shape (class PointA, and
  // **disabled**, so adding one never changes what is on screen) is
  // `app::makeNewOp()`, which is now the single home of that rule for both
  // stacks.
  //
  // The chosen kind lives in ImGui's ID-scoped storage rather than in a
  // `static` local, because this function draws more than one stack per frame
  // now -- a `static` would make the GRADE combo and the selected layer's
  // combo the same combo.
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID kindKey = ImGui::GetID("newOpKind");
  int newOpKindIdx = storage->GetInt(kindKey, 0);
  const char* kKindNames[] = {"Levels", "Curves", "Exposure",
                              "Saturation", "Grayscale", "Channel Mixer"};
  const float addW = ImGui::CalcTextSize("+ Add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SetNextItemWidth(std::max(
      80.0f, ImGui::GetContentRegionAvail().x - addW - ImGui::GetStyle().ItemSpacing.x));
  if (ImGui::Combo("##newOpKind", &newOpKindIdx, kKindNames, IM_ARRAYSIZE(kKindNames)))
    storage->SetInt(kindKey, newOpKindIdx);
  ImGui::SameLine();
  if (ImGui::Button("+ Add")) bound.add(makeNewOp(static_cast<PointOpKind>(newOpKindIdx)));

  // 2/3. List, one row per op in stack order, each with an enable checkbox,
  // a kind label, up/down/delete, and (for Curves) the widget above.
  //
  // `op` is a *copy* of stack.at(i), not a reference -- reorder()/remove()
  // erase-and-insert into OpStack's internal std::vector<Op>, which can
  // invalidate references to later elements; holding a reference across one of
  // those calls and then reading it again in the same iteration (e.g. the
  // per-kind editor below) would be exactly that bug.
  // `structureChanged` stops the loop the same frame a reorder/remove
  // fires, rather than continuing to render rows against indices that no
  // longer mean what they did a moment ago -- the list simply reflects the
  // new state from the next frame on, standard immediate-mode practice. It is
  // set whether or not the mutation succeeded: a refused layer-op edit changed
  // nothing, and re-rendering the rest of the rows this frame would say so no
  // more clearly than the refusal line does.
  bool structureChanged = false;
  for (size_t i = 0; i < stack.size() && !structureChanged; ++i) {
    ImGui::PushID(static_cast<int>(i));
    Op op = stack.at(i);

    bool enabled = op.enabled;
    // Must go through the binding, never `op.enabled = ...` directly: `op`
    // here is a local copy, so a direct mutation wouldn't even reach the live
    // stack, and OpStack doesn't expose a non-const reference to mutate in
    // place either way -- setEnabled() is the only path that bumps version(),
    // which updateGradePreview()'s rebake gate depends on entirely.
    if (ImGui::Checkbox("##en", &enabled)) bound.setEnabled(i, enabled);
    ImGui::SameLine();
    ImGui::TextUnformatted(opDisplayName(op).c_str());

    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("Up")) {
      bound.move(i, i - 1);
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 >= stack.size());
    if (ImGui::SmallButton("Down")) {
      bound.move(i, i + 1);
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
      bound.remove(i);
      structureChanged = true;
    }

    // 3. Per-kind inline editor -- deliberately minimal, not exhaustive
    // polish (see each case's own comment for the specific scope cut).
    // Skipped once structureChanged is true: `op` may already be stale
    // (see the comment above the loop), and the row is about to disappear
    // from the very next frame regardless.
    if (!structureChanged) {
      ImGui::Indent();
      bool changed = false;
      switch (op.opClass == OpClass::PointA ? op.pointKind : PointOpKind::Levels) {
        case PointOpKind::Exposure:
          // Linear-light stops (ops/PointOps.hpp's ExposureParams -- the
          // one op deliberately NOT in the shaper domain). +-5 stops is a
          // generously wide but ordinary editing range.
          changed = ctlSlider("Stops", &op.exposure.stops, -5.0f, 5.0f);
          break;
        case PointOpKind::Saturation:
          // lumaWeights stays at kRec709LumaWeights -- not exposed in this
          // narrow scope, matching Grayscale's own cut below for the same
          // shared weight.
          changed = ctlSlider("Scale", &op.saturation.scale, 0.0f, 2.0f);
          break;
        case PointOpKind::Levels: {
          // A non-PointA entry lands here too (an op a newer build wrote,
          // carried verbatim by PRD I10) and must not be offered an editor at
          // all: its params fields are meaningless and writing one back would
          // turn a preserved op into a fabricated one. It is named by
          // `opDisplayName()` in the row above and left alone.
          if (op.opClass != OpClass::PointA) {
            ImGui::TextDisabled("(this build cannot edit this op -- it is carried\n"
                                "through unchanged, PRD I10)");
            break;
          }
          // ONE shared LevelsParams editor applied identically to all
          // three levels[0..2] entries on any change -- PLAN.md step 2's
          // own wording: "a composite levels adjustment is just the caller
          // passing the same LevelsParams for all three channels, not a
          // separate code path." Per-channel authoring is out of this
          // narrow scope.
          LevelsParams p = op.levels[0];
          bool ch = false;
          ch |= ctlSlider("Black in", &p.blackIn, 0.0f, 1.0f);
          ch |= ctlSlider("White in", &p.whiteIn, 0.0f, 1.0f);
          ch |= ctlSlider("Gamma", &p.gamma, 0.1f, 4.0f);
          ch |= ctlSlider("Black out", &p.blackOut, 0.0f, 1.0f);
          ch |= ctlSlider("White out", &p.whiteOut, 0.0f, 1.0f);
          if (ch) {
            op.levels[0] = op.levels[1] = op.levels[2] = p;
            changed = true;
          }
          break;
        }
        case PointOpKind::Grayscale:
          // No weight editor in this narrow scope -- default
          // kRec709LumaWeights only. Fully functional either way: add/
          // toggle/reorder/delete all work, and it bakes and grades
          // correctly at the default weights. A per-channel weight editor
          // is real, separate scope PLAN.md step 8's literal wording (it
          // names only "a curve widget," singular) doesn't call for.
          ImGui::TextDisabled("(default Rec.709 weights -- no editor in this scope)");
          break;
        case PointOpKind::ChannelMixer:
          // Same treatment as Grayscale immediately above, for the same
          // reason -- a 12-value 3x4 matrix editor is real, separate scope.
          ImGui::TextDisabled("(identity matrix -- no matrix editor in this scope)");
          break;
        case PointOpKind::Curves: {
          // The one PLAN.md step 8 explicitly calls out. Channel tabs
          // (R/G/B) pick which of curves[0..2] the widget above shows/
          // edits; selection persists per-op-row via ImGui's own ID-scoped
          // state storage (GetStateStorage()), not a new AppState field --
          // this is UI-only state, exactly like drawCurveWidget()'s own
          // drag-index storage right above it.
          const ImGuiID chKey = ImGui::GetID("curveChannel");
          int channel = storage->GetInt(chKey, 0);
          const char* chNames[3] = {"R", "G", "B"};
          for (int c = 0; c < 3; ++c) {
            if (c > 0) ImGui::SameLine();
            const bool sel = channel == c;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(chNames[c])) {
              channel = c;
              storage->SetInt(chKey, c);
            }
            if (sel) ImGui::PopStyleColor();
          }
          if (drawCurveWidget(op.curves[static_cast<size_t>(channel)])) changed = true;
          break;
        }
      }
      if (changed) bound.setOp(i, op);
      ImGui::Unindent();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
  }
  if (stack.size() == 0) ImGui::TextDisabled("(no ops)");
}

// The GRADE section: the session-level preview toggle, and the same op-stack
// editor every other stack gets. `AppState::opStack` is UI state, so these
// mutations are direct and are deliberately not recorded -- nothing about the
// document changed, and a "grade preview" entry in a document's undo history
// would be a lie about what undo would take back.
void drawGradeSection(AppState& st) {
  ImGui::Checkbox("Preview Graded Output", &st.view.grade);
  ImGui::SetItemTooltip("Shows the canvas through the op stack below\n"
                      "(PLAN.md Phase 3's grading pipeline). Off by\n"
                      "default, the same as Grayscale Preview above --\n"
                      "grading never turns on just because the stack\n"
                      "below is non-empty.\n"
                      "This stack grades the PAINTING canvas and is not\n"
                      "part of the document; a layer's own op stack is in\n"
                      "LAYERS, under the selected layer.");

  OpStackBinding bound;
  bound.stack = &st.opStack;
  bound.add = [&st](Op op) { st.opStack.add(std::move(op)); };
  bound.remove = [&st](size_t i) { st.opStack.remove(i); };
  bound.move = [&st](size_t from, size_t to) { st.opStack.reorder(from, to); };
  bound.setEnabled = [&st](size_t i, bool on) { st.opStack.setEnabled(i, on); };
  bound.setOp = [&st](size_t i, Op op) { st.opStack.setOp(i, std::move(op)); };
  drawOpStackEditor(bound);
}

// ---------------------------------------------------------------- Histogram
//
// C2 (docs/reachability-audit.md; PRD D2, P0): core/Histogram.hpp's
// `computeHistogram()` was a built, tested, read-only Document query with
// exactly one caller -- `--selftest` -- and no way for a person using the
// application to ever see it. This is that caller. Four overlaid channel
// distributions (R, G, B, Luma), drawn with plain ImGui-draw-list rectangles
// exactly like drawCurveWidget()'s plot above, over the whole open document's
// composite.
//
// **The cost, and why it does not recompute every frame.** `computeHistogram
// ()` walks every allocated tile of every populated layer (Histogram.cpp's
// `binTileRegion()`) -- proportional to what is actually resident, per that
// module's own header, but still real, size-of-the-document work, and this
// function runs every frame the header is open. Recomputing it every frame
// would make merely leaving HISTOGRAM open a standing per-frame cost with
// nothing on the canvas having changed, which is exactly the "large document
// recomputed per frame" bug this step's brief calls out by name.
//
// So the `HistogramResult` is cached across frames, keyed on
// `(OpenDocument::id, OpenDocument::revision)` -- the identical cache-key
// idiom `core/DirtyTiles.hpp`'s own header describes and `ui::DocumentTexture`
// already uses for the same reason (app/SelfTest.hpp's "the cache key"
// section: "Keyed on (id, revision, width, height); the collision only
// matters..."). `revision` is bumped by `recordEdit()` and nothing else
// (app/DocumentLifecycle.hpp) -- a brush stroke that has not yet baked into a
// `Layer` does not move it, so the panel shows the last-baked state until the
// next bake, the same staleness every other document-reading panel in this
// column (LAYERS' thumbnails, COMPS) already accepts between bakes. "Has the
// document actually changed since I last computed this?" becomes an O(1)
// integer compare instead of a per-frame recompute.
//
// Drawing the cached 256-bin result costs up to 4 * 256 = 1024
// `AddRectFilled()` calls per open frame -- the same order of magnitude as
// the brush grid's icon draw (~7751 below) and far cheaper than the tile walk
// it replaces; it is not itself cached because it is already this cheap.
void drawHistogramSection(AppState& st) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    return;
  }

  static DocumentId cachedId = 0;
  static uint64_t cachedRevision = 0;
  static bool cachedValid = false;
  static HistogramResult cached;

  // **Not while a stroke is live**, and this is the difference between a
  // panel and a frame-rate bug. `computeHistogram()` walks every allocated
  // tile: measured at **12.9 ms** on `--demo-document`'s 1024x1024, which is
  // most of a 60 Hz frame on its own and grows with the document. And
  // `app/StrokeSession.cpp:938` bumps `revision` on *every frame that
  // deposits tiles*, not once per finished stroke -- so keying the cache on
  // `revision` alone would recompute the whole document on every frame of
  // every stroke, for a panel the user is not looking at while painting.
  //
  // Holding the previous result for the duration of the stroke costs a
  // histogram that lags the wet edge by one stroke, which is what every
  // editor does anyway, and `g_stroke.active()` going false on mouse-up
  // brings it straight back into step.
  const bool strokeLive = g_stroke.active();
  if (!strokeLive &&
      (!cachedValid || cachedId != od->id || cachedRevision != od->revision)) {
    HistogramParams params = HistogramParams::wholeDocument(od->document);
    // The composite, not just the active layer -- Histogram.hpp's own header
    // names the one place this diverges from real compositing (a pixel
    // covered by two layers would double-count) and why it was left that way:
    // today's Document invariant is at most one populated RGB layer (the same
    // invariant core/Histogram.cpp's `sampleAllLayers` branch comment states),
    // so the plain per-layer sum this module does is exactly correct now.
    params.sampleAllLayers = true;
    cached = computeHistogram(od->document, params);
    cachedId = od->id;
    cachedRevision = od->revision;
    cachedValid = true;
  }

  ImGui::TextDisabled("%llu sample(s) over %d bins",
                      static_cast<unsigned long long>(cached.sampleCount),
                      static_cast<int>(cached.r.size()));

  if (cached.sampleCount == 0 || cached.r.empty()) {
    ImGui::TextDisabled("Nothing painted yet.");
    return;
  }

  uint64_t maxCount = 1;
  for (size_t i = 0; i < cached.r.size(); ++i)
    maxCount = std::max({maxCount, cached.r[i], cached.g[i], cached.b[i], cached.luma[i]});

  const float plotW = ImGui::GetContentRegionAvail().x;
  const float plotH = 120.0f;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + plotW, origin.y + plotH),
                    IM_COL32(20, 20, 22, 255));

  // Overlaid, back-to-front, each channel translucent -- the same
  // stacked-alpha convention every mainstream editor's RGB histogram overlay
  // uses (Histogram.hpp's own header cites Photoshop for the same reasoning
  // about the domain), so a reader who has seen one before reads this one the
  // same way. Luma drawn last and more opaque, as the summary line over the
  // three colour channels.
  auto plotChannel = [&](const std::vector<uint64_t>& bins, ImU32 col) {
    const size_t n = bins.size();
    for (size_t i = 0; i < n; ++i) {
      const float x0 = origin.x + plotW * (static_cast<float>(i) / static_cast<float>(n));
      const float x1 = origin.x + plotW * (static_cast<float>(i + 1) / static_cast<float>(n));
      const float h = plotH * (static_cast<float>(bins[i]) / static_cast<float>(maxCount));
      dl->AddRectFilled(ImVec2(x0, origin.y + plotH - h),
                        ImVec2(std::max(x1, x0 + 1.0f), origin.y + plotH), col);
    }
  };
  plotChannel(cached.r, IM_COL32(235, 70, 70, 110));
  plotChannel(cached.g, IM_COL32(70, 215, 110, 110));
  plotChannel(cached.b, IM_COL32(80, 150, 235, 110));
  plotChannel(cached.luma, IM_COL32(235, 235, 230, 150));

  dl->AddRect(origin, ImVec2(origin.x + plotW, origin.y + plotH),
              ImGui::GetColorU32(ImGuiCol_Border));
  ImGui::Dummy(ImVec2(plotW, plotH));

  ImGui::TextColored(ImVec4(0.92f, 0.30f, 0.30f, 1.0f), "R");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.45f, 1.0f), "G");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.35f, 0.60f, 0.92f, 1.0f), "B");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(0.92f, 0.92f, 0.90f, 1.0f), "Luma");
}

// ------------------------------------------------------------------ Layers
//
// PLAN.md Phase 5 step 1's panel, following docs/ui.md §3.2's layer row: a
// kind glyph, the name, and a monospace sub-line reading `RGB · NORMAL · 100%`.
//
// Same split as drawCurveWidget()/app/CurveEdit and drawExportAsDialog()/
// io/ExportAs: everything that can be wrong here without a screenshot showing
// it lives outside this function and is exercised headlessly by `--selftest`.
// Specifically:
//
//  * **Which layer a row is** -- app/LayerPanel's `layerIndexForPanelRow()`.
//    The panel lists top-first while `Document::layers` is bottom-first, and
//    that reversal happens in exactly one place, in a pure function with a
//    test, because a second reversal in this draw loop is precisely how "Up"
//    ends up moving a layer down.
//  * **What a row says** -- app/LayerPanel's `layerRowTitle()` /
//    `layerRowSubLine()` / `layerKindGlyph()`.
//  * **What a button does** -- core/LayerOps' add/remove/move/duplicate and
//    the property setters, which are also where `locked` is enforced. This
//    function never touches `doc.layers` directly, so the lock cannot be
//    bypassed by a widget; a refusal comes back as the operation's own
//    sentence and is shown verbatim rather than reworded.
//  * **Dirty and journal tracking** -- app/DocumentLifecycle's
//    `recordLayerEdit()`, which is what makes every change here a structural
//    edit (PRD O5) rather than something the journal finds out about on its
//    next timer tick.
//
// --- Which half is visible, stated plainly --------------------------------
//
// **These layers are on the canvas**, as of the UI detour's step 2.
// ui/DocumentTexture composites this document and the canvas block draws it
// over the paper, so toggling a visibility box, dragging an opacity, changing
// a blend, adding a mask, grading through an adjustment layer or clipping one
// layer to another changes what is on screen while you watch. Every sentence
// this comment used to carry about a panel that "never shows what is on
// screen" is deleted rather than softened, because it is no longer true.
//
// **The paint is still not one of these layers.** `sim::PaintSim` owns a
// single dense GPU texture with no layer or document awareness; a stroke
// writes that texture and touches no `Layer::rgbTiles` anywhere. So the two
// pictures are *stacked* -- document over paper -- and not merged: a document
// that has been painted on still holds none of the paint. Closing that gap is
// the stroke bridge, a later step of this detour, and the panel says which
// half is which on screen rather than leaving a user to infer it.
//
// There are still no thumbnails. The reason used to be that a thumbnail of a
// document the canvas could not reach would imply a connection that did not
// exist; now it is only that the canvas shows the composite itself, at full
// size, which is a better thumbnail than a thumbnail.
//
// **The blend dropdown arrived with PLAN.md Phase 5 step 2** and it holds no
// list of its own. Which modes exist, which of them may be offered on *this*
// layer (PRD L5) and what each entry reads (PRD B7's display-referred label)
// all come from app/LayerPanel's `blendMenuForLayer()` / `blendMenuEntryText()`
// / `blendMenuSelection()`, which are pure and tested headlessly; this function
// only draws them and hands the chosen mode to `core::setLayerBlend()`. There
// is deliberately no string literal naming a blend mode anywhere in this file.
// The row also *displays* the blend it carries, marked `(!)` when this build
// cannot composite it.
//
// **UI detour step 3 made five built features reachable from here**: a Pigment
// layer, an Adjustment layer, add/remove mask, a layer's own op stack, and the
// Clip checkbox against a composite that now shows what it does. The gestures
// themselves -- what each one does, where the selection lands, when a command
// is offerable at all -- are app/LayerEditor's, shared with the `Layer` menu,
// and that header carries the argument. What is left here is the chrome.
//
// The kind glyph docs/ui.md §3.2 asks for, or an ASCII stand-in when the font
// cannot draw it.
//
// **No font file is loaded anywhere in this project** -- ImGui's built-in
// ProggyClean is ASCII-only -- so every one of app/LayerPanel's glyphs
// (`◉ □ ▤ ◈ ✂ ▩`) renders as ImGui's `?` fallback and the glyph column says
// nothing at all about the kind. Discovered by photographing the panel, which
// is the whole reason this step's screenshots exist.
//
// The stand-in is the kind name's initial in brackets, from
// `core::layerKindName()`, so it needs no second table to fall out of date:
// `[R]`, `[P]`, `[A]`. The real glyph is used the moment a font that has it is
// loaded, because the test is against the font rather than against a build
// flag. This lives in ui/ and not in app/LayerPanel because "can the loaded
// font draw this" is a question only the renderer can answer -- `--selftest`
// checks the glyph, which is still what the panel asks for.
// The shared half of `layerKindGlyphForFont()` and `layerCommandGlyphForFont()`
// below: decode a glyph's first code point (ImGui's own UTF-8 decoder is
// internal API, and every glyph either table hands this is ASCII or a single
// 3-byte sequence) and ask the running font whether it can draw it. Returns
// `glyph` when it can, `fallback` when it cannot -- which keeps the decode
// logic in exactly one place rather than one copy per glyph table.
std::string glyphOrFallback(const char* glyph, const std::string& fallback) {
  const unsigned char* u = reinterpret_cast<const unsigned char*>(glyph);
  unsigned int cp = 0;
  if (u[0] < 0x80) {
    cp = u[0];
  } else if ((u[0] & 0xF0) == 0xE0 && u[1] != 0 && u[2] != 0) {
    cp = static_cast<unsigned int>((u[0] & 0x0F) << 12 | (u[1] & 0x3F) << 6 | (u[2] & 0x3F));
  }
  if (cp != 0 && ImGui::GetFont()->IsGlyphInFont(static_cast<ImWchar>(cp))) return glyph;
  return fallback;
}

std::string layerKindGlyphForFont(LayerKind kind) {
  const char* name = layerKindName(kind);
  return glyphOrFallback(layerKindGlyph(kind),
                         std::string("[") + static_cast<char>(std::toupper(name[0])) + "]");
}

// The same fallback idiom as `layerKindGlyphForFont()`, for the compact icon
// toolbar's own glyphs (`app::layerCommandGlyph()`). A short, hand-picked
// abbreviation rather than an initial: several of these commands share a
// first letter ("Duplicate" / "Delete", "Rasterise" / the RGB glyph's own
// `[R]`), which the kind table never has to worry about because it only ever
// draws three of these buttons.  Only reached on a machine with no usable
// glyph source at all (ui/Fonts.cpp), so legibility under that failure -- not
// brevity -- is what this table is for.
const char* layerCommandGlyphFallback(LayerCommand command) noexcept {
  switch (command) {
    case LayerCommand::NewRgbLayer: return "[R]";
    case LayerCommand::NewPigmentLayer: return "[P]";
    case LayerCommand::NewAdjustmentLayer: return "[A]";
    case LayerCommand::DuplicateLayer: return "[Dup]";
    case LayerCommand::DeleteLayer: return "[Del]";
    case LayerCommand::AddMask: return "[+Mask]";
    case LayerCommand::RemoveMask: return "[-Mask]";
    case LayerCommand::MergeDown: return "[MrgDn]";
    case LayerCommand::MergeVisible: return "[MrgVis]";
    case LayerCommand::StampVisible: return "[Stamp]";
    case LayerCommand::FlattenImage: return "[Flat]";
    case LayerCommand::RasteriseLayer: return "[Raster]";
    case LayerCommand::MoveLayerUp:
    case LayerCommand::MoveLayerDown:
    case LayerCommand::ToggleVisible:
    case LayerCommand::ToggleLocked:
    case LayerCommand::ToggleClipped:
    case LayerCommand::ToggleAlphaLock:
    case LayerCommand::CaptureComp:
      return "?";
  }
  return "?";
}

std::string layerCommandGlyphForFont(LayerCommand command) {
  return glyphOrFallback(layerCommandGlyph(command), layerCommandGlyphFallback(command));
}

// One button in the layers panel that issues a `LayerCommand`. Greyed out by
// `app::layerCommandAvailable()` -- the same predicate the `Layer` menu greys
// its items with -- and tooltipped with the command's own menu text, so an
// abbreviated button and the menu item it duplicates can never come to mean
// different things.
void layerCommandButton(AppState& st, LayerCommand command, const char* text) {
  const OpenDocument* od = st.documents.active();
  const bool available =
      od != nullptr && layerCommandAvailable(od->document, command, od->activeLayer);
  ImGui::BeginDisabled(!available);
  if (ImGui::SmallButton(text)) runLayerCommand(st, command);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", layerCommandLabel(command));
}

// The compact icon toolbar's own button: `layerCommandButton()`'s twin, same
// availability predicate, same tooltip, same `runLayerCommand()` dispatch --
// the icon is a second face on the identical command, never a second
// implementation of one. Only the visible glyph differs, from
// `layerCommandGlyphForFont()`; the ID is the glyph plus the command's own
// label so two buttons that ever drew the same fallback glyph still get
// distinct ImGui IDs.
void layerCommandIconButton(AppState& st, LayerCommand command) {
  const OpenDocument* od = st.documents.active();
  const bool available =
      od != nullptr && layerCommandAvailable(od->document, command, od->activeLayer);
  const std::string id = layerCommandGlyphForFont(command) + "##" + layerCommandLabel(command);
  ImGui::BeginDisabled(!available);
  if (ImGui::SmallButton(id.c_str())) runLayerCommand(st, command);
  ImGui::EndDisabled();
  ImGui::SetItemTooltip("%s", layerCommandLabel(command));
}

// `layerCommandGlyphFallback()`'s twin for the eight `LayerSetCommand`s the
// Multi-selection section draws as an icon row instead of a text button --
// only reached when `drawToolGlyph()` cannot draw the merged Lucide glyph
// (its own no-usable-glyph-source failure case, `ui/Fonts.cpp`'s
// installToolIconFont() degrading silently).
const char* layerSetCommandGlyphFallback(LayerSetCommand command) noexcept {
  switch (command) {
    case LayerSetCommand::AlignSelectionLeft: return "[L]";
    case LayerSetCommand::AlignSelectionCenterX: return "[CH]";
    case LayerSetCommand::AlignSelectionRight: return "[R]";
    case LayerSetCommand::AlignSelectionTop: return "[T]";
    case LayerSetCommand::AlignSelectionCenterY: return "[CV]";
    case LayerSetCommand::AlignSelectionBottom: return "[B]";
    case LayerSetCommand::DistributeHorizontally: return "[DH]";
    case LayerSetCommand::DistributeVertically: return "[DV]";
    default: return "?";
  }
}

// `layerCommandIconButton()`'s twin for `LayerSetCommand`, hand-drawn like
// `toolFlyoutRow()` rather than an `ImGui::SmallButton` with the glyph
// embedded in its text label -- `toolFlyoutRow()`'s own comment explains why:
// a merged Lucide glyph is only proven to bake at `kToolIconSizePx`
// (`drawToolGlyph()`'s fixed size, `ui/Fonts.hpp`), and drawing it as widget
// label text at whatever size the ambient font happens to be is a different,
// unproven bake -- in practice, at this panel's compact text size, the thin
// strokes of e.g. align-left/align-right collapse into near-identical blurs.
// Same availability predicate (`core::layerSetCommandAvailable()`), same
// tooltip, same `runLayerSetCommand()` dispatch as `layerCommandIconButton()`;
// only reachable for the eight commands `layerSetCommandIconCodepoint()`
// actually has a glyph for. `visible` is the selection already restricted to
// the active filter, computed once by the caller and threaded through rather
// than recomputed per button.
void layerSetCommandIconButton(AppState& st, const Document& doc, const LayerSelection& visible,
                                LayerSetCommand command) {
  const bool available = layerSetCommandAvailable(doc, command, visible);
  ImGui::PushID(static_cast<int>(command));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float sz = ImGui::GetFrameHeight();
  const ImVec2 size(sz, sz);
  const bool clickedRaw = ImGui::InvisibleButton("##align", size);
  const bool clicked = clickedRaw && available;
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = (hovered && available) ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                           : ImGui::GetColorU32(ImGuiCol_Button);
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg, ImGui::GetStyle().FrameRounding);

  const ImU32 fg = available ? ImGui::GetColorU32(ImGuiCol_Text)
                              : (atelierToken(kTextSecondary) & 0x00FFFFFFu) | IM_COL32(0, 0, 0, 110);
  const ImVec2 c(p.x + size.x * 0.5f, p.y + size.y * 0.5f);
  if (!drawToolGlyph(dl, layerSetCommandIconCodepoint(command), c, fg)) {
    const char* fb = layerSetCommandGlyphFallback(command);
    const ImVec2 ts = ImGui::CalcTextSize(fb);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), fg, fb);
  }
  // Separate from `hovered`, which also picks the button's fill above --
  // gating that on the tooltip's own stationary+delay timer would make the
  // button itself feel laggy to hover, not just its tooltip.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s", layerSetCommandLabel(command));
  if (clicked) runLayerSetCommand(st, command);
  ImGui::PopID();
}

// --- LAYERS (design "naturalPaint Panels" turn 2, option 2a) ----------------
//
// 2a's own summary of what it changes over 1a: "every row now shows an
// open-padlock in the slot the closed padlock occupies, so the slot reads as a
// control rather than as empty space; the three `New` buttons collapse into one
// `NEW` with a kind popup carrying all seven kinds and their rails; and that
// button merges into the existing command row."
//
// **This is a re-presentation, not a new feature.** Every value the panel draws
// was already produced by something that `--selftest` reaches: the metadata
// line is `app::layerRowSubLine()` unchanged (`(!)` on an unimplemented blend
// included), the filter is `app::LayerFilter`, the refusal sentences are
// `core/Merge`'s and `app/LayerEditor`'s verbatim, and every mutation still goes
// through `recordLayerEdit()`. What is new here is the geometry, the kind rail,
// the lock affordance and the `NEW` popup -- and the four *pure* pieces of that
// (the rail colours, the popup's seven entries, the link badge, the filter and
// count labels) live in app/LayerPanel, not here, so the suite can read them.
//
// --- What the design asks for that this does NOT draw, and why -------------
//
// The rule, in the brief's own words: anything the model cannot supply must not
// be drawn as though it can.
//
//  1. **The tab strip `LAYERS | CHANNELS | COMPS`.** Not drawn -- not even as
//     three inert words. The right-hand column's navigation idiom is already a
//     stack of collapsing headers and LAYERS, HISTORY and COMPS are each one of
//     them (`app::controlsSections()`); a tab strip inside the LAYERS section
//     whose COMPS tab switched panels would fight the COMPS section sitting two
//     headers below it, and one whose tabs did not switch would be three
//     labels pretending to be a control. The identical judgement is already
//     recorded one section down for turn 4a's COLOR/BRUSH/LAYERS strip
//     ("adopting the tab strip means restructuring LAYERS and COMPS too --
//     which is turns 1 and 3's redesign"); this is the other end of the same
//     sentence and it comes out the same way. What the strip's right-hand slot
//     carries -- the layer count -- *is* drawn, in the header band below,
//     because that is information rather than navigation.
//
//  2. **CHANNELS.** `core/Channels` landed recently (named coverage channels,
//     saved selections, quick mask) and has no UI at all -- no panel, no entry
//     point, nothing in `app/` that draws one. Building that tab is a panel of
//     its own, not a slice of this one, so it is left out entirely rather than
//     shipped as a tab that opens on an empty pane.
//
//  3. **`MEDIA:WATERCOLOUR - WET - 4.2s`.** Nothing in the model knows any part
//     of that line except the word MEDIA. `core::Layer` has no medium name and
//     no wet state: core/Layer.hpp says a Media layer "needs the fluid solver's
//     own per-medium state" and has none. Wetness exists, but it is
//     `sim::PaintSim`'s -- one dense canvas-wide field with no layer awareness
//     (`readTileOccupancy()` reports mass and wetness per *document tile*), and
//     there is no seconds-until-dry anywhere in the build. So a Media row reads
//     `MEDIA - NORMAL - 100%` from `layerRowSubLine()` like every other kind,
//     and the drying meter the design draws in the trailing slot is absent.
//
//  4. **`FLATS - 153 FILLS`.** A Flats layer has no fills. core/Merge.cpp says
//     it in as many words -- "a Flats layer no regions" -- and there is no fill
//     list, no count and no Fills panel. A Flats row reads `FLATS - NORMAL -
//     100%`.
//
//  5. **The `NEW` popup's shortcut column** (`SHIFT-CMD-N`, `SHIFT-CMD-R`).
//     `keymaps/default.json` binds no layer action to any key, and `CMD-N`
//     there is bound to `clear_canvas` -- so those labels would name keys that
//     do something else, or nothing. `app::newLayerShortcutsExist()` is the
//     assertion that pins this, and is what a later revision that wires them
//     will trip.
//
//  6. **One command row.** The design draws seven cells; this build offers
//     twelve commands plus Layer Properties, so the row below the list is two
//     rows. What 2a actually asked for -- that the three `New` buttons collapse
//     into one -- is done, and those two rows are 28 px shorter than the three
//     the panel had.
//
// The `(!)` marker is drawn, on exactly the modes `core::blendIsImplemented()`
// says are not composited: `normal`, `plus`, `multiply`, `screen`, `min`, `max`
// and `mix` are, so a `dissolve` arriving from a newer build (PRD I10) is
// the case that gets marked and nothing this build can set ever does.

// One row's geometry. Named rather than left as literals at the eight sites
// that read them, so a row and the header band above it cannot drift apart
// about where the leading controls end and the text column starts.
constexpr float kLayerRailW      = 3.0f;   // the kind rail down the leading edge
constexpr float kLayerClipIndent = 9.0f;   // the clipped row's bracket column
constexpr float kLayerRowPadX    = 6.0f;
constexpr float kLayerRowPadY    = 4.0f;
constexpr float kLayerRowGap     = 5.0f;   // between the leading controls
constexpr float kLayerLineGap    = 1.0f;   // name -> metadata line
constexpr float kLayerEyeW       = 14.0f;
constexpr float kLayerLockW      = 12.0f;
constexpr float kLayerMaskChipW  = 12.0f;
// The alpha-lock indicator's own chip -- a checkerboard, not a second padlock.
// `kLayerLockW` above already owns the padlock glyph two slots to the left of
// the name column, and drawing a SECOND padlock here for a different flag
// would read as "this layer is locked twice", not "locked a different way".
// A checkerboard is the transparency mark every image editor already uses for
// "no pixel here", so a checkerboard chip reads as "about alpha" on sight
// instead of borrowing the general lock's own glyph for a narrower promise.
constexpr float kLayerAlphaLockChipW = 12.0f;
constexpr float kLayerOpacityW   = 52.0f;
constexpr float kLayerGroupIndentW = 10.0f;  // per nesting level (PRD C7's UI half)
constexpr float kLayerDisclosureW  = 10.0f;  // the collapse/expand triangle, Group rows only

// The four shades the token table does not carry, declared here for
// `kMatrixColumnAlt`'s reason one section down: ui/AtelierTheme.hpp holds
// docs/ui.md section 1's twelve *role* tokens, and these are not roles -- they
// are option 2a's own row states and message fills, read off its markup.
// (The seventh thing 2a colours, the per-kind rail, is an identity rather than
// a state and lives with the kind in app/LayerPanel.)
constexpr uint32_t kLayerRowHover  = 0x353232;  // the hover wash on an unselected row
constexpr uint32_t kLayerLockRest  = 0x4f4c4c;  // the open padlock, at rest
constexpr uint32_t kLayerSelMeta   = 0xffd9d1;  // the metadata line on a selected row
constexpr uint32_t kLayerRefusalBg = 0x4a1207;  // the refusal band's fill
constexpr uint32_t kLayerWarnRail  = 0xd9c23d;  // the warning band's rail

// The eye and the padlock, drawn rather than set as glyphs.
//
// Two reasons, and the second is the load-bearing one. (a) It is already this
// file's idiom for icons -- `drawToolIcon()`'s own comment: "Icons are drawn
// rather than loaded so there is no asset to ship and they stay crisp at any
// DPI." (b) A glyph would need a codepoint in `requiredUiCodepoints()`, and
// that list is built by walking `layerKindGlyph()` and `layerCommandGlyph()`
// precisely so it cannot drift; hand-adding two padlocks to it would be adding
// the drift it exists to prevent, and would fail on a machine whose merge font
// happens not to carry them (ui/Fonts.hpp: Apple Symbols is already 6/7).
void drawEyeGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool open) {
  dl->AddEllipse(c, ImVec2(s * 0.50f, s * 0.30f), col, 0.0f, 0, 1.2f);
  dl->AddCircleFilled(c, s * 0.15f, col, 10);
  // eye-off: the almond stays, so the slot keeps its shape and the row's
  // leading edge does not reflow when a layer is hidden.
  if (!open)
    dl->AddLine(ImVec2(c.x - s * 0.50f, c.y + s * 0.42f),
                ImVec2(c.x + s * 0.50f, c.y - s * 0.42f), col, 1.4f);
}

// **The design's headline change.** The same slot, three states: an open
// padlock at rest, the open padlock at full weight on hover, the closed padlock
// on a locked row. The open one is what makes the slot read as a control rather
// than as the empty space eight unlocked rows used to leave there.
void drawPadlockGlyph(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool closed) {
  // `IM_PI` lives in imgui_internal.h, which this file does not include and
  // should not start including for one shackle. Screen y is down, so PI..2PI is
  // the half turn that sweeps *above* the padlock's body.
  constexpr float kPi = 3.14159265f;
  const float bw = s * 0.66f;
  const float bh = s * 0.42f;
  const float top = c.y + s * 0.06f;
  dl->AddRectFilled(ImVec2(c.x - bw * 0.5f, top), ImVec2(c.x + bw * 0.5f, top + bh), col);
  const float r = bw * 0.36f;
  if (closed) {
    dl->PathArcTo(ImVec2(c.x, top), r, kPi, kPi * 2.0f, 10);
    dl->PathStroke(col, 0, 1.3f);
  } else {
    // Hinged on the left, the free leg swung up and clear of the body on the
    // right -- Lucide's own `lock-open` reading, so a user who knows the icon
    // from anywhere else reads this one without being told.
    const ImVec2 hinge(c.x - bw * 0.20f, top);
    dl->PathArcTo(hinge, r, kPi, kPi * 1.72f, 10);
    dl->PathStroke(col, 0, 1.3f);
  }
}

// The clipping bracket in a clipped row's indent column: a hairline running the
// height of the row with a short foot at the bottom, pointing at the base layer
// below it. The row is indented by the same column, which is the standard
// editor reading of "this one belongs to that one" and is what the design
// draws.
void drawClipBracket(ImDrawList* dl, ImVec2 min, float w, float h, ImU32 col) {
  const float x = min.x + w * 0.45f;
  dl->AddLine(ImVec2(x, min.y), ImVec2(x, min.y + h), col, 1.0f);
  dl->AddLine(ImVec2(x, min.y + h - 1.0f), ImVec2(x + w * 0.5f, min.y + h - 1.0f), col, 1.0f);
}

// The collapse/expand triangle on a Group row (PRD C7's UI half): pointing
// right when collapsed (there is more here, closed), pointing down when
// expanded -- the same two states a disclosure triangle draws everywhere
// else, so nothing about it has to be learned.
void drawDisclosureTriangle(ImDrawList* dl, ImVec2 c, float s, ImU32 col, bool expanded) {
  const float r = s * 0.32f;
  if (expanded)
    dl->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f),
                          ImVec2(c.x, c.y + r * 0.7f), col);
  else
    dl->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r),
                          ImVec2(c.x + r * 0.7f, c.y), col);
}

// One line of text, clipped to a rectangle rather than wrapped or ellipsised.
// The panel is 322 px wide and a metadata line at the worst case
// (`ADJUSTMENT - NORMAL - 100% - 2 OPS - CLIPPED`) does not fit; clipping is
// what the design does and it keeps every row exactly the same height, which
// wrapping would not.
void drawClippedText(ImDrawList* dl, ImVec2 at, float maxW, ImU32 col, const char* text) {
  const ImVec4 clip(at.x, at.y - 1.0f, at.x + maxW, at.y + ImGui::GetFontSize() + 1.0f);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, col, text, nullptr, 0.0f, &clip);
}

// The design's opacity control: a 52 px meter filled to the value with the
// percentage right-aligned inside it, set by clicking or dragging anywhere
// across it. Absolute rather than relative, because that is what a meter means
// -- clicking three quarters of the way along sets 75%.
//
// Reports every frame of a drag, like the properties dialog's slider, so a
// locked layer refuses every one of them rather than only the first; coalescing
// an interaction into one undo entry is core/History's job, not this one's.
bool layerOpacityMeter(const char* id, float* v, float w, float h) {
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton(id, ImVec2(w, h));
  bool changed = false;
  if (ImGui::IsItemActive() && w > 0.0f) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - at.x) / w, 0.0f, 1.0f);
    if (t != *v) {
      *v = t;
      changed = true;
    }
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 lo(at.x, at.y), hi(at.x + w, at.y + h);
  dl->AddRectFilled(lo, ImVec2(at.x + w * std::clamp(*v, 0.0f, 1.0f), hi.y),
                    atelierToken(kChromeMid));
  dl->AddRect(lo, hi, atelierToken(kDivider), 0.0f, 0, 1.0f);
  char pct[16];
  std::snprintf(pct, sizeof pct, "%.0f%%", std::floor(static_cast<double>(*v) * 100.0 + 0.5));
  const ImVec2 sz = ImGui::CalcTextSize(pct);
  dl->AddText(ImVec2(hi.x - 3.0f - sz.x, at.y + (h - sz.y) * 0.5f), atelierToken(kTextPrimary),
              pct);
  return changed;
}

// One entry of the `NEW` popup: the kind's rail as a swatch, its glyph, its
// name -- and, for the four kinds `core/LayerOps` has no maker function for, a
// disabled row whose tooltip says which piece is missing
// (`app::layerKindUnbuildableReason()`, which quotes core/Layer.hpp rather than
// inventing a second account). Disabled rather than absent for the same reason
// twenty tool cells are disabled rather than absent: the popup is where a
// reader learns the product has seven layer kinds.
// `w` and `glyphCol` are measured once by the caller over the whole menu rather
// than per row: an explicit width because the Selectable's own label is empty
// (everything visible is drawn below it) and a zero-width Selectable in an
// auto-sized popup would leave the popup with no width to size itself from, and
// a shared glyph column because `layerKindGlyphForFont()` falls back to `[A]` on
// a machine with no glyph source and the names would otherwise step sideways
// between rows.
bool newLayerKindMenuItem(const NewLayerKindEntry& entry, float w, float glyphCol) {
  const float h = ImGui::GetTextLineHeight() + 4.0f;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!entry.buildable);
  const std::string label = std::string("##newkind") + layerKindName(entry.kind);
  const bool pressed = ImGui::Selectable(label.c_str(), false, 0, ImVec2(w, h));
  ImGui::EndDisabled();
  if (!entry.buildable) ImGui::SetItemTooltip("%s", layerKindUnbuildableReason(entry.kind));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 rail = atelierToken(layerKindRailRgb(entry.kind));
  const ImU32 text = atelierToken(entry.buildable ? kTextPrimary : kTextSecondary);
  dl->AddRectFilled(ImVec2(at.x, at.y), ImVec2(at.x + kLayerRailW, at.y + h), rail);
  const float ty = at.y + (h - ImGui::GetFontSize()) * 0.5f;
  dl->AddText(ImVec2(at.x + kLayerRailW + 6.0f, ty), text,
              layerKindGlyphForFont(entry.kind).c_str());
  dl->AddText(ImVec2(at.x + kLayerRailW + 6.0f + glyphCol + 8.0f, ty), text,
              layerKindName(entry.kind));
  return pressed && entry.buildable;
}

// The popup's width and its glyph column, measured over every entry so no name
// is clipped and no two rows put their name in a different place.
ImVec2 newLayerKindMenuMetrics() {
  float glyphCol = 0.0f;
  float nameCol = 0.0f;
  for (const NewLayerKindEntry& entry : newLayerKindMenu()) {
    glyphCol = std::max(glyphCol, ImGui::CalcTextSize(layerKindGlyphForFont(entry.kind).c_str()).x);
    nameCol = std::max(nameCol, ImGui::CalcTextSize(layerKindName(entry.kind)).x);
  }
  return ImVec2(kLayerRailW + 6.0f + glyphCol + 8.0f + nameCol + 10.0f, glyphCol);
}

void drawLayersSection(AppState& st) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    ImGui::TextWrapped("A session starts with one, so this means every document was closed. "
                       "File > New Document or File > Open... makes another, and its layers "
                       "are drawn on the canvas over the paper. Painting is separate: a "
                       "stroke writes sim::PaintSim's texture, not a layer, so paint never "
                       "appears in this list.");
    return;
  }

  Document& doc = od->document;
  static char renameBuf[128] = "";
  // A reference to the document's own active layer, so every mutation below
  // -- click, ctrl-click, shift-range, a command's returned row -- moves what
  // the brush paints into as well as what this panel edits. The clamp on the
  // next line is the reason `activeLayerIndex()` exists: a stack can shrink
  // under a stored index between frames.
  size_t& selected = od->activeLayer;
  if (selected >= doc.layers.size()) selected = doc.layers.empty() ? 0 : doc.layers.size() - 1;

  auto run = [&](LayerOpResult r) {
    const DocumentOpResult out = recordLayerEdit(*od, std::move(r));
    g_layers.lastError = out.ok ? std::string() : out.error;
    return out.ok;
  };

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float panelW = ImGui::GetContentRegionAvail().x;
  const ImU32 ruleCol = atelierToken(kRule);
  const ImU32 divCol = atelierToken(kDivider);
  const ImU32 textCol = atelierToken(kTextPrimary);
  const ImU32 mutedCol = atelierToken(kTextSecondary);

  // The rows the filter lets through, needed here rather than at the loop
  // because the header band's count reports both numbers.
  // `layersMatchingFilter()` returns model indices ascending (bottom-first), so
  // walking it backwards is the panel's own top-first order -- app/LayerPanel
  // still owns the single reversal in the codebase and nothing here computes
  // `count - 1 - row` a second time.
  const size_t count = doc.layers.size();
  // Filtered rows, minus whatever a collapsed group is hiding (PRD C7's UI
  // half). The two are independent concerns kept in one pass here rather
  // than merged into `app::layersMatchingFilter()` itself: that function is
  // the design's own PRD C15 filter, text-and-kind, and collapse is neither
  // -- app/LayerPanel.hpp's `layerHiddenByCollapsedGroup()` is deliberately a
  // second predicate, not a third field bolted onto `LayerFilter`.
  std::vector<size_t> visibleRows;
  for (const size_t i : layersMatchingFilter(doc, g_layers.filter))
    if (!layerHiddenByCollapsedGroup(doc, i, g_layers.collapsedGroups)) visibleRows.push_back(i);

  // --- The header band -----------------------------------------------------
  //
  // The design's tab strip, minus the tabs -- see this section's doc comment
  // for why there are none. What is left is the strip's right-hand slot: the
  // count, monospace and right-aligned like every numeric in this chrome
  // (docs/ui.md section 1), reading `3/8` when the filter is hiding five rows
  // so that neither number can be mistaken for the other.
  //
  // **The document name used to lead this band and no longer does**
  // (`docs/testing-issues.md` T26, reported as "what is the first UI item, it
  // seems to show the document name -- remove it"). It was redundant three
  // ways over: the tab strip above the canvas names every open document and
  // marks the active one, the title band names it again, and this panel is
  // unambiguously about whatever document is active. A third copy in the
  // panel's first row bought nothing and cost the row its only real job.
  //
  // **The count stays, and that is not the same question.** It is the slot the
  // design actually specifies here, and it is the ONLY place the filter's
  // effect is visible -- with five rows hidden, `3/8` is what distinguishes
  // "this document has three layers" from "this box is hiding five of them".
  // Removing it would delete feedback rather than a duplicate.
  {
    const float h = ImGui::GetTextLineHeight() + 4.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    pushAtelierMono();
    const std::string countText = layerPanelCountLabel(visibleRows.size(), count);
    const ImVec2 sz = ImGui::CalcTextSize(countText.c_str());
    dl->AddText(ImVec2(at.x + panelW - sz.x, at.y + 2.0f), mutedCol, countText.c_str());
    popAtelierMono();
    ImGui::Dummy(ImVec2(panelW, h));
    // **This tooltip used to end with a claim that has been false since the RGB
    // stroke routes landed**: "a stroke reaches no layer and nothing painted
    // appears here." `strokeRouteWritesLayer()` (app/StrokeSession.hpp) now
    // answers true for eight of the nine routes -- CpuDeposit, RgbDeposit,
    // RgbErase, PigmentErase, PencilDeposit, TonalBrush, CloneStamp and
    // Smudge. Only `PaintSim`, the solver route a Pigment layer takes, still
    // paints somewhere this panel cannot show.
    //
    // Left as a narrower, true statement rather than deleted, because the
    // surprise it was written to prevent is real and still happens -- it is
    // just no longer the general case. The predicate is named here rather than
    // the route list being retyped, so the next route to arrive updates this
    // sentence's meaning without anyone having to remember to edit it.
    ImGui::SetItemTooltip("%d x %d, %zu layer(s).\n"
                        "A stroke on a Pigment layer paints sim::PaintSim's own canvas,\n"
                        "which has no layer awareness -- so that one route alone leaves\n"
                        "nothing here. Every other route writes the layer.",
                        doc.width, doc.height, doc.layers.size());
    // docs/ui.md section 1's 2px rule, closing the header against the controls.
    dl->AddLine(ImVec2(at.x, at.y + h), ImVec2(at.x + panelW, at.y + h), ruleCol,
                kRuleThickness);
    ImGui::Dummy(ImVec2(panelW, kRuleThickness));
  }

  // --- The filter band -----------------------------------------------------
  //
  // app/LayerPanel.hpp states the filter's whole rule; the two halves visible
  // here are that a hidden row **stays selected** (so clearing the box brings
  // it back) and that `runLayerSetCommand()` restricts every gesture to the
  // rows on screen.
  {
    const std::string kindLabel = layerKindFilterLabel(g_layers.filter.kind);
    pushAtelierMono();
    const float kindW = ImGui::CalcTextSize(kindLabel.c_str()).x + 26.0f;
    popAtelierMono();
    ImGui::SetNextItemWidth(std::max(60.0f, panelW - kindW - 6.0f));
    if (ImGui::InputTextWithHint("##layerfilter", "Filter by name", g_layers.filterBuf,
                                 sizeof(g_layers.filterBuf)))
      g_layers.filter.text = g_layers.filterBuf;
    ImGui::SetItemTooltip("Filters which rows are drawn -- nothing else.\n"
                        "A hidden row stays selected, so clearing this\n"
                        "brings it back, and a Multi-selection command\n"
                        "acts only on the rows you can see.");
    ImGui::SameLine(0.0f, 6.0f);
    pushAtelierMono();
    ImGui::SetNextItemWidth(kindW);
    if (ImGui::BeginCombo("##layerkindfilter", kindLabel.c_str())) {
      if (ImGui::Selectable(layerKindFilterLabel(std::nullopt).c_str(),
                            !g_layers.filter.kind.has_value()))
        g_layers.filter.kind.reset();
      for (const NewLayerKindEntry& entry : newLayerKindMenu()) {
        const bool on =
            g_layers.filter.kind.has_value() && *g_layers.filter.kind == entry.kind;
        // Every kind, including the four that cannot be *created*: a document
        // that arrived carrying a Text layer (PRD I10) is exactly the case a
        // user needs to filter for, and refusing to offer the kind here would
        // make the one layer they cannot make the one layer they cannot find.
        if (ImGui::Selectable(layerKindFilterLabel(entry.kind).c_str(), on))
          g_layers.filter.kind = entry.kind;
      }
      ImGui::EndCombo();
    }
    popAtelierMono();
  }

  // --- The selected layer's blend and opacity ------------------------------
  //
  // Krita's own docker layout, which is also the design's: a persistent bar
  // naming the *selected* layer's blend and opacity above the list rather than
  // a control on every row. It always describes `selected`, so clicking a
  // different row changes what this reads on the very next frame with no state
  // of its own to sync.
  if (selected < doc.layers.size()) {
    pushAtelierMono();
    const float blendLabelW = ImGui::CalcTextSize("BLEND").x;
    const float opacityLabelW = ImGui::CalcTextSize("OPACITY").x;
    popAtelierMono();
    const float fixed = blendLabelW + opacityLabelW + kLayerOpacityW + 4.0f * 6.0f;
    // One row when the combo still gets a usable width at this panel width, two
    // when it does not. The wrap is `app::layoutLabelledControl()`'s own rule
    // -- a label never clips and a widget never starves -- applied to a band
    // whose three labels this file draws itself.
    const bool oneRow = (panelW - fixed) >= 90.0f;

    const std::vector<BlendMode> menu = blendMenuForLayer(doc, selected);
    const size_t sel = blendMenuSelection(doc, selected, menu);
    const std::string preview = sel < menu.size()
                                    ? blendMenuEntryText(menu[sel])
                                    : doc.layers[selected].blend + "  (this build cannot set this)";

    pushAtelierMono();
    ImGui::TextUnformatted("BLEND");
    popAtelierMono();
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::SetNextItemWidth(oneRow ? (panelW - fixed) : -1.0f);
    if (ImGui::BeginCombo("##activeLayerBlend", preview.c_str())) {
      for (size_t m = 0; m < menu.size(); ++m) {
        const bool isSelected = (m == sel);
        if (ImGui::Selectable(blendMenuEntryText(menu[m]).c_str(), isSelected))
          run(setLayerBlend(doc, selected, menu[m]));
        if (isSelected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("The selected layer's blend mode. Blend modes are\n"
                        "chosen for the linear working space; Screen only\n"
                        "behaves above 1.0 if 1.0 is white, so it is labelled\n"
                        "display-referred (PRD B7). Mix is offered only on a\n"
                        "Pigment layer sitting on another Pigment layer (PRD\n"
                        "L5). A mode this build cannot composite is marked\n"
                        "(!) on the row itself.");
    if (oneRow) ImGui::SameLine(0.0f, 6.0f);
    pushAtelierMono();
    ImGui::TextUnformatted("OPACITY");
    popAtelierMono();
    ImGui::SameLine(0.0f, 6.0f);
    float opacity = doc.layers[selected].opacity;
    pushAtelierMono();
    const bool moved = layerOpacityMeter("##activeLayerOpacity", &opacity, kLayerOpacityW,
                                         ImGui::GetTextLineHeight() + 2.0f);
    popAtelierMono();
    if (moved) run(setLayerOpacity(doc, selected, opacity));
    ImGui::SetItemTooltip("The selected layer's opacity -- click or drag anywhere\n"
                        "across the meter. A locked layer refuses every frame of\n"
                        "the drag, not merely the first.");
  } else {
    ImGui::TextDisabled("No layer selected.");
  }

  // The 2px rule closing the controls against the list, as the design draws it.
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(at.x, at.y + 2.0f), ImVec2(at.x + panelW, at.y + 2.0f), ruleCol,
                kRuleThickness);
    ImGui::Dummy(ImVec2(panelW, 4.0f));
  }

  // --- The rows ------------------------------------------------------------
  //
  // `structureChanged` stops the loop the same frame a reorder/add/remove fires
  // rather than continuing to render rows against indices that no longer mean
  // what they did -- the identical precaution drawOpStackEditor() takes over
  // core::OpStack, and for the identical reason (core/LayerOps' move/remove
  // shift the vector).
  bool structureChanged = false;
  if (visibleRows.empty() && count > 0)
    ImGui::TextDisabled("no layer matches the filter (%zu hidden)", count);

  const float lineH = ImGui::GetTextLineHeight();
  const float rowH = kLayerRowPadY * 2.0f + lineH * 2.0f + kLayerLineGap;

  // T11: bounded scroll region, the same idiom drawHistorySection() (T8)
  // uses for `##historyrows` -- see that function's comment for the two
  // findings this reuses rather than rediscovers.
  //
  // **The floor is not defensive padding, it is the empty case.** A document
  // with zero layers is representable (app/LayerEditor.cpp: "removing the
  // last layer is allowed", core/LayerOps.hpp), and `BeginChild()` reads a
  // height of `0.0f` as *fill the rest of the column*
  // (`imgui.cpp`: `if (size.y <= 0.0f) size.y = ImMax(content_avail.y + size.y, 4.0f);`).
  // Without `std::max` here, the one state with nothing to show would be the
  // one that swallows every section below LAYERS.
  //
  // **A third finding, not in drawHistorySection() (T8) to reuse, found while
  // screenshotting this one for T11's own verification list.** `BeginChild()`
  // sizes an OUTER box; a bordered child's rows still sit inside the current
  // style's `WindowPadding`, so a box sized to exactly N row-heights is a few
  // pixels short of them once that padding is subtracted back out, and shows
  // a scrollbar for content that fits. `##historyrows` has the identical gap
  // -- confirmed by screenshot on this build, `min(2, 8)` rows -- so this
  // is not new to LAYERS, only newly caught here. Added back in rather than
  // carried over silently, so the two rows' worth of padding is counted once
  // instead of clipped off the bottom.
  // **Sized to the panel's own remaining room, not a fixed row count.**
  // `kLayersVisibleRows` used to be a hard ceiling of 8 regardless of how much
  // vertical space this dock actually had -- on a tall dock the list stopped
  // growing well short of the space available and scrolled early; on a short
  // one 8 rows could already be more than fit. The reserve below is for
  // everything this function still draws AFTER the child: the rule + Dummy
  // before the command row, the command row itself, and the collapsed
  // "Multi-selection" header -- three UI-control-height lines, roughly. An
  // error/warning message band (rare, and only ever present for one frame's
  // worth of a refusal or a merge warning) is not accounted for, on purpose:
  // reserving for its variable, text-wrap-dependent height every frame would
  // permanently shrink the list for a case that is usually absent, and the
  // child recomputes every frame regardless, so the one frame a message is
  // showing simply borrows a little of the list's row budget rather than
  // clipping anything.
  const float reserveBelowChild =
      ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y + 3.0f;
  const float availableForChild =
      std::max(rowH, ImGui::GetContentRegionAvail().y - reserveBelowChild);
  const size_t rowsThatFit =
      std::max<size_t>(1, static_cast<size_t>(availableForChild / rowH));
  const size_t rowsToShow = std::min(visibleRows.size(), rowsThatFit);
  const float childH =
      std::max(rowH, static_cast<float>(rowsToShow) * rowH) + 2.0f * ImGui::GetStyle().WindowPadding.y;

  // Auto-scroll follows the SELECTED layer -- triggered by a change in
  // `selected`, not every frame, so it never fights the user's own scroll.
  // **The trigger holds for two frames, not one**, because `SetScrollHereY()`
  // only sets `ScrollTarget`; ImGui turns that into a position at the next
  // `Begin()` of this child by clamping `scroll = ImMin(scroll,
  // window->ScrollMax)` (`CalcNextScrollFromScrollTargetAndClamp()` in
  // imgui.cpp), and `ScrollMax` comes from the content size measured on the
  // PREVIOUS frame -- which is 0 on the frame this child first exists, so a
  // one-frame trigger clamps to the top instead of moving anything. See
  // drawHistorySection()'s longer comment on `s_followCursorFrames` for the
  // full derivation; this is the same clamp, not a different bug.
  static size_t s_lastSelectedLayer = SIZE_MAX;
  static int s_followSelectionFrames = 0;
  if (selected != s_lastSelectedLayer) s_followSelectionFrames = 2;
  s_lastSelectedLayer = selected;
  const bool followSelection = s_followSelectionFrames > 0;
  if (s_followSelectionFrames > 0) --s_followSelectionFrames;

  // Rows sit flush against each other, separated by the design's 1px divider
  // rather than by ImGui's inter-item gap.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
  if (ImGui::BeginChild("##layerrows", ImVec2(0.0f, childH), true)) {
    // This child's own draw list, fetched fresh rather than reusing the outer
    // `dl` captured before this function had a child window: clipping and
    // scroll offset are both properties of the draw list a command lands in,
    // so drawing rows through the panel's own list would neither clip to this
    // box nor move when it scrolls.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t vr = visibleRows.size(); vr-- > 0 && !structureChanged;) {
      const size_t i = visibleRows[vr];
      const Layer& layer = doc.layers[i];
      ImGui::PushID(static_cast<int>(i));

      const ImVec2 o = ImGui::GetCursorScreenPos();
      const bool inSelection = g_layers.selection.contains(i);
      const bool renamingThis = g_layers.renaming.has_value() && *g_layers.renaming == i;

      // The row-wide hit target, submitted FIRST and marked overlappable so the
      // eye and lock buttons drawn on top of it still take their own clicks.
      // `SetNextItemAllowOverlap()` is what makes that ordering safe: the row
      // only claims the mouse when the previous frame's hovered id was the row
      // itself, so a click landing on the eye can never also select the row.
      ImGui::SetNextItemAllowOverlap();
      // `InvisibleButton()` asserts on a zero-width item, and a controls column
      // measured at zero width during a layout pass is a real state -- the same
      // degenerate input `controlsWheelScrollStep()` guards against.
      ImGui::InvisibleButton("##row", ImVec2(std::max(1.0f, panelW), rowH));
      // Right here, and not further down after the eye/lock buttons: those
      // are their own tiny items and would overwrite the cursor bookkeeping
      // `SetScrollHereY()` reads with their own small rect. This "##row"
      // InvisibleButton's rect is the whole row, which is the rect that
      // should end up on screen.
      if (followSelection && i == selected) ImGui::SetScrollHereY();
      // **Hover is asked of the rectangle, not of the item**, and that is not a
      // shortcut -- it is the whole of the design's lock affordance. The eye and
      // the padlock are separate items sitting on top of this one, so
      // `IsItemHovered()` goes false the moment the pointer reaches either of
      // them: the padlock would dim exactly as you moved to click it, which is
      // the opposite of "the slot reads as a control". `AllowWhenBlockedByActiveItem`
      // keeps the wash on through a drag of the row itself.
      const bool rowHovered =
          ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
          ImGui::IsMouseHoveringRect(o, ImVec2(o.x + panelW, o.y + rowH));
      // Selection on **press**, not on release, which is what the retired
      // `Selectable(..., ImGuiSelectableFlags_AllowDoubleClick)` did and is what
      // makes the double-click test below work at all: `IsMouseDoubleClicked()`
      // is true only on the frame of the second *press*, so a rename started from
      // an InvisibleButton's release value would never fire. Pressing also begins
      // a drag, which is the order every editor uses -- the row you drag is the
      // row that became selected under your finger.
      const bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
      const bool rowRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
      const bool rowDoubleClicked = rowClicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

      // **Drag to reorder.** The dragged payload is the model index; the drop
      // target is whichever row's rectangle the pointer released over, refined by
      // which half of that row it was in (`app::layerDropTargetIndex()` turns
      // "row + half" into the `to` `core::moveLayer()` wants). Not offered while
      // renaming -- a text field and a drag source on the same item would fight
      // over the mouse.
      if (!renamingThis) {
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
          ImGui::SetDragDropPayload("NP_LAYER_ROW", &i, sizeof(size_t));
          ImGui::TextUnformatted(layerRowTitle(layer, i).c_str());
          ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NP_LAYER_ROW")) {
            const size_t from = *static_cast<const size_t*>(payload->Data);
            const bool droppedAboveMidpoint = ImGui::GetMousePos().y < (o.y + rowH * 0.5f);
            const size_t to = layerDropTargetIndex(i, droppedAboveMidpoint, count);
            if (from != to) run(moveLayer(doc, from, to));
            structureChanged = true;
          }
          ImGui::EndDragDropTarget();
        }
      }

      // **Right-click context menu.** Selects the row first, the same way a
      // left-click does, so a command chosen from the menu acts on the row the
      // user right-clicked rather than whatever was selected before -- and so
      // `runLayerCommand()` below, which always acts on
      // `OpenDocument::activeLayer`, is acting on the right one.
      if (rowRightClicked) {
        g_layers.selection = singleLayerSelection(i);
        selected = i;
      }
      if (ImGui::BeginPopupContextItem("layerRowContext")) {
        for (const LayerCommand command : allLayerCommands()) {
          // CaptureComp captures the whole stack, not this row -- it belongs to
          // the COMPS panel and the `Layer` menu, not a per-row menu whose every
          // other entry is about the layer under the cursor.
          if (command == LayerCommand::CaptureComp) continue;
          const bool available = layerCommandAvailable(doc, command, i);
          if (ImGui::MenuItem(layerCommandLabel(command), nullptr, false, available)) {
            runLayerCommand(st, command);
            structureChanged = true;
          }
        }
        // **Add Op, for now.** The full op-stack editor lives in Layer
        // Properties -- but *adding* one is a single gesture with no parameters
        // to show yet (`app::makeNewOp()` always builds a disabled PointA op), so
        // it gets a fast path here rather than waiting for the dialog.
        if (ImGui::BeginMenu("Add Op")) {
          for (const PointOpKind kind :
               {PointOpKind::Levels, PointOpKind::Curves, PointOpKind::Exposure,
                PointOpKind::Saturation, PointOpKind::Grayscale, PointOpKind::ChannelMixer}) {
            if (ImGui::MenuItem(pointOpKindName(kind))) {
              run(addLayerOp(doc, i, makeNewOp(kind)));
              structureChanged = true;
            }
          }
          ImGui::EndMenu();
        }
        ImGui::EndPopup();
      }

      // **Stop the row, not just the loop.** A drop or a command from the row's
      // own menu has already run by this point, and Delete/Merge/Flatten shrink
      // `doc.layers` -- so `layer`, a reference into that vector, no longer names
      // what it did and `i` may be past the end. Everything below reads both. The
      // loop condition tests the same flag, but only on the *next* iteration,
      // which is one row's worth of reads too late.
      if (structureChanged) {
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rowH));
        ImGui::PopID();
        break;
      }

      // ---- the row's paint, over the hit target ----------------------------
      if (inSelection)
        dl->AddRectFilled(o, ImVec2(o.x + panelW, o.y + rowH), atelierToken(kRowSelected));
      else if (rowHovered)
        dl->AddRectFilled(o, ImVec2(o.x + panelW, o.y + rowH), atelierToken(kLayerRowHover));
      dl->AddLine(ImVec2(o.x, o.y + rowH - 1.0f), ImVec2(o.x + panelW, o.y + rowH - 1.0f), divCol,
                  kDividerThickness);

      // The kind rail, and the clipped row's indent.
      dl->AddRectFilled(o, ImVec2(o.x + kLayerRailW, o.y + rowH),
                        atelierToken(layerKindRailRgb(layer.kind)));
      float x = o.x + kLayerRailW;
      if (layer.clipped) {
        drawClipBracket(dl, ImVec2(x, o.y), kLayerClipIndent, rowH, atelierToken(kHairline));
        x += kLayerClipIndent;
      }
      // **A group's members read as INSIDE it** (PRD C7's UI half, this
      // task's own reachability requirement): one indent step, plus a
      // vertical guide, per level of `app::layerGroupDepth()` -- the same
      // "this one belongs to that one" reading `drawClipBracket()` above
      // already gives the clip indent, applied to nesting instead. A group
      // nested two deep draws two guides and indents its own row twice; its
      // members read one step deeper again.
      const size_t groupDepth = layerGroupDepth(doc, i);
      for (size_t lvl = 0; lvl < groupDepth; ++lvl) {
        const float guideX = x + (static_cast<float>(lvl) + 0.5f) * kLayerGroupIndentW;
        dl->AddLine(ImVec2(guideX, o.y), ImVec2(guideX, o.y + rowH), atelierToken(kHairline), 1.0f);
      }
      x += static_cast<float>(groupDepth) * kLayerGroupIndentW;
      x += kLayerRowPadX;

      // The two icon slots. Colours first, because all three states of the lock
      // are decided here and the design's rest/hover/locked ramp is the whole
      // point of the slot.
      const ImU32 eyeCol = layer.visible ? textCol : mutedCol;
      const ImU32 lockCol = layer.locked ? mutedCol
                            : rowHovered ? mutedCol
                                         : atelierToken(kLayerLockRest);
      const float iconY = o.y + rowH * 0.5f;
      const ImVec2 eyeAt(x, o.y + (rowH - kLayerEyeW) * 0.5f);
      drawEyeGlyph(dl, ImVec2(x + kLayerEyeW * 0.5f, iconY), kLayerEyeW, eyeCol, layer.visible);
      x += kLayerEyeW + kLayerRowGap;
      const ImVec2 lockAt(x, o.y + (rowH - kLayerLockW) * 0.5f);
      drawPadlockGlyph(dl, ImVec2(x + kLayerLockW * 0.5f, iconY), kLayerLockW, lockCol,
                       layer.locked);
      x += kLayerLockW + kLayerRowGap;

      // The collapse/expand triangle -- Group rows only. An ordinary row
      // reserves no slot for one and no gap either: indentation is what this
      // panel spends on "you are one level in", not a blank triangle on
      // every row that is not a group. The actual `InvisibleButton()` is
      // issued later, alongside the eye and the padlock, for the identical
      // overlap-ordering reason their own comment gives -- so a click on the
      // triangle can never also select or drag the row.
      const bool isGroupRow = layer.kind == LayerKind::Group;
      ImVec2 discAt(x, o.y);
      bool groupCollapsed = false;
      if (isGroupRow) {
        groupCollapsed = g_layers.collapsedGroups.count(layer.groupTag) != 0;
        discAt = ImVec2(x, o.y + (rowH - kLayerDisclosureW) * 0.5f);
        drawDisclosureTriangle(dl, ImVec2(x + kLayerDisclosureW * 0.5f, iconY), kLayerDisclosureW,
                               textCol, !groupCollapsed);
        x += kLayerDisclosureW + kLayerRowGap;
      }

      // The kind glyph, muted on a hidden layer along with the name, so a hidden
      // row reads as hidden from its leading edge rather than only from the word
      // HIDDEN at the far end of a metadata line the panel may have clipped.
      const ImU32 nameCol = layer.visible ? textCol : mutedCol;
      const std::string glyph = layerKindGlyphForFont(layer.kind);
      const float glyphW = std::max(11.0f, ImGui::CalcTextSize(glyph.c_str()).x);
      dl->AddText(ImVec2(x, o.y + kLayerRowPadY), nameCol, glyph.c_str());
      x += glyphW + kLayerRowGap;

      // The trailing slot, measured before the text column so the two cannot
      // overlap. `LINKED+n` is the design's own occupant of it (§6.1: "LINKED+n
      // takes the trailing slot, so nothing in the worst case is truncated"); the
      // mask chip sits beside it when the layer has both.
      float trailingX = o.x + panelW - kLayerRowPadX;
      const std::string linkBadge = layerLinkBadgeText(doc, i);
      if (!linkBadge.empty()) {
        pushAtelierMono();
        const ImVec2 sz = ImGui::CalcTextSize(linkBadge.c_str());
        trailingX -= sz.x + 8.0f;
        dl->AddRect(ImVec2(trailingX, o.y + (rowH - sz.y) * 0.5f - 2.0f),
                    ImVec2(trailingX + sz.x + 6.0f, o.y + (rowH + sz.y) * 0.5f + 2.0f), divCol);
        dl->AddText(ImVec2(trailingX + 3.0f, o.y + (rowH - sz.y) * 0.5f), mutedCol,
                    linkBadge.c_str());
        popAtelierMono();
        trailingX -= kLayerRowGap;
      }
      if (layer.mask.has_value()) {
        // A half-filled square: the mask indicator, and deliberately not a
        // thumbnail. `layerRowSubLine()` already says `MASK` in the metadata
        // line; what this adds is that the marker survives the line being
        // clipped, which at 322 px it often is. It says nothing about the mask's
        // *contents* -- there is no way to paint one yet, and a thumbnail that
        // was always uniform would be worse than none.
        trailingX -= kLayerMaskChipW;
        const ImVec2 lo(trailingX, o.y + (rowH - kLayerMaskChipW) * 0.5f);
        const ImVec2 hi(lo.x + kLayerMaskChipW, lo.y + kLayerMaskChipW);
        dl->AddRectFilled(lo, ImVec2(hi.x, (lo.y + hi.y) * 0.5f), atelierToken(kRule));
        dl->AddRectFilled(ImVec2(lo.x, (lo.y + hi.y) * 0.5f), hi, atelierToken(kChromeDeep));
        dl->AddRect(lo, hi, atelierToken(kHairline));
        trailingX -= kLayerRowGap;
      }
      if (layer.alphaLocked) {
        // Alpha lock's own status chip (this task's requirement 6): a 2x2
        // checkerboard in the same two theme tokens the mask chip just above
        // draws with, so it is legible on both a light and a dark canvas for
        // the identical reason the mask chip already is -- neither token is a
        // literal colour, both come from `atelierToken()`. Deliberately NOT a
        // second padlock; see `kLayerAlphaLockChipW`'s own comment for why.
        // Purely a status light, like the mask chip beside it: there is
        // nothing here to click, `ToggleAlphaLock` is reached through the
        // `Layer` menu and the row's own right-click menu, both of which
        // walk `allLayerCommands()` and needed no new code to offer it.
        trailingX -= kLayerAlphaLockChipW;
        const ImVec2 lo(trailingX, o.y + (rowH - kLayerAlphaLockChipW) * 0.5f);
        const ImVec2 hi(lo.x + kLayerAlphaLockChipW, lo.y + kLayerAlphaLockChipW);
        const ImVec2 mid((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
        const ImU32 tileA = atelierToken(kRule);
        const ImU32 tileB = atelierToken(kChromeDeep);
        dl->AddRectFilled(lo, mid, tileA);
        dl->AddRectFilled(ImVec2(mid.x, lo.y), ImVec2(hi.x, mid.y), tileB);
        dl->AddRectFilled(ImVec2(lo.x, mid.y), ImVec2(mid.x, hi.y), tileB);
        dl->AddRectFilled(mid, hi, tileA);
        dl->AddRect(lo, hi, atelierToken(kHairline));
        trailingX -= kLayerRowGap;
      }

      // The colour label chip (PRD C15), immediately before the name as the
      // design places it. Drawn only for a label this build has a swatch for; an
      // unrecognised one from a newer build shows as text in the metadata line
      // instead, because painting it in some default colour would make two
      // different labels look like one (app/LayerPanel.hpp).
      float textX = x;
      if (const std::optional<LayerLabelSwatch> swatch = layerColorLabelSwatch(layer.colorLabel)) {
        dl->AddRectFilled(ImVec2(textX, o.y + kLayerRowPadY + 1.0f),
                          ImVec2(textX + kLayerRailW, o.y + kLayerRowPadY + lineH - 1.0f),
                          ImGui::GetColorU32(ImVec4(swatch->r, swatch->g, swatch->b, 1.0f)));
        textX += kLayerRailW + 4.0f;
      }
      const float textW = std::max(20.0f, trailingX - textX);

      if (renamingThis) {
        // **Inline rename.** The field replaces the name line entirely rather
        // than sitting beside it -- two editable widgets for one value on screen
        // at once is what the properties dialog's own "Name" field would
        // otherwise become the moment this row is also the selection.
        ImGui::SetCursorScreenPos(ImVec2(textX, o.y + kLayerRowPadY - 1.0f));
        if (g_layers.renameFieldFocusPending) {
          ImGui::SetKeyboardFocusHere();
          g_layers.renameFieldFocusPending = false;
        }
        ImGui::SetNextItemWidth(textW);
        const bool committed =
            ImGui::InputText("##inlineRename", g_layers.renameFieldBuf,
                             sizeof(g_layers.renameFieldBuf), ImGuiInputTextFlags_EnterReturnsTrue);
        if (committed) {
          run(setLayerName(doc, i, g_layers.renameFieldBuf));
          g_layers.renaming.reset();
        } else if (ImGui::IsItemDeactivated()) {
          // Lost focus without Enter -- a click elsewhere, Tab, or Escape.
          // Cancel: the buffer is simply never written back.
          g_layers.renaming.reset();
        }
      } else {
        drawClippedText(dl, ImVec2(textX, o.y + kLayerRowPadY), textW, nameCol,
                        layerRowTitle(layer, i).c_str());
      }

      // The metadata line, in the monospace face docs/ui.md section 1 puts caps
      // labels in -- `app::layerRowSubLine()` verbatim, which is where `(!)`,
      // `(display-referred)`, `MASK`, `n OPS`, `CLIPPED`, `HIDDEN`, `LOCKED` and
      // the colour label's own name all come from. Nothing is assembled here, so
      // every one of those markers is covered by the assertions that already
      // exist for that function.
      {
        pushAtelierMono();
        const std::string sub = layerRowSubLine(layer);
        drawClippedText(dl, ImVec2(textX, o.y + kLayerRowPadY + lineH + kLayerLineGap), textW,
                        inSelection ? atelierToken(kLayerSelMeta) : mutedCol, sub.c_str());
        popAtelierMono();
      }

      // ---- the icon buttons, last, so they take the mouse -------------------
      ImGui::SetCursorScreenPos(eyeAt);
      if (ImGui::InvisibleButton("##vis", ImVec2(kLayerEyeW, kLayerEyeW)))
        run(setLayerVisible(doc, i, !layer.visible));
      ImGui::SetItemTooltip("Visibility. Allowed even on a locked layer --\n"
                          "hiding a layer changes nothing about it.");
      ImGui::SetCursorScreenPos(lockAt);
      if (ImGui::InvisibleButton("##lock", ImVec2(kLayerLockW, kLayerLockW)))
        run(setLayerLocked(doc, i, !layer.locked));
      ImGui::SetItemTooltip("%s. A locked layer refuses edits that change its\n"
                          "pixels or its place in the stack -- visibility and\n"
                          "this padlock itself still work.",
                          layer.locked ? "Locked -- click to unlock"
                                       : "Unlocked -- click to lock");
      if (isGroupRow) {
        ImGui::SetCursorScreenPos(discAt);
        if (ImGui::InvisibleButton("##disclosure", ImVec2(kLayerDisclosureW, kLayerDisclosureW))) {
          if (groupCollapsed)
            g_layers.collapsedGroups.erase(layer.groupTag);
          else
            g_layers.collapsedGroups.insert(layer.groupTag);
          // `visibleRows` was built at the top of this function, before this
          // click, so it still names rows this group's new collapse state
          // says should not be drawn (or is now missing ones that should).
          // Nothing about `doc.layers` changed, but the row SET this frame
          // is walking did -- the identical staleness `structureChanged`
          // already exists to stop the loop for, reused here rather than
          // given a second flag that means the same thing.
          structureChanged = true;
        }
        ImGui::SetItemTooltip("%s", groupCollapsed ? "Expand group -- show its members"
                                                  : "Collapse group -- hide its members");
      }

      // Selection, decided after the icon buttons so that a click they claimed is
      // not also a click on the row. Multi-select (PRD C12): plain click replaces
      // the selection, ctrl-click (cmd-click on this platform) toggles one row,
      // shift-click extends from `g_layers.shiftAnchor` -- the last row clicked
      // WITHOUT shift -- to this one, so a run of shift-clicks grows one
      // contiguous range instead of re-anchoring at each step. `selected` still
      // follows the row that was clicked in every case (including shift), so the
      // controls above the list always describe a row the user just touched --
      // that is a separate question from where the range starts, see the field
      // comment on `shiftAnchor`.
      if (rowClicked) {
        const ImGuiIO& io = ImGui::GetIO();
        std::vector<size_t> next;
        if (io.KeyShift) {
          const size_t anchor = std::min(g_layers.shiftAnchor, doc.layers.empty()
                                                                    ? size_t{0}
                                                                    : doc.layers.size() - 1);
          const size_t lo = std::min(anchor, i);
          const size_t hi = std::max(anchor, i);
          for (size_t k = lo; k <= hi; ++k) next.push_back(k);
        } else if (io.KeyCtrl || io.KeySuper) {
          next = g_layers.selection.indices;
          const auto at = std::find(next.begin(), next.end(), i);
          if (at != next.end())
            next.erase(at);
          else
            next.push_back(i);
          // A selection is never empty: ctrl-clicking the only selected row
          // leaves it selected rather than leaving the panel with no primary row
          // for the opacity meter and the blend combo to describe.
          if (next.empty()) next.push_back(i);
        } else {
          next.push_back(i);
        }
        // Not on the shift branch: that is exactly the case this anchor has to
        // survive across.
        if (!io.KeyShift) g_layers.shiftAnchor = i;
        g_layers.selection = makeLayerSelection(std::move(next));
        selected = i;
        if (rowDoubleClicked) {
          g_layers.renaming = i;
          std::snprintf(g_layers.renameFieldBuf, sizeof(g_layers.renameFieldBuf), "%s",
                        layer.name.c_str());
          g_layers.renameFieldFocusPending = true;
        }
      }

      ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rowH));
      ImGui::PopID();
    }
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();

  // --- The messages --------------------------------------------------------
  //
  // The refusal, verbatim. core/LayerOps' and core/Merge's sentences already
  // name the layer and say what to do about it, so there is no second
  // vocabulary here to drift from the model's -- the same rule
  // drawExportAsDialog() follows for io/Export's messages. Shared with the
  // `Layer` menu: a command refused from the menu bar is answered here, because
  // this is where a user is looking at the layer it was refused on.
  //
  // The design's own two examples are a refusal and a warning, and the two are
  // drawn differently for the reason they are kept in different members: a
  // refusal means nothing happened, a warning means something did and something
  // that was adjustable a moment ago is not any more (core/Merge.hpp §3).
  {
    const ImU32 accentCol = atelierToken(kAccent);
    auto messageBand = [&](const std::string& text, ImU32 fill, ImU32 rail, ImU32 body) {
      const float wrapW = panelW - 14.0f;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float h = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrapW).y + 10.0f;
      dl->AddRectFilled(at, ImVec2(at.x + panelW, at.y + h), fill);
      dl->AddRectFilled(at, ImVec2(at.x + kLayerRailW, at.y + h), rail);
      ImGui::SetCursorScreenPos(ImVec2(at.x + kLayerRailW + 6.0f, at.y + 5.0f));
      ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrapW);
      ImGui::PushStyleColor(ImGuiCol_Text, body);
      ImGui::TextWrapped("%s", text.c_str());
      ImGui::PopStyleColor();
      ImGui::PopTextWrapPos();
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
    };
    if (!g_layers.lastError.empty()) {
      messageBand(g_layers.lastError, atelierToken(kLayerRefusalBg), accentCol, textCol);
      if (ImGui::SmallButton("Dismiss")) g_layers.lastError.clear();
    }
    if (!g_layers.lastWarnings.empty()) {
      for (const std::string& w : g_layers.lastWarnings)
        messageBand(w, atelierToken(kChromeDeep), atelierToken(kLayerWarnRail), mutedCol);
      if (ImGui::SmallButton("Dismiss##layerwarnings")) g_layers.lastWarnings.clear();
    }
  }

  // --- The command row -----------------------------------------------------
  //
  // 2a's second change: the three `New` buttons are one `NEW` with a kind
  // popup, and it sits at the head of the command row rather than above the
  // list. Everything after it is the icon toolbar that was already here --
  // every icon still funnels through `layerCommandIconButton()`, same
  // availability predicate, same tooltip, same dispatch, so an icon is a second
  // face on each command and not a second implementation of one.
  {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(at.x, at.y + 1.0f), ImVec2(at.x + panelW, at.y + 1.0f), ruleCol,
                kRuleThickness);
    ImGui::Dummy(ImVec2(panelW, 3.0f));
  }
  {
    // Disabled with no document only; which *kinds* it can make is the popup's
    // own business, and three of the seven entries in it are always live.
    pushAtelierMono();
    const bool pressed = ImGui::SmallButton("NEW +");
    popAtelierMono();
    if (pressed) ImGui::OpenPopup("newLayerKind");
    ImGui::SetItemTooltip("New layer -- pick a kind. Three of the seven kinds can be\n"
                        "made in this build; the other four are listed and disabled,\n"
                        "because the kinds exist and their content does not.");
    if (ImGui::BeginPopup("newLayerKind")) {
      const ImVec2 metrics = newLayerKindMenuMetrics();
      pushAtelierMono();
      ImGui::TextDisabled("NEW LAYER");
      popAtelierMono();
      ImGui::Separator();
      for (const NewLayerKindEntry& entry : newLayerKindMenu())
        if (newLayerKindMenuItem(entry, metrics.x, metrics.y)) {
          runLayerCommand(st, entry.command);
          ImGui::CloseCurrentPopup();
        }
      ImGui::EndPopup();
    }
  }
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::DuplicateLayer);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::DeleteLayer);
  ImGui::SameLine();
  // PLAN.md Phase 5 step 4's pair. A mask that reveals everything costs no
  // allocation (core/Mask.hpp), and "Hide All" is deliberately absent --
  // core/LayerOps.hpp says why.
  layerCommandIconButton(st, LayerCommand::AddMask);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::RemoveMask);

  // PLAN.md Phase 5 step 10 / PRD C10 (P0), C11 (P1)'s five operations that
  // consume layers -- undo is the only way back from any of those, which is why
  // they keep a row of their own.
  layerCommandIconButton(st, LayerCommand::MergeDown);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::MergeVisible);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::FlattenImage);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::StampVisible);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::RasteriseLayer);
  ImGui::SameLine();
  // **Layer Properties** (Krita's own "sliders" toolbar button): opens the
  // modal below, which is where Colour label, Clip and this layer's op stack
  // live. Not a `LayerCommand` -- it mutates no document, only opens a dialog
  // -- so it draws its own button rather than going through
  // `layerCommandIconButton()`.
  {
    const bool available = selected < doc.layers.size();
    ImGui::BeginDisabled(!available);
    if (ImGui::SmallButton(glyphOrFallback("\xE2\x9A\x99", "[Props]").c_str()))
      ImGui::OpenPopup("Layer Properties");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Layer Properties -- name, opacity, blend mode,\n"
                        "colour label, visibility, lock, clip and this\n"
                        "layer's op stack, all in one place.");
  }

  // --- Multi-selection (PLAN.md Phase 5 step 11) ---------------------------
  //
  // Below the command row rather than above the list, so the design's band
  // order -- filter, active-layer controls, rows, messages, commands -- is
  // unbroken. Every set command, walked from `core::allLayerSetCommands()` --
  // the same list the `Layer` > Selection menu walks, so the panel and the menu
  // cannot offer different sets. One button per line rather than a packed grid:
  // these labels are long, a 322 px panel clips them, and a clipped label is
  // the exact failure app/ControlsLayout exists to have fixed once. Collapsed
  // by default, so a single-selection session never sees it.
  // --controls-all-open opens this one too, so `--screenshot` can photograph a
  // section whose default state is closed.
  if (st.controlsAllOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
  if (ImGui::CollapsingHeader("Multi-selection")) {
    ImGui::TextDisabled("%zu layer(s) selected -- ctrl-click a row to add,",
                        g_layers.selection.size());
    ImGui::TextDisabled("shift-click to extend.");
    const LayerSelection visible =
        restrictSelectionToFilter(doc, g_layers.selection, g_layers.filter);

    // ALIGNMENT -- the six align-to-selection-bounds commands plus the two
    // distribute commands, as an icon row rather than the flat text-button
    // list below: this is Figma's own presentation of the identical
    // operations (align left/centre-h/right, top/centre-v/bottom, then
    // distribute h/v), and the icons are the same Lucide glyphs the tool
    // palette already uses (`core::layerSetCommandIconCodepoints()`, merged
    // into the shared icon font by `main.cpp` alongside `toolIconCodepoints()`).
    // `AlignCanvas*` stays in the text list below: it is the same six ops
    // against a different reference frame, and its own label already says
    // "to Canvas" -- a second icon row for it would teach two icon sets for
    // one operation instead of one icon set plus a text distinction.
    ImGui::TextDisabled("Alignment");
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionLeft);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionCenterX);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionRight);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionTop);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionCenterY);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::AlignSelectionBottom);
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::DistributeHorizontally);
    ImGui::SameLine();
    layerSetCommandIconButton(st, doc, visible, LayerSetCommand::DistributeVertically);

    for (const LayerSetCommand command : allLayerSetCommands()) {
      // The eight commands above already have their own icon row; skip them
      // here so the operation isn't offered twice under two different
      // controls.
      switch (command) {
        case LayerSetCommand::AlignSelectionLeft:
        case LayerSetCommand::AlignSelectionCenterX:
        case LayerSetCommand::AlignSelectionRight:
        case LayerSetCommand::AlignSelectionTop:
        case LayerSetCommand::AlignSelectionCenterY:
        case LayerSetCommand::AlignSelectionBottom:
        case LayerSetCommand::DistributeHorizontally:
        case LayerSetCommand::DistributeVertically:
          continue;
        default:
          break;
      }
      // **Enabled exactly when `core::layerSetCommandAvailable()` says so,
      // and no second rule** -- core/LayerSetOps.hpp's own line: "Every
      // other reason a command may not go ahead ... is a REFUSAL, offered
      // and answered with a sentence." `GroupLayers` on a member that is
      // already inside a group is exactly that: available (there is a
      // selection to try), refused by `applyLayerSetOp()` when pressed, and
      // the refusal -- "already inside a group" -- lands in the message
      // band below by name, through the same `g_layers.lastError` every
      // other refusal in this section already uses. Nothing here special-
      // cases Group or Ungroup.
      const bool available = layerSetCommandAvailable(doc, command, visible);
      ImGui::BeginDisabled(!available);
      if (ImGui::SmallButton(layerSetCommandLabel(command))) runLayerSetCommand(st, command);
      ImGui::EndDisabled();
    }
  }

  // --- Layer Properties (Krita's own dialog, opened by the gear above) -----
  //
  // Everything a selected row used to expand to show lives here instead, bound
  // to `selected` for the dialog's whole open lifetime -- a `BeginPopupModal`
  // blocks every other widget in the application, so `selected` cannot change
  // out from under it mid-edit the way it could while it was inline in the
  // list. Kind, and everything the reference dialog shows that this build has
  // no equivalent of (colour space, ICC profile, per-channel toggles), is left
  // out rather than faked with a control that would do nothing -- the same rule
  // this section's own omissions follow.
  //
  // --open-layer-properties: the same id BeginPopupModal() opens on a click,
  // so --screenshot can photograph it. See AppState::openLayerProperties.
  if (st.openLayerProperties) {
    st.openLayerProperties = false;
    ImGui::OpenPopup("Layer Properties");
  }
  // --- no dim behind this modal, and no longer a special case --------------
  //
  // **It stays modal.** Modality is what the paragraph above relies on: it is
  // why `selected` cannot change under the dialog mid-edit, and dropping it
  // would hand this dialog the layer-deleted-underneath-you hazard
  // app/StrokeSession §5 spends a page on and cannot close today, because
  // `Layer::id` is 0 on every layer this build creates. Only the *dimming* is
  // dropped, and it always was.
  //
  // This block used to `PushStyleColor(ImGuiCol_ModalWindowDimBg, transparent)`
  // around the `BeginPopupModal()` below, because the suppression was scoped to
  // this one popup: the five decision gates (Revert, Recover Documents, the
  // document-path prompt, both Export dialogs) were held to want the wash.
  // **That is the theme's rule now, on the user's instruction** -- see
  // `applyAtelierTheme()`'s `ImGuiCol_ModalWindowDimBg`, which carries the
  // instruction verbatim and what dropping it costs. The push here would be a
  // no-op pushing transparent over transparent, so it is gone rather than left
  // as a line that looks load-bearing and is not.
  //
  // One ImGui fact worth keeping, since it is why the per-dialog override was
  // possible at all and would be needed again if any modal ever wants its dim
  // back: the colour is captured ONCE per modal window, into
  // `window->DC.ModalDimBgColor` inside `Begin()`, and read at end-of-frame by
  // `RenderDimmedBackgroundBehindWindow()` rather than re-read live. So an
  // override has to bracket `BeginPopupModal()` and nothing else -- pushing
  // around the popup's body changes nothing.
  if (ImGui::BeginPopupModal("Layer Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (selected >= doc.layers.size()) {
      ImGui::TextDisabled("This layer no longer exists.");
    } else {
      const size_t i = selected;
      Layer& layer = doc.layers[i];

      ImGui::TextDisabled("%s  %s", layerKindGlyphForFont(layer.kind).c_str(),
                          layerKindName(layer.kind));

      std::snprintf(renameBuf, sizeof(renameBuf), "%s", layer.name.c_str());
      if (ctlInputText("Name", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue))
        run(setLayerName(doc, i, renameBuf));

      float opacity = layer.opacity;
      // SliderFloat reports a change every frame of a drag; each one goes
      // through setLayerOpacity() so a locked layer refuses every one of them
      // rather than the first only. History (Phase 5 step 7) is what owns
      // coalescing an interaction into one undo entry.
      if (ctlSlider("Opacity", &opacity, 0.0f, 1.0f, "%.2f"))
        run(setLayerOpacity(doc, i, opacity));

      // The blend dropdown, identical in every particular to the one above the
      // list -- two controls for the one field, exactly the redundancy the
      // reference dialog itself has (Krita's docker bar and its own Layer
      // Properties both carry Blending mode).
      const std::vector<BlendMode> menu = blendMenuForLayer(doc, i);
      const size_t sel = blendMenuSelection(doc, i, menu);
      const std::string preview =
          sel < menu.size() ? blendMenuEntryText(menu[sel]) : layer.blend + "  (this build "
                                                                           "cannot set this)";
      if (ctlBeginCombo("Blending mode", preview.c_str())) {
        for (size_t m = 0; m < menu.size(); ++m) {
          const bool isSelected = (m == sel);
          if (ImGui::Selectable(blendMenuEntryText(menu[m]).c_str(), isSelected))
            run(setLayerBlend(doc, i, menu[m]));
          if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::SetItemTooltip("Blend modes are chosen for the linear working space.\n"
                          "One of them -- Screen -- only behaves above 1.0 if 1.0\n"
                          "is white, so it is labelled display-referred (PRD B7).\n"
                          "Mix is offered only on a Pigment layer sitting on\n"
                          "another Pigment layer (PRD L5).");

      ImGui::TextUnformatted("Colour label");
      ImGui::SameLine();
      // "None" first, then the seven `kLayerColorLabelNames` swatches, each a
      // small filled square rather than a text button -- `layerColorLabelSwatch()`
      // is the same lookup the row chip uses, so a colour picked here is the
      // exact colour that shows there. `PushID(name)` gives each swatch its own
      // ID despite every one of them sharing the label "##colorLabel".
      if (ImGui::SmallButton("None##colorLabelNone"))
        run(setLayerColorLabel(doc, i, kNoLayerColorLabel));
      for (const char* name : kLayerColorLabelNames) {
        ImGui::SameLine();
        ImGui::PushID(name);
        const std::optional<LayerLabelSwatch> swatch = layerColorLabelSwatch(name);
        if (swatch.has_value())
          ImGui::PushStyleColor(ImGuiCol_Button,
                                ImGui::GetColorU32(ImVec4(swatch->r, swatch->g, swatch->b, 1.0f)));
        if (ImGui::Button("##colorLabel", ImVec2(18.0f, 18.0f)))
          run(setLayerColorLabel(doc, i, name));
        if (swatch.has_value()) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", name);
        ImGui::PopID();
      }

      bool visible = layer.visible;
      if (ImGui::Checkbox("Visible", &visible)) run(setLayerVisible(doc, i, visible));
      bool locked = layer.locked;
      if (ImGui::Checkbox("Locked", &locked)) run(setLayerLocked(doc, i, locked));
      // PLAN.md Phase 5 step 9 / PRD C9. Disabled at the bottom of the stack,
      // the same "nothing below this layer" rule the row's own checkbox used to
      // enforce.
      ImGui::BeginDisabled(i == 0 && !layer.clipped);
      bool clipped = layer.clipped;
      if (ImGui::Checkbox("Clip to Layer Below", &clipped)) run(setLayerClipped(doc, i, clipped));
      ImGui::EndDisabled();
      ImGui::SetItemTooltip("Clip to the layer below (PRD C9): this layer shows only\n"
                          "where the layer beneath it has alpha. The bottom layer\n"
                          "cannot be clipped: there is nothing below it.");

      // The layer's own op stack (PLAN.md Phase 5 steps 3 and 5). On an
      // Adjustment layer this stack is the layer's entire content: the kind
      // holds no pixels, so a fresh one is an exact no-op until an op here is
      // added *and* enabled.
      if (ImGui::TreeNodeEx("layerOps", ImGuiTreeNodeFlags_DefaultOpen, "Ops (%zu)",
                            layer.ops.size())) {
        ImGui::SetItemTooltip("This layer's own non-destructive op stack. It\n"
                            "composites through core/Composite: over the layer's\n"
                            "own pixels for a layer that has them, and over\n"
                            "everything beneath for an Adjustment layer.\n"
                            "Every change here is recorded, so undo takes it\n"
                            "back and the canvas updates. The right-click menu on\n"
                            "the row itself also has \"Add Op\", for adding one\n"
                            "without opening this dialog first.");
        OpStackBinding bound;
        bound.stack = &layer.ops;
        bound.add = [&](Op op) { run(addLayerOp(doc, i, std::move(op))); };
        bound.remove = [&](size_t opIndex) { run(removeLayerOp(doc, i, opIndex)); };
        bound.move = [&](size_t from, size_t to) { run(moveLayerOp(doc, i, from, to)); };
        bound.setEnabled = [&](size_t opIndex, bool on) {
          run(setLayerOpEnabled(doc, i, opIndex, on));
        };
        bound.setOp = [&](size_t opIndex, Op op) {
          run(setLayerOp(doc, i, opIndex, std::move(op)));
        };
        drawOpStackEditor(bound);
        ImGui::TreePop();
      }
    }
    ImGui::Separator();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // --- The diagnostics, below the panel rather than above it ---------------
  //
  // PRD **A6** (P0) and the revision cache, in bytes, on screen. The design's
  // panel has nothing like these and they are deliberately last rather than
  // deleted: "only visible documents hold GPU textures, at most two" is a claim
  // about memory, and a claim about memory that only `--selftest` can see is
  // one a running session can drift away from unnoticed. `uploads` counts
  // recomposites; `cached` counts frames that cost two integer comparisons. In
  // a still window the second number climbs at the frame rate and the first
  // does not move at all.
  ImGui::Dummy(ImVec2(0.0f, 4.0f));
  textDisabledWrapped("composite: %llu upload(s), %llu cached, last %.2f ms",
                      static_cast<unsigned long long>(g_documentTextures.uploads()),
                      static_cast<unsigned long long>(g_documentTextures.cacheHits()),
                      g_documentTextures.lastUploadMs());
  textDisabledWrapped("GPU: %zu / %zu document(s) resident, %.1f MB",
                      g_documentTextures.residentDocuments(), kVisibleDocumentCap,
                      static_cast<double>(g_documentTextures.gpuTextureBytes()) /
                          (1024.0 * 1024.0));
}

// --- The simulation sections ----------------------------------------------
//
// One function per collapsing header (UI detour step 3). These were an
// unbroken run of statements inside drawUI()'s controls window; splitting them
// costs nothing and is what lets `app::controlsSections()` decide the order
// and the default-open set as *data* rather than by where a statement happens
// to sit in a 200-line block. Every labelled control goes through ctlSlider()
// so its name cannot be clipped by the panel edge -- four of them were.

// docs/ui.md section 3.3, and PRD **L4** (P0): "The colour panel has RGB and
// PIGMENT modes; PIGMENT selects physical constants, not just a colour."
//
// The reconciliation this section exists for, in the design's own words: the
// wireframe's COLOR panel was HSV + hex + RGB, "but pigment selection drives
// *physical* constants -- density, staining, granulation -- so Ultramarine and
// Phthalo Blue behave differently at the same RGB." So PIGMENT mode shows
// those three numbers beside the well, because they are the reason the mode
// exists; a well that only set a colour would be RGB mode with fewer options.
//
// The pigment well itself is the swatch row that used to be a 62 px strip
// along the bottom of the window. Same palette, same click, same tooltip --
// what changed is that it is in the panel the design puts colour in, and that
// selecting a pigment now visibly reports what it selected.
//
// **This state used to be panel-local, and that was the bug.** Two file
// statics lived here -- `bool g_colorPigmentMode` and
// `float g_colorRgb[3]` -- on the stated grounds that "which mode the panel is
// in is not a property of the document and no file records it". The first half
// is still true; the conclusion was not. `g_colorRgb` was **read by no file in
// `src/`**: the picker below was fully live, wrote to it every frame, and
// nothing downstream ever looked, which the panel itself admitted in a line of
// body text ("Not yet connected: no tool reads this colour").
//
// Both now live on `app/AppState.hpp`'s `BrushState`, beside the palette index
// they are the alternative to, because the mode does not merely decide what the
// panel draws -- it decides what `foregroundSrgb()` returns and therefore what
// every brush, bucket and gradient in the build lays down. A file static could
// not be reached by `app/StrokeSession`, could not be written by the
// eyedropper, and could not be asserted by `--selftest`.
void drawColorSection(AppState& st) {
  // The mode toggle in the header row, where section 3.3 puts it ("COLOR
  // gains a mode toggle in its header"). Two buttons rather than a combo: the
  // set has two members and will not grow, and the accent then does what the
  // accent is for -- marking which one is on.
  const auto modeButton = [](const char* label, bool on) {
    if (on) {
      float ac[3], fg[3];
      unpackRgb(kAccent, ac);
      unpackRgb(kOnAccent, fg);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(ac[0], ac[1], ac[2], 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(ac[0], ac[1], ac[2], 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(fg[0], fg[1], fg[2], 1.0f));
    }
    const bool pressed = ImGui::SmallButton(label);
    if (on) ImGui::PopStyleColor(3);
    return pressed;
  };
  const bool pigmentMode = st.brush.colorMode == ColorMode::Pigment;
  if (modeButton("PIGMENT", pigmentMode)) st.brush.colorMode = ColorMode::Pigment;
  ImGui::SameLine();
  if (modeButton("RGB", !pigmentMode)) st.brush.colorMode = ColorMode::Rgb;

  const std::vector<Pigment>& palette = defaultPalette();
  // `foregroundPhysicalConstants()` rather than `palette[st.brush.pigment]`:
  // the index is user-settable and an out-of-range one would index past the
  // vector here, which is exactly the read that function exists to make safe.
  // It is also the honest name for what this row is in RGB mode -- the pigment
  // whose *constants* are still in force, not the colour.
  const Pigment& sel = foregroundPhysicalConstants(st.brush);

  if (pigmentMode) {
    // The well. Wrapped to the panel width rather than laid out in one row:
    // the strip this came from was as wide as the window, and a 322 px column
    // is not.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sw = 26.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    float rowX = 0.0f;
    for (size_t i = 0; i < palette.size(); ++i) {
      if (rowX > 0.0f && rowX + sw <= avail) ImGui::SameLine(0.0f, 3.0f);
      else rowX = 0.0f;
      ImGui::PushID(static_cast<int>(i));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw)))
        st.brush.pigment = static_cast<int>(i);
      const Pigment& pg = palette[i];
      dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw),
                        IM_COL32((int)(pg.rgb[0] * 255), (int)(pg.rgb[1] * 255),
                                 (int)(pg.rgb[2] * 255), 255));
      const bool on = st.brush.pigment == static_cast<int>(i);
      dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                  on ? atelierToken(kAccent) : atelierToken(kDivider), 0.0f, 0,
                  on ? kRuleThickness : kDividerThickness);
      ImGui::SetItemTooltip("%s", pg.name);
      ImGui::PopID();
      rowX += sw + 3.0f;
    }
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::TextUnformatted(sel.name);

    // The three constants, which are the whole argument for this mode. One
    // row each rather than doubled up: doubling up ("density ... staining
    // ...") only saved a line at the cost of a ragged second line
    // ("granulation ..." alone, its value landing under a different column
    // than either value above it, since "granulation" and "density" are
    // different lengths). A fixed 12-column label field -- wide enough for
    // "granulation", the longest of the four labels below -- makes every
    // value in this block line up in the same column instead, and there is
    // vertical room to spare for the extra row (this branch's own square is
    // capped by the panel's WIDTH, not its height, in the panels this mode
    // is actually shown in -- see the `else` branch's own comment on the
    // same trade for the mode that matters more here).
    pushAtelierMono();
    ImGui::TextDisabled("density     %.2f", sel.density);
    ImGui::TextDisabled("staining    %.2f", sel.staining);
    ImGui::TextDisabled("granulation %.2f", sel.granulation);
    // "The RGB readout stays visible as the resulting colour, read-only."
    ImGui::TextDisabled("rgb         %.3f %.3f %.3f", sel.rgb[0], sel.rgb[1], sel.rgb[2]);
    popAtelierMono();
  } else {
    // `ColorPicker4()` (which `ColorPicker3` forwards to) sizes its
    // saturation/value square from `CalcItemWidth()` alone -- `sv_picker_size
    // = width - (bars_width + spacing)` (imgui_widgets.cpp) -- and that square
    // is exactly as tall as it is wide. So `SetNextItemWidth(avail.x)` alone
    // already makes the picker responsive to the panel getting narrower, but
    // says nothing about the panel getting SHORTER: a wide-but-short COLOR
    // section (other panels stacked above/below it, or a shallow dock) still
    // asks for a picker as tall as it is wide, which can run past the
    // available height and force a scrollbar -- not "fits the given space".
    //
    // So the square's side is also capped by the available HEIGHT, minus a
    // reserve for what draws below it: this widget's own RGB input row (no
    // `NoInputs` flag is passed, so ImGui draws one) plus this function's own
    // two `TextDisabled` lines after the `ColorPicker3` call -- the
    // paragraph that used to follow them moved behind the panel's "?" button
    // (see the comment at the end of this branch), so it no longer costs
    // space here. Still a generous line-count rather than a measured value,
    // for the same reason as before: overshooting costs a few px of unused
    // space, undershooting reintroduces the scrollbar this exists to avoid.
    //
    // That "few px of unused space" stops being a rounding error once the
    // panel is much TALLER than it is wide: the square is capped at
    // `avail.x` regardless of how much vertical room there is, so a tall
    // dock leaves a second, much bigger gap below `side + reserveBelow`
    // that this reserve was never meant to cover -- content pinned to the
    // top with dead air trailing off underneath it, not "fits the given
    // space" either. `blockHeight` below is the true total height of
    // everything this branch draws (picker, its own input row, the three
    // readout lines); when the panel offers more than that, the whole
    // block is nudged down by half the difference so the leftover space
    // splits evenly above and below it instead of pooling entirely at the
    // bottom. On a panel exactly tall enough for `blockHeight` (the
    // previously-tuned case) the offset is zero and nothing changes.
    constexpr float kRgbPickerMinSide = 96.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float reserveBelow = lineH * 5.0f;  // input row + 3 aligned readout lines + margin
    const float side = std::max(kRgbPickerMinSide, std::min(avail.x, avail.y - reserveBelow));
    const float blockHeight = side + reserveBelow;
    if (avail.y > blockHeight)
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (avail.y - blockHeight) * 0.5f);
    ImGui::SetNextItemWidth(side);
    // Writes straight into `BrushState::rgb`, which is where the foreground
    // lives -- no copy back, no second source of truth. The array is
    // `std::array<float,3>` and ImGui wants a `float*`, so `.data()`; the
    // storage is contiguous by the standard, so this is the same pointer the
    // retired `float g_colorRgb[3]` handed over.
    ImGui::ColorPicker3("##rgb", st.brush.rgb.data(),
                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Float);
    // **This paragraph used to say "Not yet connected: no tool reads this
    // colour", and it was true.** It is now connected -- `foregroundSrgb()`
    // returns this triple in RGB mode, and every route reads it -- so what is
    // left to say is the one thing that is still *not* carried across, said
    // plainly rather than left to be discovered by a wash that granulates
    // unexpectedly. docs/ui.md §3.3 licenses the colour half explicitly ("it
    // maps through RGB->latent, with the caveat ... that the decomposition is
    // plausible rather than true"); it says nothing about the constants,
    // because there is nothing to say: three floats cannot produce them.
    pushAtelierMono();
    ImGui::TextDisabled("density     %.2f", sel.density);
    ImGui::TextDisabled("staining    %.2f", sel.staining);
    ImGui::TextDisabled("granulation %.2f", sel.granulation);
    popAtelierMono();
    // The paragraph that used to sit here permanently ("This colour paints:
    // RGB layers take it exactly...") now lives behind this panel's "?"
    // button (ui/ControlsLayout.cpp's `Color` entry, drawn by
    // `drawPanelGrip()`) -- it is context that matters occasionally, not on
    // every glance at the panel.
  }
}

// --- BRUSH SETTINGS (design "naturalPaint Panels" turn 4, option 4a) --------
//
// The panel is drawn as one of the right column's collapsing sections rather
// than behind the design's COLOR/BRUSH/LAYERS tab strip. **That is a
// deliberate deviation and worth saying out loud**: the column already has a
// navigation idiom, and adopting the tab strip means restructuring LAYERS and
// COMPS too -- which is turns 1 and 3's redesign, not 4a's. The tab strip
// belongs to whoever lands those; 4a's contribution is what goes *inside* the
// BRUSH tab, and that is what is built here.
//
// Two of 4a's parts are absent because the systems under them are: the preset
// header (name, EDITED badge, save/revert -- there are no brush presets) and
// the TEST STROKE footer (there is no off-canvas stroke preview surface).
// Neither is faked; a dead control that looks live is worse than one that is
// not drawn.
//
// **4a's TIP PREVIEW is now present**, and is the one part of this panel that
// is not chrome: `app/DabPreview` rasterises a real dab through
// `dabCoverage()` and `depositTexel()` and this file only uploads and places
// it. That header carries every decision -- three pressures, one shared scale,
// the loaded colour over paper -- and none of them is repeated here.

// --- The tip preview's texture ---------------------------------------------
//
// **ui/DocumentTexture is not reusable for this, and the reasons are structural
// rather than a matter of taste.** It is keyed on `(DocumentId, revision,
// width, height)` and takes an `OpenDocument`; there is no document here. It
// composites through `compositeDocumentStraightHalf()`, which walks a layer
// stack. It uploads `RGBA16Float` **linear** texels, which is correct for the
// document and wrong here -- a linear texture drawn through ImGui's own
// pipeline is the present-transfer defect ui/CanvasQuad.hpp was written about,
// and routing an already-sRGB-encoded preview through ui/CanvasQuad instead
// would encode it a second time. And it holds a canvas-sized f16 mirror and a
// tile-band scratch for an incremental path that a 192x64 image has no use for.
// What is shared with it is the *policy*, not the code: cache on a key, upload
// only on a miss, and make the counters public so a test can prove the cache
// invalidates.
//
// So this is 30 lines of its own: one `RGBA8Unorm` texture, created once,
// re-written when `DabPreviewCache::generation()` moves. `RGBA8Unorm` and not
// its sRGB sibling because `gfx/Context` takes a non-sRGB surface, which makes
// Dear ImGui's gamma uniform 1.0 -- so these bytes reach the screen exactly as
// every chrome colour does, which is what makes `ImGui::Image()` the right call
// here and the wrong one for the document.
//
// Never released, for `g_documentTextures`' reason: gfx/Wgpu.hpp's convention
// is that a GPU object lives for the process, and ImGui's WebGPU backend builds
// its bind group from the view pointer fresh each frame and releases it at the
// end of `RenderDrawData`, so a view that is created once and kept has no
// stale-bind-group hazard at all (which is the hazard DocumentTexture::retired_
// exists for, and it arises only from *replacing* a view).
class DabPreviewTexture {
 public:
  WGPUTextureView viewFor(GpuContext& gpu, const DabPreviewImage& img, uint64_t generation) {
    if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) return nullptr;
    if (texture_ == nullptr) {
      WGPUTextureDescriptor td = {};
      td.label = sv("brush tip preview");
      td.dimension = WGPUTextureDimension_2D;
      td.size = {static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1};
      td.format = WGPUTextureFormat_RGBA8Unorm;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
      view_ = wgpuTextureCreateView(texture_, nullptr);
      // The image is a fixed size (app/DabPreview's constants), so there is no
      // resize path and no retired list. If that ever stops being true this is
      // where the size has to enter the key.
      width_ = img.width;
      height_ = img.height;
    }
    if (generation != uploaded_) {
      WGPUTexelCopyTextureInfo dst = {};
      dst.texture = texture_;
      dst.mipLevel = 0;
      dst.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout = {};
      layout.bytesPerRow = static_cast<uint32_t>(width_) * 4u;
      layout.rowsPerImage = static_cast<uint32_t>(height_);
      const WGPUExtent3D extent = {static_cast<uint32_t>(width_),
                                   static_cast<uint32_t>(height_), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, img.rgba.data(), img.rgba.size(), &layout,
                            &extent);
      uploaded_ = generation;
      ++uploads_;
    }
    return view_;
  }

  uint64_t uploads() const noexcept { return uploads_; }

 private:
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  // `DabPreviewCache::generation()` starts at 0 and is bumped before the first
  // image is handed out, so 0 here is unambiguously "nothing uploaded yet" and
  // the first frame always writes.
  uint64_t uploaded_ = 0;
  uint64_t uploads_ = 0;
};


// --- The test stroke's texture ---------------------------------------------
//
// `DabPreviewTexture` above is not reused because it hard-codes its extent on
// first upload and has no resize path -- its own comment says so, and says
// this is where a second size would have to enter the key. Rather than give
// that class a size key it does not need (the dab strip genuinely never
// changes size), this is the same thirty lines at the stroke strip's own
// dimensions. The policy is identical and stated once, there: cache on a key,
// upload only on a miss, keep the counters public so a test can prove the
// cache invalidates.
class StrokePreviewTexture {
 public:
  WGPUTextureView viewFor(GpuContext& gpu, const StrokePreviewImage& img, uint64_t generation) {
    if (img.width <= 0 || img.height <= 0 || img.rgba.empty()) return nullptr;
    if (texture_ == nullptr) {
      WGPUTextureDescriptor td = {};
      td.label = sv("brush test stroke preview");
      td.dimension = WGPUTextureDimension_2D;
      td.size = {static_cast<uint32_t>(img.width), static_cast<uint32_t>(img.height), 1};
      td.format = WGPUTextureFormat_RGBA8Unorm;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
      view_ = wgpuTextureCreateView(texture_, nullptr);
      width_ = img.width;
      height_ = img.height;
    }
    if (generation != uploaded_) {
      WGPUTexelCopyTextureInfo dst = {};
      dst.texture = texture_;
      dst.mipLevel = 0;
      dst.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout = {};
      layout.bytesPerRow = static_cast<uint32_t>(width_) * 4u;
      layout.rowsPerImage = static_cast<uint32_t>(height_);
      const WGPUExtent3D extent = {static_cast<uint32_t>(width_),
                                   static_cast<uint32_t>(height_), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, img.rgba.data(), img.rgba.size(), &layout, &extent);
      uploaded_ = generation;
      ++uploads_;
    }
    return view_;
  }

  uint64_t uploads() const noexcept { return uploads_; }

 private:
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  uint64_t uploaded_ = 0;
  uint64_t uploads_ = 0;
};

StrokePreviewCache g_strokePreview;
StrokePreviewTexture g_strokePreviewTexture;

// 4a's TEST STROKE footer, finally present -- `app/StrokePreview` paints one
// real stroke through a real `app/StrokeSession` and this file only uploads
// and places it. That header carries every decision (the path, the taper, the
// integer scale, the pigment route) and none is repeated here.
//
// **This sits where the three-dab TIP PREVIEW used to.** The dab preview is
// not deleted -- `app/BrushRowIcon` still draws every library row with it, and
// a 40 px row has no space for a stroke -- but in the editor it answered
// strictly less than this does: spacing, scatter, direction, velocity, fade,
// the dual tip's second stamp, dab overlap, grain and the Minimum Diameter
// floor are all invisible in a stationary dab, and between them that is most
// of what the panel's sliders control. `app/StrokePreview` §1 lists them.
void drawTestStroke(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  const StrokePreviewImage& img = g_strokePreview.imageFor(st.brush, lut);
  const WGPUTextureView view =
      g_strokePreviewTexture.viewFor(gpu, img, g_strokePreview.generation());

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = static_cast<float>(img.width);
  const float h = static_cast<float>(img.height);

  if (view != nullptr) {
    dl->AddImage(reinterpret_cast<ImTextureID>(view), o, ImVec2(o.x + w, o.y + h));
  } else {
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), atelierToken(kChromeDeep));
  }
  dl->AddRect(o, ImVec2(o.x + w, o.y + h), atelierToken(kDivider), 0.0f, 0, kDividerThickness);
  ImGui::Dummy(ImVec2(w, h));

  // The numbers a picture cannot say. DABS is the one worth reading while
  // dragging Spacing: it is literally what that slider changes, and watching
  // it fall from 2186 to 22 explains the picture faster than the picture does.
  pushAtelierMono();
  if (img.refused) {
    ImGui::TextDisabled("REFUSED  %s", img.refusal.c_str());
  } else if (img.scale <= 1) {
    ImGui::TextDisabled("1:1   %zu dabs", img.dabs);
  } else {
    ImGui::TextDisabled("1:%d   %zu dabs", img.scale, img.dabs);
  }
  popAtelierMono();
  // `IsMouseHoveringRect()` covers the image itself (the text label above is
  // the actual last item once `IsItemHovered()` is asked, so it alone would
  // miss most of the swatch) -- a raw geometry test with no delay mechanism
  // of its own, so only the `IsItemHovered()` half of this OR gets
  // `DelayNormal`. Hovering the image directly still shows the tooltip
  // without a delay; this is a known, minor inconsistency rather than a
  // trap this build has a clean fix for.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) ||
      ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h)))
    ImGui::SetTooltip(
        "One test stroke on empty paper -- the same S-curve and the same\n"
        "0 -> 1 -> 0 pressure taper `--brush-sheet` paints every imported\n"
        "preset with, painted here by the real stroke engine.\n"
        "%zu dabs, %zu texels covered.",
        img.dabs, img.texels);
}


// drawDynamicsMatrix() and drawLinkEditor() moved to ui/DynamicsMatrixPanel.cpp --
// see that header's own comment for why, and for the shelved matrix's three-edit
// path back to being live again.

// --- BRUSH LIBRARY ---------------------------------------------------------
//
// Picking a brush and authoring one are different acts done at different
// rates, so they are two panes rather than one. This is the pane that answers
// "which brush", and it is deliberately a plain list: a row is a name, a
// stripe showing the tip's proportions, and the count of what drives it.
//
// **Picking a brush with unsaved edits discards them, and the pane says so
// before it happens rather than after.** The alternative -- auto-saving into
// the preset on the way out -- would mean a brush silently becoming whatever
// it was last nudged into, which is the failure mode that makes people stop
// trusting a library.
//
// **Imported `.abr` libraries (app/BrushLibraryFile.hpp) are drawn by exactly
// the same row code as the built-ins**, under a header carrying the file's
// name, its status and its remove button. That the two kinds of row are one
// code path is the point: a remembered library's rows are drawn from seven
// cached numbers with its `.abr` unread, and if they were drawn by a second,
// simpler path the difference between "loaded" and "not loaded yet" would show
// up as a visual difference the user has to learn to read.
//
// The three things this pane owns that the model beneath it cannot:
//
//   1. **The `+`**, which opens the one typed-path modal this build has (see
//      `requestBrushLibraryImport()` below).
//   2. **The click that is a first use.** `useLibrary()` is called before
//      `lib.active` moves, and `lib.active` moves only on success -- so the
//      pane cannot reach a state where a row is selected and its parameters
//      have not been read. app/BrushLibraryFile.hpp §5.
//   3. **Saying so when that read fails.** A failed load leaves the row where
//      it is, names the file in the line under the list, and offers Retry and
//      Remove. Not a silent no-op, which is what a row that just does nothing
//      when clicked would be.
//
// Every mutation is deferred to after the row loop. `useLibrary()` and
// `unload()` both rewrite `lib.presets`, and the loop is walking a snapshot
// of it -- acting in place would leave the rest of this frame's rows reading
// indices into a vector that had moved under them.

// The pane's own status line: what the last import, use or unload said. Widget
// state, so a file-local global rather than a field on AppState, exactly as
// `g_docStatus` is -- and separate from `g_docStatus` because a brush message
// must not be wiped by the next document operation, nor wipe one.
std::string g_brushLibraryStatus;
// The per-brush report io/AbrBrushes produced for the last library read (PRD
// G9), shown behind a disclosure rather than in the status line: it is one
// line per brush that lost something, and a twelve-brush pack can produce
// twelve of them.
std::vector<std::string> g_brushLibraryNotes;

// Defined with the document dialogs further down this file (search
// `DocPathAction`). Declared here because the BRUSH LIBRARY pane is drawn
// above that block and **must not grow a path-entry UI of its own** -- there
// is exactly one typed-path modal in this build and this is how a third caller
// reaches it.
void requestBrushLibraryImport();

// The hover preview, and the whole of this pane's answer to "cache an icon".
//
// One `DabPreviewCache` and one `DabPreviewTexture` for the entire list, not
// one per row, because **only one row is hovered at a time**. The comment this
// replaced weighed a per-row thumbnail and rejected it on exactly this ground
// -- "N images means either N GPU textures or one atlas plus a per-row UV
// rect" -- and that objection still stands for a thumbnail *in* every row. It
// does not stand for one image beside the row the pointer is on.
//
// The rows themselves keep the bar, and that part of the old decision is
// unchanged and still right: at 18 px a hardness-0.12 dab is an indistinct
// smudge while the bar still says "long and thin" legibly.
//
// What makes this worth the two globals is imported brushes. "Round Bristle
// 03" is a name whose brush you already know; "Kyle's Inker 7" is not, and a
// pack arrives twelve of those at a time. **And the preview draws from the
// cached row, so it works on a library whose `.abr` has not been read** --
// which is the feature: you can see what a brush looks like before paying to
// load the pack it is in.
DabPreviewCache g_rowPreview;
DabPreviewTexture g_rowPreviewTexture;

// One row of the list. Everything except the click handling, which differs
// between a loaded preset and a cached one and is done by the caller.
void drawBrushRowGlyph(const BrushRow& row, bool isActive, bool dimmed) {
  // The tip's proportions, at a glance: a bar whose length tracks the radius
  // and whose height tracks roundness. It is deliberately an obvious
  // abstraction rather than a fake dab, so it cannot be mistaken for one --
  // the real dab is the hover preview above.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  constexpr float kSwatchW = 40.0f, kSwatchH = 18.0f;
  const float frac = std::clamp(row.radius / 60.0f, 0.08f, 1.0f);
  const float h = std::max(2.0f, kSwatchH * row.roundness);
  dl->AddRectFilled(ImVec2(o.x, o.y + (kSwatchH - h) * 0.5f),
                    ImVec2(o.x + kSwatchW * frac, o.y + (kSwatchH + h) * 0.5f),
                    atelierToken(isActive ? kAccent : (dimmed ? kDivider : kTextSecondary)));
  ImGui::Dummy(ImVec2(kSwatchW, kSwatchH));
  ImGui::SameLine();
}

// The hovered row's dab, in a tooltip, over the text that names the brush.
void drawBrushRowTooltip(AppState& st, GpuContext& gpu, const MixboxLut& lut,
                         const BrushPaneRow& r, const BrushLibrary& lib, bool edited) {
  const bool loaded = r.presetIndex != kNoPresetIndex;
  // A loaded preset previews its real pressure family, through the same
  // `dabPreviewTipsFor()` the BRUSH EDITOR uses; a cached row previews at
  // neutral dynamics, because its dynamics are exactly the half that has not
  // been read (app/BrushLibraryFile.hpp §4).
  const std::array<BrushTip, kDabPreviewCells> tips =
      loaded ? brushPresetIconTips(lib.presets[r.presetIndex], st.brush, lut)
             : brushRowIconTips(r.row, st.brush, lut);
  const DabPreviewImage& img = g_rowPreview.imageFor(tips);
  const WGPUTextureView view = g_rowPreviewTexture.viewFor(gpu, img, g_rowPreview.generation());

  ImGui::BeginTooltip();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = static_cast<float>(img.width);
  const float h = static_cast<float>(img.height);
  if (view != nullptr) {
    dl->AddImage(reinterpret_cast<ImTextureID>(view), o, ImVec2(o.x + w, o.y + h));
  } else {
    // No adapter, or a texture that could not be made. Say so with a filled
    // rectangle rather than a gap, for drawTestStroke()'s reason: a gap reads
    // as "this brush deposits nothing".
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), atelierToken(kChromeDeep));
  }
  dl->AddRect(o, ImVec2(o.x + w, o.y + h), atelierToken(kDivider), 0.0f, 0, kDividerThickness);
  ImGui::Dummy(ImVec2(w, h));

  ImGui::Text("%s", r.row.name.c_str());
  ImGui::TextDisabled("radius %.0f px, spacing %.2f r, %u link%s", r.row.radius, r.row.spacing,
                      r.row.linkCount, r.row.linkCount == 1 ? "" : "s");
  if (!loaded) {
    // The honest limit, said where it is noticed rather than only in a header.
    if (r.row.linkCount > 0)
      ImGui::TextDisabled(
          "Not loaded yet -- shown without its %u dynamics. Click to read the library.",
          r.row.linkCount);
    else
      ImGui::TextDisabled("Not loaded yet. Click to read the library.");
  }
  if (loaded && r.presetIndex == lib.active && edited)
    ImGui::TextDisabled("Edited -- picking another brush discards it.");
  ImGui::EndTooltip();
}

// Write the preferences file, and say so only when it fails.
//
// Called after anything that changes what should come back next launch. The
// cost is a few hundred bytes rewritten; `RecentDocuments` is saved on the
// same footing after every document operation, and this file is smaller.
//
// **Not called when there is nothing to remember.** A user who never imports a
// library should not have this application create a settings file, let alone a
// settings directory, because they clicked a built-in brush.
void saveBrushLibraries(AppState& st) {
  if (st.brushLibraries.libraries().empty()) return;
  std::string err;
  if (!st.brushLibraries.saveToFile(defaultBrushLibraryFilePath(), st.brush.brushLibrary, &err))
    g_brushLibraryStatus = err;
}

// PRD G6's user-authored presets (app/UserBrushLibrary.hpp), loaded lazily
// exactly like `st.brushLibraries` above -- and read that module's header
// before touching either call site below, because *where* this runs matters:
//
// **Must complete before `st.brushLibraries.loadFromFile()`'s own first
// call.** That module's `resolveActive()` restores the last active preset by
// counting "the Nth preset with `libraryId == 0`" (app/BrushLibraryFile.hpp's
// `active` line) -- built-ins AND user presets both qualify, so a user
// preset not yet appended to `lib.presets` when that count runs makes the
// count short, landing `active` on the wrong preset or falling back to a
// built-in. `drawBrushLibrarySection()` below calls this first, then
// `st.brushLibraries.loadFromFile()`, in that order, for exactly this
// reason.
//
// **Also called from `drawBrushSection()`**, defensively: Save lives there,
// a collapsed BRUSH LIBRARY pane is a real layout state, and `saveUserBrush-
// Library()` below must never serialise a `lib` that has not yet read
// user-presets.txt's existing content -- that would write back only what is
// in memory this session and silently drop every preset this session never
// touched. Guarded by its own flag, so a frame where both call sites fire
// costs one extra boolean check.
// The dab library, scanned once and then only on demand (app/DabLibrary.hpp
// §2). Cheap by construction -- one `stat()` per file against the index, no
// decode for anything unchanged -- but still not run until something actually
// needs a tip, which is the same lazy gate the two preference stores above use.
// "Reveal in Finder" -- a watched folder nobody can find is not a feature.
//
// **The folder is created here, and this is the one place that creates it.**
// `rescan()` deliberately does not (app/DabLibrary.hpp): a scan that made
// directories because a picker opened would leave folders in the user's
// Application Support for an application they only launched. Pressing a button
// labelled "Reveal" is a different thing entirely -- it is a user asking to be
// shown the folder, and showing them a path that does not exist would be a
// worse answer than making it.
//
// `SDL_OpenURL` on a `file://` URL is what the platform layer already has;
// there is no `open -R` shell-out here and no `NSWorkspace` call, so this is
// the same on every platform SDL supports and adds no dependency.
void revealDabFolder(AppState& st) {
  std::error_code ec;
  std::filesystem::create_directories(st.dabLibrary.userRoot(), ec);
  const std::string url = "file://" + st.dabLibrary.userRoot();
  if (!SDL_OpenURL(url.c_str()))
    g_brushLibraryStatus = "could not open " + st.dabLibrary.userRoot();
}

void ensureDabLibraryScanned(AppState& st) {
  if (st.dabLibraryScanned) return;
  st.dabLibraryScanned = true;
  st.dabLibrary.setRoots(dabUserRootPath(), dabImportedRootPath(), dabIndexPath());
  st.dabLibrary.rescan();
  // The load half of the persistence: `user-presets.txt` stores an id, not a
  // bitmap, so this is what turns a freshly parsed preset's `dabId` into the
  // tip it names. A preset whose tip has been deleted keeps its note and
  // paints procedurally rather than failing to load.
  std::vector<std::string> notes;
  resolveDabIds(st.brush.brushLibrary, st.dabLibrary, &notes);
  if (!notes.empty() && g_brushLibraryStatus.empty()) g_brushLibraryStatus = notes.front();

  // `--brush-dab-demo <id>`: the same two assignments the picker's own
  // selection makes, and deliberately the same two -- a demo path that set the
  // brush differently from the control it stands in for would photograph
  // something no user can reach.
  if (!st.dabDemoId.empty()) {
    if (auto bitmap = st.dabLibrary.resolve(st.dabDemoId)) {
      st.brush.dabId = st.dabDemoId;
      st.brush.tipBitmap = std::move(bitmap);
    } else {
      g_brushLibraryStatus = "--brush-dab-demo: no dab with id '" + st.dabDemoId + "'";
    }
    st.dabDemoId.clear();
  }
}

void ensureUserBrushLibraryLoaded(AppState& st) {
  if (st.userBrushLibraryLoaded) return;
  st.userBrushLibraryLoaded = true;
  std::string err;
  if (!st.userBrushLibrary.loadFromFile(defaultUserPresetsFilePath(), st.brush.brushLibrary,
                                        &err) &&
      !err.empty())
    g_brushLibraryStatus = err;
  ensureDabLibraryScanned(st);
}


// Write user-presets.txt, and say so only when it fails -- saveBrushLibraries()'s
// shape, for the file app/UserBrushLibrary.hpp §4 durability-hardens instead
// (write-to-temp-then-rename): what is lost if this write is interrupted is
// the user's own work, not a re-importable `.abr` reference.
//
// Called after Save, Duplicate and Delete wherever they touch a preset the
// user owns. **Not gated on "is there anything to write"**, unlike
// `saveBrushLibraries()`: that guard is there so an install that only ever
// picks built-ins never creates a settings file. Reaching this function at
// all means one of those three buttons just ran on a user-owned preset --
// including Delete removing the last one, where writing the now-empty file
// is what makes the deletion stick rather than the brush walking back in
// next launch (`unload()`'s own unconditional write is the same call, same
// reasoning, one file over).
void saveUserBrushLibrary(AppState& st) {
  ensureUserBrushLibraryLoaded(st);
  std::string err;
  if (!st.userBrushLibrary.saveToFile(defaultUserPresetsFilePath(), st.brush.brushLibrary, &err))
    g_brushLibraryStatus = err;
}

void drawBrushLibrarySection(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  BrushLibrary& lib = st.brush.brushLibrary;
  const bool edited = brushIsEdited(st.brush);

  // **user-presets.txt FIRST, then the `.abr` registry** -- `ensureUserBrush-
  // LibraryLoaded()`'s own comment explains why the order is load-bearing:
  // the registry's `resolveActive()` counts user presets among the built-ins
  // when it restores which preset was active, so they must already be in
  // `lib.presets` before it runs.
  ensureUserBrushLibraryLoaded(st);

  // **The preferences, on the first frame this pane is drawn -- and nothing
  // else.** `recentDocumentsLoaded` above does the same for the same reason
  // (PRD A2, ADR-0001: a file nobody asked for costs nothing). This reads a
  // few hundred bytes of text and `stat()`s each remembered library; it opens
  // no `.abr`.
  if (!st.brushLibrariesLoaded) {
    st.brushLibrariesLoaded = true;
    std::string err;
    if (!st.brushLibraries.loadFromFile(defaultBrushLibraryFilePath(), lib, &err) &&
        !err.empty())
      g_brushLibraryStatus = err;
  }

  // --- The `+` ------------------------------------------------------------
  //
  // At the top of the pane rather than on the CollapsingHeader itself: the
  // header is emitted by the generic section loop below, and putting a button
  // in it would mean either a per-section exception in that loop or a button
  // on every section. One row here costs the same 18 px and belongs to this
  // pane alone.
  if (ImGui::SmallButton("+")) requestBrushLibraryImport();
  ImGui::SameLine();
  ImGui::TextDisabled("Import a Photoshop .abr library");
  // The click branch stays undelayed on purpose: a click is a deliberate
  // gesture, not an incidental pass-through, so there is nothing to guard it
  // from -- only the hover branch gets the stationary+delay treatment.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal) || ImGui::IsItemClicked())
    ImGui::SetTooltip(
        "Imports the brush parameters -- size, spacing, roundness, angle, the dynamics,\n"
        "the paper texture -- and the sampled bitmap tips, which are written out to the\n"
        "dab library so they survive unloading this pack. Anything that could not be\n"
        "brought across is named in the import's own notes.");
  ImGui::Separator();

  const std::vector<BrushPaneRow> rows = st.brushLibraries.paneRows(lib);

  // Deferred, because both of these rewrite `lib.presets` and the loop below
  // is walking a snapshot of it.
  size_t clickedRow = static_cast<size_t>(-1);
  uint32_t unloadRequest = 0;
  uint32_t retryRequest = 0;

  const uint32_t pendingLib = st.brushLibraries.pendingActiveLibrary();
  const size_t pendingRow = st.brushLibraries.pendingActiveRow();

  const auto drawRow = [&](const BrushPaneRow& r, size_t index) {
    ImGui::PushID(static_cast<int>(index));
    const bool loaded = r.presetIndex != kNoPresetIndex;
    const bool isActive = loaded && r.presetIndex == lib.active;
    const bool broken = r.status == BrushLibraryStatus::Missing ||
                        r.status == BrushLibraryStatus::Failed;
    drawBrushRowGlyph(r.row, isActive, !loaded);

    // The marker for "this is the brush you were on when you quit", which §5
    // of app/BrushLibraryFile.hpp deliberately does NOT restore -- restoring it
    // would mean reading a `.abr` during startup. A dot is enough to find the
    // row again; anything louder would read as an error.
    std::string label = r.row.name;
    if (r.libraryId != 0 && r.libraryId == pendingLib && r.rowIndex == pendingRow)
      label = "\xc2\xb7 " + label;

    if (broken) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    else if (!loaded) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(
                                                atelierToken(kTextSecondary)));
    if (ImGui::Selectable(label.c_str(), isActive)) clickedRow = index;
    if (broken || !loaded) ImGui::PopStyleColor();

    // `drawBrushRowTooltip()` draws its own tooltip window via
    // BeginTooltip()/EndTooltip() rather than SetTooltip(), so it does not
    // pick up style.HoverFlagsForTooltipMouse's delay on its own -- the
    // delay has to live on this gate instead.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      drawBrushRowTooltip(st, gpu, lut, r, lib, edited);
    ImGui::PopID();
  };

  // Built-ins and anything Duplicate made, first and unheaded: they are what
  // the pane has always listed and they belong to no file.
  for (size_t i = 0; i < rows.size(); ++i)
    if (rows[i].libraryId == 0) drawRow(rows[i], i);

  // Then one group per imported library.
  for (const RememberedLibrary& entry : st.brushLibraries.libraries()) {
    ImGui::PushID(static_cast<int>(entry.id) + 0x40000);
    ImGui::Separator();
    pushAtelierMono();
    ImGui::TextUnformatted(entry.displayName().c_str());
    popAtelierMono();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu | %s", entry.rows.size(), brushLibraryStatusName(entry.status));
    ImGui::SetItemTooltip("%s", entry.path.c_str());

    ImGui::SameLine();
    // Remove, at the right of the group's own header, so it is unambiguous
    // *which* library it removes -- a Remove button in the pane's footer would
    // act on whatever happened to be selected.
    if (ImGui::SmallButton("Remove")) unloadRequest = entry.id;
    ImGui::SetItemTooltip("Removes this library's brushes and forgets it. Brushes you made\n"
                        "with Duplicate are yours and stay.");
    if (entry.status == BrushLibraryStatus::Missing ||
        entry.status == BrushLibraryStatus::Failed) {
      ImGui::SameLine();
      // The recovery path for a library whose file has gone. Clicking any of
      // its rows retries too; this exists so the action is visible without
      // having to guess that a click would do anything.
      if (ImGui::SmallButton("Retry")) retryRequest = entry.id;
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
      ImGui::TextWrapped("%s", entry.failure.c_str());
      ImGui::PopStyleColor();
    } else if (entry.status == BrushLibraryStatus::Stale) {
      ImGui::TextDisabled("The file has changed since these were listed; picking one re-reads it.");
    }
    ImGui::PopID();

    for (size_t i = 0; i < rows.size(); ++i)
      if (rows[i].libraryId == entry.id) drawRow(rows[i], i);
  }

  // --- The deferred mutations --------------------------------------------

  if (clickedRow < rows.size()) {
    const BrushPaneRow r = rows[clickedRow];
    if (r.presetIndex != kNoPresetIndex) {
      if (r.presetIndex != lib.active) {
        lib.active = r.presetIndex;
        applyPresetToBrush(lib.presets[r.presetIndex], st.brush);
        st.brushLibraries.clearPendingActive();
        saveBrushLibraries(st);
      }
    } else {
      // **First use** (app/BrushLibraryFile.hpp §5). The load happens first and
      // `lib.active` moves only if it succeeded, which is what makes "a row is
      // selected but its parameters have not loaded" unreachable.
      const BrushLibraryLoadResult loadResult =
          st.brushLibraries.useLibrary(r.libraryId, lib);
      g_brushLibraryStatus = loadResult.status;
      g_brushLibraryNotes = loadResult.notes;
      if (loadResult.ok) {
        // Find the row again now that the presets exist. Re-derived rather
        // than assumed to be at `presets.size() - count + rowIndex`: a re-read
        // of a changed file can hold a different number of brushes, and
        // arithmetic that assumed otherwise would select the wrong one.
        const std::vector<BrushPaneRow> after = st.brushLibraries.paneRows(lib);
        size_t landed = kNoPresetIndex;
        size_t lastOfLibrary = kNoPresetIndex;
        for (const BrushPaneRow& a : after) {
          if (a.libraryId != r.libraryId || a.presetIndex == kNoPresetIndex) continue;
          lastOfLibrary = a.presetIndex;
          if (a.rowIndex == r.rowIndex) landed = a.presetIndex;
        }
        if (landed == kNoPresetIndex && lastOfLibrary != kNoPresetIndex) {
          // The file lost brushes since its rows were cached, and the one that
          // was clicked is one of them. The last brush of the library is a
          // real, deliberate landing place; saying nothing and leaving the
          // selection where it was would look like the click missed.
          landed = lastOfLibrary;
          g_brushLibraryStatus +=
              " -- the brush that was listed here is no longer in the file; selected the "
              "last one instead.";
        }
        if (landed != kNoPresetIndex) {
          lib.active = landed;
          applyPresetToBrush(lib.presets[landed], st.brush);
        }
        st.brushLibraries.clearPendingActive();
        saveBrushLibraries(st);
      }
      // On a failure: `lib.active` has not moved and `st.brush` has not been
      // touched, so nothing became something else. `g_brushLibraryStatus`
      // names the file and the group above now draws Retry beside it.
    }
  }

  if (retryRequest != 0) {
    const BrushLibraryLoadResult r = st.brushLibraries.useLibrary(retryRequest, lib);
    g_brushLibraryStatus = r.status;
    g_brushLibraryNotes = r.notes;
    if (r.ok) saveBrushLibraries(st);
  }

  if (unloadRequest != 0) {
    std::string message;
    st.brushLibraries.unload(unloadRequest, lib, &message);
    g_brushLibraryStatus = message;
    g_brushLibraryNotes.clear();
    // Written unconditionally, including down to an empty list: the whole
    // point of Remove is that the library does not come back next launch, and
    // skipping the write when the list empties is how the last unload would
    // silently fail to stick.
    std::string err;
    if (!st.brushLibraries.saveToFile(defaultBrushLibraryFilePath(), lib, &err))
      g_brushLibraryStatus = err;
  }

  ImGui::Separator();
  if (!g_brushLibraryStatus.empty()) {
    ImGui::TextWrapped("%s", g_brushLibraryStatus.c_str());
    if (!g_brushLibraryNotes.empty()) {
      // PRD G9's report. Collapsed, because it is one line per brush that lost
      // something and a twelve-brush pack can produce twelve -- but present,
      // because "the imported brush does less than the original" is exactly
      // what an import must not do quietly.
      if (ImGui::TreeNode("What the import could not bring across")) {
        for (const std::string& note : g_brushLibraryNotes)
          ImGui::BulletText("%s", note.c_str());
        ImGui::TreePop();
      }
    }
  }

  if (ImGui::Button("Duplicate")) {
    // From the LIVE brush, not from the stored preset: duplicating is the
    // sanctioned way to keep an edit without overwriting what it came from,
    // so it has to capture what is on screen.
    BrushPreset made = presetFromBrush(
        uniquePresetName(lib, lib.active < lib.presets.size()
                                  ? lib.presets[lib.active].name
                                  : std::string("Brush")),
        st.brush);
    lib.presets.push_back(made);
    lib.active = lib.presets.size() - 1;
    // PRD G6: a Duplicate is a brush the user now owns, and must survive a
    // quit exactly like a Save does -- see saveUserBrushLibrary()'s comment
    // above. Written immediately rather than waiting for an edit, because a
    // Duplicate that is not yet edited has nothing for Save's `edited` gate
    // to enable (`presetFromBrush()` captured the live brush, so the new
    // preset already matches it): without this call, a brush kept by
    // Duplicate alone and never subsequently nudged would be exactly as
    // unpersisted as A7 originally described.
    saveUserBrushLibrary(st);
  }
  ImGui::SameLine();
  // Never the last one: a library with no rows has nothing to pick, and the
  // live brush would be left pointing at an index that does not exist.
  //
  // **And never one that belongs to an imported library.** A library's rows
  // are a cache of what its `.abr` contains; deleting one brush out of a pack
  // would leave the cache describing a preset list that no longer exists, and
  // the next launch would bring the brush back from the file anyway -- a
  // delete that appears to work and silently undoes itself. Remove takes the
  // whole library; Duplicate is how one brush from it is kept.
  const bool activeIsImported =
      lib.active < lib.presets.size() && lib.presets[lib.active].libraryId != 0;
  ImGui::BeginDisabled(lib.presets.size() <= 1 || activeIsImported);
  if (ImGui::Button("Delete")) {
    if (lib.active < lib.presets.size()) {
      lib.presets.erase(lib.presets.begin() + static_cast<std::ptrdiff_t>(lib.active));
      if (lib.active >= lib.presets.size()) lib.active = lib.presets.size() - 1;
      applyPresetToBrush(lib.presets[lib.active], st.brush);
      saveBrushLibraries(st);
      // PRD G6: if the deleted preset was one of the user's own,
      // user-presets.txt must stop describing it -- and unconditionally,
      // same reasoning as `unload()`'s own unconditional write: deleting the
      // very last user preset has to leave a file with none, not skip the
      // write and let the last one silently walk back in next launch. A
      // deleted built-in costs this call nothing: `serialize()` only ever
      // describes presets with `libraryId == 0 && !builtin`, so a built-in
      // was never in this file to remove.
      saveUserBrushLibrary(st);
    }
  }
  ImGui::EndDisabled();
  if (activeIsImported)
    ImGui::SetItemTooltip(
        "This brush belongs to an imported library, which mirrors its file.\n"
        "Duplicate it to keep a copy of your own, or Remove the whole library.");

  if (edited) {
    pushAtelierMono();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)),
                       "EDITED -- picking another brush discards it");
    popAtelierMono();
  }
}

}  // namespace

// --- The brush groups ------------------------------------------------------
//
// **Declared in ui/BrushSettingsWindow.hpp and defined here**, with external
// linkage on purpose: the settings window calls the same code the docked
// column does, so the two surfaces cannot drift into disagreeing about their
// own brush. That header's section 2 gives the full reason the definitions
// are here rather than there -- each group reaches this file's own file-local
// helpers (`drawTestStroke()`, `ensureDabLibraryScanned()`,
// `revealDabFolder()`, `saveUserBrushLibrary()`, the `ctlSlider()` family and
// its shared widest-label column), and hoisting those out of an 11,000-line
// file to satisfy a header would be a far larger and riskier change than
// declaring five functions. Those helpers stay in the unnamed namespace above
// and stay reachable from here, because an unnamed namespace is visible to
// the namespace enclosing it.
//
// `drawBrushSection()` at the bottom calls them in column order; the window
// calls them one per tab.

namespace {

// --- The presentation-table dispatch --------------------------------------
//
// `ui/BrushFieldPresentation.hpp`'s table says WHICH leaves get a control and
// what each one is called and ranged; these functions say HOW to draw one,
// dispatched on the leaf's own C++ type. Overloaded rather than templated or
// switched, matching `brush/BrushModelIo.cpp`'s `toFieldString()`/
// `parseField()` overload set for the identical reason: the set of types a
// `BrushModel` leaf can be is CLOSED (bool, int32_t, float, std::string,
// `VarianceControl`, `CoverageBlend`), so a leaf of some new type added to
// `BrushModel` with no matching overload here fails to COMPILE rather than
// silently drawing nothing or picking the wrong widget.

void drawBrushFieldControl(const BrushFieldSpec& spec, bool& v) { ImGui::Checkbox(spec.label, &v); }

void drawBrushFieldControl(const BrushFieldSpec& spec, int32_t& v) {
  int iv = v;
  if (ctlSliderInt(spec.label, &iv, spec.iLo, spec.iHi)) v = iv;
}

void drawBrushFieldControl(const BrushFieldSpec& spec, float& v) {
  ctlSlider(spec.label, &v, spec.lo, spec.hi, spec.fmt);
}

void drawBrushFieldControl(const BrushFieldSpec& spec, std::string& v) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), "%s", v.c_str());
  if (spec.readOnly) {
    ImGui::BeginDisabled(true);
    ctlInputText(spec.label, buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
    ImGui::EndDisabled();
  } else if (ctlInputText(spec.label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
    v = buf;
  }
}

void drawBrushFieldControl(const BrushFieldSpec& spec, VarianceControl& v) {
  if (ctlBeginCombo(spec.label, varianceControlName(v))) {
    for (int i = 0; i <= static_cast<int>(VarianceControl::InitialDirection); ++i) {
      const auto candidate = static_cast<VarianceControl>(i);
      const bool selected = v == candidate;
      if (ImGui::Selectable(varianceControlName(candidate), selected)) v = candidate;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

void drawBrushFieldControl(const BrushFieldSpec& spec, CoverageBlend& v) {
  if (ctlBeginCombo(spec.label, coverageBlendName(v))) {
    for (int i = 0; i <= static_cast<int>(CoverageBlend::LinearHeight); ++i) {
      const auto candidate = static_cast<CoverageBlend>(i);
      const bool selected = v == candidate;
      if (ImGui::Selectable(coverageBlendName(candidate), selected)) v = candidate;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

// Draws every leaf of `st.brush.model` whose path starts with `prefix`, in
// `visitBrushModelFields()`'s own walk order, skipping:
//   * any path found in `skipPaths` -- already drawn by a hand-written
//     control elsewhere on the SAME tab (Tip Shape's Radius/Angle/Roundness/
//     Spacing/Hardness sliders, which use different units than a raw model
//     leaf would);
//   * any path with no row in `brushFieldPresentationTable()` -- silently,
//     because `runBrushPanelBindingTest()` is what proves that means the
//     path is instead on the omission table, not that it was forgotten.
//
// **`Variance::fadeSteps` is drawn disabled, with a reason, whenever its own
// sibling `control` is not `Fade`** (Variance.hpp's own comment: fadeSteps
// only matters under Fade). This walk can do that with one piece of local
// state because `visitVariance()` (brush/BrushModelFields.hpp) always visits
// a Variance's five leaves in the fixed order control/jitter/minimum/
// fadeSteps/present, so the most recently seen `VarianceControl` is always
// the fadeSteps leaf's own sibling, never a different Variance's.
void drawBrushModelFieldsForPrefix(AppState& st, const char* prefix,
                                    const std::set<std::string>& skipPaths = {}) {
  const size_t prefixLen = std::strlen(prefix);
  VarianceControl lastControl = VarianceControl::Off;
  visitBrushModelFields(st.brush.model, [&](const std::string& path, auto& leaf) {
    if (path.compare(0, prefixLen, prefix) != 0) return;
    if (skipPaths.count(path) != 0) return;
    using LeafType = std::decay_t<decltype(leaf)>;
    if constexpr (std::is_same_v<LeafType, VarianceControl>) lastControl = leaf;
    const BrushFieldSpec* spec = findBrushFieldSpec(path);
    if (spec == nullptr) return;
    if constexpr (std::is_same_v<LeafType, int32_t>) {
      const bool isFadeSteps =
          path.size() > 10 && path.compare(path.size() - 10, 10, ".fadeSteps") == 0;
      if (isFadeSteps) {
        const bool relevant = lastControl == VarianceControl::Fade;
        ImGui::BeginDisabled(!relevant);
        drawBrushFieldControl(*spec, leaf);
        ImGui::EndDisabled();
        if (!relevant) ImGui::TextDisabled("Only used when Control is Fade.");
        return;
      }
    }
    drawBrushFieldControl(*spec, leaf);
  });
}

// Draws `path`'s own control (a checkbox, in practice -- every panel's own
// `enabled` leaf is a bool) unconditionally, live, regardless of any
// enclosing BeginDisabled(). Used for the one control on each gated tab that
// must stay clickable even while the rest of the tab is greyed out: the
// checkbox that turns the greying off.
void drawBrushModelField(AppState& st, const char* path) {
  visitBrushModelFields(st.brush.model, [&](const std::string& p, auto& leaf) {
    if (p != path) return;
    const BrushFieldSpec* spec = findBrushFieldSpec(p);
    if (spec == nullptr) return;
    drawBrushFieldControl(*spec, leaf);
  });
}

}  // namespace

void drawBrushPresetHeader(AppState& st) {
  // Defensive, not the primary load site (that is drawBrushLibrarySection()
  // above): the BRUSH LIBRARY pane can be collapsed while this one is drawn,
  // and Save below must never write user-presets.txt from a `lib` that has
  // not read the file's existing content -- see ensureUserBrushLibraryLoaded()'s
  // own comment. Idempotent, so this costs one boolean check on every other
  // frame.
  ensureUserBrushLibraryLoaded(st);

  // --- The preset header: which brush this is, and whether it still is ----
  BrushLibrary& lib = st.brush.brushLibrary;
  if (lib.active < lib.presets.size()) {
    pushAtelierMono();
    ImGui::TextUnformatted(lib.presets[lib.active].name.c_str());
    popAtelierMono();
    const bool edited = brushIsEdited(st.brush);
    if (edited) {
      ImGui::SameLine();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)), "EDITED");
    }
    // **Save is refused on a brush that belongs to an imported library**, for
    // drawBrushLibrarySection()'s Delete reason and one more: this build does
    // not write `.abr` files, so there is nowhere for the edit to go. Kept as
    // a shadow edit it would survive until the next launch and then be
    // silently replaced by what the file says -- work lost with no message.
    // Duplicate takes one click and produces a preset the user owns, which is
    // where an edit belongs. Revert stays enabled: throwing an edit away needs
    // no file.
    const bool fromLibrary = lib.presets[lib.active].libraryId != 0;
    // **A built-in is never overwritten** -- PRD G6 / A7, and app/
    // UserBrushLibrary.hpp §3's full reasoning. `BrushPreset::builtin`
    // (brush/Library.hpp) is what tells "one of the four shipped defaults"
    // apart from "a preset the user made", since `libraryId == 0` is true of
    // both. Save on a built-in forks a new preset instead of shadowing it,
    // exactly what Duplicate does, so the button's label says so.
    const bool isBuiltIn = lib.presets[lib.active].builtin;
    ImGui::BeginDisabled(!edited || fromLibrary);
    // Overwrites the active preset with what is on screen and persists it
    // (app/UserBrushLibrary.hpp) when it is already the user's own; forks a
    // new one, named uniquely off the current preset, when it is a built-in.
    // Revert throws the edit away. Both buttons are disabled when there is
    // no edit, so neither is a button that does nothing.
    if (ImGui::Button(isBuiltIn ? "Save As New" : "Save")) {
      if (isBuiltIn) {
        BrushPreset made =
            presetFromBrush(uniquePresetName(lib, lib.presets[lib.active].name), st.brush);
        lib.presets.push_back(made);
        lib.active = lib.presets.size() - 1;
      } else {
        lib.presets[lib.active] = presetFromBrush(lib.presets[lib.active].name, st.brush);
      }
      saveUserBrushLibrary(st);
    }
    ImGui::EndDisabled();
    if (fromLibrary)
      ImGui::SetItemTooltip(
          "'%s' came from an imported .abr, and this build does not write .abr files.\n"
          "Duplicate keeps your version as a brush of your own.",
          lib.presets[lib.active].name.c_str());
    if (isBuiltIn && edited)
      ImGui::SetItemTooltip(
          "'%s' is a built-in brush, so it cannot be overwritten -- there would be no way\n"
          "back to it. This makes a brush of your own with these settings, the same as\n"
          "Duplicate, and saves it.",
          lib.presets[lib.active].name.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!edited);
    if (ImGui::Button("Revert")) applyPresetToBrush(lib.presets[lib.active], st.brush);
    ImGui::EndDisabled();
    ImGui::Separator();
  }
}

void drawBrushTipShapeGroup(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  // --- TIP ---------------------------------------------------------------
  // Radius, hardness and spacing already existed; roundness is new with this
  // panel. Spacing is in radii, and the design's caption is the reason it can
  // be: dabs are spaced by arc length, not by time, so the number means the
  // same thing however fast the pen moves.
  //
  // The preview goes ABOVE the sliders, which is 4a's own order and is also
  // the useful one: a slider drag is judged by what happens above it, and a
  // preview below the controls is under the hand that is dragging them.
  drawTestStroke(st, gpu, lut);
  // kBrushRadiusMin/Max (app/AppState.hpp): the one range for this field,
  // also read by the options bar's SIZE slider. See that constant's comment.
  //
  // `st.brush.radius`/`hardness`/`spacing`/`roundness`/`angle` are gone
  // (Part 5) -- each is now a projection of `st.brush.model.tip`, read into
  // a local before the slider (which needs a `float*` it can write through
  // every frame) and written back after, in this widget's own units
  // (radius is half of `diameterPx`; spacing is a 0..1 fraction of
  // `spacingPercent`).
  {
    float radius = st.brush.model.tip.diameterPx / 2.0f;
    ctlSlider("Radius", &radius, kBrushRadiusMin, kBrushRadiusMax, "%.0f px");
    st.brush.model.tip.diameterPx = radius * 2.0f;
  }
  // kBrushHardnessMin/Max (app/AppState.hpp): the one range for this field,
  // also read by the options bar's HARD slider.
  ctlSlider("Hardness", &st.brush.model.tip.hardness, kBrushHardnessMin, kBrushHardnessMax);
  {
    // `spacing` here is in RADII (the slider's own "%.2f r" label, and the
    // old, now-deleted `st.brush.spacing` scalar's exact range/unit:
    // 0.02..1.0 r) -- `spacingPercent` is a percentage OF THE DIAMETER, so
    // the boundary conversion is `/100 * 2` one way and `/2 * 100` (`* 50`)
    // the other, not a bare `/100`/`* 100`. See
    // `app/StrokeSession::brushTipFor()`'s own comment on `tip.spacing` for
    // the full argument.
    float spacing = st.brush.model.tip.spacingPercent / 100.0f * 2.0f;
    ctlSlider("Spacing", &spacing, 0.02f, 1.0f, "%.2f r");
    st.brush.model.tip.spacingPercent = spacing * 50.0f;
  }
  ctlSlider("Roundness", &st.brush.model.tip.roundness, 0.05f, 1.0f);
  ctlSlider("Angle", &st.brush.model.tip.angleDeg, -180.0f, 180.0f, "%.0f deg");
  ImGui::TextDisabled("Dabs are spaced by arc length, not by time.");

  // --- The tip itself -----------------------------------------------------
  //
  // Below the shape sliders rather than above them, because the sliders shape
  // whatever tip is chosen and the preview at the top of this section already
  // shows the result: the order reads "here is what it does, here is its
  // shape, here is what it is stamping".
  //
  // **The whole panel is one call into `ui/DabPicker`** -- the merge rule for
  // this file is move code out, never in, and a grid with a hit test and an
  // atlas in it is exactly the kind of thing that should not be another two
  // hundred lines here. It reports; this block decides.
  if (ImGui::TreeNodeEx("Tip", ImGuiTreeNodeFlags_DefaultOpen)) {
    ensureDabLibraryScanned(st);
    const DabPickerAction action = drawDabPicker("brush-tip", st.dabLibrary, gpu, st.brush.dabId);
    if (action.rescanRequested) st.dabLibrary.rescan();
    if (action.revealRequested) revealDabFolder(st);
    if (action.selected) {
      // **The tip, and nothing else** (ui/DabPicker.hpp §3). Radius, spacing,
      // hardness and roundness are left exactly as they were, so trying the
      // next tip along is one click to do and one to undo.
      st.brush.dabId = action.id;
      st.brush.tipBitmap = action.id.empty() ? nullptr : st.dabLibrary.resolve(action.id);
      // An id that resolved a moment ago for the thumbnail and does not now
      // means the file went away between the two. Falling back to the
      // procedural tip AND clearing the id keeps the two in step -- an id
      // naming a bitmap the brush is not using is what would get saved.
      if (!action.id.empty() && st.brush.tipBitmap == nullptr) st.brush.dabId.clear();
    }
    if (action.useNativeSize && action.nativeWidth > 0 && action.nativeHeight > 0) {
      // The tip's own larger dimension is its DIAMETER; Radius is half of it.
      // Same reading `io/AbrBrushes.cpp` gives a `#Prc` diameter, and clamped
      // to the slider's own range rather than to something wider, so the
      // button can never put the brush somewhere the slider cannot express.
      const float diameter = static_cast<float>(std::max(action.nativeWidth, action.nativeHeight));
      st.brush.model.tip.diameterPx =
          std::clamp(diameter * 0.5f, kBrushRadiusMin, kBrushRadiusMax) * 2.0f;
    }
    if (action.useFileSpacing)
      // `action.fileSpacing` is already in the same RADII unit the slider
      // above uses (`ui/DabPicker.cpp`'s own conversion off the dab
      // library's `spacingPercent` field) -- `* 50` is the radii ->
      // percent-of-diameter boundary conversion, not `* 100` (this group's
      // Spacing slider, just above, names the same factor of two).
      st.brush.model.tip.spacingPercent = std::clamp(action.fileSpacing, 0.02f, 1.0f) * 50.0f;
    ImGui::TreePop();
  }

  // --- The rest of tip.* ---------------------------------------------------
  //
  // diameterPx/angleDeg/roundness/spacingPercent/hardness are drawn above by
  // hand, in different units (radius, spacing-in-radii) than a raw model
  // leaf would use -- skipped here so this walk cannot draw a second,
  // differently-scaled control for the same field. `tip.computed` is on
  // `ui/BrushFieldPresentation`'s omission table, so it is simply absent
  // from `brushFieldPresentationTable()` and this walk skips it on its own.
  drawBrushModelFieldsForPrefix(
      st, "tip.",
      {"tip.diameterPx", "tip.angleDeg", "tip.roundness", "tip.spacingPercent", "tip.hardness"});
}

void drawBrushPaintGroup(AppState& st) {
  // kBrushLoadMin/Max (app/AppState.hpp): the one range for this field, also
  // read by the options bar's LOAD slider.
  ctlSlider("Load", &st.brush.load, kBrushLoadMin, kBrushLoadMax);

  // **WET reaches sim::PaintSim's `brushWater` and nothing else.**
  // `applyToolToBrush()` (:742) is the only reader of `st.brush.wetness`, and
  // its only call site is the canvas block's `paintTool && ...` branch below,
  // which is reached exclusively when `strokeRouteFor()` answers
  // `StrokeRoute::PaintSim` -- the Water tool always, or Brush/DryBrush with
  // no document layer to have aimed at (app/StrokeSession.cpp:87). Every
  // layer-writing route -- CpuDeposit, RgbDeposit, RgbErase, PigmentErase --
  // calls `brushTipFor()` instead, and `BrushTip` (app/StrokeSession.hpp) has
  // no water field at all: the slider cannot reach a layer even in
  // principle, not just in this build's current wiring. Same
  // disabled-rather-than-hidden treatment as OPACITY below, for the reason
  // its own comment gives -- and WET is ignored on *more* routes than
  // OPACITY (four, not one), so it deserves at least the same honesty.
  {
    const OpenDocument* od = st.documents.active();
    const Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;
    const StrokeRoute route = strokeRouteFor(st.brush.tool, target);
    const bool honoured = wetnessReachesSolver(route);
    ImGui::BeginDisabled(!honoured);
    // kBrushWetnessMin/Max (app/AppState.hpp): the one range for this field,
    // also read by the options bar's WET slider.
    ctlSlider("Water", &st.brush.wetness, kBrushWetnessMin, kBrushWetnessMax);
    ImGui::EndDisabled();
    if (honoured)
      ImGui::TextDisabled("Water content the solver canvas mixes into this stroke.");
    else
      ImGui::TextDisabled("Water reaches the solver canvas; this stroke goes to %s.",
                          strokeRouteName(route));
  }

  // Opacity is the stroke's CEILING and flow is its rate, which is the one
  // pair in this panel whose difference is invisible from the numbers alone --
  // hence the caption. It reaches three of the four layer-writing routes: an
  // accumulator is what enforces a ceiling (or a floor), and the pigment
  // DEPOSIT is the one route with none, because wet pigment's density is the
  // solver's answer to the same question. Drawn disabled rather than hidden on
  // the route that ignores
  // it, so that a painter who turns it down and sees no change is told why
  // instead of concluding it is broken -- and so the control does not appear
  // and vanish as the active layer changes.
  {
    const OpenDocument* od = st.documents.active();
    const Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;
    const StrokeRoute route = strokeRouteFor(st.brush.tool, target);
    // **The eraser reads this same slider as its STRENGTH** (brush/RgbErase.hpp
    // §2, brush/PigmentErase.hpp §2), so it is live on **both** erase routes.
    // One control with one meaning -- the fraction of the maximum effect one
    // stroke may reach -- rather than a second "strength" number that would
    // leave OPACITY dimmed and inert whenever the eraser was selected, which is
    // the exact complaint this disabled-rather-than-hidden treatment was written
    // to answer.
    //
    // `cpu-deposit` is still the one layer-writing route that ignores it, and
    // still for the stated reason: the pigment DEPOSIT has no per-stroke
    // accumulator, so it has no ceiling to raise or lower. The pigment ERASE
    // does -- `E` is the fraction removed, dimensionless, so the floor
    // `mass_0 * (1 - strength)` needs nothing the deposit was missing.
    const bool erasing =
        route == StrokeRoute::RgbErase || route == StrokeRoute::PigmentErase;
    // **The tonal route reads the same slider as its STRENGTH too**
    // (brush/TonalBrush.hpp §3), for the reason the erase routes do: it already
    // means "the fraction of the maximum effect one stroke may reach" on three
    // routes, and giving Dodge a private "exposure" number would leave OPACITY
    // dimmed and inert whenever a tonal tool was selected -- the exact complaint
    // this disabled-rather-than-hidden treatment exists to answer.
    const bool toning = route == StrokeRoute::TonalBrush;
    const bool smudging = route == StrokeRoute::Smudge;
    // The clone reads it as its per-stroke ceiling too -- the same slider and
    // the same meaning, "the fraction of the maximum effect one stroke may
    // reach" (brush/CloneStamp §1's accumulator is brush/RgbDeposit §2's). Left
    // out of `honoured`, this control would have been dimmed over a sentence
    // saying it did nothing while it in fact set how opaque the copy came out.
    const bool honoured = erasing || toning || smudging ||
                          route == StrokeRoute::RgbDeposit ||
                          route == StrokeRoute::CloneStamp;
    // **And the smudge reads it as its STRENGTH** (brush/Smudge.hpp §3) -- one
    // more reading of the same slider, so it is live on that route too. Its
    // sentence is its own rather than the eraser's, because the number does
    // something visibly different: at 0 the tool is a bit-exact no-op and at 1
    // it carries the colour it picked up at pen-down for the whole stroke with
    // no decay at all, which is not what "how much it takes" describes.
    ImGui::BeginDisabled(!honoured);
    ctlSlider("Opacity", &st.brush.opacity, 0.0f, 1.0f);
    ImGui::EndDisabled();
    if (erasing)
      ImGui::TextDisabled("Flow is how fast it bites; opacity is how much it takes.");
    else if (toning)
      ImGui::TextDisabled("Flow is how fast the tone moves; opacity is how far it goes.");
    else if (smudging)
      ImGui::TextDisabled("Opacity is how far the colour is carried; 0 does nothing.");
    else if (honoured)
      ImGui::TextDisabled("Flow is how fast paint builds; opacity is where it stops.");
    else
      ImGui::TextDisabled("Opacity is a stroke ceiling; this stroke goes to %s.",
                          strokeRouteName(route));
  }

  // --- LOADED PIGMENT ----------------------------------------------------
  // The three constants belong to the PIGMENT (paint/Palette.hpp's
  // `density`/`staining`/`granulation` -- "the real pigment measurements
  // published with Mixbox"), not to the brush, so they are read-only here.
  //
  // **This comment used to claim the PIGMENT section further down the column
  // "edits the solver's own globals, which are a different set of numbers
  // with the same three names" -- that was false.** `st.sim.density` /
  // `staining` / `granulation` are not a second, independent set: main.cpp's
  // simulation block overwrites all three from the active pigment
  // unconditionally, every frame ("Physical constants follow the selected
  // paint, not a global slider", main.cpp:2521-2525), which is the *same*
  // three numbers this block already shows. A slider on them there used to
  // look live and snap back one frame later -- A5 in the reachability audit.
  // drawPigmentSection() now gives them the same disabled, read-only
  // treatment as this block, for the same reason.
  if (ImGui::CollapsingHeader("LOADED PIGMENT", ImGuiTreeNodeFlags_DefaultOpen)) {
    const std::vector<Pigment>& palette = defaultPalette();
    const size_t idx = st.brush.pigment >= 0 &&
                               static_cast<size_t>(st.brush.pigment) < palette.size()
                           ? static_cast<size_t>(st.brush.pigment)
                           : 0;
    const Pigment& p = palette[idx];
    ImGui::ColorButton("##loaded", ImVec4(p.rgb[0], p.rgb[1], p.rgb[2], 1.0f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                       ImVec2(34, 46));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextUnformatted(p.name);
    pushAtelierMono();
    ImGui::Text("DENSITY      %.2f", p.density);
    ImGui::Text("STAINING     %.2f", p.staining);
    ImGui::Text("GRANULATION  %.2f", p.granulation);
    popAtelierMono();
    ImGui::EndGroup();
  }
}

void drawBrushTextureGroup(AppState& st, bool ownPage) {
  // --- PAPER GRAIN ---------------------------------------------------------
  // Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp) -- docs/
  // reachability-audit.md's B10 caught exactly this mistake once already: a
  // feature built and wired into the deposit with no control anywhere in the
  // chrome to turn it on. This is that control.
  //
  // Closed by default, not because grain is unimportant but because it is
  // OFF by default (`GrainParams::enabled`) and a header that opens on a
  // section doing nothing yet reads as broken before anyone has touched it --
  // the same call `ImGuiTreeNodeFlags_DefaultOpen`'s absence makes for every
  // other collapsed-by-default section in this column.
  //
  // **Named "PAPER GRAIN" rather than plain "GRAIN"**, deliberately distinct
  // from the watercolour solver's own "Grain block" slider two panels over
  // (`drawMediumSection()`'s `st.sim.grainBlock`, an unrelated granulation-
  // blocking parameter on the fluid model): the two are different mechanisms
  // on different routes (this one on the CPU layer routes, that one on
  // `sim::PaintSim`), and a user who has seen one "Grain" slider elsewhere
  // must not read this one as the same control moved.
  //
  // `ownPage` short-circuits the header rather than passing it a
  // default-open flag: on a tab there should be no header, not an opened one.
  // ui/BrushSettingsWindow.hpp carries the reasoning.
  //
  // **`ownPage` now also draws a `SeparatorText()` label**, unlike before
  // this task: a tab used to have exactly one topic, so no title was needed
  // (the tab's own label already said what the page was) -- now it has two,
  // PAPER GRAIN and the model.texture.* section appended below, and two
  // controls both named "Enabled" with two different "Scale"/"Depth"
  // sliders under them is genuinely ambiguous without one.
  if (ownPage) ImGui::SeparatorText("PAPER GRAIN");
  if (ownPage || ImGui::CollapsingHeader("PAPER GRAIN")) {
    // **Reaches all four layer-writing routes**, and that is a correction
    // rather than a fact that was always true. This block used to read
    // `honoured = route == StrokeRoute::CpuDeposit` and grey itself out on
    // every other route, because grain genuinely was called from
    // `depositDab()` alone; `brush/RgbDeposit.cpp`, `brush/RgbErase.cpp` and
    // `brush/PigmentErase.cpp` each computed coverage and never asked the
    // paper about it.
    //
    // The texture work closed that gap in the engine -- all four now call
    // `grainCoverageAt()` -- and **left this predicate behind**, which is the
    // worse half of the same defect the disabled-with-a-reason idiom exists to
    // prevent: a control that works, greyed out, over a sentence explaining
    // that it cannot. An RGB layer is what File > New gives you, so that was
    // every stroke most painters make.
    //
    // What is left is a real limit and keeps its honesty: the SOLVER route
    // (`sim::PaintSim`) has no grain of its own, and no CPU coverage for
    // `grainCoverageAt()` to modify.
    //
    // The answer is `grainReachesRoute()` (app/StrokeSession.hpp) rather than
    // a four-way comparison written out here, for the reason that predicate's
    // own comment gives: a private copy of the answer is what went stale in
    // the first place, and there is now a selftest standing behind this one.
    const OpenDocument* od = st.documents.active();
    const Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;
    const StrokeRoute route = strokeRouteFor(st.brush.tool, target);
    const bool honoured = grainReachesRoute(route);
    ImGui::BeginDisabled(!honoured);
    ImGui::Checkbox("Enabled", &st.brush.grain.enabled);
    ImGui::BeginDisabled(!st.brush.grain.enabled);
    // One slider drives both `periodX` and `periodY` -- `GrainParams`'s own
    // comment on why the struct keeps them separate (the patent's NR/NC are
    // independent) while this, the only control surface that writes them,
    // chooses to move them together: a rectangular-period paper is a real
    // thing but not a distinction this panel's first control needs to offer,
    // and one slider is one fewer number for a painter reaching for "make
    // the paper coarser" to reconcile.
    int period = st.brush.grain.periodX;
    if (ctlSliderInt("Scale", &period, 4, 96)) {
      st.brush.grain.periodX = period;
      st.brush.grain.periodY = period;
    }
    ctlSlider("Depth", &st.brush.grain.depth, 0.0f, 1.0f);
    ctlSlider("Strength", &st.brush.grain.strength, 0.0f, 2.0f);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!honoured)
      ImGui::TextDisabled("Grain reaches the layer routes; this stroke goes to %s.",
                          strokeRouteName(route));
    else if (st.brush.grain.enabled)
      ImGui::TextDisabled("Deep valleys fill; peaks get skipped, at the same pressure.");
    else
      ImGui::TextDisabled("Off: every dab covers exactly what its falloff says, paper or not.");
  }

  // --- BrushModel::texture -- Photoshop's imported Texture panel ----------
  //
  // A DIFFERENT struct from PAPER GRAIN above (`st.brush.grain`): this is
  // `model.texture`, what a '.abr' file's own Texture panel carries --
  // pattern, scale, depth, blend mode, brightness/contrast. Not yet read at
  // paint time (`BrushModel.hpp`'s own comment: "imported by nothing until
  // now" still holds for this struct, unlike shape/scatter/transfer, which
  // Phase B/C wired), so its controls are shown live and persist to the
  // model and the saved preset -- the same treatment Tool Options gets, for
  // the same reason: this is ordinary "not wired yet", not the specific,
  // permanent refusals Dual Brush's LinearHeight blend or Color Dynamics
  // carry.
  //
  // `ownPage`-gated for the identical reason PAPER GRAIN's own header is:
  // the docked column has no room for 16 more controls (`ui/
  // BrushSettingsWindow.hpp` §2's asymmetry) -- so this section exists only
  // on the window's own Texture tab.
  if (ownPage) {
    ImGui::SeparatorText("TEXTURE (IMPORTED)");
    drawBrushModelField(st, "texture.enabled");
    const bool enabled = st.brush.model.texture.enabled;
    ImGui::BeginDisabled(!enabled);
    drawBrushModelFieldsForPrefix(st, "texture.", {"texture.enabled"});
    ImGui::EndDisabled();
    if (!enabled)
      ImGui::TextDisabled("Texture is off -- turn it on above to edit these.");
  }
}

// **The two draws below are gated behind `st.showAdvancedDynamics`.** The
// 10x12 LINK MATRIX (`ui/DynamicsMatrixPanel.hpp`) is shelved: nothing that
// paints reads `BrushState::links` any more (brush/BrushModel.hpp's own
// header), so drawing it un-gated would show a live-looking editor over a
// brush property that no longer does anything. `--advanced-dynamics` is the
// flag that reopens it -- see `ui/DynamicsMatrixPanel.hpp` for the
// three-edit path all the way back to it driving a stroke again.
void drawBrushDynamicsGroup(AppState& st) {
  // --- DYNAMICS ----------------------------------------------------------
  if (ImGui::CollapsingHeader("DYNAMICS", ImGuiTreeNodeFlags_DefaultOpen)) {
    pushAtelierMono();
    ImGui::Text("%zu LINKS", st.brush.links.links.size());
    popAtelierMono();
    if (st.showAdvancedDynamics) drawDynamicsMatrix(st);
  }
  if (ImGui::CollapsingHeader("LINK", ImGuiTreeNodeFlags_DefaultOpen))
    if (st.showAdvancedDynamics) drawLinkEditor(st);
}

// --- The six Photoshop-shaped, presentation-table-driven groups -----------
//
// Window-only (`ui/BrushSettingsWindow.hpp` §2's asymmetry) and each built
// the same way: the panel's own `Enabled` checkbox drawn live by hand (so it
// stays clickable while the rest of the tab is greyed out), then a generic
// walk of that panel's own path prefix through `ui/BrushFieldPresentation`'s
// table, disabled with a one-line reason when the panel itself is off. Every
// leaf either gets a control this way or is on the omission table --
// `runBrushPanelBindingTest()` is what proves it.

void drawBrushShapeDynamicsGroup(AppState& st) {
  drawBrushModelField(st, "shape.enabled");
  const bool enabled = st.brush.model.shape.enabled;
  ImGui::BeginDisabled(!enabled);
  drawBrushModelFieldsForPrefix(st, "shape.", {"shape.enabled"});
  ImGui::EndDisabled();
  if (!enabled) ImGui::TextDisabled("Shape Dynamics is off -- turn it on above to edit these.");
}

void drawBrushScatteringGroup(AppState& st) {
  drawBrushModelField(st, "scatter.enabled");
  const bool enabled = st.brush.model.scatter.enabled;
  ImGui::BeginDisabled(!enabled);
  drawBrushModelFieldsForPrefix(st, "scatter.", {"scatter.enabled"});
  ImGui::EndDisabled();
  if (!enabled) ImGui::TextDisabled("Scattering is off -- turn it on above to edit these.");
}

void drawBrushDualBrushGroup(AppState& st) {
  drawBrushModelField(st, "dual.enabled");
  const bool enabled = st.brush.model.dual.enabled;
  ImGui::BeginDisabled(!enabled);

  // Blend Mode stays live inside the `enabled` gate but OUTSIDE the
  // renderability gate below it -- a painter stuck on Linear Height needs
  // this control clickable to get off it.
  drawBrushModelField(st, "dual.blend");

  // **Linear Height is REFUSED, deliberately** (brush/CoverageBlend.hpp's
  // own comment on `CoverageBlend::LinearHeight`): no per-pixel formula in
  // any source consulted, and `applyCoverageBlend()` will not compute it --
  // `coverageBlendIsRenderable()` is the same predicate that function's own
  // caller must check. 29 of 66 dual-brush presets measured name it, so this
  // is not a rare corner: the second tip's shape/scatter controls below are
  // real but currently inert whenever this is the blend, and this says so
  // rather than leaving them live over nothing.
  const bool blendRenderable = coverageBlendIsRenderable(st.brush.model.dual.blend);
  ImGui::BeginDisabled(!blendRenderable);
  drawBrushModelFieldsForPrefix(st, "dual.", {"dual.enabled", "dual.blend"});
  ImGui::EndDisabled();
  if (!blendRenderable)
    ImGui::TextDisabled(
        "Linear Height is refused, deliberately -- no per-pixel formula for it exists in "
        "any source consulted. Pick a different Blend Mode above to edit these.");

  ImGui::EndDisabled();
  if (!enabled) ImGui::TextDisabled("Dual Brush is off -- turn it on above to edit these.");
}

void drawBrushColorDynamicsGroup(AppState& st) {
  // **A permanent fact about the ENGINE, independent of this panel's own
  // `enabled` gate below** -- `StrokeSession::brushTipFor()` passes the
  // Hue/Saturation/Value identity regardless of what is set here (that
  // function's own comment: "future work, not this commit's"), and measured
  // 1 of 101 presets uses this panel at all. So this is a standing caption,
  // not a BeginDisabled(): the controls stay live and persist to the model
  // and the saved preset either way, honestly reflecting that this build
  // remembers Color Dynamics without yet painting from it.
  textDisabledWrapped(
      "naturalPaint does not yet apply Color Dynamics to a stroke. These settings persist "
      "to the model and the saved preset, but are not read when painting.");
  ImGui::Separator();

  drawBrushModelField(st, "color.enabled");
  const bool enabled = st.brush.model.color.enabled;
  ImGui::BeginDisabled(!enabled);
  drawBrushModelFieldsForPrefix(st, "color.", {"color.enabled"});
  ImGui::EndDisabled();
  if (!enabled) ImGui::TextDisabled("Color Dynamics is off -- turn it on above to edit these.");
}

void drawBrushTransferGroup(AppState& st) {
  drawBrushModelField(st, "transfer.enabled");
  const bool enabled = st.brush.model.transfer.enabled;
  ImGui::BeginDisabled(!enabled);
  drawBrushModelFieldsForPrefix(st, "transfer.", {"transfer.enabled"});
  ImGui::EndDisabled();
  if (!enabled) ImGui::TextDisabled("Transfer is off -- turn it on above to edit these.");
}

void drawBrushToolOptionsGroup(AppState& st) {
  // No `enabled` field on `PsToolOptions` -- Tool Options has no off switch
  // in Photoshop either (BrushSettingsTab::ToolOptions's own comment).
  drawBrushModelFieldsForPrefix(st, "options.");

  // The bare top-level checkbox tail -- `noise`/`wetEdges`/`airbrush`/
  // `brushPose`, no `options.` prefix and no Photoshop panel of their own
  // (`BrushModel::noise`'s own comment calls this "the checkbox tail of
  // Photoshop's panel"). Landed on this tab for lack of a more specific one
  // -- see `BrushSettingsTab::ToolOptions`'s own comment on the decision.
  ImGui::Separator();
  textDisabledWrapped(
      "Parsed from the file and carried; none of the four below is applied to a stroke "
      "yet -- io/AbrBrushes.cpp has each one's own refusal reason.");
  drawBrushModelField(st, "noise");
  drawBrushModelField(st, "wetEdges");
  drawBrushModelField(st, "airbrush");
  drawBrushModelField(st, "brushPose");
}

// The docked BRUSH column: every group, in the order it has always drawn
// them. The window (ui/BrushSettingsWindow) draws the same five calls one per
// tab, which is what makes the two surfaces incapable of disagreeing.
void drawBrushSection(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  drawBrushPresetHeader(st);
  drawBrushTipShapeGroup(st, gpu, lut);
  drawBrushPaintGroup(st);
  drawBrushTextureGroup(st, /*ownPage=*/false);
  drawBrushDynamicsGroup(st);
}

namespace {

// A5 (reachability audit): Density, Staining and Granulation used to be live
// `ctlSlider()`s here, and a drag on any of them worked for exactly one
// frame. main.cpp's simulation block runs after `ImGui::Render()` and before
// the sim upload and overwrites all three from the *active pigment's own*
// constants, unconditionally, every frame:
//
//   st.sim.density = pig.density;
//   st.sim.staining = pig.staining;
//   st.sim.granulation = pig.granulation;
//
// with the comment "Physical constants follow the selected paint, not a
// global slider, so switching from Phthalo Blue to Ultramarine actually
// changes behaviour" (main.cpp:2521-2525). That comment is the design
// decision, not a bug to route around, and it is a *domain* question before
// it is a code one: CONTEXT.md does not gloss "Pigment" as a struct, but
// PLAN.md's own step-8 record does, in exactly these words -- describing
// `brushTipFor()`'s deliberate choice not to carry these three fields into
// `brush/Deposit`, "because `brush/Deposit` simulates no settling, lifting
// or granulation and three dead fields would imply a fidelity that is not
// there." Settling, lifting and granulation are properties of the SOLVER's
// simulated paper and water, driven by which real paint is loaded --
// `paint/Palette.hpp`'s header calls the numbers "the real pigment
// measurements published with Mixbox," and real paints differ in exactly
// this way (a staining pigment resists being lifted, a granulating one
// pools in the paper's tooth). A user picks that behaviour by picking a
// pigment, not by dialling a slider independent of one -- the same reading
// `drawBrushSection()`'s LOADED PIGMENT block already gives these three,
// read-only, several hundred lines above this section. Editing them here as
// a per-session override, and *keeping* that override past the next frame,
// is a real alternative -- but nothing in the PRD, CONTEXT.md, the palette
// header or main.cpp's own comment asks for one, and inventing somewhere for
// it to live (a per-preset shadow value? a global multiplier?) would be
// answering a question nobody asked. So the honest fix is the
// disabled-rather-than-hidden treatment `drawBrushSection()` already gives
// OPACITY and WET: the value is real and worth showing, the control here
// just is not what owns it.
void drawPigmentSection(AppState& st) {
  ImGui::BeginDisabled();
  ctlSlider("Density", &st.sim.density, 0.0f, 1.0f);
  ImGui::SetItemTooltip("How fast pigment drops out of suspension.");
  ctlSlider("Staining", &st.sim.staining, 0.02f, 1.0f);
  ImGui::SetItemTooltip("Resistance to being lifted back into the water.");
  ctlSlider("Granulation", &st.sim.granulation, 0.0f, 1.0f);
  ImGui::SetItemTooltip("Affinity for the paper's valleys.");
  ImGui::EndDisabled();
  ImGui::TextDisabled("Owned by the loaded pigment -- pick a different paint to change these.");

  ctlSlider("Diffusion", &st.sim.pigmentDiffuse, 0.0f, 1.0f);
  ImGui::SetItemTooltip("Pigment spreading through the wet film.\n"
                      "At zero the water outruns the pigment and the\n"
                      "leading edge of a wash runs clear.");
}

// The three media share one header and one slot in the column, because exactly
// one of them is ever on screen -- switching medium switches the whole solver
// (main.cpp), so these are alternatives rather than three sections.
void drawMediumSection(AppState& st, PaintSim* sim) {
  ImGui::TextDisabled("%s", paintModeName(st.mode));
  if (st.mode == PaintMode::Oil) {
    ctlSlider("Brush load", &st.sim.brushLoad, 0.0f, 3.0f);
    ImGui::SetItemTooltip("Paint the brush picks up when a stroke starts.\n"
                        "It runs out as you paint, like a real one.");
    ctlSlider("Pressure", &st.sim.penetration, 0.05f, 2.0f);
    ctlSlider("Squish", &st.sim.oilPressure, 0.0f, 4.0f);
    ImGui::SetItemTooltip("vp = -c * grad(penetration): paint pushed out\n"
                        "sideways from under the bristles.");
    ctlSlider("Transfer", &st.sim.xferFraction, 0.0f, 0.5f);
    ctlSlider("Max transfer", &st.sim.maxXfer, 0.0f, 0.1f, "%.4f");
    ctlSlider("Levelling", &st.sim.viscosity, 0.0f, 0.25f);
    ImGui::SetItemTooltip("Wet paint relaxing under surface tension.\n"
                        "At zero the brush stamps leave periodic ridges.");
    ctlSlider("Impasto light", &st.sim.impastoLight, 0.0f, 1.5f);
    ctlSlider("Adhesion", &st.sim.adhesion, 0.0f, 0.4f);
    ImGui::SetItemTooltip("Paint never fully leaves a cell. At zero the\n"
                        "canvas feels like Teflon.");
  } else if (st.mode == PaintMode::Ink) {
    ctlSlider("Relaxation", &st.sim.omega, 0.5f, 1.95f);
    ImGui::SetItemTooltip("LBE omega. Viscosity = (1/omega - 1/2)/3.");
    ctlSlider("Blocking", &st.sim.blocking, 0.0f, 0.9f);
    ImGui::SetItemTooltip("Base permeability of the paper. Higher blocks\n"
                        "more flow and pins the mark's edge.");
    ctlSlider("Grain block", &st.sim.grainBlock, 0.0f, 0.9f);
    ctlSlider("Glue", &st.sim.glue, 0.0f, 0.6f);
    ImGui::SetItemTooltip("Artists add glue to limit spread.");
    ctlSlider("Receptivity", &st.sim.receptivity, 0.1f, 2.5f);
    ImGui::SetItemTooltip("Wet paper takes less ink. Lower this and a second\n"
                        "stroke over a damp mark barely registers.");
    ctlSlider("Settle rate", &st.sim.settleScale, 0.0f, 0.05f, "%.4f");
    ImGui::SetItemTooltip("How fast ink fixes to the fibres. Too high and it\n"
                        "deposits before it can travel, so nothing bleeds.");
    if (sim) ctlSliderInt("Lattice steps", &sim->inkSubsteps, 1, 20);
    ctlSlider("Evaporation", &st.sim.evaporation, 0.0f, 0.03f, "%.4f");
  } else {
    ctlSlider("Viscosity", &st.sim.viscosity, 0.0f, 0.5f);
    ctlSlider("Drag", &st.sim.drag, 0.0f, 1.5f);
    ImGui::SetItemTooltip("Above ~0.5 the velocity field dies before water moves.");
    ctlSlider("Edge darkening", &st.sim.edgeDarkening, 0.0f, 2.0f, "%.3f");
    ImGui::SetItemTooltip("Curtis FlowOutward: pulls pigment to the stroke rim.\n"
                        "Zero this and washes go flat.");
    ctlSlider("Paper slope", &st.sim.paperSlope, 0.0f, 4.0f);

    // One control for the wet lifetime. Evaporation and absorption are derived
    // from it rather than exposed separately: letting the two drift out of step
    // only makes the timing unpredictable, and neither means much alone.
    if (ctlSlider("Working time", &st.workingTime, 1.0f, 20.0f, "%.1f s"))
      setWorkingTime(st.sim, st.workingTime);
    ImGui::SetItemTooltip("How long a wash keeps bleeding before it sets.\n"
                        "Good to about 15%% across this range. Past ~20 s the\n"
                        "wash spreads thin enough that capillary dilution ends\n"
                        "it regardless of how slowly it dries.");

    ctlSlider("Max film", &st.sim.maxFilm, 0.2f, 8.0f);
    ImGui::SetItemTooltip("Deepest water the paper holds before it runs.\n"
                        "Raise it far and a wash empties into its own rim.");
    ctlSlider("Capillary diffuse", &st.sim.diffuseRate, 0.0f, 1.0f);
    ImGui::SetItemTooltip("How fast water wicks through the fibres.\n"
                        "Sets how far a wash reaches, not how long it lasts.");
  }
}

void drawBoardTiltSection(AppState& st) {
  tiltPad(st, 96.0f);
  ImGui::SameLine();
  ImGui::BeginGroup();
  const float steep = std::sqrt(st.sim.tiltX * st.sim.tiltX +
                                st.sim.tiltY * st.sim.tiltY) / kMaxTilt;
  ImGui::TextDisabled("%.0f%%", steep * 100.0f);
  ImGui::TextDisabled("drag to");
  ImGui::TextDisabled("tilt");
  ImGui::Dummy(ImVec2(0, 4));
  if (ImGui::SmallButton("Level")) { st.sim.tiltX = 0.0f; st.sim.tiltY = 0.0f; }
  ImGui::EndGroup();
  ImGui::SetItemTooltip("Only wet paint runs — the force scales with film\n"
                      "depth, so a puddle streaks and damp paper does not.\n"
                      "Double-click the pad to level.");
}

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q7):
// "a simple settings surface... doesn't need its own dedicated window" --
// a couple of sliders here, alongside the rest of the view/sim controls,
// is sufficient. Mirrors the View menu's Grid/Snap checkboxes (same
// AppState fields; either path sets the one place that's actually read,
// this file's canvas block below).
void drawGridSection(AppState& st) {
  ImGui::Checkbox("Show grid", &st.showGrid);
  ctlSlider("Spacing", &st.gridSpacing, 4.0f, 512.0f, "%.0f px");
  ctlSliderInt("Subdivisions", &st.gridSubdivisions, 1, 10);
  ImGui::Checkbox("Snap", &st.snappingEnabled);
  ImGui::SetItemTooltip("Snaps guide creation/dragging to guides, the grid\n"
                      "and canvas edges. Never affects freehand painting.");
}

void drawSolverSection(AppState& st, PaintSim* sim) {
  if (sim) {
    if (st.mode == PaintMode::Watercolor)
      ctlSliderInt("Jacobi iters", &sim->jacobiIterations, 1, 60);
    ctlSliderInt("Substeps", &sim->substeps, 1, 6);
  }
  ImGui::Checkbox("Paused", &st.paused);
  if (ImGui::Button("Clear canvas")) st.requestClear = true;
  ImGui::SameLine();
  if (ImGui::Button("Reload shaders")) st.requestReload = true;
}

// ----------------------------------------------------------------- History
//
// PLAN.md Phase 5 step 8 ("History panel listing entries by originating tool
// or op; clicking one moves the cursor there in a single replay, not N"; PRD
// O2, O3, with O1's redo and O4's snapshots made visible). docs/ui.md §5 puts
// it in this right-hand docked column.
//
// Same split as drawLayersSection()/app/LayerPanel: everything that can be
// wrong here without a screenshot showing it lives in app/HistoryPanel.hpp and
// is exercised headlessly by `--selftest`. Specifically:
//
//  * **Which state a row is** -- `HistoryPanelRow::serial`, and NOT its index.
//    Eviction shifts every index down by one, so an index-keyed row would
//    silently repoint at a different state; this loop never passes a row
//    number to anything.
//  * **What a row says** -- `historyRowText()`, including the PAST / CURRENT /
//    REDOABLE word, so the redo tail a new edit would destroy is legible in
//    text and not only in a colour.
//  * **What a click does** -- `historyPanelClick()`, which performs exactly one
//    `History::jumpTo()` at any distance (PRD O3) and refuses, with numbers,
//    rather than redirecting when the row's state is gone.
//  * **What the notes say** -- `historyDroppedNote()` and
//    `historyRedoTailNote()`.
//
// Two deliberate choices in this function itself:
//
//  1. **The click is applied after the row loop, never inside it.** A jump
//     changes the cursor, which changes every row's state; acting mid-loop
//     would render the rest of the list against a cursor that has already
//     moved. The identical precaution drawLayersSection() takes with
//     `structureChanged`, for the identical reason.
//  2. **`History::budgetPressure()` and `overBudget()` are not called here.**
//     Both run `bytes()`, an O(slots) scan over every tile of every entry, and
//     this function runs every frame. The part of that a user can act on --
//     that undo has stopped going back, and why -- is in
//     `historyDroppedNote()`, which reads two O(1) counters.
//
// **What this panel does NOT show, stated plainly**: a painted stroke. It
// lists the open document's history, and `sim::PaintSim` owns a dense GPU
// texture no stroke escapes -- core/History.hpp says so at length. So the rows
// are layer operations, placing an image, duplicating and reverting, and a
// session spent painting produces exactly one row.
// One path from a widget to a history cursor move, shared by the HISTORY
// panel and the title bar's undo/redo pair -- docs/ui.md section 2 draws
// undo/redo in the title bar, so there are now two widgets that move the
// cursor and there must still be one place that knows what moving it costs.
//
// A cursor move is not an edit: it must NOT go through recordEdit(), which
// would append a history entry for the act of moving through history. It does
// change the document on screen, so the dirty flag and the journal's
// structural revision both move -- an undone "add layer" changes the layer
// stack, and a journal that kept the pre-jump structural state would be
// holding a document that is no longer open.
std::string g_historyError;
void installHistoryCursor(OpenDocument& od, const HistoryPanelClick& r) {
  g_historyError = r.ok ? std::string() : r.refusal;
  if (!r.ok || r.document == nullptr) return;
  od.document = *r.document;
  ++od.revision;
  ++od.structuralRevision;
}

// One place that installs a selection, so the three things that must move
// together cannot drift apart: the selection itself, the revision that tells
// the cached bounds and the GPU coverage they are stale, and nothing else.
//
// A selection is deliberately NOT a history edit (see OpenDocument::selection
// on why it lives outside `Document`): drawing a marquee is not an act to be
// undone, and an undo that restored a marquee along with the pixels would
// surprise anyone who has used an editor.
void installSelection(OpenDocument& od, std::optional<Selection> selection) {
  // Remember what a deselect threw away, so Reselect has something to restore.
  // Only this transition -- engaged to absent -- is recorded; see
  // `OpenDocument::lastDeselected` on why replacing one shape with another is
  // deliberately not.
  if (!selection.has_value() && od.selection.has_value()) {
    od.lastDeselected = od.selection;
  }
  od.selection = std::move(selection);
  ++od.selectionRevision;
}

}  // namespace

// ---------------------------------------------------------------------------
// The Select menu's dialog -> engine boundary (docs/reachability-audit.md C5;
// PRD E4/E8/E9). Declared in ui/MacPaintUI.hpp and defined here, external
// linkage, for the same reason `commitDrawnSelection()` a few sections up
// is: `app/selftest/SelectMenu.cpp` calls these directly, because the ImGui
// popups that also call them cannot run without a window.
// ---------------------------------------------------------------------------

bool selectRefineEnabled(const OpenDocument& od) noexcept { return od.selection.has_value(); }

bool selectRangeEnabled(const OpenDocument& od) noexcept {
  const Layer* target = activeLayerOf(od);
  return target != nullptr && target->rgbTiles.has_value();
}

bool selectUndoRefineEnabled(const OpenDocument& od) noexcept {
  return !od.refineUndoStack.empty();
}

Selection applySelectRefineAction(MenuAction action, const Selection& current, float radius) {
  // One switch, one engine call per case -- the shape `performMenuAction()`
  // itself uses, and for the identical reason stated there: a call site that
  // fell through to a default would be a menu item wired to nothing, which is
  // worse than wired to the wrong thing because it is silent. There is no
  // `default:` here for the same reason -- an action added to the enum
  // without a row here should fail to compile once this switch is marked
  // exhaustive, not fall through to returning `current` unchanged, which
  // would look like a working "Grow" that grows nothing.
  switch (action) {
    case MenuAction::SelectGrow: return growSelection(current, radius);
    case MenuAction::SelectShrink: return shrinkSelection(current, radius);
    case MenuAction::SelectFeather: return featherSelection(current, radius);
    default: return current;
  }
}

Selection applySelectColourRangeAction(const std::array<float, 3>& swatchSrgb, float tolerance,
                                       float edgeBand, const TileStore& source, int32_t width,
                                       int32_t height) {
  // sRGB -> STRAIGHT LINEAR, per channel -- the identical conversion
  // foregroundLinearRgba() applies to the swatch above, and for the identical
  // reason: `selectColourRange()` wants straight linear RGBA
  // (core/SelectionRefine.hpp says so at the parameter), and skipping this
  // selects a colour roughly twice as dark as the one the swatch showed,
  // which reads as a colour-management bug rather than a missing conversion.
  const std::array<float, 4> linear = {srgbDecode(swatchSrgb[0]), srgbDecode(swatchSrgb[1]),
                                       srgbDecode(swatchSrgb[2]), 1.0f};
  SelectionRangeParams params;
  params.tolerance = tolerance;
  // Clamped here rather than left to core/SelectionRefine's own internal
  // clamp (SelectionRangeParams::edgeBand's comment says it clamps to
  // tolerance) so a caller inspecting `params` before the call sees the value
  // that will actually be used, not one that only becomes correct inside the
  // engine.
  params.edgeBand = std::min(edgeBand, tolerance);
  return selectColourRange(source, linear, width, height, params);
}

Selection applySelectLuminanceRangeAction(float low, float high, float edgeBand,
                                          const TileStore& source, int32_t width,
                                          int32_t height) {
  SelectionLuminanceRange range;
  range.low = low;
  range.high = high;
  range.edgeBand = edgeBand;
  return selectLuminanceRange(source, width, height, range);
}

void installRefinedSelection(OpenDocument& od, std::optional<Selection> result) {
  // Pushed BEFORE installSelection() moves od.selection out from under this
  // read -- the ordering that makes "one entry per operation" true rather
  // than aspirational.
  od.refineUndoStack.push_back(od.selection);
  installSelection(od, std::move(result));
}

bool undoLastRefine(OpenDocument& od) {
  if (od.refineUndoStack.empty()) return false;
  std::optional<Selection> previous = std::move(od.refineUndoStack.back());
  od.refineUndoStack.pop_back();
  // Through installSelection(), not a bare assignment: undoing a refine still
  // bumps selectionRevision (the GPU coverage upload and the cached bounds
  // must both notice), and if the refine had emptied a previously-engaged
  // selection down to `std::nullopt`, restoring an engaged one here is
  // exactly the transition installSelection()'s lastDeselected bookkeeping
  // already knows how to leave alone (it only fires the other direction,
  // engaged -> absent).
  installSelection(od, std::move(previous));
  return true;
}

namespace {

// Where every one of PRD E3's five selection tools ends: the shape it drew,
// combined with what was already installed through the PRD E7 modifier the
// gesture latched, installed once.
//
// `drawn` absent means **the gesture produced no shape** -- a click with no
// drag, a lasso of two points, a wand on a layer it cannot read. That is not
// the same as a shape covering nothing, and the two get different answers
// below.
//
// Three rules live here, and the reason they are in one function rather than
// repeated per tool is that each is a rule about *user intent* that would be
// easy to get subtly different in five places:
//
//  1. **An empty gesture with no modifier deselects.** That is what clicking
//     off a marquee means in every editor.
//
//  2. **An empty gesture WITH a modifier does nothing at all.** A miss while
//     Shift-adding is a miss, not an instruction to throw away the selection
//     being built up. Losing a careful multi-gesture selection to one twitchy
//     click is the most annoying way this feature can be got wrong.
//
//  3. **Subtract and Intersect against no selection are refused**, not
//     evaluated. Both would produce an *engaged* selection covering nothing --
//     core/SelectionMask.hpp's "why is nothing happening when I paint" -- from
//     a gesture the user meant as a refinement of something that was not there.
// Declared in ui/MacPaintUI.hpp rather than kept file-local, so --selftest
// can reach the three intent rules below. That header says why they are
// worth reaching.
}  // namespace

std::array<float, 4> foregroundLinearRgba(int pigmentIndex) {
  const std::vector<Pigment>& palette = defaultPalette();
  if (pigmentIndex < 0 || static_cast<size_t>(pigmentIndex) >= palette.size())
    return {0.0f, 0.0f, 0.0f, 1.0f};
  const Pigment& pig = palette[static_cast<size_t>(pigmentIndex)];
  return {srgbDecode(pig.rgb[0]), srgbDecode(pig.rgb[1]), srgbDecode(pig.rgb[2]), 1.0f};
}

std::array<float, 4> foregroundLinearRgba(const BrushState& brush) {
  // One decode, of `app/StrokeSession`'s one answer to "what colour is the
  // foreground". Alpha is 1.0 for the reason the index overload's header
  // comment gives: the foreground well has no opacity of its own.
  const std::array<float, 3> fg = foregroundSrgb(brush);
  return {srgbDecode(fg[0]), srgbDecode(fg[1]), srgbDecode(fg[2]), 1.0f};
}

GradientStops currentGradientStops(const BrushState& brush) {
  return gradientToolStops(foregroundLinearRgba(brush));
}

EyedropperPick applyEyedropperPick(AppState& st, PixelCoord at) {
  EyedropperPick out;

  const OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    // The solver canvas is not a document and `probePixel()` takes one, so
    // there is genuinely nothing to sample. Said, rather than ignored: a tool
    // that does nothing without explaining itself is the defect this whole
    // change exists to remove.
    out.report = "Nothing to sample: no document is open. File > New Document makes one.";
    st.lastPickReport = out.report;
    return out;
  }

  ProbeParams params;
  params.sampleSize = st.eyedropper.sampleSize;
  params.source = st.eyedropper.source;
  // Not a user setting -- whichever layer is active at the instant of the
  // click. `ProbeSource::CurrentLayer` reads it and `ActiveAndBelow` stops at
  // it; `AllLayers` ignores it entirely.
  params.activeLayerIndex = static_cast<int32_t>(od->activeLayer);
  out.sample = probePixel(od->document, at, params);

  // Nothing there. See the header: not applied, and said out loud rather than
  // silently leaving the foreground alone, because "the swatch did not change"
  // is indistinguishable from "the tool is broken".
  if (!(out.sample.linear[3] > 0.0f)) {
    out.report = "Nothing to sample there: that point is transparent in this sample source.";
    st.lastPickReport = out.report;
    return out;
  }

  // **`display`, not `linear`.** The foreground is display-referred sRGB
  // (app/AppState.hpp's `BrushState::rgb`), and `ProbeSample::display` is
  // exactly `srgbEncode()` of the linear value the document holds -- so this
  // is the encode half of the same boundary `foregroundLinearRgba()` decodes,
  // taken from the struct that already computed it rather than re-derived
  // here. Storing the *linear* value in a field everything treats as sRGB
  // would make the swatch, the picker and the Mixbox LUT wrong together, and
  // every picked colour would repaint far too dark -- the sort of wrong nobody
  // notices until they hold the result against the pixel they picked it from.
  //
  // Clamped to [0,1]. Working-space values legitimately exceed 1.0
  // (color/Space.hpp: "Working-space values are linear light and can
  // legitimately exceed 1.0"), and so therefore can their sRGB encoding -- but
  // `ImGui::ColorPicker3` cannot show such a value, an 8-bit swatch cannot draw
  // it, and `MixboxLut::rgbToLatent()` clamps it away regardless. Clamping here
  // rather than at those three places means the number the picker shows is the
  // number the next stroke uses, instead of a fourth value only the clamps know
  // about.
  for (int c = 0; c < 3; ++c) st.brush.rgb[c] = std::clamp(out.sample.display[c], 0.0f, 1.0f);

  out.switchedToRgbMode = st.brush.colorMode == ColorMode::Pigment;
  st.brush.colorMode = ColorMode::Rgb;

  char buf[256];
  if (out.switchedToRgbMode) {
    std::snprintf(buf, sizeof(buf),
                  "Picked %.3f %.3f %.3f -- COLOR switched to RGB mode: a sampled colour has "
                  "no density, staining or granulation, so those keep coming from %s.",
                  static_cast<double>(st.brush.rgb[0]), static_cast<double>(st.brush.rgb[1]),
                  static_cast<double>(st.brush.rgb[2]),
                  foregroundPhysicalConstants(st.brush).name);
  } else {
    std::snprintf(buf, sizeof(buf), "Picked %.3f %.3f %.3f into the foreground colour.",
                  static_cast<double>(st.brush.rgb[0]), static_cast<double>(st.brush.rgb[1]),
                  static_cast<double>(st.brush.rgb[2]));
  }
  out.report = buf;
  st.lastPickReport = out.report;
  out.applied = true;
  return out;
}

void commitDrawnSelection(AppState& st, OpenDocument& od,
                          const std::optional<Selection>& drawn) {
  if (!drawn.has_value()) {
    if (st.marqueeCombine == SelectionCombine::Replace) installSelection(od, std::nullopt);
    return;
  }
  if (!od.selection.has_value()) {
    if (st.marqueeCombine == SelectionCombine::Replace ||
        st.marqueeCombine == SelectionCombine::Add) {
      installSelection(od, *drawn);
    }
    return;
  }
  installSelection(od, combineSelections(*od.selection, *drawn, st.marqueeCombine));
}

namespace {

// Undo-while-wet. The solver keeps a stroke that has not finished drying, and
// a cursor move replaces the document without touching the solver -- so paint
// the document has never seen stays on screen over a state the user undid to,
// and the next bake deposits it there. app/StrokeBake.hpp section 4 has the
// full argument; this is where it is paid for.
//
// **Call this BEFORE computing the history target, never after.** A forced
// bake can append or amend an entry, so `cursor() - 1` evaluated beforehand
// is off by one: the user sees "document + wet stroke", the stroke becomes
// the newest entry, and one undo should land on the document that was under
// it. `moveHistoryCursor()` (defined after this translation unit's anonymous
// namespaces close, so `--selftest` can reach it) is the one caller and
// settles first for exactly this reason. Row clicks in the HISTORY panel are
// identified by *serial* and survive either order, but they go through the
// same path so there is one rule regardless.
void settleWetPaintBeforeHistoryMove(AppState& st, std::unique_ptr<PaintSim>& sim,
                                     GpuContext& gpu, OpenDocument& od) {
  // No sim means no solver fields and so nothing wet to settle -- the common
  // case before the first stroke of a session (ADR-0001: idle costs no GPU
  // memory, so the sim does not exist until something is painted).
  if (!sim) return;
  st.bakeCycle.forceBake(gpu, *sim, &od, sim->mode());
}

void drawHistorySection(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    return;
  }

  History& h = od->history;
  std::string& lastError = g_historyError;
  auto install = [&](const HistoryPanelClick& r) { installHistoryCursor(*od, r); };

  const std::vector<HistoryPanelRow> rows = historyPanelRows(h);
  ImGui::TextDisabled("%zu state(s), cursor on %zu", rows.size(),
                      rows.empty() ? 0 : h.cursor() + 1);

  // Undo and Redo go through `moveHistoryCursor()` -- the same function the
  // title bar's pair, the Edit menu's Undo/Redo and ⌘Z/⇧⌘Z all call (D1,
  // docs/reachability-audit.md) -- so there is exactly one path from any of
  // the four to a cursor move and exactly one place a refusal can come from.
  // The two guards are `canUndo()` / `canRedo()`, so the neighbouring row
  // always exists and `cursor() - 1` cannot wrap.
  ImGui::BeginDisabled(!h.canUndo());
  if (ImGui::SmallButton("Undo")) moveHistoryCursor(st, sim, gpu, *od, -1);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!h.canRedo());
  // Redo settles too, inside `moveHistoryCursor()`, and the consequence is
  // worth stating: if paint was applied after an undo, the forced bake
  // records a new entry, which truncates the redo tail -- so the target
  // computed after the settle correctly finds no such row and the move
  // refuses by name. That is the honest answer ("you painted after undoing,
  // so there is nothing to redo") and it is only reachable because the
  // target is computed AFTER the settle, which is why
  // `settleWetPaintBeforeHistoryMove()`'s own comment insists on that order
  // and `moveHistoryCursor()` is the only place doing both now.
  if (ImGui::SmallButton("Redo")) moveHistoryCursor(st, sim, gpu, *od, +1);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton("Snapshot"))
    h.takeSnapshot("snapshot " + std::to_string(h.snapshots().size() + 1), od->document);
  ImGui::SetItemTooltip("A named state exempt from the byte budget's eviction\n"
                      "until you dismiss it (PRD O4). Kept in its own list, not\n"
                      "in the undo chain, so undoing past it cannot lose it.");

  const std::string dropped = historyDroppedNote(h);
  if (!dropped.empty()) ImGui::TextWrapped("%s", dropped.c_str());

  // Rows, oldest at the top -- the OPPOSITE of drawLayersSection()'s order,
  // and app/HistoryPanel.hpp says why the two panels differ. There is no
  // reversal in this loop and there must not be one.
  //
  // T8: this list used to draw straight into the collapsing header, so a long
  // session grew the panel without bound. Bounded here in a fixed-height
  // scrolling child -- the same idiom `##pick`/`##plan`/`##report` (this
  // file, ~5397-5493) and `##toolgrid` (~7751) already use for a list that
  // can outgrow its column. The height is `kHistoryVisibleRows` row-heights,
  // not a pasted pixel number, so it tracks the font/theme's own line
  // spacing instead of drifting from it if either changes; the buttons above
  // and the redo-tail/error lines below stay outside the child so they never
  // scroll out of view.
  constexpr int kHistoryVisibleRows = 8;
  const float rowH = ImGui::GetTextLineHeightWithSpacing();
  // **The `std::max` is not defensive padding, it is the empty case.** An
  // empty history is an expressible state -- `History::empty()` is a
  // first-class predicate, and the "%zu state(s)" line forty lines above
  // already branches on `rows.empty()` -- and `BeginChild()` reads a height
  // of `0.0f` as *fill the rest of the column*
  // (`imgui.cpp`: `if (size.y <= 0.0f) size.y = ImMax(content_avail.y + size.y, 4.0f)`).
  // So the one case with nothing to show is the one that would have swelled
  // to eat every panel below it. One row tall, and it draws as an empty box
  // under a line that says "0 state(s)", which is the honest picture.
  //
  // **Plus the child's own vertical padding**, which this calculation shipped
  // without and which drawLayersSection() (T11) caught by screenshot on a
  // panel showing two rows against an eight-row cap. `BeginChild()` sizes the
  // OUTER box, but a bordered child's rows are laid out inside the style's
  // `WindowPadding`, so N row-heights of box holds slightly less than N rows
  // of content -- and the panel grows a scrollbar for content that fits.
  // Counted once here rather than left to be clipped off the bottom.
  const float childH =
      std::max(rowH, static_cast<float>(std::min(rows.size(),
                                                 static_cast<size_t>(kHistoryVisibleRows))) *
                         rowH) +
      2.0f * ImGui::GetStyle().WindowPadding.y;

  // Auto-scroll follows the CURRENT row, but only right after the cursor
  // moves -- not every frame, which would fight the user's own scrolling:
  // they scroll up to read an old row, and the very next frame, which has
  // nothing to do with a cursor move, would yank them straight back down.
  // So this compares the CURRENT row's serial (stable across an amend, per
  // History::amend()'s header, and unique per entry otherwise) against the
  // one seen last frame.
  //
  // **The trigger holds for two frames, not one, and the reason is a clamp
  // against a number that does not exist yet.** `SetScrollHereY()` only sets
  // `ImGuiWindow::ScrollTarget`; Dear ImGui turns that into a real scroll
  // position at this child's NEXT `Begin()`, and the last thing it does there
  // is `scroll = ImMin(scroll, window->ScrollMax)`
  // (`CalcNextScrollFromScrollTargetAndClamp()` in `imgui.cpp`). `ScrollMax`
  // comes from the content size measured on the PREVIOUS frame -- and on the
  // frame a child first exists that is **0**, so any target whatsoever
  // clamps to the top.
  //
  // That is not a corner case here, it is the common one: the panel is
  // usually first drawn on a document that already has history, so "the
  // child's first frame" and "the cursor serial changed from 0" are the same
  // frame. Screenshot-measured on this build: 1 frame leaves the list showing
  // row 1; 2 lands on the CURRENT row. The second frame is the one that has a
  // real `ScrollMax` to clamp against.
  //
  // `s_followCursorFrames` is the holdover, and a `static` local is correct
  // here for the same reason `pendingControlsScrollPx` (~7853) is one -- this
  // function has exactly one HISTORY panel to remember it for.
  static uint64_t s_lastCursorSerial = 0;
  static int s_followCursorFrames = 0;
  uint64_t currentSerial = 0;
  for (const HistoryPanelRow& row : rows)
    if (row.state == HistoryRowState::Current) currentSerial = row.serial;
  if (currentSerial != s_lastCursorSerial) s_followCursorFrames = 2;
  s_lastCursorSerial = currentSerial;
  const bool followCursor = s_followCursorFrames > 0;
  if (s_followCursorFrames > 0) --s_followCursorFrames;

  uint64_t clickedEntry = 0;
  if (ImGui::BeginChild("##historyrows", ImVec2(0.0f, childH), true)) {
    for (const HistoryPanelRow& row : rows) {
      ImGui::PushID(static_cast<int>(row.serial));
      const bool redoable = row.state == HistoryRowState::Redoable;
      if (redoable) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
      const std::string text = historyRowText(row);
      if (ImGui::Selectable(text.c_str(), row.state == HistoryRowState::Current))
        clickedEntry = row.serial;
      if (redoable) ImGui::PopStyleColor();
      // A row is a whole sentence built from an edit label a user typed part of
      // ("rename layer 2 to ..."), so it has no bounded width and a docked column
      // will clip some of them. A row is a list item rather than a label, so it
      // is clipped rather than wrapped -- and the full text is one hover away,
      // which is the part that was missing.
      ImGui::SetItemTooltip("%s", text.c_str());
      if (followCursor && row.state == HistoryRowState::Current) ImGui::SetScrollHereY();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  const std::string tail = historyRedoTailNote(h);
  if (!tail.empty()) ImGui::TextWrapped("%s", tail.c_str());

  // Snapshots, as their own group below the list and never interleaved into
  // it: they are not on the linear chain and have no cursor position, which is
  // core/History.hpp's reason for their being a second list in the first
  // place.
  const std::vector<HistorySnapshotRow> snaps = historySnapshotRows(h);
  if (!snaps.empty()) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("SNAPSHOTS (exempt from eviction)");
    uint64_t restore = 0;
    size_t dismiss = kNoHistoryRow;
    for (const HistorySnapshotRow& row : snaps) {
      ImGui::PushID(static_cast<int>(row.serial));
      ImGui::TextUnformatted(historySnapshotRowText(row).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Restore")) restore = row.serial;
      ImGui::SameLine();
      if (ImGui::SmallButton("Dismiss")) dismiss = row.index;
      ImGui::PopID();
    }
    if (restore != 0) install(historyPanelRestoreSnapshot(h, restore));
    if (dismiss != kNoHistoryRow) h.dismissSnapshot(dismiss);
  }

  if (clickedEntry != 0) install(historyPanelClick(h, clickedEntry));

  // The refusal, verbatim, for the same reason drawLayersSection() shows
  // core/LayerOps' sentence verbatim: app/HistoryPanel's refusals already name
  // the counts and say what did not happen, so there is no second vocabulary
  // here to drift from theirs.
  if (!lastError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 110, 255));
    ImGui::TextWrapped("%s", lastError.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss##historyerror")) lastError.clear();
  }
}

// ------------------------------------------------------------------- comps
//
// PLAN.md Phase 5 step 12 ("**Layer comps** -- named sets of visibility,
// position and properties, restorable in one click and **persisted in the
// document** as an `np:comps` blob on part 0"; PRD C14).
//
// Same split as the two sections above: everything that can be wrong here
// without a screenshot showing it is in app/CompPanel.hpp and core/LayerCompOps.hpp
// and is exercised headlessly by `--selftest` -- what a row says, how many of a
// comp's layers are still in the document, what a restore could not do and the
// sentence it says about it. What is here is the chrome.
//
// Three choices in this function itself:
//
//  1. **Capture goes through `layerCommandButton()`**, i.e. through
//     `app::applyLayerCommand()`, which is the same surface the `Layer` menu's
//     "Capture Layer Comp" item uses. The other four operations carry a comp
//     index and so call core/LayerCompOps directly through `recordLayerEdit()`,
//     exactly as the opacity slider and the blend dropdown in the LAYERS panel
//     already do -- app/LayerEditor.hpp's own boundary between a gesture and a
//     control that carries a value.
//  2. **An action is applied after the row loop, never inside it.** A delete or
//     a reorder changes every index above it, and acting mid-loop would render
//     the rest of the list against indices that no longer mean what they did.
//     The identical precaution `drawLayersSection()` takes with
//     `structureChanged` and `drawHistorySection()` with its deferred click;
//     app/CompPanel.hpp section (b) is why that, rather than a stable comp id,
//     is the right level for this list.
//  3. **A partial restore is offered rather than greyed out**, and answered
//     with `core::layerCompRestoreSummary()`'s sentence. A comp captured over
//     five layers, one since deleted, is still worth restoring; a disabled
//     button would explain nothing. app/LayerEditor.hpp's availability-versus-
//     refusal rule.
// Panel-local, like `drawHistorySection()`'s `lastError`: a selection and the
// two sentences a restore can leave behind. Nothing here is a second copy of
// model state -- the rows are rebuilt from the document every frame. At file
// scope rather than function-local because `setCompsPanelRestoreSummary()`
// reaches `g_compsSummary`, for the reason ui/MacPaintUI.hpp gives.
size_t g_compsSelected = 0;
std::string g_compsError;
std::string g_compsSummary;

void drawCompsSection(AppState& st) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    return;
  }
  Document& doc = od->document;

  size_t& selected = g_compsSelected;
  std::string& lastError = g_compsError;
  std::string& lastSummary = g_compsSummary;
  static char renameBuf[128] = "";

  const std::vector<CompPanelRow> rows = compPanelRows(doc);
  if (selected >= rows.size()) selected = rows.empty() ? 0 : rows.size() - 1;

  textDisabledWrapped("%zu comp(s) -- named sets of layer visibility, opacity, blend and clip",
                      rows.size());
  ImGui::SetItemTooltip("A comp captures every layer's visibility, opacity, blend\n"
                      "and clip, and restores them in one click (PRD C14).\n"
                      "It is saved in the document, as np:comps on part 0.\n"
                      "\n"
                      "PRD C14 also says \"position\". Layers in this build have\n"
                      "no position: tiles are stored in absolute document\n"
                      "coordinates and core::Layer has no offset field, so there\n"
                      "is nothing to capture or restore. That third of C14 is\n"
                      "not delivered, and this says so rather than capturing a\n"
                      "zero and calling it done.\n"
                      "\n"
                      "Not captured either: the mask (removing one discards its\n"
                      "pixels), the lock (a working state, not an appearance),\n"
                      "the name, and the per-layer op stack. core/LayerComp.hpp\n"
                      "argues each exclusion.");

  layerCommandButton(st, LayerCommand::CaptureComp, "+ Capture");

  auto run = [&](LayerOpResult r) {
    const DocumentOpResult out = recordLayerEdit(*od, std::move(r));
    lastError = out.ok ? std::string() : out.error;
    return out.ok;
  };

  // The deferred actions, applied after the loop (choice 2 above).
  size_t restore = rows.size(), remove = rows.size(), moveUp = rows.size(),
         moveDown = rows.size();

  // T11: bounded scroll region, the same idiom drawHistorySection() (T8) and
  // drawLayersSection() (also T11) use for `##historyrows` / `##layerrows`.
  // No follow-the-selection here -- the entry that scoped this work says
  // COMPS should not grow one just for symmetry, and nothing here changes
  // `selected` except a click, which is already on screen when it happens.
  //
  // A comp row is not one line the way a HISTORY row is: it is a Selectable
  // plus an indented button row, and a THIRD line -- the rename field --
  // only when it is the selected row. `rowH` below is built from the same
  // two theme metrics ImGui itself sizes those two lines from
  // (GetTextLineHeightWithSpacing() for the Selectable, GetFrameHeightWithSpacing()
  // for the SmallButton row) rather than a pasted pixel number, for the same
  // reason drawHistorySection() gives for its own row metric -- it tracks the
  // theme rather than drifting from it. It is an approximation (the rename
  // field's extra line is not counted), which only means a row with its
  // rename field open scrolls slightly tighter than the others; the box
  // stays a fixed height and stays scrollable either way.
  // The `WindowPadding` term is drawLayersSection()'s own T11 finding, not
  // rediscovered here: a bordered child's rows sit inside the current
  // style's `WindowPadding`, so a box sized to exactly N row-heights is a
  // few pixels short of them once that padding comes back out, and shows a
  // scrollbar for content that fits.
  constexpr int kCompsVisibleRows = 4;
  const float compRowH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing();
  const float compsChildH =
      std::max(compRowH, static_cast<float>(std::min(rows.size(),
                                                     static_cast<size_t>(kCompsVisibleRows))) *
                             compRowH) +
      2.0f * ImGui::GetStyle().WindowPadding.y;

  if (ImGui::BeginChild("##compsrows", ImVec2(0.0f, compsChildH), true)) {
    for (const CompPanelRow& row : rows) {
      ImGui::PushID(static_cast<int>(row.index));
      if (ImGui::Selectable(compRowText(row).c_str(), selected == row.index))
        selected = row.index;
      if (compRowIsPartial(row))
        ImGui::SetItemTooltip("%zu of this comp's %zu layers are still in the document.\n"
                          "Restoring applies those and reports the rest -- a comp is\n"
                          "matched by layer id, never by position, so nothing lands\n"
                          "on a layer it was not captured from.",
                          row.stillHere, row.captured);
      ImGui::Indent();
      ImGui::BeginDisabled(!row.known);
      if (ImGui::SmallButton("Restore")) restore = row.index;
      ImGui::EndDisabled();
      if (!row.known)
        ImGui::SetItemTooltip("This comp was written by a build whose comp format this\n"
                          "one does not read. It is kept and written back unchanged\n"
                          "(PRD I10), but its contents cannot be applied.");
      ImGui::SameLine();
      // **Up the panel is DOWN the index here**, the opposite of the LAYERS
      // panel, because this list is not reversed. The arithmetic is
      // app/CompPanel's rather than this loop's, for app/LayerPanel.hpp's stated
      // reason -- a second place that knows a direction is how "up" ends up
      // moving a thing down, and this loop had that bug before the two functions
      // existed.
      const size_t upTo = compRowMoveUpTarget(row.index, rows.size());
      const size_t downTo = compRowMoveDownTarget(row.index, rows.size());
      ImGui::BeginDisabled(upTo == kNoCompRow);
      if (ImGui::SmallButton("Up")) moveUp = row.index;
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(downTo == kNoCompRow);
      if (ImGui::SmallButton("Down")) moveDown = row.index;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::SmallButton("Delete")) remove = row.index;

      if (selected == row.index && row.known) {
        std::snprintf(renameBuf, sizeof(renameBuf), "%s", row.name.c_str());
        if (ctlInputText("Comp name", renameBuf, sizeof(renameBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
          run(renameLayerComp(doc, row.index, renameBuf));
      }
      ImGui::Unindent();
      ImGui::PopID();
    }
  }
  ImGui::EndChild();

  if (restore < rows.size()) {
    LayerCompRestoreReport report;
    if (run(restoreLayerComp(doc, restore, &report)))
      lastSummary = layerCompRestoreSummary(report);
    else
      lastSummary.clear();
  } else if (remove < rows.size()) {
    if (run(deleteLayerComp(doc, remove))) lastSummary.clear();
  } else if (moveUp < rows.size()) {
    const size_t to = compRowMoveUpTarget(moveUp, rows.size());
    if (to != kNoCompRow && run(moveLayerComp(doc, moveUp, to))) selected = to;
  } else if (moveDown < rows.size()) {
    const size_t to = compRowMoveDownTarget(moveDown, rows.size());
    if (to != kNoCompRow && run(moveLayerComp(doc, moveDown, to))) selected = to;
  }

  // The restore's own sentence: what it could not do, with the numbers.
  // core/LayerCompOps writes it, so the panel has no second vocabulary to drift
  // from the model's -- the rule drawLayersSection() and drawHistorySection()
  // both already follow for their refusals.
  if (!lastSummary.empty()) {
    ImGui::TextWrapped("%s", lastSummary.c_str());
    if (ImGui::SmallButton("Dismiss##compsummary")) lastSummary.clear();
  }
  if (!lastError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 110, 255));
    ImGui::TextWrapped("%s", lastError.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss##compserror")) lastError.clear();
  }
}

// ---------------------------------------------------------------- Export As
//
// PLAN.md Phase 4 step 7 / PRD I15 ("Export As: target format, colour space,
// bit depth **and resize**, with saveable presets"). Everything that can be
// wrong here without a screenshot showing it -- what may be offered, what a
// combination costs, how a preset is stored and what happens to one this
// build cannot honour -- lives in io/ExportAs.hpp and is exercised headlessly
// by --selftest. This function is the widgets and nothing else, the same
// split app/CurveEdit + drawCurveWidget() already has.
//
// Two properties are structural rather than careful:
//
//  1. **It cannot offer a combination io/Export would refuse.** The format
//     list is offerableExportFormats() and the depth list is
//     offerableExportDepths(format), both computed from io/Capabilities'
//     runtime query -- so in an NP_USE_OIIO=OFF build EXR is not in the menu
//     at all, and in an ON build EXR offers half and 32-bit float but not
//     8-bit, because that is what this OpenImageIO actually writes. The
//     formats this build cannot write are still *shown*, greyed, with the
//     capability query's own reason as their tooltip: a menu that silently
//     omits EXR cannot answer "why can't I export EXR?".
//  2. **Every message it shows is io/Export's own.** The red line under the
//     controls is exportRefusalReason()'s string verbatim, not a reworded
//     one, so there is no second vocabulary here to drift from the encoder's.
//
// **Updated by PLAN.md Phase 4 step 8.** When step 7 landed, the Export
// button was disabled and said so, because there was no `core::Document`
// anywhere in the running application to export from. There is now:
// `AppState::documents` holds them, and File > New Document / Open... puts
// one there. So the button is live whenever a document is open, exporting the
// *active document* -- and still disabled, with the accurate reason, when
// none is. What has NOT changed is the honest half: the painting canvas is
// still not a document (sim::PaintSim owns one dense texture with no layer
// awareness), so this exports what was opened, never what is on screen. The
// dialog says which of the two it is looking at rather than leaving it to be
// inferred.
bool g_exportAsRequested = false;

void drawExportAsDialog(AppState& st, uint32_t canvasW, uint32_t canvasH) {
  // Session state. Function-local statics, exactly like drawGradeSection()'s
  // newOpKindIdx and the Add Guide popup's fields -- this is UI state, not
  // app state, and app/AppState's ownership is PLAN.md Phase 4 step 8's
  // decision to make rather than this step's to pre-empt.
  static ExportRequest request;
  static ExportPresetStore presets;
  static bool presetsLoaded = false;
  static char presetNameBuf[96] = "";
  static char exportPathBuf[512] = "";
  static std::string status;
  // Whether the OS save panel raised by "Choose..." below is still out.
  //
  // **Drained here, above the early return, and that placement is the whole
  // point.** The panel is asynchronous and this popup can be closed while it
  // is still up; if the drain lived inside the popup body it would never run
  // for that user, ui/FileDialog's one-at-a-time mailbox would stay pending
  // for the rest of the session, and every File > Open afterwards would be
  // refused with no panel anywhere to explain why.
  static bool exportPathPanelInFlight = false;
  if (exportPathPanelInFlight) {
    if (const std::optional<FileDialogOutcome> picked = takeFileDialogOutcome()) {
      exportPathPanelInFlight = false;
      if (picked->chose)
        std::snprintf(exportPathBuf, sizeof(exportPathBuf), "%s", picked->path.c_str());
      else if (!picked->error.empty())
        status = picked->error;
    }
  }

  if (g_exportAsRequested) {
    g_exportAsRequested = false;
    ImGui::OpenPopup("Export As");
  }
  if (!ImGui::BeginPopupModal("Export As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  // Loaded on first open, never at startup: a preset file nobody asked for
  // costs nothing (PRD A2, ADR-0001), and --selftest's idle-RSS measurement
  // would notice if that stopped being true.
  if (!presetsLoaded) {
    presetsLoaded = true;
    if (!presets.loadFromFile(defaultExportPresetsPath()))
      status = presets.error();
    else if (!presets.problems().empty())
      status = presets.problems().front();
  }

  const std::string presetsPath = defaultExportPresetsPath();
  const ImVec4 kError(0.95f, 0.45f, 0.40f, 1.0f);
  const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);

  // What is being exported, named rather than assumed. These are two
  // genuinely different things in this build and conflating them on screen
  // would be the dishonest option.
  const OpenDocument* activeDoc = st.documents.active();
  const uint32_t srcW = activeDoc ? static_cast<uint32_t>(activeDoc->document.width) : canvasW;
  const uint32_t srcH = activeDoc ? static_cast<uint32_t>(activeDoc->document.height) : canvasH;
  if (activeDoc)
    ImGui::TextDisabled("Source: %ux%u (document '%s')", srcW, srcH,
                        documentDisplayName(*activeDoc).c_str());
  else
    ImGui::TextDisabled("Source: %ux%u (the live canvas -- not a document; nothing to export)",
                        srcW, srcH);
  ImGui::Separator();

  // --- Format -------------------------------------------------------------
  if (ImGui::BeginCombo("Format", imageFormatName(request.format))) {
    for (const FormatCapability& caps : allFormatCapabilities()) {
      const bool writable = caps.canWrite;
      if (!writable) ImGui::BeginDisabled();
      if (ImGui::Selectable(imageFormatName(caps.format), caps.format == request.format) &&
          writable) {
        request.format = caps.format;
        // Keep the depth legal for the new format rather than leaving an
        // impossible pair on screen and refusing it a line later.
        const std::vector<ExportBitDepth> depths = offerableExportDepths(request.format);
        if (!depths.empty() &&
            std::find(depths.begin(), depths.end(), request.bitDepth) == depths.end())
          request.bitDepth = depths.front();
      }
      if (!writable) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled |
                                 ImGuiHoveredFlags_DelayNormal)) {
          const std::string why =
              exportRefusalReason(caps.format, request.targetSpace, request.bitDepth, nullptr,
                                  nullptr);
          ImGui::SetTooltip("%s", why.c_str());
        }
      }
    }
    ImGui::EndCombo();
  }

  // --- Target colour space (PRD I5) ---------------------------------------
  if (ImGui::BeginCombo("Colour space", exportTargetSpaceName(request.targetSpace))) {
    for (int i = 0; i < 3; ++i) {
      const auto s = static_cast<ExportTargetSpace>(i);
      if (ImGui::Selectable(exportTargetSpaceName(s), s == request.targetSpace))
        request.targetSpace = s;
    }
    ImGui::EndCombo();
  }

  // --- Bit depth (PRD B6, I5) ---------------------------------------------
  const std::vector<ExportBitDepth> depths = offerableExportDepths(request.format);
  if (ImGui::BeginCombo("Bit depth", exportBitDepthName(request.bitDepth))) {
    for (ExportBitDepth d : depths) {
      if (ImGui::Selectable(exportBitDepthName(d), d == request.bitDepth)) request.bitDepth = d;
    }
    ImGui::EndCombo();
  }

  // --- Resize (PRD I15's "and resize") ------------------------------------
  ImGui::Separator();
  if (ImGui::BeginCombo("Resize", exportResizeModeName(request.resize.mode))) {
    for (std::size_t i = 0; i < kExportResizeModeCount; ++i) {
      const auto m = static_cast<ExportResizeMode>(i);
      if (ImGui::Selectable(exportResizeModeName(m), m == request.resize.mode))
        request.resize.mode = m;
    }
    ImGui::EndCombo();
  }
  if (request.resize.mode == ExportResizeMode::Percent) {
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Percent", &request.resize.percent, 1.0f, 100.0f, "%.1f%%");
  } else if (request.resize.mode == ExportResizeMode::FitWithin) {
    int box[2] = {static_cast<int>(request.resize.maxWidth),
                  static_cast<int>(request.resize.maxHeight)};
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt2("Fit within (px)", box)) {
      request.resize.maxWidth = static_cast<uint32_t>(box[0] > 0 ? box[0] : 0);
      request.resize.maxHeight = static_cast<uint32_t>(box[1] > 0 ? box[1] : 0);
    }
    ImGui::TextDisabled("Aspect preserved; a smaller document is never enlarged.");
  }

  // --- Validation, in io/Export's own words -------------------------------
  const ExportValidation validation = validateExportRequest(
      request, srcW, srcH, activeDoc ? &activeDoc->document.workingSpace : nullptr, nullptr);
  ImGui::Separator();
  if (validation.ok) {
    ImGui::Text("Output: %ux%u", validation.outWidth, validation.outHeight);
    for (const std::string& w : validation.warnings) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("! %s", w.c_str());
      ImGui::PopStyleColor();
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::TextWrapped("%s", validation.error.c_str());
    ImGui::PopStyleColor();
  }

  // --- Presets (PRD I15's "with saveable presets") ------------------------
  ImGui::Separator();
  ImGui::TextUnformatted("Presets");
  if (ImGui::BeginCombo("##presets", "Load a preset...")) {
    if (presets.presets().empty()) ImGui::TextDisabled("(none saved yet)");
    for (const ExportPreset& p : presets.presets()) {
      // A preset this build cannot honour is shown, greyed, with the reason
      // -- never silently dropped and never silently substituted. See
      // io/ExportAs.hpp: this is exactly what an NP_USE_OIIO=ON preset looks
      // like in an OFF build.
      const std::string why = exportRequestAvailability(p.request);
      if (!why.empty()) ImGui::BeginDisabled();
      if (ImGui::Selectable(p.name.c_str()) && why.empty()) {
        request = p.request;
        std::snprintf(presetNameBuf, sizeof(presetNameBuf), "%s", p.name.c_str());
        status = "Loaded preset '" + p.name + "'.";
      }
      if (!why.empty()) {
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", why.c_str());
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputText("##presetName", presetNameBuf, sizeof(presetNameBuf));
  ImGui::SameLine();
  if (ImGui::Button("Save preset")) {
    ExportPreset p;
    p.name = presetNameBuf;
    p.request = request;
    std::string err;
    if (!presets.savePreset(p, &err)) {
      status = err;
    } else if (!presets.saveToFile(presetsPath, &err)) {
      status = err;
    } else {
      status = "Saved preset '" + p.name + "' to " + presetsPath;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete preset")) {
    std::string err;
    if (!presets.removePreset(presetNameBuf)) {
      status = std::string("No preset named '") + presetNameBuf + "' to delete.";
    } else if (!presets.saveToFile(presetsPath, &err)) {
      status = err;
    } else {
      status = std::string("Deleted preset '") + presetNameBuf + "'.";
    }
  }
  if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
  ImGui::TextDisabled("Presets file: %s", presetsPath.c_str());

  // --- Export, and the honest part ----------------------------------------
  ImGui::Separator();
  ImGui::SetNextItemWidth(360.0f);
  ImGui::InputText("Output file", exportPathBuf, sizeof(exportPathBuf));
  ImGui::SameLine();
  if (ImGui::Button("Choose...")) {
    // **One filter row, naming the format this panel is currently set to** --
    // not the whole writable list. macOS appends the *first* allowed type to
    // a bare filename, so offering every writable extension here would let it
    // append `.exr` to a file `exportDocumentWithRequestToFile()` is about to
    // write as a PNG, and the mismatch would only show up when something
    // tried to read it back.
    //
    // The field stays: this fills it in rather than replacing it, because an
    // export path is often typed as a variation on the last one.
    const FileDialogFilterRow row{imageFormatName(request.format),
                                  imageFormatExtension(request.format)};
    if (requestFileDialogWithFilter(FileDialogPurpose::ExportImage,
                                    fileDialogDirectoryOf(exportPathBuf), row))
      exportPathPanelInFlight = true;
    else
      status = "A file panel is already open; finish or cancel it first.";
  }
  const bool canExport = activeDoc != nullptr && validation.ok && exportPathBuf[0] != '\0';
  if (!canExport) ImGui::BeginDisabled();
  if (ImGui::Button("Export...")) {
    std::string err;
    if (exportDocumentWithRequestToFile(activeDoc->document, exportPathBuf, request, &err))
      status = std::string("Exported ") + exportPathBuf;
    else
      status = err;
  }
  if (!canExport) ImGui::EndDisabled();
  ImGui::SameLine();
  if (!activeDoc) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("No document is open, so there is nothing to export. Use File > New "
                       "Document or File > Open... -- the painting canvas is a solver texture, "
                       "not a document, and still has no bridge into one.");
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("Exports the open document, not the painting canvas.");
  }
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ------------------------------------------- Export Comps / Layers To Files
//
// PLAN.md Phase 5 step 13 / PRD I16 + I17. The same split
// drawExportAsDialog() has, and for the same reason: everything that can be
// wrong without a screenshot showing it -- the name template and its path
// refusals, the collision rule, the overwrite rule, the state-set, the
// per-file report -- lives in io/ExportStates.hpp and is exercised headlessly
// by --selftest. This function is widgets.
//
// The one property worth naming here, because it is what the dialog is shaped
// around: **`planStateExport()` writes nothing**, so the table of filenames
// below is not a guess about what Export will do -- it is the same value the
// export loop will consume, recomputed every frame. A user sees the exact
// filenames, the exact skips and the exact refusal before committing, which is
// the whole reason the refusals are a pre-flight rather than a surprise at
// file 3.
bool g_exportStatesRequested = false;

void drawExportStatesDialog(AppState& st) {
  static ExportStatesRequest request;
  static ExportPresetStore presets;
  static bool presetsLoaded = false;
  static char dirBuf[512] = "";
  static char templateBuf[256] = "{name}";
  static std::vector<bool> picked;
  static std::string status;
  static ExportStatesReport lastRun;
  static bool hasRun = false;
  static bool justOpened = false;

  // --open-export-states: the same id BeginPopupModal() opens on a click, so
  // the dialog can be photographed. See AppState::openExportStatesDialog.
  if (g_exportStatesRequested || st.openExportStatesDialog) {
    g_exportStatesRequested = false;
    if (!st.exportStatesFolder.empty() && dirBuf[0] == '\0')
      std::snprintf(dirBuf, sizeof(dirBuf), "%s", st.exportStatesFolder.c_str());
    justOpened = true;
    ImGui::OpenPopup("Export Comps / Layers To Files");
  }
  if (!ImGui::BeginPopupModal("Export Comps / Layers To Files", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  if (!presetsLoaded) {
    presetsLoaded = true;
    presets.loadFromFile(defaultExportPresetsPath());
  }

  const ImVec4 kError(0.95f, 0.45f, 0.40f, 1.0f);
  const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);
  const ImVec4 kGood(0.55f, 0.85f, 0.55f, 1.0f);

  const OpenDocument* activeDoc = st.documents.active();
  if (activeDoc == nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("No document is open, so there are no comps or layers to export. Use "
                       "File > New Document or File > Open... -- the painting canvas is a "
                       "solver texture, not a document.");
    ImGui::PopStyleColor();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  const Document& doc = activeDoc->document;
  request.documentName = documentDisplayName(*activeDoc);
  // Strip an extension from the document name so {doc} does not put ".npaint"
  // in the middle of a filename.
  const size_t dot = request.documentName.rfind('.');
  if (dot != std::string::npos && dot > 0) request.documentName.resize(dot);

  // A dialog that opens on an empty list is a dialog that looks broken. A
  // document with no comps has nothing for PRD I17 to enumerate, and PRD I16
  // is the same loop over a list it certainly does have, so that is where it
  // opens. Only on open, so switching back is never fought.
  if (justOpened) {
    justOpened = false;
    if (doc.comps.empty()) request.source = ExportStateSource::Layers;
    picked.clear();
  }

  ImGui::TextDisabled("Source: document '%s', %zu comps, %zu layers",
                      request.documentName.c_str(), doc.comps.size(), doc.layers.size());
  ImGui::Separator();

  // --- What is enumerated: PRD I17 or PRD I16 -----------------------------
  int sourceIdx = request.source == ExportStateSource::Comps ? 0 : 1;
  const bool sourceChanged = ImGui::RadioButton("Comps (PRD I17)", &sourceIdx, 0);
  ImGui::SameLine();
  const bool sourceChanged2 = ImGui::RadioButton("Layers (PRD I16)", &sourceIdx, 1);
  if (sourceChanged || sourceChanged2) picked.clear();
  request.source = sourceIdx == 0 ? ExportStateSource::Comps : ExportStateSource::Layers;

  // --- The four Export As settings, and a preset that carries them --------
  if (ImGui::BeginCombo("Preset", "Load an Export As preset...")) {
    if (presets.presets().empty()) ImGui::TextDisabled("(none saved yet)");
    for (const ExportPreset& p : presets.presets()) {
      const std::string why = exportRequestAvailability(p.request);
      if (!why.empty()) ImGui::BeginDisabled();
      if (ImGui::Selectable(p.name.c_str()) && why.empty()) {
        request.format = p.request;
        status = "Loaded preset '" + p.name + "'.";
      }
      if (!why.empty()) {
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", why.c_str());
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::BeginCombo("Format", imageFormatName(request.format.format))) {
    for (ImageFormat f : offerableExportFormats()) {
      if (ImGui::Selectable(imageFormatName(f), f == request.format.format)) {
        request.format.format = f;
        const std::vector<ExportBitDepth> depths = offerableExportDepths(f);
        if (!depths.empty() && std::find(depths.begin(), depths.end(), request.format.bitDepth) ==
                                   depths.end())
          request.format.bitDepth = depths.front();
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::BeginCombo("Colour space", exportTargetSpaceName(request.format.targetSpace))) {
    for (int i = 0; i < 3; ++i) {
      const auto s = static_cast<ExportTargetSpace>(i);
      if (ImGui::Selectable(exportTargetSpaceName(s), s == request.format.targetSpace))
        request.format.targetSpace = s;
    }
    ImGui::EndCombo();
  }
  if (ImGui::BeginCombo("Bit depth", exportBitDepthName(request.format.bitDepth))) {
    for (ExportBitDepth d : offerableExportDepths(request.format.format)) {
      if (ImGui::Selectable(exportBitDepthName(d), d == request.format.bitDepth))
        request.format.bitDepth = d;
    }
    ImGui::EndCombo();
  }
  if (ImGui::BeginCombo("Resize", exportResizeModeName(request.format.resize.mode))) {
    for (std::size_t i = 0; i < kExportResizeModeCount; ++i) {
      const auto m = static_cast<ExportResizeMode>(i);
      if (ImGui::Selectable(exportResizeModeName(m), m == request.format.resize.mode))
        request.format.resize.mode = m;
    }
    ImGui::EndCombo();
  }
  if (request.format.resize.mode == ExportResizeMode::Percent) {
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Percent", &request.format.resize.percent, 1.0f, 100.0f, "%.1f%%");
  } else if (request.format.resize.mode == ExportResizeMode::FitWithin) {
    int box[2] = {static_cast<int>(request.format.resize.maxWidth),
                  static_cast<int>(request.format.resize.maxHeight)};
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt2("Fit within (px)", box)) {
      request.format.resize.maxWidth = static_cast<uint32_t>(box[0] > 0 ? box[0] : 0);
      request.format.resize.maxHeight = static_cast<uint32_t>(box[1] > 0 ? box[1] : 0);
    }
  }

  // --- Where, and under what names (PRD I17's "with a name template") -----
  ImGui::Separator();
  ImGui::SetNextItemWidth(420.0f);
  ImGui::InputText("Output folder", dirBuf, sizeof(dirBuf));
  ImGui::SetNextItemWidth(420.0f);
  ImGui::InputText("Name template", templateBuf, sizeof(templateBuf));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 560.0f);
  ImGui::TextWrapped("%s", exportNameTemplateHelp().c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Checkbox("Overwrite files that already exist", &request.overwriteExisting);
  if (!request.overwriteExisting)
    ImGui::TextDisabled("Off: an existing output path refuses the whole batch (PRD P4).");
  request.outputDirectory = dirBuf;
  request.nameTemplate = templateBuf;

  // --- Which ones (PRD I17's "a choice of which comps") -------------------
  const size_t total =
      request.source == ExportStateSource::Comps ? doc.comps.size() : doc.layers.size();
  if (picked.size() != total) picked.assign(total, true);
  ImGui::Separator();
  ImGui::Text("Which %s", exportStateSourcePlural(request.source));
  ImGui::SameLine();
  if (ImGui::SmallButton("All")) picked.assign(total, true);
  ImGui::SameLine();
  if (ImGui::SmallButton("None")) picked.assign(total, false);
  if (ImGui::BeginChild("##pick", ImVec2(420.0f, 96.0f), true)) {
    for (size_t i = 0; i < total; ++i) {
      const std::string label =
          (request.source == ExportStateSource::Comps
               ? (doc.comps[i].name.empty() ? std::string("(unnamed comp)") : doc.comps[i].name)
               : (doc.layers[i].name.empty() ? std::string("(unnamed layer)")
                                             : doc.layers[i].name)) +
          "##" + std::to_string(i);
      bool on = picked[i];
      if (ImGui::Checkbox(label.c_str(), &on)) picked[i] = on;
    }
  }
  ImGui::EndChild();
  request.selection.clear();
  for (size_t i = 0; i < total; ++i)
    if (picked[i]) request.selection.push_back(i);
  // An empty `selection` means "all" to io/ExportStates, which is not what an
  // empty set of checkboxes means here. Said rather than silently exporting
  // everything.
  const bool noneChosen = request.selection.empty();

  // --- The plan: exactly what a click would write, computed for free ------
  ImGui::Separator();
  // Recomputed every frame rather than cached, which is the right trade here
  // and is flagged rather than assumed: the plan is a pure function of controls
  // the user is editing, so caching it would mean an invalidation rule with one
  // entry per control. It does cost one `stat` per planned file per frame while
  // the modal is open (the exists-check of io/ExportStates §7). That is a
  // handful of stats on a local disk; if this ever runs against a slow network
  // mount, the fix is to cache on a controls-changed hash, not to drop the
  // check.
  ExportStatesReport plan;
  if (!noneChosen) plan = planStateExport(doc, request);
  if (noneChosen) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("Nothing is selected, so there is nothing to export.");
    ImGui::PopStyleColor();
  } else if (!plan.error.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::TextWrapped("%s", plan.error.c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::Text("Will write %zu file%s (%zu skipped):", plan.items.size() - plan.skipped(),
                plan.items.size() - plan.skipped() == 1 ? "" : "s", plan.skipped());
    if (ImGui::BeginChild("##plan", ImVec2(560.0f, 120.0f), true)) {
      for (const ExportStateItem& item : plan.items) {
        if (item.filename.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
          ImGui::TextWrapped("skipped -- %s", item.reason.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::TextUnformatted(item.filename.c_str());
        }
      }
    }
    ImGui::EndChild();
  }

  // --- Export, and the per-file report (PRD P4) ---------------------------
  ImGui::Separator();
  const bool canExport = !noneChosen && plan.error.empty();
  if (!canExport) ImGui::BeginDisabled();
  if (ImGui::Button("Export")) {
    lastRun = exportDocumentStates(doc, request);
    hasRun = true;
    status = exportStatesSummary(lastRun);
  }
  if (!canExport) ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("Exports the open document's %s, never the painting canvas.",
                      exportStateSourcePlural(request.source));

  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, lastRun.ok ? kGood : kError);
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  if (hasRun && !lastRun.items.empty()) {
    // Per file, always -- never one line claiming a count. PRD P4.
    if (ImGui::BeginChild("##report", ImVec2(560.0f, 120.0f), true)) {
      for (const ExportStateItem& item : lastRun.items) {
        const bool bad = item.outcome == ExportItemOutcome::Failed ||
                         item.outcome == ExportItemOutcome::NotAttempted;
        if (bad) ImGui::PushStyleColor(ImGuiCol_Text, kError);
        ImGui::TextWrapped("%-13s %s%s%s", exportItemOutcomeName(item.outcome),
                           item.filename.empty() ? item.stateName.c_str() : item.filename.c_str(),
                           item.reason.empty() ? "" : " -- ", item.reason.c_str());
        if (bad) ImGui::PopStyleColor();
        for (const std::string& w : item.warnings) {
          ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
          ImGui::TextWrapped("    ! %s", w.c_str());
          ImGui::PopStyleColor();
        }
      }
    }
    ImGui::EndChild();
  }

  if (ImGui::Button("Close")) {
    st.openExportStatesDialog = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ------------------------------------------------------- Document lifecycle
//
// PLAN.md Phase 4 step 8 / PRD I18 ("Revert, duplicate document, save a copy,
// save incremental, open recent"). Everything with a decision in it lives in
// app/DocumentLifecycle.hpp and is exercised headlessly by --selftest; this is
// the widgets, the same split step 7 and app/CurveEdit already use.
//
// This comment used to open by saying **there is no native file picker in
// this codebase**, that adding one was "a platform-integration job (NSOpenPanel
// behind an interface) rather than part of a lifecycle step", and that Open,
// Save As and Save a Copy therefore took a typed path in a small modal.
//
// That is no longer true, and the platform-integration job turned out to be
// nobody's: SDL3's vendored copy already ships an AppKit backend for exactly
// this (`src/dialog/cocoa/SDL_cocoadialog.m` -- a real `NSOpenPanel` /
// `NSSavePanel`, presented as a sheet), and this configuration already links
// it. ui/FileDialog wraps it, and every action in this enum now opens the
// OS's own panel. **The typed-path modal is gone**; the field, its pre-fill
// rule and its 512-byte buffer went with it.
//
// What did not change is the shape around it, and that is deliberate: the
// enum, the request flag, `applyDocumentPathAction()` and the status line are
// all exactly as they were. A panel is one more way of producing a string, so
// the five actions that consume one did not need to know which way it came.
//
// Which state lives where follows app/AppState.hpp's rule: the session and
// the recent list are on AppState; the pending action and the last status
// line are function-local, because they are widget state.
//
// **`ImportBrushes` is the third caller**, and it is here rather than in the
// BRUSH LIBRARY pane for exactly the reason `ImportImage` is: the `+` in that
// pane needs a path from the user, and there is one place in this build that
// asks for one. It is the odd one out in this enum in that it touches no
// document at all -- it reads a `.abr` into the application's brush library --
// which is why `applyDocumentPathAction()` returns from its case rather than
// falling into the `DocumentOpResult` tail below.
enum class DocPathAction { None, Open, SaveAs, SaveCopy, ImportImage, ImportBrushes };

bool g_docPathRequested = false;
DocPathAction g_docPathAction = DocPathAction::None;
bool g_revertConfirmRequested = false;
std::string g_docStatus;
// Whether the last `applyDocumentPathAction()` succeeded, which is how the path
// modal decides to close itself.
//
// It used to read `g_docStatus.rfind("OK: ", 0) == 0`. That worked only because
// every action's success message happened to start with the same four
// characters, and Import Image's does not: an import writes no file, so "OK:
// <path>" would be a sentence about something that did not happen. A boolean
// says the thing the modal actually needs to know, and cannot be broken by a
// future action wording its success differently.
bool g_docPathActionOk = false;

// Which action the panel *currently on screen* is for.
//
// Separate from `g_docPathAction`, and it has to be. `g_docPathAction` is set
// by whoever raises the request, and the OS panel is asynchronous -- so a
// second request arriving while a panel is up (the native menu bar still
// works while a sheet is presented) would rewrite `g_docPathAction` under the
// panel the user is looking at, and the file they then pick would be applied
// as the *new* action. Save As followed by Open, answered once, would open
// the file the user meant to save over. This records what was actually asked,
// at the moment it was asked, and is what the outcome is applied as.
//
// `None` means no panel of ours is in flight, which is also how this
// function's poll knows an outcome in the mailbox belongs to the Export As
// panel rather than to it.
DocPathAction g_docPathInFlight = DocPathAction::None;

// The action a failed one is offered a second go at, and the popup that
// offers it. See `drawDocumentDialogs()`.
DocPathAction g_docPathProblemAction = DocPathAction::None;
constexpr const char* kDocPathProblemPopup = "File problem";

// The word for an action, in the user's terms. One place, because it is now
// read by three: the panel's title and accept button (through
// ui/FileDialog's plan), the failure popup's heading, and the status line.
const char* docPathActionVerb(DocPathAction action) {
  switch (action) {
    case DocPathAction::Open: return "open";
    case DocPathAction::SaveAs: return "save";
    case DocPathAction::SaveCopy: return "save a copy of";
    case DocPathAction::ImportImage: return "import";
    case DocPathAction::ImportBrushes: return "import";
    case DocPathAction::None: return "use";
  }
  return "use";
}

// The panel each action wants. Total, and with no `default:` -- an action
// added here without a panel is a build failure (`-Werror=switch`), not an
// action whose menu item quietly does nothing.
//
// `None` maps to `OpenDocument` and is never requested: the request site
// checks the flag first. It is here because a `switch` that returns from
// every case still has to have every case.
FileDialogPurpose fileDialogPurposeFor(DocPathAction action) {
  switch (action) {
    case DocPathAction::Open: return FileDialogPurpose::OpenDocument;
    case DocPathAction::SaveAs: return FileDialogPurpose::SaveDocument;
    case DocPathAction::SaveCopy: return FileDialogPurpose::SaveCopy;
    case DocPathAction::ImportImage: return FileDialogPurpose::ImportImage;
    case DocPathAction::ImportBrushes: return FileDialogPurpose::ImportBrushes;
    case DocPathAction::None: return FileDialogPurpose::OpenDocument;
  }
  return FileDialogPurpose::OpenDocument;
}

// Forward-declared up beside the BRUSH LIBRARY pane, which is drawn earlier in
// this file. Sets the same two globals File > Open does, so the `+` cannot
// end up with a second panel of its own.
void requestBrushLibraryImport() {
  g_docPathAction = DocPathAction::ImportBrushes;
  g_docPathRequested = true;
}

void applyDocumentPathAction(AppState& st, DocPathAction action, const std::string& path) {
  OpenDocument* doc = st.documents.active();
  DocumentOpResult r;
  g_docPathActionOk = false;
  switch (action) {
    case DocPathAction::Open: {
      // **Any file this build can read, dispatched on its contents.**
      //
      // This used to be `openNpaintDocument()` and therefore opened `.npaint`
      // and nothing else -- while io/ImageIO's `openImageAsDocument()` sat
      // finished and asserted with no caller outside `--selftest`, its own
      // header saying it was "the function a future File > Open would call".
      // app/OpenAnyFile is that call, and it decides which reader a file goes
      // to from the file's bytes rather than from its name: a `.npaint` is an
      // OpenEXR carrying `np:version`, so one saved as `.exr` (PRD I8 -- the
      // same container under a different name) still opens as a document, and a
      // PNG someone named `sketch.npaint` still opens as a picture.
      //
      // **Not folded into the `r.ok` path below**, for exactly the reason the
      // import case is not: that path prefixes its status with "OK: <path>",
      // which is a sentence about a file that was *written*. An open writes
      // nothing, and app/OpenAnyFile's own sentence already names the file,
      // what kind of thing it turned out to be, and -- for a picture -- that
      // the new document is bound to no file yet.
      OpenAnyResult opened = openAnyFileAsDocument(path, &st.recentDocuments);
      g_docStatus = opened.status;
      for (const std::string& w : opened.warnings) g_docStatus += "\n! " + w;
      g_docPathActionOk = opened.ok;
      if (opened.ok) {
        st.documents.add(std::move(opened.document));
        // app/OpenAnyFile has already decided *whether* to add a recent entry
        // (its header says why a picture cannot go in that list yet); this is
        // only the write-through to disk, kept on the one success path so it
        // stays beside the save-as and save-a-copy ones below.
        std::string saveErr;
        st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
      }
      return;
    }
    case DocPathAction::SaveAs:
      if (!doc) return;
      r = saveDocumentAs(*doc, path, {}, &st.recentDocuments);
      break;
    case DocPathAction::SaveCopy:
      if (!doc) return;
      // No RecentDocuments argument exists on this one -- see
      // app/DocumentLifecycle.hpp on why a copy is not an open document.
      r = saveDocumentCopy(*doc, path);
      break;
    case DocPathAction::ImportImage: {
      // The one caller of app/ImportImage in the running application, and the
      // one a drag-and-drop handler would join rather than duplicate (there is
      // no SDL_EVENT_DROP_FILE handler anywhere in src/ today -- see
      // app/ImportImage.hpp).
      if (!doc) return;
      const ImportImageResult imported = importImageAsLayer(*doc, path);
      // **Not folded into the `r.ok` path below.** That path is for operations
      // that wrote a file and prefixes its status with "OK: <path>"; an import
      // wrote nothing, and its own sentence already names the file, the pixel
      // size and the layer. Routing it through the generic branch would replace
      // a true sentence with a misleading one.
      g_docStatus = imported.status;
      for (const std::string& w : imported.warnings) g_docStatus += "\n! " + w;
      // Read by the modal below to decide whether to close. A refused import
      // keeps the dialog up with the reason under the text field, exactly as a
      // refused save does, rather than dropping the user back to the canvas
      // with the one thing they needed to read sitting one line high beside the
      // menus.
      g_docPathActionOk = imported.ok;
      return;
    }
    case DocPathAction::ImportBrushes: {
      // **No document is required**, unlike every other case here: a brush
      // library belongs to the application, not to a canvas. Importing one
      // with nothing open is a perfectly reasonable thing to do before
      // starting a drawing.
      //
      // The preferences file may not have been read yet -- it is read on the
      // first frame the BRUSH LIBRARY pane draws, and this can be reached from
      // the menu bar with that pane collapsed. Reading it here first is what
      // stops an import from writing a file that has forgotten every library
      // the user already had.
      if (!st.brushLibrariesLoaded) {
        st.brushLibrariesLoaded = true;
        std::string loadErr;
        (void)st.brushLibraries.loadFromFile(defaultBrushLibraryFilePath(),
                                             st.brush.brushLibrary, &loadErr);
      }
      const BrushLibraryLoadResult imported =
          st.brushLibraries.importFile(path, st.brush.brushLibrary);
      g_docStatus = imported.status;
      g_brushLibraryStatus = imported.status;
      g_brushLibraryNotes = imported.notes;
      if (imported.ok) {
        std::string saveErr;
        if (!st.brushLibraries.saveToFile(defaultBrushLibraryFilePath(), st.brush.brushLibrary,
                                          &saveErr))
          g_docStatus += "\n! " + saveErr;
      }
      // Same contract as the image import above: a refusal keeps the dialog up
      // with the reason under the text field rather than dropping the user
      // back to the canvas with the one thing they needed to read sitting one
      // line high beside the menus.
      g_docPathActionOk = imported.ok;
      return;
    }
    case DocPathAction::None:
      return;
  }
  g_docPathActionOk = r.ok;
  if (r.ok) {
    std::string saveErr;
    st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    g_docStatus = "OK: " + r.path;
    for (const std::string& w : r.warnings) g_docStatus += "\n! " + w;
  } else {
    g_docStatus = r.error;
  }
}

// The one dialog for the one decision (app/CloseDecision.hpp). Opened from
// `AppState::pendingClose` rather than from a request flag, so whichever close
// path raised the question -- the tab strip's `x` or File > Close Document --
// this is what answers it, and neither path can grow a second dialog of its
// own that says something different.
constexpr const char* kCloseDecisionPopup = "Close document";

// **Every answer to that dialog goes through here**, and that is the point
// rather than tidiness.
//
// The popup has more exits than it has buttons -- three buttons, two keys, the
// document vanishing underneath it, and a dismissal from outside -- and a quit
// sequence (app/QuitSequence.hpp) is waiting on whichever of them fires. One
// exit that called `resolveDocumentClose()` directly would answer the question
// and leave the quit stranded: no next question, no exit, and a `quitSequence`
// stuck `running` so that every later Cmd-Q is refused for the rest of the
// session. With no quit in flight this is exactly `resolveDocumentClose()`.
CloseOutcome answerPendingClose(AppState& st, CloseAnswer answer) {
  CloseOutcome outcome;
  const QuitStep step =
      answerQuitQuestion(st.documents, st.pendingClose, st.quitSequence, answer,
                         documentSaverFor(&st.recentDocuments), &outcome);
  if (!step.status.empty()) g_docStatus = step.status;
  // The one line in the UI that ends the process, and it is reachable only by
  // emptying the queue of dirty documents -- never by `--screenshot`, which
  // does not run a sequence and does not come through this function.
  if (step.exitNow) st.quit = true;
  return outcome;
}

// Whether a file panel of `drawDocumentDialogs()`'s is in the middle of
// asking something -- from the frame the request is raised, through the panel
// being up, through the frame its answer is applied, and on through the
// failure popup if that answer could not be carried out.
//
// The pending-close path (app/CloseDecision.hpp) waits on this, and the
// window it has to cover is wider than "a panel is on screen": a request
// raised this frame has no panel yet, and an answer that failed has none any
// more but is still being asked about.
bool docPathDialogBusy() {
  return g_docPathRequested || g_docPathInFlight != DocPathAction::None ||
         ImGui::IsPopupOpen(kDocPathProblemPopup);
}

void drawDocumentDialogs(AppState& st) {
  // Why not `g_docStatus`: a failed save has to stay legible in the dialog
  // until the user does something about it, and `g_docStatus` is overwritten
  // by every other document operation and is drawn one line high beside the
  // menus. Widget state, so function-local, per app/AppState.hpp's rule.
  static std::string closeDialogError;

  // --- Save / Don't Save / Cancel -----------------------------------------
  //
  // First in the function, so that answering `Save` on a document that has
  // never been saved can hand straight over to the path dialog below in the
  // same frame rather than leaving a blank one in between.
  //
  // `wasOpen` is read before anything can open or close the popup, which is
  // what lets the `else` branch below tell "the popup was dismissed without an
  // answer" apart from "we have only just asked for it".
  const bool closePopupWasOpen = ImGui::IsPopupOpen(kCloseDecisionPopup);
  if (st.pendingClose.asking() && !closePopupWasOpen) {
    closeDialogError.clear();
    ImGui::OpenPopup(kCloseDecisionPopup);
  }
  if (ImGui::BeginPopupModal(kCloseDecisionPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    OpenDocument* closing = st.documents.find(st.pendingClose.document);
    if (closing == nullptr) {
      // Closed by something else while the question was up, so there is
      // nothing left to ask about. Answered as Cancel: it is the one answer
      // that cannot act on a document at all, which is the right thing to send
      // when the user never got to see the question they are being answered
      // for.
      //
      // A quit sequence waiting on this question is abandoned by that Cancel,
      // which is the conservative reading and the deliberate one: the user is
      // owed a question about each remaining dirty document, and the honest way
      // to get back to asking them is the next Cmd-Q rather than carrying on
      // through a sequence whose current entry evaporated.
      (void)answerPendingClose(st, CloseAnswer::Cancel);
      st.pendingClose.clear();
      ImGui::CloseCurrentPopup();
    } else {
      // The question names the document and the work, in app/CloseDecision's
      // words -- which are PRD I11's own `unsavedWorkSummary()`. Read from the
      // live record, so a rename behind the dialog renames the dialog.
      ImGui::TextWrapped("%s", closeQuestion(*closing).c_str());
      ImGui::Spacing();

      // **Don't Save is set apart, on the left, in the warning colour.** It is
      // the only button here that destroys anything, so it does not sit in the
      // row where a user's hand goes -- macOS's own arrangement for a
      // destructive third choice, and the reason for the 40 px gap rather than
      // the usual item spacing: a mis-aimed click on Cancel must not land on
      // it.
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
      const bool pressedDontSave = ImGui::Button("Don't Save");
      ImGui::PopStyleColor();
      ImGui::SameLine(0.0f, 40.0f);
      const bool pressedCancel = ImGui::Button("Cancel");
      ImGui::SameLine();
      const bool pressedSave = ImGui::Button("Save");

      // The two keys, through app/CloseDecision's mapping rather than through
      // a pair of literals here -- the mapping is asserted in `--selftest` and
      // this is the call site that has to be the one being asserted.
      //
      // Handled explicitly because this build does not set
      // `ImGuiConfigFlags_NavEnableKeyboard`, so ImGui neither activates a
      // focused button on Enter nor closes a modal on Escape; both keys are
      // ours to read. **No key can reach Don't Save** -- there is no third
      // branch here and `closeAnswerForKey()` has no third answer to give.
      std::optional<CloseAnswer> answer;
      if (pressedDontSave) answer = CloseAnswer::DontSave;
      else if (pressedCancel) answer = CloseAnswer::Cancel;
      else if (pressedSave) answer = CloseAnswer::Save;
      else if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        answer = closeAnswerForKey(CloseKey::Escape);
      else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
        answer = closeAnswerForKey(CloseKey::Enter);

      if (answer) {
        const CloseOutcome outcome = answerPendingClose(st, *answer);
        if (outcome.needsDestination) {
          // No native file picker exists in this build, so Save on a document
          // that has never been saved falls back to the Save As dialog below
          // -- the same one File > Save As... opens, not a second one. The
          // pending close stays alive across it and is finished (or dropped)
          // by the block that follows that dialog.
          //
          // **The document being closed is made active first, and it has to
          // be.** `applyDocumentPathAction()` writes `st.documents.active()`,
          // so closing an unsaved tab that was *not* the active one and
          // choosing Save would otherwise write the wrong document to the
          // typed path -- silently, and over whatever the user typed. Bringing
          // it to the front is also what the user is owed: the file name they
          // are about to type is for the document the question named, and they
          // should be looking at it while they type. The cost is that backing
          // out of the file name leaves that tab active rather than the one
          // that was; the alternative costs a file.
          if (const std::optional<size_t> pendingIndex =
                  documentIndexById(st.documents, st.pendingClose.document))
            st.documents.setActive(*pendingIndex);
          g_docPathAction = DocPathAction::SaveAs;
          g_docPathRequested = true;
        }
        // Still asking, after an answer, has two causes now and only one of
        // them is a failure:
        //
        //  * the answer could not be carried out -- a save that failed. The
        //    dialog stays up with the writer's own error beside it rather than
        //    closing on a document that is still dirty.
        //  * a quit sequence has just moved this same dialog on to the *next*
        //    dirty document (app/QuitSequence). Nothing went wrong, and
        //    painting "Closed Study, discarding its unsaved changes." in the
        //    error colour underneath the next document's question would read as
        //    though it had.
        //
        // So the error is set from what the outcome *did*, not from whether a
        // question happens to be up afterwards.
        const bool answerFailed = !outcome.closed && !outcome.vanished &&
                                  !outcome.needsDestination && st.pendingClose.asking();
        closeDialogError = answerFailed ? outcome.status : std::string();
        if (!st.pendingClose.asking()) ImGui::CloseCurrentPopup();
      }
      if (!closeDialogError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
        ImGui::TextWrapped("%s", closeDialogError.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::EndPopup();
  } else if (st.pendingClose.asking() && closePopupWasOpen) {
    // The popup was open at the top of this frame and is not open now, and no
    // button above ran -- so something outside this block dismissed it. The
    // only honest reading of a question dismissed without an answer is the
    // answer that changes nothing, which is what Escape means too -- and, when
    // a quit is in flight, what abandons it.
    (void)answerPendingClose(st, closeAnswerForKey(CloseKey::Escape));
  }

  // --- The OS file panel ---------------------------------------------------
  //
  // Raised on a flag and serviced here, rather than by `performMenuAction()`
  // calling into SDL where the menu item is handled. That was already the rule
  // (ui/MenuModel.hpp's `MenuEffect::Deferred`, asserted by
  // app/selftest/MenuModel.cpp) and the native panel makes it a harder one:
  // `SDL_ShowFileDialogWithProperties()` is documented main-thread-only, and a
  // native menu callback fires from inside SDL's Cocoa pump with no frame in
  // progress.
  if (g_docPathRequested) {
    g_docPathRequested = false;
    g_docStatus.clear();
    // Where the panel opens. The typed-path modal pre-filled the field with
    // the active document's own path; a panel gets the *folder* only, because
    // that is all SDL's backend exposes (ui/FileDialog.hpp says what that
    // costs). An import is reading someone else's file, so the open
    // document's folder is a guess with nothing behind it and the OS's own
    // last-used directory is the better one.
    std::string startDir;
    if (g_docPathAction != DocPathAction::ImportImage &&
        g_docPathAction != DocPathAction::ImportBrushes) {
      if (const OpenDocument* d = st.documents.active())
        startDir = fileDialogDirectoryOf(d->path);
    }
    if (requestFileDialog(fileDialogPurposeFor(g_docPathAction), startDir)) {
      g_docPathInFlight = g_docPathAction;
    } else {
      // A panel is already up -- ours or the Export As one. Saying so is the
      // point: a menu item that appears to do nothing is the defect this
      // whole audit exists about, and the panel the user cannot see may be
      // behind the window.
      g_docStatus = "A file panel is already open; finish or cancel it first.";
    }
  }
  // The outcome, once the user has answered. Guarded on our own in-flight
  // marker rather than on the mailbox alone, because the Export As panel
  // shares that mailbox and its answer is not ours to apply.
  if (g_docPathInFlight != DocPathAction::None) {
    if (const std::optional<FileDialogOutcome> picked = takeFileDialogOutcome()) {
      const DocPathAction action = g_docPathInFlight;
      g_docPathInFlight = DocPathAction::None;
      if (picked->chose) {
        applyDocumentPathAction(st, action, picked->path);
        if (!g_docPathActionOk) {
          // The typed-path modal kept itself up on a refusal, with the reason
          // under the field, and the argument for that has not changed: the
          // one thing the user needs to read must not be left sitting one
          // line high beside the menus while they are looking at the canvas.
          // There is no field to keep up any more, so this is the smallest
          // thing that keeps the property -- the reason, and a second go at
          // it that does not make them find the menu item again.
          g_docPathProblemAction = action;
          ImGui::OpenPopup(kDocPathProblemPopup);
        }
      } else if (!picked->error.empty()) {
        g_docStatus = picked->error;
      } else {
        // Cancelled. Not an error, and deliberately not reported as one --
        // `g_docStatus` was cleared when the panel was raised, so the status
        // line simply goes quiet.
      }
    }
  }
  if (ImGui::BeginPopupModal(kDocPathProblemPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Could not %s that file.", docPathActionVerb(g_docPathProblemAction));
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", g_docStatus.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (ImGui::Button("Choose Another File...")) {
      // Read before the popup closes; `g_docPathProblemAction` is not cleared
      // by closing, but the request below overwrites `g_docPathAction` and
      // the two are easy to confuse from a distance.
      const DocPathAction retry = g_docPathProblemAction;
      ImGui::CloseCurrentPopup();
      g_docPathAction = retry;
      g_docPathRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // A close that was waiting for a file name, now that the dialog above has
  // gone one way or the other.
  //
  // Here rather than on that dialog's "Save As" button because the popup has
  // more exits than it has buttons -- the button, its Cancel, and any future
  // dismissal -- and a pending close left dangling behind one of them would
  // block every subsequent close for the rest of the session with "is already
  // waiting for an answer".
  //
  // **The test is not `IsPopupOpen("Document path")` any more, and getting it
  // wrong would be silent.** That popup no longer exists; a native panel is
  // not an ImGui popup, so the old test would have read "false" on the very
  // frame the panel went up, resolved this block immediately, and backed out
  // of every close-with-save before the user had seen a file panel at all.
  // `docPathDialogBusy()` is true from the moment the request is raised until
  // its outcome has been applied, and stays true while the failure popup is
  // up.
  if (st.pendingClose.awaitingDestination && !docPathDialogBusy()) {
    const OpenDocument* target = st.documents.find(st.pendingClose.document);
    st.pendingClose.awaitingDestination = false;
    if (target != nullptr && target->hasPath() && !target->isDirty()) {
      // Save As did exactly what Save would have. Answering Save again
      // finishes the close, and finds nothing left to write (see
      // app/CloseDecision's already-clean branch), so the file is not written
      // a second time.
      (void)answerPendingClose(st, CloseAnswer::Save);
    } else {
      // Backing out of the file name backs out of the close. Re-raising the
      // question instead would bounce the user between two dialogs with no way
      // out that did not either write a file or discard their work.
      //
      // And it backs out of a quit that was waiting behind it, for the same
      // reason Cancel does: the user declined to name a file for work they were
      // asked about, which is not an answer that can be carried forward to the
      // next document.
      // The name is read before the clear, not after: `PendingClose::clear()`
      // empties it, and a sentence naming '' is worse than no sentence.
      const std::string backedOut = st.pendingClose.name;
      g_docStatus = "Close cancelled: '" + backedOut + "' has not been saved.";
      st.pendingClose.clear();
      const QuitStep abandoned =
          abandonQuit(st.quitSequence, "Quit cancelled: '" + backedOut +
                                           "' was not saved, so nothing else was closed.");
      if (!abandoned.status.empty()) g_docStatus = abandoned.status;
    }
  }

  // Revert's confirmation is the refusal itself: revertDocument() is called
  // without the discard flag, and its own PRD I11 message -- which names the
  // edits -- is what the dialog shows. There is no second sentence here to
  // drift from it.
  if (g_revertConfirmRequested) {
    g_revertConfirmRequested = false;
    g_docStatus.clear();
    ImGui::OpenPopup("Revert");
  }
  if (ImGui::BeginPopupModal("Revert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    OpenDocument* doc = st.documents.active();
    if (!doc) {
      ImGui::CloseCurrentPopup();
    } else {
      // Attempted, not probed: a clean document has nothing to confirm, so
      // the unconfirmed call simply succeeds and the popup closes. A dirty
      // one refuses *before* touching anything (see revertDocument()'s order
      // of checks), so calling it every frame the popup is open is free.
      const DocumentOpResult attempt = revertDocument(*doc, {});
      if (attempt.ok) {
        g_docStatus = "Reverted " + attempt.path;
        ImGui::CloseCurrentPopup();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", attempt.error.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Discard and revert")) {
          const DocumentOpResult done = revertDocument(*doc, {true});
          g_docStatus = done.ok ? "Reverted " + done.path : done.error;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------ Recovery offer
//
// PLAN.md Phase 4 step 9 / PRD O8: "Unclean scratch directories are offered
// for recovery on launch, named and dated; never opened silently, never
// auto-deleted." The list is discovered by main() before the journal's own
// session begins and lives on AppState; this is the offer.
//
// Three things the widgets here must not get wrong, each of which is a
// property of app/Journal rather than of this dialog, and none of which this
// dialog is allowed to work around:
//
//  * **Nothing opens by itself.** The modal appears; a document arrives in
//    the session only when its own Recover button is pressed.
//  * **Declining deletes nothing.** "Later" closes the dialog and leaves
//    every byte where it was, so the same offer is made next launch. Only
//    the explicitly labelled Discard button removes anything, and it says
//    what it is about to remove.
//  * **A damaged entry is shown, not hidden.** An entry whose model file is
//    truncated is listed with app/Journal's own sentence and no Recover
//    button, because the alternative -- omitting it -- would look identical
//    to work that was never journalled at all.
bool g_recoveryRequested = false;
std::string g_recoveryStatus;

void drawRecoveryDialog(AppState& st) {
  // Opened once, on the first frame after launch, when there is something to
  // offer. `recoveryOfferPending` is cleared as the popup is opened rather
  // than when it closes, so declining is not re-asked every frame.
  if (st.recoveryOfferPending && !st.recovery.empty()) {
    st.recoveryOfferPending = false;
    g_recoveryRequested = true;
  }
  if (g_recoveryRequested) {
    g_recoveryRequested = false;
    g_recoveryStatus.clear();
    ImGui::OpenPopup("Recover Documents");
  }
  if (!ImGui::BeginPopupModal("Recover Documents", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);
  if (st.recovery.empty()) {
    ImGui::TextDisabled("No unfinished sessions were found.");
  } else {
    ImGui::TextWrapped("These sessions ended without shutting down. Nothing has been opened "
                       "or deleted.");
  }
  if (!journalAvailable()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", journalUnavailableReason().c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Separator();

  for (size_t s = 0; s < st.recovery.size(); ++s) {
    RecoverySession& session = st.recovery[s];
    ImGui::PushID(static_cast<int>(s));
    // PRD O8's "named and dated": the date the session *started*, and the
    // documents' own names.
    ImGui::Text("Session of %s  --  %zu document(s)", session.startedAtLocal.c_str(),
                session.documents.size());
    ImGui::TextDisabled("%s", session.directory.c_str());
    for (const std::string& p : session.problems) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("%s", p.c_str());
      ImGui::PopStyleColor();
    }
    for (size_t d = 0; d < session.documents.size(); ++d) {
      const RecoveryDocument& entry = session.documents[d];
      ImGui::PushID(static_cast<int>(d));
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("%s", entry.displayName.c_str());
      if (!entry.unsavedSummary.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", entry.unsavedSummary.c_str());
      }
      if (entry.intact) {
        ImGui::SameLine();
        if (ImGui::Button("Recover")) {
          OpenDocument recovered;
          const DocumentOpResult r = recoverDocument(entry, &recovered);
          if (r.ok) {
            g_recoveryStatus = "Recovered " + documentDisplayName(recovered) +
                               ". It is unsaved -- save it where you want it kept.";
            st.documents.add(std::move(recovered));
          } else {
            g_recoveryStatus = r.error;
          }
        }
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("%s", entry.problem.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::PopID();
    }
    if (ImGui::Button("Discard this session")) {
      std::string discardError;
      if (discardRecoverySession(session, &discardError)) {
        g_recoveryStatus = "Discarded " + session.directory;
        st.recovery.erase(st.recovery.begin() + static_cast<std::ptrdiff_t>(s));
        ImGui::PopID();
        break;
      }
      g_recoveryStatus = discardError;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(deletes the files above; there is no undo)");
    ImGui::Separator();
    ImGui::PopID();
  }

  if (!g_recoveryStatus.empty()) ImGui::TextWrapped("%s", g_recoveryStatus.c_str());
  if (ImGui::Button("Later")) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  ImGui::TextDisabled("Nothing is deleted; this is offered again next launch.");
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// The Filter menu's four dialogs (docs/reachability-audit.md C1; app/FilterOps
// does the actual work)
// ---------------------------------------------------------------------------
//
// Same idiom as `drawExportAsDialog()` throughout: a modal opened from a
// `g_...Requested` flag set by `performMenuAction()` (ui/MenuModel.hpp's
// `MenuEffect::Deferred` -- a native menu's callback cannot call
// `ImGui::OpenPopup()` itself), `ImGuiWindowFlags_AlwaysAutoResize`, a
// verb-named confirm button and a `Cancel` beside it, function-local
// `static` fields for the dialog's own widget state (this is UI state, not
// `AppState`'s, per that struct's own ownership rule). The pixel that
// actually changes is still, in every case, `app/FilterOps.cpp`'s four
// `applyX()` functions -- the ones `--selftest` (app/selftest/FilterMenu.cpp)
// also calls, so the dialog and the test cannot disagree about what
// confirming one does.
//
// **All four now carry a live preview (docs/testing-issues.md T15).** Each
// dialog calls the matching `previewX()` (app/FilterOps.hpp) on every frame
// its own parameters actually change, and hands the result to
// `setFilterPreview()` below, which the canvas draw code
// (`filterPreviewViewFor()`, consulted where `addCanvasQuad()` draws the
// document) shows INSTEAD of the real document texture until the dialog
// closes. Nothing is written to the document until the confirm button:
// `previewX()` takes `const OpenDocument&` and cannot mutate it, so Cancel
// (or Escape, or simply closing the window) needs no undo step -- there is
// nothing to undo. See `FilterPreviewState` and `FilterPreviewTexture` below
// for the mechanism and this task's own report for its measured cost against
// PRD F3.
//
// **Why there is no in-dialog refusal banner.** The Filter menu's four items
// are already disabled, with the reason in their tooltip, whenever
// `ctx.filterLayerUsable` is false (`menuContextFromState()`, `buildMenuModel()`
// above) -- so an ImGui click cannot open one of these popups on a layer that
// cannot take it. `applyX()` re-checks anyway (this file's own header
// explains why: the confirm button fires at least one frame after the menu
// click that opened the dialog, and the guard belongs where it is relied on).
// The refusal message is still shown if that re-check ever fires, so a race
// is reported rather than silently eaten -- it is just not the ordinary path.
// `previewX()` shares that same re-check (`computePixelFilter()`'s call to
// `pixelOpRefusalFor()`), so a preview that cannot be computed simply is not
// shown -- the canvas keeps drawing the real, unfiltered document until the
// confirm button's own refusal message explains why.

// ---------------------------------------------------------------------------
// T15's live preview: a display-only overlay, never the committed document
// ---------------------------------------------------------------------------
//
// The two shapes this could have taken (docs/testing-issues.md T15 states
// the choice explicitly): run the real op and let Cancel undo it, or compute
// into a scratch buffer and draw that instead of the layer, discarding it on
// Cancel. This is the second one. The first was rejected there for two
// reasons that hold just as hard from this side of the wiring: it would put
// a rejected filter into the undo history the user then has to see past, and
// it would re-run the full-resolution op on every slider tick with no way to
// tell "the user is dragging" from "the user is done" apart from waiting for
// mouse-up -- which the display-only overlay does not need to know at all.
enum class FilterPreviewOwner {
  None,
  GaussianBlur,
  Sharpen,
  UnsharpMask,
  AddNoise,
  Emboss,
  Median,
  MotionBlur,
  // Image > Adjustments' four dialogs (app/AdjustmentOps). They share this
  // enum with the Filter menu's seven rather than getting a parallel one,
  // because they share the machinery it identifies: one preview at a time,
  // owned by whichever modal is open, cleared by whichever modal closes. Two
  // owner enums would mean two previews could be live at once and the canvas
  // would have to pick.
  AdjustLevels,
  AdjustCurves,
  AdjustExposure,
  AdjustChannelMixer,
  AdjustBrightnessContrast,
  AdjustHueSaturation,
  AdjustVibrance,
  AdjustColorBalance,
  AdjustBlackAndWhite,
  AdjustPhotoFilter,
  AdjustPosterize,
  AdjustThreshold,
  AdjustGradientMap,
  // Not a dialog. The gradient tool's live drag preview shares this
  // machinery with the seventeen modals above because it needs exactly what
  // they need -- one hypothetical `Document` composited in place of the real
  // one -- and because sharing it is what guarantees only ONE such preview
  // can be live at a time. A parallel mechanism for the canvas would let a
  // gradient drag and an open Levels dialog both claim the composite, and
  // the canvas would have to pick between them.
  //
  // It differs from all seventeen in what CLEARS it: a dialog clears on the
  // frame its popup stops being open, and this clears on the frame the tool
  // is not the gradient or the pointer is not down (see the owner-guarded
  // call beside the canvas input block).
  GradientTool
};

struct FilterPreviewState {
  FilterPreviewOwner owner = FilterPreviewOwner::None;
  DocumentId documentId = 0;
  size_t layerIndex = 0;
  // The active layer's full post-filter content -- what `previewX()` handed
  // back, already blended through the selection by `compositeFilterResult()`.
  // Bit-identical to what `applyX()` would write into `*target->rgbTiles` at
  // the SAME parameters, because both route through `computePixelFilter()`
  // (app/FilterOps.cpp) -- see that file's header on why this is the one
  // function rather than two, which is what stops the preview and the commit
  // from computing two different answers.
  TileStore tiles;
  // Bumped by every `setFilterPreview()` call, never by a frame that merely
  // redraws the same one. `FilterPreviewTexture::viewFor()` below re-uploads
  // only when this has moved past what it last uploaded, so a dialog sitting
  // open with nothing dragged costs one integer comparison, matching
  // `DocumentTexture`'s own revision-cache argument.
  uint64_t generation = 0;
};
FilterPreviewState g_filterPreview;

// Called by a dialog on a frame its own parameters actually changed -- a
// slider moved, or the dialog just opened -- never on every frame it merely
// stays open. `owner` identifies which dialog this is, so the four draw
// functions below (each of which runs every frame, whether or not ITS OWN
// popup is the one open) cannot overwrite a preview that belongs to another
// one of them.
void setFilterPreview(FilterPreviewOwner owner, DocumentId id, size_t layerIndex,
                      TileStore tiles) {
  g_filterPreview.owner = owner;
  g_filterPreview.documentId = id;
  g_filterPreview.layerIndex = layerIndex;
  g_filterPreview.tiles = std::move(tiles);
  ++g_filterPreview.generation;
}

// Only clears the preview if `owner` is the one holding it. That guard is
// load-bearing, not defensive filler: all four dialog draw functions run
// every frame regardless of which popup (if any) is actually open, and each
// one's own "my popup isn't open" branch calls this to clear up after
// itself -- see the four `drawXDialog()` bodies below. Without the owner
// check, `drawSharpenDialog()` running on a frame Gaussian Blur's popup is
// the one open (Sharpen's own popup is not open on that frame, same as
// nearly every other frame) would blank a preview it does not own, and the
// canvas would flash back to the unfiltered document mid-drag.
//
// This is also what makes Cancel correct **however the dialog closes**, not
// only via its own Cancel button: `BeginPopupModal()` returning false is
// Dear ImGui's own signal that the popup closed, and that covers the Cancel
// click, a successful confirm, AND Escape (ImGui closes the topmost popup on
// Escape by default, before this file's own Cancel button ever runs) in one
// place, so there is exactly one line per dialog that has to get "did this
// close" right rather than three.
void clearFilterPreview(FilterPreviewOwner owner) {
  if (g_filterPreview.owner != owner) return;
  g_filterPreview.owner = FilterPreviewOwner::None;
  g_filterPreview.tiles = TileStore{};
}

// The overlay's GPU texture. Same format and the same upload shape as
// `ui/DocumentTexture`'s `DocumentTexture` (RGBA16Float, straight alpha,
// packed by the identical `compositeDocumentStraightHalf()`), because it is
// drawn through the identical `addCanvasQuad()` pipeline and has to encode
// exactly the same way to look like the same picture.
//
// **Not `DocumentTexture` itself**, for `DabPreviewTexture`'s own reason
// above plus one specific to this task: `DocumentTexture` is keyed on the
// LIVE document's `(id, revision, width, height)`, and its incremental path
// diffs the current upload against a `Document` snapshot it keeps across
// frames (core/DirtyTiles.hpp). Pointing that machinery at a preview -- a
// document state that never really existed and must never be mistaken for
// one that did -- would either poison `g_documentTextures`' own cache (a
// later real frame would find a revision it believes it already drew) or
// require teaching it to tell a real document from a hypothetical one. A
// second, dedicated texture, keyed on the caller's own `generation` rather
// than a revision counter, has neither problem.
//
// **Always a full recomposite -- there is no incremental path here at all,
// and that is deliberate rather than unfinished.** The incremental path
// needs a PREVIOUS snapshot of the SAME document to diff against; a
// preview's "before" is the live document and its "after" is a hypothetical
// one that was never itself uploaded, so there is nothing to diff it
// against. See this task's own report for what that costs, measured, against
// PRD F3's 20 ms pen-to-photon budget.
class FilterPreviewTexture {
 public:
  WGPUTextureView viewFor(GpuContext& gpu, const Document& doc, uint64_t generation) {
    if (doc.width <= 0 || doc.height <= 0) return nullptr;
    const bool freshTexture =
        texture_ == nullptr || doc.width != width_ || doc.height != height_;
    if (freshTexture) {
      // Retire, don't release -- `ui/DocumentTexture.hpp`'s `retired_`
      // explains why: ImGui's WebGPU backend caches a bind group keyed by
      // the view pointer and the bind group holds a strong reference to what
      // it points at, so releasing a view whose address the allocator then
      // hands back to a new texture would leave last frame's cached bind
      // group pointing at freed memory.
      if (texture_ != nullptr) retired_.push_back(Retired{texture_, view_});
      WGPUTextureDescriptor td = {};
      td.label = sv("filter preview");
      td.dimension = WGPUTextureDimension_2D;
      td.size = {static_cast<uint32_t>(doc.width), static_cast<uint32_t>(doc.height), 1};
      td.format = WGPUTextureFormat_RGBA16Float;
      td.mipLevelCount = 1;
      td.sampleCount = 1;
      td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      texture_ = wgpuDeviceCreateTexture(gpu.device, &td);
      view_ = wgpuTextureCreateView(texture_, nullptr);
      width_ = doc.width;
      height_ = doc.height;
      uploaded_ = 0;  // a fresh texture holds nothing yet; force the upload below
    }
    if (freshTexture || generation != uploaded_) {
      const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc);
      WGPUTexelCopyTextureInfo dst = {};
      dst.texture = texture_;
      dst.mipLevel = 0;
      dst.aspect = WGPUTextureAspect_All;
      WGPUTexelCopyBufferLayout layout = {};
      layout.bytesPerRow = static_cast<uint32_t>(width_) * 4u * sizeof(uint16_t);
      layout.rowsPerImage = static_cast<uint32_t>(height_);
      const WGPUExtent3D extent = {static_cast<uint32_t>(width_),
                                   static_cast<uint32_t>(height_), 1};
      wgpuQueueWriteTexture(gpu.queue, &dst, halves.data(), halves.size() * sizeof(uint16_t),
                            &layout, &extent);
      uploaded_ = generation;
      ++uploads_;
    }
    return view_;
  }

  uint64_t uploads() const noexcept { return uploads_; }

 private:
  struct Retired {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
  };
  WGPUTexture texture_ = nullptr;
  WGPUTextureView view_ = nullptr;
  int32_t width_ = 0;
  int32_t height_ = 0;
  uint64_t uploaded_ = 0;
  uint64_t uploads_ = 0;
  std::vector<Retired> retired_;
};
FilterPreviewTexture g_filterPreviewTexture;

// The canvas draw code's one hook into all of this: `nullptr` when nothing
// has an active preview for `activeDoc`, otherwise the view to draw INSTEAD
// of `g_documentTextures`' real one.
//
// Builds a `Document` that shares every tile with `activeDoc.document`
// (`TileStoreOf`'s copy constructor is an O(tiles) refcount bump, not a byte
// copy -- app/FilterOps.cpp's own "why the original is copied" section makes
// this same argument) except the active layer's RGB tiles, which point at
// the filtered scratch buffer instead. That is T15's "draw that instead of
// the layer" made literal: a WHOLE document composite -- correct across
// every blend mode and every layer above or below the one being filtered,
// not a crop or a single-layer blit -- built from a hypothetical `Document`
// that is thrown away at the end of the frame and never touches
// `activeDoc` itself.
WGPUTextureView filterPreviewViewFor(GpuContext& gpu, const OpenDocument& activeDoc) {
  if (g_filterPreview.owner == FilterPreviewOwner::None) return nullptr;
  if (g_filterPreview.documentId != activeDoc.id) return nullptr;
  if (g_filterPreview.layerIndex >= activeDoc.document.layers.size()) return nullptr;

  Document previewDoc = activeDoc.document;
  previewDoc.layers[g_filterPreview.layerIndex].rgbTiles = g_filterPreview.tiles;
  return g_filterPreviewTexture.viewFor(gpu, previewDoc, g_filterPreview.generation);
}

// Shared by all four dialogs below, called on every frame their OWN
// parameters actually changed: runs `previewFn` (one of `previewGaussianBlur`
// / `previewSharpen` / `previewUnsharpMask` / `previewAddNoise`,
// app/FilterOps.hpp) and either shows the result or clears whatever `owner`
// was previously showing. Refusal and "nothing changed" both fall through to
// the clear, for two different reasons: a refused layer has nothing to
// preview (the confirm button's own re-check still reports why, exactly as
// it did before this task), and an identity request's preview would be
// pixel-identical to the real document anyway, so clearing it costs nothing
// visible and skips a texture upload nobody would see change.
template <typename PreviewFn, typename Params>
void updateFilterPreview(OpenDocument* od, FilterPreviewOwner owner, PreviewFn previewFn,
                         const Params& params) {
  if (od != nullptr) {
    TileStore tiles;
    const FilterOpResult r = previewFn(*od, params, &tiles);
    if (r.refusal == PixelOpRefusal::None && r.texelsChanged > 0) {
      if (const std::optional<size_t> idx = activeLayerIndex(*od)) {
        setFilterPreview(owner, od->id, *idx, std::move(tiles));
        return;
      }
    }
  }
  clearFilterPreview(owner);
}

bool g_gaussianBlurRequested = false;
bool g_sharpenRequested = false;
bool g_unsharpMaskRequested = false;
bool g_addNoiseRequested = false;
bool g_embossRequested = false;
bool g_medianRequested = false;
bool g_motionBlurRequested = false;

void drawGaussianBlurDialog(AppState& st) {
  static float sigma = 8.0f;  // texels; ops/Blur.hpp's own worked examples use this
  static std::string status;
  // Was the popup open last frame? Distinguishes "the slider itself didn't
  // move" (skip the recompute) from "the dialog just appeared, showing
  // whatever `sigma` was left at" (recompute once so the canvas has a
  // preview from the very first visible frame, not only after the first
  // drag). `BeginPopupModal()` itself can't answer that -- it only says
  // whether the popup is open THIS frame.
  static bool wasOpen = false;

  if (g_gaussianBlurRequested) {
    g_gaussianBlurRequested = false;
    status.clear();
    ImGui::OpenPopup("Gaussian Blur");
  }
  if (!ImGui::BeginPopupModal("Gaussian Blur", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    // Reached on every kind of close -- Cancel, a successful Blur, or Escape
    // -- which is exactly what makes this the one place Cancel's new job
    // (T15: discard the preview) gets done, regardless of which of the three
    // just happened. See `clearFilterPreview()`'s own comment.
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::GaussianBlur);
    return;
  }

  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Radius (sigma, texels)", &sigma, 0.0f, 250.0f, "%.1f");
  // **On release, not on every tick of the drag.** `SliderFloat()`'s own
  // return value is true on EVERY frame the value moves, and recomputing the
  // preview that often would call `previewGaussianBlur()` (and, behind it,
  // `filterPreviewViewFor()`'s full document recomposite) once per pixel of
  // mouse travel. This task's own report measures what that costs at this
  // app's own default document size, `main.cpp`'s 1024x1024 `kCanvasW`/
  // `kCanvasH` (app/selftest/FilterMenu.cpp section G): ~275 ms of engine
  // time alone at sigma 8, ~14x PRD F3's whole 20 ms pen-to-photon budget --
  // and that is the SMALL end of what a user opens. Recomputing on every
  // tick would not be a slow live preview; it would be the whole
  // application not responding to input for the length of the drag.
  //
  // `IsItemDeactivatedAfterEdit()` is Dear ImGui's own "the user just
  // finished editing this" signal -- true once, on mouse-up (or on Enter for
  // a typed value), never on the intermediate frames of a drag. Trading
  // continuous liveness for that is an honest, scoped mitigation of the
  // FREQUENCY of a too-slow recompute, not a fix for its per-call cost: the
  // one recompute this still does, on release, is exactly as slow as it was
  // before. Fitting inside F3 for real needs what this task's report names
  // and does not build -- a preview at view resolution, or on a downsampled
  // proxy, so the recompute itself gets cheap rather than merely rarer.
  const bool sigmaSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled("0 is the identity. ops/Blur.hpp's apron is ceil(4 * sigma) texels.");

  // Also recomputed on the dialog's first visible frame (`!wasOpen`), so the
  // canvas shows a preview from the moment it opens rather than only after
  // the first completed drag.
  if (sigmaSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::GaussianBlur, previewGaussianBlur, sigma);
  wasOpen = true;

  if (ImGui::Button("Blur") && od != nullptr) {
    const FilterOpResult r = applyGaussianBlur(*od, sigma);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "gaussian blur");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (radius 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawSharpenDialog(AppState& st) {
  static float strength = 1.0f;  // UnsharpParams::amount; 0 is the identity
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_sharpenRequested) {
    g_sharpenRequested = false;
    status.clear();
    ImGui::OpenPopup("Sharpen");
  }
  if (!ImGui::BeginPopupModal("Sharpen", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::Sharpen);
    return;
  }

  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Strength", &strength, 0.0f, 3.0f, "%.2f");
  // On release, not on every tick -- see drawGaussianBlurDialog()'s own
  // comment on `IsItemDeactivatedAfterEdit()` for why. Sharpen's engine call
  // is `unsharpMaskTiles()` at the fixed `kSharpenSigma` (a small radius, so
  // cheaper than a large Gaussian Blur sigma) but still a full-canvas pass at
  // every recompute, and the SAME full document recomposite behind it.
  const bool strengthSettled = ImGui::IsItemDeactivatedAfterEdit();
  // kSharpenSigma is named rather than offered as a control: ops/Filters.hpp
  // section 3 argues at length for why 1.0 is the one radius this one-click
  // filter should have, and a slider here would be the second radius control
  // the header's own "one-click filter, not an operator" distinction warns
  // against re-opening. Unsharp Mask, two rows down in this same menu, is
  // where the radius becomes a dial.
  ImGui::TextDisabled("Fixed radius (sigma %.1f) -- see Unsharp Mask for a radius control.",
                      kSharpenSigma);

  if (strengthSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::Sharpen, previewSharpen, strength);
  wasOpen = true;

  if (ImGui::Button("Sharpen") && od != nullptr) {
    const FilterOpResult r = applySharpen(*od, strength);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "sharpen");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (strength 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawUnsharpMaskDialog(AppState& st) {
  static UnsharpParams params;  // amount 1, threshold 0, blur.sigma 0 at struct default
  static float radius = 2.0f;   // params.blur.sigma; kept apart so 0 isn't the opening value
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_unsharpMaskRequested) {
    g_unsharpMaskRequested = false;
    status.clear();
    ImGui::OpenPopup("Unsharp Mask");
  }
  if (!ImGui::BeginPopupModal("Unsharp Mask", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::UnsharpMask);
    return;
  }

  OpenDocument* od = st.documents.active();

  // On release, not on every tick, for all three sliders -- see
  // drawGaussianBlurDialog()'s own comment on `IsItemDeactivatedAfterEdit()`.
  // This dialog's radius is the SAME Gaussian sigma Gaussian Blur's own
  // slider drives (`params.blur.sigma` below), so the cost measured there
  // (app/selftest/FilterMenu.cpp section G) applies here unchanged at the
  // same radius.
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Amount", &params.amount, 0.0f, 5.0f, "%.2f");
  bool paramsSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Radius (sigma, texels)", &radius, 0.1f, 250.0f, "%.1f");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Threshold", &params.threshold, 0.0f, 0.20f, "%.3f");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled(
      "Threshold is shaper-domain (ops/Filters.hpp section 2): 0.02 ignores differences "
      "smaller than 27%% of the local level, at every brightness.");

  params.blur.kind = BlurKind::Gaussian;
  params.blur.sigma = radius;

  if (paramsSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::UnsharpMask, previewUnsharpMask, params);
  wasOpen = true;

  if (ImGui::Button("Sharpen") && od != nullptr) {
    const FilterOpResult r = applyUnsharpMask(*od, params);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "unsharp mask");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (amount or radius 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawAddNoiseDialog(AppState& st) {
  static NoiseParams params;  // amount 0, Gaussian, not monochrome, seed 0 at struct default
  static int distributionIdx = 1;  // 0 = Uniform, 1 = Gaussian -- matches params' own default
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (g_addNoiseRequested) {
    g_addNoiseRequested = false;
    status.clear();
    // A fresh seed every time the dialog opens: PRD-shaped noise is
    // reproducible GIVEN a seed (ops/Filters.hpp's "whole reproducibility
    // contract"), which is a claim about re-running the SAME request, not
    // about every Add Noise ever looking identical. `--selftest` pins the
    // reproducibility half directly, by seed, rather than through this UI
    // convenience.
    params.seed = static_cast<uint64_t>(ImGui::GetTime() * 1e6) ^ 0x9e3779b97f4a7c15ull;
    ImGui::OpenPopup("Add Noise");
  }
  if (!ImGui::BeginPopupModal("Add Noise", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AddNoise);
    return;
  }

  OpenDocument* od = st.documents.active();

  // Amount is the one continuous drag here -- on release, not on every tick,
  // for the reason drawGaussianBlurDialog()'s own comment on
  // `IsItemDeactivatedAfterEdit()` gives. Uniform/Gaussian and Monochrome are
  // single-click, so `IsItemDeactivatedAfterEdit()` reports the same frame
  // their old plain "changed" read did -- used here anyway, for one shared
  // idiom across all four controls rather than a drag-only exception.
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Amount", &params.amount, 0.0f, 0.5f, "%.3f");
  bool paramsSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::RadioButton("Uniform", &distributionIdx, 0);
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SameLine();
  ImGui::RadioButton("Gaussian", &distributionIdx, 1);
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  params.distribution =
      distributionIdx == 0 ? NoiseDistribution::Uniform : NoiseDistribution::Gaussian;
  ImGui::Checkbox("Monochrome", &params.monochrome);
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled("Amount is shaper-domain (ops/Filters.hpp section 5): 0.05 is a +/-83%% "
                      "swing in linear light at every brightness above the shadow toe.");
  ImGui::Text("Seed: %llu", static_cast<unsigned long long>(params.seed));
  ImGui::SameLine();
  if (ImGui::Button("New seed")) {
    params.seed = static_cast<uint64_t>(ImGui::GetTime() * 1e6) ^ 0x9e3779b97f4a7c15ull;
    paramsSettled = true;
  }

  if (paramsSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AddNoise, previewAddNoise, params);
  wasOpen = true;

  if (ImGui::Button("Add Noise") && od != nullptr) {
    const FilterOpResult r = applyAddNoise(*od, params);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "add noise");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (amount 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Three more Filter-menu dialogs, extending ops/Filters.hpp sections 7-9
// (emboss, median/despeckle, motion blur) through the identical
// request-flag / popup / live-preview-on-release shape the four dialogs
// above already establish. Each one's `IsItemDeactivatedAfterEdit()`
// discipline and its Cancel-clears-the-preview behaviour are copied from
// `drawGaussianBlurDialog()`'s own comments rather than re-argued here.
// ---------------------------------------------------------------------------

void drawEmbossDialog(AppState& st) {
  // Angle/distance rather than raw dx/dy: `EmbossParams::dx`/`dy` are
  // document-texel integers (ops/Filters.hpp section 7's exact two-tap
  // offset), but a dial that reads "Angle" and "Distance" is what an Emboss
  // dialog actually looks like everywhere else, and the conversion belongs
  // here rather than in the engine -- ops/Filters.hpp's own params struct is
  // deliberately the unambiguous, already-integer form a UI converts INTO,
  // not a second place that re-derives what "45 degrees" means.
  static float angleDeg = 135.0f;  // upper-left light, the conventional default
  static int distance = 2;         // texels
  static float depth = 3.0f;
  static float amount = 1.0f;
  static std::string status;
  static bool wasOpen = false;

  if (g_embossRequested) {
    g_embossRequested = false;
    status.clear();
    ImGui::OpenPopup("Emboss");
  }
  if (!ImGui::BeginPopupModal("Emboss", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::Emboss);
    return;
  }

  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Angle", &angleDeg, 0.0f, 360.0f, "%.0f deg");
  bool paramsSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderInt("Distance", &distance, 0, 8, "%d texels");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Depth", &depth, -10.0f, 10.0f, "%.2f");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Amount", &amount, 0.0f, 1.0f, "%.2f");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled(
      "0 amount is the identity. Distance 0 (or Depth 0) is a flat grey card, not a no-op -- "
      "ops/Filters.hpp section 7 says why a stylize filter's neutral point is not its "
      "identity.");

  constexpr float kPi = 3.14159265f;  // IM_PI lives in imgui_internal.h; see drawPadlockGlyph()
  const float angleRad = angleDeg * (kPi / 180.0f);
  EmbossParams params;
  params.dx = static_cast<int32_t>(std::lround(static_cast<double>(distance) *
                                               std::cos(static_cast<double>(angleRad))));
  params.dy = static_cast<int32_t>(std::lround(static_cast<double>(distance) *
                                               std::sin(static_cast<double>(angleRad))));
  params.depth = depth;
  params.amount = amount;

  if (paramsSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::Emboss, previewEmboss, params);
  wasOpen = true;

  if (ImGui::Button("Emboss") && od != nullptr) {
    const FilterOpResult r = applyEmboss(*od, params);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "emboss");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (amount 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawMedianDialog(AppState& st) {
  static int radius = 1;  // texels; ops/Filters.hpp section 8's despeckle
  static std::string status;
  static bool wasOpen = false;

  if (g_medianRequested) {
    g_medianRequested = false;
    status.clear();
    ImGui::OpenPopup("Median");
  }
  if (!ImGui::BeginPopupModal("Median", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::Median);
    return;
  }

  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderInt("Radius", &radius, 0, 8, "%d texels");
  const bool radiusSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled(
      "0 is the identity. Window is (2*radius+1)^2 texels -- radius 8 is a 17x17 window, "
      "the expensive end of this dial (see this task's own [measured] median timing line).");

  const MedianParams params{radius};

  if (radiusSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::Median, previewMedian, params);
  wasOpen = true;

  if (ImGui::Button("Despeckle") && od != nullptr) {
    const FilterOpResult r = applyMedian(*od, params);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "median");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (radius 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawMotionBlurDialog(AppState& st) {
  static float angleDeg = 0.0f;
  static int distance = 8;  // texels; ops/Filters.hpp section 9's radius
  static std::string status;
  static bool wasOpen = false;

  if (g_motionBlurRequested) {
    g_motionBlurRequested = false;
    status.clear();
    ImGui::OpenPopup("Motion Blur");
  }
  if (!ImGui::BeginPopupModal("Motion Blur", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::MotionBlur);
    return;
  }

  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Angle", &angleDeg, 0.0f, 180.0f, "%.0f deg");
  bool paramsSettled = ImGui::IsItemDeactivatedAfterEdit();
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderInt("Distance", &distance, 0, 60, "%d texels");
  paramsSettled |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::TextDisabled(
      "0 distance is the identity. The kernel is symmetric about the texel, so an angle and "
      "angle+180 are the same request -- ops/Filters.hpp section 9 says so.");

  constexpr float kPi = 3.14159265f;  // IM_PI lives in imgui_internal.h; see drawPadlockGlyph()
  MotionBlurParams params;
  params.angleRadians = angleDeg * (kPi / 180.0f);
  params.radius = distance;

  if (paramsSettled || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::MotionBlur, previewMotionBlur, params);
  wasOpen = true;

  if (ImGui::Button("Motion Blur") && od != nullptr) {
    const FilterOpResult r = applyMotionBlur(*od, params);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), "motion blur");
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (distance 0, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

// ===========================================================================
// Image > Adjustments (app/AdjustmentOps.hpp)
// ===========================================================================
//
// Four modals and one immediate command, on the same request-flag / popup /
// preview-on-release shape the Filter dialogs above establish -- their
// `IsItemDeactivatedAfterEdit()` discipline and their Cancel-clears-the-
// preview behaviour are `drawGaussianBlurDialog()`'s comments and are not
// re-argued here.
//
// **The one structural difference from the seven above, and the reason for
// it.** These read `st.requestAdjustment` (app/AppState.hpp) instead of a
// file-static `g_xRequested`, because three of them have a keyboard chord and
// a chord is resolved in `main.cpp`, which cannot reach a static in this
// translation unit. `serviceAdjustmentRequest()` is the single place that
// consumes the field, so the menu path and the chord path converge before
// either one opens anything.
//
// **Why the preview is not throttled harder than the filters', and why it is
// still throttled at all.** A point op is dramatically cheaper than a spatial
// one -- no apron, no gather, and `ops/PointOpTiles` visits only tiles the
// layer already has, so an adjustment on a small sketch in a large canvas
// touches a handful of tiles where a blur touches every one. But the recompute
// still walks every occupied texel through a `std::function` per op and then
// through `filterPreviewViewFor()`'s whole-document recomposite, and that
// second half is the same cost it is for a blur. Recomputing on every frame of
// a slider drag would therefore still be a full document composite per pixel
// of mouse travel. So: same `IsItemDeactivatedAfterEdit()` throttle, for the
// second half of the cost rather than the first.

// The channel selector shared by the Levels and Curves dialogs -- Photoshop's
// own arrangement, one channel edited at a time behind a combo, rather than
// fifteen sliders at once.
//
// **Index 0 is "RGB", and editing it writes all three channels**, which is
// what a composite adjustment IS in `ops/PointOps.hpp`'s vocabulary: "a
// *composite* levels adjustment is just the caller passing the same
// LevelsParams for all three channels, not a separate code path". That is the
// engine's rule, honoured here rather than reinvented -- there is no fourth
// composite slot in the params, and this combo is the only place the idea
// exists.
//
// The visible consequence, stated because a user will meet it: editing a
// single channel and then editing RGB overwrites what that channel held. That
// is Photoshop's behaviour too, and the alternative -- compositing a fourth
// curve on top of three -- would mean the dialog and the engine disagreed
// about what the layer is going to look like.
constexpr const char* kAdjustChannelLabels[] = {"RGB", "Red", "Green", "Blue"};

bool drawAdjustChannelCombo(int* channelIdx) {
  ImGui::SetNextItemWidth(120.0f);
  return ImGui::Combo("Channel", channelIdx, kAdjustChannelLabels,
                      IM_ARRAYSIZE(kAdjustChannelLabels));
}

// The Levels dialog's histogram: the bins `drawHistogramSection()` already
// draws, with the current channel's black point / white point / gamma
// overlaid as three draggable handles, so a black/white point can be set by
// looking at where the data actually is rather than by number alone.
//
// **A domain crossing sits at the middle of this widget, and getting it wrong
// silently mis-sets every drag.** The histogram's bins are `srgbEncode()`d --
// display/perceptual, `core/Histogram.cpp`'s own choice -- but `blackIn` /
// `whiteIn` are LINEAR (`ops/PointOps.hpp`'s own header: "scene-linear HDR
// headroom above 1.0" is a legal `whiteIn`, which only makes sense for a
// linear quantity). So a handle's SCREEN position is always
// `srgbEncode(linearValue)` and a drag always writes back
// `srgbDecode(screenPosition)` -- never the raw fraction across the plot.
// Getting this backwards would not crash or look obviously wrong, only
// silently set the wrong linear value for wherever the user dragged to, which
// is worse than a crash.
//
// The white handle's drag range is clamped to the plot itself (linear-domain
// values up to `srgbDecode(1.0)`, i.e. display white): scene-linear headroom
// above that is real and the numeric "White in" slider still reaches it (up
// to 4.0, unrestricted), but a histogram plotted over [0,1] display has no
// picture to drag a handle past its own right edge onto.
bool drawLevelsHistogramWidget(const HistogramResult& hist, int channelIdx, LevelsParams& shown) {
  if (hist.sampleCount == 0 || hist.r.empty()) {
    ImGui::TextDisabled("Nothing painted yet.");
    return false;
  }

  const std::vector<uint64_t>* channelBins = nullptr;
  switch (channelIdx) {
    case 1: channelBins = &hist.r; break;
    case 2: channelBins = &hist.g; break;
    case 3: channelBins = &hist.b; break;
    default: break;  // RGB composite: all four, overlaid, below.
  }

  uint64_t maxCount = 1;
  for (size_t i = 0; i < hist.r.size(); ++i)
    maxCount = std::max({maxCount, hist.r[i], hist.g[i], hist.b[i], hist.luma[i]});

  constexpr float kPlotH = 100.0f;
  constexpr float kHandleH = 14.0f;  // The triangle strip below the plot.
  const float plotW = 220.0f;        // Matches every slider's `SetNextItemWidth` in this dialog.
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(origin, ImVec2(origin.x + plotW, origin.y + kPlotH), IM_COL32(20, 20, 22, 255));

  auto plotChannel = [&](const std::vector<uint64_t>& bins, ImU32 col) {
    const size_t n = bins.size();
    for (size_t i = 0; i < n; ++i) {
      const float x0 = origin.x + plotW * (static_cast<float>(i) / static_cast<float>(n));
      const float x1 = origin.x + plotW * (static_cast<float>(i + 1) / static_cast<float>(n));
      const float h = kPlotH * (static_cast<float>(bins[i]) / static_cast<float>(maxCount));
      dl->AddRectFilled(ImVec2(x0, origin.y + kPlotH - h),
                        ImVec2(std::max(x1, x0 + 1.0f), origin.y + kPlotH), col);
    }
  };
  if (channelBins != nullptr) {
    plotChannel(*channelBins, IM_COL32(225, 225, 220, 200));
  } else {
    // Same stacked-alpha overlay drawHistogramSection() uses, so a reader who
    // has seen that panel reads this one the same way.
    plotChannel(hist.r, IM_COL32(235, 70, 70, 110));
    plotChannel(hist.g, IM_COL32(70, 215, 110, 110));
    plotChannel(hist.b, IM_COL32(80, 150, 235, 110));
    plotChannel(hist.luma, IM_COL32(235, 235, 230, 150));
  }
  dl->AddRect(origin, ImVec2(origin.x + plotW, origin.y + kPlotH),
              ImGui::GetColorU32(ImGuiCol_Border));

  // Display-domain (0..1) positions of the three handles. `whiteIn` clamped
  // for DRAWING only -- see the header comment on why the stored value is
  // never clamped here.
  const float blackDisp = std::clamp(srgbEncode(shown.blackIn), 0.0f, 1.0f);
  const float whiteDisp = std::clamp(srgbEncode(shown.whiteIn), 0.0f, 1.0f);
  // Photoshop's own convention for where the midtone (gamma) handle sits: the
  // input fraction across [black, white] that maps to 50% output, inverting
  // `t = pow(midFrac, 1/gamma)` at `t = 0.5`. Display-only, like the other two
  // -- gamma itself has no linear/display distinction to get wrong.
  const float midFrac = std::pow(0.5f, 1.0f / std::clamp(shown.gamma, 0.1f, 4.0f));
  const float gammaDisp = blackDisp + (whiteDisp - blackDisp) * midFrac;

  // The true, unclamped position (used for hit-testing/dragging below) vs. the
  // position actually drawn (nudged inward so a handle at disp=0 or disp=1 --
  // the common default state -- draws its full triangle rather than half of it
  // hanging past the plot's own border).
  auto handleX = [&](float disp) { return origin.x + plotW * disp; };
  constexpr float kHandleHalfW = 5.0f;
  auto drawHandle = [&](float disp, ImU32 col) {
    const float x = std::clamp(handleX(disp), origin.x + kHandleHalfW, origin.x + plotW - kHandleHalfW);
    const float y0 = origin.y + kPlotH;
    dl->AddTriangleFilled(ImVec2(x - kHandleHalfW, y0 + kHandleH), ImVec2(x + kHandleHalfW, y0 + kHandleH),
                          ImVec2(x, y0 + 2.0f), col);
  };
  drawHandle(blackDisp, IM_COL32(40, 40, 40, 255));
  drawHandle(gammaDisp, IM_COL32(170, 170, 170, 255));
  drawHandle(whiteDisp, IM_COL32(240, 240, 240, 255));

  ImGui::Dummy(ImVec2(plotW, kPlotH + kHandleH));

  // One combined hit-test/drag strip spanning the plot plus the handle row --
  // simpler than three separate InvisibleButtons, and there is no case where
  // two handles need independent simultaneous drags (one mouse). Nearest
  // handle wins a click, same tie-break shape as `hitTestPoint()`
  // (app/CurveEdit.cpp) uses for curve knots.
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##levelshist", ImVec2(plotW, kPlotH + kHandleH));
  bool edited = false;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  const ImGuiID dragKey = ImGui::GetID("levelsHistDrag");
  int dragHandle = storage->GetInt(dragKey, -1);  // 0 black, 1 gamma, 2 white
  if (ImGui::IsItemActivated()) {
    const float mx = ImGui::GetIO().MousePos.x - origin.x;
    const float dBlack = std::fabs(mx - handleX(blackDisp));
    const float dGamma = std::fabs(mx - handleX(gammaDisp));
    const float dWhite = std::fabs(mx - handleX(whiteDisp));
    const float best = std::min({dBlack, dGamma, dWhite});
    dragHandle = (best == dBlack) ? 0 : (best == dGamma ? 1 : 2);
    storage->SetInt(dragKey, dragHandle);
  }
  if (dragHandle >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const float disp = std::clamp((ImGui::GetIO().MousePos.x - origin.x) / plotW, 0.0f, 1.0f);
    if (dragHandle == 0) {
      shown.blackIn = srgbDecode(std::min(disp, whiteDisp));
      edited = true;
    } else if (dragHandle == 2) {
      shown.whiteIn = srgbDecode(std::max(disp, blackDisp));
      edited = true;
    } else {
      // Solve `pow(t, 1/gamma) = 0.5` for `gamma` at the dragged position's
      // fraction across [black, white] -- the inverse of `midFrac` above.
      // Clamped strictly inside (0,1): at either end the equation has no
      // finite solution (gamma -> 0 or -> infinity), and clamped to the same
      // [0.1, 4.0] the numeric slider allows, so a drag cannot produce a
      // gamma the slider itself would reject.
      const float span = std::max(whiteDisp - blackDisp, 1e-4f);
      const float t = std::clamp((disp - blackDisp) / span, 0.02f, 0.98f);
      shown.gamma = std::clamp(std::log(0.5f) / std::log(t), 0.1f, 4.0f);
      edited = true;
    }
  } else if (dragHandle >= 0) {
    dragHandle = -1;
    storage->SetInt(dragKey, dragHandle);
  }
  return edited;
}

// Shared tail of all four dialogs: the commit button, Cancel, and the refusal
// line. `applyFn` returns the `FilterOpResult` its `applyX()` produced.
//
// Written once rather than four times because the three-way outcome -- refused
// / changed nothing / done -- is the part most likely to drift if copied, and
// because "nothing changed" MUST close the popup rather than sit there looking
// broken (a neutral params struct is a legitimate thing to click OK on).
template <typename ApplyFn>
void drawAdjustmentButtons(OpenDocument* od, const char* verb, const char* label,
                           std::string& status, ApplyFn applyFn) {
  if (ImGui::Button(verb) && od != nullptr) {
    const FilterOpResult r = applyFn(*od);
    if (r.refusal != PixelOpRefusal::None) {
      status = pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), label);
    } else if (r.texelsChanged == 0) {
      status = "Nothing changed (neutral settings, or no selected texels).";
      ImGui::CloseCurrentPopup();
    } else {
      status.clear();
      ImGui::CloseCurrentPopup();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
}

void drawLevelsDialog(AppState& st) {
  static std::array<LevelsParams, 3> channels{};
  static int channelIdx = 0;
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment
  // Computed once, on the frame the dialog opens, not every frame it is held
  // open: `adjustmentHistogramFor()` walks the active layer's tiles, and
  // nothing else can repaint them while a modal Adjustments dialog owns the
  // frame, so a fresh read every frame would cost real time for a number that
  // cannot have changed since the last one.
  static HistogramResult histogram;

  if (!ImGui::BeginPopupModal("Levels", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustLevels);
    return;
  }
  OpenDocument* od = st.documents.active();
  if (!wasOpen) histogram = (od != nullptr) ? adjustmentHistogramFor(*od) : HistogramResult{};

  bool edited = drawAdjustChannelCombo(&channelIdx);
  // The values shown are channel 0's whenever RGB is selected -- see
  // `kAdjustChannelLabels`' comment. A composite edit has just written the
  // same numbers to all three, so channel 0 is a faithful reading of what the
  // composite holds, not an arbitrary pick among three.
  LevelsParams shown = channels[channelIdx == 0 ? 0 : channelIdx - 1];

  edited |= drawLevelsHistogramWidget(histogram, channelIdx, shown);

  ImGui::SeparatorText("Input");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Black in", &shown.blackIn, 0.0f, 1.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("White in", &shown.whiteIn, 0.0f, 4.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Gamma", &shown.gamma, 0.1f, 4.0f, "%.3f");

  ImGui::SeparatorText("Output");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Black out", &shown.blackOut, 0.0f, 1.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("White out", &shown.whiteOut, 0.0f, 1.0f, "%.3f");

  // `whiteIn` runs past 1.0 deliberately: this is a scene-linear working space
  // and a highlight above 1.0 is ordinary, so a Levels white point capped at
  // 1.0 could not reach it. ops/PointOps.hpp's own note on the `t` clamp
  // explains what happens above `whiteIn` -- it saturates, which is correct
  // black/white-point behaviour and not the module's no-clamp policy being
  // broken.
  ImGui::TextDisabled("White in above 1.0 reaches scene-linear highlights.");

  if (edited) {
    if (channelIdx == 0) {
      channels[0] = shown;
      channels[1] = shown;
      channels[2] = shown;
    } else {
      channels[static_cast<size_t>(channelIdx - 1)] = shown;
    }
  }
  // Live: `edited` is a slider's own return value, which SliderFloat already
  // reports true on every frame the drag actually moves the value, not only
  // once on release -- so recomputing on `edited` directly is the live preview
  // "for non-expensive image adjustments... hsv curves levels etc" asked for.
  // Levels is a point op (ops/PointOpTiles): no apron, no tile the source did
  // not already have, cheap enough to afford this. The combo is not a slider
  // and never reports edited either way, but IS covered by `edited` above (it
  // is OR'd into the same variable via `drawAdjustChannelCombo`'s return).
  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustLevels, previewLevelsAdjustment, channels);
  wasOpen = true;

  drawAdjustmentButtons(od, "Levels", "levels", status,
                        [](OpenDocument& d) { return applyLevelsAdjustment(d, channels); });
  ImGui::EndPopup();
}

void drawCurvesDialog(AppState& st) {
  static std::array<Curve, 3> channels{};
  static int channelIdx = 0;
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (!ImGui::BeginPopupModal("Curves", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustCurves);
    return;
  }
  OpenDocument* od = st.documents.active();

  const bool switched = drawAdjustChannelCombo(&channelIdx);

  // **The same `drawCurveWidget()` the GRADE panel's op-stack editor draws,
  // at the same default size.** Not a second curve editor: that function is
  // already shared with the BRUSH SETTINGS panel's LINK editor precisely so a
  // third caller costs nothing, and its plotted spline is sampled through the
  // very `evalCurve()` the engine will run -- so what this dialog draws cannot
  // diverge from what the adjustment computes.
  //
  // Axes are shaper-domain (ADR-0004), which is what the control points ARE by
  // contract; nothing here converts, and `applyCurves()` does the
  // encode/decode round trip per channel.
  Curve& editing = channels[channelIdx == 0 ? 0 : static_cast<size_t>(channelIdx - 1)];
  ImGui::PushID(channelIdx);
  bool edited = drawCurveWidget(editing);
  ImGui::PopID();
  if (edited && channelIdx == 0) {
    channels[1] = channels[0];
    channels[2] = channels[0];
  }
  ImGui::TextDisabled("Click to add a point, drag to move. Axes are the shaper domain.");
  if (ImGui::Button("Reset channel")) {
    editing.clear();
    if (channelIdx == 0) {
      channels[1].clear();
      channels[2].clear();
    }
    edited = true;
  }

  // Live, same as Levels: `applyCurvesAdjustment`/`previewCurvesAdjustment` run
  // through the identical `pointOpTiles` bridge Levels does (app/AdjustmentOps),
  // so this is exactly as cheap, and `drawCurveWidget()` already reports
  // "changed" on every frame of a drag -- which used to be the reason this
  // recomputed only on release (throttled via `IsMouseDown()`), and is now
  // exactly the live-update signal the recompute wants.
  if (edited || switched || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustCurves, previewCurvesAdjustment, channels);
  wasOpen = true;

  drawAdjustmentButtons(od, "Curves", "curves", status,
                        [](OpenDocument& d) { return applyCurvesAdjustment(d, channels); });
  ImGui::EndPopup();
}

void drawExposureDialog(AppState& st) {
  static ExposureParams params{};
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (!ImGui::BeginPopupModal("Exposure", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustExposure);
    return;
  }
  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(220.0f);
  // Live: a point op (app/AdjustmentOps), cheap enough to recompute on every
  // frame `SliderFloat` reports the value actually changed, not only on
  // release -- see drawLevelsDialog()'s comment for the full argument.
  const bool edited = ImGui::SliderFloat("Stops", &params.stops, -6.0f, 6.0f, "%+.2f");
  // Stops, not a percentage, and a pure multiply in linear light -- which is
  // why this is the one adjustment here that is physically meaningful rather
  // than perceptual, and why it is NOT authored in the shaper domain the way
  // Curves is. ops/PointOps.hpp section 3 is the authority.
  ImGui::TextDisabled("output = input * 2^stops, in linear light. 0 is the identity.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustExposure, previewExposureAdjustment, params);
  wasOpen = true;

  drawAdjustmentButtons(od, "Exposure", "exposure", status,
                        [](OpenDocument& d) { return applyExposureAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawChannelMixerDialog(AppState& st) {
  static ChannelMixerParams params{};
  static int outputIdx = 0;
  static std::string status;
  static bool wasOpen = false;  // see drawGaussianBlurDialog()'s own comment

  if (!ImGui::BeginPopupModal("Channel Mixer", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustChannelMixer);
    return;
  }
  OpenDocument* od = st.documents.active();

  // One OUTPUT channel at a time, and no composite entry -- deliberately
  // unlike Levels and Curves above. A channel mixer's matrix row IS the output
  // channel's definition (`output[i] = m[i][0]*r + m[i][1]*g + m[i][2]*b +
  // m[i][3]`), so "all three at once" would mean writing the same row three
  // times, which is a greyscale conversion, not a composite mix -- and this
  // application already has `Desaturate` for that.
  static const char* kOutputs[] = {"Red", "Green", "Blue"};
  ImGui::SetNextItemWidth(120.0f);
  // Live: see drawLevelsDialog()'s comment -- a point op, so recomputing on
  // every frame a control's value actually changes (not only on release) is
  // cheap enough to afford.
  bool edited = false;
  const bool switched = ImGui::Combo("Output channel", &outputIdx, kOutputs, IM_ARRAYSIZE(kOutputs));
  auto& row = params.matrix[static_cast<size_t>(outputIdx)];

  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Red source", &row[0], -2.0f, 2.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Green source", &row[1], -2.0f, 2.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Blue source", &row[2], -2.0f, 2.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Constant", &row[3], -1.0f, 1.0f, "%.3f");

  // Photoshop shows a "Total" here because a row summing past 100% brightens
  // that channel; the same is true in linear light, so the number is worth
  // showing even though this build does not constrain it.
  ImGui::TextDisabled("Source total: %+.3f (1.000 preserves this channel's level).",
                      static_cast<double>(row[0] + row[1] + row[2]));

  if (edited || switched || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustChannelMixer,
                        previewChannelMixerAdjustment, params);
  wasOpen = true;

  drawAdjustmentButtons(od, "Mix", "channel mixer", status,
                        [](OpenDocument& d) { return applyChannelMixerAdjustment(d, params); });
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// The nine dialogs for the ops that needed new arithmetic
// ---------------------------------------------------------------------------
//
// Every one has the identical skeleton -- BeginPopupModal, clear the preview
// on any close, controls, recompute on `settled || !wasOpen`, then
// `drawAdjustmentButtons()` -- so what is worth reading in each is only the
// controls and the one line of explanation under them. The skeleton itself is
// argued once, in drawGaussianBlurDialog() and in this section's own header.

void drawBrightnessContrastDialog(AppState& st) {
  static GainOffsetGammaParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Brightness/Contrast", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustBrightnessContrast);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Gain, offset and gamma rather than two sliders labelled "brightness" and
  // "contrast", because those two names do not name an operation --
  // docs/operations.md §1.2 calls this trio "the honest form of
  // brightness/contrast" and ops/ToneOps.hpp derives the operand order from
  // ASC-CDL. Gain reads as contrast, offset as brightness, and the labels say
  // both so a painter looking for the familiar control finds it.
  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Gain (contrast)", &params.gain, 0.0f, 4.0f, "%.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Offset (brightness)", &params.offset, -1.0f, 1.0f, "%+.3f");
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Gamma", &params.gamma, 0.1f, 4.0f, "%.3f");
  ImGui::TextDisabled("output = pow(input * gain + offset, 1/gamma), in linear light.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustBrightnessContrast,
                        previewBrightnessContrast, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "brightness/contrast", status,
                        [](OpenDocument& d) { return applyBrightnessContrast(d, params); });
  ImGui::EndPopup();
}

void drawHueSaturationDialog(AppState& st) {
  static HueSaturationParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Hue/Saturation", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustHueSaturation);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  edited |= ImGui::Checkbox("Colorize", &params.colorize);
  if (params.colorize) {
    // Colorize replaces every pixel's hue with one target, keeping each
    // pixel's own luma -- so the ordinary hue/saturation controls below would
    // have nothing to act on, and showing them live but inert is worse than
    // not showing them.
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::SliderFloat("Target hue", &params.colorizeHueDegrees, -180.0f, 180.0f, "%.1f deg");
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::SliderFloat("Target saturation", &params.colorizeSaturation, 0.0f, 2.0f, "%.3f");
  } else {
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::SliderFloat("Hue", &params.hueDegrees, -180.0f, 180.0f, "%.1f deg");
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::SliderFloat("Saturation", &params.saturation, 0.0f, 3.0f, "%.3f");
  }
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Lightness", &params.lightness, -1.0f, 1.0f, "%+.3f");
  // Worth saying out loud, because it is the property that makes this op
  // usable at all: the hue rotation is about the normalised Rec.709 luma
  // axis, so it moves colour without moving brightness. Lightness is the
  // separate, deliberate control for that.
  ImGui::TextDisabled("Hue rotates about the luma axis, so brightness is preserved exactly.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustHueSaturation,
                        previewHueSaturationAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "hue/saturation", status,
                        [](OpenDocument& d) { return applyHueSaturationAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawVibranceDialog(AppState& st) {
  static VibranceParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Vibrance", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustVibrance);
    return;
  }
  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(220.0f);
  // Live: see drawLevelsDialog()'s comment -- a point op.
  const bool edited = ImGui::SliderFloat("Amount", &params.amount, -1.0f, 2.0f, "%+.3f");
  ImGui::TextDisabled("Weighted by existing saturation: muted colours move most.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustVibrance, previewVibranceAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "vibrance", status,
                        [](OpenDocument& d) { return applyVibranceAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawColorBalanceDialog(AppState& st) {
  static ColorBalanceParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Colour Balance", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustColorBalance);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Three tonal ranges, each a cyan-red / magenta-green / yellow-blue triple.
  // Photoshop draws these as three named sliders per range rather than "R G B"
  // because the axis a painter thinks in is the opposed pair, and the two
  // labellings are the same number: pushing "red" IS pulling "cyan".
  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  auto rangeSliders = [&edited](const char* title, std::array<float, 3>& v, float lo, float hi) {
    ImGui::SeparatorText(title);
    ImGui::PushID(title);
    const char* kLabels[] = {"Cyan / Red", "Magenta / Green", "Yellow / Blue"};
    for (int c = 0; c < 3; ++c) {
      ImGui::SetNextItemWidth(220.0f);
      edited |= ImGui::SliderFloat(kLabels[c], &v[static_cast<size_t>(c)], lo, hi, "%+.3f");
    }
    ImGui::PopID();
  };
  rangeSliders("Shadows (lift)", params.shadowsLift, -0.5f, 0.5f);
  rangeSliders("Midtones (gamma)", params.midtonesGamma, -1.0f, 1.0f);
  rangeSliders("Highlights (gain)", params.highlightsGain, -0.5f, 0.5f);
  edited |= ImGui::Checkbox("Preserve luminosity", &params.preserveLuminosity);

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustColorBalance,
                        previewColorBalanceAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "colour balance", status,
                        [](OpenDocument& d) { return applyColorBalanceAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawBlackAndWhiteDialog(AppState& st) {
  static BlackAndWhiteParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Black & White", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustBlackAndWhite);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  float* const weights[] = {&params.reds,  &params.yellows, &params.greens,
                            &params.cyans, &params.blues,   &params.magentas};
  const char* labels[] = {"Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
  for (int i = 0; i < 6; ++i) {
    ImGui::SetNextItemWidth(220.0f);
    edited |= ImGui::SliderFloat(labels[i], weights[i], -1.0f, 2.0f, "%.3f");
  }
  if (ImGui::Button("Reset to Rec.709")) {
    params = BlackAndWhiteParams{};
    edited = true;
  }
  ImGui::SameLine();
  // Not a slogan -- a measured property, and measured is the operative word.
  // `--selftest` runs both commands over a real layer and reports the worst
  // channel difference: one f16 storage step, which is the tile format's
  // rounding and not the arithmetic (ops/MonoOps.hpp derives why the two
  // agree algebraically). "Matches" rather than "equals" because the earlier
  // wording promised bit-equality, which is not true and which a user could
  // in principle catch us on.
  ImGui::TextDisabled("the defaults match Desaturate");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustBlackAndWhite,
                        previewBlackAndWhiteAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "black & white", status,
                        [](OpenDocument& d) { return applyBlackAndWhiteAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawPhotoFilterDialog(AppState& st) {
  static PhotoFilterParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Photo Filter", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustPhotoFilter);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::ColorEdit3("Filter colour", params.color.data(), ImGuiColorEditFlags_Float);
  // The two presets every photo-filter control ships with. Values are the
  // conventional warming/cooling gel colours, in linear light.
  if (ImGui::Button("Warming (85)")) {
    params.color = {1.0f, 0.72f, 0.42f};
    edited = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Cooling (80)")) {
    params.color = {0.40f, 0.68f, 1.0f};
    edited = true;
  }
  ImGui::SetNextItemWidth(220.0f);
  edited |= ImGui::SliderFloat("Density", &params.density, 0.0f, 1.0f, "%.3f");
  edited |= ImGui::Checkbox("Preserve luminosity", &params.preserveLuminosity);
  ImGui::TextDisabled("Density blends toward the filtered colour; 0 is the identity.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustPhotoFilter,
                        previewPhotoFilterAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "photo filter", status,
                        [](OpenDocument& d) { return applyPhotoFilterAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawPosterizeDialog(AppState& st) {
  // 4 rather than PosterizeParams' own default of 0. Zero is the op's declared
  // "off" value (ops/ToneOps.hpp), which is the right default for a params
  // struct and the wrong one for a dialog: a Posterize that opens showing "0
  // levels" and previewing nothing looks broken. 4 is a visible, conventional
  // starting point.
  static PosterizeParams params{4};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Posterize", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustPosterize);
    return;
  }
  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(220.0f);
  // Live: see drawLevelsDialog()'s comment -- a point op.
  const bool edited = ImGui::SliderInt("Levels", &params.levels, 2, 32);
  ImGui::TextDisabled("Quantised in the shaper domain, so the bands are perceptually even.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustPosterize, previewPosterizeAdjustment,
                        params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "posterize", status,
                        [](OpenDocument& d) { return applyPosterizeAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawThresholdDialog(AppState& st) {
  static ThresholdParams params{};
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Threshold", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustThreshold);
    return;
  }
  OpenDocument* od = st.documents.active();

  ImGui::SetNextItemWidth(220.0f);
  // Live: see drawLevelsDialog()'s comment -- a point op.
  const bool edited = ImGui::SliderFloat("Level", &params.threshold, 0.0f, 1.0f, "%.3f");
  ImGui::TextDisabled("Rec.709 luma, compared in the shaper domain. At or above is white.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustThreshold, previewThresholdAdjustment,
                        params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "threshold", status,
                        [](OpenDocument& d) { return applyThresholdAdjustment(d, params); });
  ImGui::EndPopup();
}

void drawGradientMapDialog(AppState& st) {
  // A black -> white ramp, planted on first use. `GradientMapParams{}` is
  // deliberately EMPTY (ops/MonoOps.hpp: no colour stops means passthrough, so
  // the default is an exact identity), which is right for the op and useless
  // for a dialog -- an empty gradient map previews nothing and looks broken.
  // The dialog is the layer that knows a person is looking at it.
  static GradientMapParams params = [] {
    GradientMapParams p;
    p.stops.colorStops.push_back(ColorStop{0.0f, {0.0f, 0.0f, 0.0f}});
    p.stops.colorStops.push_back(ColorStop{1.0f, {1.0f, 1.0f, 1.0f}});
    return p;
  }();
  static std::string status;
  static bool wasOpen = false;

  if (!ImGui::BeginPopupModal("Gradient Map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    wasOpen = false;
    clearFilterPreview(FilterPreviewOwner::AdjustGradientMap);
    return;
  }
  OpenDocument* od = st.documents.active();

  // Live: see drawLevelsDialog()'s comment -- a point op.
  bool edited = false;
  for (size_t i = 0; i < params.stops.colorStops.size(); ++i) {
    ImGui::PushID(static_cast<int>(i));
    ImGui::SetNextItemWidth(90.0f);
    edited |= ImGui::SliderFloat("##pos", &params.stops.colorStops[i].position, 0.0f, 1.0f, "%.2f");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    edited |= ImGui::ColorEdit3("##col", params.stops.colorStops[i].color.data(),
                                ImGuiColorEditFlags_Float);
    if (params.stops.colorStops.size() > 2) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        params.stops.colorStops.erase(params.stops.colorStops.begin() +
                                      static_cast<ptrdiff_t>(i));
        edited = true;
        ImGui::PopID();
        break;
      }
    }
    ImGui::PopID();
  }
  if (ImGui::Button("Add stop")) {
    params.stops.colorStops.push_back(ColorStop{0.5f, {0.5f, 0.5f, 0.5f}});
    edited = true;
  }
  // `gradientColorAt()` requires stops sorted ascending by position -- its own
  // contract, the same one ops/Gradient's other callers honour. The sliders
  // let a stop be dragged past its neighbour, so the sort happens here on
  // every edit -- now every LIVE edit, not just on release, or a stop dragged
  // past a neighbour would preview against an unsorted list for the rest of
  // that drag, which is exactly the ordering `gradientColorAt()` assumes it
  // never has to handle.
  if (edited) sortGradientStops(params.stops);
  ImGui::TextDisabled("Rec.709 luma indexes the ramp. Outside the stops, the ends extend flat.");

  if (edited || !wasOpen)
    updateFilterPreview(od, FilterPreviewOwner::AdjustGradientMap,
                        previewGradientMapAdjustment, params);
  wasOpen = true;
  drawAdjustmentButtons(od, "Apply", "gradient map", status,
                        [](OpenDocument& d) { return applyGradientMapAdjustment(d, params); });
  ImGui::EndPopup();
}

// The five commands with no dialog: Invert and the four solvers. Each runs
// through the same app/AdjustmentOps entry point the menu would call, so each
// records one history entry and refuses on the same layers for the same
// reasons. A refusal goes to stderr, since there is no modal to put it in --
// named as a real gap rather than a design: a painter who invokes Auto Tone
// on a Pigment layer sees nothing happen and gets no account of why. The fix
// is a transient status line in the chrome, which does not exist yet.
template <typename ApplyFn>
void performImmediateAdjustment(AppState& st, const char* label, ApplyFn applyFn) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) return;
  const FilterOpResult r = applyFn(*od);
  if (r.refusal != PixelOpRefusal::None) {
    std::fprintf(stderr, "[%s] %s\n", label,
                 pixelOpRefusalMessage(r.refusal, activeLayerOf(*od), label).c_str());
  }
}

// The single consumer of `AppState::requestAdjustment`. Runs BEFORE the four
// modals below, so a request made this frame opens its popup on this frame
// rather than the next one.
//
// **Desaturate is performed here, not in a dialog**, because it has no
// parameters -- see `MenuEffect`'s `AdjustDesaturate` comment in
// ui/MenuModel.cpp. It still goes through the same `applyDesaturate()` the
// menu would call, so it records the same one history entry and refuses on the
// same layers for the same reasons; its refusal message goes to stderr rather
// than into a modal, since there is no modal to put it in.
void serviceAdjustmentRequest(AppState& st) {
  const AdjustmentRequest req = st.requestAdjustment;
  if (req == AdjustmentRequest::None) return;
  st.requestAdjustment = AdjustmentRequest::None;

  switch (req) {
    // The ten with a dialog. The popup name here must match the string the
    // matching `drawXDialog()` passes to `BeginPopupModal()` exactly -- ImGui
    // identifies popups by that string, so a typo is a menu item that opens
    // nothing at all and reports no error.
    case AdjustmentRequest::Levels:             ImGui::OpenPopup("Levels"); break;
    case AdjustmentRequest::Curves:             ImGui::OpenPopup("Curves"); break;
    case AdjustmentRequest::Exposure:           ImGui::OpenPopup("Exposure"); break;
    case AdjustmentRequest::ChannelMixer:       ImGui::OpenPopup("Channel Mixer"); break;
    case AdjustmentRequest::BrightnessContrast: ImGui::OpenPopup("Brightness/Contrast"); break;
    case AdjustmentRequest::HueSaturation:      ImGui::OpenPopup("Hue/Saturation"); break;
    case AdjustmentRequest::Vibrance:           ImGui::OpenPopup("Vibrance"); break;
    case AdjustmentRequest::ColorBalance:       ImGui::OpenPopup("Colour Balance"); break;
    case AdjustmentRequest::BlackAndWhite:      ImGui::OpenPopup("Black & White"); break;
    case AdjustmentRequest::PhotoFilter:        ImGui::OpenPopup("Photo Filter"); break;
    case AdjustmentRequest::Posterize:          ImGui::OpenPopup("Posterize"); break;
    case AdjustmentRequest::Threshold:          ImGui::OpenPopup("Threshold"); break;
    case AdjustmentRequest::GradientMap:        ImGui::OpenPopup("Gradient Map"); break;

    // The six that act on the spot.
    case AdjustmentRequest::Desaturate:
      performImmediateAdjustment(st, "desaturate",
                                 [](OpenDocument& d) { return applyDesaturate(d); });
      break;
    case AdjustmentRequest::Invert:
      performImmediateAdjustment(st, "invert",
                                 [](OpenDocument& d) { return applyInvert(d); });
      break;
    case AdjustmentRequest::AutoTone:
      performImmediateAdjustment(st, "auto tone",
                                 [](OpenDocument& d) { return applyAutoTone(d); });
      break;
    case AdjustmentRequest::AutoContrast:
      performImmediateAdjustment(st, "auto contrast",
                                 [](OpenDocument& d) { return applyAutoContrast(d); });
      break;
    case AdjustmentRequest::AutoColor:
      performImmediateAdjustment(st, "auto colour",
                                 [](OpenDocument& d) { return applyAutoColor(d); });
      break;
    case AdjustmentRequest::Equalize:
      performImmediateAdjustment(st, "equalize",
                                 [](OpenDocument& d) { return applyEqualize(d); });
      break;

    case AdjustmentRequest::None:
      break;
  }
}

void drawAdjustmentDialogs(AppState& st) {
  serviceAdjustmentRequest(st);
  drawLevelsDialog(st);
  drawCurvesDialog(st);
  drawExposureDialog(st);
  drawChannelMixerDialog(st);
  drawBrightnessContrastDialog(st);
  drawHueSaturationDialog(st);
  drawVibranceDialog(st);
  drawColorBalanceDialog(st);
  drawBlackAndWhiteDialog(st);
  drawPhotoFilterDialog(st);
  drawPosterizeDialog(st);
  drawThresholdDialog(st);
  drawGradientMapDialog(st);
}

// ---------------------------------------------------------------------------
// The Image menu's two dialogs (PRD D17; app/FilterOps again)
// ---------------------------------------------------------------------------
//
// Document-level, so there is no `PixelOpRefusal` here (app/FilterOps.hpp's
// header argues why) -- the only failure mode is `ops/DocumentTransform`'s
// own, a zero extent, and it is shown exactly as it comes back rather than
// translated through a second vocabulary.
bool g_imageSizeRequested = false;
bool g_canvasSizeRequested = false;
bool g_numericTransformRequested = false;

void drawImageSizeDialog(AppState& st) {
  static int width = 0;
  static int height = 0;
  static int kernelIdx = 2;  // CatmullRom -- ops/Transform.hpp's own default
  static std::string status;
  static const ResampleKernel kKernels[] = {ResampleKernel::Nearest, ResampleKernel::Bilinear,
                                            ResampleKernel::CatmullRom, ResampleKernel::Mitchell,
                                            ResampleKernel::Lanczos3};

  OpenDocument* od = st.documents.active();
  if (g_imageSizeRequested) {
    g_imageSizeRequested = false;
    status.clear();
    if (od != nullptr) {
      width = od->document.width;
      height = od->document.height;
    }
    ImGui::OpenPopup("Image Size");
  }
  if (!ImGui::BeginPopupModal("Image Size", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputInt("Width", &width);
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputInt("Height", &height);
  constexpr int kKernelCount = sizeof(kKernels) / sizeof(kKernels[0]);
  if (ImGui::BeginCombo("Resample", resampleKernelName(kKernels[kernelIdx]))) {
    for (int i = 0; i < kKernelCount; ++i) {
      if (ImGui::Selectable(resampleKernelName(kKernels[i]), i == kernelIdx)) kernelIdx = i;
    }
    ImGui::EndCombo();
  }
  ImGui::TextDisabled(
      "Every layer resamples once, RGB, masks and any Pigment latents alike "
      "(ops/DocumentTransform.hpp); a Pigment layer's own kernel stays lobe-free "
      "regardless of this choice.");

  const bool valid = width > 0 && height > 0;
  if (!valid) ImGui::BeginDisabled();
  if (ImGui::Button("Resize") && od != nullptr) {
    const DocumentOpOutcome r =
        applyImageSize(*od, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                       kKernels[kernelIdx]);
    status = r.ok ? std::string() : r.error;
    if (r.ok) ImGui::CloseCurrentPopup();
  }
  if (!valid) ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

void drawCanvasSizeDialog(AppState& st) {
  static int width = 0;
  static int height = 0;
  static int anchorIdx = 4;  // Center -- CanvasAnchor's own middle entry
  static std::string status;
  static const char* kAnchorGlyph[9] = {"NW", "N", "NE", "W", "*", "E", "SW", "S", "SE"};

  OpenDocument* od = st.documents.active();
  if (g_canvasSizeRequested) {
    g_canvasSizeRequested = false;
    status.clear();
    if (od != nullptr) {
      width = od->document.width;
      height = od->document.height;
    }
    ImGui::OpenPopup("Canvas Size");
  }
  if (!ImGui::BeginPopupModal("Canvas Size", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputInt("Width", &width);
  ImGui::SetNextItemWidth(200.0f);
  ImGui::InputInt("Height", &height);
  ImGui::TextUnformatted("Anchor");
  // The nine-cell grid every canvas-size dialog offers, in the exact row-major
  // order `ops/Transform.hpp`'s `CanvasAnchor` enumerators are declared in --
  // so `anchorIdx` IS the enum's integer value and needs no lookup table
  // between the two.
  for (int i = 0; i < 9; ++i) {
    if (i % 3 != 0) ImGui::SameLine();
    ImGui::PushID(i);
    if (ImGui::RadioButton(kAnchorGlyph[i], anchorIdx == i)) anchorIdx = i;
    ImGui::PopID();
  }
  ImGui::TextDisabled("Existing pixels keep their values exactly; only the extent changes "
                      "(ops/Transform.hpp's cropImage(), zero resamples).");

  const bool valid = width > 0 && height > 0;
  if (!valid) ImGui::BeginDisabled();
  if (ImGui::Button("Resize") && od != nullptr) {
    const DocumentOpOutcome r =
        applyCanvasSize(*od, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                        static_cast<CanvasAnchor>(anchorIdx));
    status = r.ok ? std::string() : r.error;
    if (r.ok) ImGui::CloseCurrentPopup();
  }
  if (!valid) ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

// PRD item 10: a Photoshop-style numeric Transform dialog -- typed rotate/
// scale/translate fields instead of dragging the Free Transform gizmo's
// handles. Begins the identical `app/TransformSession` a Cmd+T gizmo would
// (same beginLayer()/beginSelectionPixels() choice, same undo funnel at
// commit), but drives `pending()` from these fields via `setPending()`
// instead of a drag -- see app/TransformSession.hpp's `composeNumericTransform()`
// and `setPending()` for why that is this session's own sanctioned path, not
// a bypass of "no setter but through a drag".
void drawNumericTransformDialog(AppState& st, GpuContext& gpu) {
  static float rotateDeg = 0.0f;
  static float scaleXPercent = 100.0f;
  static float scaleYPercent = 100.0f;
  static float translateX = 0.0f;
  static float translateY = 0.0f;
  static std::string status;

  OpenDocument* od = st.documents.active();
  if (g_numericTransformRequested) {
    g_numericTransformRequested = false;
    status.clear();
    // A session already active (e.g. the user has Free Transform's gizmo
    // open from Cmd+T) is refused rather than silently reused or restarted:
    // reusing it would need to read the CURRENT pending() back into these
    // numeric fields (an inverse-compose this dialog does not implement),
    // and restarting would silently discard whatever the user already
    // dragged in. Both are surprising; naming the conflict is not.
    if (st.transform.active()) {
      status = "A transform is already in progress. Press Return to apply it or "
               "Escape to cancel it, then try again.";
      ImGui::OpenPopup("Numeric Transform");
    } else if (od == nullptr) {
      status = "Numeric Transform needs an open document with a layer.";
      ImGui::OpenPopup("Numeric Transform");
    } else {
      const std::optional<size_t> li = activeLayerIndex(*od);
      if (!li) {
        status = "Numeric Transform needs an open document with a layer.";
      } else {
        const TransformBeginResult began =
            od->selection ? st.transform.beginSelectionPixels(*od, *od->selection, *li)
                          : st.transform.beginLayer(*od, *li);
        if (!began.ok) {
          status = began.error;
        } else {
          rotateDeg = 0.0f;
          scaleXPercent = 100.0f;
          scaleYPercent = 100.0f;
          translateX = 0.0f;
          translateY = 0.0f;
          beginTransformPreview(st, gpu);
        }
      }
      ImGui::OpenPopup("Numeric Transform");
    }
  }
  if (!ImGui::BeginPopupModal("Numeric Transform", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  if (!st.transform.active()) {
    // Reached only on a refusal path above (no document, no layer, or
    // beginLayer/beginSelectionPixels itself refused) -- nothing to show but
    // why, and an OK to dismiss.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button("OK")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }

  const DocumentRegion& bounds = st.transform.sourceBounds();
  const Point2 pivot{static_cast<float>(bounds.x) + static_cast<float>(bounds.width) * 0.5f,
                     static_cast<float>(bounds.y) + static_cast<float>(bounds.height) * 0.5f};

  bool edited = false;
  ImGui::SetNextItemWidth(200.0f);
  edited |= ImGui::DragFloat("Rotate (deg)", &rotateDeg, 0.5f, -360.0f, 360.0f, "%.1f");
  ImGui::SetNextItemWidth(200.0f);
  edited |= ImGui::DragFloat("Scale X (%)", &scaleXPercent, 0.5f, 1.0f, 1000.0f, "%.1f");
  ImGui::SetNextItemWidth(200.0f);
  edited |= ImGui::DragFloat("Scale Y (%)", &scaleYPercent, 0.5f, 1.0f, 1000.0f, "%.1f");
  ImGui::SetNextItemWidth(200.0f);
  edited |= ImGui::DragFloat("Move X (px)", &translateX, 0.5f, -100000.0f, 100000.0f, "%.1f");
  ImGui::SetNextItemWidth(200.0f);
  edited |= ImGui::DragFloat("Move Y (px)", &translateY, 0.5f, -100000.0f, 100000.0f, "%.1f");

  if (edited) {
    const Mat3 m = composeNumericTransform(rotateDeg, scaleXPercent / 100.0f,
                                           scaleYPercent / 100.0f, translateX, translateY, pivot);
    st.transform.setPending(m);
  }

  ImGui::TextDisabled(
      "Rotate and scale are about the centre of the %s; Move is an additional offset.",
      od != nullptr && od->selection ? "selection" : "layer");

  if (ImGui::Button("Apply") && od != nullptr) {
    const TransformCommitResult done = st.transform.commit(*od);
    status = done.ok ? std::string() : done.error;
    if (done.ok) {
      ImGui::CloseCurrentPopup();
      g_transformPreview.reset();
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    st.transform.cancel();
    g_transformPreview.reset();
    ImGui::CloseCurrentPopup();
  }
  if (!status.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", status.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// The menu bar's ImGui backend (ui/MenuModel.hpp)
// ---------------------------------------------------------------------------
//
// What used to be here was `drawDocumentMenuItems()` plus a 200-line
// `BeginMainMenuBar()` block, and between them they held forty-one
// `ImGui::MenuItem()` call sites that each *declared* an item and *performed*
// its action in the body of an `if`. ui/MenuModel.hpp's header comment carries
// the full argument for why that had to come apart; the short form is that a
// native `NSMenu` is built once, out of band, and calls back with no `if` to
// be the body of -- so an AppKit backend written against the old shape would
// have had to contain a second copy of every action.
//
// What is left in this file is a *backend*: `menuContextFromState()` reads the
// live application into ui/MenuModel.hpp's pure snapshot, `drawMenuNodes()`
// draws the tree that comes back, and neither of them contains an action.
// Every click -- from this bar or from the native one -- ends in
// `performMenuAction()` at the bottom of this section.

// `View > Add Guide...` used to call `ImGui::OpenPopup()` inline from inside
// `BeginMenu()`, and got away with it only because "AddGuidePopup" is
// identified by a global string ID rather than by one on the menu's own ID
// stack. It would not get away with it under a native menu bar, whose callback
// fires on the AppKit main thread with no ImGui frame in progress at all. So
// it joins `Export As...` and `Export Comps...` on the one-flag-per-frame
// route those two already took, for the reason those two already had --
// ui/MenuModel.hpp records all three as `MenuEffect::Deferred`.
bool g_addGuideRequested = false;

// --- The Select menu's five dialogs (docs/reachability-audit.md C5) --------
//
// Same one-flag-per-frame route as ExportAs/AddGuide above, for the same
// reason: `MenuAction::SelectGrow` &c. arrive from a native menu's AppKit
// callback with no ImGui frame in progress, so `performMenuAction()` sets a
// flag instead of calling `ImGui::OpenPopup()` directly.
//
// `SelectUndoRefine` needs none of these -- it has nothing to ask the user,
// so `performMenuAction()` performs it on the spot (see that case's comment).
bool g_selectGrowRequested = false;
bool g_selectShrinkRequested = false;
bool g_selectFeatherRequested = false;
bool g_selectColourRangeRequested = false;
bool g_selectLuminanceRangeRequested = false;

// The shape Grow, Shrink and Feather share: a title, a one-line explanation
// of what THIS op's radius means (grow/shrink move an edge; feather softens
// one -- different enough to be worth saying, per core/SelectionRefine.hpp
// §1 vs ops/Feather.hpp §2), and one positive radius. Written once rather
// than three times, because three near-identical copies is exactly the shape
// that lets one silently drift -- a click confirming "Feather" that reads a
// stale "Grow" radius because a copy-paste missed one rename, say.
//
// `radius` defaults to 4.0f: visible against a typical marquee without being
// a startling first click, and NOT zero -- growSelection(s, 0) is a
// documented no-op (core/SelectionRefine.hpp), so a default of zero would
// make a user's first "Grow" click look like nothing happened at all.
struct RefineRadiusDialog {
  const char* popupId;
  const char* verb;          // the confirm button's label: "Grow", "Shrink", "Feather"
  const char* explanation;
  MenuAction action;         // which of applySelectRefineAction()'s three cases to reach
  float radius = 4.0f;
};

// One popup, shared by all three static instances in drawSelectMenuDialogs()
// below. `*requested` is the flag `performMenuAction()` set; `dlg.action`
// (not a passed-in function pointer) is what selects growSelection() /
// shrinkSelection() / featherSelection() inside applySelectRefineAction() --
// keeping that dispatch in ONE switch, shared with --selftest, rather than
// wiring each call site to a function pointer here where nothing but a
// human reading the diff could notice two dialogs pointed at the same one.
void drawRefineRadiusDialog(AppState& st, RefineRadiusDialog& dlg, bool* requested) {
  if (*requested) {
    *requested = false;
    ImGui::OpenPopup(dlg.popupId);
  }
  if (!ImGui::BeginPopupModal(dlg.popupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::TextWrapped("%s", dlg.explanation);
  ImGui::SetNextItemWidth(160.0f);
  // 500.0f: comfortably past anything a document this build ships needs --
  // core/SelectionRefine.hpp's own cost section prices a Select All on a 4K
  // canvas grown by 20 at ~18 ms and ~816 MiB peak, transient; 500 is the
  // point a further ceiling would be protecting against a typo, not a
  // workflow, and DragFloat's own clamp already refuses anything past it.
  ImGui::DragFloat("Radius (px)", &dlg.radius, 0.25f, 0.0f, 500.0f, "%.2f");
  ImGui::Separator();

  OpenDocument* od = st.documents.active();
  const bool usable = od != nullptr && selectRefineEnabled(*od);
  if (od == nullptr) {
    ImGui::TextDisabled("No document is open.");
  } else if (!usable) {
    ImGui::TextDisabled("Nothing is selected -- there is no edge to move.");
  }
  ImGui::BeginDisabled(!usable);
  if (ImGui::Button(dlg.verb)) {
    installRefinedSelection(*od, applySelectRefineAction(dlg.action, *od->selection, dlg.radius));
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Colour Range (PRD E9). The colour comes from an on-the-spot swatch rather
// than an eyedropper: PRD A1/A2 (docs/reachability-audit.md) records that
// this build's eyedropper has nowhere to put a picked colour yet, and Colour
// Range does not need to wait on that -- Photoshop's own dialog offers a
// swatch alongside its eyedropper for the same reason `selectColourRange()`
// takes a colour value rather than only a seed coordinate (core/
// SelectionRefine.hpp §3: "a function that can only be given a texel cannot
// serve the swatch").
void drawSelectColourRangeDialog(AppState& st) {
  static float swatchSrgb[3] = {0.5f, 0.5f, 0.5f};
  // Defaults match core/SelectionRefine.hpp's SelectionRangeParams -- the
  // same numbers ops/FloodFill.hpp's magic wand defaults to (§3: "the same
  // numbers meaning the same thing"), so a user arriving from the wand
  // already knows what this slider does.
  static float tolerance = kFloodDefaultTolerance;
  static float edgeBand = kFloodDefaultEdgeBand;

  if (g_selectColourRangeRequested) {
    g_selectColourRangeRequested = false;
    ImGui::OpenPopup("Colour Range");
  }
  if (!ImGui::BeginPopupModal("Colour Range", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::TextWrapped(
      "Selects every pixel on the active layer within tolerance of this colour -- connected "
      "or not, unlike the magic wand.");
  ImGui::ColorEdit3("Colour", swatchSrgb);
  ImGui::SetNextItemWidth(160.0f);
  ImGui::SliderFloat("Tolerance", &tolerance, 0.0f, 1.0f, "%.3f");
  ImGui::SetNextItemWidth(160.0f);
  // Capped at `tolerance` on the slider itself as well as internally
  // (applySelectColourRangeAction() clamps again) -- an edge band wider than
  // the tolerance it is softening the OUTSIDE of is not a state the dialog
  // should let a user reach and then silently correct underneath them.
  ImGui::SliderFloat("Edge softness", &edgeBand, 0.0f, std::max(tolerance, 0.001f), "%.3f");
  ImGui::Separator();

  OpenDocument* od = st.documents.active();
  const bool usable = od != nullptr && selectRangeEnabled(*od);
  const Layer* target = usable ? activeLayerOf(*od) : nullptr;
  if (!usable) {
    ImGui::TextDisabled("The active layer has no RGB pixels to sample.");
  }
  ImGui::BeginDisabled(!usable);
  if (ImGui::Button("Select")) {
    const std::array<float, 3> swatch = {swatchSrgb[0], swatchSrgb[1], swatchSrgb[2]};
    installRefinedSelection(
        *od, applySelectColourRangeAction(swatch, tolerance, edgeBand, *target->rgbTiles,
                                          od->document.width, od->document.height));
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// Luminance Range (PRD E9): a band rather than a tolerance around a sample.
void drawSelectLuminanceRangeDialog(AppState& st) {
  static float low = 0.0f;
  static float high = 1.0f;
  static float edgeBand = kFloodDefaultEdgeBand;

  if (g_selectLuminanceRangeRequested) {
    g_selectLuminanceRangeRequested = false;
    ImGui::OpenPopup("Luminance Range");
  }
  if (!ImGui::BeginPopupModal("Luminance Range", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ImGui::TextWrapped(
      "Selects every pixel on the active layer whose brightness falls in this band "
      "(display-encoded, so 0.75..1.0 means the visibly brightest quarter).");
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Low", &low, 0.0f, 1.0f, "%.3f");
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("High", &high, 0.0f, 1.0f, "%.3f");
  if (low > high) {
    // core/SelectionRefine.hpp: "low > high selects nothing (an empty band is
    // empty, not inverted)". Said out loud here rather than left for the
    // user to discover from an empty result with no explanation -- the same
    // "honest refusal" standard the audit asks of the menu items themselves.
    const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("Low is above High -- this selects nothing, rather than everything "
                       "outside the band.");
    ImGui::PopStyleColor();
  }
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Edge softness", &edgeBand, 0.0f, 0.25f, "%.3f");
  ImGui::Separator();

  OpenDocument* od = st.documents.active();
  const bool usable = od != nullptr && selectRangeEnabled(*od);
  const Layer* target = usable ? activeLayerOf(*od) : nullptr;
  if (!usable) {
    ImGui::TextDisabled("The active layer has no RGB pixels to sample.");
  }
  ImGui::BeginDisabled(!usable);
  if (ImGui::Button("Select")) {
    installRefinedSelection(
        *od, applySelectLuminanceRangeAction(low, high, edgeBand, *target->rgbTiles,
                                             od->document.width, od->document.height));
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// The five dialogs together, called once a frame from the same place
// drawExportAsDialog() &c. are: outside BeginMainMenuBar()/EndMainMenuBar(),
// because a popup opened from a menu item has to begin outside the menu
// bar's own ID stack (the comment beside those calls explains this at
// length; it is not repeated a sixth time here).
void drawSelectMenuDialogs(AppState& st) {
  static RefineRadiusDialog growDlg{
      "Grow Selection", "Grow",
      "Moves the selection's edge outward by this many pixels -- a real number, so the new "
      "edge can land mid-pixel rather than snapping to a whole one.",
      MenuAction::SelectGrow};
  static RefineRadiusDialog shrinkDlg{
      "Shrink Selection", "Shrink",
      "Moves the selection's edge inward by this many pixels.", MenuAction::SelectShrink};
  static RefineRadiusDialog featherDlg{
      "Feather Selection", "Feather",
      "Softens the selection's edge with a blur -- the visible soft band is about this many "
      "pixels on each side of the original edge.",
      MenuAction::SelectFeather};

  drawRefineRadiusDialog(st, growDlg, &g_selectGrowRequested);
  drawRefineRadiusDialog(st, shrinkDlg, &g_selectShrinkRequested);
  drawRefineRadiusDialog(st, featherDlg, &g_selectFeatherRequested);
  drawSelectColourRangeDialog(st);
  drawSelectLuminanceRangeDialog(st);
}

MenuFamilyEntry familyEntry(std::string label, bool enabled, bool checked,
                            bool separatorAfter = false, std::string tooltip = {}) {
  MenuFamilyEntry e;
  e.label = std::move(label);
  e.tooltip = std::move(tooltip);
  e.enabled = enabled;
  e.checked = checked;
  e.separatorAfter = separatorAfter;
  return e;
}

// The live application, as the pure snapshot `buildMenuModel()` consumes.
//
// **The six families are resolved here, not there.** ui/MenuModel.hpp explains
// why at length: a `const Document*` parked on the context would be a dangling
// pointer the first time a user closed a document with a native menu open, and
// the native backend genuinely does read this state after the frame that
// produced it has ended. So availability is asked of `app/LayerEditor` and
// `core/LayerSetOps` -- the modules that own the rule -- inside the frame, and
// only the answers travel.
MenuContext menuContextFromState(AppState& st) {
  MenuContext ctx;

  // **This load used to be lazy, and it cannot be any more.** It sat at the
  // top of `drawDocumentMenuItems()` and therefore ran the first time the File
  // menu was *opened*, on the argument (PRD A2, ADR-0001) that a file nobody
  // asked for costs nothing.
  //
  // A native menu bar has no such moment. It is on screen from launch, and it
  // is built out of band, so `Open Recent` has to know whether it has entries
  // before the user has expressed any interest in the File menu at all. The
  // choice was between eager on both platforms and a lazy path that exists
  // only on one -- and a load that happens at a different time on macOS than
  // on Linux is the kind of difference that makes a bug reproduce on one
  // machine and not the other.
  //
  // The cost is one small file read on the first UI frame. It is deliberately
  // NOT moved to startup: `--selftest` never calls drawUI(), so the idle-RSS
  // assertion (PLAN.md 1.4 / ADR-0001 bullet 5) is still sampled on a process
  // that has never touched this file.
  if (!st.recentDocumentsLoaded) {
    st.recentDocumentsLoaded = true;
    st.recentDocuments.loadFromFile(defaultRecentDocumentsPath());
  }

  const OpenDocument* doc = st.documents.active();
  ctx.hasDocument = doc != nullptr;
  ctx.hasPath = ctx.hasDocument && doc->hasPath();

  // --- Edit -----------------------------------------------------------------
  //
  // D1 + D2: mirrors the guards at the one place each of these commands is
  // actually performed -- `moveHistoryCursor()`'s callers for undo/redo, the
  // request-flag consumption block a few hundred lines down in this file for
  // the clipboard nine. See ui/MenuModel.hpp's `MenuContext::canUndo` comment
  // for why that is copied rather than shared.
  {
    const Layer* target = doc != nullptr ? activeLayerOf(*doc) : nullptr;
    ctx.canUndo = doc != nullptr && doc->history.canUndo();
    ctx.canRedo = doc != nullptr && doc->history.canRedo();
    ctx.hasActiveLayer = target != nullptr;
    ctx.hasEditableLayer = target != nullptr && !target->locked;
    ctx.clipboardHasContent = !st.clipboard.empty();
    ctx.hasSelection = doc != nullptr && doc->selection.has_value();
    ctx.hasLastDeselected = doc != nullptr && doc->lastDeselected.has_value();
  }

  // --- Open Recent --------------------------------------------------------
  // A missing entry is shown, greyed, with the reason in its tooltip -- never
  // dropped behind the user's back (app/DocumentLifecycle.hpp argues why).
  {
    const std::vector<RecentDocument>& entries = st.recentDocuments.entries();
    for (const RecentDocument& entry : entries) {
      std::string why;
      const bool missing = recentDocumentMissing(entry.path, &why);
      ctx.recentDocuments.push_back(
          familyEntry(entry.displayName, !missing, false, false, missing ? why : entry.path));
    }
  }

  // --- Layer --------------------------------------------------------------
  if (doc != nullptr) {
    const Document& d = doc->document;
    const size_t selected = doc->activeLayer;
    ctx.activeLayerTitle = selected < d.layers.size()
                               ? layerRowTitle(d.layers[selected], selected)
                               : std::string("(no layer selected)");

    for (const LayerCommand command : allLayerCommands()) {
      // The three toggles show the selected layer's current state as a check
      // mark, which is what makes "Toggle Visibility" honest about which way
      // it is about to go.
      bool checked = false;
      if (selected < d.layers.size()) {
        if (command == LayerCommand::ToggleVisible) checked = d.layers[selected].visible;
        if (command == LayerCommand::ToggleLocked) checked = d.layers[selected].locked;
        if (command == LayerCommand::ToggleClipped) checked = d.layers[selected].clipped;
        if (command == LayerCommand::ToggleAlphaLock) checked = d.layers[selected].alphaLocked;
      }
      // Grouped as the panel groups them: creation, then the whole-layer
      // operations, then the mask, then the flags.
      const bool rule = command == LayerCommand::NewAdjustmentLayer ||
                        command == LayerCommand::MoveLayerDown ||
                        command == LayerCommand::RemoveMask ||
                        command == LayerCommand::ToggleClipped;
      ctx.layerCommands.push_back(familyEntry(layerCommandLabel(command),
                                              layerCommandAvailable(d, command, selected),
                                              checked, rule));
    }

    // The LAYERS panel's "Multi-selection" section walks the identical list,
    // so the two views cannot come to offer different sets.
    const LayerSelection visible = restrictSelectionToFilter(d, g_layers.selection, g_layers.filter);
    ctx.layerSelectionNote = std::to_string(g_layers.selection.size()) + " layer(s) selected" +
                             (visible.size() != g_layers.selection.size()
                                  ? ", some hidden by the filter"
                                  : "");
    for (const LayerSetCommand command : allLayerSetCommands()) {
      const bool rule = command == LayerSetCommand::MoveLayersDown ||
                        command == LayerSetCommand::UnclipLayers ||
                        command == LayerSetCommand::UnlinkLayers ||
                        command == LayerSetCommand::LabelGrey ||
                        command == LayerSetCommand::AlignSelectionBottom ||
                        command == LayerSetCommand::AlignCanvasBottom;
      ctx.layerSetCommands.push_back(familyEntry(
          layerSetCommandLabel(command), layerSetCommandAvailable(d, command, visible), false,
          rule));
    }
  }

  // --- Select ---------------------------------------------------------------
  //
  // Resolved against `*doc` here, inside the frame, for the identical reason
  // every other predicate on this context is: a native menu backend reads
  // this snapshot after the frame that produced it has ended, so a live
  // `OpenDocument*` on the context would be a dangling pointer the first time
  // a user closed a document with the menu open.
  if (doc != nullptr) {
    ctx.hasEngagedSelection = selectRefineEnabled(*doc);
    ctx.hasRgbSource = selectRangeEnabled(*doc);
    ctx.hasRefineUndo = selectUndoRefineEnabled(*doc);
  }

  // --- Medium / Goodies ---------------------------------------------------
  for (int i = 0; i < static_cast<int>(PaintMode::Count); ++i) {
    const PaintMode m = static_cast<PaintMode>(i);
    ctx.paintModes.push_back(familyEntry(paintModeName(m), true, st.mode == m));
  }
  // A4 (reachability audit): see `toolMenuFamily()`'s own comment
  // (ui/MacPaintUI.hpp) for the bug this replaced -- this loop used to pass
  // `enabled = true` unconditionally, so all 27 tools were freely selectable
  // from Goodies while `toolButton()` (AtelierChrome.cpp:456) gated the same
  // list correctly one panel over. Factored out rather than fixed in place
  // so `--selftest` can call the exact predicate the menu uses without
  // needing an `AppState` or touching the recent-documents file this
  // function's own first line reads.
  ctx.tools = toolMenuFamily(st.brush.tool);
  ctx.paused = st.paused;

  // --- View ---------------------------------------------------------------
  ctx.mirrorX = st.view.mirrorX;
  ctx.mirrorY = st.view.mirrorY;
  ctx.grayscale = st.view.grayscale;
  ctx.showRulers = st.showRulers;
  ctx.showNavigator = st.showNavigator;
  ctx.showBrushSettings = st.showBrushSettings;
  ctx.showGuides = st.showGuides;
  ctx.showGrid = st.showGrid;
  ctx.snappingEnabled = st.snappingEnabled;
  ctx.hasGuides = !st.guides.empty();

  // --- Window -------------------------------------------------------------
  ctx.showDemo = st.showDemo;
  for (size_t i = 0; i < st.documents.count(); ++i) {
    const OpenDocument* d = st.documents.at(i);
    ctx.openDocuments.push_back(familyEntry(documentDisplayName(*d) + (d->isDirty() ? " *" : ""),
                                            true, i == st.documents.activeIndex(), false,
                                            d->isDirty() ? d->unsavedWorkSummary() : std::string()));
  }

  // --- Filter / Image ------------------------------------------------
  //
  // The identical predicate the paint bucket and the gradient gate on
  // (~7058 above), resolved here rather than carried as a `Layer*` -- see
  // ui/MenuModel.hpp's own header on why the context holds no live pointer.
  // `activeLayerOf()` on a `const OpenDocument*` is unavailable (the
  // non-const overload only), so this reaches for the mutable one exactly
  // as the rest of this function already has `doc` for.
  {
    Layer* target = doc != nullptr ? activeLayerOf(*st.documents.active()) : nullptr;
    const PixelOpRefusal reason = pixelOpRefusalFor(target);
    ctx.filterLayerUsable = reason == PixelOpRefusal::None;
    if (!ctx.filterLayerUsable)
      ctx.filterRefusalNote = pixelOpRefusalMessage(reason, target, "filter");
  }

  ctx.nativeAppMenuPresent = nativeMenuBarInstalled();
  return ctx;
}

// Draw one level of the tree. Recursive, because the tree is.
void drawMenuNodes(AppState& st, const std::vector<MenuNode>& nodes, uint32_t canvasW,
                   uint32_t canvasH) {
  for (const MenuNode& n : nodes) {
    switch (n.kind) {
      case MenuNodeKind::Separator:
        ImGui::Separator();
        break;
      case MenuNodeKind::Note:
        ImGui::TextDisabled("%s", n.label.c_str());
        break;
      case MenuNodeKind::Submenu:
        if (ImGui::BeginMenu(n.label.c_str(), n.enabled)) {
          drawMenuNodes(st, n.children, canvasW, canvasH);
          ImGui::EndMenu();
        }
        break;
      case MenuNodeKind::Command:
      case MenuNodeKind::Check: {
        // `PushID(param)` rather than the `"##doc0"` suffix the Window menu
        // used to bake into its labels. Two open documents with the same
        // display name would otherwise share an ImGui ID and the second would
        // be unclickable -- and a label carrying an ImGui ID hack inside it is
        // a label the native backend would have to know to strip.
        ImGui::PushID(n.param);
        const char* shortcut = n.shortcutText.empty() ? nullptr : n.shortcutText.c_str();
        if (ImGui::MenuItem(n.label.c_str(), shortcut, n.checked, n.enabled))
          performMenuAction(st, n.action, n.param, canvasW, canvasH);
        // Hover text is carried on the node rather than written at the call
        // site, so the explanation a greyed item owes the user survives into
        // whichever backend is drawing. `AllowWhenDisabled` for the disabled
        // ones is the whole point: "Import Image..." is greyed precisely when
        // it has something to explain.
        // `n.enabled` used to pick between `ImGuiHoveredFlags_None` and
        // `_AllowWhenDisabled` here; SetItemTooltip()'s own default flags
        // already carry `AllowWhenDisabled`, and that flag is a no-op on an
        // item that is not disabled, so the ternary added nothing this
        // could not get from the default.
        if (!n.tooltip.empty()) ImGui::SetItemTooltip("%s", n.tooltip.c_str());
        ImGui::PopID();
        break;
      }
    }
  }
}

}  // namespace

// Declared in ui/MacPaintUI.hpp, which carries the full argument for why this
// exists and why it is public. Defined here, after the anonymous namespace
// closes, for the same reason `performMenuAction()` just below is: it calls
// two names that stay file-local (`settleWetPaintBeforeHistoryMove()`,
// `installHistoryCursor()`), which this translation unit can still see this
// far down even though neither has external linkage.
void moveHistoryCursor(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
                       OpenDocument& od, int direction) {
  settleWetPaintBeforeHistoryMove(st, sim, gpu, od);
  History& h = od.history;
  installHistoryCursor(od, historyPanelClick(h, historySerialForRow(h, h.cursor() + direction)));
}

// Declared in ui/MacPaintUI.hpp, which carries the full argument. Defined
// here, after the anonymous namespace closes, so it can still call
// `familyEntry()` (file-local, defined above) while itself having the
// external linkage `menuContextFromState()` and `app/selftest/MenuBasics.cpp`
// both need -- the former to assign `ctx.tools`, the latter to assert the
// A4 fix directly.
std::vector<MenuFamilyEntry> toolMenuFamily(Tool current) {
  std::vector<MenuFamilyEntry> tools;
  for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
    const Tool t = static_cast<Tool>(i);
    const bool implemented = toolImplemented(t);
    tools.push_back(familyEntry(toolName(t), implemented, current == t, false,
                                implemented ? std::string() : toolTooltip(t)));
  }
  return tools;
}

// ---------------------------------------------------------------------------
// **The one place a menu action is performed.**
// ---------------------------------------------------------------------------
//
// Declared in ui/MenuModel.hpp and defined here rather than there, on purpose.
// Roughly half of these bodies poke this file's own file-local dialog state --
// `g_exportAsRequested`, `g_docPathAction`, `g_docStatus`, `g_recoveryRequested`,
// `g_revertConfirmRequested`, `g_addGuideRequested`, `g_layers` -- and the
// alternative was to promote nine anonymous-namespace globals into `AppState`
// so that a header could see them. That is a much larger and much riskier
// change than declaring one function, and it would have moved dialog
// bookkeeping into the application state for no reason except a header's
// convenience.
//
// Defined **after** the anonymous namespace closes so it has external linkage
// (ui/MacNativeMenu.mm and app/selftest/MenuModel.cpp both call it) while still
// seeing every one of those file-local names, which are declared above it in
// the same translation unit.
//
// Every body here is the body of the `if (ImGui::MenuItem(...))` it replaced,
// moved verbatim. Nothing was rewritten on the way; the point of the exercise
// was to change *where* the actions live, not what they do.
//
// **Safe to call outside an ImGui frame.** That is a requirement, not an
// observation: a native menu's callback fires on the AppKit main thread from
// inside SDL's Cocoa pump, with no frame in progress. Every action that opens
// a modal sets a flag instead of calling `ImGui::OpenPopup()`, which
// ui/MenuModel.hpp records as `MenuEffect::Deferred` and which
// app/selftest/MenuModel.cpp asserts is true of all nine of them.
void performMenuAction(AppState& st, MenuAction action, int param, uint32_t canvasW,
                       uint32_t canvasH) {
  OpenDocument* doc = st.documents.active();

  switch (action) {
    // --- File -------------------------------------------------------------
    case MenuAction::NewCanvas:
      st.requestClear = true;
      break;

    case MenuAction::NewDocument:
      // T9: used to silently inherit the solver canvas's dimensions
      // (`canvasW`/`canvasH` are now unused by this case, but stay on
      // `performMenuAction`'s signature since every other action shares it).
      // Opens ui/NewDocumentDialog.hpp's modal instead, which is where a size
      // actually gets chosen -- a preset, a hand-typed size, or the
      // clipboard's own resolution.
      requestNewDocumentDialog();
      g_docStatus.clear();
      break;

    case MenuAction::Open:
      g_docPathAction = DocPathAction::Open;
      g_docPathRequested = true;
      break;

    case MenuAction::OpenRecentEntry: {
      const std::vector<RecentDocument>& entries = st.recentDocuments.entries();
      if (param < 0 || static_cast<size_t>(param) >= entries.size()) break;
      // The model already greys a missing entry out, and both backends honour
      // that. This re-checks anyway, and the reason is specific to the native
      // bar: an `NSMenu` is built once and then *sits there*, so the file can
      // be moved or deleted between the menu being built and the user picking
      // the row. The ImGui bar could not reach this branch; the native one
      // can, and the honest answer is the reason rather than a silent no-op.
      std::string why;
      if (recentDocumentMissing(entries[static_cast<size_t>(param)].path, &why)) {
        g_docStatus = why;
        break;
      }
      OpenDocument opened;
      const DocumentOpResult r =
          openRecentDocument(st.recentDocuments, static_cast<size_t>(param), &opened);
      if (r.ok) {
        st.documents.add(std::move(opened));
        std::string saveErr;
        st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
      }
      g_docStatus = r.ok ? "Opened " + r.path : r.error;
      break;
    }

    case MenuAction::ClearRecentMenu: {
      st.recentDocuments.clear();
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
      break;
    }

    case MenuAction::ImportImage:
      g_docPathAction = DocPathAction::ImportImage;
      g_docPathRequested = true;
      break;

    case MenuAction::Save: {
      if (doc == nullptr) break;
      const DocumentOpResult r = saveDocument(*doc, {}, &st.recentDocuments);
      if (r.ok) {
        std::string saveErr;
        st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
      }
      g_docStatus = r.ok ? "Saved " + r.path : r.error;
      break;
    }

    case MenuAction::SaveAs:
      g_docPathAction = DocPathAction::SaveAs;
      g_docPathRequested = true;
      break;

    case MenuAction::SaveCopy:
      g_docPathAction = DocPathAction::SaveCopy;
      g_docPathRequested = true;
      break;

    case MenuAction::SaveIncremental: {
      if (doc == nullptr) break;
      const DocumentOpResult r = saveDocumentIncremental(*doc, {}, &st.recentDocuments);
      if (r.ok) {
        std::string saveErr;
        st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
      }
      g_docStatus = r.ok ? "Saved " + r.path : r.error;
      break;
    }

    case MenuAction::RecoverDocuments:
      // The list is re-scanned rather than reused, so a session another
      // instance finished since launch is not offered from a stale snapshot.
      st.recovery = discoverRecoverySessions();
      g_recoveryRequested = true;
      break;

    case MenuAction::Revert:
      g_revertConfirmRequested = true;
      break;

    case MenuAction::DuplicateDocument:
      if (doc == nullptr) break;
      st.documents.add(duplicateDocument(*doc));
      g_docStatus = "Duplicated (unbound -- Save As gives it a file of its own).";
      break;

    case MenuAction::CloseDocument: {
      if (doc == nullptr) break;
      // The same entry point the tab strip's close box uses
      // (app/CloseDecision.hpp): a clean document closes on this click, a
      // dirty one raises Save / Don't Save / Cancel.
      const CloseOutcome outcome =
          requestDocumentClose(st.documents, st.documents.activeIndex(), st.pendingClose);
      if (!outcome.status.empty()) g_docStatus = outcome.status;
      break;
    }

    case MenuAction::ExportAs:
      g_exportAsRequested = true;
      break;

    case MenuAction::ExportStates:
      g_exportStatesRequested = true;
      break;

    // **`requestQuit`, not `quit`.**
    //
    // This one line is the whole reason ui/MenuModel.hpp has a `MenuEffect`
    // enum. `AppState::quit` stops the frame loop outright, with nothing in
    // the way; `AppState::requestQuit` is answered by main.cpp's guard against
    // every open document (app/QuitSequence) -- Save / Don't Save / Cancel,
    // once per dirty document. This item used to set `quit` directly and
    // therefore threw away every unsaved document without a word, and also
    // deleted the recovery journal's copy of them on the way past, because a
    // clean shutdown removes the scratch directory (PRD O8).
    //
    // A native backend has a quieter way to make the identical mistake:
    // wiring the item to Cocoa's `@selector(terminate:)`. See
    // `MenuItemSpec::mayUseSystemSelector` and the Part D assertions in
    // app/selftest/MenuModel.cpp, which exist so that neither form of it can
    // be reintroduced without the suite going red.
    case MenuAction::Quit:
      st.requestQuit = true;
      break;

    // --- Edit -------------------------------------------------------------
    //
    // D1 + D2 (docs/reachability-audit.md). Every one of these eleven sets
    // the SAME request flag main.cpp's keymap dispatch already sets for the
    // matching chord (see that file's SDL_EVENT_KEY_DOWN block) and, for
    // Undo/Redo, that the title-bar buttons and the HISTORY panel already
    // drive through `moveHistoryCursor()`. Nothing here performs an edit
    // directly -- the request-flag consumption a few hundred lines down in
    // this file (drawUI()'s "selection and clipboard commands" block, and
    // its undo/redo block just above it) is the one place that does, because
    // that is the one place with an `OpenDocument&`, a `sim` and a `gpu` to
    // do it with. A menu callback and a native one both fire outside that
    // context, which is exactly the constraint the flag exists to route
    // around.
    case MenuAction::Undo:
      st.requestUndo = true;
      break;
    case MenuAction::Redo:
      st.requestRedo = true;
      break;
    // A request, like every neighbour here, and for this one's own reason:
    // starting a transform has to choose between the whole active layer and
    // just the pixels under a selection, which needs the live document. The
    // canvas block has one; a native menu callback on the AppKit thread does
    // not. `keymaps/default.json`'s "free_transform" sets the identical flag
    // from main.cpp, so the chord and the menu item are one code path from
    // here on rather than two that can drift.
    case MenuAction::FreeTransform:
      st.requestFreeTransform = true;
      break;
    case MenuAction::NumericTransform:
      g_numericTransformRequested = true;
      break;
    case MenuAction::Cut:
      st.requestCut = true;
      break;
    case MenuAction::Copy:
      st.requestCopy = true;
      break;
    case MenuAction::CopyMerged:
      st.requestCopyMerged = true;
      break;
    case MenuAction::Paste:
      st.requestPaste = true;
      break;
    case MenuAction::DeleteSelection:
      st.requestDeleteSelection = true;
      break;
    case MenuAction::SelectAll:
      st.requestSelectAll = true;
      break;
    case MenuAction::Deselect:
      st.requestDeselect = true;
      break;
    case MenuAction::Reselect:
      st.requestReselect = true;
      break;
    case MenuAction::InvertSelection:
      st.requestInvertSelection = true;
      break;

    case MenuAction::ClearCanvas:
      st.requestClear = true;
      break;

    // --- Layer ------------------------------------------------------------
    case MenuAction::LayerCommandItem: {
      const std::vector<LayerCommand>& commands = allLayerCommands();
      if (param < 0 || static_cast<size_t>(param) >= commands.size()) break;
      runLayerCommand(st, commands[static_cast<size_t>(param)]);
      break;
    }

    case MenuAction::LayerSetCommandItem: {
      const std::vector<LayerSetCommand>& commands = allLayerSetCommands();
      if (param < 0 || static_cast<size_t>(param) >= commands.size()) break;
      runLayerSetCommand(st, commands[static_cast<size_t>(param)]);
      break;
    }

    // --- Select -------------------------------------------------------------
    //
    // Each of the five sets a request flag rather than acting -- the same
    // `MenuEffect::Deferred` route ExportAs/AddGuide use, and for the
    // identical reason: opening the dialog is `ImGui::OpenPopup()`, which
    // must not be called from a native menu's AppKit callback. The engine
    // call itself happens in drawSelectMenuDialogs()'s confirm button, through
    // applySelectRefineAction() / applySelectColourRangeAction() /
    // applySelectLuminanceRangeAction() (ui/MacPaintUI.hpp) -- the boundary
    // app/selftest/SelectMenu.cpp exercises directly, since it cannot open a
    // popup either.
    case MenuAction::SelectGrow:
      g_selectGrowRequested = true;
      break;
    case MenuAction::SelectShrink:
      g_selectShrinkRequested = true;
      break;
    case MenuAction::SelectFeather:
      g_selectFeatherRequested = true;
      break;
    case MenuAction::SelectColourRange:
      g_selectColourRangeRequested = true;
      break;
    case MenuAction::SelectLuminanceRange:
      g_selectLuminanceRangeRequested = true;
      break;

    // Acts immediately -- there is nothing to ask the user, so there is no
    // dialog to defer to. `doc == nullptr` (no open document) is exactly
    // `selectUndoRefineEnabled()`'s false case, reached here only via a stale
    // queued native-menu action (see undoLastRefine()'s own comment).
    case MenuAction::SelectUndoRefine:
      if (doc != nullptr) undoLastRefine(*doc);
      break;

    // --- Medium / Goodies -------------------------------------------------
    case MenuAction::PaintModeItem: {
      if (param < 0 || param >= static_cast<int>(PaintMode::Count)) break;
      const PaintMode m = static_cast<PaintMode>(param);
      // Guarded, not unconditional: `requestMode` clears the canvas, and
      // picking the mode that is already current must not.
      if (st.mode != m) {
        st.mode = m;
        st.requestMode = true;
      }
      break;
    }

    case MenuAction::ToolItem:
      if (param < 0 || param >= static_cast<int>(Tool::Count)) break;
      st.brush.tool = static_cast<Tool>(param);
      break;

    case MenuAction::PauseSolver:
      st.paused = !st.paused;
      break;

    case MenuAction::ReloadShaders:
      st.requestReload = true;
      break;

    // --- View -------------------------------------------------------------
    case MenuAction::FitToWindow: st.requestFitWindow = true; break;
    case MenuAction::Zoom100:     st.requestZoom100 = true;   break;
    case MenuAction::ZoomIn:      st.requestZoomIn = true;    break;
    case MenuAction::ZoomOut:     st.requestZoomOut = true;   break;

    case MenuAction::MirrorX:          st.view.mirrorX = !st.view.mirrorX;       break;
    case MenuAction::MirrorY:          st.view.mirrorY = !st.view.mirrorY;       break;
    case MenuAction::ResetRotation:    st.view.rotation = 0.0f;                  break;
    case MenuAction::ResetView:        st.view = resetCanvasView(st.view);       break;
    case MenuAction::GrayscalePreview: st.view.grayscale = !st.view.grayscale;   break;
    case MenuAction::Rulers:           st.showRulers = !st.showRulers;           break;
    case MenuAction::Navigator:        st.showNavigator = !st.showNavigator;     break;
    // Inline, not Deferred: this flips a bool that next frame's
    // `drawBrushSettingsWindow()` reads. It opens no ImGui popup, so it is
    // safe to perform from a native menu callback with no frame in progress
    // -- which is the distinction MenuEffect exists to make.
    case MenuAction::BrushSettings:
      st.showBrushSettings = !st.showBrushSettings;
      break;
    case MenuAction::Guides:           st.showGuides = !st.showGuides;           break;
    case MenuAction::Grid:             st.showGrid = !st.showGrid;               break;
    case MenuAction::Snap:             st.snappingEnabled = !st.snappingEnabled; break;

    case MenuAction::AddGuide:
      g_addGuideRequested = true;
      break;

    case MenuAction::ClearGuides:
      st.guides.clear();
      break;

    // --- Window -----------------------------------------------------------
    case MenuAction::ImGuiDemo:
      st.showDemo = !st.showDemo;
      break;

    case MenuAction::ActivateDocument:
      if (param < 0 || static_cast<size_t>(param) >= st.documents.count()) break;
      st.documents.setActive(static_cast<size_t>(param));
      break;

    // --- Filter -------------------------------------------------------
    case MenuAction::GaussianBlur: g_gaussianBlurRequested = true; break;
    case MenuAction::Sharpen:      g_sharpenRequested = true;      break;
    case MenuAction::UnsharpMask:  g_unsharpMaskRequested = true;  break;
    case MenuAction::AddNoise:     g_addNoiseRequested = true;     break;
    case MenuAction::Emboss:       g_embossRequested = true;       break;
    case MenuAction::Median:       g_medianRequested = true;       break;
    case MenuAction::MotionBlur:   g_motionBlurRequested = true;   break;

    // --- Image ----------------------------------------------------------
    case MenuAction::ImageSize:  g_imageSizeRequested = true;  break;
    case MenuAction::CanvasSize: g_canvasSizeRequested = true; break;

    // --- Image > Adjustments ---------------------------------------------
    //
    // Into `st.requestAdjustment`, not into a file-static -- see that field's
    // comment in app/AppState.hpp. `keymaps/default.json`'s "adjust_levels",
    // "adjust_curves" and "adjust_desaturate" write the identical values from
    // main.cpp, which is the whole point: the chord and the menu item are one
    // path from here on, exactly as `MenuAction::FreeTransform` and Cmd+T
    // already are.
    case MenuAction::AdjustLevels:
      st.requestAdjustment = AdjustmentRequest::Levels;
      break;
    case MenuAction::AdjustCurves:
      st.requestAdjustment = AdjustmentRequest::Curves;
      break;
    case MenuAction::AdjustExposure:
      st.requestAdjustment = AdjustmentRequest::Exposure;
      break;
    case MenuAction::AdjustChannelMixer:
      st.requestAdjustment = AdjustmentRequest::ChannelMixer;
      break;
    case MenuAction::AdjustDesaturate:
      st.requestAdjustment = AdjustmentRequest::Desaturate;
      break;
    case MenuAction::AdjustBrightnessContrast:
      st.requestAdjustment = AdjustmentRequest::BrightnessContrast;
      break;
    case MenuAction::AdjustHueSaturation:
      st.requestAdjustment = AdjustmentRequest::HueSaturation;
      break;
    case MenuAction::AdjustVibrance:
      st.requestAdjustment = AdjustmentRequest::Vibrance;
      break;
    case MenuAction::AdjustColorBalance:
      st.requestAdjustment = AdjustmentRequest::ColorBalance;
      break;
    case MenuAction::AdjustBlackAndWhite:
      st.requestAdjustment = AdjustmentRequest::BlackAndWhite;
      break;
    case MenuAction::AdjustPhotoFilter:
      st.requestAdjustment = AdjustmentRequest::PhotoFilter;
      break;
    case MenuAction::AdjustInvert:
      st.requestAdjustment = AdjustmentRequest::Invert;
      break;
    case MenuAction::AdjustPosterize:
      st.requestAdjustment = AdjustmentRequest::Posterize;
      break;
    case MenuAction::AdjustThreshold:
      st.requestAdjustment = AdjustmentRequest::Threshold;
      break;
    case MenuAction::AdjustGradientMap:
      st.requestAdjustment = AdjustmentRequest::GradientMap;
      break;
    case MenuAction::AdjustAutoTone:
      st.requestAdjustment = AdjustmentRequest::AutoTone;
      break;
    case MenuAction::AdjustAutoContrast:
      st.requestAdjustment = AdjustmentRequest::AutoContrast;
      break;
    case MenuAction::AdjustAutoColor:
      st.requestAdjustment = AdjustmentRequest::AutoColor;
      break;
    case MenuAction::AdjustEqualize:
      st.requestAdjustment = AdjustmentRequest::Equalize;
      break;

    // Not a silent default: a `MenuAction` added to the enum without a body
    // here is a menu item that draws, highlights, clicks and does absolutely
    // nothing -- which is the single worst failure a menu can have, because it
    // is indistinguishable to the user from the feature being broken. Listing
    // the two non-actions explicitly means the compiler names the omission.
    case MenuAction::None:
    case MenuAction::Count:
      break;
  }
}

// ===========================================================================
// The dockable panel system
// ===========================================================================
//
// The user's instruction, in full, because every decision below answers some
// part of it:
//
//   *"revamp the right panel to be dockable, not scrollable, I want to be
//   able to put the parts I want in and have them stay put, and put others in
//   flyout mode or dock around the app"*
//
//   *"All four edges but move the brush setting and the tool pallet to
//   dockable panels as well, this makes the UI modular and customizable"*
//
// What was here before: one `##controls` window on the right, holding thirteen
// `CollapsingHeader`s in a single scroll, plus two separate welded bands (the
// `##tools` palette on the left edge and the options bar under the tab strip)
// that were not panels at all and could not be moved.
//
// What is here now: **four docks, a flyout rail, and fifteen panels that can
// be in any of them.** The model is `app/PanelLayout` (headless, persisted,
// version-1-compatible), the slot arithmetic is `ui/DockLayout` (headless,
// asserted to tile exactly), and this file is the only part that knows what a
// splitter looks like.
//
// --- Why a dock is one window with child slots, and not N windows ---------
//
// Each dock draws ONE ImGui window covering the dock rect, and each panel
// inside it a `BeginChild()` at its slot. The alternative -- one top-level
// window per panel -- was written first and is worse in a way that shows
// immediately: fifteen top-level windows are fifteen entries in ImGui's
// focus/z-order list, and the moment one is clicked it comes forward, so a
// panel could be raised above the splitter that resizes it. Children have no
// independent z-order, which is exactly the property a docked panel wants.
//
// **Every panel draws into the dock's window**, with no exceptions. There used
// to be one -- the OPTIONS panel was `drawAtelierOptionsBar()`, a band that
// opened its own window -- and it did not work: a `Begin()` inside a
// `BeginChild()` is a second top-level window at the same coordinates, so the
// panel drew either behind its own dock or over its neighbours depending on
// focus order. That function is `drawAtelierOptionsBarContent()` now and draws
// into whatever window is current, which deleted the special case rather than
// papering over it.
//
// --- "not scrollable" is a property of the DOCK, not of the panel ----------
//
// The dock never scrolls. Every panel in it gets a slot, and the slots tile
// the dock exactly (ui/DockLayout). A panel whose content is taller than its
// own slot scrolls **inside that slot** -- which is why the child below is
// created without `NoScrollbar`, deliberately, in a file where nearly every
// other child suppresses it. That is the difference between "LAYERS is below
// the fold because HISTOGRAM above it is long" (the complaint) and "LAYERS is
// exactly where I put it and has its own scrollbar" (the fix).
//
// The one exception is disclosed rather than hidden: when the minima cannot
// fit the dock at all (`DockTiling::overflowed`), the slots run past the
// dock's edge and the dock itself is allowed to scroll. See `drawDock()`.

constexpr const char* kPanelMenuPopup = "##panelmenu";

// The flyout rail's width: a strip of one-click launchers along the canvas's
// right edge, one per panel in flyout mode. Defined up here with the other
// panel-chrome constants because the tear-off drag preview needs it too --
// dropping a panel in the middle of the canvas sends it to the rail, and the
// preview has to be able to show where that is.
//
// **34, not the 28 this shipped with, and the six pixels are a measurement.**
// A cell's label is up to `kSectionShortLabelMax` mono characters, which is
// 21 px at this build's face; ImGui left-aligns a label it cannot centre and
// clips it at the button rect, so the old 24 px button (28 minus the rail's
// own 2 px padding either side) cut the last glyph off `GRA` and `GRI` -- the
// two labels the default rail exists to keep apart. Caught on a screenshot,
// which is the only place a clipped glyph shows: the label rule itself is
// `app/ControlsLayout`'s and was, correctly, green throughout.
constexpr float kRailW = 34.0f;
constexpr float kRailPad = 2.0f;
// The cell, derived rather than written twice -- a button sized independently
// of the rail holding it is how the clipping above happened in the first
// place.
constexpr float kRailCell = kRailW - 2.0f * kRailPad;

// The dock side a placement names. Only meaningful for the four dock
// placements; `panelPlacementIsDock()` is the guard, and `Bottom` is the
// default arm rather than an assert because this is called per panel per
// frame and a wrong answer here is a misplaced rect, not a crash.
DockSide dockSideFor(PanelPlacement p) {
  switch (p) {
    case PanelPlacement::Left:  return DockSide::Left;
    case PanelPlacement::Right: return DockSide::Right;
    case PanelPlacement::Top:   return DockSide::Top;
    default:                    return DockSide::Bottom;
  }
}

// The placement a `DockSide` names. `dockSideFor()`'s inverse, and written out
// rather than derived so that adding a fifth placement one day is a compile
// error here instead of a silent mis-drop.
PanelPlacement placementForDockSide(DockSide side) {
  switch (side) {
    case DockSide::Left:  return PanelPlacement::Left;
    case DockSide::Right: return PanelPlacement::Right;
    case DockSide::Top:   return PanelPlacement::Top;
    default:              return PanelPlacement::Bottom;
  }
}

// The strip a tear-off drop would land in, for the drag preview.
//
// When the target dock already exists this is that dock -- the panel joins
// what is there. When it does not, the dock has to be carved out of the
// canvas, and the preview shows the band it would take rather than a phantom
// zero-width strip at the window edge. Showing the real cost of a new dock
// before the drop is the difference between a preview and a guess.
AtelierRect panelDropPreviewRect(const AtelierBands& bands, const DockDropTarget& target) {
  if (!target.isDock) {
    // The flyout: preview the rail's own strip at the canvas's right edge.
    const AtelierRect& c = bands.canvas;
    if (c.empty()) return AtelierRect{};
    return AtelierRect{c.right() - kRailW, c.y, kRailW, c.h};
  }
  const AtelierRect& c = bands.canvas;
  switch (target.side) {
    case DockSide::Left:
      return bands.leftDock.empty() ? AtelierRect{c.x, c.y, kRightColumnW * 0.5f, c.h}
                                    : bands.leftDock;
    case DockSide::Right:
      return bands.rightDock.empty()
                 ? AtelierRect{c.right() - kRightColumnW, c.y, kRightColumnW, c.h}
                 : bands.rightDock;
    case DockSide::Top:
      return bands.topDock.empty() ? AtelierRect{c.x, c.y, c.w, kOptionsBarH} : bands.topDock;
    default:
      return bands.bottomDock.empty()
                 ? AtelierRect{c.x, c.bottom() - kOptionsBarH, c.w, kOptionsBarH}
                 : bands.bottomDock;
  }
}

// Written on every change rather than at quit. A layout is cheap to write and
// there is no moment this app is guaranteed to reach on the way out --
// app/DocumentLifecycle's recent-documents list is persisted the same way and
// for the same reason.
void savePanelLayout(const AppState& st) {
  std::string err;
  if (!st.panels.saveToFile(defaultPanelLayoutFilePath(), &err))
    std::fprintf(stderr, "[panel-layout] save failed: %s\n", err.c_str());
}

// The tool palette's body, flowed into whatever rectangle it has been given.
//
// This used to be the `##tools` window's whole contents, sized from
// `bands.toolPalette.h` -- a band that was always a tall narrow column, so a
// single column of cells was the only arrangement it ever needed. TOOLS is a
// panel now and can be docked to the top or bottom edge, where a single column
// of 18 cells in a 46 px strip is not a layout at all, so the grid flows:
// `atelierToolGrid()` picks the cell size and the column count together from
// both of the panel's dimensions.
//
// **The FG swatch is vertical-only.** It is drawn below the grid, which is
// where docs/ui.md section 2's diagram puts it, and that placement only makes
// sense in a column -- in a 46 px strip there is no "below". So a horizontal
// arrangement omits it rather than squeezing it, and the colour it shows is
// still one click away in the COLOR panel, which is where it is chosen.
void drawToolsPanelBody(AppState& st, float availW, float availH, bool vertical) {
  const float swatchSide = kToolCellMax - 8.0f;
  const float swatchArea = vertical ? kToolSwatchAreaH : 0.0f;
  const float gridH = std::max(0.0f, availH - swatchArea);
  const AtelierToolGrid grid = atelierToolGrid(availW, gridH);

  // ItemSpacing is 6px everywhere else in the chrome (ui/AtelierTheme.cpp) but
  // the grid's own cell size already accounts for every pixel it has -- 6px of
  // spacing *between* cells is gaps the fitting arithmetic never saw, and this
  // is exactly the "buttons are too large" the user once pointed at: the cells
  // themselves were not the (only) problem, the gaps between them were. Zeroed
  // here, scoped to just this child, rather than globally.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

  // `NoScrollbar` for the one case fitting cannot rescue (`grid.overflows`):
  // Dear ImGui's mouse wheel still scrolls a `NoScrollbar` child, so every
  // cell stays reachable, it is only the bar itself -- and the width it would
  // otherwise reserve -- that this suppresses.
  if (ImGui::BeginChild("##toolgrid", ImVec2(0.0f, gridH), 0, ImGuiWindowFlags_NoScrollbar)) {
    // --flyout-demo names a *tool*, not a slot index -- matched by
    // toolGroupIndex() so a reordering of ui/AtelierChrome.hpp's kToolGroups
    // cannot silently point the demo at the wrong group.
    const int brushGroup = toolGroupIndex(Tool::Brush);
    int inRow = 0;
    for (int g = 0; g < kToolGroupCount; ++g) {
      // The flow. `SameLine()` before every cell except the first of a row is
      // what turns the same single-column loop into a grid without a second
      // copy of it -- in a one-column grid the condition is never true and the
      // behaviour is byte-for-byte what it was.
      if (inRow > 0) ImGui::SameLine(0.0f, 0.0f);
      toolGroupButton(st, g, grid.cell, st.openToolFlyoutDemo && g == brushGroup);
      if (++inRow >= grid.columns) inRow = 0;
      // A group rule only reads as a separator in a column. In a flowed grid
      // it would be a stray dash in the middle of a row, so it is drawn only
      // where it means something -- the groups themselves are unchanged either
      // way, and `kToolGroups`' membership is what a user actually navigates
      // by (each cell's flyout still lists its own group).
      if (kToolGroups[g].ruleAfter && grid.columns == 1) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(rp, ImVec2(rp.x + grid.cell, rp.y + kDividerThickness),
                          atelierToken(kDivider));
        ImGui::Dummy(ImVec2(grid.cell, kDividerThickness));
      }
    }
    if (inRow > 0) ImGui::SameLine(0.0f, 0.0f);
    moreToolsButton(grid.cell);
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();  // ItemSpacing, pushed above BeginChild()

  if (!vertical) return;

  // The foreground swatch, below the grid where docs/ui.md section 2's diagram
  // puts `FG/BG` -- drawn after EndChild() precisely so it is *not* part of the
  // scrolling grid above and cannot move with it. Its side is pinned to
  // `kToolCellMax`, not the live cell size, so it does not resize or reflow as
  // the panel resizes.
  //
  // **FG only, no BG.** The pair is Photoshop's, and the second half of it
  // means something only once something fills with it: Fill-with-colour is PRD
  // D26 and the paint bucket D25, both phase 6, and nothing in this build reads
  // a background colour. A second well that no operation consumes would be
  // chrome promising a feature that is not behind it.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  // **`foregroundSrgb()`, not `defaultPalette()[st.brush.pigment]`.** This is
  // the well PRD Q10 says the eyedropper picks *into*, so it has to show the
  // colour that was picked.
  const std::array<float, 3> fg = foregroundSrgb(st.brush);
  dl->AddRectFilled(p, ImVec2(p.x + swatchSide, p.y + swatchSide),
                    IM_COL32((int)(fg[0] * 255), (int)(fg[1] * 255), (int)(fg[2] * 255), 255));
  dl->AddRect(p, ImVec2(p.x + swatchSide, p.y + swatchSide), atelierToken(kRule), 0.0f, 0,
              kRuleThickness);
  ImGui::Dummy(ImVec2(swatchSide, swatchSide));
  ImGui::SetItemTooltip("Foreground: %s\nChosen in the COLOR panel (docs/ui.md section 3.3), "
                      "or picked with the eyedropper (PRD Q10)",
                      foregroundName(st.brush));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
  ImGui::TextUnformatted("FG");
  ImGui::PopStyleColor();
}

// One panel's contents, with no frame of its own.
//
// The switch is what the `##controls` loop's switch used to be, moved here so
// that a dock slot, a flyout and (in future) anything else that wants to show
// a panel all reach the same bodies. `Options` is the one arm that does not
// draw into the caller's window -- see the note at the top of this section.
void drawPanelBody(AppState& st, ControlsSection section, std::unique_ptr<PaintSim>& sim,
                   GpuContext& gpu, const MixboxLut& lut, const AtelierRect& slot,
                   bool vertical) {
  switch (section) {
    case ControlsSection::Tools:
      drawToolsPanelBody(st, slot.w, slot.h, vertical);
      break;
    case ControlsSection::Options:
      // The one panel whose background is not the dock's. It is the design's
      // options band (docs/ui.md section 2 paints it `#201e1d`, a shade deeper
      // than the panels around it), and that reads as a band whether it is
      // welded to the window or docked to an edge -- so the token comes with
      // it rather than being left behind with the geometry.
      {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        dl->AddRectFilled(o, ImVec2(o.x + sz.x, o.y + sz.y), atelierToken(kChromeDeep));
      }
      // `GetWindowHeight()`, not `slot.h`. The rect handed to a panel body is
      // its CONTENT region -- the body padding already subtracted -- and that
      // is the right quantity for a grid that has to fit inside it. It is the
      // wrong one for vertical centring, which has to be measured against the
      // panel's full height or the content lands half the padding too high.
      // That is a 6 px lift, and the golden `toolbar` view is what named it.
      drawAtelierOptionsBarContent(st, ImGui::GetWindowHeight(), g_strokeRefusal);
      break;
    // PLAN.md Phase 5 step 1 ("Multiple layers in `Document`, with reorder,
    // visibility, lock, opacity"; PRD C4).
    case ControlsSection::Layers:       drawLayersSection(st); break;
    // PLAN.md Phase 5 step 8 ("History panel ...", PRD O2/O3).
    case ControlsSection::History:      drawHistorySection(st, sim, gpu); break;
    // PLAN.md Phase 5 step 12 ("Layer comps ...", PRD C14).
    case ControlsSection::Comps:        drawCompsSection(st); break;
    // PLAN.md Phase 3 step 8 ("Op-stack UI -- reorder, toggle, delete, and a
    // curve widget operating in the shaper domain").
    case ControlsSection::Grade:        drawGradeSection(st); break;
    // C2 (docs/reachability-audit.md; PRD D2, P0).
    case ControlsSection::Histogram:    drawHistogramSection(st); break;
    // docs/ui.md section 3.3 / PRD L4.
    case ControlsSection::Color:        drawColorSection(st); break;
    // Two panes, because picking a brush and authoring one are different acts
    // (drawBrushLibrarySection()'s own comment). `gpu` and `lut` for the hover
    // preview alone, which is a real rasterised dab in a real texture.
    case ControlsSection::BrushLibrary: drawBrushLibrarySection(st, gpu, lut); break;
    // `gpu` and `lut` for the TIP PREVIEW alone, for the same reason.
    case ControlsSection::Brush:        drawBrushSection(st, gpu, lut); break;
    case ControlsSection::Pigment:      drawPigmentSection(st); break;
    case ControlsSection::Medium:       drawMediumSection(st, sim.get()); break;
    case ControlsSection::BoardTilt:    drawBoardTiltSection(st); break;
    case ControlsSection::Grid:         drawGridSection(st); break;
    case ControlsSection::Solver:       drawSolverSection(st, sim.get()); break;
  }
}

// Is this section's subject present right now?
//
// The board is a shallow-water idea: only the watercolour solver reads
// `tiltX/tiltY`. A section can be absent when its subject is; it is still in
// the layout, because the layout is where the user put it and not what the app
// currently has to say.
//
// **Distinct from the user hiding it.** That is a placement
// (`PanelPlacement::Hidden`); this is the app reporting it has nothing to
// show. Keeping them separate is what stops one being read as the other.
bool panelHasSubject(const AppState& st, ControlsSection section) {
  if (section == ControlsSection::BoardTilt) return st.mode == PaintMode::Watercolor;
  return true;
}

// A panel's grip: the strip you click to collapse it, right-click to move it,
// and DRAG to tear it off.
//
// ==========================================================================
// What this replaces, and the bug that made it necessary
// ==========================================================================
//
// The first version asked one question -- "does a header fit?" -- and answered
// it with `slot.h >= kPanelHeaderExtent + kPanelMinHeight`, i.e. 26 + 72 = 98
// px. Thirteen panels in a 1180 px right dock give each expanded one 83 px.
// **So every panel in the application lost its header at once**, and a
// collapsed panel -- whose slot is exactly 26 px, and which IS nothing but a
// header -- lost it most of all: nine anonymous grey bars with no triangle, no
// title and no click target, and no way to reopen them.
//
// Two mistakes, both worth naming because they are easy to make again:
//
//  1. **The floor excluded the thing it had to contain.** `kPanelMinHeight`
//     was a body height, so a panel at exactly its floor could never have a
//     header. ui/DockLayout.hpp now defines it as grip + body and static_asserts
//     the relation.
//  2. **The "does it fit" test was applied to a panel that is only a grip.** A
//     collapsed panel does not need room for a grip AND a body; it needs room
//     for a grip, which is what its slot already is.
//
// So the rule is now the opposite one, and it is unconditional: **every panel
// always has a grip.** What varies is the grip's SHAPE, never its existence.
// A panel a person cannot get hold of is a panel they have lost, and that is
// not a state this chrome is allowed to reach.
//
// ==========================================================================
// Two shapes
// ==========================================================================
//
//  * **A bar** across the top of the slot, 26 px, the ordinary case. Shows a
//    disclosure triangle and -- if the slot is wide enough for it -- the
//    panel's title. A narrow slot (the 52 px left dock) drops the title rather
//    than clipping it (app/ControlsLayout.hpp §2's rule, applied to a header)
//    and shows grip dots instead, so the strip still reads as grabbable.
//  * **A rail** down the left edge, 14 px, when the slot is too SHORT to spare
//    26 px of height -- the 46 px top dock holding OPTIONS, where a bar would
//    leave 20 px of controls. A rail costs width instead of height, which a
//    full-width band has to spare and a 46 px one does not.
//
// The rail is why the options bar and the tool palette are grabbable at all.
// The previous revision drew them headerless and left the PANELS menu as the
// only way to reach them, which is exactly the complaint: *"I don't see
// handles to tear off any of the panels like tool settings or the tool bar on
// the left."*
constexpr float kPanelRailW = 14.0f;
// How far the pointer must travel before a press on a grip becomes a tear-off
// rather than a click. Dear ImGui's own default drag threshold is 6 px; a
// grip is a small target and a collapse is cheap to undo, so this is a little
// larger than that to keep an imprecise click from becoming a move.
constexpr float kPanelDragThresholdPx = 8.0f;
// The "?" help button's diameter, and the gap it keeps from the grip's right
// edge -- one shared constant so `panelGripFor()`'s title-fit check and
// `drawPanelGrip()`'s own drawing can never disagree about how much width
// the button costs.
constexpr float kPanelHelpBtnSize = 16.0f;
constexpr float kPanelHelpBtnMargin = 6.0f;

enum class PanelGripKind { Bar, Rail };

struct PanelGripLayout {
  PanelGripKind kind = PanelGripKind::Bar;
  // Bar height, or rail width.
  float extent = kPanelHeaderExtent;
  bool showTitle = true;
};

PanelGripLayout panelGripFor(ControlsSection section, const AtelierRect& slot, bool collapsed) {
  PanelGripLayout g;

  // A collapsed panel IS its grip: the slot is `kPanelHeaderExtent` tall and
  // all of it is the bar. Checked first, because the short-slot test below
  // would otherwise send every collapsed panel to a rail -- which is how the
  // outgoing bug turned a collapse into a disappearance.
  const bool roomForBar = collapsed || slot.h >= kPanelHeaderExtent + kPanelMinBody;
  if (!roomForBar && slot.w >= kPanelRailW + kPanelMinWidth) {
    g.kind = PanelGripKind::Rail;
    g.extent = kPanelRailW;
    g.showTitle = false;
    return g;
  }

  g.kind = PanelGripKind::Bar;
  g.extent = std::min(kPanelHeaderExtent, std::max(1.0f, slot.h));
  const ControlsSectionSpec& spec = controlsSectionSpec(section);
  pushAtelierMono();
  const float titleW = ImGui::CalcTextSize(spec.title).x;
  popAtelierMono();
  // 22 px for the triangle and its gap, 4 px of breathing room after the word
  // -- the same two numbers `drawPanelGrip()` lays the bar out with, so the
  // two cannot disagree about whether the title fits. Sections with a "?"
  // help button (`spec.helpText != nullptr`) also reserve the button's own
  // width, or a tight panel could draw a title running straight into it.
  const float helpReserve =
      spec.helpText != nullptr ? (kPanelHelpBtnSize + kPanelHelpBtnMargin) : 0.0f;
  g.showTitle = slot.w >= 22.0f + titleW + 4.0f + helpReserve;
  return g;
}

// The per-panel context menu: where to send this panel. Opened by right-
// clicking a grip, and drawn from the PANELS menu too, so the two routes offer
// exactly the same moves rather than drifting apart.
//
// Returns true if the layout changed and needs saving.
bool drawPanelPlacementItems(AppState& st, ControlsSection section) {
  bool changed = false;
  const PanelPlacement current = st.panels.placementOf(section);
  struct Row { PanelPlacement placement; const char* label; };
  static constexpr Row kRows[] = {
      {PanelPlacement::Left, "Dock left"},     {PanelPlacement::Right, "Dock right"},
      {PanelPlacement::Top, "Dock top"},       {PanelPlacement::Bottom, "Dock bottom"},
      {PanelPlacement::Flyout, "Flyout"},      {PanelPlacement::Hidden, "Hide"},
  };
  for (const Row& r : kRows) {
    if (!ImGui::MenuItem(r.label, nullptr, current == r.placement)) continue;
    if (r.placement == current) continue;
    st.panels.setPlacement(section, r.placement);
    // Moving a panel out of a flyout closes the flyout, or it would keep
    // hovering over the canvas showing a panel that is now docked.
    if (st.flyoutOpen && st.flyoutSection == section) st.flyoutOpen = false;
    changed = true;
  }
  return changed;
}

// Draw one panel's grip and return the rect its BODY should occupy.
//
// Drawn by hand rather than with `CollapsingHeader` because the open/closed
// state has to live in `app/PanelLayout` -- it is persisted, and it feeds
// `ui/DockLayout`'s slot arithmetic -- and `CollapsingHeader` keeps its own in
// ImGui's ini state where neither can reach it. The tear-off drag is the other
// reason: a header that can start a drag is not a header ImGui ships.
AtelierRect drawPanelGrip(AppState& st, ControlsSection section, const AtelierRect& slot,
                          bool collapsed, bool* layoutChanged) {
  const ControlsSectionSpec& spec = controlsSectionSpec(section);
  const PanelGripLayout g = panelGripFor(section, slot, collapsed);
  const bool bar = g.kind == PanelGripKind::Bar;

  const AtelierRect grip = bar ? AtelierRect{slot.x, slot.y, slot.w, g.extent}
                               : AtelierRect{slot.x, slot.y, g.extent, slot.h};
  const AtelierRect body = bar
                               ? AtelierRect{slot.x, slot.y + g.extent, slot.w,
                                             std::max(0.0f, slot.h - g.extent)}
                               : AtelierRect{slot.x + g.extent, slot.y,
                                             std::max(0.0f, slot.w - g.extent), slot.h};

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(grip.x, grip.y), ImVec2(grip.right(), grip.bottom()),
                    atelierToken(kChromeMid));

  ImGui::SetCursorScreenPos(ImVec2(grip.x, grip.y));
  ImGui::PushID(static_cast<int>(section));
  // Without this, the "?" help button drawn later in this same function --
  // on top of this button, inside its own bounding box -- would never
  // receive a hover or a click: Dear ImGui's default hit-testing gives the
  // FIRST item submitted at a given pixel the hover, not the last (topmost)
  // one, so the whole-bar grip button would silently eat every click in the
  // help button's corner. `SetNextItemAllowOverlap()` is the documented fix
  // (imgui.h: "allow next item to be overlapped by a subsequent item").
  ImGui::SetNextItemAllowOverlap();
  const bool clicked = ImGui::InvisibleButton(
      "##grip", ImVec2(std::max(1.0f, grip.w), std::max(1.0f, grip.h)));
  const bool hovered = ImGui::IsItemHovered();

  // **The tear-off.** A press that travels far enough stops being a click and
  // becomes a move; `drawUI()`'s drop handler decides where it lands.
  if (ImGui::IsItemActive() && !st.panelDragActive &&
      ImGui::IsMouseDragging(ImGuiMouseButton_Left, kPanelDragThresholdPx)) {
    st.panelDragActive = true;
    st.panelDragSection = section;
  }
  // Suppressed while a tear-off is in flight: ImGui reports a press as a click
  // on release however far the pointer travelled, so without this every
  // completed drag would also toggle the panel shut on its way out.
  if (clicked && !st.panelDragActive) {
    st.panels.setCollapsed(section, !collapsed);
    *layoutChanged = true;
  }
  if (hovered || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##panelctx");
  if (ImGui::BeginPopup("##panelctx")) {
    pushAtelierMono();
    ImGui::TextDisabled("%s", spec.title);
    popAtelierMono();
    ImGui::Separator();
    if (drawPanelPlacementItems(st, section)) *layoutChanged = true;
    ImGui::EndPopup();
  }
  ImGui::PopID();

  const uint32_t ink = atelierToken(hovered ? kTextPrimary : kTextSecondary);

  if (bar) {
    // The disclosure triangle, pointing down when open and right when closed --
    // the direction convention every other tree in this chrome uses.
    const float cy = grip.y + grip.h * 0.5f;
    const float tx = grip.x + 8.0f;
    if (collapsed)
      dl->AddTriangleFilled(ImVec2(tx, cy - 4.0f), ImVec2(tx, cy + 4.0f), ImVec2(tx + 6.0f, cy),
                            ink);
    else
      dl->AddTriangleFilled(ImVec2(tx - 1.0f, cy - 2.0f), ImVec2(tx + 9.0f, cy - 2.0f),
                            ImVec2(tx + 4.0f, cy + 4.0f), ink);

    // docs/ui.md section 1: caps labels are monospace. A panel title is the
    // largest caps label in a dock, so it is the one where the face change is
    // most of what tells a grip from the prose under it.
    //
    // **Drawn only if it fits.** app/ControlsLayout.hpp §2 is a whole section
    // on this exact failure -- the column once read "Granulatio", "Edge darke",
    // "Paper slop" and "Working ti" -- and its rule is that a label is never
    // half a word.
    if (g.showTitle) {
      pushAtelierMono();
      const float th = ImGui::GetFontSize();
      dl->AddText(ImVec2(grip.x + 22.0f, cy - th * 0.5f), atelierToken(kTextPrimary), spec.title);
      popAtelierMono();

      // The "?" help button -- context that matters occasionally (what a
      // Pigment vs. RGB colour actually carries, say) rather than on every
      // glance at the panel, so it is one click away instead of a permanent
      // paragraph in the body. Only drawn where there is something to show
      // (`spec.helpText != nullptr`) and only when the title itself fits --
      // `panelGripFor()` already reserved this button's own width as part of
      // that same fit check, so the two can never disagree.
      if (spec.helpText != nullptr) {
        const ImVec2 btnCenter(grip.right() - kPanelHelpBtnMargin - kPanelHelpBtnSize * 0.5f, cy);
        ImGui::PushID(static_cast<int>(section));
        ImGui::PushID("##help");
        ImGui::SetCursorScreenPos(
            ImVec2(btnCenter.x - kPanelHelpBtnSize * 0.5f, btnCenter.y - kPanelHelpBtnSize * 0.5f));
        ImGui::InvisibleButton("##helpBtn", ImVec2(kPanelHelpBtnSize, kPanelHelpBtnSize));
        const bool helpHovered = ImGui::IsItemHovered();
        if (helpHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("##helpPopup");
        const uint32_t helpInk = atelierToken(helpHovered ? kTextPrimary : kTextSecondary);
        dl->AddCircle(btnCenter, kPanelHelpBtnSize * 0.5f - 1.0f, helpInk, 0, kDividerThickness);
        pushAtelierMono();
        const ImVec2 qSize = ImGui::CalcTextSize("?");
        dl->AddText(ImVec2(btnCenter.x - qSize.x * 0.5f, btnCenter.y - qSize.y * 0.5f), helpInk,
                    "?");
        popAtelierMono();
        if (ImGui::BeginPopup("##helpPopup")) {
          ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
          ImGui::TextUnformatted(spec.helpText);
          ImGui::PopTextWrapPos();
          ImGui::EndPopup();
        }
        ImGui::PopID();
        ImGui::PopID();
      }
    } else {
      // No room for the word, so the strip says "grabbable" the way every
      // drag handle does: two rows of dots. Without this a titleless bar is
      // an anonymous grey band, which is precisely what the outgoing bug
      // looked like from the user's side.
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 2; ++j)
          dl->AddRectFilled(ImVec2(grip.right() - 14.0f + j * 4.0f, cy - 4.0f + i * 4.0f),
                            ImVec2(grip.right() - 12.0f + j * 4.0f, cy - 2.0f + i * 4.0f), ink);
    }
  } else {
    // The rail: grip dots down the middle, and the triangle at the top so the
    // collapse affordance reads the same as the bar's.
    const float cx = grip.x + grip.w * 0.5f;
    if (collapsed)
      dl->AddTriangleFilled(ImVec2(cx - 3.0f, grip.y + 5.0f), ImVec2(cx - 3.0f, grip.y + 13.0f),
                            ImVec2(cx + 3.0f, grip.y + 9.0f), ink);
    else
      dl->AddTriangleFilled(ImVec2(cx - 4.0f, grip.y + 6.0f), ImVec2(cx + 4.0f, grip.y + 6.0f),
                            ImVec2(cx, grip.y + 12.0f), ink);
    const float midY = grip.y + grip.h * 0.5f;
    for (int i = 0; i < 3; ++i)
      dl->AddRectFilled(ImVec2(cx - 1.0f, midY - 6.0f + i * 5.0f),
                        ImVec2(cx + 1.0f, midY - 4.0f + i * 5.0f), ink);
  }

  // The tooltip is unconditional: it is the only name a titleless grip has,
  // and on a wide one it still says which panel the pointer is over without
  // the user having to read back up to the bar. A fresh, delayed
  // IsItemHovered() rather than the shared `hovered` above: that bool also
  // sets the cursor and the `ink` colour, and delaying either would make the
  // grip itself feel laggy to hover, not just its tooltip.
  if (!st.panelDragActive && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s -- in the %s\nClick to %s, drag to move it, right-click for the menu",
                      spec.title, panelPlacementKey(st.panels.placementOf(section)),
                      collapsed ? "expand" : "collapse");
  return body;
}

// One dock's tiling, from the model.
//
// **Shared rather than computed inside `drawDock()`**, because the tear-off
// drop handler needs exactly the same rectangles: a pointer inside a dock has
// to resolve to a slot (`dockSlotDropAt`), and it can only do that against the
// tiling that dock actually drew. Recomputed rather than cached in `AppState`
// -- it is a dozen floats over at most fifteen slots, and a cache would be a
// second copy of the layout that could disagree with the first.
DockTiling dockTilingFor(const AppState& st, PanelPlacement placement, const AtelierRect& rect) {
  const DockSide side = dockSideFor(placement);
  const bool vertical = dockStacksVertically(side);
  std::vector<DockSlotSpec> specs;
  for (const PanelSlot& slot : st.panels.slotsIn(placement)) {
    // **A slot's geometry is its FIRST member's**, not its active tab's -- see
    // `PanelSlot` in app/PanelLayout.hpp. Switching tabs must not resize the
    // slot, and a stack collapses as a unit.
    const ControlsSection lead = slot.leader();
    DockSlotSpec spec;
    // A section whose subject is absent still holds its place in the layout
    // but takes no space this frame: it collapses to its grip, so the user can
    // see it is there and see that it has nothing to say, rather than having
    // the dock silently reflow around a panel that vanished. A STACK stays
    // expanded while any of its members has a subject -- collapsing a shared
    // slot because the tab on top went quiet would take the others down with
    // it.
    bool anySubject = false;
    for (const ControlsSection m : slot.members)
      if (panelHasSubject(st, m)) anySubject = true;
    spec.collapsed = st.panels.isCollapsed(lead) || !anySubject;
    spec.weight = st.panels.weightOf(lead);
    spec.minExtent = vertical ? kPanelMinHeight : kPanelMinWidth;
    spec.headerExtent = kPanelHeaderExtent;
    specs.push_back(spec);
  }
  return dockTile(rect, side, specs);
}

// A TAB STACK's grip: the strip of tabs across the top of a shared slot.
//
// The user's instruction: *"tab support for putting multiple panels into a
// stack."* `app/PanelLayout` owns which panels share a slot and which of them
// is on top; `ui/DockLayout` gives the slot its rectangle; this draws the one
// row that lets a person switch between them, and returns the body rect the
// ACTIVE tab's panel gets.
//
// **A stacked slot always gets a bar, even where a lone panel would get a
// rail.** `panelGripFor()`'s rail shape saves height by spending width, which
// is right for a single panel in a 46 px top dock -- but a rail can only carry
// one panel's affordances, and the other tabs would then have no way to be
// reached at all. A stack in a slot too short for a full bar gets a clamped
// one instead: cramped is recoverable, unreachable is not.
AtelierRect drawStackGrip(AppState& st, const PanelSlot& slot, const AtelierRect& rect,
                          bool collapsed, bool* layoutChanged) {
  const float gripH = std::min(kPanelHeaderExtent, std::max(1.0f, rect.h));
  const AtelierRect grip{rect.x, rect.y, rect.w, gripH};
  const AtelierRect body{rect.x, rect.y + gripH, rect.w, std::max(0.0f, rect.h - gripH)};

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(grip.x, grip.y), ImVec2(grip.right(), grip.bottom()),
                    atelierToken(kChromeMid));

  // --- the collapse triangle, which acts on the WHOLE stack ---------------
  //
  // On the stack, not on the active tab: collapsing hides the bodies and keeps
  // the tab strip, so the other members stay reachable. Collapsing only the
  // visible one would leave a slot showing tabs for panels whose state the
  // user cannot see or change, which is a worse answer than either extreme.
  constexpr float kTriW = 20.0f;
  const float cy = grip.y + grip.h * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(grip.x, grip.y));
  ImGui::PushID(9000);
  const bool triClicked =
      ImGui::InvisibleButton("##stackfold", ImVec2(kTriW, std::max(1.0f, grip.h)));
  const bool triHovered = ImGui::IsItemHovered();
  if (triClicked) {
    st.panels.setCollapsed(slot.leader(), !collapsed);
    *layoutChanged = true;
  }
  if (triHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  // Separate from `triHovered`, which also picks the `ink` colour below --
  // gating that on the tooltip's own stationary+delay timer would make the
  // triangle itself feel laggy to hover, not just its tooltip.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s this stack of %d panels", collapsed ? "Expand" : "Collapse",
                      static_cast<int>(slot.members.size()));
  ImGui::PopID();
  {
    const uint32_t ink = atelierToken(triHovered ? kTextPrimary : kTextSecondary);
    const float tx = grip.x + 7.0f;
    if (collapsed)
      dl->AddTriangleFilled(ImVec2(tx, cy - 4.0f), ImVec2(tx, cy + 4.0f), ImVec2(tx + 6.0f, cy),
                            ink);
    else
      dl->AddTriangleFilled(ImVec2(tx - 1.0f, cy - 2.0f), ImVec2(tx + 9.0f, cy - 2.0f),
                            ImVec2(tx + 4.0f, cy + 4.0f), ink);
  }

  // --- the tabs -----------------------------------------------------------
  const float tabsX = grip.x + kTriW;
  const float tabsW = std::max(0.0f, grip.right() - tabsX);
  const size_t n = slot.members.size();
  const float tabW = tabsW / static_cast<float>(n);

  for (size_t i = 0; i < n; ++i) {
    const ControlsSection section = slot.members[i];
    const ControlsSectionSpec& spec = controlsSectionSpec(section);
    const bool isActive = i == slot.activeIndex;
    const AtelierRect tab{tabsX + static_cast<float>(i) * tabW, grip.y, tabW, grip.h};

    ImGui::SetCursorScreenPos(ImVec2(tab.x, tab.y));
    ImGui::PushID(static_cast<int>(section));
    const bool clicked =
        ImGui::InvisibleButton("##tab", ImVec2(std::max(1.0f, tab.w), std::max(1.0f, tab.h)));
    const bool hovered = ImGui::IsItemHovered();

    // A tab tears off exactly like a lone panel's grip does -- same threshold,
    // same drag state, same drop handler. That is the point: a tab is not a
    // different kind of thing from a panel, it is a panel sharing a slot.
    if (ImGui::IsItemActive() && !st.panelDragActive &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, kPanelDragThresholdPx)) {
      st.panelDragActive = true;
      st.panelDragSection = section;
    }
    if (clicked && !st.panelDragActive && !isActive) {
      st.panels.setActiveInStack(section);
      *layoutChanged = true;
    }
    if (hovered || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##tabctx");
    if (ImGui::BeginPopup("##tabctx")) {
      pushAtelierMono();
      ImGui::TextDisabled("%s", spec.title);
      popAtelierMono();
      ImGui::Separator();
      if (ImGui::MenuItem("Unstack into its own slot")) {
        st.panels.unstack(section);
        *layoutChanged = true;
      }
      ImGui::Separator();
      if (drawPanelPlacementItems(st, section)) *layoutChanged = true;
      ImGui::EndPopup();
    }
    ImGui::PopID();

    // The active tab is drawn in the dock's own base tone so it reads as
    // continuous with the body below it, and carries a 2 px accent underline
    // -- the same rule the tab strip over the canvas uses for documents.
    if (isActive)
      dl->AddRectFilled(ImVec2(tab.x, tab.y), ImVec2(tab.right(), tab.bottom()),
                        atelierToken(kChromeBase));
    // An inactive tab gets no hover FILL, deliberately: the theme has no
    // hover-surface token, and inventing a shade here would be a fourth chrome
    // grey that nothing else in the build uses. Hover brightens the label
    // instead, which is exactly what `drawPanelGrip()` already does with its
    // own `ink`.
    if (isActive && !collapsed)
      dl->AddRectFilled(ImVec2(tab.x, tab.bottom() - kRuleThickness),
                        ImVec2(tab.right(), tab.bottom()), atelierToken(kAccent));
    // A hairline between tabs so two inactive ones do not read as one strip.
    if (i + 1 < n)
      dl->AddRectFilled(ImVec2(tab.right() - 1.0f, tab.y + 4.0f),
                        ImVec2(tab.right(), tab.bottom() - 4.0f), atelierToken(kRule));

    // **The full title if it fits, otherwise the short unique label** --
    // `controlsSectionShortLabel()` among this stack's own members, which is
    // the same function and the same guarantee the flyout rail uses. Never a
    // clipped word: app/ControlsLayout.hpp section 2's rule, which a tab strip
    // is the easiest place in this chrome to break, because tab width falls as
    // fast as members are added.
    pushAtelierMono();
    std::string label = spec.title;
    if (ImGui::CalcTextSize(label.c_str()).x > tab.w - 6.0f)
      label = controlsSectionShortLabel(section, slot.members);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    if (ts.x <= tab.w - 2.0f)
      dl->AddText(ImVec2(tab.x + (tab.w - ts.x) * 0.5f, cy - ts.y * 0.5f),
                  atelierToken((isActive || hovered) ? kTextPrimary : kTextSecondary),
                  label.c_str());
    popAtelierMono();

    // A fresh, delayed IsItemHovered() rather than the shared `hovered`
    // above: that bool also sets the cursor and the label colour, and
    // delaying either would make the tab itself feel laggy to hover, not
    // just its tooltip.
    if (!st.panelDragActive && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s -- tab %d of %d\nClick to show it, drag to move it, right-click "
                        "for the menu",
                        spec.title, static_cast<int>(i + 1), static_cast<int>(n));
  }
  return body;
}

// One slot's grip, whichever kind it is. The dock's draw loop calls only this.
AtelierRect drawSlotGrip(AppState& st, const PanelSlot& slot, const AtelierRect& rect,
                         bool collapsed, bool* layoutChanged) {
  if (slot.stacked()) return drawStackGrip(st, slot, rect, collapsed, layoutChanged);
  return drawPanelGrip(st, slot.leader(), rect, collapsed, layoutChanged);
}

// Draw one dock: its background, its panels in their slots, and the splitters
// between them.
void drawDock(AppState& st, PanelPlacement placement, const AtelierRect& dockRect,
              std::unique_ptr<PaintSim>& sim, GpuContext& gpu, const MixboxLut& lut,
              bool* layoutChanged) {
  if (dockRect.empty()) return;
  const std::vector<PanelSlot> slots = st.panels.slotsIn(placement);
  if (slots.empty()) return;

  const DockSide side = dockSideFor(placement);
  const bool vertical = dockStacksVertically(side);
  const DockTiling tiling = dockTilingFor(st, placement, dockRect);

  // The dock's own window: background, splitters, and the panels' children.
  //
  // `NoScrollbar` in the steady state -- that is the whole point of the
  // feature. The overflow case is the single disclosed exception: when the
  // minima cannot fit, the slots really do run past the edge and the dock is
  // allowed to scroll, because the alternative is panels too small to use.
  const ImGuiWindowFlags dockFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
      (tiling.overflowed ? 0 : ImGuiWindowFlags_NoScrollbar);

  char dockId[32];
  std::snprintf(dockId, sizeof(dockId), "##dock_%s", panelPlacementKey(placement));
  ImGui::SetNextWindowPos(ImVec2(dockRect.x, dockRect.y));
  ImGui::SetNextWindowSize(ImVec2(dockRect.w, dockRect.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImGui::ColorConvertU32ToFloat4(atelierToken(kChromeBase)));
  const bool dockOpen = ImGui::Begin(dockId, nullptr, dockFlags);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (!dockOpen) {
    ImGui::End();
    return;
  }

  for (size_t i = 0; i < slots.size() && i < tiling.slots.size(); ++i) {
    const PanelSlot& panelSlot = slots[i];
    // The tab whose body is drawn. For a lone panel that is the panel.
    const ControlsSection section = panelSlot.activeSection();
    const DockSlot& slot = tiling.slots[i];
    const AtelierRect r = slot.rect;

    ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
    // Keyed on the SLOT's leader, not on the active tab: an ImGui id that
    // changes when a tab is switched would hand the body child a new identity
    // every time, discarding its scroll position -- so switching to a tab and
    // back would silently scroll the first one to the top.
    ImGui::PushID(static_cast<int>(panelSlot.leader()));

    // Every panel has a grip, always -- see `drawPanelGrip()` for the bug that
    // rule replaced; a stack gets a tab strip instead. What comes back is the
    // room left for the body.
    const AtelierRect body = drawSlotGrip(st, panelSlot, r, slot.collapsed, layoutChanged);

    if (!slot.collapsed && body.h > 0.0f && body.w > 0.0f) {
      ImGui::SetCursorScreenPos(ImVec2(body.x, body.y));
      char bodyId[48];
      std::snprintf(bodyId, sizeof(bodyId), "##body_%s", controlsSectionKey(section));
      // **No `NoScrollbar` here, deliberately.** This is where "not
      // scrollable" is bought: the DOCK does not scroll, so a panel whose
      // content exceeds its own slot scrolls within itself and never pushes a
      // neighbour off the bottom.
      // The theme's own `WindowPadding`, not a pair invented here. A docked
      // panel's body is the window the tool palette and the controls column
      // each used to be, so it has to inset its content by exactly what those
      // did or every glyph in it lands a couple of pixels off -- which is what
      // the golden `tools` view measured when this was `6.0f`.
      //
      // **`AlwaysUseWindowPadding` is not optional here.** Dear ImGui zeroes
      // `WindowPadding` for a borderless child window unless this flag asks
      // for it, so pushing the style var alone silently does nothing and every
      // panel's content runs flush against the dock's edge -- which is exactly
      // what happened, and what the golden `toolbar` view caught: the options
      // bar's content sat 8 px left of where the welded band had drawn it.
      // **Transparent, so the dock's own background is what shows.** The theme
      // paints `ImGuiCol_ChildBg` a shade deeper than `WindowBg`, which is
      // right for a child that is a distinct surface (the tool grid inside the
      // palette, say) and wrong for one that is only a scroll region. Left at
      // the theme's value it painted the deep tone across the panel's full
      // slot, including the padding margin the old top-level windows left in
      // the base tone -- a 13-level frame the golden `tools` view caught with
      // its zero tolerance.
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kWindowPaddingX, kWindowPaddingY));
      const bool bodyOpen = ImGui::BeginChild(bodyId, ImVec2(body.w, body.h),
                                              ImGuiChildFlags_AlwaysUseWindowPadding);
      // **Popped the instant the child exists, not after its contents.** A
      // style colour applies to every `BeginChild()` made while it is pushed,
      // so leaving it in scope hands the transparency to the panel's OWN
      // children too -- the tool grid's background vanished, and the dock's
      // base tone showed through the gap beside each cell. Scoping it to this
      // one call is the fix; the panel's internals keep the theme's `ChildBg`
      // exactly as they had it when each panel was its own window.
      ImGui::PopStyleColor();
      ImGui::PopStyleVar();
      if (bodyOpen) {
        if (!panelHasSubject(st, section)) {
          // Only reachable when a headerless panel loses its subject; a
          // panel with a header is collapsed instead. Said out loud rather
          // than drawn blank.
          ImGui::TextDisabled("Nothing to show for the current mode.");
        } else {
          const AtelierRect inner{body.x, body.y, ImGui::GetContentRegionAvail().x,
                                  ImGui::GetContentRegionAvail().y};
          drawPanelBody(st, section, sim, gpu, lut, inner, vertical);
        }
      }
      ImGui::EndChild();
    }
    ImGui::PopID();
  }

  // The splitters. Drawn last within the dock so they sit above the panel
  // children's own hit areas -- a splitter that a panel body can steal the
  // mouse from is a splitter that intermittently refuses to drag.
  for (size_t i = 0; i < tiling.splitters.size(); ++i) {
    const AtelierRect& sp = tiling.splitters[i];
    // **Which panels this boundary redistributes between**, which is not
    // necessarily the two it sits between -- see `dockDragPairFor()` for the
    // rule and for the defect that produced it. A collapsed neighbour is
    // ballast, not a wall: the drag reaches past it to the nearest expanded
    // panel, and only a side with no expanded panel at all makes the boundary
    // inert. The previous rule -- both immediate neighbours expanded -- left
    // the DEFAULT dock with zero draggable splitters.
    const DockDragPair pair = dockDragPairFor(tiling.slots, i);
    const bool live = pair.live;
    ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y));
    ImGui::PushID(static_cast<int>(1000 + i));
    if (live)
      ImGui::InvisibleButton("##split", ImVec2(std::max(1.0f, sp.w), std::max(1.0f, sp.h)));
    const bool hovered = live && ImGui::IsItemHovered();
    if (live && (hovered || ImGui::IsItemActive()))
      ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);

    // The 2 px rule drawn down the middle of the 6 px handle, so the splitter
    // looks like every other rule in the chrome and behaves like a grip.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const uint32_t col =
        atelierToken((live && (hovered || ImGui::IsItemActive())) ? kAccent : kRule);
    if (vertical) {
      const float ry = sp.y + (sp.h - kRuleThickness) * 0.5f;
      dl->AddRectFilled(ImVec2(sp.x, ry), ImVec2(sp.x + sp.w, ry + kRuleThickness), col);
    } else {
      const float rx = sp.x + (sp.w - kRuleThickness) * 0.5f;
      dl->AddRectFilled(ImVec2(rx, sp.y), ImVec2(rx + kRuleThickness, sp.y + sp.h), col);
    }

    // `pair`'s indices are into `tiling.slots`, which `dockTilingFor()` builds
    // one-for-one from `slots` -- so the two are parallel and either index is
    // valid in both. Checked rather than assumed because the consequence of
    // that ceasing to be true is an out-of-bounds read, and the check costs a
    // comparison on a frame where the mouse is down.
    const bool pairInRange = pair.indexA < slots.size() && pair.indexB < slots.size();
    if (live && pairInRange && ImGui::IsItemActive()) {
      const float delta = vertical ? ImGui::GetIO().MouseDelta.y : ImGui::GetIO().MouseDelta.x;
      if (delta != 0.0f) {
        const AtelierRect& ra = tiling.slots[pair.indexA].rect;
        const AtelierRect& rb = tiling.slots[pair.indexB].rect;
        const float ea = vertical ? ra.h : ra.w;
        const float eb = vertical ? rb.h : rb.w;
        // The LEADER carries the slot's weight, so it is the leader's weight a
        // drag rewrites -- the same rule `dockTilingFor()` reads it by.
        const ControlsSection sa = slots[pair.indexA].leader();
        const ControlsSection sb = slots[pair.indexB].leader();
        const DockDragResult d =
            dockApplyDrag(ea, eb, st.panels.weightOf(sa), st.panels.weightOf(sb),
                          vertical ? kPanelMinHeight : kPanelMinWidth, delta);
        st.panels.setWeight(sa, d.weightA);
        st.panels.setWeight(sb, d.weightB);
      }
    }
    // Written back on release rather than on every frame of the drag -- a
    // drag is dozens of frames and each would be a file write, an fsync and a
    // rename for a number the user is still choosing.
    if (live && ImGui::IsItemDeactivated()) *layoutChanged = true;
    ImGui::PopID();
  }

  ImGui::End();
}

// The dock's outer edge: drag it to resize the whole dock.
//
// A separate gesture from the splitters above, and deliberately a separate
// piece of state on `AppState` -- the two have different bounds and different
// write-backs, and one bool doing both jobs is how a drag ends up resizing the
// wrong thing.
void drawDockEdge(AppState& st, PanelPlacement placement, const AtelierRect& dockRect,
                  bool* layoutChanged) {
  if (dockRect.empty()) return;
  const bool horizontal = (placement == PanelPlacement::Top || placement == PanelPlacement::Bottom);

  // The grip sits on the side of the dock that faces the canvas.
  AtelierRect grip;
  if (placement == PanelPlacement::Left)
    grip = AtelierRect{dockRect.right(), dockRect.y, kDockSplitterThickness, dockRect.h};
  else if (placement == PanelPlacement::Right)
    grip = AtelierRect{dockRect.x - kDockSplitterThickness, dockRect.y, kDockSplitterThickness,
                       dockRect.h};
  else if (placement == PanelPlacement::Top)
    grip = AtelierRect{dockRect.x, dockRect.bottom(), dockRect.w, kDockSplitterThickness};
  else
    grip = AtelierRect{dockRect.x, dockRect.y - kDockSplitterThickness, dockRect.w,
                       kDockSplitterThickness};

  char id[40];
  std::snprintf(id, sizeof(id), "##dockedge_%s", panelPlacementKey(placement));
  ImGui::SetNextWindowPos(ImVec2(grip.x, grip.y));
  ImGui::SetNextWindowSize(ImVec2(grip.w, grip.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
  if (ImGui::Begin(id, nullptr, flags)) {
    ImGui::InvisibleButton("##grip", ImVec2(std::max(1.0f, grip.w), std::max(1.0f, grip.h)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
      ImGui::SetMouseCursor(horizontal ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive()) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      float extent = horizontal ? dockRect.h : dockRect.w;
      // Which way "bigger" is depends on which edge the dock is attached to.
      switch (placement) {
        case PanelPlacement::Left:   extent += d.x; break;
        case PanelPlacement::Right:  extent -= d.x; break;
        case PanelPlacement::Top:    extent += d.y; break;
        default:                     extent -= d.y; break;
      }
      st.panels.setDockExtent(placement, extent);
    }
    if (ImGui::IsItemDeactivated()) *layoutChanged = true;
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// The flyout rail: a strip of buttons for every panel in flyout mode, and the
// flyout itself.
//
// The user asked for **both** this and the PANELS menu -- "rail plus the
// PANELS menu" -- and the two are not redundant. The rail is one click for a
// panel you reach for often; the menu is the complete list, including panels
// that are nowhere on screen, which is what makes `Hidden` recoverable rather
// than a trap.
//
// The rail is drawn on the canvas's right edge, inside the canvas region, so
// it costs the docks nothing and disappears entirely when no panel is in
// flyout mode.

// ---------------------------------------------------------------------------
// The chrome scrim: the modal dim, with the image held out of it.
//
// ui/AtelierTheme.hpp carries the design and both of the measured mistakes that
// shaped it -- read that before changing anything here, particularly before
// "simplifying" this into a wash inside each chrome window (a dock's panels are
// child windows with their own draw lists and would stay bright) or adding
// `NoBringToFrontOnFocus` to the flags below (that would sink it behind every
// window it exists to cover).
//
// Begun twice per frame: once early, empty, to claim its slot ahead of any
// popup, and once here to draw. `NoInputs` keeps it out of hit-testing -- the
// modal already blocks clicks to the chrome; this only makes that visible.
bool beginChromeScrimWindow() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing;
  return ImGui::Begin("##modalchromescrim", nullptr, flags);
}

// **Geometry is `viewport minus bands.canvas`, as four strips** -- not a list
// of the windows to cover. That is what makes it survive: the title band, the
// tab strip, the status bar and all four docks are greyed because they are
// outside the canvas rect, and a dock the user drags wider stays covered
// without this knowing docks exist.
void drawModalChromeScrim(const AtelierBands& bands) {
  const bool open = beginChromeScrimWindow();
  if (!open || !modalDimActive()) {
    ImGui::End();
    return;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float vx0 = vp->Pos.x, vy0 = vp->Pos.y;
  const float vx1 = vx0 + vp->Size.x, vy1 = vy0 + vp->Size.y;

  const AtelierRect& c = bands.canvas;
  if (c.empty()) {
    // No canvas region at all: nothing to hold out, so the wash is the whole
    // viewport -- which is what ImGui would have drawn.
    washRectForModal(vx0, vy0, vx1, vy1);
  } else {
    washRectForModal(vx0, vy0, vx1, c.y);               // title, tabs, top dock
    washRectForModal(vx0, c.bottom(), vx1, vy1);        // bottom dock, status bar
    washRectForModal(vx0, c.y, c.x, c.bottom());        // left dock
    washRectForModal(c.right(), c.y, vx1, c.bottom());  // right dock
  }
  ImGui::End();
}

void drawPanelRail(AppState& st, const AtelierRect& canvas, bool* layoutChanged) {
  const std::vector<ControlsSection> flyouts = st.panels.sectionsIn(PanelPlacement::Flyout);
  if (flyouts.empty() || canvas.empty()) return;

  // One cell plus its 2 px of vertical spacing per panel, plus the window's
  // own padding top and bottom -- so the strip ends where the last cell does
  // rather than trailing empty chrome over the canvas.
  const float h = std::min(canvas.h, static_cast<float>(flyouts.size()) * (kRailCell + 2.0f) + 8.0f);
  const AtelierRect rail{canvas.right() - kRailW, canvas.y, kRailW, h};

  ImGui::SetNextWindowPos(ImVec2(rail.x, rail.y));
  ImGui::SetNextWindowSize(ImVec2(rail.w, rail.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kRailPad, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImGui::ColorConvertU32ToFloat4(atelierToken(kChromeBase)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoScrollbar;
  if (ImGui::Begin("##panelrail", nullptr, flags)) {
    for (const ControlsSection s : flyouts) {
      const ControlsSectionSpec& spec = controlsSectionSpec(s);
      ImGui::PushID(static_cast<int>(s));
      const bool isOpen = st.flyoutOpen && st.flyoutSection == s;
      if (isOpen)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)));
      // The shortest prefix of the title that no other panel ON THIS RAIL
      // shares -- see `controlsSectionShortLabel()`, which owns the rule and
      // is asserted headlessly. Not a Lucide glyph: there is no icon assigned
      // per panel anywhere in this build, and inventing one mapping here would
      // be a second, undocumented icon table beside ui/AtelierChrome.hpp's
      // real one.
      const std::string label = controlsSectionShortLabel(s, flyouts);
      pushAtelierMono();
      const bool clicked = ImGui::Button(label.c_str(), ImVec2(kRailCell, kRailCell));
      popAtelierMono();
      if (isOpen) ImGui::PopStyleColor();
      if (clicked) {
        // Clicking the open one closes it -- a toggle, so the rail is a way
        // out as well as a way in.
        st.flyoutOpen = !isOpen;
        st.flyoutSection = s;
      }
      ImGui::SetItemTooltip("%s", spec.title);
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##railctx");
      if (ImGui::BeginPopup("##railctx")) {
        if (drawPanelPlacementItems(st, s)) *layoutChanged = true;
        ImGui::EndPopup();
      }
      ImGui::PopID();
    }
    // Last inside the window, so it lands over the cells. This window overlays
    // the canvas by design, so it is not covered by any band's own wash.
    washCurrentWindowForModal();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// The open flyout itself: one panel, floating over the canvas beside the rail.
void drawFlyoutPanel(AppState& st, const AtelierRect& canvas, std::unique_ptr<PaintSim>& sim,
                     GpuContext& gpu, const MixboxLut& lut, bool* layoutChanged) {
  if (!st.flyoutOpen || canvas.empty()) return;
  // A panel moved out of flyout mode while its flyout was open closes it here
  // rather than drawing a docked panel twice.
  if (st.panels.placementOf(st.flyoutSection) != PanelPlacement::Flyout) {
    st.flyoutOpen = false;
    return;
  }

  const float w = std::min(kRightColumnW, std::max(kPanelMinWidth, canvas.w - kRailW - 16.0f));
  const float h = std::min(canvas.h - 16.0f, 420.0f);
  if (w <= 0.0f || h <= 0.0f) return;
  const AtelierRect box{canvas.right() - kRailW - w - 4.0f, canvas.y + 8.0f, w, h};

  ImGui::SetNextWindowPos(ImVec2(box.x, box.y));
  ImGui::SetNextWindowSize(ImVec2(box.w, box.h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImGui::ColorConvertU32ToFloat4(atelierToken(kChromeBase)));
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                                 ImGuiWindowFlags_NoScrollbar;
  const bool open = ImGui::Begin("##flyoutpanel", nullptr, flags);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (open) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRect(ImVec2(box.x, box.y), ImVec2(box.right(), box.bottom()), atelierToken(kRule), 0.0f,
                0, kRuleThickness);
    const AtelierRect fbody =
        drawPanelGrip(st, st.flyoutSection, box, /*collapsed=*/false, layoutChanged);
    ImGui::SetCursorScreenPos(ImVec2(fbody.x, fbody.y));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kWindowPaddingX, kWindowPaddingY));
    const bool fbodyOpen = ImGui::BeginChild("##flyoutbody", ImVec2(fbody.w, fbody.h),
                                             ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (fbodyOpen) {
      const AtelierRect inner{fbody.x, fbody.y, ImGui::GetContentRegionAvail().x,
                              ImGui::GetContentRegionAvail().y};
      drawPanelBody(st, st.flyoutSection, sim, gpu, lut, inner, /*vertical=*/true);
      // Inside the child, not after `EndChild()`: a child window owns its own
      // draw list and renders after its parent's, so a wash added to the parent
      // would sit under everything this body just drew. That is the same trap
      // the dock hit -- ui/AtelierTheme.hpp.
      washCurrentWindowForModal();
    }
    ImGui::EndChild();
    // And the parent, for the grip and border around that child.
    washCurrentWindowForModal();
  }
  ImGui::End();
}

// The "PANELS" affordance and its popup: every panel, where it is, and a way
// to move it.
//
// **This lives in the title bar, outside every dock**, which is what makes
// hiding every panel a legal state rather than a trap -- app/PanelLayout.hpp's
// header says the model deliberately does not force a panel to stay docked,
// and this is the other half of that bargain. An empty window is recoverable
// because the way back in was never in a dock.
//
// docs/ui.md section 2's own diagram has a `panels` control in this band, and
// the comment beside the undo/redo buttons said of it: "Not built: the
// design's `panels` control. There is no panel manager to wire it to, and a
// button that does nothing is worse than a gap." There is one now.
//
// Every mutation is deferred to after the row loop. `moveUp`/`moveDown`
// reorder the very vector being iterated, so applying one inside the loop
// would walk an invalidated sequence -- and the visible symptom (a row drawn
// twice, or skipped) looks like a drawing bug rather than the iterator bug it
// is.
void drawPanelMenu(AppState& st) {
  pushAtelierMono();
  const bool clicked = ImGui::SmallButton("PANELS");
  popAtelierMono();
  ImGui::SetItemTooltip("Where every panel is docked, and how to move it.\n"
                      "Right-click any panel's header for the same menu.");
  if (clicked) ImGui::OpenPopup(kPanelMenuPopup);
  if (!ImGui::BeginPopup(kPanelMenuPopup)) return;

  enum class Act { None, Up, Down, Reset };
  Act act = Act::None;
  ControlsSection target = ControlsSection::Color;
  bool changed = false;

  // Taken by value: a row's own body can reach a mutation of this vector, and
  // a reference would be walking a sequence something below it may reorder.
  // Fifteen entries; the copy is not worth a reader's doubt.
  const std::vector<PanelEntry> entries = st.panels.entries();
  for (size_t i = 0; i < entries.size(); ++i) {
    const PanelEntry& entry = entries[i];
    const ControlsSectionSpec& spec = controlsSectionSpec(entry.section);
    ImGui::PushID(static_cast<int>(i));

    // Disabled at the ends of the panel's OWN placement rather than of the
    // whole list -- see app/PanelLayout::moveUp()'s comment for why an arrow
    // that jumps a panel to another edge of the window is not a reorder.
    const std::vector<ControlsSection> siblings = st.panels.sectionsIn(entry.placement);
    const bool atFront = !siblings.empty() && siblings.front() == entry.section;
    const bool atBack = !siblings.empty() && siblings.back() == entry.section;
    ImGui::BeginDisabled(atFront);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) { act = Act::Up; target = entry.section; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(atBack);
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) { act = Act::Down; target = entry.section; }
    ImGui::EndDisabled();
    ImGui::SameLine();

    pushAtelierMono();
    char label[64];
    std::snprintf(label, sizeof(label), "%-14s %s", spec.title,
                  panelPlacementKey(entry.placement));
    if (ImGui::BeginMenu(label)) {
      popAtelierMono();
      if (drawPanelPlacementItems(st, entry.section)) changed = true;
      pushAtelierMono();
      ImGui::EndMenu();
    }
    popAtelierMono();
    ImGui::PopID();
  }

  ImGui::Separator();
  if (ImGui::SmallButton("Reset to default arrangement")) act = Act::Reset;

  if (act != Act::None) {
    switch (act) {
      case Act::Up:    st.panels.moveUp(target); break;
      case Act::Down:  st.panels.moveDown(target); break;
      case Act::Reset: st.panels.resetToDefault(); st.flyoutOpen = false; break;
      case Act::None:  break;
    }
    changed = true;
  }
  if (changed) savePanelLayout(st);
  ImGui::EndPopup();
}

void drawUI(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH) {
  // First, before any branch can skip it: last frame's cursor request is not
  // this frame's answer. See `g_canvasCursor`'s own comment -- the canvas block
  // that sets it is reachable only on some frames, so a reset that lived beside
  // it would let a crosshair outlive the pointer being over the canvas.
  g_canvasCursor.reset();
  g_canvasBitmapTool.reset();  // same reasoning, ui/ToolCursor.hpp §7

  // Next, and before anything reads the state those actions write: whatever
  // the native menu bar collected since the last frame.
  //
  // A native menu's callback arrives on the AppKit main thread from inside
  // SDL's Cocoa pump -- the same thread as this loop, but at a moment when
  // there is no ImGui frame, no `AppState&` in scope and no canvas size to
  // hand `NewDocument`. So the native backend performs nothing; it enqueues an
  // id, and this is where the id is turned into the action, through the same
  // `performMenuAction()` the ImGui bar below calls directly.
  //
  // **Drained here, at the top, rather than at the end of the frame.** A menu
  // pick that toggles `showGuides` or switches the active document has to be
  // visible in the frame the user sees next, not the one after -- a native
  // menu that appeared to need two clicks would be indistinguishable from one
  // that had dropped the first.
  //
  // Empty every frame on every platform without a native bar, and empty on
  // almost every frame with one; the loop costs one mutex acquisition.
  {
    MenuAction queued = MenuAction::None;
    int queuedParam = 0;
    while (dequeueMenuAction(&queued, &queuedParam))
      performMenuAction(st, queued, queuedParam, canvasW, canvasH);
  }

  const ImGuiViewport* vp = ImGui::GetMainViewport();

  // The document-tabs sub-rect, computed early -- `drawAtelierTabStrip()` has
  // to run from inside the `BeginMainMenuBar()`/`EndMainMenuBar()` block just
  // below (see its own declaration comment for why: it draws into the
  // caller's window now rather than opening one of its own), which is well
  // before `bands` -- the full layout, with the live dock extents -- gets
  // computed further down this function. That is not a problem: `titleBar`
  // and `tabStrip` are the only two `AtelierBands` fields `atelierLayout()`
  // computes before it even looks at `docks`, so asking for them early with
  // the default dock extents gives the identical rect the later, real call
  // will -- this is the same value computed twice, not two different
  // answers.
  const AtelierRect earlyTabStrip =
      atelierLayout(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y, !st.documents.empty()).tabStrip;

  // ------------------------------------------------------------ title bar
  //
  // docs/ui.md section 2's first band: 36 px, wordmark then menus, undo/redo
  // at the right. ImGui fixes the main menu bar's height at `GetFrameHeight()`
  // -- font size plus twice the frame padding -- so the padding is pushed for
  // exactly the `BeginMainMenuBar()` call that reads it and popped
  // immediately, leaving every *menu item* at its normal size. The band is
  // 36 px because the design says 36 px; the items are then centred in it.
  //
  // **This is also the document tab strip's row now.** The user's own words
  // -- "The tab bar and the space where the menus used to live take up too
  // much space, collapse them into one band" -- merged what used to be two
  // stacked bands (this 36 px one and a 34 px one under it) into this single
  // one: `ui/AtelierLayout.hpp`'s `kTitleWordmarkW`/`kTitleControlsW` reserve
  // this band's two ends for the wordmark and for Undo/Redo/PANELS/fps, and
  // `AtelierBands::tabStrip` is the space left over in the middle, drawn by
  // `drawAtelierTabStrip()` -- called near the end of this very
  // `BeginMainMenuBar()`/`EndMainMenuBar()` block, below, straight into this
  // window rather than one of its own. It used to open its own window and
  // draw later in the frame, over the canvas block; see that function's
  // declaration comment (ui/AtelierChrome.hpp) for why sharing this window is
  // now required rather than optional -- a window of its own would sit
  // BEHIND this one once the two overlap in screen space, painted over and
  // invisible. Nothing here had to change to make room for it -- the
  // wordmark and the right-aligned cluster below were already nowhere near
  // the row's centre.
  // ---- the menu model (ui/MenuModel) --------------------------------------
  //
  // Built, published and handed to the native backend **before**
  // `BeginMainMenuBar()`, and deliberately outside the `if (menuBarOpen)`
  // below. `BeginMainMenuBar()` can return false -- a viewport too short to
  // hold the bar is enough -- and on the frames where it does, the ImGui bar
  // has nothing to draw but the *native* bar still exists and still has to be
  // told what the menus are. Publishing from inside that branch would leave
  // the menu at the top of the screen frozen on whatever it last saw.
  //
  // `menuContextFromState()` reads the live application, `buildMenuModel()`
  // shapes the tree, `drawMenuNodes()` (inside, below) draws it, and
  // `performMenuAction()` is the only thing that acts. ui/MenuModel.hpp's
  // header comment carries the argument for the split.
  const std::vector<MenuNode> menus = buildMenuModel(menuContextFromState(st));
  publishMenuModel(menus);
  updateNativeMenuBar();

  const float titleBarPad = (kTitleBarH - ImGui::GetFontSize()) * 0.5f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, titleBarPad));
  const bool menuBarOpen = ImGui::BeginMainMenuBar();
  ImGui::PopStyleVar();
  if (menuBarOpen) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         (kTitleBarH - ImGui::GetFrameHeight()) * 0.5f);

    // docs/ui.md section 6: "substitute the naturalPaint wordmark in the menu
    // bar". The wireframe's "ATELIER 2D" is not adopted -- the project keeps
    // its name, and the codename appears in this build only as the module
    // prefix for the chrome that implements the design.
    ImGui::TextUnformatted("naturalPaint");
    ImGui::SameLine(0.0f, 14.0f);

    // ---- the menus, from ui/MenuModel ------------------------------------
    //
    // Seven `BeginMenu()` blocks and forty-one `ImGui::MenuItem()` call sites
    // used to be written out here, each of them declaring an item and
    // performing its action in the body of an `if`. What is left is a backend
    // that draws a tree built above, and it contains no action at all.
    //
    // **On macOS the menus are drawn once, at the top of the screen, and not
    // here.** The alternative -- draw them in both bars -- was rejected on
    // sight: two menu bars a centimetre apart, both live, both showing File,
    // is not a thing any Mac application does, and a user who found a
    // discrepancy between them would have no way to know which one was
    // authoritative.
    //
    // **The band itself stays**, and does not collapse to nothing. It is
    // reserved by `atelierLayout()` (ui/AtelierLayout.cpp, `b.titleBar`,
    // `kTitleBarH` = 36 px from docs/ui.md section 2) and it carries four
    // things that are not menus and have nowhere else to go: the naturalPaint
    // wordmark, the one-line document status (documents-empty only, see
    // below), the Undo and Redo buttons the design puts at the right of this
    // band, and the frame rate -- plus, now, the document tabs the band below
    // used to hold. Reclaiming the 36 px would delete Undo and Redo from the
    // chrome to remove a strip that is not actually dead -- so what macOS
    // reclaims is the *menus'* share of the band, and the band keeps doing
    // its other jobs.
    if (!nativeMenuBarInstalled()) {
      for (const MenuNode& menu : menus) {
        // --open-layer-menu: the same id `BeginMenu()` below opens on a click,
        // so the menu can be photographed. See AppState::openLayerMenu. It
        // stays a special case here rather than becoming a field on the model,
        // because it is a screenshot fixture and not a property of the menu --
        // an NSMenu has no equivalent and should not be given one.
        //
        // **Which means the fixture does nothing once the native bar is
        // installed**, and that is stated rather than hidden: a native menu
        // opens in the system bar, outside this process's window, and
        // `app/Screenshot` photographs the window. The flag is still accepted
        // and still works on Linux and Windows. `tools/golden/run_golden.sh`
        // does not use it -- its six views are --demo-document (used twice,
        // once for `toolbar` and once for the `titlebar` view added by F2),
        // --ui-layer-demo, --pigment-stroke-demo, --marquee-demo and
        // --flyout-demo -- so no golden capture depends on it.
        if (st.openLayerMenu && menu.label == "Layer") ImGui::OpenPopup("Layer");
        if (ImGui::BeginMenu(menu.label.c_str(), menu.enabled)) {
          drawMenuNodes(st, menu.children, canvasW, canvasH);
          ImGui::EndMenu();
        }
      }
    }

    // The active document's name used to be here, with a `*` dirty marker,
    // and the argument for it was that "a Save whose target is not on screen
    // is how the wrong file gets overwritten". That argument is now the tab
    // strip's -- it shows every open document by name with an accent dot on
    // the dirty one, which is strictly more of what the name was here to
    // provide. Two copies of it would be the design's own redundancy, not a
    // safeguard.
    OpenDocument* activeForBar = st.documents.active();
    // **Only drawn with no documents open.** Before the tab strip merged into
    // this row it sat in a band of its own, so a transient message here
    // (`setDocumentStatusLine()`, e.g. "12 files dropped") never competed
    // with anything for the space. Now the middle of this row is
    // `bands.tabStrip`, drawn later in the frame and on top -- see this
    // function's earlier comment -- so a message here while documents are
    // open would either draw underneath the tab strip's own background and
    // vanish, or draw into the wordmark's reserved width and clip. Neither is
    // an improvement on the tab strip's own dirty-dot and tooltip, which is
    // why this only fires when there is no tab strip to compete with.
    if (!g_docStatus.empty() && st.documents.empty()) {
      const size_t firstLine = g_docStatus.find('\n');
      const std::string oneLine =
          firstLine == std::string::npos ? g_docStatus : g_docStatus.substr(0, firstLine);
      ImGui::TextDisabled("| %s", oneLine.c_str());
      ImGui::SetItemTooltip("%s", g_docStatus.c_str());
    }

    // Right-aligned: undo / redo, where docs/ui.md section 2 draws them.
    //
    // The zoom, the dimensions and the working space that used to sit here
    // have moved to the status bar, which is where the design puts them and
    // which did not exist until now. The frame rate stays -- it is not in the
    // design at all, and it is here rather than in the status bar for exactly
    // that reason: it is instrumentation, and it should be the first thing
    // removed when the title bar needs the room.
    //
    // docs/ui.md section 2's `panels` control, which this comment used to say
    // was "not built: there is no panel manager to wire it to, and a button
    // that does nothing is worse than a gap." There is a panel manager now
    // (`app/PanelLayout`), and this is the button. It is drawn just before
    // Undo/Redo, which is the order the design's own diagram has:
    // `undo redo`, then the panels control.
    History* titleHistory = activeForBar != nullptr ? &activeForBar->history : nullptr;
    char status[64];
    // docs/reachability-audit.md F2: frozen to a fixed string on a
    // `--screenshot` run -- see AppState::screenshotCliActive.
    if (st.screenshotCliActive) {
      std::snprintf(status, sizeof(status), "-- fps");
    } else {
      std::snprintf(status, sizeof(status), "%.1f fps",
                    st.frameMs > 0.0f ? 1000.0f / st.frameMs : 0.0f);
    }
    const float panelsW = ImGui::CalcTextSize("PANELS").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float undoW = ImGui::CalcTextSize("Undo").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float redoW = ImGui::CalcTextSize("Redo").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float statusW = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - panelsW - undoW - redoW - statusW - 52.0f);
    drawPanelMenu(st);
    ImGui::SameLine();
    // D1: routed through `moveHistoryCursor()`, the same function the
    // HISTORY panel's pair, the Edit menu's Undo/Redo and ⌘Z/⇧⌘Z all call --
    // one implementation, four callers, so the title bar cannot drift from
    // what a keystroke does.
    ImGui::BeginDisabled(titleHistory == nullptr || !titleHistory->canUndo());
    if (ImGui::SmallButton("Undo")) moveHistoryCursor(st, sim, gpu, *activeForBar, -1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(titleHistory == nullptr || !titleHistory->canRedo());
    if (ImGui::SmallButton("Redo")) moveHistoryCursor(st, sim, gpu, *activeForBar, +1);
    ImGui::EndDisabled();
    ImGui::SameLine(ImGui::GetWindowWidth() - statusW - 12.0f);
    ImGui::TextDisabled("%s", status);

    // The document tabs, drawn into this same window -- see
    // `drawAtelierTabStrip()`'s declaration comment for why it takes no
    // window of its own any more. It only ever reads `bands.tabStrip`, so a
    // bands value with nothing else filled in is exactly as good as the real
    // one -- and `earlyTabStrip` rather than `bands.tabStrip` because `bands`
    // itself (with the live dock extents) is not computed until later in
    // this function; see this variable's own comment above for why that is
    // the same rect regardless.
    AtelierBands tabBands;
    tabBands.tabStrip = earlyTabStrip;
    if (drawAtelierTabStrip(st, tabBands, g_split, &g_docStatus)) {
      // **The same dialog File > New raises, not a second way to make a
      // document.** See this call's own comment where it used to live, a few
      // hundred lines below, for the full argument -- unchanged by the move.
      requestNewDocumentDialog();
      g_docStatus.clear();
    }
    ImGui::EndMainMenuBar();
  }

  // PLAN.md Phase 2 step 12, PRD Q5: "guide at a numeric or percentage
  // position" via a small popup opened from the View menu's "Add Guide..."
  // item -- doesn't need to be elaborate, just a position field and an
  // orientation choice. Defined here, outside BeginMainMenuBar/EndMainMenuBar
  // (popups are identified by a global string ID, not nested inside the ID
  // stack of whatever widget opened them), so it renders regardless of which
  // menu is currently open.
  //
  // The `OpenPopup()` used to be the menu item's own body, and it is a flag
  // now (`MenuAction::AddGuide` -> `g_addGuideRequested`, ui/MenuModel.hpp's
  // `MenuEffect::Deferred`). The global string ID is what let the old
  // arrangement work at all; it would not have survived a native menu bar,
  // whose callback runs on the AppKit main thread with no ImGui frame in
  // progress -- `OpenPopup()` from there writes to a context that is not
  // between NewFrame() and Render(). Export As and Export Comps had already
  // taken this route for the neighbouring ID-stack reason; this is the third.
  if (g_addGuideRequested) {
    g_addGuideRequested = false;
    ImGui::OpenPopup("AddGuidePopup");
  }
  if (ImGui::BeginPopup("AddGuidePopup")) {
    static int orientationIdx = 0;  // 0 = Horizontal, 1 = Vertical
    static char posBuf[32] = "50%";
    ImGui::TextUnformatted("Add Guide");
    ImGui::Separator();
    ImGui::RadioButton("Horizontal", &orientationIdx, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Vertical", &orientationIdx, 1);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Position", posBuf, sizeof(posBuf));
    ImGui::TextDisabled("e.g. 512 or 50%%");
    const bool horizontal = orientationIdx == 0;
    // A Horizontal guide's position is along the document's height (it sits
    // at a fixed Y); a Vertical guide's is along its width (a fixed X) --
    // same axis pairing app/Snapping.hpp's parseGuidePosition() doc comment
    // spells out. `canvasDimensionsFor()` (naturalPaint canvasdim fix), not
    // `canvasW`/`canvasH` directly: this popup is a second, independent call
    // site outside the canvas block below, and reading the fixed solver-
    // canvas constants here instead of the open document was this exact bug
    // in a second place -- a "50%" guide on an 800x1200 document used to
    // land at Y=512 (half of 1024), not Y=600 (half of the document it was
    // actually being drawn on).
    const CanvasDimensions guideDims = canvasDimensionsFor(st.documents.active(), canvasW, canvasH);
    const float axisExtent = horizontal ? guideDims.h : guideDims.w;
    const auto parsed = parseGuidePosition(posBuf, axisExtent);
    if (ImGui::Button("Add") && parsed) {
      st.guides.push_back(
          Guide{horizontal ? GuideOrientation::Horizontal : GuideOrientation::Vertical, *parsed});
      ImGui::CloseCurrentPopup();
    }
    if (!parsed) {
      ImGui::SameLine();
      ImGui::TextDisabled("(enter a number or a percentage)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  // Claim the chrome scrim's slot in `g.Windows`, before the first dialog is
  // begun and with nothing drawn into it. A window's slot is fixed when it is
  // created (imgui.cpp:7077) and the scrim's whole job depends on sitting above
  // the chrome and below the dialog -- so it has to be created before any popup
  // window is, and a modal can already be open on frame 1 (--open-layer-
  // properties, or the crash-recovery prompt at launch). ui/AtelierTheme.hpp
  // carries the full argument and the measurement that forced it. The rects go
  // in during the second Begin, after the chrome; a window may be begun more
  // than once in a frame and the second pass appends without moving it.
  beginChromeScrimWindow();
  ImGui::End();

  // docs/testing-issues.md T9 ("New Document has no size dialog"), out here
  // for the same ID-stack reason as every dialog below: a popup opened from a
  // menu item has to be begun outside the menu bar's ID stack.
  drawNewDocumentDialog(st);

  // PLAN.md Phase 4 step 7 ("Export As"), defined out here for the same
  // reason the Add Guide popup above is: a popup opened from a menu item has
  // to be begun outside the menu bar's ID stack.
  drawExportAsDialog(st, canvasW, canvasH);

  // PLAN.md Phase 5 step 13 ("Export comps to files, and layers to files"),
  // out here for the same ID-stack reason.
  drawExportStatesDialog(st);

  // PLAN.md Phase 4 step 8 ("Document lifecycle"), out here for the same
  // reason: a modal opened from a menu item must be begun outside the menu
  // bar's ID stack.
  drawDocumentDialogs(st);

  // PLAN.md Phase 4 step 9 ("the recovery journal"), same placement rule
  // again -- and it must be begun here rather than only from the menu,
  // because PRD O8's offer happens on launch, before any menu is touched.
  drawRecoveryDialog(st);

  // docs/reachability-audit.md C1: the Filter menu's seven dialogs (the
  // original four plus emboss/median/motion blur) and the Image menu's two,
  // out here for the identical ID-stack reason as every dialog above.
  drawGaussianBlurDialog(st);
  drawSharpenDialog(st);
  drawUnsharpMaskDialog(st);
  drawAddNoiseDialog(st);
  drawEmbossDialog(st);
  drawMedianDialog(st);
  drawMotionBlurDialog(st);
  drawAdjustmentDialogs(st);
  drawImageSizeDialog(st);
  drawCanvasSizeDialog(st);
  drawNumericTransformDialog(st, gpu);
  // docs/reachability-audit.md C5 (PRD E4/E8/E9): the Select menu's five
  // refine dialogs, same placement rule again.
  drawSelectMenuDialogs(st);

  // ------------------------------------------------------------ the bands
  //
  // docs/ui.md section 2, computed by ui/AtelierLayout from the *full*
  // viewport rather than from `WorkPos`/`WorkSize`: the title bar above is a
  // band of the design too, and ImGui shrinks the work area by the main menu
  // bar it drew itself. Taking the full viewport means one function owns every
  // edge in the window, including the one the menu bar sits on -- and that
  // function is testable without a window (see `--selftest`, atelier chrome).
  // The tab strip appears when there is a document to name. Not "when there
  // is more than one": a single open document still has a name, a dirty
  // marker and a `+` beside it. It costs no other band anything either way --
  // `AtelierBands::tabStrip` is nested inside the title row rather than a
  // band of its own now, so a document opening or closing never moves
  // anything below it.
  // T12: read the user's arrangement on the first frame that draws the chrome,
  // not at startup -- see AppState::panelsLoaded. A missing file is the
  // ordinary first-run case and yields the default arrangement, so there is
  // nothing to report and no error branch here.
  //
  // **Before `atelierLayout()`, not after.** The dock extents are an input to
  // the band arithmetic now, so a layout read one frame late would lay the
  // whole window out against the defaults for that frame and then jump.
  if (!st.panelsLoaded) {
    st.panelsLoaded = true;
    st.panels.loadFromFile(defaultPanelLayoutFilePath(), nullptr);
    // --panel-stack-demo, applied AFTER the load and never written back --
    // see `AppState::panelStackDemo`. Two stacks, deliberately: an expanded
    // one whose tab strip is the thing being photographed, and a collapsed one
    // beside it, because "a collapsed stack still shows its tabs" is the rule
    // that keeps the other members reachable and is the one most likely to be
    // broken by a later change to the grip code.
    if (st.panelStackDemo) {
      st.panels.stackWith(ControlsSection::Histogram, ControlsSection::Color);
      st.panels.stackWith(ControlsSection::Grade, ControlsSection::Color);
      st.panels.setActiveInStack(ControlsSection::Color);
      st.panels.stackWith(ControlsSection::Comps, ControlsSection::History);
      st.panels.setCollapsed(ControlsSection::History, true);
    }
  }
  // `effectiveDockExtents()`, not `dockExtents()`: a dock holding no panels is
  // not on screen at all, whatever extent it remembers. That rule is the
  // model's (app/PanelLayout.hpp) so a test can assert it without a window.
  const PanelDockExtents pd = st.panels.effectiveDockExtents();
  AtelierDockExtents dockExtents;
  dockExtents.left = pd.left;
  dockExtents.right = pd.right;
  dockExtents.top = pd.top;
  dockExtents.bottom = pd.bottom;
  const AtelierBands bands = atelierLayout(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y,
                                           /*showTabStrip=*/!st.documents.empty(), dockExtents);

  // Any dock, splitter or header gesture below sets this; it is written back
  // once, after every dock has drawn. One write per frame that changed
  // something, rather than one per gesture -- several can fire in a frame (a
  // collapse and a splitter release, say) and each would otherwise be its own
  // file write, fsync and rename.
  bool panelLayoutChanged = false;

  const ImGuiWindowFlags fixedFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

  // ------------------------------------------------------------- the docks
  //
  // Four calls where there used to be two hand-placed windows (`##tools` on
  // the left, `##controls` on the right) and one welded band (the options bar,
  // drawn far below with the rest of the chrome). Every panel in the
  // application now goes through the same path: `app/PanelLayout` says which
  // dock it is in, `ui/DockLayout` says where in that dock, and `drawDock()`
  // draws it. See this file's "dockable panel system" section for the design.
  //
  // Order matters, but only for one reason: a dock's window is created here
  // and the canvas's is created below, and `fixedFlags` carries
  // `NoBringToFrontOnFocus`, so creation order IS z-order. Docks first means a
  // flyout drawn after the canvas can float above it.
  drawDock(st, PanelPlacement::Top, bands.topDock, sim, gpu, lut, &panelLayoutChanged);
  drawDock(st, PanelPlacement::Left, bands.leftDock, sim, gpu, lut, &panelLayoutChanged);
  drawDock(st, PanelPlacement::Right, bands.rightDock, sim, gpu, lut, &panelLayoutChanged);
  drawDock(st, PanelPlacement::Bottom, bands.bottomDock, sim, gpu, lut, &panelLayoutChanged);

  // The label column, measured and reported (UI detour step 3, problem 1b).
  // Printed when it changes rather than every frame: it settles on the first
  // frame that has drawn every open panel once, and moves again only if a
  // panel that opens later carries a wider label. A log line is what makes "no
  // label is clipped" a number rather than a look at a screenshot.
  //
  // **Neither side of this merge compiled on its own**, which is why the
  // resolution is a third thing rather than a choice. D0 moved the state this
  // reads (`g_labelColumn` and friends) into ui/LabelledControl.cpp as
  // file-static, so main's inline `printf` no longer had the symbols; and D0's
  // replacement call passed `kControlsW`, which the dockable-panel revision
  // deleted (see this file's own note on it) because the right-hand column
  // stopped being a fixed width. So: D0's factoring -- this is the one call
  // into that module from outside its own six functions -- carrying main's
  // number, `bands.rightDock.w`, the dock extent the user actually drags.
  //
  // The branch's 93-line controls-column loop that also sat in this conflict
  // is deliberately NOT here: it is the pre-dock single column, and the four
  // `drawDock()` calls above are what replaced it.
  reportLabelColumnIfChanged(bands.rightDock.w, bands.rightDock.w);

  // ------------------------------------------------------------ canvas
  //
  // PRD **A5**: the band divides into at most two panes. Both halves have to
  // agree before it does -- `atelierSplitPanes()` refuses a canvas too small
  // to hold two usable panes, and `atelierPaneDocuments()` refuses a session
  // with nothing to put in the second one -- so `splitActive` is the two
  // answers together and everything below reads that rather than the mode.
  //
  // **The focused pane is this window**, at a smaller rect. That is the whole
  // change to the block below: rulers, guides, the navigator, pan, zoom,
  // rotate and painting are unmoved, and they are unmoved because the
  // unfocused pane deliberately has none of them (see where it is drawn,
  // after this window's End()).
  const AtelierPaneDocuments paneDocs = atelierPaneDocuments(st.documents, g_split);
  const AtelierPanes panes = atelierSplitPanes(bands.canvas, g_split.mode);
  const bool splitActive = panes.count == 2 && paneDocs.count == 2;
  const AtelierRect focusedRect = splitActive ? panes.pane[paneDocs.focusedPane] : bands.canvas;
  const ImVec2 canvasPos(focusedRect.x, focusedRect.y);
  const ImVec2 canvasSize(focusedRect.w, focusedRect.h);
  ImGui::SetNextWindowPos(canvasPos);
  ImGui::SetNextWindowSize(canvasSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  // The canvas surround (PRD **L6**), and the one place this chrome
  // deliberately disagrees with the wireframe it implements.
  //
  // docs/ui.md section 1's warning callout: the design painted this `#2d2b2b`,
  // the same near-black as the panels, and "simultaneous contrast makes paint
  // read lighter and more saturated against a near-black surround than it
  // truly is, which is precisely the judgement a painting application must not
  // distort." So the surround is its own value, defaulting to mid-grey, and it
  // is deliberately NOT `ImGuiCol_WindowBg` -- every other window in the frame
  // stays chrome, and only this one is the neutral you judge colour against.
  ImGui::PushStyleColor(ImGuiCol_WindowBg,
                        ImGui::ColorConvertU32ToFloat4(atelierToken(atelierSurround())));
  if (ImGui::Begin("##canvas", nullptr, fixedFlags)) {
    const ImVec2 fullAvail = ImGui::GetContentRegionAvail();
    // The active document's own size, not `canvasW`/`canvasH` (main.cpp's
    // fixed `kCanvasW`/`kCanvasH`) -- naturalPaint canvasdim bug: a
    // non-square document used to display square because every one of this
    // block's downstream computations (fit-to-window, `drawSize`, the
    // `ViewTransform`, the corner quad, the navigator, the zoom anchor,
    // hit-testing, the grid overlay and guides -- all listed in
    // `canvasDimensionsFor()`'s own comment) read `texW`/`texH`, and
    // `texW`/`texH` read the two compile-time constants instead of the
    // document. `st.documents.active()` rather than `paneDocs`'s focused
    // pane: `atelierPaneDocuments()`'s own contract (ui/AtelierChrome.hpp,
    // "the focused pane always shows the session's active document")
    // guarantees the two are always the same `OpenDocument*`, so this reads
    // the session directly rather than through a second lookup that could
    // only ever agree with it. `canvasW`/`canvasH` survive only as the
    // "no document open" fallback `canvasDimensionsFor()` itself takes --
    // this call is the single place that decision gets made; nothing below
    // re-derives it.
    const CanvasDimensions canvasDims = canvasDimensionsFor(st.documents.active(), canvasW, canvasH);
    const float texW = canvasDims.w;
    const float texH = canvasDims.h;

    // PLAN.md Phase 2 step 12: rulers reserve a thin strip along the top and
    // left of this window when shown, so the paintable area shrinks by
    // exactly that much. `avail`/`paintOrigin` below replace the old
    // `avail`/`canvasPos` everywhere else in this block that computes
    // paint-area geometry (fit-to-window, drawSize/origin, the zoom anchor,
    // the paint hit-test rect) -- at st.showRulers == false (the default),
    // rulerThickness == 0 so paintOrigin == canvasPos and avail ==
    // fullAvail == canvasSize exactly, meaning this step changes nothing
    // about the existing pan/zoom/paint arithmetic unless rulers are on.
    const float rulerThickness = st.showRulers ? kRulerThickness : 0.0f;
    const ImVec2 avail(fullAvail.x - rulerThickness, fullAvail.y - rulerThickness);
    const ImVec2 paintOrigin(canvasPos.x + rulerThickness, canvasPos.y + rulerThickness);

    // --- fit to window / 100% (PRD Q1) -- consumed here, not where the
    // menu item or key fired, because both need `avail`: the canvas
    // window's actual on-screen size, which only exists inside this
    // Begin()/End() block. Same request-then-consume shape as
    // requestClear/requestReload below. Both re-centre (panX/panY = 0) --
    // "fit" and "100%" both mean "show me the whole thing squarely," and
    // rotation/mirror are untouched: those are independent view toggles,
    // not reset by a zoom command.
    if (st.requestFitWindow) {
      st.view.zoom = clampViewZoom(std::min(avail.x / texW, avail.y / texH));
      st.view.panX = 0.0f;
      st.view.panY = 0.0f;
      st.requestFitWindow = false;
    }
    if (st.requestZoom100) {
      st.view.zoom = 1.0f;
      st.view.panX = 0.0f;
      st.view.panY = 0.0f;
      st.requestZoom100 = false;
    }

    // `drawSize`/`origin` are exactly the pre-step-11 computation --
    // unrotated, unmirrored, zoom/pan only. Kept as-is (rather than folded
    // into the view matrix) specifically so wheel-zoom's cursor-anchoring
    // below, which reads `origin` and `canvasPos`, keeps behaving exactly
    // as it did before this step -- the regression bar PLAN.md step 11
    // asks for. Mirror and rotation are layered on top of this quad's own
    // *centre* (`pivotScreen` below), not baked into `origin` itself.
    const ImVec2 drawSize(texW * st.view.zoom, texH * st.view.zoom);
    const ImVec2 origin(
        paintOrigin.x + std::max(0.0f, (avail.x - drawSize.x) * 0.5f) + st.view.panX,
        paintOrigin.y + std::max(0.0f, (avail.y - drawSize.y) * 0.5f) + st.view.panY);

    // One view matrix (PLAN.md Phase 2 step 11): mirror and rotation pivot
    // around the canvas's own centre -- the point zoom/pan alone would
    // already draw the unrotated, unmirrored quad's centre at. See
    // app/ViewTransform.hpp/.cpp for why toScreen()/toCanvas() are
    // guaranteed to be exact inverses of each other rather than two
    // independently hand-derived formulas, and its .cpp's comment for the
    // algebra proving this reduces to today's plain `origin + p*zoom` when
    // mirror/rotation are at identity.
    const ImVec2 pivotScreen(origin.x + drawSize.x * 0.5f, origin.y + drawSize.y * 0.5f);
    const ViewTransform xform(st.view, Vec2{texW * 0.5f, texH * 0.5f},
                              Vec2{pivotScreen.x, pivotScreen.y});
    const Vec2 xc00 = xform.toScreen(Vec2{0.0f, 0.0f});
    const Vec2 xc10 = xform.toScreen(Vec2{texW, 0.0f});
    const Vec2 xc11 = xform.toScreen(Vec2{texW, texH});
    const Vec2 xc01 = xform.toScreen(Vec2{0.0f, texH});
    const ImVec2 q00(xc00.x, xc00.y), q10(xc10.x, xc10.y), q11(xc11.x, xc11.y),
        q01(xc01.x, xc01.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Drop shadow so the sheet reads as paper lying on a dark desk. A fixed
    // screen-space offset applied to the already-transformed quad, not
    // mapped through the transform itself -- a shadow shouldn't mirror or
    // rotate along with the paper; real light doesn't.
    dl->AddQuadFilled(ImVec2(q00.x + 6, q00.y + 6), ImVec2(q10.x + 6, q10.y + 6),
                      ImVec2(q11.x + 6, q11.y + 6), ImVec2(q01.x + 6, q01.y + 6),
                      IM_COL32(0, 0, 0, 110));
    // AddImageQuad, not AddImage: AddImage can only place an axis-aligned
    // rect, which has no way to express a flipped or rotated quad. This is
    // the one drawing change mirror/rotation actually needed -- everything
    // else is the transform feeding it different corner points.
    // Precedence chain, most-specific-preview-wins: grade, then
    // grayscale, then the plain canvas -- a deliberate narrow-scope
    // choice (PLAN.md Phase 3 step 6). Composing grade and grayscale
    // together isn't a normal use case and isn't required by PLAN.md's
    // Verify criterion for either step, so they stay mutually exclusive
    // by this precedence rather than layered.
    const bool gradeActive = st.view.grade && sim;
    const bool grayscaleActive = !gradeActive && st.view.grayscale && sim;
    if (sim) {
      const WGPUTextureView tv = gradeActive     ? sim->gradedView()
                                 : grayscaleActive ? sim->grayscaleView()
                                                    : sim->canvasView();
      // Not AddImageQuad: sim/PaintSim's canvas is linear light in an
      // RGBA8Unorm texture, and ImGui's pipeline would present it with the
      // wrong transfer function. ui/CanvasQuad owns that conversion.
      addCanvasQuad(dl, tv, q00, q10, q11, q01);
    } else {
      // 1.4 / ADR-0001: no PaintSim exists yet (nothing painted this
      // session), so there is no composite to show. A flat blank-paper
      // quad reads as "ready to paint" rather than a rendering glitch --
      // the first stroke below constructs the sim and this becomes the
      // real canvasView() from the very next frame.
      dl->AddQuadFilled(q00, q10, q11, q01, IM_COL32(250, 250, 247, 255));
    }

    // --- The open document, over the paper (UI detour step 2) -------------
    //
    // The same quad, drawn second, so the document composites over whatever
    // the paper is -- PaintSim's canvas when a stroke has constructed one,
    // the flat blank sheet when nothing has. **A new document is fully
    // transparent**, so this call changes nothing about the picture until a
    // layer holds content; that is this step's regression boundary and it is
    // checkable in a screenshot.
    //
    // ui/DocumentTexture.hpp owns the argument for all three of the
    // decisions behind this one line -- RGBA16Float rather than 8-bit,
    // straight alpha rather than premultiplied because of ImGui's global
    // blend state, and the revision cache that keeps an unchanged frame free.
    //
    // The document is drawn on the *canvas* quad, and (naturalPaint
    // canvasdim fix, `canvasDimensionsFor()`) that quad is now sized to the
    // document's own `texW`/`texH`, not a fixed 1024x1024 -- so the document
    // draws at its own aspect ratio, not stretched onto a square.
    //
    // **The paper underneath it can still be stretched, and that is the
    // known remaining gap, not an oversight.** `sim::PaintSim`'s canvas
    // texture is a fixed size set once, at whichever call to
    // `ensurePaintSim()` first constructs it (below, at the first
    // Watercolour/Oil stroke) -- `canvasDimensionsFor()`'s own comment has
    // the full argument for why recreating it on every later resize is out
    // of this track's scope. So a document painted, then resized via Canvas
    // Size, then painted with Watercolour/Oil again shows its wet paint
    // stretched to the document's new aspect while the dry layer pixels
    // above it (this call) are not -- the two pictures visibly disagreeing
    // is the honest symptom of one texture staying the old size under a
    // quad that now correctly follows the new one, and it stops being a
    // question at all once the stroke bridge makes the document the canvas.
    //
    // No warnings are collected: `compositeDocumentPremultiplied()` would
    // report an unimplemented blend once per layer per upload, and the layers
    // panel already marks exactly those layers `(!)` on their own rows, which
    // is the same fact in the place a user can act on it.
    const OpenDocument* activeDocument = st.documents.active();

    // --- Free Transform (Cmd+T / Edit > Free Transform) ---------------------
    //
    // Serviced here, ahead of everything that reads the session, because two
    // separate things downstream ask "is a transform live on this canvas?"
    // and both must get the answer that includes THIS frame's request: the
    // composite decision immediately below (which layers to hide, see
    // ui/TransformCompositeSplit) and the input claim further down. Sitting
    // ahead of every tool block is the half that was always load-bearing --
    // the gizmo claims the mouse first, so a drag meant to move the box
    // cannot also lay down a stroke. That failure would be silent in the
    // worst way: the picture moves AND gains a brush mark, and only one of
    // the two is undoable as the user expects.
    //
    // It was serviced ~230 lines further down until the composite split
    // needed the answer earlier. Everything it touches is `st` and `gpu`; the
    // `xform` this comment used to claim it needed is not read by any line of
    // it.
    if (st.requestFreeTransform) {
      st.requestFreeTransform = false;
      OpenDocument* od = st.documents.active();
      const std::optional<size_t> li = od != nullptr ? activeLayerIndex(*od) : std::nullopt;
      if (od == nullptr || !li) {
        g_docStatus = "Free Transform needs an open document with a layer.";
      } else {
        // A selection transforms the pixels under it; no selection transforms
        // the whole layer. Photoshop's own rule, and the one a user who has
        // just drawn a marquee will expect -- the alternative (always the
        // whole layer) would silently ignore a selection they made on purpose.
        const TransformBeginResult began =
            od->selection ? st.transform.beginSelectionPixels(*od, *od->selection, *li)
                          : st.transform.beginLayer(*od, *li);
        // Refusals are shown, never swallowed: `beginLayer`/
        // `beginSelectionPixels` refuse a locked layer, an empty one and a
        // Pigment selection-transform BY NAME (app/TransformSession.hpp), and
        // a menu item that appeared enabled and then did nothing at all is
        // the defect docs/reachability-audit.md is named after.
        if (!began.ok) g_docStatus = began.error;
        // T14: the live pixel preview's ONE upload for this whole session --
        // never from the drag loop below, which only ever moves WHERE this
        // already-uploaded texture is drawn (`pending()` changing the quad's
        // four corners), never what it holds. A no-op on `!began.ok` (the
        // session stayed inactive), which `beginTransformPreview()` checks
        // itself rather than this call site re-deriving it.
        beginTransformPreview(st, gpu);
      }
    }

    // ===== Tool::Move -- BEGIN: last frame's pen-up, and the nudge =========
    //
    // Serviced HERE, above the composite decision, for the reason
    // `g_moveCommitPending`'s own declaration gives: a commit is only free of
    // a one-frame hole in the picture if the pixels land before this frame
    // decides which layers to hide. The drag itself is claimed several
    // hundred lines below, with the other tools.
    if (g_moveCommitPending) {
      g_moveCommitPending = false;
      g_moveDragging = false;
      OpenDocument* od = st.documents.active();
      if (od == nullptr || !st.transform.active() || st.transform.documentId() != od->id) {
        // The document went away, or the user switched tabs between pen-up
        // and here. Dropped rather than committed: `commit()` would refuse a
        // foreign document anyway (app/TransformSession.hpp), and a session
        // left live with no gizmo and no drag behind it is worse than none.
        st.transform.cancel();
        g_transformPreview.reset();
      } else if (g_moveDx == 0.0f && g_moveDy == 0.0f) {
        // A click with no drag is not an edit. `commit()` would treat the
        // identity as a no-op and record nothing, so this is only saving the
        // status line from announcing a move that did not happen.
        st.transform.cancel();
        g_transformPreview.reset();
      } else {
        const TransformCommitResult done = st.transform.commit(*od);
        g_docStatus = done.ok ? (done.exact != ExactRemap::None
                                     ? "Moved -- lossless, no resampling."
                                     : "Moved.")
                              : done.error;
        // Only on success, matching the Free Transform block above: a refusal
        // leaves `active()` true, and the session then simply becomes an
        // ordinary Free Transform gizmo the user can adjust and commit with
        // Return -- so the preview they are still looking at has to survive.
        if (done.ok) g_transformPreview.reset();
      }
    }
    // Arrow-key nudge (app/MoveTool.hpp section 4). Only while Move is the
    // active tool and no transform session is live -- with a session up, the
    // arrows would be fighting the gizmo for the same keys. Guarded on
    // `WantTextInput` because these are bare keys and the layer-rename box one
    // panel over needs them, which is app/Keymap's own rule for unmodified
    // keys stated in the gizmo's Return/Escape comment below.
    if (toolMovesPixels(st.brush.tool) && !st.transform.active() &&
        !ImGui::GetIO().WantTextInput) {
      // 10 px for Shift is the conventional big nudge; the step lives here
      // rather than in app/MoveTool because it is a UI convention, not part
      // of what a move means.
      const float step = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
      float nx = 0.0f;
      float ny = 0.0f;
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) nx -= step;
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) nx += step;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) ny -= step;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) ny += step;
      if (nx != 0.0f || ny != 0.0f) {
        if (OpenDocument* od = st.documents.active()) {
          const TransformCommitResult done = nudgeMove(*od, nx, ny);
          // Refused out loud, exactly as a drag's begin is -- a locked layer
          // swallowing an arrow key is the same silent no-op.
          if (!done.ok) g_docStatus = done.error;
        }
      }
    }
    // ===== Tool::Move -- END ===============================================

    // --- Free Transform: the three-way canvas -------------------------------
    //
    // ui/TransformCompositeSplit's own header carries the argument; what is
    // decided here is only which of three arrangements this frame draws:
    //
    //   `split`    layers below -> the preview quad -> layers above. The
    //              transformed layer's pixels appear once, at their new
    //              position, with the layers that belong in front of them in
    //              front of them.
    //   `hide`     layers below AND above in one texture, preview quad over
    //              the top. Taken when the split would not be exact (a blend
    //              or an adjustment above reads the backdrop) or when there
    //              is simply nothing above to draw, which is the common case
    //              of transforming the top layer and costs one texture, not
    //              two.
    //   neither    no live transform: exactly the code path this block had
    //              before any of this existed.
    //
    // Both live arrangements hide the transformed layer in the composite, so
    // the canvas stops showing the picture twice -- the original standing
    // still underneath the moving copy -- which is the defect being fixed.
    // The document is NOT modified to achieve it; see the header.
    const size_t transformLayer = st.transform.layerIndex();
    const bool transformOnThisDoc = st.transform.active() && activeDocument != nullptr &&
                                    st.transform.documentId() == activeDocument->id &&
                                    transformLayer < activeDocument->document.layers.size();
    const bool transformSplitDraws =
        transformOnThisDoc && anyVisibleLayerAbove(activeDocument->document, transformLayer) &&
        transformSplitIsExact(activeDocument->document, transformLayer);

    // The two hidden-layer views, rebuilt only when the document, its
    // revision or the transformed layer actually changes -- NOT every frame.
    // A `Document` copy shares its tile storage (copy-on-write slots) but
    // still copies each layer's tile MAP, which on a large document is real
    // work to repeat 60 times a second for a picture that has not changed.
    // A drag changes only where the quad lands, so this cache is hit for
    // every frame of one.
    struct TransformSplitViews {
      DocumentId id = 0;
      uint64_t revision = 0;
      size_t layerIndex = static_cast<size_t>(-1);
      bool split = false;
      bool valid = false;
      OpenDocument below;  // layers strictly below (split), or all but one (hide)
      OpenDocument above;  // layers strictly above; unused when `split` is false
    };
    static TransformSplitViews views;
    if (!transformOnThisDoc) {
      // Give the copies back the moment the session ends. Two canvas-sized
      // documents' worth of tile maps is not something to hold for a session
      // because one transform happened in it.
      if (views.valid) views = TransformSplitViews{};
    } else if (!views.valid || views.id != activeDocument->id ||
               views.revision != activeDocument->revision ||
               views.layerIndex != transformLayer || views.split != transformSplitDraws) {
      views = TransformSplitViews{};
      views.id = activeDocument->id;
      views.revision = activeDocument->revision;
      views.layerIndex = transformLayer;
      views.split = transformSplitDraws;
      views.below.id = activeDocument->id;
      views.below.revision = activeDocument->revision;
      views.below.document =
          transformSplitDraws
              ? documentWithLayersAtOrAboveHidden(activeDocument->document, transformLayer)
              : documentWithLayerHidden(activeDocument->document, transformLayer);
      if (transformSplitDraws) {
        views.above.id = activeDocument->id;
        views.above.revision = activeDocument->revision;
        views.above.document =
            documentWithLayersAtOrBelowHidden(activeDocument->document, transformLayer);
      }
      views.valid = true;
    }

    // Dedicated textures rather than `g_documentTextures`: the pool holds
    // exactly `kVisibleDocumentCap` slots chosen BY DOCUMENT ID, and both
    // halves here share the active document's id. Routing them through it
    // would make the split pane and the navigator fight the transform for the
    // same slot every frame.
    //
    // **What is and is not given back.** Created lazily, so a session that
    // never opens Free Transform allocates neither. The `views` cache above
    // IS released when the session ends -- that is the large CPU part, two
    // documents' worth of tile maps. These two GPU textures are NOT: like
    // every other texture in this codebase they live for the process
    // (ui/DocumentTexture.hpp decision 5 and `release()`'s own comment on why
    // handing a view back while ImGui may still hold a bind group for it is
    // not safe). So a session that uses Free Transform once carries two
    // canvas-sized RGBA16F textures for the rest of its life -- 2 x 32 MiB on
    // a 2048x2048 document, on top of PRD A6's own figure. That is a real
    // cost and it is stated here rather than discovered later.
    static DocumentTexture transformBelowTexture;
    static DocumentTexture transformAboveTexture;
    // Distinct, and carrying the layer index: two transforms on two different
    // layers of an unedited document are the same {id, revision} and would
    // otherwise hit each other's cached composite.
    const uint64_t belowVariant = 1u + static_cast<uint64_t>(transformLayer) * 2u;
    const uint64_t aboveVariant = 2u + static_cast<uint64_t>(transformLayer) * 2u;

    // Declared out here rather than inside the block that computes it: the
    // above-half of a split transform is drawn much later, from the gizmo
    // block, and must make the same viewport-priority request this frame's
    // main composite did rather than silently asking for the whole backlog.
    DocumentTextureViewport docViewport{};

    WGPUTextureView documentView = nullptr;
    if (activeDocument != nullptr) {
      // ui/DocumentTexture.hpp decision 6: which part of the document is
      // actually on screen this frame, in document-pixel space -- so a big
      // off-screen edit (a full-document filter, a layer toggle covering
      // most of a large document) does not make THIS frame wait on tiles
      // nobody can currently see. The AABB of the paint area's four corners,
      // mapped back through the same `xform` the canvas quad itself was
      // built from (screen -> canvas) -- axis-aligned under a rotated view,
      // the identical "honest at a glance, wrong only in the corners"
      // compromise the navigator rect above already makes, for the same
      // reason: a rectangle that is slightly too generous costs a few extra
      // tiles this call, never a stale pixel, while one that is too small is
      // still safe (see ui/DocumentTexture.hpp's own margin) and merely one
      // frame slower to catch up.
      const Vec2 vp00 = xform.toCanvas(Vec2{paintOrigin.x, paintOrigin.y});
      const Vec2 vp10 = xform.toCanvas(Vec2{paintOrigin.x + avail.x, paintOrigin.y});
      const Vec2 vp11 = xform.toCanvas(Vec2{paintOrigin.x + avail.x, paintOrigin.y + avail.y});
      const Vec2 vp01 = xform.toCanvas(Vec2{paintOrigin.x, paintOrigin.y + avail.y});
      const float vpMinX = std::min(std::min(vp00.x, vp10.x), std::min(vp11.x, vp01.x));
      const float vpMaxX = std::max(std::max(vp00.x, vp10.x), std::max(vp11.x, vp01.x));
      const float vpMinY = std::min(std::min(vp00.y, vp10.y), std::min(vp11.y, vp01.y));
      const float vpMaxY = std::max(std::max(vp00.y, vp10.y), std::max(vp11.y, vp01.y));
      docViewport = DocumentTextureViewport{
          static_cast<int32_t>(std::floor(vpMinX)), static_cast<int32_t>(std::floor(vpMinY)),
          static_cast<int32_t>(std::ceil(vpMaxX)), static_cast<int32_t>(std::ceil(vpMaxY))};

      // T15's live preview, checked first: a Filter dialog with a preview
      // computed for THIS document takes precedence over the real, unfiltered
      // texture -- `filterPreviewViewFor()` returns nullptr on every other
      // frame (no dialog open, a different document active, or a refused
      // preview), in which case this is exactly the one line it replaced.
      // Nothing here can leave a document showing filtered pixels after its
      // dialog closes: the four `drawXDialog()` functions clear
      // `g_filterPreview` the very frame their popup stops being open, before
      // this code runs again.
      documentView = filterPreviewViewFor(gpu, *activeDocument);
      if (documentView == nullptr && transformOnThisDoc && views.valid) {
        // The transformed layer is hidden in this composite. Its pixels are
        // drawn by the gizmo block's quad instead, at the position the drag
        // has taken them to.
        documentView =
            transformBelowTexture.viewFor(gpu, views.below, nullptr, &docViewport, belowVariant);
      }
      if (documentView == nullptr)
        documentView = g_documentTextures.viewFor(gpu, *activeDocument, nullptr, &docViewport);
      addCanvasQuad(dl, documentView, q00, q10, q11, q01);
    }
    dl->AddQuad(q00, q10, q11, q01, ImGui::GetColorU32(ImGuiCol_Border));

    // --- navigator (docs/ui.md section 2) --------------------------------
    //
    // The same composite texture the canvas just drew, at thumbnail size in
    // the bottom-right corner, with the visible region marked in the accent.
    // Free: `DocumentTexture::viewFor()` is revision-cached, so the second
    // call in a frame is a map lookup and no second composite happens.
    //
    // Drawn into the canvas window rather than as its own floating window,
    // because it floats over the *surround* -- a window would take focus and
    // would have to be excluded from the canvas hit test.
    //
    // The viewport rectangle is derived from `origin`/`drawSize`/`avail`, the
    // same three the canvas block computed for itself, rather than from a
    // second reconstruction of the same arithmetic. It is axis-aligned, so
    // under a rotated view it marks the *bounding box* of what is visible
    // rather than the rotated quad -- honest at a glance and wrong only in the
    // corners, which is the same compromise drawRulers() makes for the same
    // reason.
    const AtelierRect navBox =
        st.showNavigator && documentView != nullptr
            ? atelierNavigatorRect(focusedRect, texW, texH)
            : AtelierRect{};
    if (!navBox.empty()) {
      const ImVec2 navMin(navBox.x, navBox.y);
      const ImVec2 navMax(navBox.right(), navBox.bottom());
      dl->AddRectFilled(ImVec2(navMin.x + 4, navMin.y + 4), ImVec2(navMax.x + 4, navMax.y + 4),
                        IM_COL32(0, 0, 0, 110));
      // Paper, not chrome: the document composites with straight alpha, so an
      // unpainted region is transparent and takes whatever is behind it. On
      // the canvas that is the paper quad, and a navigator backed by chrome
      // deep would show black where the canvas shows white -- a thumbnail that
      // does not match the picture it is a thumbnail of.
      dl->AddRectFilled(navMin, navMax, atelierToken(kCanvasPaper));
      addCanvasImage(dl, documentView, navMin, navMax);

      const float visX0 = (paintOrigin.x - origin.x) / st.view.zoom;
      const float visY0 = (paintOrigin.y - origin.y) / st.view.zoom;
      const AtelierRect vis = atelierNavigatorMap(navBox, texW, texH, visX0, visY0,
                                                  visX0 + avail.x / st.view.zoom,
                                                  visY0 + avail.y / st.view.zoom);
      if (!vis.empty())
        dl->AddRect(ImVec2(vis.x, vis.y), ImVec2(vis.right(), vis.bottom()),
                    atelierToken(kAccent), 0.0f, 0, kRuleThickness);
      dl->AddRect(navMin, navMax, atelierToken(kRule), 0.0f, 0, kRuleThickness);
    }

    ImGui::SetCursorScreenPos(paintOrigin);
    ImGui::InvisibleButton("##canvasHit", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    // Read here rather than at the cursor decision far below, because
    // `IsItemActive()` speaks about the item most recently submitted and by
    // then that is a ruler, a guide or nothing at all -- this is the only line
    // in the block where it still means `##canvasHit`.
    //
    // It exists so a drag that STARTED on the canvas keeps the canvas cursor
    // when the pointer runs off the sheet, which is the ordinary way to pan:
    // grab the paper and throw it. `hovered` alone goes false the moment the
    // pointer crosses onto a panel, and the pan cursor would flicker back to an
    // arrow mid-gesture while the view was still moving under it.
    const bool canvasHeld = ImGui::IsItemActive();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Pen input maps back through the transform's actual inverse (docs/
    // shortcuts.md section 3) -- not a second, independently re-derived
    // "un-zoom, un-rotate, un-mirror" formula. This is what keeps painting
    // landing in the right place under a mirror or a rotation: `tx`/`ty`
    // feed the exact same stroke/paint-tool logic below that they always
    // did, in canvas texel space, regardless of what the view is doing.
    const Vec2 canvasMouse = xform.toCanvas(Vec2{mouse.x, mouse.y});
    const float tx = canvasMouse.x;
    const float ty = canvasMouse.y;


    // Everything below reads this rather than `st.transform.active()` so the
    // gizmo's own `commit()`/`cancel()` further down cannot change the answer
    // half way through one frame's input handling.
    //
    // **`documentId()` is part of the answer, not a refinement of it.** A
    // transform session outlives a document switch -- nothing cancels it, and
    // there is no reason it should, since switching back ought to find the
    // gizmo where it was left. But it belongs to ONE document, and this canvas
    // block draws whichever document is active now. Without this comparison a
    // session begun in document A drew its box and its uploaded pixel preview
    // over document B, claimed B's mouse for handles that describe A's layer,
    // and offered a Return that `commit()` (which now refuses it) would have
    // applied to B. Treated as "not active here" rather than cancelled, so the
    // work survives switching away and back.
    // `transformOnThisDoc`, computed once at the top of this block where the
    // composite decision needed it. Reused rather than re-derived so the gate
    // on the input and the gizmo cannot drift from the gate that decided
    // which composite the canvas is showing -- if those two ever disagreed,
    // the transformed layer would be hidden with no preview drawn over it, or
    // drawn twice.
    const bool transformActive = transformOnThisDoc;
    if (transformActive) {
      // Handle sizes are fixed on SCREEN and converted to document space by
      // the view's own zoom, so a handle stays the same size under the finger
      // at 12% and at 1600%. `st.view.zoom` is the transform's uniform length
      // scale -- rotation and mirror both preserve length -- which is why one
      // divide is exact here rather than approximate, the same argument the
      // guide-snap radius above already makes.
      const float zoom = std::max(st.view.zoom, 0.01f);
      const float handleRadiusDoc = kTransformHandleHitPx / zoom;
      const float rotateReachDoc = kTransformRotateReachPx / zoom;

      // ---- input, claimed before any tool sees it -------------------------
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const TransformHandle grabbed = st.transform.hitTest(
            Point2{tx, ty}, handleRadiusDoc, rotateReachDoc);
        // A click on nothing is NOT a commit and NOT a cancel -- it is
        // nothing. Photoshop commits on a click outside; this build does not,
        // deliberately: a mis-aimed click that bakes a resample the user was
        // still adjusting is unrecoverable in the way an extra keystroke
        // never is. Return commits, Escape cancels, and both are stated in
        // the status line for as long as the session is live.
        if (grabbed != TransformHandle::None) st.transform.beginDrag(grabbed, Point2{tx, ty});
      }
      if (st.transform.dragging()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          // Modifiers read live, per frame, not latched at grab time -- so
          // Shift pressed half way through a corner drag starts constraining
          // immediately instead of needing the drag restarted. That is
          // app/TransformSession's own contract and this is the call that
          // depends on it.
          st.transform.updateDrag(Point2{tx, ty}, ImGui::GetIO().KeyShift,
                                  ImGui::GetIO().KeyAlt);
        } else {
          st.transform.endDrag();
        }
      }

      // ---- commit and cancel ----------------------------------------------
      //
      // Read with `ImGui::IsKeyPressed()` rather than through app/Keymap:
      // Return and Escape are bare keys, and app/Keymap's own rule (and
      // MenuKeyEquivalent's) is that an unmodified key must not be claimed
      // globally, because it would be swallowed out of every text field in
      // the application -- the layer-rename box one panel over included.
      // Here the claim is scoped to a live transform session, which is
      // exactly the scope that makes it safe.
      if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        // Nothing was written, so there is nothing to unwind -- see
        // app/TransformSession.hpp's "cancel needs no restore step".
        st.transform.cancel();
        g_transformPreview.reset();  // T14: this session's uploaded crop is dead with it.
      } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                 ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        if (OpenDocument* od = st.documents.active()) {
          const TransformCommitResult done = st.transform.commit(*od);
          g_docStatus = done.ok ? (done.exact != ExactRemap::None
                                       ? "Transform applied -- lossless, no resampling."
                                       : "Transform applied.")
                                : done.error;
          // Only on success: a refusal (a locked layer that got locked mid-
          // drag, a collapsed matrix) leaves `active()` true so the user can
          // adjust and try again (this header's own "On refusal ... active()
          // stays true" contract), and the preview they are still looking at
          // has to survive that -- resetting it here would blank the picture
          // out from under a drag the user has not given up on.
          if (done.ok) g_transformPreview.reset();
        }
      }
    }

    // --- rulers + drag-to-create guides (PRD Q5) -- ruler hit regions live
    // in the band `avail`/`paintOrigin` carved out above, so they never
    // overlap ##canvasHit's paint/pan/rotate rect; only exist at all when
    // st.showRulers is on ("drag-to-create... only meaningful once rulers
    // exist and are visible", per this step's own scope note).
    if (rulerThickness > 0.0f) {
      drawRulers(dl, xform, st.view, canvasPos, paintOrigin, avail, rulerThickness);

      ImGui::SetCursorScreenPos(ImVec2(paintOrigin.x, canvasPos.y));
      ImGui::InvisibleButton("##rulerTop", ImVec2(avail.x, rulerThickness));
      if (ImGui::IsItemActivated())
        st.pendingGuide = Guide{GuideOrientation::Horizontal, canvasMouse.y};

      ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, paintOrigin.y));
      ImGui::InvisibleButton("##rulerLeft", ImVec2(rulerThickness, avail.y));
      if (ImGui::IsItemActivated())
        st.pendingGuide = Guide{GuideOrientation::Vertical, canvasMouse.x};
    }

    // While a guide drag is in progress, update its (possibly snapped)
    // position every frame the mouse stays down; on release, commit it into
    // st.guides -- always, even if it landed outside [0,texW]/[0,texH] or
    // was barely dragged off the ruler (the numeric/percentage popup allows
    // any value too; "Clear Guides" in the View menu is the way back).
    // view.zoom is the transform's uniform length scale (rotation and
    // mirror both preserve length), so dividing the fixed screen-space
    // snap radius by it converts to document-space exactly, not
    // approximately -- the snap feels the same size on screen at any zoom.
    if (st.pendingGuide) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float snapThresholdDoc =
            st.snappingEnabled ? (kSnapThresholdPx / std::max(st.view.zoom, 0.01f)) : 0.0f;
        const SnapResult snap = resolveSnap(canvasMouse, st.guides, st.gridSpacing,
                                            st.gridSubdivisions, texW, texH, snapThresholdDoc);
        st.pendingGuide->position = st.pendingGuide->orientation == GuideOrientation::Horizontal
                                        ? snap.point.y
                                        : snap.point.x;
      } else {
        st.guides.push_back(*st.pendingGuide);
        st.pendingGuide.reset();
      }
    }

    // --- rotate view on drag (PRD Q4: "R" held + drag) -- resolved before
    // the pan/zoom blocks below since, like Hand-tool panning, it claims
    // the left-mouse-drag gesture; the two must agree on who wins. Not
    // routed through app/Keymap's resolve(): that dispatcher fires once per
    // discrete SDL_EVENT_KEY_DOWN, which has no way to express "held," so
    // this reads live key state directly the same way this file's own
    // tiltPad()/panning already read live mouse state instead of going
    // through a discrete action. `⇧R` (reset) *is* a discrete action --
    // see main.cpp's "reset_rotation" dispatch arm -- because resetting is
    // a one-shot command, not a hold.
    const bool rotateHeld = ImGui::IsKeyDown(ImGuiKey_R);
    // !st.pendingGuide: a guide drag-to-create claims the left-mouse-drag
    // gesture too (PRD Q5), the same way Hand-tool panning already does
    // below -- these must not fire simultaneously with dragging a new guide
    // off a ruler.
    const bool rotating = rotateHeld && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                          !st.pendingGuide.has_value();
    if (rotating) {
      // Angular delta from a linear mouse delta around `pivotScreen`:
      // d(theta) ~= cross(radiusVector, mouseDelta) / |radiusVector|^2.
      // Exact in the limit and a good approximation at frame-to-frame
      // scale, same spirit as tiltPad's direct-manipulation mapping above.
      const ImVec2 v(mouse.x - pivotScreen.x, mouse.y - pivotScreen.y);
      const float r2 = v.x * v.x + v.y * v.y;
      // Too close to the pivot and the angle becomes ill-conditioned --
      // skip this frame's update rather than let a stray pixel of jitter
      // spin the view.
      if (r2 > 400.0f) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        st.view.rotation += (v.x * d.y - v.y * d.x) / r2;
      }
    }

    // --- brush size by canvas gesture (PRD R5, D3) -- ⌃⌥-drag, horizontal
    // only. Resolved here, ahead of the zoom/pan blocks below, for the same
    // reason `rotating` is resolved ahead of them: it also claims the
    // left-mouse-drag gesture, and the competitors need to agree on a
    // winner. Live modifier check each frame, matching `rotateHeld` above,
    // rather than latched at gesture start -- the same house style, not a
    // new inconsistency. docs/shortcuts.md section 2 assigns this chord
    // "size and hardness by dragging"; this track's brief scopes it to size
    // (the horizontal half) only -- hardness (vertical) is a second track's
    // to add.
    const bool sizingHeld = ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyAlt;
    const bool sizing = sizingHeld && hovered && !st.pendingGuide.has_value() &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    if (sizing) {
      // Per-frame, against the CURRENT radius -- the same incremental style
      // `rotating` above and `panning` below already use, not a remembered
      // drag-start value. app/ZoomAndSize.hpp's header comment on
      // `radiusForDrag()` is where the "this is still honestly a pure
      // function of total drag pixels" argument lives; it does not need
      // restating at every call site.
      const float radius = st.brush.model.tip.diameterPx / 2.0f;
      st.brush.model.tip.diameterPx =
          clampBrushRadius(radiusForDrag(radius, ImGui::GetIO().MouseDelta.x)) * 2.0f;
    }

    // --- zoom, anchored the same way regardless of what triggered it (PRD
    // Q1) -- one function, `applyZoomFactor`, and app/ZoomAndSize.hpp's
    // `panForAnchoredZoom()` underneath it, reused by every caller below
    // rather than each inventing its own anchoring rule. `anchorScreen` is
    // the screen point that must stay under the same document coordinate
    // after the zoom -- the wheel and the Zoom tool pass the actual cursor
    // position; a keyboard/menu command has no cursor to anchor on, so it
    // passes the viewport's own centre, matching how those commands behave
    // with no drag or click driving them.
    //
    // **This replaces a formula that never read the mouse position at all**
    // -- see app/ZoomAndSize.hpp's own header comment for the counterexample
    // that proves the old one drifted off whatever point it happened to
    // anchor on rather than the cursor. No existing `--selftest` assertion
    // exercised the old formula's exact output (grepped for
    // `applyZoomFactor`/anchor coverage before writing this), so nothing
    // here is a documented behaviour change -- it is a latent bug this
    // track's own P0 anchor requirement forced into the open.
    auto applyZoomFactor = [&](float factor, ImVec2 anchorScreen) {
      const float oldZoom = st.view.zoom;
      const float newZoom = clampViewZoom(oldZoom * factor);
      if (newZoom == oldZoom) return;  // clamped to the existing limit: nothing to repan
      st.view.panX = panForAnchoredZoom(anchorScreen.x, origin.x, oldZoom, newZoom,
                                        paintOrigin.x, avail.x, texW);
      st.view.panY = panForAnchoredZoom(anchorScreen.y, origin.y, oldZoom, newZoom,
                                        paintOrigin.y, avail.y, texH);
      st.view.zoom = newZoom;
    };
    // --- wheel over the canvas: a NOTCHED wheel zooms (unchanged from
    // before this track); a precise trackpad/Magic Mouse sample pans
    // instead of zooming. app/WheelInput.hpp's header comment is where the
    // two devices are told apart and where the "reuse the anchor, don't
    // reinvent the arithmetic" argument for `applyZoomFactor` already lives
    // one paragraph above this block; what is new here is only the branch.
    // Every major macOS creative app treats a plain two-finger scroll as
    // PAN and reserves an actual pinch for zoom (Preview, Photos,
    // Photoshop) -- before this track, EVERY wheel sample over the canvas
    // reached the single `applyZoomFactor` call below, so a two-finger
    // scroll (a precise, high-frequency sample stream) was reinterpreted as
    // dozens of zoom steps a second, which is the canvas's share of this
    // track's "much too fast" bug.
    if (hovered) {
      const float wheelY = ImGui::GetIO().MouseWheel;
      const float wheelX = ImGui::GetIO().MouseWheelH;
      if (wheelDeltaIsPrecise(wheelY) || wheelDeltaIsPrecise(wheelX)) {
        const CanvasPanDelta pan = canvasPanForPreciseWheel(wheelX, wheelY);
        st.view.panX += pan.dx;
        st.view.panY += pan.dy;
      } else if (wheelY != 0.0f) {
        applyZoomFactor(1.0f + wheelY * 0.12f, mouse);
      }
    }
    // --- raw two-finger trackpad gesture (item 4, ui/MacTrackpadTouch.hpp) --
    // pinch-to-zoom and two-finger rotate BOTH still fire through
    // `pollPinchMagnification()`/`pollRotationDegrees()` below (AppKit
    // synthesises those independently of what this app does at the touch
    // level), so both are drained UNCONDITIONALLY every frame either way --
    // a sample that arrives during a raw-touch gesture must not leak into a
    // later, unrelated one, the same reasoning that already applied when
    // this file had only the classifier-driven path. Which path actually
    // MOVES the view is decided once, by whether raw touch capture reports
    // an active two-finger touch this frame: see ui/TouchGesture.hpp's own
    // header comment for why the classifier alone produces the "now zoom,
    // now rotate around centre, now pan" mode-switching this replaces --
    // pivoting at the CURSOR (not always the canvas centre, unlike the old
    // rotate-only path immediately below) is the fix for the "not pinned to
    // the two points" half of that complaint.
    static TouchGestureSession touchGestureSession;
    const std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> twoFingerTouch =
        pollTwoFingerTouch();
    const float pinchMagnification = pollPinchMagnification();
    const float rotationDegrees = pollRotationDegrees();
    if (twoFingerTouch.has_value()) {
      if (hovered) {
        const Vec2 canvasCenterVec{texW * 0.5f, texH * 0.5f};
        const Vec2 paintOriginVec{paintOrigin.x, paintOrigin.y};
        const Vec2 availVec{avail.x, avail.y};
        const Vec2 texVec{texW, texH};
        const Vec2 cursorVec{mouse.x, mouse.y};
        touchGestureSession.update(twoFingerTouch, st.view, canvasCenterVec, paintOriginVec,
                                   availVec, texVec, cursorVec);
        // The touch pair's own translation (the shared midpoint sliding --
        // a two-finger drag riding along with a pinch/rotate, or on its
        // own) is NOT part of the anchored zoom/rotate `update()` just
        // applied; added here, converted from the trackpad's own
        // dimensionless [0,1] fraction to screen points via its physical
        // `deviceSize`, and sign-adjusted for the System Settings natural/
        // traditional scrolling preference -- see ui/MacTrackpadTouch.hpp's
        // header comment on `trackpadDeviceSize()`/`trackpadNaturalScrolling()`
        // for why neither conversion belongs in the pure math itself.
        const TwoTouchDelta delta = touchGestureSession.lastDelta();
        const TrackpadDeviceSize deviceSize = trackpadDeviceSize();
        float extraPanX = delta.panDx * deviceSize.width;
        float extraPanY = delta.panDy * deviceSize.height;
        if (!trackpadNaturalScrolling()) {
          extraPanX = -extraPanX;
          extraPanY = -extraPanY;
        }
        st.view.panX += extraPanX;
        st.view.panY += extraPanY;
      } else {
        // Not hovered: no anchor to pin to, so this frame does not drive a
        // gesture -- but the session's own "was a pair active last frame"
        // state must still see the gap, or the NEXT frame that regains
        // hover would (wrongly) treat itself as a continuation of a
        // gesture this file never actually applied any of.
        touchGestureSession.update(std::nullopt, st.view, Vec2{}, Vec2{}, Vec2{}, Vec2{}, Vec2{});
      }
    } else {
      // No live raw-touch pair this frame (0, 1, or 3+ fingers, or raw
      // capture unavailable this session at all) -- clear any prior
      // gesture's baseline and fall back to the classifier-driven path
      // exactly as it worked before this track, so a session where raw
      // capture failed to install (ui/MacTrackpadTouch.mm's own stderr
      // message says so) degrades to "the old behaviour", not "broken".
      touchGestureSession.update(std::nullopt, st.view, Vec2{}, Vec2{}, Vec2{}, Vec2{}, Vec2{});
      // --- pinch-to-zoom, anchored under the cursor the same way the wheel
      // above is.
      if (hovered && pinchMagnification != 0.0f)
        applyZoomFactor(zoomFactorForPinch(pinchMagnification), mouse);
      // --- two-finger trackpad rotate. Unlike zoom/pan there is no anchor
      // to preserve here -- `view.rotation` is always about the canvas's
      // own centre (`ViewTransform`'s `canvasCenter`, PRD Q4), the same
      // pivot the existing `R`+drag rotate gesture above already turns the
      // canvas about. app/WheelInput.hpp's
      // `canvasRotationRadiansForTrackpad()`/`wrapRotationRadians()` header
      // comments carry the sign derivation and the wraparound reasoning;
      // neither is re-derived here.
      if (hovered && rotationDegrees != 0.0f)
        st.view.rotation = wrapRotationRadians(st.view.rotation +
                                               canvasRotationRadiansForTrackpad(rotationDegrees));
    }
    const ImVec2 viewportCenter(paintOrigin.x + avail.x * 0.5f, paintOrigin.y + avail.y * 0.5f);
    if (st.requestZoomIn) {
      applyZoomFactor(kZoomStepFactor, viewportCenter);
      st.requestZoomIn = false;
    }
    if (st.requestZoomOut) {
      applyZoomFactor(1.0f / kZoomStepFactor, viewportCenter);
      st.requestZoomOut = false;
    }

    // --- the Zoom tool itself (PRD Q1, A3): click zooms in at the clicked
    // point, Alt/Option-click zooms out, press-drag horizontally scrubs
    // continuously. Gated on `toolZoomsView()` rather than
    // `st.brush.tool == Tool::Zoom` directly -- see app/ZoomAndSize.hpp's
    // header comment on that predicate for why: it is meant to be the
    // literal expression this block is gated on, so a future disjunction of
    // "does this tool have any canvas handler" can absorb it by including
    // the predicate rather than hand-listing `Tool::Zoom` a second time.
    // `!sizingHeld`: the size gesture above also claims a left-mouse-drag
    // and already won the tie for `rotating`/`panning`; Zoom loses it too.
    // `hovered || canvasHeld`: a plain click needs `hovered` (a click that
    // started on a side panel must never zoom just because the tool happens
    // to be Zoom), but a SCRUB that wanders off the canvas edge needs
    // `canvasHeld` too, or it would freeze the instant the cursor crosses
    // onto a panel -- the exact reason this file's own `canvasHeld` comment
    // gives for existing: "a drag that STARTED on the canvas keeps [going]
    // when the pointer runs off the sheet." `rotating`/`panning` above don't
    // need this union because they have no click-only case to guard against.
    if (toolZoomsView(st.brush.tool) && (hovered || canvasHeld) &&
        !st.pendingGuide.has_value() && !sizingHeld) {
      if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        // Scrubby zoom, anchored at the SCREEN point the drag started from
        // -- `io.MouseClickedPos[0]`, which ImGui itself remembers for the
        // whole gesture, not the live mouse position. The live position
        // moves purely to signal "more/less zoom"; anchoring on it instead
        // would make the point the user actually clicked on walk out from
        // under the cursor as they scrub, which is the opposite of what a
        // scrubby zoom is for. Reapplying `applyZoomFactor` with this SAME
        // fixed anchor every frame, against whatever zoom the previous
        // frame left, keeps that original point pinned for the whole drag
        // -- app/ZoomAndSize.hpp's header comment on `panForAnchoredZoom()`
        // is where that induction argument is written out.
        const ImVec2 clickPos = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
        applyZoomFactor(zoomFactorForDrag(ImGui::GetIO().MouseDelta.x), clickPos);
      } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        // A release with zero accumulated drag -- `GetMouseDragDelta()`'s
        // own documented "0 until the mouse moves past a distance
        // threshold" semantics -- is a plain click, not the tail end of a
        // scrub the branch above already handled. Alt/Option picks the
        // direction, matching every other Alt-reverses convention already
        // in this file (e.g. the paint bucket's global-fill modifier above).
        const ImVec2 dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
        if (dragDelta.x == 0.0f && dragDelta.y == 0.0f) {
          const float factor = ImGui::GetIO().KeyAlt ? (1.0f / kZoomStepFactor) : kZoomStepFactor;
          applyZoomFactor(factor, mouse);
        }
      }
    }

    const bool panning =
        !rotateHeld && !sizingHeld && !st.pendingGuide.has_value() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         // `toolPansView()` rather than `== Tool::Hand`: this expression IS
         // the hand tool's canvas handler, and `toolHasCanvasHandler()`'s
         // completeness check reads the same predicate rather than a
         // description of it (app/StrokeSession §6b).
         (toolPansView(st.brush.tool) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)));
    if (panning) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      st.view.panX += d.x;
      st.view.panY += d.y;
    }

    // --- selection and clipboard commands (PRD M1-M5) ----------------------
    //
    // Serviced here rather than in main.cpp's key handler because every one of
    // them needs the active OpenDocument, and several need the sim.
    {
      OpenDocument* od = st.documents.active();
      Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;

      // D1 (docs/reachability-audit.md): undo/redo's keymap-and-menu path.
      // Serviced here for the identical reason the block below is -- ⌘Z and
      // the Edit menu's Undo/Redo both fire from places with no OpenDocument
      // and no sim to settle wet paint against -- and guarded the same way
      // the HISTORY panel's and title bar's own buttons already are, so a
      // chord fired with nothing to undo, or a native menu callback racing a
      // document close, is a no-op rather than a call into
      // `moveHistoryCursor()` against a history that is not there.
      if (od != nullptr) {
        History& h = od->history;
        if (st.requestUndo && h.canUndo()) moveHistoryCursor(st, sim, gpu, *od, -1);
        if (st.requestRedo && h.canRedo()) moveHistoryCursor(st, sim, gpu, *od, +1);
      }
      st.requestUndo = false;
      st.requestRedo = false;

      if (st.requestSelectAll && od != nullptr)
        installSelection(*od, selectAll(od->document.width, od->document.height));
      if (st.requestDeselect && od != nullptr) installSelection(*od, std::nullopt);
      // Reselect (⌘⇧D): restore what the last Deselect threw away. A no-op
      // when nothing has been deselected this session, rather than an error --
      // the command is unreachable from a menu that has not been built yet, so
      // its only route in is a key that may well have been pressed by mistake.
      if (st.requestReselect && od != nullptr && od->lastDeselected.has_value())
        installSelection(*od, od->lastDeselected);
      // Inverse (⌘⇧I), PRD E4.
      //
      // **Deliberately a no-op when nothing is selected**, though the algebra
      // would happily answer. Absent means "no restriction", so its honest
      // complement is a selection covering nothing -- and installing that in
      // response to one keystroke would leave the document refusing every
      // edit, with marching ants nowhere to show why. That is precisely the
      // state core/SelectionMask.hpp names as "why is nothing happening when I
      // paint", and it is not somewhere a single keypress should be able to
      // put someone.
      if (st.requestInvertSelection && od != nullptr && od->selection.has_value())
        installSelection(*od, invertSelection(*od->selection, od->document.width,
                                              od->document.height));

      const Selection* sel =
          (od != nullptr && od->selection.has_value()) ? &*od->selection : nullptr;

      if (st.requestCopy && target != nullptr)
        st.clipboard = copyThroughSelection(*target, sel);
      if (st.requestCopyMerged && od != nullptr)
        st.clipboard = copyMergedThroughSelection(od->document, sel);
      if (st.requestCut && target != nullptr) {
        Clipboard taken = cutThroughSelection(*target, sel);
        // Only an actual cut is an edit. A refusal -- a locked layer, or a
        // selection covering nothing -- must not put a history entry in for
        // an act that changed no pixel.
        if (!taken.empty()) {
          st.clipboard = std::move(taken);
          od->recordEdit("cut", EditKind::Content);
        }
      }
      if (st.requestPaste && od != nullptr && !st.clipboard.empty()) {
        // Above the active layer, which is where every editor puts it.
        const size_t at = std::min(od->activeLayer + 1, od->document.layers.size());
        if (pasteAsLayer(od->document, st.clipboard, at).has_value()) {
          od->activeLayer = at;
          od->recordEdit("paste", EditKind::Structural);
        }
      }
      if (st.requestDeleteSelection && target != nullptr && !target->locked) {
        size_t changed = 0;
        if (target->rgbTiles.has_value())
          changed += clearThroughSelection(*target->rgbTiles, sel);
        if (target->pigmentTiles.has_value())
          changed += clearThroughSelection(*target->pigmentTiles, sel);
        if (changed > 0) od->recordEdit("clear selection", EditKind::Content);
      }

      st.requestSelectAll = false;
      st.requestDeselect = false;
      st.requestReselect = false;
      st.requestInvertSelection = false;
      st.requestCopy = false;
      st.requestCopyMerged = false;
      st.requestCut = false;
      st.requestPaste = false;
      st.requestDeleteSelection = false;

      // --- keep the GPU coverage in step -----------------------------------
      //
      // `setSelection()` uploads a canvas-sized texture, which is expensive
      // enough to want an "has it changed?" test rather than a per-frame
      // rebuild. The revision is that test.
      //
      // The cached BOUNDS used to be refreshed here too, for the marching
      // ants. They are gone: the ants now draw the selection's true boundary
      // (core/SelectionBoundary), which carries its own revision-keyed cache
      // and is asked for at the point it is drawn. Recomputing a bounding box
      // that nothing reads would have been work for a picture nobody sees.
      //
      // The sim upload's condition includes a sim that has not been told yet,
      // deliberately: it must ALSO happen when a sim is constructed
      // mid-session, at which point the revision has not moved.
      if (od != nullptr) {
        const bool stale = od->selectionRevision != st.cachedSelectionRevision ||
                           od->id != st.cachedSelectionDoc;
        const bool simUninformed =
            sim && od->selection.has_value() != sim->hasSelection();
        if (stale || simUninformed) {
          if (sim) {
            sim->setSelection(gpu, od->selection.has_value() ? &*od->selection : nullptr);
          }
          st.cachedSelectionRevision = od->selectionRevision;
          st.cachedSelectionDoc = od->id;
        }
      }
    }

    // --- the selection tools (PRD E3) -------------------------------------
    //
    // Five tools, one ending. Rectangle and ellipse rubber-band a drag; the
    // lasso follows the pointer; the polygon lasso accumulates clicks; the
    // wand takes a single click and floods. What they produce is always a
    // `Selection`, and it always leaves through `commitDrawnSelection()`
    // below, so the PRD E7 boolean modifiers and the empty-gesture rules are
    // written once rather than five times and cannot drift apart.
    //
    // The shape is built ONCE, at the end of the gesture, never per frame:
    // every constructor in core/SelectionShapes allocates tiles, and
    // rebuilding one 120 times a second to draw an outline the draw code can
    // trace from its own points would be work for nothing.
    // `toolDrawsSelection()` rather than the five-way `||` this used to spell
    // inline: this expression IS these five tools' canvas handler, and
    // `toolHasCanvasHandler()`'s completeness check reads the same predicate
    // rather than a copy of it (app/StrokeSession §6b).
    const bool selectionTool = toolDrawsSelection(st.brush.tool);

    if (selectionTool && !panning && !rotating && !sizingHeld && !st.pendingGuide.has_value()) {
      const ImGuiIO& mods = ImGui::GetIO();
      // `!transformActive`: while a Free Transform gizmo owns the canvas the
      // selection tools do not get the mouse. Without this a drag on the box
      // would move the pixels AND draw a new marquee over them.
      const bool clicked =
          hovered && !transformActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      OpenDocument* od = st.documents.active();

      // Latched at mouse-down for every tool, for the reason
      // app/AppState.hpp's `marqueeCombine` gives: Shift is also the
      // constrain modifier, so "which boolean" is a question asked once, at
      // the start, and not re-read from a hand that moved during the drag.
      // The polygon lasso latches on its FIRST click only -- the modifier
      // held on the third click of a five-click path is not a new answer to
      // the same question.
      if (clicked && (!st.polygonLassoActive || st.brush.tool != Tool::PolygonLasso)) {
        st.marqueeCombine = selectionCombineFromModifiers(mods.KeyShift, mods.KeyAlt);
      }

      switch (st.brush.tool) {
        case Tool::Marquee:
        case Tool::EllipseMarquee: {
          if (clicked) {
            st.marqueeDragging = true;
            st.marqueeX0 = tx;
            st.marqueeY0 = ty;
            // A Space-move offset from whatever drag last used this field
            // has no meaning for a brand-new drag (app/SelectionDrag.hpp's
            // own doc comment on SelectionMoveState).
            st.marqueeMove = SelectionMoveState{};
          }
          if (st.marqueeDragging) {
            st.marqueeX1 = tx;
            st.marqueeY1 = ty;

            // T10 (docs/testing-issues.md): live modifier reads for drag
            // GEOMETRY only -- deliberately separate from `st.marqueeCombine`
            // above, which latches Shift/Option at mouse-down to answer a
            // different question (which boolean) that a moving hand must not
            // be allowed to re-answer mid-drag. Shift/Option here only ever
            // shape app/SelectionDrag.hpp's pure box math; they never touch
            // `marqueeCombine`.
            updateSelectionMove(st.marqueeMove, ImGui::IsKeyDown(ImGuiKey_Space), tx, ty);
            const SelectionDragBox box = computeSelectionDragBox(
                st.marqueeX0, st.marqueeY0, st.marqueeX1, st.marqueeY1,
                st.marqueeMove.offsetX, st.marqueeMove.offsetY, mods.KeyShift, mods.KeyAlt);
            st.marqueeBoxX0 = box.x0;
            st.marqueeBoxY0 = box.y0;
            st.marqueeBoxX1 = box.x1;
            st.marqueeBoxY1 = box.y1;

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
              st.marqueeDragging = false;
              if (od != nullptr) {
                // Clamped to the canvas: a drag that left the window would
                // otherwise select texels the document does not have, and
                // every consumer would then walk tiles holding nothing.
                // `box` is already sorted (SelectionDragBox's own contract),
                // so this is a plain clamp, not a further min/max.
                const float x0 = std::clamp(box.x0, 0.0f, texW);
                const float y0 = std::clamp(box.y0, 0.0f, texH);
                const float x1 = std::clamp(box.x1, 0.0f, texW);
                const float y1 = std::clamp(box.y1, 0.0f, texH);
                std::optional<Selection> drawn;
                if (x1 - x0 >= 1.0f && y1 - y0 >= 1.0f) {
                  drawn = st.brush.tool == Tool::Marquee
                              ? selectRectangle(x0, y0, x1, y1)
                              // The drag's bounding box is the ellipse's
                              // bounding box, which is how every editor reads
                              // an ellipse drag -- centre and radii, not two
                              // points on the curve. T10's constrain/from-
                              // centre/space-move gestures all resolve to
                              // `box` above before this point is reached, so
                              // the ellipse needs nothing extra for any of
                              // them.
                              : selectEllipse((x0 + x1) * 0.5f, (y0 + y1) * 0.5f,
                                              (x1 - x0) * 0.5f, (y1 - y0) * 0.5f);
                }
                commitDrawnSelection(st, *od, drawn);
              }
            }
          }
          break;
        }

        case Tool::Lasso: {
          if (clicked) {
            st.marqueeDragging = true;
            st.lassoPoints.clear();
            st.lassoPoints.push_back(SelectionPoint{tx, ty});
          }
          if (st.marqueeDragging) {
            // One vertex per texel of travel, not one per frame. See
            // app/AppState.hpp's `lassoPoints`: coincident vertices become
            // zero-length edges the rasteriser still has to walk.
            const SelectionPoint& last = st.lassoPoints.back();
            if (std::fabs(tx - last.x) >= 1.0f || std::fabs(ty - last.y) >= 1.0f) {
              st.lassoPoints.push_back(SelectionPoint{tx, ty});
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
              st.marqueeDragging = false;
              if (od != nullptr) {
                std::optional<Selection> drawn;
                // The path closes itself -- selectPolygon() joins last to
                // first -- so a lasso released away from where it started is
                // completed with a straight line, which is what every editor
                // does rather than refusing the gesture.
                if (st.lassoPoints.size() >= 3) drawn = selectPolygon(st.lassoPoints);
                commitDrawnSelection(st, *od, drawn);
              }
              st.lassoPoints.clear();
            }
          }
          break;
        }

        case Tool::PolygonLasso: {
          if (clicked) {
            if (!st.polygonLassoActive) {
              st.polygonLassoActive = true;
              st.lassoPoints.clear();
            }
            st.lassoPoints.push_back(SelectionPoint{tx, ty});
          }
          // Closing the path: a double-click, or Enter. Both are offered
          // because a double-click is the discoverable gesture and Enter is
          // the one a user with a full path and a shaky hand wants -- a
          // stray single click at the end of a careful twenty-vertex path
          // would otherwise add a twenty-first.
          const bool closeIt = st.polygonLassoActive &&
                               ((hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ||
                                ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
          if (closeIt) {
            st.polygonLassoActive = false;
            if (od != nullptr) {
              std::optional<Selection> drawn;
              if (st.lassoPoints.size() >= 3) drawn = selectPolygon(st.lassoPoints);
              commitDrawnSelection(st, *od, drawn);
            }
            st.lassoPoints.clear();
          }
          // Escape abandons the path WITHOUT touching the selection. An
          // unfinished gesture is not a request to deselect, and this is the
          // only selection tool that can be half-made.
          if (st.polygonLassoActive && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            st.polygonLassoActive = false;
            st.lassoPoints.clear();
          }
          break;
        }

        case Tool::MagicWand: {
          if (clicked && od != nullptr && tx >= 0 && ty >= 0 && tx < texW && ty < texH) {
            const Layer* target = activeLayerOf(*od);
            // The wand reads RGB tiles. A Pigment layer's latents are not an
            // RGB neighbourhood and ops/FloodFill says so in its own header;
            // rather than flood something that is not there, the click is
            // refused and the selection is left exactly as it was.
            if (target != nullptr && target->rgbTiles.has_value()) {
              FloodFillParams params;
              // Global mode on Option -- "fill all similar" (PRD D25) is the
              // same predicate without the connectivity walk. It shares the
              // modifier with Subtract deliberately: the wand's Option-click
              // subtracts a GLOBAL region, which is the combination a user
              // reaching for either one actually wants.
              params.reach = mods.KeyAlt ? FloodFillReach::Global : FloodFillReach::Contiguous;
              commitDrawnSelection(
                  st, *od,
                  floodFillSelection(*target->rgbTiles,
                                     PixelCoord{static_cast<int32_t>(tx),
                                                static_cast<int32_t>(ty)},
                                     od->document.width, od->document.height, params));
            }
          }
          break;
        }

        default:
          break;
      }
    } else {
      st.marqueeDragging = false;
      // Switching away from the polygon lasso mid-path abandons it. Leaving
      // it live would resume a stale path on return, with vertices the user
      // has long forgotten placing.
      if (st.polygonLassoActive) {
        st.polygonLassoActive = false;
        st.lassoPoints.clear();
      }
    }

    // ===== Tool::Move -- BEGIN: the drag (app/MoveTool.hpp) ===============
    //
    // A Move gesture IS a Free Transform restricted to a pure translation: the
    // same `st.transform` session, the same "one matrix, one resample at
    // commit" discipline, the same uploaded preview quad and the same
    // ui/TransformCompositeSplit stack order. What differs is only the
    // modality -- Free Transform waits for Return, Move commits on pen-up,
    // which is what makes it a tool rather than a mode -- and that the drag
    // never touches a scale or rotate handle, so the box's handles are not
    // drawn (see the `!g_moveDragging` guard in the gizmo's draw block).
    //
    // Placed with the other tool blocks, after `panning`/`rotating`/
    // `sizingHeld` exist, and gated on all three for the same reason every
    // block here is: a space-drag, a rotate-drag or a size-scrub must not
    // also pick the picture up.
    if (toolMovesPixels(st.brush.tool) && !panning && !rotating && !sizingHeld &&
        !st.pendingGuide.has_value()) {
      if (!g_moveDragging && !g_moveCommitPending && !st.transform.active() && hovered &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (OpenDocument* od = st.documents.active()) {
          const TransformBeginResult began = beginMove(st.transform, *od);
          if (!began.ok) {
            // app/MoveTool.hpp section 3: shown, never a dead drag. The drag
            // is deliberately NOT started -- a gesture that moved a preview
            // around for a second and then committed nothing is a worse lie
            // than a pointer that does not pick the picture up at all.
            g_docStatus = began.error;
          } else {
            g_moveDragging = true;
            g_moveStartX = tx;
            g_moveStartY = ty;
            g_moveDx = 0.0f;
            g_moveDy = 0.0f;
            // The session's ONE upload, exactly as Cmd+T's begin does it.
            beginTransformPreview(st, gpu);
          }
        }
      } else if (g_moveDragging && !g_moveCommitPending) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
          // Absolute, from this gesture's own start cursor -- never frame N's
          // matrix times frame N+1's delta. app/MoveTool.hpp's
          // `setMoveTranslation()` carries the argument.
          g_moveDx = tx - g_moveStartX;
          g_moveDy = ty - g_moveStartY;
          setMoveTranslation(st.transform, g_moveDx, g_moveDy);
        } else {
          // Committed at the TOP of the NEXT frame, not here. This frame's
          // composite was already built with this layer hidden, so writing
          // the pixels now would blank it for one frame; `g_moveDragging`
          // stays true until then so the preview keeps standing in for it.
          g_moveCommitPending = true;
        }
      }
    }
    // ===== Tool::Move -- END ===============================================

    // --- the eyedropper (PRD Q10, P0) --------------------------------------
    //
    // **The tool that claimed to be implemented for two phases and had no
    // handler.** `kToolMeta` said `implemented = true`, so the palette cell was
    // clickable and highlighted, `toolCursorOnTarget()` withheld the `Refuse`
    // cursor *because* of that flag and handed out a bespoke `ToolCursor::Sample`
    // pointer -- and then the click arrived here and nothing whatsoever
    // consumed it. Every tier of the chrome said live except the one that acts.
    // `toolHasCanvasHandler()` (ui/AtelierChrome) is what stops that recurring,
    // and this block is the handler that makes the eyedropper's row in that
    // table true.
    //
    // **A drag re-samples continuously**, on every frame the button is held,
    // not only on the click. Photoshop does the same and the reason is the
    // gesture people actually make: over a gradient, a photograph or a
    // brushstroke's soft edge the exact texel matters, and "click, look at the
    // swatch, undo the pick, click again" is a loop nobody should be made to
    // run. Watching the swatch while dragging converges in one gesture. The
    // cost of continuous sampling is bounded and small -- `probePixel()` is
    // read-only, allocates nothing on the RGB path, and walks at most
    // `sampleSize^2` texels of a box that is clipped to the document.
    //
    // **A pick records NO history entry**, deliberately, and this is the same
    // rule the paint bucket ten lines below obeys from the other side: history
    // is for edits to the document, and both ops there return a written-texel
    // count precisely so that a fill which changed nothing records nothing. A
    // pick changes no texel at all. Three consequences settle it: a
    // continuously-resampling drag would push hundreds of rows into a panel
    // PRD O2 says is scanned to find an edit to undo; undoing "past" a pick
    // would have to restore a foreground colour that `core::History` does not
    // store and has no field for; and a user who wants their previous colour
    // back has the COLOR panel right there, which is a cheaper answer than a
    // history model that tracks tool state.
    if (toolSamplesCanvas(st.brush.tool) && !panning && !rotating &&
        !st.pendingGuide.has_value()) {
      OpenDocument* od = st.documents.active();
      // Held, not clicked -- see the drag paragraph above. `hovered` keeps a
      // drag that wanders off the canvas from sampling coordinates the
      // document does not have; `probePixel()` would answer transparent black
      // there anyway, but refusing to sample at all leaves the last good pick
      // in place rather than replacing it with a refusal sentence.
      const bool sampling =
          hovered && !transformActive && ImGui::IsMouseDown(ImGuiMouseButton_Left);
      if (sampling && (od == nullptr || (tx >= 0 && ty >= 0 && tx < texW && ty < texH)))
        applyEyedropperPick(st, PixelCoord{static_cast<int32_t>(tx), static_cast<int32_t>(ty)});
    }

    // === BEGIN Tool::Measure handler (app/MeasureLine) =====================
    //
    // Gated on `toolMeasuresCanvas()`, the SEVENTH gate, and deliberately not
    // on `toolSamplesCanvas()` above -- Measure shares the eyedropper's
    // palette group and cursor, and folding it into that predicate would send
    // ruler drags into `applyEyedropperPick()`. app/StrokeSession.hpp §6b.
    //
    // The gesture is three events and three calls, all of the arithmetic in
    // app/MeasureLine so `--selftest` drives the identical sequence headlessly.
    // Unclamped `tx`/`ty` on purpose: a measurement that runs off the edge of
    // the picture is a legitimate thing to want (how far past the crop is
    // this?), unlike a selection, which must not name texels the document
    // does not have.
    if (toolMeasuresCanvas(st.brush.tool)) {
      const OpenDocument* od = st.documents.active();
      const bool blocked =
          panning || rotating || sizingHeld || transformActive || st.pendingGuide.has_value();
      if (!blocked && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        beginMeasureLine(st.measure, od != nullptr ? od->id : 0u, tx, ty);
      if (st.measure.dragging) {
        // **`!IsMouseDown`, not `IsMouseReleased`.** A Space-pan or a
        // size-drag started mid-measurement takes this block out of the
        // `blocked` arm for as long as it lasts, and the one frame carrying
        // the release event can land inside it -- after which `IsMouseReleased`
        // is false forever and the ruler would go on following a pointer whose
        // button is up. Asking whether the button is still down cannot miss an
        // edge, because it is not an edge.
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) endMeasureLine(st.measure);
        else if (!blocked) updateMeasureLine(st.measure, tx, ty);
      }
    } else if (st.measure.active) {
      // Leaving the tool throws the ruler away (app/MeasureLine.hpp's
      // `clearMeasureLine()`): a line drawn with Measure means nothing under
      // the brush, and there would be no gesture left to dismiss it with.
      clearMeasureLine(st.measure);
    }
    // === END Tool::Measure handler =========================================

    // --- the gradient tool's live preview, cleared -------------------------
    //
    // The gradient preview's own "my popup is not open" line.
    //
    // Every `drawXDialog()` below clears its preview on every frame its modal
    // is not open, and this is the canvas's version of that: the preview is
    // set only inside the gradient drag a few lines down, so every OTHER
    // frame -- a different tool selected, the pointer up, a document switch,
    // a transform started mid-air -- has to be a frame that clears it. Doing
    // this only where the drag ends would leave the ramp on screen after any
    // path out of a drag that is not a clean mouse-release, and there are
    // several (the tool changing under a keyboard shortcut while the button
    // is held is the easy one to reach).
    //
    // Owner-guarded, so it cannot blank a Levels or Curves preview: that is
    // `clearFilterPreview()`'s whole argument, and this call is exactly the
    // every-frame unconditional caller the guard was written for.
    if (!(st.brush.tool == Tool::Gradient && st.gradientDrag.active))
      clearFilterPreview(FilterPreviewOwner::GradientTool);

    // --- the pixel-writing tools: paint bucket and gradient ----------------
    //
    // PRD D25/D26 and D24. Unlike the five selection tools above, these are
    // EDITS: each writes texels, so each needs a locked-layer refusal and a
    // history entry, and neither may record one when it changed nothing --
    // both ops return a written-texel count for exactly that reason, and
    // that count is what gates `recordEdit()` here.
    //
    // Both are RGB-layer only. A Pigment layer stores latents plus mass, not
    // colour, and filling one with a straight RGBA would be writing the wrong
    // kind of value into it; ops/FloodFill and ops/Gradient both take a
    // `TileStore` and say so.
    //
    // **That refusal used to be silent, and this is where it stopped being.**
    // The gate was one local `usable` bool spelled inline here, and it sat
    // *inside* the click condition -- so a bucket click on the layer kind
    // `CONTEXT.md` makes the default for a new layer evaluated to false and
    // vanished: no fill, no history entry, no message, nothing on the canvas
    // and nothing in the chrome. It is the same invisible wrong-target failure
    // app/StrokeSession §1 was written about, and the brush had had a refusal
    // for it since the RGB route landed while these two had not. The predicate
    // now lives beside `strokeRouteWritesLayer()` (that header's §6) so the
    // gate, the message and the options bar's indicator are one answer rather
    // than three, and the click is REFUSED OUT LOUD instead of discarded.
    if (toolWritesRgbPixels(st.brush.tool) && !panning && !rotating && !sizingHeld &&
        !st.pendingGuide.has_value()) {
      OpenDocument* od = st.documents.active();
      Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;
      const PixelOpRefusal refusal = pixelOpRefusalFor(target);
      const bool usable = refusal == PixelOpRefusal::None;

      // **A refusal is cleared the moment it stops being true**, every frame
      // and not only on the next click. The options bar shows one line, and it
      // shows the refusal *instead of* the route indicator -- so a stale
      // sentence about a Pigment layer would go on hiding the live "-> rgb-fill"
      // for the RGB layer the user has just fixed the problem by selecting,
      // until they clicked again to find out. A fill tool has no in-flight
      // state to protect (unlike a stroke, whose refusal is cleared at the next
      // pen-down because it must survive the frames of the gesture it refused),
      // so "this layer can take the fill" is the whole truth about it and there
      // is nothing left to say.
      if (usable) g_strokeRefusal.clear();

      // The foreground colour, DECODED TO LINEAR.
      //
      // `paint/Palette`'s `rgb` is display-referred sRGB -- it is drawn
      // straight into an 8-bit swatch and handed raw to the Mixbox LUT, whose
      // API is sRGB -- while both ops want STRAIGHT LINEAR, which is what this
      // build's working space is. ops/Gradient.hpp states the rule and puts
      // the decode on the caller: "a colour picked from an sRGB swatch must be
      // decoded by whoever builds the stop list, not here". Skipping it fills
      // roughly twice as dark as the swatch shown, which reads as a colour
      // management bug rather than a missing conversion.
      //
      // **The `BrushState` overload, not the palette-index one.** It used to
      // pass `st.brush.pigment`, which was the same colour until the
      // eyedropper existed; leaving it would have made the bucket and the
      // gradient fill with the pigment the user replaced while the brush
      // painted the colour they picked -- two tools disagreeing about the
      // foreground, which is worse than either being wrong alone.
      const std::array<float, 4> fg = foregroundLinearRgba(st.brush);

      if (st.brush.tool == Tool::PaintBucket) {
        // `usable` is deliberately NOT in this condition. A click on the canvas
        // is a click on the canvas whatever the active layer is made of, and
        // whether it can be honoured is answered *inside*, where there is
        // somewhere to put the answer. Putting it back in the condition is what
        // makes the refusal silent again.
        if (hovered && !transformActive && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            tx >= 0 && ty >= 0 &&
            tx < texW && ty < texH) {
          if (!usable) {
            // The same band, the same colour and the same voice as the stroke
            // refusals below -- and cleared on the next click that lands, so
            // the sentence describes this click rather than an older one.
            g_strokeRefusal = pixelOpRefusalMessage(refusal, target, "paint bucket");
          } else {
            // No clear here: the block head already cleared it this frame, and
            // a second one would suggest the refusal's lifetime is per click.
            FloodFillParams params;
            // Same modifier meaning the wand gives it: Option is "fill all
            // similar" (PRD D25's second half), the global predicate pass
            // rather than the connectivity walk.
            params.reach = ImGui::GetIO().KeyAlt ? FloodFillReach::Global
                                                 : FloodFillReach::Contiguous;
            Selection region = floodFillSelection(
                *target->rgbTiles,
                PixelCoord{static_cast<int32_t>(tx), static_cast<int32_t>(ty)},
                od->document.width, od->document.height, params);
            // **The active selection still bounds the bucket.** PRD E1 is P0 --
            // "every deposit and every op respects the active selection" -- and
            // a bucket that ignored it would be the one op in the build that
            // painted outside the marching ants.
            if (od->selection.has_value()) {
              region = combineSelections(region, *od->selection, SelectionCombine::Intersect);
            }
            if (fillThroughSelection(*target->rgbTiles, region, fg) > 0) {
              od->recordEdit("paint bucket", EditKind::Content);
            }
          }
        }
      } else {
        // The gradient is a DRAG: its two handles are where the pointer went
        // down and came up, which is the geometry ops/Gradient takes.
        //
        // **The gradient had the identical defect and gets the identical fix**
        // -- it shared the one `usable` bool, and a drag on a Pigment layer was
        // discarded at pen-up with nothing said. Fixing only the bucket would
        // have left the same silence in the tool sitting in the same palette
        // group behind the same guard.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          // **Refused at pen-DOWN, not at pen-up**, which is the one place the
          // two tools' refusals differ in shape. A bucket's wasted gesture is
          // one click; a gradient's is a drag across the canvas, and letting
          // the user pull the whole ramp before admitting it was never going to
          // land is the same silence in slower motion. So a refused gradient
          // never starts a drag at all.
          if (!usable) {
            g_strokeRefusal = pixelOpRefusalMessage(refusal, target, "gradient");
          } else {
            st.gradientDrag.active = true;
            st.gradientDrag.x0 = tx;
            st.gradientDrag.y0 = ty;
            // The far handle starts ON the near one, so the first frame of
            // the drag is degenerate and `gradientDragIsUsable()` refuses a
            // preview for it -- rather than leaving last drag's endpoint here
            // and previewing a ramp aimed somewhere the pointer has not been.
            st.gradientDrag.x1 = tx;
            st.gradientDrag.y1 = ty;
          }
        }
        if (st.gradientDrag.active) {
          // Only recomputed on the frames the far handle actually moved. A
          // preview costs a `TileStore` copy plus a full-region
          // `renderGradient()`, and a held-still pointer during a drag is the
          // common case, not the rare one -- this is `FilterPreviewState`'s
          // own `generation` argument applied one level earlier, at the
          // render rather than at the upload.
          //
          // `--gradient-demo drag` pins the far handle (app/AppState.hpp) --
          // it is the one thing a screenshot run cannot supply, because a
          // held drag's endpoint is the live mouse and a screenshot run's
          // mouse is wherever the human left it.
          const bool aimMoved = st.gradientDragDemo ||
                                (tx != st.gradientDrag.x1 || ty != st.gradientDrag.y1);
          if (!st.gradientDragDemo) {
            st.gradientDrag.x1 = tx;
            st.gradientDrag.y1 = ty;
          }

          // === the live preview ==========================================
          //
          // **Rendered by `renderGradient()` into a copy of the layer, not by
          // a second approximate drawing of the ramp on the overlay.** An
          // overlay quad would be quick and would be a different picture: it
          // would miss the selection, miss every blend mode and every layer
          // above, and -- because it would be its own code -- would be free to
          // disagree with the commit in exactly the ways a preview must not.
          // This path is the pixels, composited where they will land.
          //
          // The copy is `TileStoreOf`'s refcount bump (see
          // `filterPreviewViewFor()`), so what is actually paid per frame is
          // the gradient's own writes into the tiles it touches.
          //
          // One frame of lag, and it is structural: the canvas quad is drawn
          // near the top of this function and this input block runs near the
          // bottom, so what is set here is composited next frame. Every
          // Filter dialog's preview has the same shape and nobody has ever
          // been able to see it; the rubber-band line below is drawn from
          // this same frame's pointer, so the line leads the ramp by one
          // frame during a fast drag rather than the ramp leading the line.
          if (aimMoved) {
            const std::optional<size_t> previewLayer = activeLayerIndex(*od);
            if (gradientDragIsUsable(st.gradientDrag.x0, st.gradientDrag.y0, st.gradientDrag.x1,
                                     st.gradientDrag.y1) &&
                previewLayer.has_value()) {
              TileStore scratch = *target->rgbTiles;
              const GradientRegion previewRegion{0, 0, od->document.width,
                                                 od->document.height};
              const Selection* previewSel =
                  od->selection.has_value() ? &*od->selection : nullptr;
              renderGradient(scratch,
                             previewRegion,
                             gradientToolGeometry(st.gradient, st.gradientDrag.x0,
                                                  st.gradientDrag.y0, st.gradientDrag.x1,
                                                  st.gradientDrag.y1),
                             currentGradientStops(st.brush),
                             previewSel);
              setFilterPreview(FilterPreviewOwner::GradientTool, od->id, *previewLayer,
                               std::move(scratch));
            } else {
              // A drag that has shrunk back to nothing stops previewing,
              // because pen-up would now commit nothing. The preview and the
              // commit refuse on the one shared predicate
              // (`app/GradientTool.hpp` § 7) precisely so this cannot become a
              // ramp on screen that vanishes when the hand lifts.
              clearFilterPreview(FilterPreviewOwner::GradientTool);
            }
          }

          if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            st.gradientDrag.active = false;
            // The preview goes the instant the hand lifts, whether or not the
            // commit below writes anything. Leaving it up for the frame the
            // real pixels land would be harmless; leaving it up when the
            // commit REFUSES would leave a ramp on screen that is in no
            // document and would survive until the next tool change.
            clearFilterPreview(FilterPreviewOwner::GradientTool);

            // `usable` is re-tested rather than trusted from pen-down: the
            // drag spans frames, and `*target->rgbTiles` is dereferenced two
            // lines down. Nothing in this build can lock or retype a layer
            // while the pointer is held -- the same argument app/StrokeSession
            // §5 makes about its own latched target -- so this is the guard
            // being kept where it is relied on rather than where it happens to
            // have been established.
            //
            // The ramp, the aim and the "is this a gradient at all" test all
            // come from `app/GradientTool`, and the preview twenty lines above
            // calls the identical three functions with the identical
            // arguments. That is the whole reason those functions exist
            // (`app/GradientTool.hpp` § 1): what the user watched during the
            // drag and what lands here are not two computations that agree,
            // they are one computation run twice.
            if (usable && gradientDragIsUsable(st.gradientDrag.x0, st.gradientDrag.y0,
                                               st.gradientDrag.x1, st.gradientDrag.y1)) {
              const GradientRegion region{0, 0, od->document.width, od->document.height};
              const Selection* sel =
                  od->selection.has_value() ? &*od->selection : nullptr;
              if (renderGradient(*target->rgbTiles, region,
                                 gradientToolGeometry(st.gradient, st.gradientDrag.x0,
                                                      st.gradientDrag.y0, st.gradientDrag.x1,
                                                      st.gradientDrag.y1),
                                 currentGradientStops(st.brush), sel) > 0) {
                od->recordEdit("gradient", EditKind::Content);
              }
            }
          }
        }
      }
    }

    // --- stroke ---
    // **`!transformActive` belongs on BOTH tool predicates, not on the
    // `strokeTool` derived from them.** Putting it only on `strokeTool` is
    // what this gate was first written as, and it did not stop a stroke -- it
    // REROUTED one. The layer-writing branch below is `if (strokeTool && ...)`
    // and the solver branch is its `else if (paintTool && ...)`, so closing
    // the first merely dropped the pen through into the second, which painted
    // into the solver canvas instead of the layer. Caught by photographing
    // the pixels, not by reading the condition: strokeTool really was false,
    // and the paint really did land. Gating the two leaf predicates means a
    // branch added later inherits the rule instead of having to remember it.
    const bool paintTool = (st.brush.tool == Tool::Brush ||
                            st.brush.tool == Tool::Water ||
                            st.brush.tool == Tool::DryBrush) &&
                           !transformActive;
    // **The eraser is a stroke tool but never a SOLVER stroke**, which is why it
    // is a second flag rather than a fourth line above. It joins `paintTool` at
    // the two branches below that reach a layer and at the cursor ring, and is
    // deliberately absent from the branch that constructs `PaintSim`: that
    // simulation has no alpha and no erase step, so an eraser reaching it would
    // run the *paint* path and add pigment where the user asked for its removal
    // (app/StrokeSession.hpp §1's Eraser rows). Folding it into `paintTool` would
    // have been one word and would have made the eraser deposit watercolour on
    // the canvas texture the moment no document was open.
    const bool eraseTool = st.brush.tool == Tool::Eraser && !transformActive;
    // **The pencil is a stroke tool and never a solver stroke either**, and it
    // is a third flag for the identical reason the eraser is a second one. It
    // joins `paintTool` at the branches that reach a layer and at the cursor
    // ring, and is deliberately absent from the branch that constructs
    // `PaintSim`: that simulation's whole output is diffusion, wet edges and
    // granulation, so a pencil reaching it would draw the softest mark in the
    // build with the one tool chosen for having no soft edge
    // (brush/PencilDeposit §0, app/StrokeSession.hpp §1's Pencil rows).
    const bool pencilTool = st.brush.tool == Tool::Pencil && !transformActive;
    // **Dodge and Burn are stroke tools and never SOLVER strokes**, a third
    // flag for the identical reason `eraseTool` is a second one: `sim::PaintSim`
    // has no tonal step, so a Dodge reaching it would run the *paint* path with
    // the loaded pigment and deposit colour where the user asked for a tonal
    // shift (app/StrokeSession.hpp §1's Dodge/Burn rows). `!transformActive` is
    // on this leaf predicate and not on `strokeTool`, for the reason the comment
    // above spells out: gating only the derived bool REROUTES a stroke into the
    // solver branch rather than stopping it.
    const bool tonalTool =
        (st.brush.tool == Tool::Dodge || st.brush.tool == Tool::Burn) && !transformActive;
    // --- Clone Stamp: the source gesture, and nothing else -----------------
    //
    // **Deliberately three lines here plus one block below**, because this
    // function is shared with five other tools and everything else the clone
    // needs already exists: it is a stroke tool like any other from
    // `strokeTool` onward, `StrokeSession::begin()` owns its refusal, and
    // `app/StrokeSession`'s two free functions own the gesture's rules. The
    // only thing that cannot live anywhere but here is that a pointer position
    // exists here and nowhere else.
    //
    // `!cloneAnchoring` on `strokeTool` is the load-bearing half: an
    // Option+click is a mouse-down like any other, so without it the same
    // frame that set the source would also start a stroke *from* it -- one
    // dab of a perfect self-copy, an undo entry for a click that was meant to
    // change nothing, and a source the user cannot reset without painting.
    const bool cloneTool = st.brush.tool == Tool::CloneStamp && !transformActive;
    const bool cloneAnchoring = cloneTool && ImGui::GetIO().KeyAlt && !sizingHeld;
    // **The smudge is a stroke tool and never a SOLVER stroke either**, and it
    // is a third flag for the eraser's reason rather than a fourth line in
    // `paintTool`: `sim::PaintSim` has no smudge step, so a smudge reaching it
    // would run the *paint* path and deposit the loaded FOREGROUND pigment --
    // a tool whose whole promise is "introduces nothing that was not already in
    // the picture" introducing a colour that was not (app/StrokeSession.hpp
    // §1's Smudge rows).
    //
    // **This line is the one the completeness check cannot see, and that is
    // worth knowing.** `toolHasCanvasHandler()` derives its answer from
    // `toolBeginsStroke()`, which PROBES `strokeRouteFor()` -- so a tool given a
    // route but left out of this hand-written whitelist would be
    // `toolImplemented()`, palette-clickable, cursor-live, and completely inert
    // under the pen: the eyedropper's original defect exactly, arriving through
    // the one predicate in the chain that is still a list of `Tool` values
    // rather than the gate itself. The smudge route was written, flipped and
    // green in `--selftest` before this line existed.
    const bool smudgeTool = st.brush.tool == Tool::Smudge && !transformActive;
    // **`strokeTool` is `toolBeginsStroke()` now, not the OR of the three flags
    // above**, and that is the structural half of the paragraph just above. The
    // three flags stay, because the branches below genuinely have to tell the
    // tools apart -- only `paintTool` may construct the solver, and each of the
    // three needs its own refusal sentences -- but the question "does the
    // canvas listen to this tool at all" now reads the SAME predicate
    // `ui/AtelierChrome`'s `toolHasCanvasHandler()` reads, rather than a
    // hand-written list that agreed with it on the day it was typed.
    // `toolBeginsStroke()` probes `strokeRouteFor()`, so the two cannot drift:
    // a sixth stroke tool routed later is picked up here for free, and
    // `app/selftest/Smudge.cpp` pins the accepted set to exactly the five that
    // exist today so that adding one is a decision rather than an accident.
    //
    // It is exactly the OR it replaces today, tool for tool -- Brush, Water,
    // DryBrush, Eraser, Smudge -- so this changes no behaviour; it changes what
    // a future tool has to remember.
    // **`toolBeginsStroke()`, not a list of leaf flags** -- and the
    // `!cloneAnchoring` term is the one thing the derived predicate cannot know:
    // an Option-click that sets the clone source must not also start a stroke,
    // which is a property of the GESTURE rather than of the tool.
    const bool strokeTool =
        toolBeginsStroke(st.brush.tool) && !transformActive && !cloneAnchoring;
    const bool inside = tx >= 0 && ty >= 0 && tx < texW && ty < texH;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (cloneTool && hovered && inside && !panning && !rotating && !sizingHeld &&
        !st.pendingGuide.has_value()) {
      if (cloneAnchoring) {
        // Clicked, not held: an anchor is a point, and a held Option-drag
        // re-aiming the source on every frame would make the offset whatever
        // the pointer happened to be over when the button came up.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          setCloneAnchor(st.clone, Vec2{tx, ty});
          g_strokeRefusal.clear();
        }
      } else if (down && !g_stroke.active()) {
        // Pen-down, before `begin()` below reads the offset. Idempotent after
        // the first stroke since the anchor (this build clones aligned), and a
        // no-op with no anchor -- in which case `begin()` refuses out loud.
        latchCloneOffset(st.clone, Vec2{tx, ty});
      }
    }

    st.sim.brushActive = 0;
    st.paintingThisFrame = false;
    st.pendingDabs.clear();

    // Oil's contact -> velocity -> transfer pipeline (PaintSim::frame(),
    // shaders/oil_*.wgsl) still wants a genuine segment, not a point: its
    // tangential brush-velocity term (oil_velocity.wgsl's `vb`) and the
    // swept brush-grid footprint (include/donor.wgsl's brushGridCoord) both
    // need real motion between two positions. Feeding it
    // (last dab before now) -> (newest dab), both arc-length-quantised,
    // keeps that motion meaningful while making "holding still produces no
    // motion" true by construction -- jitter too small to cross a spacing
    // threshold never produces a dab, so it never advances this segment
    // either. That is what makes deleting oil_transfer.wgsl's
    // velocityCutoff safe (ADR-0003 bullet 4): a truly held brush now
    // reaches this function with pendingDabs empty, every frame.
    auto applyDabsToOilSegment = [&]() {
      if (st.pendingDabs.empty()) return;
      st.sim.brushAx = st.lastDabX;
      st.sim.brushAy = st.lastDabY;
      st.sim.brushBx = st.pendingDabs.back().x;
      st.sim.brushBy = st.pendingDabs.back().y;
      st.sim.brushActive = 1;
      st.lastDabX = st.pendingDabs.back().x;
      st.lastDabY = st.pendingDabs.back().y;
    };

    // !st.pendingGuide: same reasoning as `rotating`/`panning` above --
    // freehand painting must never fire while a guide drag is in progress.
    // This is also the boundary that keeps snapping (app/Snapping.hpp)
    // entirely out of the brush path: resolveSnap() is only ever called
    // from the pendingGuide block above, never from here -- a stroke is
    // exactly as continuous and unsnapped after this step as before it.
    // --- which route this stroke takes (app/StrokeSession section 1) -------
    //
    // Decided *before* `ensurePaintSim()` below, and that ordering is the
    // whole of ADR-0001's idle-memory rule surviving this step: a stroke into
    // a Pigment or RGB layer must not construct the solver, or painting on a
    // layer kind that needs no simulation would allocate every field a
    // watercolour session does. That mattered for one layer kind when the
    // deposit landed; it now matters for the kind `Document::createBlank()`
    // actually makes, so an ordinary File > New and a brush stroke allocates
    // no solver fields at all.
    OpenDocument* strokeDoc = st.documents.active();
    const Layer* strokeTarget = strokeDoc != nullptr ? activeLayerOf(*strokeDoc) : nullptr;
    const StrokeRoute route = strokeRouteFor(st.brush.tool, strokeTarget);

    if (strokeTool && down && hovered && inside && !panning && !rotating && !sizingHeld &&
        !st.pendingGuide.has_value() && strokeRouteWritesLayer(route)) {
      // **The pen reaches a Layer.** app/StrokeSession section 4 said this was
      // "a missing decision rather than missing plumbing", and the decision it
      // was missing was `OpenDocument::activeLayer`.
      //
      // **All five layer-writing routes come through here**, and this branch
      // does not know which -- `StrokeSession::begin()` reads the route table
      // once and owns the difference from there. A Pigment target deposits
      // latent and mass through brush/Deposit; an RGB target deposits
      // premultiplied linear colour through brush/RgbDeposit, with a per-stroke
      // alpha accumulator and the active selection as its bound; an eraser on an
      // RGB target takes alpha back out through brush/RgbErase, with a
      // per-stroke *erasure* accumulator and the same bound. Everything visible
      // from this block -- the tip, the pressure schedule, the pen-up below, the
      // single history entry -- is identical for the three, which is the whole
      // reason the RGB routes were worth putting inside `StrokeSession` rather
      // than beside it.
      //
      // **The eraser is shaped by the same tip and the same dynamics**, and that
      // is ADR-0007's requirement rather than a shortcut: "it inherits the whole
      // modulation matrix -- pressure, tilt, jitter, spacing, grain -- because an
      // eraser with no dynamics is useless for the drawing work it is for". So
      // `brushTipFor()` below is called once for every stroke tool, and
      // pressure->size and pressure->flow reach an erase exactly as they reach a
      // brush: harder pressure erases a wider mark and bites faster. Radius,
      // hardness, roundness, angle and spacing come from the active preset for
      // the same reason -- a painter who has set up a soft round tip expects to
      // erase with the shape they are drawing with, and a separate eraser tip
      // would be a second brush editor to keep in step.
      //
      // Shaped exactly as that header predicted: `begin()` where
      // `strokePath.reset()` is, `addPoint()` where `strokePath.addPoint()`
      // is, `end()` where `strokePath.flush()` is. No solver, no
      // `st.pendingDabs`, no `st.sim.brush*` -- those belong to the other
      // route and setting them here would leave the oil segment carrying a
      // stroke that never touched the canvas texture.
      st.paintingThisFrame = true;
      // The whole source set, not pressure alone -- tilt, azimuth and barrel
      // reach the tip here (app/PenAxes.hpp converts them), which is what the
      // DYNAMICS matrix's non-pressure rows actually drive.
      const DynamicInputs live = dynamicInputsFor(st);
      const BrushTip tip = brushTipFor(st.brush, lut, live);
      if (!g_stroke.active()) {
        g_strokeRefusal.clear();
        // **`&st.brush.model` is what makes the Variance-driven, per-dab
        // sources reach a real stroke.** Size, Angle and Roundness resolve
        // per dab inside `StrokeSession`, not once per frame in
        // `dynamicInputsFor()` above, so they can only be driven by a model
        // the session holds for the stroke's whole life. This used to be
        // `&st.brush.links` -- the matrix's `BrushLinkSet`, now shelved
        // behind `--advanced-dynamics` (`ui/DynamicsMatrixPanel.hpp`) -- and
        // that pointer was what made ITS stroke-local sources (Velocity,
        // Fade, Noise, Random, Direction, Initial Direction) reachable from
        // a real click rather than only from `--selftest`. `live` is passed
        // alongside as the hardware sample `begin()` latches for the
        // stroke's life, the same role `&st.brush.links` used to share this
        // call with.
        // `&st.clone` is read only on the clone route (app/StrokeSession
        // §1b); on every other one it is ignored, so this is one argument
        // rather than a branch.
        if (!g_stroke.begin(*strokeDoc, strokeDoc->activeLayer, tip, st.brush.tool,
                            &g_strokeRefusal, &st.brush.model, live, &st.clone)) {
          st.paintingThisFrame = false;
        }
        st.lastX = tx;
        st.lastY = ty;
      }
      if (g_stroke.active()) {
        // Per frame, from this frame's pressure -- the same granularity the
        // solver route gets, which sets one brushRadius per frame.
        //
        // **Smoothed, not raw.** `g_stroke.smoothPressure()` (PaintCopilot
        // §3.2's EMA jitter filter, StrokeSession.hpp's own comment) is
        // called here rather than beside `dynamicInputsFor()` above, on
        // purpose: this branch only runs once `g_stroke.active()`, which on
        // a stroke's first painting frame is true only AFTER `begin()` has
        // already reset the filter's per-stroke state for it -- calling it
        // any earlier would blend against the previous stroke's last
        // reading. `tip` above (built from the raw sample, used only to
        // decide whether `begin()` accepts the stroke) is superseded here
        // before a single dab is ever emitted from it.
        DynamicInputs smoothed = live;
        smoothed.pressure = g_stroke.smoothPressure(live.pressure);
        g_stroke.setTip(brushTipFor(st.brush, lut, smoothed), smoothed);
        g_stroke.addPoint(tx, ty);
        st.lastX = tx;
        st.lastY = ty;
      }
    } else if (strokeTool && down && hovered && inside && !panning && !rotating && !sizingHeld &&
               !st.pendingGuide.has_value() && route == StrokeRoute::None &&
               (strokeTarget != nullptr || eraseTool || pencilTool || tonalTool ||
                smudgeTool || cloneTool)) {
      // The refusals worth saying out loud. The route table sends each of them
      // nowhere rather than falling through to the solver, because falling
      // through would put paint on the *canvas* when the user aimed at a layer
      // -- and the user has to be told, or the brush just silently stops
      // working.
      //
      // Two messages, because there are two problems and only one of them has a
      // fix the user can carry out. A locked layer is a switch in LAYERS; a
      // layer kind with nowhere to put paint is not something clearing a lock
      // will help with, and telling someone to clear a lock they have not set
      // is worse than telling them nothing.
      //
      // **The eraser still needs its own sentences, but no longer for the same
      // reason.** They used to differ because a Pigment layer was where an erase
      // did *not* go, so handing the eraser the brush's "Pick a Pigment or RGB
      // layer" would have sent the user to the one kind that refused the
      // gesture. `brush/PigmentErase` closed that row, and the two tools now
      // reach the same two kinds -- so the eraser's kind sentence says the same
      // thing in its own verb rather than naming a different destination. What
      // is still its alone is the **no target at all** case, where the brush
      // would have gone to the solver and the eraser cannot: `sim::PaintSim` has
      // no alpha and no erase step (app/StrokeSession.hpp §1).
      //
      // **The pencil needs its own kind sentence and its own no-target one**,
      // and for a reason neither of the other two has: it is the only tool
      // here that refuses a layer kind the brush *accepts*. Handing it the
      // brush's "Pick a Pigment or RGB layer" would send a user to the exact
      // kind that just refused them. app/StrokeSession.hpp §1's Pencil rows
      // are the argument; this is that argument's sentence.
      // **Dodge and Burn need their own sentences too, and for one reason the
      // eraser's rows do not have.** Their refusal on a Pigment layer is a
      // DECISION rather than a missing store (app/StrokeSession.hpp §1's
      // Dodge/Burn rows: a `Latent` premultiplied by mass is not a
      // display-referred colour, and the two meaningful operations already have
      // their own tools), so a user who reads "cannot be adjusted" there and
      // waits for the feature to land would be waiting for something that is
      // not coming. That row says which tool they actually want.
      // Each tool that refuses a layer kind the BRUSH accepts needs its own
      // sentence, or the shared "Pick a Pigment or RGB layer" sends the user to
      // a kind that just refused them. The pencil, the tonal pair and the
      // smudge are all in that position; the eraser is here for the older
      // reason its own paragraph gives.
      const char* tonalVerb = st.brush.tool == Tool::Burn ? "burned" : "dodged";
      g_strokeRefusal =
          strokeTarget == nullptr
              ? (pencilTool
                     ? std::string("no layer: the pencil has nothing to draw on. Open a "
                                   "document, or add a layer in LAYERS.")
                 : tonalTool
                     ? std::string("no layer: there is no tone to adjust. Open a "
                                   "document, or add a layer in LAYERS.")
                 : smudgeTool
                     ? std::string("no layer: the smudge has nothing to move. Open a "
                                   "document, or add a layer in LAYERS.")
                 : cloneTool
                     ? std::string("no layer: there is nothing to clone onto. Open a "
                                   "document, or add a layer in LAYERS.")
                     : std::string("no layer: the eraser has nothing to erase. Open a "
                                   "document, or add a layer in LAYERS."))
          : strokeTarget->locked
              ? std::string("locked layer: \"") + strokeTarget->name + "\" cannot be " +
                    (eraseTool    ? "erased"
                     : pencilTool ? "drawn on"
                     : tonalTool  ? tonalVerb
                     : smudgeTool ? "smudged"
                     : cloneTool  ? "cloned onto"
                                  : "painted") +
                    ". Clear its Lock in LAYERS."
          // The Pigment row, Dodge and Burn's alone: an answer, not a gap.
          : (tonalTool && strokeTarget->kind == LayerKind::Pigment)
              ? std::string("\"") + strokeTarget->name +
                    "\" is Pigment: its texels hold pigment and mass, not tone. Use the "
                    "eraser for less paint, or the colour picker for a lighter one -- or "
                    "pick an RGB layer in LAYERS."
          : tonalTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) + " and cannot be " + tonalVerb +
                    ". Pick an RGB layer in LAYERS."
          // Alpha lock refuses the two routes that MOVE alpha -- erase and
          // smudge. Pencil, clone and tonal all deliberately take a locked
          // layer, so naming them here would block an edit the lock has no
          // quarrel with. app/StrokeSession.cpp's identical test is what
          // actually refused the stroke; this is that refusal's sentence.
          : ((eraseTool || smudgeTool) && strokeTarget->kind == LayerKind::RGB &&
             strokeTarget->alphaLocked)
              ? std::string("alpha locked: \"") + strokeTarget->name + "\" cannot be " +
                    (smudgeTool ? "smudged" : "erased") +
                    ". That moves alpha, and this layer's alpha is locked -- painting still "
                    "works. Clear Lock Transparent Pixels in LAYERS."
          : eraseTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be erased. Pick a Pigment or RGB layer in LAYERS."
          : pencilTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be drawn on with the pencil. The pencil draws a hard-edged "
                    "alpha mark; only an RGB layer has alpha. Pick an RGB layer in LAYERS."
          : smudgeTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be smudged. Smudging drags colour AND alpha together, which "
                    "only an RGB layer holds. Pick an RGB layer in LAYERS."
          : cloneTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be cloned onto. Pick an RGB layer in LAYERS."
              : std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be painted. Pick a Pigment or RGB layer in LAYERS.";
    } else if (paintTool && down && hovered && inside && !panning && !rotating && !sizingHeld &&
        !st.pendingGuide.has_value()) {
      // 1.4 / ADR-0001: this is the first moment a paint tool actually
      // deposits, so it's where PaintSim gets constructed if it doesn't
      // exist yet -- everything above (hover, tool selection, sliders,
      // even the tilt pad) never needed a live sim. A fresh construction
      // starts in PaintSim's default Watercolour mode; if the user already
      // picked a different medium from the Medium menu before ever
      // painting, honour that choice now instead of silently starting
      // Watercolour and waiting for a mode switch that already happened.
      // `canvasDims.w`/`.h` (this document's own size, or the fallback if
      // none is open -- `canvasDimensionsFor()`), not the raw `canvasW`/
      // `canvasH` parameters: this is the FIRST construction of the solver
      // for the whole process, and `ensurePaintSim()` never resizes it
      // again after (its own early `if (sim) return sim.get();`), so
      // whatever size is handed to it here is what the paper quad is stuck
      // at until the app quits. Sizing it to the document that is actually
      // open when the user's first Watercolour/Oil stroke lands is strictly
      // better than always defaulting to `kCanvasW`/`kCanvasH` regardless of
      // what that document is -- it closes the common single-document
      // session -- but it does NOT make the solver follow a LATER Canvas
      // Size or a document switch; see the "paper underneath it can still
      // be stretched" comment above, a few hundred lines up in this same
      // block, for the gap that remains.
      //
      // `paintSimDimensionsFor()` and NOT `canvasDims` (the DISPLAY size,
      // computed at the top of this block): the two questions look identical
      // and are not. A quad costs nothing to make 4000x3000; `allocFields()`
      // costs 176 bytes per texel to match it, 272 once Ink is in play. So
      // the document's size goes through the solver's own budget check
      // before it reaches the allocator, or one brush-down on a large
      // document asks the driver for gigabytes it will refuse. That
      // function's header carries the arithmetic and the 512 MB budget it is
      // measured against.
      const bool wasNull = !sim;
      const CanvasDimensions simDims =
          paintSimDimensionsFor(st.documents.active(), canvasW, canvasH);
      PaintSim* s = ensurePaintSim(sim, gpu, static_cast<uint32_t>(simDims.w),
                                   static_cast<uint32_t>(simDims.h), lut);
      if (s && wasNull && st.mode != PaintMode::Watercolor) s->setMode(gpu, st.mode);

      if (s) {
        st.paintingThisFrame = true;
        if (!st.strokeActive) {
          st.strokeActive = true;
          st.lastX = tx;
          st.lastY = ty;
          st.lastDabX = tx;
          st.lastDabY = ty;
          // A fresh stroke recharges the oil brush with the selected paint,
          // and resets the arc-length emitter -- leftover distance and point
          // history from whatever stroke happened before must not bleed
          // into this one.
          st.sim.brushReload = 1;
          st.strokePath.reset();
        } else {
          st.sim.brushReload = 0;
        }
        // The link set, not two literals -- the same resolution
        // app/StrokeSession's brushTipFor() does, so the solver route and the
        // CPU deposit route cannot drift apart. This used to be a copy of the
        // two curves; now both read `st.brush.links`.
        const DynamicResult dyn = evaluateLinks(st.brush.links, dynamicInputsFor(st));
        const float sizeMul = dyn.at(DynamicTarget::Size);
        const float flowMul = dyn.at(DynamicTarget::Flow);

        applyToolToBrush(st);
        // The floor, applied once, right here -- `evaluateLinks()` just above
        // already resolves the WHOLE link set in one call (this route has no
        // separate per-dab stroke-local pass the way `app/StrokeSession`
        // does: `sim::PaintSim` advances by frame, not by dab), so `sizeMul`
        // above is already the complete product and this is its one and only
        // multiply to floor. `st.brush.links.multiplyFloor[Size]` is
        // `brush/Dynamics.hpp`'s own per-target floor, in the target's [0,1]
        // units; `st.brush.radius` is the unscaled base radius the fraction
        // is against -- the same pairing `app/StrokeSession.cpp`'s
        // `brushTipFor()` computes into `BrushTip::sizeFloorPx`, restated
        // here because this route never builds a `BrushTip` to carry it on.
        // See that field's own comment (brush/Deposit.hpp) for the worked
        // counter-example proving a floor belongs at the LAST multiply, not
        // folded into an intermediate one.
        //
        // `st.brush.radius`/`spacing` are gone (Part 5) -- this SOLVER
        // route (`sim::PaintSim`, not the CPU deposit `brushTipFor()`/
        // `StrokeSession` migrated elsewhere in this change) still reads
        // `st.brush.links` directly, unmigrated and out of this task's own
        // scope; only the two deleted scalars are redirected here, to their
        // `model.tip` projections, with no other change to this route's
        // behaviour.
        const float baseRadius = st.brush.model.tip.diameterPx / 2.0f;
        st.sim.brushRadius =
            std::max(baseRadius * sizeMul,
                     baseRadius *
                         st.brush.links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)]);
        st.sim.brushPigment *= flowMul;
        st.sim.brushWater *= flowMul;

        // Arc-length dab emission (1.3 / ADR-0003): feed this frame's
        // sampled position through the centripetal Catmull-Rom emitter. It
        // returns 0..N dab positions spaced `spacing * radius` px apart
        // along the smoothed path, carrying any leftover sub-spacing
        // distance across frames so spacing stays continuous rather than
        // resetting on every render frame.
        //
        // `spacingPercent` is a percentage OF THE DIAMETER; this route wants
        // a fraction OF THE RADIUS (same "radii" unit the old, now-deleted
        // `st.brush.spacing` scalar always held) -- `/100 * 2` is that
        // conversion, identically to `app/StrokeSession::brushTipFor()`'s
        // own `tip.spacing` (its comment names the same doubling).
        const float spacingPx =
            std::max(st.brush.model.tip.spacingPercent / 100.0f * 2.0f * st.sim.brushRadius, 0.1f);
        st.strokePath.addPoint(tx, ty, spacingPx, st.pendingDabs);
        applyDabsToOilSegment();

        st.lastX = tx;
        st.lastY = ty;
      }
    } else if (!down) {
      if (st.strokeActive) {
        // Pen-up: flush whatever tail segment the emitter was still holding
        // back for lack of a real "next" sample to confirm its shape --
        // otherwise the last stretch of a stroke (up to one spacing
        // interval) silently deposits nothing.
        const float spacingPx = std::max(st.brush.model.tip.spacingPercent / 100.0f * 2.0f * st.sim.brushRadius, 0.1f);
        st.strokePath.flush(spacingPx, st.pendingDabs);
        applyDabsToOilSegment();
      }
      st.strokeActive = false;
      // The CPU route's pen-up. Separate from `strokeActive`, which is the
      // solver's flag: the two routes never run at once (the route is decided
      // per frame and one branch above is taken), but a stroke that began on
      // one and was interrupted -- window blur, a layer deleted mid-stroke --
      // must still be ended exactly once. `end()` records the single history
      // entry, and records nothing at all if the stroke deposited nothing.
      if (g_stroke.active()) {
        // Read before `end()` rather than after, so the line reports the route
        // of the stroke that is ending and not whatever a later `begin()`
        // happens to leave behind -- the same discipline the counters below
        // rely on, which `end()` deliberately does not clear.
        const char* routeName = strokeRouteName(g_stroke.route());
        g_stroke.end();
        std::printf("[stroke] %s (%s): %zu dabs, %zu texels, %zu tiles\n",
                    g_stroke.label().c_str(), routeName, g_stroke.dabCount(),
                    g_stroke.texelsWritten(), g_stroke.strokeTiles().size());
      }
    }

    // --- grid + guides overlay (PRD Q7, Q5) -- drawn once, after every
    // input-driven update above has settled for this frame (guide commit,
    // pan/zoom/rotate), so a guide committed this same frame draws
    // immediately rather than lagging a frame behind. ---
    if (st.showGrid) drawGridOverlay(dl, xform, texW, texH, st.gridSpacing, st.gridSubdivisions,
                                     st.view.zoom);

    // --- the selection, and the gesture in progress (PRD E6) --------------
    //
    // After the grid so it sits on top: the selection is the thing the user is
    // currently acting on, and a grid line crossing the ants would read as
    // part of the boundary.
    //
    // **Two different things, drawn two different ways, and keeping them apart
    // is what this block is for:**
    //
    //   The COMMITTED selection is a real `Selection`, and what is drawn is
    //   that store's TRUE boundary (core/SelectionBoundary) -- every island,
    //   every hole, every concave corner. It used to be the bounding box, which
    //   drew a lasso as a rectangle and a Shift-add of two disjoint shapes as
    //   one box round both.
    //
    //   The GESTURE IN PROGRESS has no `Selection` behind it at all: every
    //   constructor in core/SelectionShapes allocates tiles, so the shape is
    //   built once at mouse-up rather than 120 times a second (the selection
    //   tools block above says so in those words). Its preview is therefore
    //   drawn from the raw gesture points -- the drag corners for the two
    //   marquees, the accumulated path for the two lassos.
    //
    // The committed outline is drawn even mid-gesture, which the previous
    // `else` did not do. A Shift-add has to show what it is adding to; hiding
    // the existing ants for the duration of the drag is half of what made
    // "shift just draws another rectangle" look true.
    if (OpenDocument* selOd = st.documents.active(); selOd != nullptr) {
      // Revision-keyed, so this costs a hash-free key compare on every frame
      // the selection has not changed. core/SelectionBoundary.hpp has the
      // measurement that makes the cache necessary rather than tidy.
      drawMarchingAnts(dl, xform,
                       st.selectionBoundary.boundaryFor(
                           selOd->selection.has_value() ? &*selOd->selection : nullptr,
                           selOd->id, selOd->selectionRevision));
    }

    // --- Free Transform's gizmo ------------------------------------------
    //
    // Drawn here, near the end of the canvas block, so it reads over both the
    // picture and the selection ants. Its INPUT was claimed at the top of the
    // block, several hundred lines above: the two halves sit apart on purpose,
    // because the ordering constraints are opposite -- input has to come
    // before the tools so a move-drag cannot also paint, and the drawing has
    // to come after them so nothing paints over the affordance the user is
    // aiming at.
    // `transformActive`, not `st.transform.active()`: it already carries the
    // "and this is the document that session belongs to" half of the question
    // (see its own comment where the input was claimed). Reusing it is also
    // what keeps the drawn box and the claimed input from ever disagreeing
    // about whether the gizmo is live on this canvas.
    if (transformActive) {
      const float zoomNow = std::max(st.view.zoom, 0.01f);
      const TransformHandlePositions h =
          st.transform.handlePositions(kTransformRotateReachPx / zoomNow);
      auto toScr = [&](Point2 p) {
        const Vec2 s = xform.toScreen(Vec2{p.x, p.y});
        return ImVec2(s.x, s.y);
      };
      const ImU32 line = atelierToken(kAccent);
      const ImVec2 tl = toScr(h.topLeft);
      const ImVec2 tr = toScr(h.topRight);
      const ImVec2 br = toScr(h.bottomRight);
      const ImVec2 bl = toScr(h.bottomLeft);

      // --- T14: the live pixel preview, under the wireframe ------------------
      //
      // `g_transformPreview` was uploaded ONCE, at this session's `begin*()`
      // (`beginTransformPreview()`), from the untouched source -- nothing
      // here re-reads a tile or re-resamples anything, every frame. What
      // moves each frame is only where this same texture is DRAWN: the same
      // four corners (`tl`/`tr`/`br`/`bl`) the wireframe box below is about
      // to outline, already mapped through `pending()` by `handlePositions()`
      // above, so the pixels and the box they sit inside cannot draw apart
      // even by a rounding difference -- one `TransformHandlePositions` feeds
      // both. `addCanvasQuad()`'s GPU rasteriser supplies the "cheap kernel"
      // docs/testing-issues.md's T14 entry asks for (ui/CanvasQuad.cpp's own
      // minify-linear/magnify-nearest sampler) as a side effect of drawing an
      // ordinary textured quad -- there is no CPU resample in this file at
      // all, which is what keeps a 2048x2048 document's drag frame cheap (see
      // this step's own cost measurement).
      //
      // Drawn only when `view() != nullptr`: null for a Pigment layer's
      // whole-layer transform (ui/TransformPreviewTexture.hpp's own named
      // scope reduction) and for a session whose upload has not landed yet,
      // in either of which the wireframe-only box below is everything this
      // step draws, exactly as it did before this file existed.
      //
      // **Known and accepted**: the document composite drawn earlier in this
      // same canvas block still shows this layer's UNTRANSFORMED content at
      // its ORIGINAL position underneath this quad -- nothing was written to
      // the document, so there is nothing else for that composite to show.
      // ui/TransformPreviewTexture.hpp's own header names why hiding it is
      // out of scope (it would cost a full per-frame recomposite,
      // ui/DocumentTexture.hpp's own measured 22-89 ms, well past this
      // quad's own near-zero cost and past PRD F3 on its own). What is on
      // screen during a drag is therefore the original in place PLUS the
      // live preview at its new position -- strictly more information than
      // the wireframe box alone gave, which is the bar this step sets, not a
      // claim that the drag view is pixel-identical to Photoshop's.
      if (g_transformPreview.view() != nullptr)
        addCanvasQuad(dl, g_transformPreview.view(), tl, tr, br, bl);

      // --- the layers ABOVE the transformed one, back in front -------------
      //
      // Drawn last of the three, over the moving pixels, which is the whole
      // point of the split: a layer that sits above the one being dragged
      // belongs in front of it while it is dragged, not behind it. Only taken
      // when `transformSplitDraws` said the arrangement is exact and that
      // there is something above to draw -- otherwise the layers above are
      // already in the composite underneath and this would draw them twice.
      // Same canvas corners as the document quad, because this half occupies
      // the same canvas; only its content differs.
      if (transformSplitDraws && views.valid) {
        const WGPUTextureView aboveView =
            transformAboveTexture.viewFor(gpu, views.above, nullptr, &docViewport, aboveVariant);
        if (aboveView != nullptr) addCanvasQuad(dl, aboveView, q00, q10, q11, q01);
      }

      // Four segments rather than `AddRect`: once the pending matrix carries a
      // rotation the box is no longer axis-aligned, and `AddRect` would draw
      // its bounding box -- a rectangle that is not where the pixels are
      // going. The handles come from the same `TransformHandlePositions` the
      // hit test used, so what is drawn and what is clickable cannot drift.
      // ===== Tool::Move -- BEGIN: no box, no handles ======================
      // A Move drag runs the same session, so everything above -- the hidden
      // layer, the preview quad, the layers-above split -- is shared and must
      // keep drawing. The affordances are not: Move offers no scale, no
      // rotate and no click-a-handle-to-grab, so drawing nine handles the
      // gesture cannot use would be advertising four operations that are not
      // there. The box goes with them: without handles it is a marquee-
      // coloured rectangle around the picture with no meaning of its own, and
      // the moving pixels themselves are already the feedback.
      if (!g_moveDragging) {
        // ===== Tool::Move -- END ==========================================
        dl->AddLine(tl, tr, line, kRuleThickness);
        dl->AddLine(tr, br, line, kRuleThickness);
        dl->AddLine(br, bl, line, kRuleThickness);
        dl->AddLine(bl, tl, line, kRuleThickness);
        dl->AddLine(toScr(h.topCenter), toScr(h.rotate), line, kRuleThickness);

        const float r = kTransformHandleDrawPx * 0.5f;
        auto square = [&](ImVec2 c) {
          // Filled with paper and outlined, not filled with the accent: a solid
          // accent square on a dark picture and on a light one are different
          // amounts of visible, and the outline is what makes it read on both.
          dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r),
                            atelierToken(kCanvasPaper));
          dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), line, 0.0f, 0, 1.0f);
        };
        for (const Point2 p : {h.topLeft, h.topCenter, h.topRight, h.middleLeft, h.middleRight,
                               h.bottomLeft, h.bottomCenter, h.bottomRight})
          square(toScr(p));
        // A disc, not a ninth square. It does something the eight do not, and
        // shape is how that reads at 7 px without a label.
        const ImVec2 rot = toScr(h.rotate);
        dl->AddCircleFilled(rot, r, atelierToken(kCanvasPaper));
        dl->AddCircle(rot, r, line, 0, 1.0f);
      }
    }

    if (st.marqueeDragging &&
        (st.brush.tool == Tool::Marquee || st.brush.tool == Tool::EllipseMarquee)) {
      // The live rubber band. T10: `marqueeBoxX0..Y1` is this frame's
      // computeSelectionDragBox() result, already carrying Shift-constrain/
      // Option-from-centre/Space-move -- not the drag's raw corners -- so
      // this preview and what mouse-up actually commits can never disagree.
      //
      // T13: which SHAPE that box is walked as still has to match the tool
      // -- the box alone does not say. Both branches read the identical
      // `marqueeBoxX0..Y1`; only the overload differs.
      if (st.brush.tool == Tool::EllipseMarquee) {
        drawMarchingAnts(dl, xform, st.marqueeBoxX0, st.marqueeBoxY0, st.marqueeBoxX1,
                         st.marqueeBoxY1, kEllipseMarqueePreviewSegments);
      } else {
        drawMarchingAnts(dl, xform, st.marqueeBoxX0, st.marqueeBoxY0, st.marqueeBoxX1,
                         st.marqueeBoxY1);
      }
    } else if (st.gradientDrag.active) {
      // The gradient's own rubber band: a LINE from where the pen went down
      // to where it is now, which is literally the geometry being aimed
      // (`app/GradientTool.hpp` § 6 -- pen-down is t=0, the pointer is t=1).
      //
      // **This arm also closes a bug that was here before the gradient had
      // any band at all.** A gradient drag sets `marqueeDragging` -- it is the
      // third setter of that flag, after the two marquee tools and the lasso
      // -- so with no arm of its own it fell into the branch below and drew
      // `st.lassoPoints`: the stale outline of whatever lasso the user drew
      // last, hanging in the air during a gradient drag, pinned to nothing.
      // Exactly the failure this file's next comment describes the lasso
      // itself having suffered, one tool later. A flag shared by three
      // gestures needs three arms, and "everything that is not a marquee is a
      // lasso" stops being true the moment a third tool sets it.
      //
      // Drawn like the Measure ruler rather than as marching ants, for the
      // ruler's own stated reason: ants mean "this region is selected"
      // everywhere else in this build, and this line selects nothing. It is a
      // direction.
      const Vec2 a = xform.toScreen(Vec2{st.gradientDrag.x0, st.gradientDrag.y0});
      const Vec2 b = xform.toScreen(Vec2{st.gradientDrag.x1, st.gradientDrag.y1});
      // A dark casing under the accent core. The line is drawn over the
      // user's own picture at whatever colour that happens to be, and the
      // ruler gets away with a bare accent stroke because it is used against
      // a canvas the user is measuring rather than one they are actively
      // filling -- while this band sits on top of the very gradient it is
      // aiming, whose bright end can match the accent closely enough to
      // erase it.
      dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), IM_COL32(0, 0, 0, 160),
                  kRuleThickness + 2.0f);
      dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), atelierToken(kAccent), kRuleThickness);
      // The two ends are not the same thing and are not drawn the same: a
      // hollow ring at t=0 and a filled disc at t=1, so a drag whose
      // direction matters can be read at a glance without moving the pointer.
      dl->AddCircle(ImVec2(a.x, a.y), 4.0f, IM_COL32(0, 0, 0, 160), 0, 3.0f);
      dl->AddCircle(ImVec2(a.x, a.y), 4.0f, atelierToken(kAccent), 0, 1.5f);
      dl->AddCircleFilled(ImVec2(b.x, b.y), 4.0f, IM_COL32(0, 0, 0, 160));
      dl->AddCircleFilled(ImVec2(b.x, b.y), 3.0f, atelierToken(kAccent));

      // **What the two handles mean is not the same for all three kinds, so
      // neither is the band.** A bare line says "from here to there", which is
      // true for Linear and misleading for the other two: a Radial drag is a
      // RADIUS and an Angular drag is a zero-angle RAY whose length means
      // nothing at all. Drawing one shape for three geometries would be the
      // marquee/lasso mistake again -- one preview standing in for gestures
      // that differ -- so each kind gets the mark that states its own rule.
      const float rimR = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      if (st.gradient.kind == GradientKind::Radial && rimR > 1.0f) {
        // The t=1 circle. Screen-space radius is the screen-space handle
        // distance because `xform` is uniform zoom plus rotation plus mirror
        // -- all conformal, so a circle stays a circle and there is no ellipse
        // to construct. A non-uniform scale would break that, and this build
        // has none (ui/ViewTransform).
        //
        // The segment count is given explicitly rather than left at 0.
        // ImGui's auto-tessellation is bounded by `CircleTessellationMaxError`
        // in LOGICAL pixels, and on a 2x display that error is doubled on the
        // way to the framebuffer -- a large rim came out visibly faceted,
        // straight-edged enough to read as a polygon someone drew on purpose.
        // Scaled with the radius and capped, so a rim larger than the window
        // does not cost a thousand segments for arcs nobody can see.
        const int rimSegs = std::clamp(static_cast<int>(rimR * 0.5f), 32, 256);
        dl->AddCircle(ImVec2(a.x, a.y), rimR, IM_COL32(0, 0, 0, 160), rimSegs, 3.0f);
        dl->AddCircle(ImVec2(a.x, a.y), rimR, atelierToken(kAccent), rimSegs, 1.5f);
      } else if (st.gradient.kind == GradientKind::Angular && rimR > 1.0f) {
        // A short arc leaving the ray, showing which way the sweep goes.
        //
        // Worth the twelve lines: the direction is CLOCKWISE ON SCREEN, and
        // `ops/Gradient.hpp` is explicit that this is a consequence of
        // document space being y-down rather than a preference -- which is
        // precisely the kind of fact that is invisible until the first
        // gradient comes out mirrored. Drawn at a fixed screen radius, not a
        // fraction of the drag: the drag's length means nothing to this kind,
        // so scaling the hint by it would imply otherwise.
        const float a0 = std::atan2(b.y - a.y, b.x - a.x);
        constexpr float kHintR = 26.0f;
        constexpr float kHintSweep = 1.0f;  // radians, ~57 degrees
        if (rimR > kHintR * 1.25f) {
          dl->PathArcTo(ImVec2(a.x, a.y), kHintR, a0, a0 + kHintSweep, 24);
          dl->PathStroke(IM_COL32(0, 0, 0, 160), 0, 3.5f);
          dl->PathArcTo(ImVec2(a.x, a.y), kHintR, a0, a0 + kHintSweep, 24);
          dl->PathStroke(atelierToken(kAccent), 0, 1.5f);
          // The arrowhead, so the arc reads as a direction rather than as a
          // decorative tick. Two short strokes back from the arc's far end.
          const float ae = a0 + kHintSweep;
          const ImVec2 tip(a.x + std::cos(ae) * kHintR, a.y + std::sin(ae) * kHintR);
          const float back = ae - 0.30f;
          const ImVec2 inner(a.x + std::cos(back) * (kHintR - 5.0f),
                             a.y + std::sin(back) * (kHintR - 5.0f));
          const ImVec2 outer(a.x + std::cos(back) * (kHintR + 5.0f),
                             a.y + std::sin(back) * (kHintR + 5.0f));
          dl->AddLine(tip, inner, atelierToken(kAccent), 1.5f);
          dl->AddLine(tip, outer, atelierToken(kAccent), 1.5f);
        }
      }
    } else if (st.marqueeDragging || st.polygonLassoActive) {
      // The lasso path as it is being drawn.
      //
      // This branch previously did not exist, and the marquee's rubber band ran
      // in its place: `marqueeDragging` is set by the freehand lasso too, while
      // `marqueeX0..Y1` are only ever written by the two marquee tools. So a
      // lasso drag drew the STALE rectangle left behind by whatever marquee was
      // dragged last -- which is, precisely and literally, "the lasso only
      // draws rectangular selections".
      //
      // Drawn OPEN rather than closed. `selectPolygon()` does close the path
      // when the gesture ends, but showing the closing chord while the hand is
      // still moving would draw a line the user has not made yet across the
      // middle of their own shape.
      std::vector<Vec2> pts;
      pts.reserve(st.lassoPoints.size() + 1);
      for (const SelectionPoint& p : st.lassoPoints) pts.push_back(Vec2{p.x, p.y});
      // The polygon lasso's path only moves on clicks, so without the pointer
      // appended the segment being placed is invisible until it is committed.
      // The freehand lasso needs nothing of the kind -- its last point already
      // tracks the pointer within a texel.
      if (st.polygonLassoActive && hovered) pts.push_back(Vec2{tx, ty});
      float lassoPhase = marchingAntPhase();
      drawAntPolyline(dl, xform, pts, /*closed=*/false, lassoPhase);
    }

    // === BEGIN Tool::Measure ruler (app/MeasureLine) =======================
    //
    // Drawn from `xform`, like the marquee band and the transform wireframe
    // above, so the line sits on the picture and follows every zoom, pan,
    // rotation and mirror without knowing that any of them exist.
    //
    // Not marching ants: ants mean "this region is selected" everywhere else
    // in this build, and a ruler selects nothing. A solid accent line with a
    // tick at each end is the transform wireframe's own vocabulary, which
    // already means "chrome over the picture, not part of it".
    //
    // `measureLineAppliesTo()` is what keeps a ruler dragged on one document
    // from being drawn over another (app/MeasureLine.hpp §1).
    if (toolMeasuresCanvas(st.brush.tool)) {
      const OpenDocument* measureDoc = st.documents.active();
      if (measureLineAppliesTo(st.measure, measureDoc != nullptr ? measureDoc->id : 0u)) {
        const ImU32 rulerCol = atelierToken(kAccent);
        const Vec2 a = xform.toScreen(Vec2{st.measure.x0, st.measure.y0});
        const Vec2 b = xform.toScreen(Vec2{st.measure.x1, st.measure.y1});
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), rulerCol, kRuleThickness);
        // 3 px discs rather than the transform box's squares: the endpoints
        // are not handles, nothing can be grabbed, and shape is the only
        // thing carrying that distinction at this size.
        dl->AddCircleFilled(ImVec2(a.x, a.y), 3.0f, rulerCol);
        dl->AddCircleFilled(ImVec2(b.x, b.y), 3.0f, rulerCol);
      }
    }
    // === END Tool::Measure ruler ===========================================

    // === BEGIN Tool::CloneStamp source marker ==============================
    //
    // The report this answers, verbatim: "clone needs to show an indicator of
    // where it is cloning from, Opt click should set that anchor with an
    // indicator of where it was put and drawing with the clone tool should
    // move that indicator to show what is currently being cloned."
    //
    // **Two marks, not one, because they are two different points.** Until a
    // stroke has fixed the offset the source IS the anchor; from the first
    // pen-down onward the source is `pointer + offset` and the anchor is only
    // where the copy started. Drawing one shape for both would be the
    // marquee/lasso mistake the gradient's own rubber band records a few
    // hundred lines above: one preview standing in for gestures that differ.
    //
    // Everything read here is already correct and already session state
    // (`AppState::CloneSourceState`); this block adds no state and changes no
    // behaviour. `--selftest` cannot reach it -- it is a canvas draw block --
    // so it is covered by `tools/golden/run_golden.sh`'s `clone_anchor` and
    // `clone_source` views instead, and by nothing else.
    //
    // **Drawn only while the Clone Stamp is the selected tool**, the same rule
    // `toolMeasuresCanvas()` gives the ruler directly above and the same rule
    // every other tool-owned mark on this canvas follows. The alternative --
    // always showing it, on the grounds that the source survives a tool switch
    // and hiding live state is a lie -- was rejected because the thing being
    // hidden is not lost and is one palette click from being back, while the
    // cost of the other choice is a crosshair sitting permanently on someone's
    // painting with no control anywhere in this build to dismiss it. A mark a
    // user cannot turn off had better be about the tool in their hand.
    //
    // `transformActive` is folded in through `cloneTool` itself: a transform
    // owns the canvas while it runs, and its wireframe is what the pointer is
    // acting on.
    if (cloneTool && st.clone.haveAnchor) {
      // Halo-under-accent, the gradient rubber band's own vocabulary. A single
      // accent stroke is invisible over paint the same hue, and the clone's
      // whole subject is the picture underneath -- so the mark carries its own
      // contrast rather than assuming the document will supply it.
      const ImU32 haloCol = IM_COL32(0, 0, 0, 160);
      const ImU32 markCol = atelierToken(kAccent);

      // --- the anchor: a fixed-size crosshair ------------------------------
      //
      // Fixed in SCREEN px, not scaled by zoom, and that is the distinction
      // being drawn rather than a shortcut. An anchor is a POINT -- it has no
      // extent, so a mark that grew with the zoom would be claiming one. The
      // gap in the middle is what keeps the texel it names visible; a solid
      // cross would cover the one pixel the user clicked to choose.
      const Vec2 anchorScr = xform.toScreen(st.clone.anchor);
      const ImVec2 ac(anchorScr.x, anchorScr.y);
      constexpr float kTickGap = 3.5f;
      constexpr float kTickLen = 10.0f;
      for (int pass = 0; pass < 2; ++pass) {
        const ImU32 col = pass == 0 ? haloCol : markCol;
        const float w = pass == 0 ? 3.0f : 1.5f;
        dl->AddLine(ImVec2(ac.x - kTickLen, ac.y), ImVec2(ac.x - kTickGap, ac.y), col, w);
        dl->AddLine(ImVec2(ac.x + kTickGap, ac.y), ImVec2(ac.x + kTickLen, ac.y), col, w);
        dl->AddLine(ImVec2(ac.x, ac.y - kTickLen), ImVec2(ac.x, ac.y - kTickGap), col, w);
        dl->AddLine(ImVec2(ac.x, ac.y + kTickGap), ImVec2(ac.x, ac.y + kTickLen), col, w);
      }

      // --- the live source: a ring the size of what is actually read -------
      //
      // **Gated on `haveOffset`, not on `g_stroke.active()`, and the choice is
      // load-bearing in both directions.**
      //
      // Not on a live stroke: `source = pointer + offset` is as true between
      // strokes as during one -- this build clones ALIGNED, so the offset that
      // is showing is exactly the offset the next stroke will use. Hiding the
      // mark on pen-up would make the indicator disappear at precisely the
      // moment a retoucher lifts off to check their aim.
      //
      // But on `haveOffset`: before the first pen-down the offset is (0, 0)
      // and means nothing, and the source for a stroke started under the
      // current pointer would be the ANCHOR (`latchCloneOffset()` derives
      // `anchor - penDown` for exactly that reason). So the crosshair above is
      // already the whole truth in that state, and a second ring drawn on top
      // of it would be a duplicate that starts lying the instant the pointer
      // moves.
      //
      // Radius is the brush tip's, in canvas space, so this ring is the region
      // that will actually be sampled rather than a decorative dot -- the same
      // number and the same reasoning as the brush cursor ring further down,
      // which says the same thing about the destination. The pair reads as
      // "this disc goes there", which is what a clone is.
      if (st.clone.haveOffset && hovered && inside) {
        const Vec2 srcDoc{tx + st.clone.offset.x, ty + st.clone.offset.y};
        const Vec2 srcScr = xform.toScreen(srcDoc);
        const Vec2 ptrScr = xform.toScreen(Vec2{tx, ty});
        const ImVec2 sc(srcScr.x, srcScr.y);
        const ImVec2 pc(ptrScr.x, ptrScr.y);
        const float srcR = std::max(4.0f, (st.brush.model.tip.diameterPx * 0.5f) * st.view.zoom);

        // The offset itself, drawn once. Dim and hairline deliberately: it is
        // the least important of the three things here (the two ends are what
        // the eye needs) and a full-weight line across the middle of the
        // picture would be chrome competing with the painting for attention.
        // Worth its two lines anyway -- without it the ring is an unexplained
        // circle somewhere else on the canvas, and the report's own words are
        // about the RELATIONSHIP ("where it is cloning FROM").
        dl->AddLine(sc, pc, IM_COL32(0, 0, 0, 70), 2.0f);
        // The accent at reduced alpha, DERIVED from the token rather than
        // retyped as a literal. `IM_COL32(255, 86, 60, 90)` is the same colour
        // today and is a hand-copy of `kAccent`'s 0xff563c, which is the drift
        // this file already avoids the same way two hundred lines up
        // (`kTextSecondary` masked and re-alpha'd for the overflow glyph): the
        // day the theme's accent moves, a literal keeps the old one and
        // nothing says so.
        dl->AddLine(sc, pc, (markCol & 0x00FFFFFFu) | IM_COL32(0, 0, 0, 90), 1.0f);

        // Segment count given explicitly for the reason the gradient's rim
        // circle gives: ImGui's auto-tessellation error budget is in LOGICAL
        // px and doubles on the way to a 2x framebuffer, so a large ring comes
        // out visibly faceted. Scaled and capped the same way.
        const int srcSegs = std::clamp(static_cast<int>(srcR * 0.5f), 24, 128);
        dl->AddCircle(sc, srcR, haloCol, srcSegs, 3.0f);
        dl->AddCircle(sc, srcR, markCol, srcSegs, 1.5f);
        // A filled centre dot, which the crosshair deliberately does not have:
        // at a glance the two marks differ by whether the middle is solid, and
        // that reads even when the ring is small enough to be nearly a dot
        // itself.
        dl->AddCircleFilled(sc, 2.5f, haloCol);
        dl->AddCircleFilled(sc, 1.5f, markCol);
      }
    }
    // === END Tool::CloneStamp source marker ================================

    if (st.showGuides) {
      constexpr ImU32 kGuideCol = IM_COL32(70, 190, 230, 200);
      constexpr ImU32 kPendingGuideCol = IM_COL32(140, 230, 255, 230);
      // Right-click near a guide removes it -- the only per-guide management
      // this step provides beyond bulk "Clear Guides" (View menu); repositioning
      // an existing guide isn't implemented (PRD Q5 only asks for the two
      // creation paths: drag-to-create and the numeric/percentage popup).
      // Only live while hovering the canvas and not mid-drag, so a stray
      // right-click elsewhere (or during guide creation) can't delete one.
      int guideToDelete = -1;
      if (hovered && !st.pendingGuide && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        constexpr float kPickPx = 5.0f;
        float bestDist = kPickPx;
        for (size_t i = 0; i < st.guides.size(); ++i) {
          const auto& g = st.guides[i];
          Vec2 a, b;
          if (g.orientation == GuideOrientation::Horizontal) {
            a = xform.toScreen(Vec2{0.0f, g.position});
            b = xform.toScreen(Vec2{texW, g.position});
          } else {
            a = xform.toScreen(Vec2{g.position, 0.0f});
            b = xform.toScreen(Vec2{g.position, texH});
          }
          const float d = distancePointToSegment(mouse, ImVec2(a.x, a.y), ImVec2(b.x, b.y));
          if (d < bestDist) {
            bestDist = d;
            guideToDelete = static_cast<int>(i);
          }
        }
      }
      if (guideToDelete >= 0)
        st.guides.erase(st.guides.begin() + guideToDelete);

      for (const auto& g : st.guides)
        drawGuideLine(dl, xform, g.orientation, g.position, texW, texH, kGuideCol);
      if (st.pendingGuide)
        drawGuideLine(dl, xform, st.pendingGuide->orientation, st.pendingGuide->position, texW,
                      texH, kPendingGuideCol);
    }

    // --- brush cursor ring ---
    //
    // The eraser gets one too, at the same radius and drawn the same way: it is
    // the brush's tip (ADR-0007), so the ring is telling the truth about the
    // footprint of the next dab whichever direction that dab moves the alpha.
    if (hovered && strokeTool && inside) {
      dl->AddCircle(mouse, (st.brush.model.tip.diameterPx / 2.0f) * st.view.zoom,
                    IM_COL32(235, 235, 225, 170), 32, 1.0f);
    }

    // --- the pointer itself (ui/ToolCursor) --------------------------------
    //
    // Beside the ring deliberately: the ring has been this application's only
    // cursor-like feedback since it was written, and it says one true thing --
    // how big the next dab is. Everything else the pointer could have said was
    // missing, and until this block the build made **no** cursor call at all.
    //
    // **This block sets no cursor. It records a request**, which
    // `SystemCursorTable::apply()` in main.cpp reads once at the end of the
    // frame -- ui/ToolCursor §6. The distinction is the whole safety argument:
    // this build now suppresses the ImGui SDL3 backend's own cursor handling
    // with `ImGuiConfigFlags_NoMouseCursorChange`, so there must be exactly one
    // writer, and a `SDL_SetCursor()` here would make two.
    //
    // **Scoped to the canvas, and nowhere else.** Over the panels, the menus
    // and the tool palette the cursor must stay whatever ImGui wants -- the
    // I-beam in the LAYERS filter box, the resize arrows on a window border --
    // and `apply()` gives them exactly that by falling back to
    // `ImGui::GetMouseCursor()` whenever this request is empty. So the gate
    // below is the only thing standing between a crosshair and a chrome that
    // feels broken, and it is the same `hovered` the painting, zooming and
    // picking above are gated on. That also means a popup or modal covering the
    // canvas suppresses this for free: ImGui refuses the hover under a popup,
    // so the tool cursor cannot leak out over a dialog.
    //
    // The band drawing below (`drawAtelierOptionsBar()` and the rest) happens
    // after this and may ask ImGui for cursors of its own -- harmlessly,
    // because those bands only do so when the pointer is over them, and the
    // pointer cannot be over the canvas and over a band at once.
    //
    // **Transient gestures beat the tool**, which is what the ordering here is
    // for. A guide being dragged off a ruler, a pan and a view rotation each
    // already own the drag they are in the middle of, and showing the tool's
    // cursor during one of them would describe a click the user is not about to
    // make. They are tested in the same order the input blocks above resolve
    // them, so the cursor cannot claim a gesture the canvas gave to something
    // else.
    if (hovered || canvasHeld) {
      if (st.pendingGuide.has_value()) {
        // The one place a resize cursor is literally correct rather than
        // approximately: the guide slides along one axis and the double-headed
        // arrow names which. A horizontal guide is a horizontal line that moves
        // vertically, so it takes the north-south arrow -- getting that pair
        // the wrong way round is the easy mistake here.
        //
        // Spelled as SDL shapes rather than as `ToolCursor` intents because
        // these are not tools: `ToolCursor` is what a *tool* means, and adding
        // guide-drag members to it would make that enum a grab-bag of shapes
        // and stop it being the thing a later bitmap layer can be written
        // against.
        g_canvasCursor = st.pendingGuide->orientation == GuideOrientation::Horizontal
                             ? SDL_SYSTEM_CURSOR_NS_RESIZE
                             : SDL_SYSTEM_CURSOR_EW_RESIZE;
      } else if (panning || rotating) {
        // Both drag the view rather than the document. `Pan` is the intent for
        // each: a rotation is still "the sheet follows the pointer", and SDL
        // has nothing that means rotation anyway.
        g_canvasCursor = sdlCursorFor(ToolCursor::Pan);
      } else if (transformActive) {
        // Free Transform owns the canvas here exactly as it does at the three
        // `!transformActive` guards above -- repositioning content, not the
        // view, which is `MoveObject`'s own definition. Without this branch
        // the cursor fell through to `st.brush.tool`'s ordinary cursor (e.g. a
        // crosshair, if Brush was active when Cmd+T was pressed), describing a
        // paint stroke the gizmo drag will not produce.
        g_canvasCursor = sdlCursorFor(ToolCursor::MoveObject);
        g_canvasBitmapTool = Tool::Move;
      } else {
        // The tool, against the layer it is actually pointed at -- so a brush
        // over a locked or unpaintable layer, and a bucket over a Pigment
        // layer, show the slashed circle *before* the gesture is spent rather
        // than a sentence in another band afterwards. `strokeTarget` is the
        // same active layer the stroke and fill blocks above route on, read
        // once for all three.
        //
        // Recorded BOTH ways: `g_canvasCursor` as the projected SDL shape
        // (§6's original contract, read whenever bitmaps are off or this tool
        // has none), and `g_canvasBitmapTool` as the tool itself (§7's
        // addition, read only to ask `shouldUseBitmapCursor()` whether a
        // bitmap should win instead). One call, one source of truth for both
        // -- never two opinions about which tool this frame's gesture means.
        //
        // **The tool is withheld on a refusal**, which is the whole reason
        // this is not just `st.brush.tool`: `toolCursorOnTarget()` answering
        // `Refuse` is the "this gesture will not land" case, and it has to
        // reach the user as the slashed circle rather than as a perfectly
        // ordinary brush icon over a locked layer.
        const ToolCursor cursor = toolCursorOnTarget(st.brush.tool, strokeTarget);
        g_canvasCursor = sdlCursorFor(cursor);
        if (cursor != ToolCursor::Refuse) g_canvasBitmapTool = st.brush.tool;
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();

  // ------------------------------------------------- the unfocused pane
  //
  // PRD **A5**'s second document, and it is deliberately **a view, not a
  // second editor**. No rulers, no guides, no navigator, no pan/zoom/rotate
  // and no painting: the whole document, fitted and centred, and a click that
  // focuses it.
  //
  // That is a scope decision rather than an omission, and the reason is
  // ownership. `CanvasView` -- zoom, pan, rotation, the two mirrors -- lives
  // once on `AppState`, not per document, and `sim::PaintSim` is one shared
  // canvas with no document binding at all. A second pane with its own
  // transform would need the first of those moved onto `OpenDocument`, which
  // is a change to a shared header this step does not own, and a second
  // *paintable* pane would need the second, which is the stroke bridge and is
  // not built. Fitting the whole document is the one honest thing a pane with
  // no view state of its own can show.
  //
  // **Focus is a swap, not a pointer.** Clicking here makes this document the
  // session's active one and hands the old active document to this pane, so
  // the panes stay where they are and the documents move between them. See
  // ui/AtelierChrome.hpp: every other surface in the application acts on
  // `DocumentSession::active()`, and a focus that did not move it would leave
  // the LAYERS panel describing a document the user was not looking at.
  if (splitActive) {
    const int otherPane = 1 - paneDocs.focusedPane;
    const AtelierRect r = panes.pane[otherPane];
    OpenDocument* other = paneDocs.pane[otherPane];
    ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
    ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          ImGui::ColorConvertU32ToFloat4(atelierToken(atelierSurround())));
    if (ImGui::Begin("##canvasPane2", nullptr, fixedFlags) && other != nullptr) {
      ImDrawList* pdl = ImGui::GetWindowDrawList();
      const float dw = static_cast<float>(other->document.width);
      const float dh = static_cast<float>(other->document.height);
      // 24 px of surround on every side, so the sheet reads as lying on the
      // desk rather than as a panel background that happens to be the paper.
      const float inset = 24.0f;
      const float fit = (dw > 0.0f && dh > 0.0f)
                            ? std::min((r.w - inset * 2.0f) / dw, (r.h - inset * 2.0f) / dh)
                            : 0.0f;
      if (fit > 0.0f) {
        const float sw = dw * fit, sh = dh * fit;
        const ImVec2 p0(r.x + (r.w - sw) * 0.5f, r.y + (r.h - sh) * 0.5f);
        const ImVec2 p1(p0.x + sw, p0.y + sh);
        pdl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 6), ImVec2(p1.x + 6, p1.y + 6),
                           IM_COL32(0, 0, 0, 110));
        // Blank paper, not PaintSim's canvas: that texture is the *focused*
        // session's single shared surface and drawing it here would show one
        // document's strokes under another document's layers.
        pdl->AddRectFilled(p0, p1, atelierToken(kCanvasPaper));
        // The residency rule doing its job in the one place a user can see it:
        // this call is what makes the second document *visible* in PRD A6's
        // sense, and it is the second and last slot the pool will ever give
        // out.
        //
        // The viewport passed is the WHOLE document -- this pane's own
        // design is "the whole document, fitted and centred" (see the
        // comment above this block), so nothing is ever off screen here and
        // decision 6 never defers anything for it. Passed anyway, rather
        // than nullptr, so a slot the OTHER call site left mid-backlog
        // (a document that was focused, is now in this pane) still catches
        // up through the same code path instead of silently falling back to
        // paying for its entire backlog in one call -- either is correct,
        // this is simply the one that keeps every production call site
        // making the same kind of request.
        const DocumentTextureViewport wholeDoc{0, 0, other->document.width,
                                               other->document.height};
        const WGPUTextureView v = g_documentTextures.viewFor(gpu, *other, nullptr, &wholeDoc);
        addCanvasImage(pdl, v, p0, p1);
        pdl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border));
      }
      pdl->AddText(ImVec2(r.x + 10.0f, r.y + 8.0f), atelierToken(kTextSecondary),
                   documentDisplayName(*other).c_str());

      ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
      if (ImGui::InvisibleButton("##focusPane2", ImVec2(r.w, r.h))) {
        const OpenDocument* wasActive = st.documents.active();
        const DocumentId incoming = other->id;
        g_split.companion = wasActive != nullptr ? wasActive->id : 0;
        g_split.focusedPane = otherPane;
        for (size_t i = 0; i < st.documents.count(); ++i) {
          const OpenDocument* d = st.documents.at(i);
          if (d != nullptr && d->id == incoming) {
            st.documents.setActive(i);
            break;
          }
        }
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    // Which pane is focused, marked the way the active tab is marked: a 2 px
    // accent rule along its leading edge.
    //
    // **This one genuinely needs the foreground list, unlike the band rules.**
    // It sits ON the pane window's own top edge rather than in a gap beside
    // it (ui/AtelierLayout gives the band rules their own gaps, which is what
    // let drawAtelierRules() move to the background list), so drawn behind,
    // the pane would simply cover it.
    //
    // The cost is the same one the band rules just shed: a popup overlapping
    // the top 2 px of the focused pane gets an accent line across it. Left as
    // is deliberately -- it is 2 px on one edge in a mode that is not the
    // default, where the band rules were full-height lines through every
    // flyout in the default layout. Drawing it into the pane's own draw list
    // is the real fix and needs the call moved inside that Begin/End pair.
    const AtelierRect f = panes.pane[paneDocs.focusedPane];
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2(f.x, f.y), ImVec2(f.right(), f.y + kRuleThickness), atelierToken(kAccent));
    // The rule between the panes, in the same grey every other region rule
    // uses -- and on the background list with them, because it is in a gap
    // for the same reason they are: `panes.divider` is carved out of the
    // canvas width, and neither pane's window is positioned over it.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(panes.divider.x, panes.divider.y),
        ImVec2(panes.divider.right(), panes.divider.bottom()), atelierToken(kRule));
  }

  // ------------------------------------------------------- the other bands
  //
  // **The tab strip is no longer drawn here.** It moved up into the title
  // row's own `BeginMainMenuBar()`/`EndMainMenuBar()` block, near this
  // function's top, when the two merged into one band -- see
  // `drawAtelierTabStrip()`'s declaration comment for why a window that now
  // shares screen space with the menu bar has to share the menu bar's window
  // too. What is left here is the status bar, drawn after the canvas rather
  // than before it, for one reason that is not cosmetic: it reads `st.view`,
  // and a band drawn first would show the value as it was before this
  // frame's canvas input changed it -- a zoom readout one frame stale, which
  // is exactly the juddering docs/ui.md section 5 asks the monospace
  // numerics to prevent.
  //
  // **The options bar is no longer drawn here.** It is a panel now
  // (`ControlsSection::Options`), so it is drawn by whichever dock holds it,
  // up with the other docks -- see this file's "dockable panel system"
  // section. What that costs is the one-frame freshness the comment above
  // argues for: the band reads `st.brush`, and it is now drawn before this
  // frame's canvas input rather than after. The trade is deliberate and the
  // loss is small -- the options bar shows the brush's *settings*, which the
  // canvas does not change, where the status bar shows the *view*, which it
  // does. The status bar, which is the one that actually needed it, still
  // draws here.
  drawAtelierStatusBar(st, bands, canvasW, canvasH);

  // The flyout rail and the open flyout, over the canvas. After the canvas
  // window so they float above it (creation order is z-order under
  // `NoBringToFrontOnFocus`), and before the rules so a rule still reads as
  // the topmost edge of the chrome.
  drawPanelRail(st, bands.canvas, &panelLayoutChanged);
  drawFlyoutPanel(st, bands.canvas, sim, gpu, lut, &panelLayoutChanged);

  // ------------------------------------------------------- the tear-off drop
  //
  // A panel is being dragged by its grip. This is the other half of
  // `drawPanelGrip()`'s gesture and it lives here rather than there for one
  // reason: the answer to "which dock is under the pointer" is a question
  // about the whole window, and a panel's grip only knows its own slot.
  //
  // Drawn on the FOREGROUND list so the preview sits above every dock and the
  // canvas, and evaluated after all of them so the highlight reflects this
  // frame's geometry rather than last frame's.
  if (st.panelDragActive) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    // **Two questions, in order.** A pointer already inside a dock is asking a
    // finer one than "which edge": which SLOT, and beside it or onto it --
    // because dropping onto a slot is how a tab stack is made, and stacking
    // has to be the same gesture as every other move or nobody will find it.
    // Only when the pointer is over no dock at all does the coarse,
    // canvas-relative answer apply.
    PanelPlacement hitDock = PanelPlacement::Right;
    AtelierRect hitRect;
    bool overDock = false;
    {
      const std::pair<PanelPlacement, AtelierRect> docks[] = {
          {PanelPlacement::Top, bands.topDock},
          {PanelPlacement::Left, bands.leftDock},
          {PanelPlacement::Right, bands.rightDock},
          {PanelPlacement::Bottom, bands.bottomDock},
      };
      for (const std::pair<PanelPlacement, AtelierRect>& d : docks) {
        const AtelierRect& r = d.second;
        if (r.empty()) continue;
        if (mouse.x < r.x || mouse.x >= r.right() || mouse.y < r.y || mouse.y >= r.bottom())
          continue;
        hitDock = d.first;
        hitRect = r;
        overDock = true;
        break;
      }
    }

    DockSlotDrop slotDrop;
    std::vector<PanelSlot> hitSlots;
    if (overDock) {
      hitSlots = st.panels.slotsIn(hitDock);
      const DockTiling t = dockTilingFor(st, hitDock, hitRect);
      slotDrop = dockSlotDropAt(t, dockSideFor(hitDock), mouse.x, mouse.y);
      // A slot the drag STARTED in is not a target for itself: dropping a lone
      // panel onto its own slot would ask `stackWith()` to stack a panel with
      // itself, and dropping a tab onto its own stack would be a no-op that
      // still looked like it did something.
      if (slotDrop.valid && slotDrop.slotIndex < hitSlots.size()) {
        const PanelSlot& s = hitSlots[slotDrop.slotIndex];
        bool ownSlot = false;
        for (const ControlsSection m : s.members)
          if (m == st.panelDragSection) ownSlot = true;
        if (ownSlot && slotDrop.mode == DockSlotDropMode::Into) slotDrop.valid = false;
      }
    }

    const DockDropTarget target = dockDropTargetAt(bands.canvas, mouse.x, mouse.y);
    const PanelPlacement dropPlacement =
        overDock ? hitDock
                 : (target.isDock ? placementForDockSide(target.side) : PanelPlacement::Flyout);

    // The preview strip: where the panel would land. Inside a dock that is the
    // slot itself for a stack, or the 4 px seam it would be inserted at; over
    // the canvas it is the whole dock, and for a dock that does not exist yet,
    // the edge band the drop would carve out of the canvas -- which is honest
    // about the fact that a new dock costs the canvas some room.
    AtelierRect preview;
    bool previewIsSeam = false;
    if (overDock && slotDrop.valid) {
      const DockTiling t = dockTilingFor(st, hitDock, hitRect);
      const AtelierRect& sr = t.slots[slotDrop.slotIndex].rect;
      const bool vert = dockStacksVertically(dockSideFor(hitDock));
      if (slotDrop.mode == DockSlotDropMode::Into) {
        preview = sr;
      } else {
        previewIsSeam = true;
        const bool before = slotDrop.mode == DockSlotDropMode::Before;
        constexpr float kSeam = 4.0f;
        preview = vert ? AtelierRect{sr.x, before ? sr.y : sr.bottom() - kSeam, sr.w, kSeam}
                       : AtelierRect{before ? sr.x : sr.right() - kSeam, sr.y, kSeam, sr.h};
      }
    } else if (overDock) {
      preview = hitRect;
    } else {
      preview = panelDropPreviewRect(bands, target);
    }

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (!preview.empty()) {
      const ImVec4 a = ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent));
      // **A seam is drawn solid, an area is drawn as a wash.** They mean
      // different things -- "inserted here" versus "landing in this region" --
      // and a 4 px strip filled at the same 22% alpha as a whole dock is
      // invisible, which would leave the two insert modes looking identical to
      // the one that stacks.
      if (previewIsSeam) {
        fg->AddRectFilled(ImVec2(preview.x, preview.y),
                          ImVec2(preview.right(), preview.bottom()), atelierToken(kAccent));
      } else {
        fg->AddRectFilled(ImVec2(preview.x, preview.y),
                          ImVec2(preview.right(), preview.bottom()),
                          IM_COL32((int)(a.x * 255), (int)(a.y * 255), (int)(a.z * 255), 56));
        fg->AddRect(ImVec2(preview.x, preview.y), ImVec2(preview.right(), preview.bottom()),
                    atelierToken(kAccent), 0.0f, 0, kRuleThickness);
      }
    }
    // The panel's name at the pointer, so a drag over a busy window still says
    // WHAT is being moved and not only where it would go.
    {
      const char* title = controlsSectionSpec(st.panelDragSection).title;
      pushAtelierMono();
      const ImVec2 sz = ImGui::CalcTextSize(title);
      const ImVec2 p(mouse.x + 14.0f, mouse.y + 10.0f);
      fg->AddRectFilled(ImVec2(p.x - 4.0f, p.y - 3.0f),
                        ImVec2(p.x + sz.x + 4.0f, p.y + sz.y + 3.0f), atelierToken(kChromeMid));
      fg->AddText(p, atelierToken(kTextPrimary), title);
      popAtelierMono();
    }
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const ControlsSection moving = st.panelDragSection;
      if (overDock && slotDrop.valid && slotDrop.slotIndex < hitSlots.size()) {
        const PanelSlot& onto = hitSlots[slotDrop.slotIndex];
        if (slotDrop.mode == DockSlotDropMode::Into) {
          // **The stack gesture.** Everything about which id, which order and
          // which tab ends up visible is `PanelLayout`'s; this only says which
          // two panels the pointer named.
          st.panels.stackWith(moving, onto.leader());
        } else {
          // Beside it. `setPlacementAt` counts PANELS, and what the pointer
          // named is a SLOT, so the index is converted through the slot list
          // rather than assumed equal -- they are only the same number when
          // nothing in the dock is stacked.
          size_t panelIndex = 0;
          for (size_t k = 0; k < slotDrop.slotIndex && k < hitSlots.size(); ++k)
            panelIndex += hitSlots[k].members.size();
          if (slotDrop.mode == DockSlotDropMode::After)
            panelIndex += onto.members.size();
          st.panels.setPlacementAt(moving, hitDock, panelIndex);
        }
        panelLayoutChanged = true;
      } else if (dropPlacement != st.panels.placementOf(moving)) {
        st.panels.setPlacement(moving, dropPlacement);
        // A panel dragged onto the rail opens straight away -- a flyout you
        // have to hunt for on the rail after asking for it is a move that
        // looks like a disappearance, which is the failure this whole pass is
        // correcting.
        if (dropPlacement == PanelPlacement::Flyout) {
          st.flyoutOpen = true;
          st.flyoutSection = moving;
        }
        panelLayoutChanged = true;
      }
      st.panelDragActive = false;
    }
  }

  // The four dock edges, so a dock can be resized as a whole rather than only
  // slot-by-slot. Drawn last of the interactive chrome: each is a 6 px window
  // straddling a dock's boundary with the canvas, and it has to be able to
  // take the mouse from both.
  drawDockEdge(st, PanelPlacement::Top, bands.topDock, &panelLayoutChanged);
  drawDockEdge(st, PanelPlacement::Left, bands.leftDock, &panelLayoutChanged);
  drawDockEdge(st, PanelPlacement::Right, bands.rightDock, &panelLayoutChanged);
  drawDockEdge(st, PanelPlacement::Bottom, bands.bottomDock, &panelLayoutChanged);

  // The second Begin of the window claimed above, now that `bands` is known.
  // After every band and dock, though it does not depend on that: they all
  // carry `NoBringToFrontOnFocus` and this does not, which is what puts it over
  // them. See ui/AtelierTheme.hpp.
  drawModalChromeScrim(bands);

  // Last, and on the foreground draw list: a 2 px rule that a neighbouring
  // window overdrew by a pixel would be a 1 px rule, and the design's whole
  // separation of major regions is that thickness.
  drawAtelierRules(bands);

  // One write per frame that changed something. See where
  // `panelLayoutChanged` is declared for why it is not one write per gesture.
  if (panelLayoutChanged) savePanelLayout(st);

  // Modeless, and drawn here rather than inside a docked panel's own window:
  // a `Begin()` nested inside another window's draw is a child of it, and this
  // one has to be able to float over the canvas and outlive the panel being
  // scrolled or collapsed.
  drawBrushSettingsWindow(st, gpu, lut);

  if (st.showDemo) ImGui::ShowDemoWindow(&st.showDemo);

  // Nothing to switch, clear or reload if no PaintSim exists yet -- st.mode
  // itself already carries the user's choice (see the Medium menu above)
  // and gets applied the moment painting actually constructs the sim.
  if (st.requestMode) {
    if (sim) sim->setMode(gpu, st.mode);
    st.requestMode = false;
  }
  if (st.requestClear) {
    if (sim) sim->clearCanvas(gpu);
    st.requestClear = false;
  }
  if (st.requestReload) {
    if (sim && sim->reloadShaders(gpu)) std::printf("[shader] reloaded\n");
    st.requestReload = false;
  }
}

const DocumentTexturePool& canvasDocumentTexture() { return g_documentTextures; }

// T14: uploads `g_transformPreview`'s ONE crop for whatever session
// `st.transform` currently holds. A no-op if no session is active, so a
// caller does not have to duplicate the `active()` check.
//
// Public (unlike `g_transformPreview` itself) because `st.transform.beginLayer()`
// has a SECOND call site outside this file -- main.cpp's drop-a-picture-and-
// it-arrives-in-a-session path -- and that path needs the identical upload
// this one does, not a second, drifting copy of it. The canvas block's own
// `st.requestFreeTransform` handler below calls this too, rather than
// inlining the upload twice.
void beginTransformPreview(AppState& st, GpuContext& gpu) {
  if (!st.transform.active()) return;
  OpenDocument* od = st.documents.active();
  const size_t li = st.transform.layerIndex();
  if (od == nullptr || li >= od->document.layers.size()) return;
  g_transformPreview.upload(gpu, od->document.layers[li], st.transform.selectionSnapshot(),
                            st.transform.sourceBounds());
}

std::optional<SDL_SystemCursor> canvasCursorRequest() { return g_canvasCursor; }

// ui/ToolCursor.hpp §7's companion to the accessor above -- same frame, same
// lifetime, the tool rather than the projected shape. See `g_canvasBitmapTool`'s
// own comment for why this is `nullopt` on frames `canvasCursorRequest()` answers
// with a guide-drag or pan/rotate shape.
std::optional<Tool> canvasCursorToolRequest() { return g_canvasBitmapTool; }

void setLayersPanelSelection(OpenDocument& doc, size_t layerIndex) {
  setActiveLayer(doc, layerIndex);
  // The multi-selection follows the primary row, so an external jump (a
  // history row, a comp) leaves the panel in the single-selection state a
  // user expects rather than in a stale multi-selection from before the jump.
  g_layers.selection = singleLayerSelection(doc.activeLayer);
}

void setLayersPanelSelectionSet(OpenDocument& doc, const LayerSelection& selection) {
  if (selection.empty()) return;
  g_layers.selection = selection;
  setActiveLayer(doc, selection.indices.front());
}

void setLayersPanelMessages(std::string error, std::vector<std::string> warnings) {
  g_layers.lastError = std::move(error);
  g_layers.lastWarnings = std::move(warnings);
}
void setCompsPanelRestoreSummary(std::string summary) { g_compsSummary = std::move(summary); }

// The one line beside the menus that every document operation leaves its result
// in, writable from outside this file.
//
// It exists for the drag-and-drop handler, which lives in main.cpp's SDL event
// loop -- a drop is an *event*, not a widget, so it is handled where the events
// are, and it has to be able to say what it did. Without this, main.cpp's only
// way to report "3 files dropped: 1 opened as a document, 2 imported as layers"
// would be a second status line of its own somewhere else on screen, which is
// how one application ends up with two places to look for the same kind of
// answer.
//
// Multi-line strings are expected and handled: the status line draws the first
// line and puts the whole thing in its tooltip, which is what makes a
// twelve-file drop's per-file problems reachable without a dialog.
void setDocumentStatusLine(std::string status) { g_docStatus = std::move(status); }

// Exactly the assignment the icon's click handler makes (see
// `drawAtelierTabStrip()`'s split-icon loop), and deliberately not one line
// more -- the declaration says why the companion and the focused pane are not
// written here.
void setSplitArrangement(AtelierSplit mode) { g_split.mode = mode; }

}  // namespace np
