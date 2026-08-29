#include "ui/AtelierTheme.hpp"

#include "imgui.h"

// "Is a modal popup open right now?" has no answer in the public Dear ImGui
// header. `GetTopMostPopupModal()` is the one that does, and it lives in
// imgui_internal.h -- which ui/MacPaintUI.cpp, the only other file that would
// want this, deliberately does not include (see `drawPadlockGlyph()` there,
// which hand-writes pi rather than pull that header in for `IM_PI`).
//
// Declaring the one entry point instead of including the header keeps that
// decision intact and costs a forward declaration: the result is only ever
// compared against null, so an incomplete `ImGuiWindow` is enough. Copied
// verbatim from imgui_internal.h line 3751 -- if it ever stops matching, this
// stops linking, which is the failure mode we want.
struct ImGuiWindow;
namespace ImGui {
IMGUI_API ImGuiWindow* GetTopMostPopupModal();
}  // namespace ImGui

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

  // kWindowPaddingX/kScrollbarSize (this file's own header) are the same two
  // numbers a window's content-width arithmetic reads elsewhere
  // (ui/AtelierLayout.hpp's kToolPaletteW) -- not a second, coincidentally-
  // equal pair of literals. See that header's comment for the defect that
  // duplication produced once.
  s.WindowPadding = ImVec2(kWindowPaddingX, kWindowPaddingY);
  s.FramePadding = ImVec2(8, 5);
  s.ItemSpacing = ImVec2(6, 6);
  s.ItemInnerSpacing = ImVec2(6, 4);
  s.ScrollbarSize = kScrollbarSize;
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
  // **Zero on purpose, and it is not "no dim" -- the dim moved.** ImGui draws
  // this token as one rect over the whole viewport, which is all or nothing,
  // and what was wanted is a wash over the chrome with the image left alone.
  // `washCurrentWindowForModal()` and ui/MacPaintUI.cpp's chrome scrim do that
  // instead; this header's own block above them carries the design. Setting a
  // non-zero value back here does not restore anything, it double-greys the
  // chrome and puts the wash back over the image.
  //
  // The instruction, in two parts: *"dont' gray out the UI when any modal
  // panels are opened"*, then *"what if the image stayed un-grayed, but the
  // toolbox and control panels were grayed out"*.
  //
  // The token was 55 % black, and one dialog -- Layer Properties -- already
  // pushed it to transparent around its own `BeginPopupModal()`, with a long
  // argument for why that suppression was scoped to one popup rather than made
  // the theme's rule: the five *decision gates* (Revert, Recover Documents, the
  // document-path prompt, and both Export dialogs) were held to want the dim,
  // because greying what is behind them is the sentence "nothing else is live
  // until you answer". That reasoning is **not** overruled -- it is what the
  // second half of the instruction preserves. Those gates still grey everything
  // a click could otherwise reach; the one surface exempted is the image, which
  // was never clickable while a modal was up anyway.
  //
  // What that buys is what the one-dialog exception was reaching for, now for
  // all of them: an adjustment dialog is an *editor whose output is the
  // canvas*, and a wash over the canvas defeats the only feedback its controls
  // have. Image > Adjustments added thirteen such dialogs, each with a live
  // preview, which is what turned a one-dialog exception into the rule.
  c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
}

bool modalDimActive() { return ImGui::GetTopMostPopupModal() != nullptr; }

void washRectForModal(float x0, float y0, float x1, float y1) {
  if (x1 <= x0 || y1 <= y0) return;
  // Lighter than the 0.55 black the full-viewport token above used to carry.
  // That value was picked to read over the *canvas*, which can be white; the
  // chrome is already near-black, and 55 % more black on top of it is off
  // rather than greyed.
  constexpr ImU32 kWash = IM_COL32(0, 0, 0, 115);
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), kWash);
}

void washCurrentWindowForModal() {
  if (!modalDimActive()) return;
  const ImVec2 p = ImGui::GetWindowPos();
  const ImVec2 s = ImGui::GetWindowSize();
  washRectForModal(p.x, p.y, p.x + s.x, p.y + s.y);
}

}  // namespace np
