#pragma once

#include "app/ControlsLayout.hpp"  // ControlsSectionSpec
#include "ui/AtelierLayout.hpp"    // AtelierRect
#include "ui/DockLayout.hpp"       // kPanelHeaderExtent, kPanelMinBody, kPanelMinWidth

// ui/PanelGrip -- the grip-fit arithmetic `ui/MacPaintUI.cpp`'s `drawPanelGrip()`
// draws from, pulled into a header so `--selftest` can call the REAL function
// instead of re-deriving its formula.
//
// This header carries only the declaration; `panelGripFor()` is still defined
// in ui/MacPaintUI.cpp, next to `drawPanelGrip()`, because it measures text
// through Dear ImGui (`ImGui::CalcTextSize()`, `pushAtelierMono()`) and every
// other ImGui-touching function in this codebase stays in that file. A
// declaration here is enough for both callers -- `drawPanelGrip()` and
// app/selftest/PanelSettings.cpp's Part C -- to reach the one definition,
// which is the whole point: before this header existed, PanelSettings.cpp
// could not call `panelGripFor()` at all (it was file-local to
// ui/MacPaintUI.cpp) and re-typed its formula instead, so a change to the
// real function's arithmetic -- e.g. `settingsReserve` silently dropped to
// zero -- left the re-derivation, and therefore `--selftest`, unable to see
// it.
namespace np {

// The "?" help button's diameter, and the gap it keeps from the grip's right
// edge -- one shared constant so `panelGripFor()`'s title-fit check and
// `drawPanelGrip()`'s own drawing can never disagree about how much width the
// button costs.
constexpr float kPanelHelpBtnSize = 16.0f;
constexpr float kPanelHelpBtnMargin = 6.0f;

// The rail's width, when the slot is too short to spare a full bar -- see
// `panelGripFor()`'s own comment for when that happens.
constexpr float kPanelRailW = 14.0f;

enum class PanelGripKind { Bar, Rail };

struct PanelGripLayout {
  PanelGripKind kind = PanelGripKind::Bar;
  // Bar height, or rail width.
  float extent = kPanelHeaderExtent;
  bool showTitle = true;
};

// Decides the grip's shape (bar vs. rail) and, for a bar, whether the title
// fits beside the disclosure triangle and this section's "?"/gear buttons.
// `spec` supplies the title text and the two flags (`helpText != nullptr`,
// `hasSettings`) that reserve those buttons' width -- taking the spec
// directly, rather than a `ControlsSection` this function looks up itself,
// is what lets a caller (namely the selftest) construct a spec no real
// section happens to have, such as "settings but no help text", and still
// exercise this exact code path.
PanelGripLayout panelGripFor(const ControlsSectionSpec& spec, const AtelierRect& slot,
                              bool collapsed);

}  // namespace np
