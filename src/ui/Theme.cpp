#include "ui/Theme.hpp"

#include "imgui.h"

namespace np {

void applyMacPaintDarkTheme() {
  ImGuiStyle& s = ImGui::GetStyle();

  // Nothing is rounded. A 1984 bitmap UI had no curves and neither does this.
  s.WindowRounding = 0.0f;
  s.ChildRounding = 0.0f;
  s.FrameRounding = 0.0f;
  s.PopupRounding = 0.0f;
  s.ScrollbarRounding = 0.0f;
  s.GrabRounding = 0.0f;
  s.TabRounding = 0.0f;

  s.WindowBorderSize = 1.0f;
  s.ChildBorderSize = 1.0f;
  s.FrameBorderSize = 1.0f;
  s.PopupBorderSize = 1.0f;
  s.TabBorderSize = 1.0f;

  s.WindowPadding = ImVec2(6, 6);
  s.FramePadding = ImVec2(6, 4);
  s.ItemSpacing = ImVec2(4, 4);
  s.ItemInnerSpacing = ImVec2(4, 4);
  s.ScrollbarSize = 14.0f;
  s.GrabMinSize = 12.0f;

  ImVec4* c = s.Colors;

  const ImVec4 ink       = ImVec4(0.070f, 0.070f, 0.075f, 1.00f);  // deepest
  const ImVec4 panel     = ImVec4(0.125f, 0.127f, 0.133f, 1.00f);
  const ImVec4 raised    = ImVec4(0.180f, 0.183f, 0.190f, 1.00f);
  const ImVec4 edge      = ImVec4(0.320f, 0.325f, 0.340f, 1.00f);
  const ImVec4 paperWhite= ImVec4(0.900f, 0.898f, 0.880f, 1.00f);
  const ImVec4 dim       = ImVec4(0.520f, 0.520f, 0.510f, 1.00f);
  // The one non-grey: a wet-ink cyan for selection, standing in for MacPaint's
  // inverted-black highlight without turning the whole UI blue.
  const ImVec4 select    = ImVec4(0.310f, 0.640f, 0.720f, 1.00f);

  c[ImGuiCol_Text]                 = paperWhite;
  c[ImGuiCol_TextDisabled]         = dim;
  c[ImGuiCol_WindowBg]             = panel;
  c[ImGuiCol_ChildBg]              = ink;
  c[ImGuiCol_PopupBg]              = panel;
  c[ImGuiCol_Border]               = edge;
  c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg]              = ink;
  c[ImGuiCol_FrameBgHovered]       = raised;
  c[ImGuiCol_FrameBgActive]        = edge;
  c[ImGuiCol_TitleBg]              = ink;
  c[ImGuiCol_TitleBgActive]        = raised;
  c[ImGuiCol_TitleBgCollapsed]     = ink;
  c[ImGuiCol_MenuBarBg]            = raised;
  c[ImGuiCol_ScrollbarBg]          = ink;
  c[ImGuiCol_ScrollbarGrab]        = edge;
  c[ImGuiCol_ScrollbarGrabHovered] = dim;
  c[ImGuiCol_ScrollbarGrabActive]  = paperWhite;
  c[ImGuiCol_CheckMark]            = select;
  c[ImGuiCol_SliderGrab]           = dim;
  c[ImGuiCol_SliderGrabActive]     = paperWhite;
  c[ImGuiCol_Button]               = raised;
  c[ImGuiCol_ButtonHovered]        = edge;
  c[ImGuiCol_ButtonActive]         = select;
  c[ImGuiCol_Header]               = raised;
  c[ImGuiCol_HeaderHovered]        = edge;
  c[ImGuiCol_HeaderActive]         = select;
  c[ImGuiCol_Separator]            = edge;
  c[ImGuiCol_SeparatorHovered]     = dim;
  c[ImGuiCol_SeparatorActive]      = select;
  c[ImGuiCol_ResizeGrip]           = raised;
  c[ImGuiCol_ResizeGripHovered]    = edge;
  c[ImGuiCol_ResizeGripActive]     = select;
  c[ImGuiCol_Tab]                  = ink;
  c[ImGuiCol_TabHovered]           = edge;
  c[ImGuiCol_TabSelected]          = raised;
  c[ImGuiCol_PlotLines]            = dim;
  c[ImGuiCol_PlotLinesHovered]     = select;
  c[ImGuiCol_PlotHistogram]        = dim;
  c[ImGuiCol_PlotHistogramHovered] = select;
  c[ImGuiCol_TextSelectedBg]       = select;
  c[ImGuiCol_DragDropTarget]       = select;
  c[ImGuiCol_NavCursor]            = select;
  c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.55f);
}

}  // namespace np
