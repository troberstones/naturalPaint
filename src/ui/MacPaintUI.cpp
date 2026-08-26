#include "ui/MacPaintUI.hpp"

#include "app/StrokeSession.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/AtelierTheme.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "app/BrushLibraryFile.hpp"
#include "app/BrushRowIcon.hpp"
#include "app/CloseDecision.hpp"
#include "app/CompPanel.hpp"
#include "app/ControlsLayout.hpp"
#include "app/CurveEdit.hpp"
#include "app/DabPreview.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/FilterOps.hpp"
#include "app/HistoryPanel.hpp"
#include "app/ImportImage.hpp"
#include "app/Journal.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "app/OpenAnyFile.hpp"
#include "app/QuitSequence.hpp"
#include "app/Snapping.hpp"
#include "app/UserBrushLibrary.hpp"
#include "app/ViewTransform.hpp"
#include "app/ZoomAndSize.hpp"
#include "color/LutBake.hpp"  // kMaxCurvePointsPerChannel
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
#include "ui/CanvasQuad.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/Fonts.hpp"
#include "ui/MacNativeMenu.hpp"
#include "ui/MenuModel.hpp"
#include "ui/ToolCursor.hpp"

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
constexpr float kControlsW = kRightColumnW;
// Peak gravity, in the same cells-per-step units as the rest of the velocity field.
constexpr float kMaxTilt = 0.50f;
// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// Screen-space px, not document-space -- the ruler strip's on-screen size is
// a layout constant like the band heights above, and the snap radius is
// defined to *feel* constant on screen regardless of zoom (converted to a
// document-space threshold per-frame; see the canvas block below).
constexpr float kRulerThickness = 20.0f;
constexpr float kSnapThresholdPx = 8.0f;

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

// --- The label column (UI detour step 3, problem 1b) ----------------------
//
// Dear ImGui draws a widget's label to the *right* of the widget, and the
// controls column is a fixed-width docked panel, so before this step four of
// its sliders read "Granulatio", "Edge darke", "Paper slop" and "Working ti" --
// clipped by the window edge, mid-word.
//
// Every labelled control in that column now goes through `ctlSlider()` /
// `ctlSliderInt()` / `ctlCombo()` below, which draw the label at the left and
// give the widget what is left. app/ControlsLayout owns the arithmetic and its
// one invariant (the widget never starts before the label ends); this half owns
// the measurement, because only ImGui knows how wide a string is in the loaded
// font at the current scale.
//
// `g_labelColumn` only grows, and it grows in the same frame that first
// measures a wider label, so a slider added later cannot clip even on its
// first frame. The widest label and the column it forced are printed whenever
// they change -- once at startup, and again only if a wider label ever appears
// -- so the claim "no label is clipped" is a measured number in the log rather
// than a look at a screenshot.
float g_labelColumn = 0.0f;
float g_widestLabelPx = 0.0f;
std::string g_widestLabel;
float g_reportedColumn = -1.0f;

// Draws `label`, positions the cursor for the widget and sizes it. Returns the
// `##`-prefixed id the widget must be given, in a caller-owned buffer, so the
// label is never drawn twice.
void beginLabelled(const char* label, char* idOut, size_t idCap) {
  std::snprintf(idOut, idCap, "##%s", label);
  const float labelPx = ImGui::CalcTextSize(label).x;
  if (labelPx > g_widestLabelPx) {
    g_widestLabelPx = labelPx;
    g_widestLabel = label;
  }
  const float startX = ImGui::GetCursorPosX();
  const LabelledControlLayout lay =
      layoutLabelledControl(g_labelColumn, labelPx, ImGui::GetContentRegionAvail().x);
  ImGui::TextUnformatted(label);
  if (!lay.labelOnOwnLine) {
    // SameLine()'s own offset argument is measured from the line start and
    // ignores the current indent, which the layers panel uses; setting the x
    // directly keeps an indented control aligned with its own group.
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetCursorPosX(startX + lay.labelColumn);
  }
  ImGui::SetNextItemWidth(lay.widgetWidth);
}

bool ctlSlider(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f") {
  char id[96];
  beginLabelled(label, id, sizeof(id));
  return ImGui::SliderFloat(id, v, lo, hi, fmt);
}

bool ctlSliderInt(const char* label, int* v, int lo, int hi) {
  char id[96];
  beginLabelled(label, id, sizeof(id));
  return ImGui::SliderInt(id, v, lo, hi);
}

// BeginCombo, laid out the same way. The caller ends it with EndCombo() as
// usual -- this only replaces the label and the width.
bool ctlBeginCombo(const char* label, const char* preview) {
  char id[96];
  beginLabelled(label, id, sizeof(id));
  return ImGui::BeginCombo(id, preview);
}

// `ImGui::TextDisabled()` that wraps at the panel edge. The same failure as
// the labels above, in a different widget: the layers panel's own status lines
// carry a document name and a counter triple, neither of which is bounded, and
// an unwrapped one is cut off by the window rather than continued.
void textDisabledWrapped(const char* fmt, ...) IM_FMTARGS(1);
void textDisabledWrapped(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", buf);
  ImGui::PopStyleColor();
}

bool ctlInputText(const char* label, char* buf, size_t cap, ImGuiInputTextFlags flags) {
  char id[96];
  beginLabelled(label, id, sizeof(id));
  return ImGui::InputText(id, buf, cap, flags);
}

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

  if (hovered) {
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
  const bool hovered = ImGui::IsItemHovered();

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

  if (hovered) {
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
      st.sim.brushHardness = st.brush.hardness;
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
  for (size_t i = 0; i < curve.size(); ++i) {
    float px = 0.0f, py = 0.0f;
    curveToPlot(curve[i].x, curve[i].y, kPlotSize, px, py);
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
    if (hit) {
      removePoint(curve, *hit);
      changed = true;
      // Indices may have shifted under the removal -- abandon any
      // in-progress drag rather than risk moving the wrong point next frame.
      dragIdx = -1;
      storage->SetInt(dragKey, dragIdx);
    }
  }

  if (hovered)
    ImGui::SetTooltip("Click empty area: add point\nDrag a point: move it\n"
                      "Right-click a point: delete it");

  ImGui::TextDisabled("%d / %d points", static_cast<int>(curve.size()),
                      kMaxCurvePointsPerChannel);
  return changed;
}

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
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Shows the canvas through the op stack below\n"
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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", layerCommandLabel(command));
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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", layerCommandLabel(command));
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
// and `mix` are, so a `linear-burn` arriving from a newer build (PRD I10) is
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
constexpr float kLayerOpacityW   = 52.0f;

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
  if (!entry.buildable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", layerKindUnbuildableReason(entry.kind));

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
  const std::vector<size_t> visibleRows = layersMatchingFilter(doc, g_layers.filter);

  // --- The header band -----------------------------------------------------
  //
  // The design's tab strip, minus the tabs -- see this section's doc comment
  // for why there are none. What is left is the strip's right-hand slot: the
  // count, monospace and right-aligned like every numeric in this chrome
  // (docs/ui.md section 1), reading `3/8` when the filter is hiding five rows
  // so that neither number can be mistaken for the other.
  {
    const float h = ImGui::GetTextLineHeight() + 4.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const std::string name = documentDisplayName(*od);
    drawClippedText(dl, ImVec2(at.x, at.y + 2.0f), panelW - 60.0f, mutedCol, name.c_str());
    pushAtelierMono();
    const std::string countText = layerPanelCountLabel(visibleRows.size(), count);
    const ImVec2 sz = ImGui::CalcTextSize(countText.c_str());
    dl->AddText(ImVec2(at.x + panelW - sz.x, at.y + 2.0f), mutedCol, countText.c_str());
    popAtelierMono();
    ImGui::Dummy(ImVec2(panelW, h));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%d x %d, %zu layer(s).\n"
                        "Painting is still separate: sim::PaintSim owns one dense\n"
                        "texture with no layer awareness, so a stroke reaches no\n"
                        "layer and nothing painted appears here.",
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
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Filters which rows are drawn -- nothing else.\n"
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
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The selected layer's blend mode. Blend modes are\n"
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
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The selected layer's opacity -- click or drag anywhere\n"
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
  // Rows sit flush against each other, separated by the design's 1px divider
  // rather than by ImGui's inter-item gap.
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
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

    // ---- the two icon buttons, last, so they take the mouse ---------------
    ImGui::SetCursorScreenPos(eyeAt);
    if (ImGui::InvisibleButton("##vis", ImVec2(kLayerEyeW, kLayerEyeW)))
      run(setLayerVisible(doc, i, !layer.visible));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Visibility. Allowed even on a locked layer --\n"
                        "hiding a layer changes nothing about it.");
    ImGui::SetCursorScreenPos(lockAt);
    if (ImGui::InvisibleButton("##lock", ImVec2(kLayerLockW, kLayerLockW)))
      run(setLayerLocked(doc, i, !layer.locked));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s. A locked layer refuses edits that change its\n"
                        "pixels or its place in the stack -- visibility and\n"
                        "this padlock itself still work.",
                        layer.locked ? "Locked -- click to unlock"
                                     : "Unlocked -- click to lock");

    // Selection, decided after the icon buttons so that a click they claimed is
    // not also a click on the row. Multi-select (PRD C12): plain click replaces
    // the selection, ctrl-click (cmd-click on this platform) toggles one row,
    // shift-click extends from the primary row to this one. `selected` follows
    // the row that was clicked in every case, so the controls above the list
    // always describe a row the user just touched.
    if (rowClicked) {
      const ImGuiIO& io = ImGui::GetIO();
      std::vector<size_t> next;
      if (io.KeyShift) {
        const size_t lo = std::min(selected, i);
        const size_t hi = std::max(selected, i);
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
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("New layer -- pick a kind. Three of the seven kinds can be\n"
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Layer Properties -- name, opacity, blend mode,\n"
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
    for (const LayerSetCommand command : allLayerSetCommands()) {
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
  // --- this modal alone does not dim the canvas ----------------------------
  //
  // **It stays modal.** Modality is what the paragraph above relies on: it is
  // why `selected` cannot change under the dialog mid-edit, and dropping it
  // would hand this dialog the layer-deleted-underneath-you hazard
  // app/StrokeSession §5 spends a page on and cannot close today, because
  // `Layer::id` is 0 on every layer this build creates. So the modality is
  // kept and only the *dimming* is dropped.
  //
  // The dim is right for a modal that asks a question -- Revert, Recover
  // Documents, Document path, and both Export dialogs are decision gates, and
  // greying what is behind them is the sentence "nothing else is live until you
  // answer". **This dialog is not a question; it is an editor whose output is
  // the canvas.** It holds the layer's op stack, so a user changing a curve or
  // an exposure here is looking *past* the dialog at the thing they are
  // changing -- and a 55 %-black wash over it defeats the only feedback the
  // control has. So the suppression is scoped to this one popup rather than
  // applied to `applyAtelierTheme()`'s token, where it would also strip the
  // five gates that want it.
  //
  // **The push must bracket `BeginPopupModal()` and nothing else.** ImGui
  // captures the dim colour once, per modal window, into `window->DC.
  // ModalDimBgColor` inside `Begin()`, and `RenderDimmedBackgroundBehindWindow()`
  // reads *that* at end-of-frame rather than re-reading the live style -- which
  // is exactly what makes a per-dialog override possible at all. Pushing after
  // the Begin, or around the popup's body, would change nothing; pushing around
  // the whole block would dim nothing anywhere.
  ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  const bool layerPropertiesOpen =
      ImGui::BeginPopupModal("Layer Properties", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PopStyleColor();
  if (layerPropertiesOpen) {
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
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blend modes are chosen for the linear working space.\n"
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", name);
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
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Clip to the layer below (PRD C9): this layer shows only\n"
                          "where the layer beneath it has alpha. The bottom layer\n"
                          "cannot be clipped: there is nothing below it.");

      // The layer's own op stack (PLAN.md Phase 5 steps 3 and 5). On an
      // Adjustment layer this stack is the layer's entire content: the kind
      // holds no pixels, so a fresh one is an exact no-op until an op here is
      // added *and* enabled.
      if (ImGui::TreeNodeEx("layerOps", ImGuiTreeNodeFlags_DefaultOpen, "Ops (%zu)",
                            layer.ops.size())) {
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("This layer's own non-destructive op stack. It\n"
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
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pg.name);
      ImGui::PopID();
      rowX += sw + 3.0f;
    }
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::TextUnformatted(sel.name);

    // The three constants, which are the whole argument for this mode. On two
    // lines because all three plus their labels do not fit a 322 px column at
    // this font, and a clipped `granulation` would hide the one of the three a
    // reader is least likely to guess from the swatch.
    pushAtelierMono();
    ImGui::TextDisabled("density %.2f    staining %.2f", sel.density, sel.staining);
    ImGui::TextDisabled("granulation %.2f", sel.granulation);
    // "The RGB readout stays visible as the resulting colour, read-only."
    ImGui::TextDisabled("rgb %.3f %.3f %.3f", sel.rgb[0], sel.rgb[1], sel.rgb[2]);
    popAtelierMono();
  } else {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
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
    ImGui::TextDisabled("density %.2f    staining %.2f", sel.density, sel.staining);
    ImGui::TextDisabled("granulation %.2f", sel.granulation);
    popAtelierMono();
    ImGui::TextWrapped(
        "This colour paints: RGB layers take it exactly, Pigment layers through RGB->latent. "
        "The three constants above are NOT from it -- an RGB triple has none -- and still come "
        "from %s. Switch to PIGMENT to change them.",
        sel.name);
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

DabPreviewCache g_dabPreview;
DabPreviewTexture g_dabPreviewTexture;

// 4a's TIP PREVIEW block: the image, its frame, its cell dividers, and the one
// line of text that says what scale it is drawn at.
//
// The frame and the dividers are `ImDrawList` primitives over the image rather
// than texels inside it, deliberately: they are chrome, they take the chrome's
// own tokens, and baking them into the image would make the "outermost covered
// texel tracks radius" assertion in `--selftest` have to know where the chrome
// stops. What `ImDrawList` cannot do is the falloff ramp, which is the whole
// reason there is a texture here at all.
void drawTipPreview(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  const std::array<BrushTip, kDabPreviewCells> tips =
      dabPreviewTipsFor(st.brush, lut, dynamicInputsFor(st));
  const DabPreviewImage& img = g_dabPreview.imageFor(tips);
  const WGPUTextureView view = g_dabPreviewTexture.viewFor(gpu, img, g_dabPreview.generation());

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  const float w = static_cast<float>(img.width);
  const float h = static_cast<float>(img.height);

  if (view != nullptr) {
    // ImGui's own pipeline, not ui/CanvasQuad: these bytes are already
    // display-referred sRGB (app/DabPreview's `DabPreviewImage`), and the
    // surface is non-sRGB, so the gamma applied is 1.0 and they arrive
    // untouched -- exactly like the chrome around them.
    dl->AddImage(reinterpret_cast<ImTextureID>(view), o, ImVec2(o.x + w, o.y + h));
  } else {
    // No adapter, or a texture that could not be made. Say so rather than
    // leaving a gap that reads as "this brush deposits nothing".
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), atelierToken(kChromeDeep));
  }
  for (int c = 1; c < kDabPreviewCells; ++c) {
    const float x = o.x + static_cast<float>(c * kDabPreviewCell);
    dl->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + h), atelierToken(kDivider),
                kDividerThickness);
  }
  dl->AddRect(o, ImVec2(o.x + w, o.y + h), atelierToken(kDivider), 0.0f, 0, kDividerThickness);
  ImGui::Dummy(ImVec2(w, h));

  // The three pressures and the scale, said in numbers, because a picture of
  // three dabs does not say which pressures they are -- and because §3's whole
  // claim is that the preview is at a *stated* ratio rather than fitted.
  pushAtelierMono();
  if (img.scale <= 1.0f)
    ImGui::TextDisabled("PRESSURE %.2f  %.2f  %.2f   1:1", kDabPreviewPressures[0],
                        kDabPreviewPressures[1], kDabPreviewPressures[2]);
  else
    ImGui::TextDisabled("PRESSURE %.2f  %.2f  %.2f   1:%.1f", kDabPreviewPressures[0],
                        kDabPreviewPressures[1], kDabPreviewPressures[2],
                        static_cast<double>(img.scale));
  popAtelierMono();
  if (ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(o, ImVec2(o.x + w, o.y + h)))
    ImGui::SetTooltip(
        "One dab on empty paper, at three pen pressures.\n"
        "radius %.0f / %.0f / %.0f px -- the same falloff the brush deposits.",
        static_cast<double>(img.radii[0]), static_cast<double>(img.radii[1]),
        static_cast<double>(img.radii[2]));
}

// One labelled row of the matrix's own geometry: a 54 px row label, twelve
// equal cells, a 34 px live-value gutter. Shared by the header row and the
// ten source rows so the columns cannot drift between them.
constexpr float kMatrixLabelW = 54.0f;
constexpr float kMatrixGutterW = 34.0f;
constexpr float kMatrixRowH = 19.0f;
constexpr float kMatrixHeadH = 16.0f;

// The two shades the matrix needs that the token table does not carry: the
// alternating column wash, and the dot marking a cell with no link. Local to
// the matrix because that is the only thing with columns to alternate.
constexpr uint32_t kMatrixColumnAlt = 0x333030;
constexpr uint32_t kMatrixEmptyDot = 0x4f4c4c;

// The DYNAMICS matrix: every source against every target it could drive.
//
// **Drawing the whole space is the design's central claim, not a stylistic
// choice** -- "an empty cell is as informative as a filled one -- you can see
// that nothing drives spacing". A list of existing links would be smaller and
// would answer a different question. The cost the design names honestly is
// that twelve targets in 322 px means two-letter heads, which have to be
// learned; the full names are on the cells' tooltips.
void drawDynamicsMatrix(AppState& st) {
  const DynamicInputs live = dynamicInputsFor(st);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float width = ImGui::GetContentRegionAvail().x;
  const float cellsW = width - kMatrixLabelW - kMatrixGutterW;
  if (!(cellsW > 0.0f)) return;
  const float cellW = cellsW / static_cast<float>(kDynamicTargetCount);

  const ImU32 ruleCol = atelierToken(kDivider);
  const ImU32 textCol = atelierToken(kTextPrimary);
  const ImU32 mutedCol = atelierToken(kTextSecondary);
  const ImU32 accentCol = atelierToken(kAccent);
  const ImU32 altCol = atelierToken(kMatrixColumnAlt);

  pushAtelierMono();

  // --- header row: the two-letter target heads --------------------------
  {
    const ImVec2 o = ImGui::GetCursorScreenPos();
    for (size_t t = 0; t < kDynamicTargetCount; ++t) {
      const float x0 = o.x + kMatrixLabelW + cellW * static_cast<float>(t);
      if (t % 2 == 1)
        dl->AddRectFilled(ImVec2(x0, o.y), ImVec2(x0 + cellW, o.y + kMatrixHeadH), altCol);
      const DynamicTarget headTarget = static_cast<DynamicTarget>(t);
      const char* ab = targetAbbrev(headTarget);
      const ImVec2 sz = ImGui::CalcTextSize(ab);
      // A refused column's head reads in the muted colour too -- the same
      // "disabled rather than absent" treatment `newLayerKindMenuItem()`
      // gives a layer kind with nowhere to land, so a reader learns the
      // matrix has twelve columns even though one of them does nothing.
      const bool headBuildable = targetUnbuildableReason(headTarget) == nullptr;
      dl->AddText(ImVec2(x0 + (cellW - sz.x) * 0.5f, o.y + (kMatrixHeadH - sz.y) * 0.5f),
                  headBuildable ? textCol : mutedCol, ab);
    }
    dl->AddLine(ImVec2(o.x, o.y + kMatrixHeadH), ImVec2(o.x + width, o.y + kMatrixHeadH),
                ruleCol, 1.0f);
    ImGui::Dummy(ImVec2(width, kMatrixHeadH));
  }

  // --- one row per source -------------------------------------------------
  for (size_t s = 0; s < kDynamicSourceCount; ++s) {
    const DynamicSource source = static_cast<DynamicSource>(s);
    const ImVec2 o = ImGui::GetCursorScreenPos();

    dl->AddText(ImVec2(o.x + 5.0f, o.y + (kMatrixRowH - ImGui::GetFontSize()) * 0.5f), textCol,
                sourceName(source));
    dl->AddLine(ImVec2(o.x + kMatrixLabelW, o.y),
                ImVec2(o.x + kMatrixLabelW, o.y + kMatrixRowH), ruleCol, 1.0f);

    for (size_t t = 0; t < kDynamicTargetCount; ++t) {
      const DynamicTarget target = static_cast<DynamicTarget>(t);
      const float x0 = o.x + kMatrixLabelW + cellW * static_cast<float>(t);
      const ImVec2 cellMin(x0, o.y);
      const ImVec2 cellMax(x0 + cellW, o.y + kMatrixRowH);
      if (t % 2 == 1) dl->AddRectFilled(cellMin, cellMax, altCol);

      const size_t at = findLink(st.brush.links, source, target);
      const bool selected = st.brush.editSource == source && st.brush.editTarget == target;
      const ImVec2 mid((cellMin.x + cellMax.x) * 0.5f, (cellMin.y + cellMax.y) * 0.5f);
      // Per-CELL, not merely per-column (`cellUnbuildableReason()`,
      // brush/Dynamics.hpp): Wetness refuses its whole column, but Hue/
      // Saturation/Value refuse only the four cells a stroke-local source
      // would otherwise resolve to a silent constant. A refused cell draws
      // as neither filled nor an empty dot: it is not a decision the user
      // can make yet, so it should not look like one that simply has not
      // been made.
      const char* unbuildable = cellUnbuildableReason(source, target);

      if (unbuildable != nullptr) {
        dl->AddLine(ImVec2(mid.x - 3.0f, mid.y), ImVec2(mid.x + 3.0f, mid.y), mutedCol, 1.0f);
      } else if (at != kNoLink) {
        // A link that has been switched off keeps its curve but drives
        // nothing, so it draws as neither filled nor empty -- an outline.
        const float half = selected ? 5.5f : 4.5f;
        const ImVec2 a(mid.x - half, mid.y - half), b(mid.x + half, mid.y + half);
        if (st.brush.links.links[at].enabled) {
          dl->AddRectFilled(a, b, accentCol);
        } else {
          dl->AddRect(a, b, accentCol, 0.0f, 0, 1.0f);
        }
        if (selected) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), textCol,
                                  0.0f, 0, 1.0f);
      } else {
        dl->AddRectFilled(ImVec2(mid.x - 1.0f, mid.y - 1.0f), ImVec2(mid.x + 1.0f, mid.y + 1.0f),
                          atelierToken(kMatrixEmptyDot));
        if (selected)
          dl->AddRect(ImVec2(mid.x - 5.5f, mid.y - 5.5f), ImVec2(mid.x + 5.5f, mid.y + 5.5f),
                      mutedCol, 0.0f, 0, 1.0f);
      }

      // One hit target per cell. Clicking an empty cell selects it rather
      // than creating a link there -- creation is the editor's ADD button,
      // so that a stray click on a 17 px cell cannot silently change how the
      // brush paints. A refused cell is disabled outright -- the same
      // `BeginDisabled()` / `IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)`
      // pair `newLayerKindMenuItem()` uses, so the reason is still reachable
      // by hovering a cell that cannot be clicked.
      ImGui::SetCursorScreenPos(cellMin);
      char id[48];
      std::snprintf(id, sizeof id, "##cell%zu_%zu", s, t);
      ImGui::BeginDisabled(unbuildable != nullptr);
      ImGui::InvisibleButton(id, ImVec2(cellW, kMatrixRowH));
      ImGui::EndDisabled();
      if (unbuildable != nullptr) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          popAtelierMono();
          ImGui::SetTooltip("%s \xE2\x86\x92 %s\n%s", sourceName(source), targetName(target),
                            unbuildable);
          pushAtelierMono();
        }
      } else {
        if (ImGui::IsItemHovered()) {
          popAtelierMono();
          ImGui::SetTooltip("%s \xE2\x86\x92 %s%s", sourceName(source), targetName(target),
                            at == kNoLink ? "  (no link)" : "");
          pushAtelierMono();
        }
        if (ImGui::IsItemClicked()) {
          st.brush.editSource = source;
          st.brush.editTarget = target;
        }
      }
    }

    // The live gutter. Its whole job is that the matrix teaches what a source
    // IS while you paint, so it shows the value in the source's own unit --
    // degrees for the angular three -- not the normalised number the model
    // carries.
    char val[32];
    sourceDisplay(source, sourceValue(live, source), val, sizeof val);
    const ImVec2 vsz = ImGui::CalcTextSize(val);
    const float gx = o.x + width - kMatrixGutterW;
    dl->AddLine(ImVec2(gx, o.y), ImVec2(gx, o.y + kMatrixRowH), ruleCol, 1.0f);
    // RANDOM genuinely is redrawn per dab now (`dynamicRandomDraw()`,
    // `app/StrokeSession`'s deposit loop), so it has no value between dabs;
    // its em dash is drawn muted for the same reason. VELOCITY, FADE, NOISE,
    // DIRECTION and INITIAL DIRECTION are also stroke-local -- `live` here is
    // `dynamicInputsFor()`'s per-FRAME hardware sample, which cannot see a
    // dab in progress, so this gutter shows their truthful idle reading (0.0,
    // "not moving" / "just started" / "at rest" / "no heading yet" / "no
    // heading LATCHED yet") rather than what the stroke is doing right now.
    dl->AddText(ImVec2(o.x + width - 5.0f - vsz.x, o.y + (kMatrixRowH - vsz.y) * 0.5f),
                source == DynamicSource::Random ? mutedCol : textCol, val);

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(width, kMatrixRowH));
    dl->AddLine(ImVec2(o.x, o.y + kMatrixRowH), ImVec2(o.x + width, o.y + kMatrixRowH), ruleCol,
                1.0f);
  }
  popAtelierMono();

  if (!st.penSeen)
    ImGui::TextDisabled("No tablet: tilt, azimuth and barrel read as rest.");
}

// The LINK editor: one cell's response curve, its range, and what it is
// resolving to right now.
void drawLinkEditor(AppState& st) {
  const DynamicSource source = st.brush.editSource;
  const DynamicTarget target = st.brush.editTarget;
  const size_t at = findLink(st.brush.links, source, target);

  pushAtelierMono();
  ImGui::Text("%s \xE2\x86\x92 %s", sourceName(source), targetName(target));
  popAtelierMono();

  // The same per-CELL refusal the matrix's own cells already enforce by
  // disabling the click that would get here -- restated rather than
  // trusted, because a link loaded from an older preset file (or a future
  // importer) could name a refused cell directly, without ever going
  // through a matrix click.
  if (const char* unbuildable = cellUnbuildableReason(source, target)) {
    ImGui::TextDisabled("%s", unbuildable);
    return;
  }

  if (at == kNoLink) {
    ImGui::TextDisabled("No link in this cell.");
    if (ImGui::Button("Add link")) {
      BrushLink made;
      made.source = source;
      made.target = target;
      targetDefaultRange(target, made.rangeLo, made.rangeHi);
      addLink(st.brush.links, made);
    }
    return;
  }

  // Hue/Saturation/Value shift the deposited colour only -- brush/
  // Dynamics.hpp's own section comment on `applyHsvDynamics()` is the full
  // argument; this is that same caveat surfaced where the one person who
  // needs it, someone wiring up exactly this link, can actually see it.
  if (target == DynamicTarget::Hue || target == DynamicTarget::Saturation ||
      target == DynamicTarget::Value) {
    ImGui::TextDisabled(
        "Shifts the deposited colour only. Density, staining and granulation stay the "
        "swatch's own.");
  }

  BrushLink& link = st.brush.links.links[at];
  const DynamicInputs live = dynamicInputsFor(st);
  const float in = sourceValue(live, source);

  ImGui::Checkbox("On", &link.enabled);
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    removeLink(st.brush.links, source, target);
    return;  // `link` is dangling from here on.
  }

  // The design's 104 px plot, and the grading stack's own widget rather than
  // a second one -- see drawCurveWidget()'s comment.
  drawCurveWidget(link.curve, 104.0f);

  // IN is the source's live value; OUT is what the link resolves to. Showing
  // both is what makes the curve legible while painting: the design rides the
  // live value along the curve as the pen moves.
  const float out = link.enabled ? linkContribution(link, in) : targetIdentity(target);
  pushAtelierMono();
  ImGui::Text("IN  %.2f", in);
  ImGui::Text("OUT %.3f", out);
  popAtelierMono();

  // RANGE bounds the OUTPUT (brush/Dynamics.hpp): at curve 0 the link resolves
  // to lo, at 1 to hi. On PRESSURE -> SIZE at 0.10-1.00 that is a floor on
  // size, not a deadzone on pressure.
  float lo = link.rangeLo, hi = link.rangeHi;
  const bool angular = targetCombine(target) == TargetCombine::Add;
  const float sliderLo = angular ? -360.0f : 0.0f;
  const float sliderHi = angular ? 360.0f : 2.0f;
  if (ctlSlider("Range lo", &lo, sliderLo, sliderHi)) link.rangeLo = lo;
  if (ctlSlider("Range hi", &hi, sliderLo, sliderHi)) link.rangeHi = hi;
  ImGui::Checkbox("Invert", &link.invert);

  // The three easing chips. They set the SAME Curve the widget edits, so a
  // chip and a hand-drawn curve cannot disagree -- and a curve dragged away
  // from all three lights none of them, which is the honest answer.
  const EasingPreset presets[3] = {EasingPreset::Linear, EasingPreset::EaseOut,
                                   EasingPreset::SCurve};
  const char* names[3] = {"Linear", "Ease out", "S"};
  for (int i = 0; i < 3; ++i) {
    if (i > 0) ImGui::SameLine();
    const bool active = matchesPreset(link.curve, presets[i]);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, atelierToken(kAccent));
    if (ImGui::Button(names[i])) link.curve = easingCurve(presets[i]);
    if (active) ImGui::PopStyleColor();
  }
}

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
    // rectangle rather than a gap, for drawTipPreview()'s reason: a gap reads
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
void ensureUserBrushLibraryLoaded(AppState& st) {
  if (st.userBrushLibraryLoaded) return;
  st.userBrushLibraryLoaded = true;
  std::string err;
  if (!st.userBrushLibrary.loadFromFile(defaultUserPresetsFilePath(), st.brush.brushLibrary,
                                        &err) &&
      !err.empty())
    g_brushLibraryStatus = err;
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
  if (ImGui::IsItemHovered() || ImGui::IsItemClicked())
    ImGui::SetTooltip(
        "Imports the brush PARAMETERS -- size, spacing, roundness, angle and the whole\n"
        "dynamics graph. Sampled bitmap tips are not imported: those brushes will behave\n"
        "like the originals and paint with this application's round tip. The import says\n"
        "which ones.");
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

    if (ImGui::IsItemHovered()) drawBrushRowTooltip(st, gpu, lut, r, lib, edited);
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry.path.c_str());

    ImGui::SameLine();
    // Remove, at the right of the group's own header, so it is unambiguous
    // *which* library it removes -- a Remove button in the pane's footer would
    // act on whatever happened to be selected.
    if (ImGui::SmallButton("Remove")) unloadRequest = entry.id;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Removes this library's brushes and forgets it. Brushes you made\n"
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
  if (activeIsImported && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip(
        "This brush belongs to an imported library, which mirrors its file.\n"
        "Duplicate it to keep a copy of your own, or Remove the whole library.");

  if (edited) {
    pushAtelierMono();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)),
                       "EDITED -- picking another brush discards it");
    popAtelierMono();
  }
}

void drawBrushSection(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
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
    if (fromLibrary && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          "'%s' came from an imported .abr, and this build does not write .abr files.\n"
          "Duplicate keeps your version as a brush of your own.",
          lib.presets[lib.active].name.c_str());
    if (isBuiltIn && edited && ImGui::IsItemHovered())
      ImGui::SetTooltip(
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

  // --- TIP ---------------------------------------------------------------
  // Radius, hardness and spacing already existed; roundness is new with this
  // panel. Spacing is in radii, and the design's caption is the reason it can
  // be: dabs are spaced by arc length, not by time, so the number means the
  // same thing however fast the pen moves.
  //
  // The preview goes ABOVE the sliders, which is 4a's own order and is also
  // the useful one: a slider drag is judged by what happens above it, and a
  // preview below the controls is under the hand that is dragging them.
  drawTipPreview(st, gpu, lut);
  // kBrushRadiusMin/Max (app/AppState.hpp): the one range for this field,
  // also read by the options bar's SIZE slider. See that constant's comment.
  ctlSlider("Radius", &st.brush.radius, kBrushRadiusMin, kBrushRadiusMax, "%.0f px");
  // kBrushHardnessMin/Max (app/AppState.hpp): the one range for this field,
  // also read by the options bar's HARD slider.
  ctlSlider("Hardness", &st.brush.hardness, kBrushHardnessMin, kBrushHardnessMax);
  ctlSlider("Spacing", &st.brush.spacing, 0.02f, 1.0f, "%.2f r");
  ctlSlider("Roundness", &st.brush.roundness, 0.05f, 1.0f);
  ctlSlider("Angle", &st.brush.angle, -180.0f, 180.0f, "%.0f deg");
  ImGui::TextDisabled("Dabs are spaced by arc length, not by time.");

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
    const bool honoured = erasing || route == StrokeRoute::RgbDeposit;
    ImGui::BeginDisabled(!honoured);
    ctlSlider("Opacity", &st.brush.opacity, 0.0f, 1.0f);
    ImGui::EndDisabled();
    if (erasing)
      ImGui::TextDisabled("Flow is how fast it bites; opacity is how much it takes.");
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

  // --- DYNAMICS ----------------------------------------------------------
  if (ImGui::CollapsingHeader("DYNAMICS", ImGuiTreeNodeFlags_DefaultOpen)) {
    pushAtelierMono();
    ImGui::Text("%zu LINKS", st.brush.links.links.size());
    popAtelierMono();
    drawDynamicsMatrix(st);
  }
  if (ImGui::CollapsingHeader("LINK", ImGuiTreeNodeFlags_DefaultOpen)) drawLinkEditor(st);
}

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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("How fast pigment drops out of suspension.");
  ctlSlider("Staining", &st.sim.staining, 0.02f, 1.0f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Resistance to being lifted back into the water.");
  ctlSlider("Granulation", &st.sim.granulation, 0.0f, 1.0f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("Affinity for the paper's valleys.");
  ImGui::EndDisabled();
  ImGui::TextDisabled("Owned by the loaded pigment -- pick a different paint to change these.");

  ctlSlider("Diffusion", &st.sim.pigmentDiffuse, 0.0f, 1.0f);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Pigment spreading through the wet film.\n"
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
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Paint the brush picks up when a stroke starts.\n"
                        "It runs out as you paint, like a real one.");
    ctlSlider("Pressure", &st.sim.penetration, 0.05f, 2.0f);
    ctlSlider("Squish", &st.sim.oilPressure, 0.0f, 4.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("vp = -c * grad(penetration): paint pushed out\n"
                        "sideways from under the bristles.");
    ctlSlider("Transfer", &st.sim.xferFraction, 0.0f, 0.5f);
    ctlSlider("Max transfer", &st.sim.maxXfer, 0.0f, 0.1f, "%.4f");
    ctlSlider("Levelling", &st.sim.viscosity, 0.0f, 0.25f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Wet paint relaxing under surface tension.\n"
                        "At zero the brush stamps leave periodic ridges.");
    ctlSlider("Impasto light", &st.sim.impastoLight, 0.0f, 1.5f);
    ctlSlider("Adhesion", &st.sim.adhesion, 0.0f, 0.4f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Paint never fully leaves a cell. At zero the\n"
                        "canvas feels like Teflon.");
  } else if (st.mode == PaintMode::Ink) {
    ctlSlider("Relaxation", &st.sim.omega, 0.5f, 1.95f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("LBE omega. Viscosity = (1/omega - 1/2)/3.");
    ctlSlider("Blocking", &st.sim.blocking, 0.0f, 0.9f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Base permeability of the paper. Higher blocks\n"
                        "more flow and pins the mark's edge.");
    ctlSlider("Grain block", &st.sim.grainBlock, 0.0f, 0.9f);
    ctlSlider("Glue", &st.sim.glue, 0.0f, 0.6f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Artists add glue to limit spread.");
    ctlSlider("Receptivity", &st.sim.receptivity, 0.1f, 2.5f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Wet paper takes less ink. Lower this and a second\n"
                        "stroke over a damp mark barely registers.");
    ctlSlider("Settle rate", &st.sim.settleScale, 0.0f, 0.05f, "%.4f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How fast ink fixes to the fibres. Too high and it\n"
                        "deposits before it can travel, so nothing bleeds.");
    if (sim) ctlSliderInt("Lattice steps", &sim->inkSubsteps, 1, 20);
    ctlSlider("Evaporation", &st.sim.evaporation, 0.0f, 0.03f, "%.4f");
  } else {
    ctlSlider("Viscosity", &st.sim.viscosity, 0.0f, 0.5f);
    ctlSlider("Drag", &st.sim.drag, 0.0f, 1.5f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Above ~0.5 the velocity field dies before water moves.");
    ctlSlider("Edge darkening", &st.sim.edgeDarkening, 0.0f, 2.0f, "%.3f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Curtis FlowOutward: pulls pigment to the stroke rim.\n"
                        "Zero this and washes go flat.");
    ctlSlider("Paper slope", &st.sim.paperSlope, 0.0f, 4.0f);

    // One control for the wet lifetime. Evaporation and absorption are derived
    // from it rather than exposed separately: letting the two drift out of step
    // only makes the timing unpredictable, and neither means much alone.
    if (ctlSlider("Working time", &st.workingTime, 1.0f, 20.0f, "%.1f s"))
      setWorkingTime(st.sim, st.workingTime);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How long a wash keeps bleeding before it sets.\n"
                        "Good to about 15%% across this range. Past ~20 s the\n"
                        "wash spreads thin enough that capillary dilution ends\n"
                        "it regardless of how slowly it dries.");

    ctlSlider("Max film", &st.sim.maxFilm, 0.2f, 8.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Deepest water the paper holds before it runs.\n"
                        "Raise it far and a wash empties into its own rim.");
    ctlSlider("Capillary diffuse", &st.sim.diffuseRate, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How fast water wicks through the fibres.\n"
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
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Only wet paint runs — the force scales with film\n"
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
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Snaps guide creation/dragging to guides, the grid\n"
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
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A named state exempt from the byte budget's eviction\n"
                      "until you dismiss it (PRD O4). Kept in its own list, not\n"
                      "in the undo chain, so undoing past it cannot lose it.");

  const std::string dropped = historyDroppedNote(h);
  if (!dropped.empty()) ImGui::TextWrapped("%s", dropped.c_str());

  // Rows, oldest at the top -- the OPPOSITE of drawLayersSection()'s order,
  // and app/HistoryPanel.hpp says why the two panels differ. There is no
  // reversal in this loop and there must not be one.
  uint64_t clickedEntry = 0;
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str());
    ImGui::PopID();
  }

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
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A comp captures every layer's visibility, opacity, blend\n"
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

  for (const CompPanelRow& row : rows) {
    ImGui::PushID(static_cast<int>(row.index));
    if (ImGui::Selectable(compRowText(row).c_str(), selected == row.index))
      selected = row.index;
    if (ImGui::IsItemHovered() && compRowIsPartial(row))
      ImGui::SetTooltip("%zu of this comp's %zu layers are still in the document.\n"
                        "Restoring applies those and reports the rest -- a comp is\n"
                        "matched by layer id, never by position, so nothing lands\n"
                        "on a layer it was not captured from.",
                        row.stillHere, row.captured);
    ImGui::Indent();
    ImGui::BeginDisabled(!row.known);
    if (ImGui::SmallButton("Restore")) restore = row.index;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !row.known)
      ImGui::SetTooltip("This comp was written by a build whose comp format this\n"
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", why.c_str());
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
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", why.c_str());
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
// **There is no native file picker in this codebase**, and adding one is a
// platform-integration job (NSOpenPanel behind an interface) rather than part
// of a lifecycle step. So Open, Save As and Save a Copy take a typed path in a
// small modal. That is deliberately spartan rather than pretending: the
// operations underneath are the real ones, and swapping the text field for a
// panel later changes this function and nothing else.
//
// Which state lives where follows app/AppState.hpp's rule: the session and
// the recent list are on AppState; the text buffer, the pending action and
// the last status line are function-local, because they are widget state.
// `ImportImage` reuses this same typed-path modal rather than growing a second
// path-entry UI beside it. There is still no native file picker in this
// codebase (see above), and inventing a *second* spartan one -- with its own
// buffer, its own pre-fill rule and its own error line -- would double the
// thing that has to be replaced when a real panel arrives, for no benefit
// today. The modal's verb, its pre-fill and the status it leaves behind are
// the only things that vary by action, and each of those is one line.
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

// Forward-declared up beside the BRUSH LIBRARY pane, which is drawn earlier in
// this file. Sets the same two globals File > Open does, so the `+` cannot
// end up with a second modal of its own.
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

void drawDocumentDialogs(AppState& st) {
  static char pathBuf[512] = "";
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

  if (g_docPathRequested) {
    g_docPathRequested = false;
    g_docStatus.clear();
    // Pre-fill with the active document's own path, so Save As on an already
    // saved document starts from its name rather than from nothing.
    //
    // **Except for an import**, which is reading someone else's file: offering
    // the open document's own `.npaint` path as the image to import is not a
    // useful starting point, and one careless Return away from a refusal that
    // names the wrong mistake. It starts empty instead.
    if (g_docPathAction == DocPathAction::ImportImage ||
        g_docPathAction == DocPathAction::ImportBrushes) {
      pathBuf[0] = '\0';
    } else if (const OpenDocument* d = st.documents.active()) {
      std::snprintf(pathBuf, sizeof(pathBuf), "%s", d->path.c_str());
    }
    ImGui::OpenPopup("Document path");
  }
  if (ImGui::BeginPopupModal("Document path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* verb = g_docPathAction == DocPathAction::Open          ? "Open"
                       : g_docPathAction == DocPathAction::SaveAs      ? "Save As"
                       : g_docPathAction == DocPathAction::SaveCopy    ? "Save a Copy"
                       : g_docPathAction == DocPathAction::ImportImage ? "Import Image"
                                                                      : "Import Brushes";
    ImGui::Text("%s", verb);
    // Says what the field now accepts, because the widening is invisible
    // otherwise: the modal looks exactly as it did when it took `.npaint`
    // alone. The second sentence is the half users get wrong -- a picture opens
    // into a document that is bound to no file, so Save As is how it acquires
    // one (app/OpenAnyFile.hpp argues why binding it to the picture would be a
    // trap).
    if (g_docPathAction == DocPathAction::Open)
      ImGui::TextDisabled(
          "A .npaint document, or any image this build reads. Which one is decided by "
          "the file's contents, not its name.\nAn image opens as a new, unsaved document; "
          "use Save As to give it a .npaint of its own.");
    if (g_docPathAction == DocPathAction::SaveCopy)
      ImGui::TextDisabled("Writes elsewhere; this document stays bound to its own file.");
    if (g_docPathAction == DocPathAction::ImportImage)
      ImGui::TextDisabled("Adds the image to this document as a new RGB layer, on top.");
    if (g_docPathAction == DocPathAction::ImportBrushes)
      ImGui::TextDisabled(
          "A Photoshop .abr library. Its brushes join the BRUSH LIBRARY pane and are\n"
          "remembered for next launch; the file itself is re-read only when one is picked.");
    ImGui::SetNextItemWidth(480.0f);
    ImGui::InputText("Path", pathBuf, sizeof(pathBuf));
    if (ImGui::Button(verb)) {
      applyDocumentPathAction(st, g_docPathAction, pathBuf);
      if (g_docPathActionOk) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    if (!g_docStatus.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
      ImGui::TextWrapped("%s", g_docStatus.c_str());
      ImGui::PopStyleColor();
    }
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
  if (st.pendingClose.awaitingDestination && !ImGui::IsPopupOpen("Document path")) {
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
// `AppState`'s, per that struct's own ownership rule). None of the four has
// a live preview: every one of them applies on the confirm button and
// nothing before it, which keeps this file thin and keeps the only place a
// pixel actually changes at `app/FilterOps.cpp`'s four `applyX()` functions
// -- the ones `--selftest` (app/selftest/FilterMenu.cpp) also calls, so the
// dialog and the test cannot disagree about what confirming one does.
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
bool g_gaussianBlurRequested = false;
bool g_sharpenRequested = false;
bool g_unsharpMaskRequested = false;
bool g_addNoiseRequested = false;

void drawGaussianBlurDialog(AppState& st) {
  static float sigma = 8.0f;  // texels; ops/Blur.hpp's own worked examples use this
  static std::string status;

  if (g_gaussianBlurRequested) {
    g_gaussianBlurRequested = false;
    status.clear();
    ImGui::OpenPopup("Gaussian Blur");
  }
  if (!ImGui::BeginPopupModal("Gaussian Blur", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Radius (sigma, texels)", &sigma, 0.0f, 250.0f, "%.1f");
  ImGui::TextDisabled("0 is the identity. ops/Blur.hpp's apron is ceil(4 * sigma) texels.");

  OpenDocument* od = st.documents.active();
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

  if (g_sharpenRequested) {
    g_sharpenRequested = false;
    status.clear();
    ImGui::OpenPopup("Sharpen");
  }
  if (!ImGui::BeginPopupModal("Sharpen", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Strength", &strength, 0.0f, 3.0f, "%.2f");
  // kSharpenSigma is named rather than offered as a control: ops/Filters.hpp
  // section 3 argues at length for why 1.0 is the one radius this one-click
  // filter should have, and a slider here would be the second radius control
  // the header's own "one-click filter, not an operator" distinction warns
  // against re-opening. Unsharp Mask, two rows down in this same menu, is
  // where the radius becomes a dial.
  ImGui::TextDisabled("Fixed radius (sigma %.1f) -- see Unsharp Mask for a radius control.",
                      kSharpenSigma);

  OpenDocument* od = st.documents.active();
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

  if (g_unsharpMaskRequested) {
    g_unsharpMaskRequested = false;
    status.clear();
    ImGui::OpenPopup("Unsharp Mask");
  }
  if (!ImGui::BeginPopupModal("Unsharp Mask", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Amount", &params.amount, 0.0f, 5.0f, "%.2f");
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Radius (sigma, texels)", &radius, 0.1f, 250.0f, "%.1f");
  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Threshold", &params.threshold, 0.0f, 0.20f, "%.3f");
  ImGui::TextDisabled(
      "Threshold is shaper-domain (ops/Filters.hpp section 2): 0.02 ignores differences "
      "smaller than 27%% of the local level, at every brightness.");

  params.blur.kind = BlurKind::Gaussian;
  params.blur.sigma = radius;

  OpenDocument* od = st.documents.active();
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
  if (!ImGui::BeginPopupModal("Add Noise", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  ImGui::SetNextItemWidth(200.0f);
  ImGui::SliderFloat("Amount", &params.amount, 0.0f, 0.5f, "%.3f");
  ImGui::RadioButton("Uniform", &distributionIdx, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Gaussian", &distributionIdx, 1);
  params.distribution =
      distributionIdx == 0 ? NoiseDistribution::Uniform : NoiseDistribution::Gaussian;
  ImGui::Checkbox("Monochrome", &params.monochrome);
  ImGui::TextDisabled("Amount is shaper-domain (ops/Filters.hpp section 5): 0.05 is a +/-83%% "
                      "swing in linear light at every brightness above the shadow toe.");
  ImGui::Text("Seed: %llu", static_cast<unsigned long long>(params.seed));
  ImGui::SameLine();
  if (ImGui::Button("New seed"))
    params.seed = static_cast<uint64_t>(ImGui::GetTime() * 1e6) ^ 0x9e3779b97f4a7c15ull;

  OpenDocument* od = st.documents.active();
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
// The Image menu's two dialogs (PRD D17; app/FilterOps again)
// ---------------------------------------------------------------------------
//
// Document-level, so there is no `PixelOpRefusal` here (app/FilterOps.hpp's
// header argues why) -- the only failure mode is `ops/DocumentTransform`'s
// own, a zero extent, and it is shown exactly as it comes back rather than
// translated through a second vocabulary.
bool g_imageSizeRequested = false;
bool g_canvasSizeRequested = false;

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
        if (!n.tooltip.empty() &&
            ImGui::IsItemHovered(n.enabled ? ImGuiHoveredFlags_None
                                           : ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", n.tooltip.c_str());
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
      st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(canvasW),
                                             static_cast<int32_t>(canvasH), WorkingSpace{}));
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
    case MenuAction::GrayscalePreview: st.view.grayscale = !st.view.grayscale;   break;
    case MenuAction::Rulers:           st.showRulers = !st.showRulers;           break;
    case MenuAction::Navigator:        st.showNavigator = !st.showNavigator;     break;
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

    // --- Image ----------------------------------------------------------
    case MenuAction::ImageSize:  g_imageSizeRequested = true;  break;
    case MenuAction::CanvasSize: g_canvasSizeRequested = true; break;

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

void drawUI(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH) {
  // First, before any branch can skip it: last frame's cursor request is not
  // this frame's answer. See `g_canvasCursor`'s own comment -- the canvas block
  // that sets it is reachable only on some frames, so a reset that lived beside
  // it would let a crosshair outlive the pointer being over the canvas.
  g_canvasCursor.reset();

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

  // ------------------------------------------------------------ title bar
  //
  // docs/ui.md section 2's first band: 36 px, wordmark then menus, undo/redo
  // at the right. ImGui fixes the main menu bar's height at `GetFrameHeight()`
  // -- font size plus twice the frame padding -- so the padding is pushed for
  // exactly the `BeginMainMenuBar()` call that reads it and popped
  // immediately, leaving every *menu item* at its normal size. The band is
  // 36 px because the design says 36 px; the items are then centred in it.
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
    // wordmark, the one-line document status, the Undo and Redo buttons the
    // design puts at the right of this band, and the frame rate. Reclaiming
    // the 36 px would delete Undo and Redo from the chrome to remove a strip
    // that is not actually dead -- so what macOS reclaims is the *menus'*
    // share of the band, and the band keeps doing its other job.
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
        // does not use it -- its five views are --demo-document,
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
    // strip's -- the band directly below shows every open document by name
    // with an accent dot on the dirty one, which is strictly more of what the
    // name was here to provide. Two copies of it would be the design's own
    // redundancy, not a safeguard.
    OpenDocument* activeForBar = st.documents.active();
    if (!g_docStatus.empty()) {
      const size_t firstLine = g_docStatus.find('\n');
      const std::string oneLine =
          firstLine == std::string::npos ? g_docStatus : g_docStatus.substr(0, firstLine);
      ImGui::TextDisabled("| %s", oneLine.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_docStatus.c_str());
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
    // Not built: the design's `panels` control. There is no panel manager to
    // wire it to, and a button that does nothing is worse than a gap.
    History* titleHistory = activeForBar != nullptr ? &activeForBar->history : nullptr;
    char status[64];
    std::snprintf(status, sizeof(status), "%.1f fps",
                  st.frameMs > 0.0f ? 1000.0f / st.frameMs : 0.0f);
    const float undoW = ImGui::CalcTextSize("Undo").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float redoW = ImGui::CalcTextSize("Redo").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float statusW = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - undoW - redoW - statusW - 40.0f);
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
    // A Horizontal guide's position is along canvasH (it sits at a fixed Y);
    // a Vertical guide's is along canvasW (a fixed X) -- same axis pairing
    // app/Snapping.hpp's parseGuidePosition() doc comment spells out.
    const float axisExtent = horizontal ? static_cast<float>(canvasH) : static_cast<float>(canvasW);
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

  // docs/reachability-audit.md C1: the Filter menu's four dialogs and the
  // Image menu's two, out here for the identical ID-stack reason as every
  // dialog above.
  drawGaussianBlurDialog(st);
  drawSharpenDialog(st);
  drawUnsharpMaskDialog(st);
  drawAddNoiseDialog(st);
  drawImageSizeDialog(st);
  drawCanvasSizeDialog(st);
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
  // marker and a `+` beside it, and a band that materialised on the second
  // document would move every other band down by 36 px at the moment a user
  // opened one.
  const AtelierBands bands = atelierLayout(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y,
                                           /*showTabStrip=*/!st.documents.empty());

  const ImGuiWindowFlags fixedFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

  // ------------------------------------------------------------ tool palette
  ImGui::SetNextWindowPos(ImVec2(bands.toolPalette.x, bands.toolPalette.y));
  ImGui::SetNextWindowSize(ImVec2(bands.toolPalette.w, bands.toolPalette.h));
  if (ImGui::Begin("##tools", nullptr, fixedFlags)) {
    // The cell size is computed fresh every frame from the band's live
    // height -- see ui/AtelierLayout.hpp's atelierToolCellSize() and the
    // comment on the (now-removed) file-scope kToolSize above for why this
    // is a local, not a cached member or a file-static. `kToolSwatchAreaH`
    // is the same constant atelierToolCellSize() itself subtracts before
    // dividing by kToolCellCount, so the grid drawn here and the grid
    // atelierToolCellSize() sized itself for agree by construction rather
    // than by two call sites happening to compute the same arithmetic twice.
    const float cellSize = atelierToolCellSize(bands.toolPalette.h);
    const float swatchSide = kToolCellMax - 8.0f;
    const float gridH = std::max(0.0f, bands.toolPalette.h - kToolSwatchAreaH);

    // ItemSpacing is 6px everywhere else in the chrome (ui/AtelierTheme.cpp)
    // but the grid's own cell size already accounts for every pixel between
    // the title bar's rule and the swatch above -- 6px of spacing *between*
    // 28 cells is 27 gaps the shrink-to-fit arithmetic above never saw, and
    // this is exactly the "buttons are too large" the user pointed at: the
    // cells themselves were not the (only) problem, the gaps between them
    // were. Zeroed here, scoped to just this child, rather than globally --
    // the rest of the chrome still wants its 6px.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    // The grid no longer scrolls in the steady state -- the whole point of
    // atelierToolCellSize() is to shrink cells until all 28 fit. It keeps
    // `ImGuiWindowFlags_NoScrollbar` anyway, for the one case shrinking
    // cannot rescue (windows shorter than ui/AtelierLayout.hpp's "honest
    // limit" comment describes, roughly 670px): Dear ImGui's mouse wheel
    // still scrolls a `NoScrollbar` child, so every cell stays reachable,
    // it is only the bar itself -- and the width it would otherwise reserve
    // -- that this suppresses. The *outer* window above does not scroll
    // either (`ImGuiWindowFlags_NoScrollbar` is in `fixedFlags`), which is
    // what keeps the FG swatch below pinned to the bottom of the band
    // regardless of how far the grid has wheel-scrolled -- it lives in the
    // outer window's content, after this child's fixed-height reservation,
    // not inside the scrolling child itself.
    if (ImGui::BeginChild("##toolgrid", ImVec2(0.0f, gridH), 0,
                          ImGuiWindowFlags_NoScrollbar)) {
      // --flyout-demo names a *tool*, not a slot index -- matched by
      // toolGroupIndex() so a reordering of ui/AtelierChrome.hpp's
      // kToolGroups cannot silently point the demo at the wrong group.
      const int brushGroup = toolGroupIndex(Tool::Brush);
      for (int g = 0; g < kToolGroupCount; ++g) {
        toolGroupButton(st, g, cellSize, st.openToolFlyoutDemo && g == brushGroup);
        if (kToolGroups[g].ruleAfter) {
          ImDrawList* dl = ImGui::GetWindowDrawList();
          const ImVec2 rp = ImGui::GetCursorScreenPos();
          dl->AddRectFilled(rp, ImVec2(rp.x + cellSize, rp.y + kDividerThickness),
                            atelierToken(kDivider));
          ImGui::Dummy(ImVec2(cellSize, kDividerThickness));
        }
      }
      moreToolsButton(cellSize);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();  // ItemSpacing, pushed above BeginChild()

    // The foreground swatch, at the bottom of the palette where docs/ui.md
    // section 2's diagram puts `FG/BG` -- drawn after EndChild() precisely so
    // it is *not* part of the scrolling grid above and cannot move with it.
    // Its side is pinned to `kToolCellMax`, not the live `cellSize`, so it
    // does not resize or reflow as the window resizes -- see
    // ui/AtelierLayout.hpp's kToolSwatchAreaH comment for why that anchoring
    // also breaks a circular dependency in the height arithmetic above.
    //
    // **FG only, no BG.** The pair is Photoshop's, and the second half of it
    // means something only once something fills with it: Fill-with-colour is
    // PRD D26 and the paint bucket D25, both phase 6, and nothing in this
    // build reads a background colour. A second well that no operation
    // consumes would be the `PRESET` dropdown problem again -- chrome
    // promising a feature that is not behind it.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // **`foregroundSrgb()`, not `defaultPalette()[st.brush.pigment]`.** This
    // is the well PRD Q10 says the eyedropper picks *into*, so it has to show
    // the colour that was picked; drawing the palette row here would leave the
    // one control a user looks at to confirm a pick showing the colour they
    // just replaced. It also indexed `st.brush.pigment` unchecked, which
    // `foregroundSrgb()` does not.
    const std::array<float, 3> fg = foregroundSrgb(st.brush);
    dl->AddRectFilled(p, ImVec2(p.x + swatchSide, p.y + swatchSide),
                      IM_COL32((int)(fg[0] * 255), (int)(fg[1] * 255), (int)(fg[2] * 255), 255));
    dl->AddRect(p, ImVec2(p.x + swatchSide, p.y + swatchSide), atelierToken(kRule), 0.0f, 0,
                kRuleThickness);
    ImGui::Dummy(ImVec2(swatchSide, swatchSide));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Foreground: %s\nChosen in the COLOR panel (docs/ui.md section 3.3), "
                        "or picked with the eyedropper (PRD Q10)",
                        foregroundName(st.brush));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
    ImGui::TextUnformatted("FG");
    ImGui::PopStyleColor();
  }
  ImGui::End();

  // --------------------------------------------------------- controls column
  //
  // docs/ui.md §2's "docked column of collapsing headers", which this was not
  // until UI detour step 3: it was one unbroken scroll with six
  // simulation-parameter sections above everything Phase 5 built, so LAYERS and
  // HISTORY were off the bottom of the window at the default size and a
  // screenshot of the running application did not contain them at all.
  //
  // The order and the default-open set are `app::controlsSections()`, as data,
  // so both are asserted by `--selftest` rather than being a property of where
  // a statement sits in this function. That header carries the argument for
  // them; what is here is the loop.
  ImGui::SetNextWindowPos(ImVec2(bands.rightColumn.x, bands.rightColumn.y));
  ImGui::SetNextWindowSize(ImVec2(bands.rightColumn.w, bands.rightColumn.h));
  // `NoScrollWithMouse` takes the wheel away from Dear ImGui for this window
  // ONLY, so the step below is the whole behaviour rather than an addition to
  // ImGui's own -- without it both would fire and one notch would scroll
  // 65 px further than intended. The scrollbar itself stays (the flag only
  // suppresses wheel handling, not the bar or dragging it).
  if (ImGui::Begin("##controls", nullptr,
                   (fixedFlags & ~ImGuiWindowFlags_NoScrollbar) |
                       ImGuiWindowFlags_NoScrollWithMouse)) {
    // Taken before the sections are emitted, because `GetScrollMaxY()` is only
    // meaningful once this frame's content has been laid out -- so the wheel
    // read here applies to the scroll range the PREVIOUS frame established,
    // which is the range the user was looking at when they turned the wheel.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        ImGui::GetScrollMaxY() > 0.0f) {
      const float step =
          controlsWheelScrollStep(ImGui::GetWindowHeight(), ImGui::GetFontSize());
      ImGui::SetScrollY(std::clamp(ImGui::GetScrollY() - wheel * step, 0.0f,
                                   ImGui::GetScrollMaxY()));
    }
    for (const ControlsSectionSpec& spec : controlsSections()) {
      // The board is a shallow-water idea: only the watercolour solver reads
      // `tiltX/tiltY`, and the section was inside the WATER branch before this
      // step. A section can be absent when its subject is; it is still in the
      // list, because the list is the column's order and not its contents.
      if (spec.section == ControlsSection::BoardTilt && st.mode != PaintMode::Watercolor)
        continue;
      // --controls-all-open: see AppState::controlsAllOpen. Once, so it is a
      // starting state rather than a mode that fights the user.
      if (st.controlsAllOpen) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
      // docs/ui.md section 1: caps labels are monospace. A section title is
      // the column's largest caps label, so it is the one where the face
      // change is most of what tells a header from the prose under it.
      pushAtelierMono();
      const bool open = ImGui::CollapsingHeader(
          spec.title, spec.defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
      popAtelierMono();
      // --controls-all-open <SECTION>: pin that header to the top of the
      // column so a screenshot can reach a section that is below the fold.
      // See AppState::controlsScrollTo.
      if (st.controlsScrollTo == spec.title) ImGui::SetScrollHereY(0.0f);
      if (!open) continue;
      switch (spec.section) {
        // PLAN.md Phase 5 step 1 ("Multiple layers in `Document`, with
        // reorder, visibility, lock, opacity"; PRD C4), and every entry point
        // UI detour step 3 added. See drawLayersSection()'s own doc comment.
        case ControlsSection::Layers:    drawLayersSection(st); break;
        // PLAN.md Phase 5 step 8 ("History panel ...", PRD O2/O3). docs/ui.md
        // §5: "The History panel (PRD O2) joins the right-hand docked column."
        // Below LAYERS, because a history row names an edit made to the stack
        // above it.
        case ControlsSection::History:   drawHistorySection(st, sim, gpu); break;
        // PLAN.md Phase 5 step 12 ("Layer comps ...", PRD C14). Below HISTORY,
        // because a comp is a saved state *of* the layer stack and a history
        // row is an edit *to* it. See drawCompsSection()'s own doc comment.
        case ControlsSection::Comps:     drawCompsSection(st); break;
        // PLAN.md Phase 3 step 8 ("Op-stack UI -- reorder, toggle, delete, and
        // a curve widget operating in the shaper domain").
        case ControlsSection::Grade:     drawGradeSection(st); break;
        // docs/ui.md section 3.3 / PRD L4. First in the column, which is the
        // design's own order -- see app/ControlsLayout.hpp's `Tool` role.
        case ControlsSection::Color:     drawColorSection(st); break;
        // Two panes, because picking a brush and authoring one are
        // different acts (drawBrushLibrarySection()'s own comment).
        // `gpu` and `lut` for the hover preview alone, which is a real
        // rasterised dab in a real texture -- the same pair, for the same
        // reason, that the BRUSH EDITOR below takes.
        case ControlsSection::BrushLibrary: drawBrushLibrarySection(st, gpu, lut); break;
        // `gpu` and `lut` for the TIP PREVIEW alone: the preview is a real
        // rasterised dab in a real texture, so this section needs the device
        // (to upload it) and the LUT (because `brushTipFor()` resolves the
        // loaded pigment through it). drawHistorySection() above already takes
        // a GpuContext for the same shape of reason.
        case ControlsSection::Brush:     drawBrushSection(st, gpu, lut); break;
        case ControlsSection::Pigment:   drawPigmentSection(st); break;
        case ControlsSection::Medium:    drawMediumSection(st, sim.get()); break;
        case ControlsSection::BoardTilt: drawBoardTiltSection(st); break;
        case ControlsSection::Grid:      drawGridSection(st); break;
        case ControlsSection::Solver:    drawSolverSection(st, sim.get()); break;
      }
      ImGui::Dummy(ImVec2(0, 6));
    }

    // The label column, measured and reported (UI detour step 3, problem 1b).
    // Printed when it changes rather than every frame: it settles on the first
    // frame that has drawn every open section once, and moves again only if a
    // section that opens later carries a wider label. A log line is what makes
    // "no label is clipped" a number rather than a look at a screenshot.
    if (g_labelColumn != g_reportedColumn) {
      g_reportedColumn = g_labelColumn;
      std::printf("[controls] label column %.0f px -- widest label \"%s\" at %.0f px, "
                  "panel %.0f px, so a slider gets %.0f px\n",
                  g_labelColumn, g_widestLabel.c_str(), g_widestLabelPx, kControlsW,
                  ImGui::GetContentRegionAvail().x - g_labelColumn);
    }
  }
  ImGui::End();

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
    const float texW = static_cast<float>(canvasW);
    const float texH = static_cast<float>(canvasH);

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
    // The document is drawn on the *canvas* quad, so a document whose own
    // dimensions differ from kCanvasW/kCanvasH is stretched onto the paper
    // rather than placed at its own size. That is right for today -- the
    // document a session starts with is exactly canvas-sized (see main.cpp),
    // and the two pictures are stacked precisely because they are not yet one
    // thing -- and it stops being a question at all once the stroke bridge
    // makes the document the canvas.
    //
    // No warnings are collected: `compositeDocumentPremultiplied()` would
    // report an unimplemented blend once per layer per upload, and the layers
    // panel already marks exactly those layers `(!)` on their own rows, which
    // is the same fact in the place a user can act on it.
    const OpenDocument* activeDocument = st.documents.active();
    WGPUTextureView documentView = nullptr;
    if (activeDocument != nullptr) {
      documentView = g_documentTextures.viewFor(gpu, *activeDocument);
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
      st.brush.radius =
          clampBrushRadius(radiusForDrag(st.brush.radius, ImGui::GetIO().MouseDelta.x));
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
    // --- zoom on wheel, anchored under the cursor ---
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
      applyZoomFactor(1.0f + ImGui::GetIO().MouseWheel * 0.12f, mouse);
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
      const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
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
          }
          if (st.marqueeDragging) {
            st.marqueeX1 = tx;
            st.marqueeY1 = ty;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
              st.marqueeDragging = false;
              if (od != nullptr) {
                // Clamped to the canvas: a drag that left the window would
                // otherwise select texels the document does not have, and
                // every consumer would then walk tiles holding nothing.
                const float x0 = std::clamp(std::min(st.marqueeX0, st.marqueeX1), 0.0f, texW);
                const float y0 = std::clamp(std::min(st.marqueeY0, st.marqueeY1), 0.0f, texH);
                const float x1 = std::clamp(std::max(st.marqueeX0, st.marqueeX1), 0.0f, texW);
                const float y1 = std::clamp(std::max(st.marqueeY0, st.marqueeY1), 0.0f, texH);
                std::optional<Selection> drawn;
                if (x1 - x0 >= 1.0f && y1 - y0 >= 1.0f) {
                  drawn = st.brush.tool == Tool::Marquee
                              ? selectRectangle(x0, y0, x1, y1)
                              // The drag's bounding box is the ellipse's
                              // bounding box, which is how every editor reads
                              // an ellipse drag -- centre and radii, not two
                              // points on the curve.
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
      const bool sampling = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
      if (sampling && (od == nullptr || (tx >= 0 && ty >= 0 && tx < texW && ty < texH)))
        applyEyedropperPick(st, PixelCoord{static_cast<int32_t>(tx), static_cast<int32_t>(ty)});
    }

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
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && tx >= 0 && ty >= 0 &&
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
            st.marqueeDragging = true;
            st.marqueeX0 = tx;
            st.marqueeY0 = ty;
          }
        }
        if (st.marqueeDragging) {
          st.marqueeX1 = tx;
          st.marqueeY1 = ty;
          if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            st.marqueeDragging = false;
            const float dx = st.marqueeX1 - st.marqueeX0;
            const float dy = st.marqueeY1 - st.marqueeY0;
            // A click with no drag has no direction, and a zero-length
            // gradient is not a fill -- it is an undefined ramp. Ignored
            // rather than guessed at.
            //
            // `usable` is re-tested rather than trusted from pen-down: the
            // drag spans frames, and `*target->rgbTiles` is dereferenced two
            // lines down. Nothing in this build can lock or retype a layer
            // while the pointer is held -- the same argument app/StrokeSession
            // §5 makes about its own latched target -- so this is the guard
            // being kept where it is relied on rather than where it happens to
            // have been established.
            if (usable && dx * dx + dy * dy >= 1.0f) {
              GradientGeometry geom;
              geom.kind = GradientKind::Linear;
              geom.x0 = st.marqueeX0;
              geom.y0 = st.marqueeY0;
              geom.x1 = st.marqueeX1;
              geom.y1 = st.marqueeY1;

              // **Foreground to transparent**, which is the only default this
              // build can honestly offer: docs/ui.md deliberately has no BG
              // half to the swatch (nothing fills with a background colour
              // until PRD D25/D26), so "foreground to background" would name a
              // colour that does not exist. The colour stops hold one colour
              // at both ends and the OPACITY stops do the fading -- which is
              // exactly why ops/Gradient keeps the two lists independent, and
              // is what stops the ramp darkening toward a transparent black
              // that was never a stop.
              GradientStops stops;
              stops.colorStops.push_back(ColorStop{0.0f, {fg[0], fg[1], fg[2]}, 0.5f});
              stops.colorStops.push_back(ColorStop{1.0f, {fg[0], fg[1], fg[2]}, 0.5f});
              stops.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
              stops.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});

              const GradientRegion region{0, 0, od->document.width, od->document.height};
              const Selection* sel =
                  od->selection.has_value() ? &*od->selection : nullptr;
              if (renderGradient(*target->rgbTiles, region, geom, stops, sel) > 0) {
                od->recordEdit("gradient", EditKind::Content);
              }
            }
          }
        }
      }
    }

    // --- stroke ---
    const bool paintTool = st.brush.tool == Tool::Brush ||
                           st.brush.tool == Tool::Water ||
                           st.brush.tool == Tool::DryBrush;
    // **The eraser is a stroke tool but never a SOLVER stroke**, which is why it
    // is a second flag rather than a fourth line above. It joins `paintTool` at
    // the two branches below that reach a layer and at the cursor ring, and is
    // deliberately absent from the branch that constructs `PaintSim`: that
    // simulation has no alpha and no erase step, so an eraser reaching it would
    // run the *paint* path and add pigment where the user asked for its removal
    // (app/StrokeSession.hpp §1's Eraser rows). Folding it into `paintTool` would
    // have been one word and would have made the eraser deposit watercolour on
    // the canvas texture the moment no document was open.
    const bool eraseTool = st.brush.tool == Tool::Eraser;
    const bool strokeTool = paintTool || eraseTool;
    const bool inside = tx >= 0 && ty >= 0 && tx < texW && ty < texH;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

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
      // **All three layer-writing routes come through here**, and this branch
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
      const BrushTip tip = brushTipFor(st.brush, lut, dynamicInputsFor(st));
      if (!g_stroke.active()) {
        g_strokeRefusal.clear();
        // **`&st.brush.links` is what makes the stroke-local dynamics sources
        // reach a real stroke.** Velocity, Fade, Noise and Random resolve per
        // dab inside `StrokeSession`, not once per frame in `dynamicInputsFor()`
        // above, so they can only be driven by a link set the session holds for
        // the stroke's whole life. The track that built those four sources could
        // not add this argument -- this block was reserved for another track at
        // the time -- and said so plainly: without it the four were reachable
        // from `--selftest` and from nowhere a user could click, which is the
        // exact defect class docs/reachability-audit.md exists to remove. It is
        // one argument, and it is the whole difference between "implemented" and
        // "reachable".
        //
        // Direction, and Initial Direction after it, added later as this
        // same link set's fifth and sixth stroke-local sources, needed no
        // second argument here -- each rides this identical
        // `&st.brush.links` pointer into `StrokeSession::begin()`, so each
        // was reachable from the live canvas from the moment
        // `sourceIsStrokeLocal()` learned about it, with no change to this
        // block at all.
        if (!g_stroke.begin(*strokeDoc, strokeDoc->activeLayer, tip, st.brush.tool,
                            &g_strokeRefusal, &st.brush.links)) {
          st.paintingThisFrame = false;
        }
        st.lastX = tx;
        st.lastY = ty;
      }
      if (g_stroke.active()) {
        // Per frame, from this frame's pressure -- the same granularity the
        // solver route gets, which sets one brushRadius per frame.
        g_stroke.setTip(tip);
        g_stroke.addPoint(tx, ty);
        st.lastX = tx;
        st.lastY = ty;
      }
    } else if (strokeTool && down && hovered && inside && !panning && !rotating && !sizingHeld &&
               !st.pendingGuide.has_value() && route == StrokeRoute::None &&
               (strokeTarget != nullptr || eraseTool)) {
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
      g_strokeRefusal =
          strokeTarget == nullptr
              ? std::string("no layer: the eraser has nothing to erase. Open a document, "
                            "or add a layer in LAYERS.")
          : strokeTarget->locked
              ? std::string("locked layer: \"") + strokeTarget->name + "\" cannot be " +
                    (eraseTool ? "erased" : "painted") + ". Clear its Lock in LAYERS."
          : eraseTool
              ? std::string("\"") + strokeTarget->name + "\" is " +
                    layerKindName(strokeTarget->kind) +
                    " and cannot be erased. Pick a Pigment or RGB layer in LAYERS."
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
      const bool wasNull = !sim;
      PaintSim* s = ensurePaintSim(sim, gpu, canvasW, canvasH, lut);
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
        st.sim.brushRadius = st.brush.radius * sizeMul;
        st.sim.brushPigment *= flowMul;
        st.sim.brushWater *= flowMul;

        // Arc-length dab emission (1.3 / ADR-0003): feed this frame's
        // sampled position through the centripetal Catmull-Rom emitter. It
        // returns 0..N dab positions spaced `spacing * radius` px apart
        // along the smoothed path, carrying any leftover sub-spacing
        // distance across frames so spacing stays continuous rather than
        // resetting on every render frame.
        const float spacingPx = std::max(st.brush.spacing * st.sim.brushRadius, 0.1f);
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
        const float spacingPx = std::max(st.brush.spacing * st.sim.brushRadius, 0.1f);
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

    if (st.marqueeDragging &&
        (st.brush.tool == Tool::Marquee || st.brush.tool == Tool::EllipseMarquee)) {
      // The live rubber band, from the drag's own corners.
      drawMarchingAnts(dl, xform, std::min(st.marqueeX0, st.marqueeX1),
                       std::min(st.marqueeY0, st.marqueeY1),
                       std::max(st.marqueeX0, st.marqueeX1),
                       std::max(st.marqueeY0, st.marqueeY1));
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
      dl->AddCircle(mouse, st.brush.radius * st.view.zoom,
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
      } else {
        // The tool, against the layer it is actually pointed at -- so a brush
        // over a locked or unpaintable layer, and a bucket over a Pigment
        // layer, show the slashed circle *before* the gesture is spent rather
        // than a sentence in another band afterwards. `strokeTarget` is the
        // same active layer the stroke and fill blocks above route on, read
        // once for all three.
        g_canvasCursor = sdlCursorFor(toolCursorOnTarget(st.brush.tool, strokeTarget));
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
        const WGPUTextureView v = g_documentTextures.viewFor(gpu, *other);
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
  // After the canvas rather than before it, for one reason that is not
  // cosmetic: both read `st.brush` and `st.view`, and a band drawn first
  // would show the values as they were before this frame's canvas input
  // changed them -- a zoom readout one frame stale, which is exactly the
  // juddering docs/ui.md section 5 asks the monospace numerics to prevent.
  if (drawAtelierTabStrip(st, bands, g_split, &g_docStatus)) {
    st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(canvasW),
                                           static_cast<int32_t>(canvasH), WorkingSpace{}));
    g_docStatus.clear();
  }
  drawAtelierOptionsBar(st, bands, g_strokeRefusal);
  drawAtelierStatusBar(st, bands, canvasW, canvasH);
  // Last, and on the foreground draw list: a 2 px rule that a neighbouring
  // window overdrew by a pixel would be a 1 px rule, and the design's whole
  // separation of major regions is that thickness.
  drawAtelierRules(bands);

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

std::optional<SDL_SystemCursor> canvasCursorRequest() { return g_canvasCursor; }

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
