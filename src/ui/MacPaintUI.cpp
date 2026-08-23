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
#include <string>
#include <vector>

#include "app/CompPanel.hpp"
#include "app/ControlsLayout.hpp"
#include "app/CurveEdit.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/HistoryPanel.hpp"
#include "app/Journal.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "color/LutBake.hpp"  // kMaxCurvePointsPerChannel
#include "core/LayerCompOps.hpp"
#include "core/LayerOps.hpp"
#include "imgui.h"
#include "io/ExportAs.hpp"
#include "io/ExportStates.hpp"
#include "ui/CanvasQuad.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/Fonts.hpp"

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

// PRD E6's marching ants, for an axis-aligned rectangle in document texel
// space.
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
// **Rectangles only.** The selection store is general (sparse coverage), but
// the only constructor is `selectRectangle()`, so a rectangle is the only
// boundary there is to draw. When lasso and wand land (PRD E3) this will need
// a real boundary trace rather than a bounding box, and the bounding box would
// then be actively wrong rather than merely approximate. Named here so that is
// a known cost and not a surprise.
void drawMarchingAnts(ImDrawList* dl, const ViewTransform& xform, float x0, float y0,
                      float x1, float y1) {
  const Vec2 c[4] = {xform.toScreen(Vec2{x0, y0}), xform.toScreen(Vec2{x1, y0}),
                     xform.toScreen(Vec2{x1, y1}), xform.toScreen(Vec2{x0, y1})};
  constexpr ImU32 kDark = IM_COL32(0, 0, 0, 190);
  constexpr ImU32 kLight = IM_COL32(255, 255, 255, 230);
  constexpr float kDash = 6.0f;
  const float phase = static_cast<float>(std::fmod(ImGui::GetTime() * 18.0, kDash * 2.0));

  for (int i = 0; i < 4; ++i) {
    const ImVec2 a(c[i].x, c[i].y);
    const ImVec2 b(c[(i + 1) % 4].x, c[(i + 1) % 4].y);
    dl->AddLine(a, b, kDark, 1.0f);
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) continue;
    const float ux = dx / len, uy = dy / len;
    for (float t = -phase; t < len; t += kDash * 2.0f) {
      const float s = std::max(t, 0.0f);
      const float e = std::min(t + kDash, len);
      if (e <= s) continue;
      dl->AddLine(ImVec2(a.x + ux * s, a.y + uy * s), ImVec2(a.x + ux * e, a.y + uy * e),
                  kLight, 1.0f);
    }
  }
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
bool drawCurveWidget(Curve& curve) {
  constexpr float kPlotSize = 200.0f;
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
  textDisabledWrapped("%s -- %d x %d, %zu layer(s)", documentDisplayName(*od).c_str(), doc.width,
                      doc.height, doc.layers.size());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The layers of the open document, composited and drawn\n"
                      "on the canvas over the paper.\n"
                      "Painting is still separate: sim::PaintSim owns one dense\n"
                      "texture with no layer awareness, so a stroke reaches no\n"
                      "layer and nothing painted appears here.");
  // The revision cache, measured rather than believed (ui/DocumentTexture.hpp
  // owns the argument). `uploads` counts recomposites; `cached` counts frames
  // that cost two integer comparisons. In a still window the second number
  // climbs at the frame rate and the first does not move at all.
  textDisabledWrapped("composite: %llu upload(s), %llu cached, last %.2f ms",
                      static_cast<unsigned long long>(g_documentTextures.uploads()),
                      static_cast<unsigned long long>(g_documentTextures.cacheHits()),
                      g_documentTextures.lastUploadMs());
  // PRD **A6** (P0) on screen, in bytes, for the same reason the line above is:
  // "only visible documents hold GPU textures, at most two" is a claim about
  // memory, and a claim about memory that only `--selftest` can see is one a
  // running session can drift away from unnoticed. With twenty tabs open this
  // reads `2 / 2 documents resident` and the megabytes do not move.
  textDisabledWrapped("GPU: %zu / %zu document(s) resident, %.1f MB",
                      g_documentTextures.residentDocuments(), kVisibleDocumentCap,
                      static_cast<double>(g_documentTextures.gpuTextureBytes()) /
                          (1024.0 * 1024.0));

  static char renameBuf[128] = "";
  // A reference to the document's own active layer, so every mutation below
  // -- click, ctrl-click, shift-range, Up/Down, a command's returned row --
  // moves what the brush paints into as well as what this panel edits. The
  // clamp on the next line is the reason `activeLayerIndex()` exists: a stack
  // can shrink under a stored index between frames.
  size_t& selected = od->activeLayer;
  if (selected >= doc.layers.size()) selected = doc.layers.empty() ? 0 : doc.layers.size() - 1;

  auto run = [&](LayerOpResult r) {
    const DocumentOpResult out = recordLayerEdit(*od, std::move(r));
    g_layers.lastError = out.ok ? std::string() : out.error;
    return out.ok;
  };

  // --- The filter and the multi-selection (PLAN.md Phase 5 step 11) --------
  //
  // app/LayerPanel.hpp states the filter's whole rule; the two halves visible
  // here are that a hidden row **stays selected** (so clearing the box brings
  // it back) and that `runLayerSetCommand()` restricts every gesture to the
  // rows on screen.
  ImGui::SetNextItemWidth(120.0f);
  if (ImGui::InputTextWithHint("##layerfilter", "filter by name", g_layers.filterBuf,
                               sizeof(g_layers.filterBuf)))
    g_layers.filter.text = g_layers.filterBuf;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Filters which rows are drawn -- nothing else.\n"
                      "A hidden row stays selected, so clearing this\n"
                      "brings it back, and a Multi-selection command\n"
                      "acts only on the rows you can see.");
  ImGui::SameLine();
  {
    const char* kindPreview =
        g_layers.filter.kind.has_value() ? layerKindName(*g_layers.filter.kind) : "any kind";
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::BeginCombo("##layerkindfilter", kindPreview)) {
      if (ImGui::Selectable("any kind", !g_layers.filter.kind.has_value()))
        g_layers.filter.kind.reset();
      for (int k = 0; k <= static_cast<int>(LayerKind::Flats); ++k) {
        const LayerKind kind = static_cast<LayerKind>(k);
        const bool on = g_layers.filter.kind.has_value() && *g_layers.filter.kind == kind;
        if (ImGui::Selectable(layerKindName(kind), on)) g_layers.filter.kind = kind;
      }
      ImGui::EndCombo();
    }
  }

  // Every set command, walked from `core::allLayerSetCommands()` -- the same
  // list the `Layer` > Selection menu walks, so the panel and the menu cannot
  // offer different sets. One button per line rather than a packed grid: these
  // labels are long, a 300 px panel clips them, and a clipped label is the
  // exact failure app/ControlsLayout exists to have fixed once. Collapsed by
  // default, so a single-selection session never sees it.
  // --controls-all-open opens this one too, so `--screenshot` can photograph a
  // section whose default state is closed -- the same reason the flag exists
  // for the outer sections (app/AppState.hpp).
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

  // **The active layer's blend mode, above the list** (Krita's own docker
  // layout: a persistent bar naming the selected layer's blend, not a
  // per-row control). Bare -- no `ctlBeginCombo()` label column, because
  // Krita's own bar has no "Blend:" prefix, just the mode filling the row --
  // and it always describes `selected`, so clicking a different row changes
  // what this reads on the very next frame with no state of its own to sync.
  if (selected < doc.layers.size()) {
    const std::vector<BlendMode> menu = blendMenuForLayer(doc, selected);
    const size_t sel = blendMenuSelection(doc, selected, menu);
    const std::string preview = sel < menu.size()
                                    ? blendMenuEntryText(menu[sel])
                                    : doc.layers[selected].blend + "  (this build cannot set this)";
    ImGui::SetNextItemWidth(-1.0f);
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
                        "L5), and does not composite yet.");
  } else {
    ImGui::TextDisabled("No layer selected.");
  }

  // Rows, top of the stack first. `structureChanged` stops the loop the same
  // frame a reorder/add/remove fires rather than continuing to render rows
  // against indices that no longer mean what they did -- the identical
  // precaution drawOpStackEditor() takes over core::OpStack, and for the
  // identical reason (core/LayerOps' move/remove shift the vector).
  bool structureChanged = false;
  const size_t count = doc.layers.size();
  // The rows the filter lets through. `layersMatchingFilter()` returns model
  // indices ascending (bottom-first), so walking it backwards is the panel's
  // own top-first order -- app/LayerPanel still owns the single reversal in the
  // codebase and nothing here computes `count - 1 - row` a second time.
  const std::vector<size_t> visibleRows = layersMatchingFilter(doc, g_layers.filter);
  if (visibleRows.empty() && count > 0)
    ImGui::TextDisabled("no layer matches the filter (%zu hidden)", count);
  for (size_t vr = visibleRows.size(); vr-- > 0 && !structureChanged;) {
    const size_t i = visibleRows[vr];
    const Layer& layer = doc.layers[i];
    ImGui::PushID(static_cast<int>(i));

    bool visible = layer.visible;
    if (ImGui::Checkbox("##vis", &visible)) {
      run(setLayerVisible(doc, i, visible));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Visibility. Allowed even on a locked layer --\n"
                        "hiding a layer changes nothing about it.");
    ImGui::SameLine();
    // Lock, moved here from the row's old second line (Krita's own row is a
    // single line too, with the lock as a bare icon rather than a labelled
    // checkbox -- app::layerKindGlyphForFont()'s font-or-fallback idiom is
    // for a *glyph*, not a checkbox, so this stays a checkbox with no visible
    // label, exactly like Visibility above it).
    bool locked = layer.locked;
    if (ImGui::Checkbox("##lock", &locked)) run(setLayerLocked(doc, i, locked));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Locked. A locked layer refuses edits that change its\n"
                        "pixels or its place in the stack -- visibility and\n"
                        "this checkbox itself still work.");
    ImGui::SameLine();
    // The colour label chip (PLAN.md Phase 5 step 11, PRD C15). Drawn only for
    // a label this build has a swatch for; an unrecognised one from a newer
    // build shows as text in the sub-line instead, because painting it in some
    // default colour would make two different labels look like one
    // (app/LayerPanel.hpp).
    if (const std::optional<LayerLabelSwatch> swatch = layerColorLabelSwatch(layer.colorLabel)) {
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float h = ImGui::GetTextLineHeight();
      ImGui::GetWindowDrawList()->AddRectFilled(
          ImVec2(at.x, at.y + 2.0f), ImVec2(at.x + 8.0f, at.y + h - 1.0f),
          ImGui::GetColorU32(ImVec4(swatch->r, swatch->g, swatch->b, 1.0f)), 2.0f);
      ImGui::Dummy(ImVec2(9.0f, h));
      ImGui::SameLine();
    }
    ImGui::TextUnformatted(layerKindGlyphForFont(layer.kind).c_str());
    ImGui::SameLine();
    if (g_layers.renaming.has_value() && *g_layers.renaming == i) {
      // **Inline rename.** The field replaces the Selectable entirely rather
      // than sitting beside it -- two editable widgets for one value on
      // screen at once is what the properties panel's own "Name" field would
      // otherwise become the moment this row is also the selection, so that
      // field is suppressed below for exactly this row while this is up.
      if (g_layers.renameFieldFocusPending) {
        ImGui::SetKeyboardFocusHere();
        g_layers.renameFieldFocusPending = false;
      }
      ImGui::SetNextItemWidth(-1.0f);
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
      // Multi-select (PRD C12). Plain click replaces the selection, ctrl-click
      // (cmd-click on this platform) toggles one row, shift-click extends from
      // the primary row to this one. `selected` follows the row that was
      // clicked in every case, so the property editors below always describe a
      // row the user just touched. `AllowDoubleClick` makes a double-click
      // still report as a click here (so single-click selection keeps
      // working) and separately queryable via `IsMouseDoubleClicked()` right
      // after, which is what starts the inline rename above.
      const bool inSelection = g_layers.selection.contains(i);
      const bool clicked = ImGui::Selectable(layerRowTitle(layer, i).c_str(), inSelection,
                                             ImGuiSelectableFlags_AllowDoubleClick);
      // The old second line -- docs/ui.md §3.2's `KIND · BLEND · OPACITY% ·
      // flags` sub-line -- lives here now instead of always being on screen,
      // which is what keeps every row exactly one line tall. The information
      // did not go away: `layerRowSubLine()` is unchanged and still tested,
      // this is only where its result is drawn.
      if (ImGui::IsItemHovered()) {
        std::string sub = layerRowSubLine(layer);
        if (const size_t partners = layerLinkPartnerCount(doc, i); partners > 0)
          sub += " \xC2\xB7 LINKED+" + std::to_string(partners);
        ImGui::SetTooltip("%s", sub.c_str());
      }
      if (clicked) {
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
          // leaves it selected rather than leaving the panel with no primary
          // row for the opacity slider and the name field to describe.
          if (next.empty()) next.push_back(i);
        } else {
          next.push_back(i);
        }
        g_layers.selection = makeLayerSelection(std::move(next));
        selected = i;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          g_layers.renaming = i;
          std::snprintf(g_layers.renameFieldBuf, sizeof(g_layers.renameFieldBuf), "%s",
                       layer.name.c_str());
          g_layers.renameFieldFocusPending = true;
        }
      }

      // **Right-click context menu.** Selects the row first, the same way a
      // left-click does, so a command chosen from the menu acts on the row
      // the user right-clicked rather than whatever was selected before --
      // and so `runLayerCommand()` below, which always acts on
      // `OpenDocument::activeLayer`, is acting on the right one.
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        g_layers.selection = singleLayerSelection(i);
        selected = i;
      }
      if (ImGui::BeginPopupContextItem("layerRowContext")) {
        for (const LayerCommand command : allLayerCommands()) {
          // CaptureComp captures the whole stack, not this row -- it belongs
          // to the COMPS panel and the `Layer` menu, not a per-row menu whose
          // every other entry is about the layer under the cursor.
          if (command == LayerCommand::CaptureComp) continue;
          const bool available = layerCommandAvailable(doc, command, i);
          if (ImGui::MenuItem(layerCommandLabel(command), nullptr, false, available)) {
            runLayerCommand(st, command);
            structureChanged = true;
          }
        }
        // **Add Op, for now.** The full op-stack editor moved to Layer
        // Properties along with everything else that used to live under the
        // selected row -- but *adding* one is a single gesture with no
        // parameters to show yet (`app::makeNewOp()` always builds a
        // disabled PointA op), so it gets a fast path here rather than
        // waiting for the dialog to be opened first.
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

      // **Drag to reorder.** The dragged payload is the model index; the
      // drop target is whichever row's rectangle the pointer released over,
      // refined by which half of that row it was in
      // (`app::layerDropTargetIndex()` turns "row + half" into the `to`
      // `core::moveLayer()` wants). Not offered while renaming -- a text
      // field and a drag source on the same item would fight over the mouse.
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
        ImGui::SetDragDropPayload("NP_LAYER_ROW", &i, sizeof(size_t));
        ImGui::TextUnformatted(layerRowTitle(layer, i).c_str());
        ImGui::EndDragDropSource();
      }
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NP_LAYER_ROW")) {
          const size_t from = *static_cast<const size_t*>(payload->Data);
          const ImVec2 rowMin = ImGui::GetItemRectMin();
          const ImVec2 rowMax = ImGui::GetItemRectMax();
          const bool droppedAboveMidpoint =
              ImGui::GetMousePos().y < (rowMin.y + rowMax.y) * 0.5f;
          const size_t to = layerDropTargetIndex(i, droppedAboveMidpoint, count);
          if (from != to) run(moveLayer(doc, from, to));
          structureChanged = true;
        }
        ImGui::EndDragDropTarget();
      }
    }
    // **One row, one line.** Up/Down are gone -- drag-to-reorder above does
    // their job -- and Opacity, Blend, Name, Clip and the op stack all moved
    // to the Layer Properties dialog (the gear button above the list), the
    // same relocation Krita's own docker makes: a row is identity and
    // structure (eye, lock, kind, name), everything else is one dialog away.
    ImGui::PopID();
  }

  // The compact icon toolbar (Photoshop/Affinity/Krita convention: a strip of
  // small glyph buttons under the stack, not a column of abbreviated text
  // ones). Below the list rather than above it -- Krita's own docker puts
  // its equivalent toolbar at the bottom too, under the rows it acts on, not
  // between the document header and the thing it edits. Every icon still
  // funnels through `layerCommandIconButton()`'s twin `layerCommandButton()`
  // -- same availability predicate, same tooltip, same dispatch -- so this
  // is a second face on each command, not a second implementation of one.
  // Two rows, split the way the five-row text version already split it:
  // creation and the single-layer edits first, then PLAN.md Phase 5 step 10
  // / PRD C10 (P0), C11 (P1)'s five operations that consume layers -- undo
  // is the only way back from any of those, which is exactly why the old
  // layout gave them their own row too.
  layerCommandIconButton(st, LayerCommand::NewRgbLayer);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::NewPigmentLayer);
  ImGui::SameLine();
  layerCommandIconButton(st, LayerCommand::NewAdjustmentLayer);
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
  // modal below, which is where Opacity, Blending mode, Colour label,
  // Visible, Locked, Clip and the op stack all moved to once the row itself
  // became one line. Not a `LayerCommand` -- it mutates no document, only
  // opens a dialog -- so it draws its own button rather than going through
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

  // --- Layer Properties (Krita's own dialog, opened by the gear above) -----
  //
  // Everything a selected row used to expand to show now lives here instead,
  // bound to `selected` for the dialog's whole open lifetime -- a
  // `BeginPopupModal` blocks every other widget in the application, so
  // `selected` cannot change out from under it mid-edit the way it could while
  // it was inline in the list. Kind, and everything the reference dialog
  // shows that this build has no equivalent of (colour space, ICC profile,
  // per-channel toggles), is left out rather than faked with a control that
  // would do nothing -- the same rule the design doc's own PRESET dropdown
  // and BG swatch follow.
  //
  // --open-layer-properties: the same id BeginPopupModal() opens on a click,
  // so --screenshot can photograph it. See AppState::openLayerProperties.
  if (st.openLayerProperties) {
    st.openLayerProperties = false;
    ImGui::OpenPopup("Layer Properties");
  }
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

      // The blend dropdown, identical in every particular to the one above
      // the list -- two controls for the one field, exactly the redundancy
      // the reference dialog itself has (Krita's docker bar and its own
      // Layer Properties both carry Blending mode).
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
                          "another Pigment layer (PRD L5), and does not composite\n"
                          "yet: no layer stores a pigment latent until Pigment\n"
                          "tiles land.");

      ImGui::TextUnformatted("Colour label");
      ImGui::SameLine();
      // "None" first, then the seven `kLayerColorLabelNames` swatches, each a
      // small filled square rather than a text button -- `layerColorLabelSwatch()`
      // is the same lookup the row chip uses, so a colour picked here is the
      // exact colour that shows there. `PushID(name)` gives each swatch its
      // own ID despite every one of them sharing the label "##colorLabel".
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
      // the same "nothing below this layer" rule the row's own checkbox used
      // to enforce.
      ImGui::BeginDisabled(i == 0 && !layer.clipped);
      bool clipped = layer.clipped;
      if (ImGui::Checkbox("Clip to Layer Below", &clipped)) run(setLayerClipped(doc, i, clipped));
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Clip to the layer below (PRD C9): this layer shows only\n"
                          "where the layer beneath it has alpha. The bottom layer\n"
                          "cannot be clipped: there is nothing below it.");

      // The layer's own op stack (PLAN.md Phase 5 steps 3 and 5). Unchanged
      // from where it used to live inline under the row -- only the location
      // moved. On an Adjustment layer this stack is the layer's entire
      // content: the kind holds no pixels, so a fresh one is an exact no-op
      // until an op here is added *and* enabled.
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

  // The refusal, verbatim. core/LayerOps' sentences already name the layer and
  // say what to do about it, so there is no second vocabulary here to drift
  // from the model's -- the same rule drawExportAsDialog() follows for
  // io/Export's messages. Shared with the `Layer` menu: a command refused from
  // the menu bar is answered here, because this is where a user is looking at
  // the layer it was refused on.
  if (!g_layers.lastError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 110, 255));
    ImGui::TextWrapped("%s", g_layers.lastError.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss")) g_layers.lastError.clear();
  }

  // PLAN.md Phase 5 step 10. A merge that succeeded and still has something to
  // say: a mask baked into alpha, an op stack turned destructive, tiles that
  // fell outside the canvas, a stamp that will now build up against itself.
  // Amber rather than red, because none of these is a failure -- and drawn
  // rather than dropped, because every one of them names something that was
  // reversible a moment ago and is not now.
  if (!g_layers.lastWarnings.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 180, 100, 255));
    for (const std::string& w : g_layers.lastWarnings) ImGui::TextWrapped("%s", w.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss##layerwarnings")) g_layers.lastWarnings.clear();
  }
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
// Panel-local state, like `drawCompsSection()`'s selection: which mode the
// panel is in is not a property of the document and no file records it.
bool g_colorPigmentMode = true;
float g_colorRgb[3] = {0.10f, 0.12f, 0.45f};

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
  if (modeButton("PIGMENT", g_colorPigmentMode)) g_colorPigmentMode = true;
  ImGui::SameLine();
  if (modeButton("RGB", !g_colorPigmentMode)) g_colorPigmentMode = false;

  const std::vector<Pigment>& palette = defaultPalette();
  const Pigment& sel = palette[st.brush.pigment];

  if (g_colorPigmentMode) {
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
    ImGui::ColorPicker3("##rgb", g_colorRgb,
                        ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
                            ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Float);
    // Said plainly rather than left for a user to discover by painting.
    // Section 3.3 allows a raw RGB colour on a Pigment layer -- "it maps
    // through RGB->latent, with the caveat ... that the decomposition is
    // plausible rather than true" -- but the mapping is not built, and neither
    // is the path from any colour here to a deposit: the pen is not yet wired
    // to a layer at all (PLAN.md's Phase 5 finding).
    ImGui::TextWrapped("Not yet connected: no tool reads this colour. The brush deposits the "
                       "PIGMENT selection, which carries physical constants an RGB triple "
                       "does not.");
  }
}

void drawBrushSection(AppState& st) {
  ctlSlider("Load", &st.brush.load, 0.0f, 2.5f);
  ctlSlider("Water", &st.brush.wetness, 0.0f, 3.0f);
  ctlSlider("Hardness", &st.brush.hardness, 0.0f, 1.0f);
  ImGui::Checkbox("Pressure -> size", &st.brush.pressureSize);
  ImGui::Checkbox("Pressure -> flow", &st.brush.pressureFlow);
  if (!st.penSeen) ImGui::TextDisabled("(no tablet detected)");
}

void drawPigmentSection(AppState& st) {
  ctlSlider("Density", &st.sim.density, 0.0f, 1.0f);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How fast pigment drops out of suspension.");
  ctlSlider("Staining", &st.sim.staining, 0.02f, 1.0f);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Resistance to being lifted back into the water.");
  ctlSlider("Granulation", &st.sim.granulation, 0.0f, 1.0f);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Affinity for the paper's valleys.");
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
  od.selection = std::move(selection);
  ++od.selectionRevision;
}

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
// it. Every call site below therefore settles first and reads the cursor
// second. Row clicks in the panel are identified by *serial* and survive
// either order, but they go through the same path so there is one rule.
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
  // Settles before the caller computes its target -- see
  // settleWetPaintBeforeHistoryMove()'s comment on why the order matters.
  auto settle = [&]() { settleWetPaintBeforeHistoryMove(st, sim, gpu, *od); };
  auto install = [&](const HistoryPanelClick& r) { installHistoryCursor(*od, r); };

  const std::vector<HistoryPanelRow> rows = historyPanelRows(h);
  ImGui::TextDisabled("%zu state(s), cursor on %zu", rows.size(),
                      rows.empty() ? 0 : h.cursor() + 1);

  // Undo and Redo go through the panel's own click rather than
  // `History::undo()` / `redo()`, so there is exactly one path from a widget to
  // a cursor move and exactly one place a refusal can come from. The two guards
  // are `canUndo()` / `canRedo()`, so the neighbouring row always exists and
  // `cursor() - 1` cannot wrap.
  ImGui::BeginDisabled(!h.canUndo());
  if (ImGui::SmallButton("Undo")) {
    settle();
    install(historyPanelClick(h, historySerialForRow(h, h.cursor() - 1)));
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!h.canRedo());
  // Redo settles too, and the consequence is worth stating: if paint was
  // applied after an undo, the forced bake records a new entry, which
  // truncates the redo tail -- so the target computed on the next line
  // correctly finds no such row and the click refuses by name. That is the
  // honest answer ("you painted after undoing, so there is nothing to redo")
  // and it is only reachable because the target is computed AFTER the settle.
  // Computing it first would install a state the history no longer holds.
  if (ImGui::SmallButton("Redo")) {
    settle();
    install(historyPanelClick(h, historySerialForRow(h, h.cursor() + 1)));
  }
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
enum class DocPathAction { None, Open, SaveAs, SaveCopy };

bool g_docPathRequested = false;
DocPathAction g_docPathAction = DocPathAction::None;
bool g_revertConfirmRequested = false;
std::string g_docStatus;

void applyDocumentPathAction(AppState& st, DocPathAction action, const std::string& path) {
  OpenDocument* doc = st.documents.active();
  DocumentOpResult r;
  switch (action) {
    case DocPathAction::Open: {
      OpenDocument opened;
      r = openNpaintDocument(path, &opened, &st.recentDocuments);
      if (r.ok) st.documents.add(std::move(opened));
      break;
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
    case DocPathAction::None:
      return;
  }
  if (r.ok) {
    std::string saveErr;
    st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    g_docStatus = "OK: " + r.path;
    for (const std::string& w : r.warnings) g_docStatus += "\n! " + w;
  } else {
    g_docStatus = r.error;
  }
}

void drawDocumentDialogs(AppState& st) {
  static char pathBuf[512] = "";

  if (g_docPathRequested) {
    g_docPathRequested = false;
    g_docStatus.clear();
    // Pre-fill with the active document's own path, so Save As on an already
    // saved document starts from its name rather than from nothing.
    if (const OpenDocument* d = st.documents.active())
      std::snprintf(pathBuf, sizeof(pathBuf), "%s", d->path.c_str());
    ImGui::OpenPopup("Document path");
  }
  if (ImGui::BeginPopupModal("Document path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* verb = g_docPathAction == DocPathAction::Open      ? "Open"
                       : g_docPathAction == DocPathAction::SaveAs  ? "Save As"
                                                                   : "Save a Copy";
    ImGui::Text("%s", verb);
    if (g_docPathAction == DocPathAction::SaveCopy)
      ImGui::TextDisabled("Writes elsewhere; this document stays bound to its own file.");
    ImGui::SetNextItemWidth(480.0f);
    ImGui::InputText("Path", pathBuf, sizeof(pathBuf));
    if (ImGui::Button(verb)) {
      applyDocumentPathAction(st, g_docPathAction, pathBuf);
      if (g_docStatus.rfind("OK: ", 0) == 0) ImGui::CloseCurrentPopup();
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

// The File menu's document half. Split out of drawUI's menu bar block only
// because it is long, not because it is separable.
void drawDocumentMenuItems(AppState& st, uint32_t canvasW, uint32_t canvasH) {
  // Lazy, first time the File menu is opened: PRD A2 and ADR-0001 both say a
  // file nobody asked for costs nothing, and --selftest's idle-RSS assertion
  // is sampled before this can run.
  if (!st.recentDocumentsLoaded) {
    st.recentDocumentsLoaded = true;
    st.recentDocuments.loadFromFile(defaultRecentDocumentsPath());
  }

  OpenDocument* doc = st.documents.active();
  const bool hasDoc = doc != nullptr;
  const bool hasPath = hasDoc && doc->hasPath();

  if (ImGui::MenuItem("New Document")) {
    st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(canvasW),
                                           static_cast<int32_t>(canvasH), WorkingSpace{}));
    g_docStatus.clear();
  }
  if (ImGui::MenuItem("Open...")) {
    g_docPathAction = DocPathAction::Open;
    g_docPathRequested = true;
  }
  if (ImGui::BeginMenu("Open Recent", !st.recentDocuments.entries().empty())) {
    const std::vector<RecentDocument>& entries = st.recentDocuments.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
      // A missing entry is shown, greyed, with the reason -- never dropped
      // behind the user's back. app/DocumentLifecycle.hpp argues why.
      std::string why;
      const bool missing = recentDocumentMissing(entries[i].path, &why);
      if (missing) ImGui::BeginDisabled();
      if (ImGui::MenuItem(entries[i].displayName.c_str()) && !missing) {
        OpenDocument opened;
        const DocumentOpResult r = openRecentDocument(st.recentDocuments, i, &opened);
        if (r.ok) {
          st.documents.add(std::move(opened));
          std::string saveErr;
          st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
        }
        g_docStatus = r.ok ? "Opened " + r.path : r.error;
      }
      if (missing) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", why.c_str());
      } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", entries[i].path.c_str());
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear Menu")) {
      st.recentDocuments.clear();
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Save", nullptr, false, hasPath)) {
    const DocumentOpResult r = saveDocument(*doc, {}, &st.recentDocuments);
    if (r.ok) {
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    g_docStatus = r.ok ? "Saved " + r.path : r.error;
  }
  if (ImGui::MenuItem("Save As...", nullptr, false, hasDoc)) {
    g_docPathAction = DocPathAction::SaveAs;
    g_docPathRequested = true;
  }
  if (ImGui::MenuItem("Save a Copy...", nullptr, false, hasDoc)) {
    g_docPathAction = DocPathAction::SaveCopy;
    g_docPathRequested = true;
  }
  if (ImGui::MenuItem("Save Incremental", nullptr, false, hasPath)) {
    const DocumentOpResult r = saveDocumentIncremental(*doc, {}, &st.recentDocuments);
    if (r.ok) {
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    g_docStatus = r.ok ? "Saved " + r.path : r.error;
  }

  ImGui::Separator();
  // PLAN.md Phase 4 step 9 / PRD O8. Always enabled, even with nothing to
  // offer: "are there unfinished sessions?" is a question a user who has just
  // had a crash will ask, and a greyed-out item answers it ambiguously. The
  // list is re-scanned rather than reused, so a session another instance
  // finished since launch is not offered from a stale snapshot.
  if (ImGui::MenuItem("Recover Documents...")) {
    st.recovery = discoverRecoverySessions();
    g_recoveryRequested = true;
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Revert", nullptr, false, hasPath)) g_revertConfirmRequested = true;
  if (ImGui::MenuItem("Duplicate Document", nullptr, false, hasDoc)) {
    st.documents.add(duplicateDocument(*doc));
    g_docStatus = "Duplicated (unbound -- Save As gives it a file of its own).";
  }
  if (ImGui::MenuItem("Close Document", nullptr, false, hasDoc)) {
    std::string err;
    // Refuses a dirty document by name. The menu is not the place to offer a
    // discard, so the message says what to do instead.
    if (!st.documents.close(st.documents.activeIndex(), false, &err)) g_docStatus = err;
  }
}

}  // namespace

void drawUI(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();

  // ------------------------------------------------------------ title bar
  //
  // docs/ui.md section 2's first band: 36 px, wordmark then menus, undo/redo
  // at the right. ImGui fixes the main menu bar's height at `GetFrameHeight()`
  // -- font size plus twice the frame padding -- so the padding is pushed for
  // exactly the `BeginMainMenuBar()` call that reads it and popped
  // immediately, leaving every *menu item* at its normal size. The band is
  // 36 px because the design says 36 px; the items are then centred in it.
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

    if (ImGui::BeginMenu("File")) {
      // "New" is the *canvas* command it has always been -- it clears the
      // solver texture. "New Document" below is a different thing entirely,
      // and the two are deliberately not merged: the canvas is not a
      // document in this build, and a menu that implied otherwise would be
      // the first place the gap got papered over.
      if (ImGui::MenuItem("New Canvas", "Cmd+N")) st.requestClear = true;
      ImGui::Separator();
      // PLAN.md Phase 4 step 8 / PRD I18.
      drawDocumentMenuItems(st, canvasW, canvasH);
      ImGui::Separator();
      // PLAN.md Phase 4 step 7 / PRD I15. No shortcut string: docs/shortcuts.md
      // has not assigned one and keymaps/default.json binds no action for it,
      // so advertising a chord that resolves to nothing would be worse than
      // menu-only -- the same call the Rulers item below already makes.
      // Sets a flag rather than calling ImGui::OpenPopup() here: a modal
      // opened from inside BeginMenu() would be opened against the menu's own
      // ID stack, and drawExportAsDialog() below runs outside it.
      if (ImGui::MenuItem("Export As...")) g_exportAsRequested = true;
      // PLAN.md Phase 5 step 13 / PRD I16, I17. Next to Export As because it
      // is Export As run in a loop over document states -- same presets, same
      // four settings, same encoder -- and the flag rather than
      // ImGui::OpenPopup() for the same ID-stack reason as above.
      if (ImGui::MenuItem("Export Comps / Layers To Files..."))
        g_exportStatesRequested = true;
      ImGui::Separator();
      if (ImGui::MenuItem("Quit", "Cmd+Q")) st.quit = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Clear Canvas", "Cmd+K")) st.requestClear = true;
      ImGui::EndMenu();
    }
    // UI detour step 3. The whole menu is `app::allLayerCommands()` walked in
    // order: a command added to that list without a menu entry is impossible,
    // which is the failure this step exists to fix (five built features with
    // no entry point at all). No shortcut strings -- docs/shortcuts.md assigns
    // none of these and keymaps/default.json binds no action for them, so
    // advertising a chord that resolves to nothing would be worse than
    // menu-only, the same call File > Export As... already makes.
    // --open-layer-menu: the same id BeginMenu() below opens on a click, so
    // the menu can be photographed. See AppState::openLayerMenu.
    if (st.openLayerMenu) ImGui::OpenPopup("Layer");
    if (ImGui::BeginMenu("Layer")) {
      const OpenDocument* od = st.documents.active();
      if (od == nullptr) {
        ImGui::TextDisabled("(no document open)");
      } else {
        const Document& doc = od->document;
        const size_t selected = od->activeLayer;
        // The row every one of these acts on, named rather than assumed: the
        // menu bar is a long way from the panel and "which layer is this
        // about" is otherwise invisible from here.
        ImGui::TextDisabled("%s", selected < doc.layers.size()
                                      ? layerRowTitle(doc.layers[selected], selected).c_str()
                                      : "(no layer selected)");
        ImGui::Separator();
        for (const LayerCommand command : allLayerCommands()) {
          // The three toggles show the selected layer's current state as a
          // check mark, which is what makes "Toggle Visibility" honest about
          // which way it is about to go.
          bool checked = false;
          if (selected < doc.layers.size()) {
            if (command == LayerCommand::ToggleVisible) checked = doc.layers[selected].visible;
            if (command == LayerCommand::ToggleLocked) checked = doc.layers[selected].locked;
            if (command == LayerCommand::ToggleClipped) checked = doc.layers[selected].clipped;
          }
          if (ImGui::MenuItem(layerCommandLabel(command), nullptr, checked,
                              layerCommandAvailable(doc, command, selected)))
            runLayerCommand(st, command);
          // Grouped as the panel groups them: creation, then the whole-layer
          // operations, then the mask, then the flags.
          if (command == LayerCommand::NewAdjustmentLayer ||
              command == LayerCommand::MoveLayerDown || command == LayerCommand::RemoveMask ||
              command == LayerCommand::ToggleClipped)
            ImGui::Separator();
        }
        ImGui::Separator();
        // PLAN.md Phase 5 step 11 / PRD C12, C13, C15. The same construction as
        // the loop above, over `core::allLayerSetCommands()`: a set command
        // added to that enum without a menu entry is impossible, and the LAYERS
        // panel's "Multi-selection" section walks the identical list, so the two
        // views cannot come to offer different sets.
        if (ImGui::BeginMenu("Selection")) {
          const LayerSelection visible =
              restrictSelectionToFilter(doc, g_layers.selection, g_layers.filter);
          ImGui::TextDisabled("%zu layer(s) selected%s", g_layers.selection.size(),
                              visible.size() != g_layers.selection.size()
                                  ? ", some hidden by the filter"
                                  : "");
          ImGui::Separator();
          for (const LayerSetCommand command : allLayerSetCommands()) {
            if (ImGui::MenuItem(layerSetCommandLabel(command), nullptr, false,
                                layerSetCommandAvailable(doc, command, visible)))
              runLayerSetCommand(st, command);
            if (command == LayerSetCommand::MoveLayersDown ||
                command == LayerSetCommand::UnclipLayers ||
                command == LayerSetCommand::UnlinkLayers ||
                command == LayerSetCommand::LabelGrey ||
                command == LayerSetCommand::AlignSelectionBottom ||
                command == LayerSetCommand::AlignCanvasBottom)
              ImGui::Separator();
          }
          ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("refusals appear in the LAYERS panel");
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Medium")) {
      for (int i = 0; i < static_cast<int>(PaintMode::Count); ++i) {
        const PaintMode m = static_cast<PaintMode>(i);
        if (ImGui::MenuItem(paintModeName(m), nullptr, st.mode == m) && st.mode != m) {
          st.mode = m;
          st.requestMode = true;
        }
      }
      ImGui::Separator();
      ImGui::TextDisabled("switching clears the canvas");
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Goodies")) {
      for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
        const Tool t = static_cast<Tool>(i);
        if (ImGui::MenuItem(toolName(t), nullptr, st.brush.tool == t))
          st.brush.tool = t;
      }
      ImGui::Separator();
      ImGui::MenuItem("Pause solver", "Space", &st.paused);
      if (ImGui::MenuItem("Reload shaders", "Cmd+R")) st.requestReload = true;
      ImGui::EndMenu();
    }
    // PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4): menu-item mirror
    // of the same keymap actions main.cpp's SDL_EVENT_KEY_DOWN dispatch
    // resolves (keymaps/default.json) -- both paths end up setting the same
    // AppState fields, so there is exactly one place ("view" state consumed
    // in this file's canvas block below) that actually acts on any of them.
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Fit to Window", "Cmd+0")) st.requestFitWindow = true;
      if (ImGui::MenuItem("100%", "Cmd+1")) st.requestZoom100 = true;
      if (ImGui::MenuItem("Zoom In", "Cmd+=")) st.requestZoomIn = true;
      if (ImGui::MenuItem("Zoom Out", "Cmd+-")) st.requestZoomOut = true;
      ImGui::Separator();
      ImGui::MenuItem("Mirror Left/Right", "F", &st.view.mirrorX);
      ImGui::MenuItem("Mirror Up/Down", "Shift+F", &st.view.mirrorY);
      if (ImGui::MenuItem("Reset Rotation", "Shift+R")) st.view.rotation = 0.0f;
      ImGui::Separator();
      ImGui::MenuItem("Grayscale Preview", "Cmd+Y", &st.view.grayscale);
      ImGui::Separator();
      // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD
      // Q5-Q7). Rulers is deliberately the one item here with no shortcut
      // string -- docs/shortcuts.md assigns it Cmd+R, but that chord is
      // already bound to reload_shaders (see main.cpp's dispatch comment
      // for the full reasoning). Menu-only until that spec conflict is
      // resolved by a product decision, not by this step picking a
      // different key on its own initiative.
      ImGui::MenuItem("Rulers", nullptr, &st.showRulers);
      // docs/ui.md section 2's NAVIGATOR. No shortcut, for the same reason
      // Rulers has none: keymaps/default.json binds no action for it, and
      // advertising a chord that resolves to nothing is worse than none.
      ImGui::MenuItem("Navigator", nullptr, &st.showNavigator);
      ImGui::MenuItem("Guides", "Cmd+;", &st.showGuides);
      if (ImGui::MenuItem("Add Guide...")) ImGui::OpenPopup("AddGuidePopup");
      if (ImGui::MenuItem("Clear Guides", nullptr, false, !st.guides.empty()))
        st.guides.clear();
      ImGui::MenuItem("Grid", "Cmd+'", &st.showGrid);
      ImGui::MenuItem("Snap", "Cmd+Shift+;", &st.snappingEnabled);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("ImGui demo", nullptr, &st.showDemo);
      ImGui::Separator();
      // The open documents, and which one the lifecycle commands act on.
      // Phase 5 step 14's tab strip is this list drawn differently, not a
      // different list (app/DocumentLifecycle.hpp).
      if (st.documents.empty()) {
        ImGui::TextDisabled("(no documents open)");
      } else {
        for (size_t i = 0; i < st.documents.count(); ++i) {
          const OpenDocument* d = st.documents.at(i);
          const std::string label =
              documentDisplayName(*d) + (d->isDirty() ? " *" : "") + "##doc" + std::to_string(i);
          if (ImGui::MenuItem(label.c_str(), nullptr, i == st.documents.activeIndex()))
            st.documents.setActive(i);
          if (ImGui::IsItemHovered() && d->isDirty())
            ImGui::SetTooltip("%s", d->unsavedWorkSummary().c_str());
        }
      }
      ImGui::EndMenu();
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
    ImGui::BeginDisabled(titleHistory == nullptr || !titleHistory->canUndo());
    if (ImGui::SmallButton("Undo")) {
      settleWetPaintBeforeHistoryMove(st, sim, gpu, *activeForBar);
      installHistoryCursor(*activeForBar,
                           historyPanelClick(*titleHistory,
                                             historySerialForRow(*titleHistory,
                                                                 titleHistory->cursor() - 1)));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(titleHistory == nullptr || !titleHistory->canRedo());
    if (ImGui::SmallButton("Redo")) {
      settleWetPaintBeforeHistoryMove(st, sim, gpu, *activeForBar);
      installHistoryCursor(*activeForBar,
                           historyPanelClick(*titleHistory,
                                             historySerialForRow(*titleHistory,
                                                                 titleHistory->cursor() + 1)));
    }
    ImGui::EndDisabled();
    ImGui::SameLine(ImGui::GetWindowWidth() - statusW - 12.0f);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
  }

  // PLAN.md Phase 2 step 12, PRD Q5: "guide at a numeric or percentage
  // position" via a small popup opened from the View menu's "Add Guide..."
  // item above (ImGui::OpenPopup("AddGuidePopup")) -- doesn't need to be
  // elaborate, just a position field and an orientation choice. Defined
  // here, outside BeginMainMenuBar/EndMainMenuBar (popups are identified by
  // a global string ID, not nested inside the ID stack of whatever widget
  // opened them), so it renders regardless of which menu is currently open.
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
    const auto& pig = defaultPalette()[st.brush.pigment];
    dl->AddRectFilled(p, ImVec2(p.x + swatchSide, p.y + swatchSide),
                      IM_COL32((int)(pig.rgb[0] * 255), (int)(pig.rgb[1] * 255),
                               (int)(pig.rgb[2] * 255), 255));
    dl->AddRect(p, ImVec2(p.x + swatchSide, p.y + swatchSide), atelierToken(kRule), 0.0f, 0,
                kRuleThickness);
    ImGui::Dummy(ImVec2(swatchSide, swatchSide));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Foreground: %s\nChosen in the COLOR panel (docs/ui.md section 3.3)",
                        pig.name);
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
  if (ImGui::Begin("##controls", nullptr,
                   fixedFlags & ~ImGuiWindowFlags_NoScrollbar)) {
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
        case ControlsSection::Brush:     drawBrushSection(st); break;
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
      st.view.zoom = std::clamp(std::min(avail.x / texW, avail.y / texH), 0.1f, 8.0f);
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

    // --- zoom, anchored the same way whether the wheel or a keyboard
    // command triggered it (PRD Q1) -- one formula, reused, rather than the
    // keyboard commands inventing a second anchoring rule.
    auto applyZoomFactor = [&](float factor) {
      const float oldZoom = st.view.zoom;
      st.view.zoom = std::clamp(st.view.zoom * factor, 0.1f, 8.0f);
      const float k = st.view.zoom / oldZoom;
      st.view.panX = (st.view.panX + (origin.x - paintOrigin.x)) * k - (origin.x - paintOrigin.x);
      st.view.panY = (st.view.panY + (origin.y - paintOrigin.y)) * k - (origin.y - paintOrigin.y);
    };
    // --- zoom on wheel, anchored under the cursor ---
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
      applyZoomFactor(1.0f + ImGui::GetIO().MouseWheel * 0.12f);
    if (st.requestZoomIn) { applyZoomFactor(1.2f); st.requestZoomIn = false; }
    if (st.requestZoomOut) { applyZoomFactor(1.0f / 1.2f); st.requestZoomOut = false; }

    const bool panning =
        !rotateHeld && !st.pendingGuide.has_value() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         (st.brush.tool == Tool::Hand && ImGui::IsMouseDragging(ImGuiMouseButton_Left)));
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

      if (st.requestSelectAll && od != nullptr)
        installSelection(*od, selectAll(od->document.width, od->document.height));
      if (st.requestDeselect && od != nullptr) installSelection(*od, std::nullopt);

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
      st.requestCopy = false;
      st.requestCopyMerged = false;
      st.requestCut = false;
      st.requestPaste = false;
      st.requestDeleteSelection = false;

      // --- keep the cached bounds and the GPU coverage in step -------------
      //
      // Both are expensive enough to want an "has it changed?" test rather
      // than a per-frame rebuild: selectionBounds() walks every selected
      // texel, and setSelection() uploads a canvas-sized texture. The
      // revision is that test.
      //
      // The sim upload is inside the same branch deliberately. It must ALSO
      // happen when a sim is constructed mid-session, which is why the
      // condition includes a sim that has not been told yet.
      if (od != nullptr) {
        const bool stale = od->selectionRevision != st.cachedSelectionRevision ||
                           od->id != st.cachedSelectionDoc;
        const bool simUninformed =
            sim && od->selection.has_value() != sim->hasSelection();
        if (stale || simUninformed) {
          st.selectionBoundsCache = od->selection.has_value()
                                        ? selectionBounds(*od->selection)
                                        : std::nullopt;
          if (sim) {
            sim->setSelection(gpu, od->selection.has_value() ? &*od->selection : nullptr);
          }
          st.cachedSelectionRevision = od->selectionRevision;
          st.cachedSelectionDoc = od->id;
        }
      }
    }

    // --- marquee (PRD E3's rectangle) --------------------------------------
    //
    // Rubber-banded in document texel space, committed on release. The
    // selection is only built once, at mouse-up, rather than every frame of
    // the drag: selectRectangle() allocates tiles, and rebuilding it 60 times
    // a second to draw a rectangle the drawing code can draw from two corners
    // anyway would be work for nothing.
    if (st.brush.tool == Tool::Marquee && !panning && !rotating &&
        !st.pendingGuide.has_value()) {
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.marqueeDragging = true;
        st.marqueeX0 = tx;
        st.marqueeY0 = ty;
        st.marqueeX1 = tx;
        st.marqueeY1 = ty;
      }
      if (st.marqueeDragging) {
        st.marqueeX1 = tx;
        st.marqueeY1 = ty;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
          st.marqueeDragging = false;
          OpenDocument* od = st.documents.active();
          if (od != nullptr) {
            // Clamped to the canvas: a drag that leaves the window would
            // otherwise select texels the document does not have, and every
            // consumer would then walk tiles that hold nothing.
            const float x0 = std::clamp(std::min(st.marqueeX0, st.marqueeX1), 0.0f, texW);
            const float y0 = std::clamp(std::min(st.marqueeY0, st.marqueeY1), 0.0f, texH);
            const float x1 = std::clamp(std::max(st.marqueeX0, st.marqueeX1), 0.0f, texW);
            const float y1 = std::clamp(std::max(st.marqueeY0, st.marqueeY1), 0.0f, texH);
            // A click with no drag DESELECTS, which is what a marquee does in
            // every editor. Committing the degenerate rectangle instead would
            // leave an engaged selection covering nothing -- technically
            // correct (core/SelectionMask distinguishes the two) and, as a
            // response to a stray click, indistinguishable from the
            // application having broken.
            if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) {
              installSelection(*od, std::nullopt);
            } else {
              installSelection(*od, selectRectangle(x0, y0, x1, y1));
            }
          }
        }
      }
    } else {
      st.marqueeDragging = false;
    }

    // --- stroke ---
    const bool paintTool = st.brush.tool == Tool::Brush ||
                           st.brush.tool == Tool::Water ||
                           st.brush.tool == Tool::DryBrush;
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
    // a Pigment layer must not construct the solver, or painting on the one
    // layer kind that needs no simulation would allocate every field a
    // watercolour session does.
    OpenDocument* strokeDoc = st.documents.active();
    const Layer* strokeTarget = strokeDoc != nullptr ? activeLayerOf(*strokeDoc) : nullptr;
    const StrokeRoute route = strokeRouteFor(st.brush.tool, strokeTarget);

    if (paintTool && down && hovered && inside && !panning && !rotating &&
        !st.pendingGuide.has_value() && route == StrokeRoute::CpuDeposit) {
      // **The pen reaches a Layer.** app/StrokeSession section 4 said this was
      // "a missing decision rather than missing plumbing", and the decision it
      // was missing was `OpenDocument::activeLayer`.
      //
      // Shaped exactly as that header predicted: `begin()` where
      // `strokePath.reset()` is, `addPoint()` where `strokePath.addPoint()`
      // is, `end()` where `strokePath.flush()` is. No solver, no
      // `st.pendingDabs`, no `st.sim.brush*` -- those belong to the other
      // route and setting them here would leave the oil segment carrying a
      // stroke that never touched the canvas texture.
      st.paintingThisFrame = true;
      const float pressure = st.penSeen ? st.penPressure : 1.0f;
      const BrushTip tip = brushTipFor(st.brush, lut, pressure);
      if (!g_stroke.active()) {
        g_strokeRefusal.clear();
        if (!g_stroke.begin(*strokeDoc, strokeDoc->activeLayer, tip, st.brush.tool,
                            &g_strokeRefusal)) {
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
    } else if (paintTool && down && hovered && inside && !panning && !rotating &&
               !st.pendingGuide.has_value() && route == StrokeRoute::None &&
               strokeTarget != nullptr) {
      // The one refusal worth saying out loud: a locked Pigment layer. The
      // route table sends it nowhere rather than falling through to the
      // solver, because falling through would put paint on the *canvas* when
      // the user aimed at a layer -- and the user has to be told, or the brush
      // just silently stops working.
      g_strokeRefusal = std::string("locked layer: \"") + strokeTarget->name +
                        "\" cannot be painted. Clear its Lock in LAYERS.";
    } else if (paintTool && down && hovered && inside && !panning && !rotating &&
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
        const float pressure = st.penSeen ? st.penPressure : 1.0f;
        const float sizeMul = st.brush.pressureSize ? (0.25f + 0.75f * pressure) : 1.0f;
        const float flowMul = st.brush.pressureFlow ? (0.15f + 0.85f * pressure) : 1.0f;

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
        g_stroke.end();
        std::printf("[stroke] %s: %zu dabs, %zu texels, %zu tiles\n", g_stroke.label().c_str(),
                    g_stroke.dabCount(), g_stroke.texelsWritten(), g_stroke.strokeTiles().size());
      }
    }

    // --- grid + guides overlay (PRD Q7, Q5) -- drawn once, after every
    // input-driven update above has settled for this frame (guide commit,
    // pan/zoom/rotate), so a guide committed this same frame draws
    // immediately rather than lagging a frame behind. ---
    if (st.showGrid) drawGridOverlay(dl, xform, texW, texH, st.gridSpacing, st.gridSubdivisions,
                                     st.view.zoom);

    // --- the selection, and the marquee being dragged (PRD E6) ------------
    //
    // After the grid so it sits on top: the selection is the thing the user is
    // currently acting on, and a grid line crossing the ants would read as
    // part of the boundary.
    if (st.marqueeDragging) {
      // The live rubber band, drawn from the drag corners rather than from a
      // Selection -- there is no Selection yet, and building one per frame to
      // draw a rectangle would allocate tiles 60 times a second.
      drawMarchingAnts(dl, xform, std::min(st.marqueeX0, st.marqueeX1),
                       std::min(st.marqueeY0, st.marqueeY1),
                       std::max(st.marqueeX0, st.marqueeX1),
                       std::max(st.marqueeY0, st.marqueeY1));
    } else if (st.selectionBoundsCache.has_value()) {
      const SelectionBounds& b = *st.selectionBoundsCache;
      drawMarchingAnts(dl, xform, static_cast<float>(b.x0), static_cast<float>(b.y0),
                       static_cast<float>(b.x1), static_cast<float>(b.y1));
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
    if (hovered && paintTool && inside) {
      dl->AddCircle(mouse, st.brush.radius * st.view.zoom,
                    IM_COL32(235, 235, 225, 170), 32, 1.0f);
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
    // accent rule along its leading edge. On the foreground list so neither
    // pane's window can overdraw it -- the same reason drawAtelierRules() is
    // there.
    const AtelierRect f = panes.pane[paneDocs.focusedPane];
    ImGui::GetForegroundDrawList()->AddRectFilled(
        ImVec2(f.x, f.y), ImVec2(f.right(), f.y + kRuleThickness), atelierToken(kAccent));
    // The rule between the panes, in the same grey every other region rule
    // uses.
    ImGui::GetForegroundDrawList()->AddRectFilled(
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

// Exactly the assignment the icon's click handler makes (see
// `drawAtelierTabStrip()`'s split-icon loop), and deliberately not one line
// more -- the declaration says why the companion and the focused pane are not
// written here.
void setSplitArrangement(AtelierSplit mode) { g_split.mode = mode; }

}  // namespace np
