#include "ui/AtelierTheme.hpp"

#include "imgui.h"

namespace np {
namespace {

// PRD L6: session state, not a constant and not a document property.
uint32_t g_surround = kCanvasSurroundDefault;

ImVec4 col(uint32_t rgb, float a = 1.0f) {
  float c[3];
  unpackRgb(rgb, c);
  return ImVec4(c[0], c[1], c[2], a);
}

// A token lightened or darkened toward white/black by `t`, for the two states
// ImGui needs that the design does not name: hovered and active fills. The
// design gives one value per surface and expects hover to be legible, so
// rather than inventing a thirteenth and a fourteenth token this derives them
// from the twelve -- a hover is the same colour with more light in it.
ImVec4 lift(uint32_t rgb, float t) {
  float c[3];
  unpackRgb(rgb, c);
  for (float& v : c) v = v + (1.0f - v) * t;
  return ImVec4(c[0], c[1], c[2], 1.0f);
}

}  // namespace

uint32_t atelierSurround() noexcept { return g_surround; }
void setAtelierSurround(uint32_t rgb) noexcept { g_surround = rgb & 0xffffffu; }

void unpackRgb(uint32_t rgb, float out[3]) noexcept {
  out[0] = static_cast<float>((rgb >> 16) & 0xffu) / 255.0f;
  out[1] = static_cast<float>((rgb >> 8) & 0xffu) / 255.0f;
  out[2] = static_cast<float>(rgb & 0xffu) / 255.0f;
}

void applyAtelierTheme() {
  ImGuiStyle& s = ImGui::GetStyle();

  // "Brutalist / modernist: flat fills, hard rules, no radius, no gradients
  // outside the colour picker" -- and docs/ui.md section 1's own note that
  // this is the aesthetic Dear ImGui renders *well*, so every radius is zero
  // because the design asks for zero, not as a stylistic shrug.
  s.WindowRounding = s.ChildRounding = s.FrameRounding = s.PopupRounding = 0.0f;
  s.ScrollbarRounding = s.GrabRounding = s.TabRounding = 0.0f;

  // Borders are drawn by ui/AtelierChrome as explicit 2px rules between the
  // bands, so ImGui's own 1px window border would double them. Frames keep a
  // hairline: a slider with no edge against a flat fill has no affordance.
  s.WindowBorderSize = 0.0f;
  s.ChildBorderSize = kDividerThickness;
  s.FrameBorderSize = kDividerThickness;
  s.PopupBorderSize = kDividerThickness;
  s.TabBorderSize = 0.0f;

  s.WindowPadding = ImVec2(8, 8);
  s.FramePadding = ImVec2(8, 5);
  s.ItemSpacing = ImVec2(6, 6);
  s.ItemInnerSpacing = ImVec2(6, 4);
  s.ScrollbarSize = 12.0f;
  s.GrabMinSize = 10.0f;

  ImVec4* c = s.Colors;

  c[ImGuiCol_Text]                 = col(kTextPrimary);
  c[ImGuiCol_TextDisabled]         = col(kTextSecondary);
  c[ImGuiCol_WindowBg]             = col(kChromeBase);
  c[ImGuiCol_ChildBg]              = col(kChromeDeep);
  c[ImGuiCol_PopupBg]              = col(kChromeDeep);
  c[ImGuiCol_Border]               = col(kDivider);
  c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);

  // Inputs sit in the deep value -- docs/ui.md section 1 lists "tool options"
  // among chrome deep, and every text field and slider track in the design is
  // a well cut into the panel rather than a raised control.
  c[ImGuiCol_FrameBg]              = col(kChromeDeep);
  c[ImGuiCol_FrameBgHovered]       = lift(kChromeDeep, 0.10f);
  c[ImGuiCol_FrameBgActive]        = lift(kChromeDeep, 0.18f);

  c[ImGuiCol_TitleBg]              = col(kChromeDeep);
  c[ImGuiCol_TitleBgActive]        = col(kChromeDeep);
  c[ImGuiCol_TitleBgCollapsed]     = col(kChromeDeep);
  c[ImGuiCol_MenuBarBg]            = col(kChromeBase);

  c[ImGuiCol_ScrollbarBg]          = col(kChromeDeep);
  c[ImGuiCol_ScrollbarGrab]        = col(kChromeMid);
  c[ImGuiCol_ScrollbarGrabHovered] = lift(kChromeMid, 0.15f);
  c[ImGuiCol_ScrollbarGrabActive]  = col(kAccent);

  // The accent is "active tool, dirty marker, selection" -- so it marks state,
  // and a hover is not state. Hover lifts the surface; the accent arrives only
  // when something is actually on.
  c[ImGuiCol_CheckMark]            = col(kAccent);
  c[ImGuiCol_SliderGrab]           = col(kAccent);
  c[ImGuiCol_SliderGrabActive]     = col(kAccent);

  c[ImGuiCol_Button]               = col(kChromeMid);
  c[ImGuiCol_ButtonHovered]        = lift(kChromeMid, 0.15f);
  c[ImGuiCol_ButtonActive]         = col(kAccent);

  // Collapsing headers are the right-hand column's section titles. Chrome mid
  // reads as a band across the panel, which is what the design draws.
  c[ImGuiCol_Header]               = col(kChromeMid);
  c[ImGuiCol_HeaderHovered]        = lift(kChromeMid, 0.15f);
  c[ImGuiCol_HeaderActive]         = lift(kChromeMid, 0.22f);

  c[ImGuiCol_Separator]            = col(kDivider);
  c[ImGuiCol_SeparatorHovered]     = col(kHairline);
  c[ImGuiCol_SeparatorActive]      = col(kAccent);
  c[ImGuiCol_ResizeGrip]           = col(kChromeMid);
  c[ImGuiCol_ResizeGripHovered]    = lift(kChromeMid, 0.15f);
  c[ImGuiCol_ResizeGripActive]     = col(kAccent);

  // Document tabs (docs/ui.md section 2's 34px strip): inactive tabs are the
  // strip's own chrome mid, the active one is chrome deep -- the design lists
  // "active tab" under chrome deep, so the selected tab is the one cut *into*
  // the strip rather than raised out of it.
  c[ImGuiCol_Tab]                  = col(kChromeMid);
  c[ImGuiCol_TabHovered]           = lift(kChromeMid, 0.15f);
  c[ImGuiCol_TabSelected]          = col(kChromeDeep);

  c[ImGuiCol_PlotLines]            = col(kTextSecondary);
  c[ImGuiCol_PlotLinesHovered]     = col(kAccent);
  c[ImGuiCol_PlotHistogram]        = col(kTextSecondary);
  c[ImGuiCol_PlotHistogramHovered] = col(kAccent);

  // "row selected" is its own token, darker and more saturated than the
  // accent, because a selected layer row is a *field* the accent-coloured
  // text has to stay readable against.
  c[ImGuiCol_TextSelectedBg]       = col(kRowSelected);
  c[ImGuiCol_DragDropTarget]       = col(kAccent);
  c[ImGuiCol_NavCursor]            = col(kAccent);
  c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

}  // namespace np
