#include "ui/BrushSettingsWindow.hpp"

#include <array>
#include <cfloat>

#include "app/AppState.hpp"
#include "imgui.h"

namespace np {
namespace {

// The table. One row per enumerator, in tab-strip order, indexed by the
// enumerator -- `brushSettingsTabSpec()` indexes straight into it and
// `--selftest` asserts that every row carries its own id, so a row inserted
// in the wrong place fails the suite instead of drawing the wrong controls
// under the right name.
constexpr std::array<BrushSettingsTabSpec, kBrushSettingsTabCount> kTabs = {{
    {BrushSettingsTab::TipShape, "Tip",
     "The shape of one dab: its size, its edge, how far apart dabs are laid,\n"
     "and which bitmap -- if any -- is being stamped."},
    {BrushSettingsTab::Paint, "Paint",
     "What the brush carries and how much of it lands: load, water, and the\n"
     "ceiling one stroke may build to."},
    {BrushSettingsTab::Texture, "Texture",
     "Paper tooth under the stroke. Deep valleys fill and peaks get skipped,\n"
     "at the same pressure."},
    {BrushSettingsTab::Dynamics, "Dynamics",
     "What moves while you paint: pressure, speed, tilt and direction wired\n"
     "to the numbers on the other tabs."},
}};

}  // namespace

const BrushSettingsTabSpec& brushSettingsTabSpec(BrushSettingsTab tab) noexcept {
  const size_t i = static_cast<size_t>(tab);
  // A `Count` (or worse) handed in is a caller bug, and returning row 0 is the
  // least surprising thing to do about it in a draw path -- the alternative is
  // an out-of-bounds read in a function whose whole job is to be safe to call
  // every frame.
  return kTabs[i < kTabs.size() ? i : 0];
}

const char* brushSettingsTabName(BrushSettingsTab tab) noexcept {
  switch (tab) {
    case BrushSettingsTab::TipShape: return "TipShape";
    case BrushSettingsTab::Paint:    return "Paint";
    case BrushSettingsTab::Texture:  return "Texture";
    case BrushSettingsTab::Dynamics: return "Dynamics";
    case BrushSettingsTab::Count:    break;
  }
  return "UNNAMED";
}

void drawBrushSettingsWindow(AppState& st, GpuContext& gpu, const MixboxLut& lut) {
  if (!st.showBrushSettings) return;

  // Wide enough for the dab grid to fit more than two columns at the picker's
  // preferred cell size, and tall enough that the Tip tab -- the longest --
  // does not open already scrolling. `FirstUseEver` rather than `Always`: once
  // a painter has sized and placed it, imgui.ini is the authority, and a
  // window that snaps back to its default size on every launch is a window
  // nobody bothers to move.
  ImGui::SetNextWindowSize(ImVec2(380.0f, 580.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(300.0f, 220.0f), ImVec2(FLT_MAX, FLT_MAX));

  // `&st.showBrushSettings` gives the title bar its close button AND makes
  // that button write the same flag the menu tick reads, so the two cannot
  // disagree about whether the window is open. Closing it from either place is
  // the same state change.
  if (!ImGui::Begin("Brush Settings", &st.showBrushSettings)) {
    // Collapsed. `Begin()` returning false still requires the matching End().
    ImGui::End();
    return;
  }

  // Above the tabs, not inside one -- see the header on why Save cannot live
  // on a tab.
  drawBrushPresetHeader(st);

  if (ImGui::BeginTabBar("brush-settings-tabs", ImGuiTabBarFlags_None)) {
    for (const BrushSettingsTabSpec& spec : kTabs) {
      // `--brush-settings-demo <tab>`, consumed rather than held: see
      // `AppState::brushSettingsDemoTab`. Only the named tab gets the flag,
      // and only until it has been drawn once.
      ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
      if (st.brushSettingsDemoTab == static_cast<int>(spec.tab))
        flags |= ImGuiTabItemFlags_SetSelected;
      if (ImGui::BeginTabItem(spec.label, nullptr, flags)) {
        // The tooltip goes on the tab itself rather than on the page, so it is
        // readable while choosing a tab -- which is the moment the question
        // "what is on this one?" is actually being asked.
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
          ImGui::SetTooltip("%s", spec.tooltip);

        // A child region so each page scrolls on its own. Without it the whole
        // window scrolls and the tab strip scrolls off the top, which makes
        // switching tabs on a long page require scrolling back up first.
        if (ImGui::BeginChild("page", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None)) {
          switch (spec.tab) {
            case BrushSettingsTab::TipShape: drawBrushTipShapeGroup(st, gpu, lut); break;
            case BrushSettingsTab::Paint:    drawBrushPaintGroup(st);              break;
            case BrushSettingsTab::Texture:
              drawBrushTextureGroup(st, /*ownPage=*/true);
              break;
            case BrushSettingsTab::Dynamics: drawBrushDynamicsGroup(st);           break;
            case BrushSettingsTab::Count:    break;
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        // Hovering a tab that is not the current one asks the same question,
        // and `BeginTabItem()` returning false is the only place it can be
        // answered for that tab.
        ImGui::SetTooltip("%s", spec.tooltip);
      }
    }
    ImGui::EndTabBar();
    // Every tab has now had its chance to take the flag; drop it so the strip
    // is clickable from the next frame on.
    st.brushSettingsDemoTab = -1;
  }

  ImGui::End();
}

}  // namespace np
