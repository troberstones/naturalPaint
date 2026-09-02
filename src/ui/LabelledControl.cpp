#include "ui/LabelledControl.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>

#include "app/ControlsLayout.hpp"

namespace np {

namespace {

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

}  // namespace

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

bool ctlSlider(const char* label, float* v, float lo, float hi, const char* fmt) {
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

// The label column, measured and reported (UI detour step 3, problem 1b).
// Printed when it changes rather than every frame: it settles on the first
// frame that has drawn every open panel once, and moves again only if a
// panel that opens later carries a wider label. A log line is what makes
// "no label is clipped" a number rather than a look at a screenshot.
void reportLabelColumnIfChanged(float panelWidthPx, float availWidthPx) {
  if (g_labelColumn != g_reportedColumn) {
    g_reportedColumn = g_labelColumn;
    std::printf("[controls] label column %.0f px -- widest label \"%s\" at %.0f px, "
                "right dock %.0f px, so a slider gets %.0f px\n",
                g_labelColumn, g_widestLabel.c_str(), g_widestLabelPx, panelWidthPx,
                std::max(0.0f, availWidthPx - g_labelColumn));
  }
}

}  // namespace np
