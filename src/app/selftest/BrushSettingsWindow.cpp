#include "app/selftest/Support.hpp"

#include <set>
#include <string>

#include "ui/BrushSettingsWindow.hpp"

namespace np {

// ---------------------------------------------------------------------------
// ui/BrushSettingsWindow's tab table -- the half of a tabbed window that can
// be wrong without anyone noticing.
//
// `--selftest` cannot reach an ImGui dispatch site (reachability-audit F4), so
// a tab strip written as a run of `BeginTabItem()` calls carries no assertions
// at all: a group dropped in a later edit is invisible until a painter goes
// looking for a control that is no longer anywhere in the application. The
// table is data for exactly that reason, and this is what reads it.
//
// The load-bearing assertion is section B. The table is **indexed by the
// enum**, and a row whose `tab` disagrees with its own index draws the
// Dynamics controls under the Texture tab -- ui/MenuModel's spec table has the
// identical hazard and pins it the identical way, because it is completely
// invisible on inspection.
// ---------------------------------------------------------------------------
bool runBrushSettingsWindowTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ======================================================================
  std::printf("  -- A. every group of brush settings has a tab --\n");
  // ======================================================================
  //
  // **Nine is Photoshop's own eight brush-settings panels -- Tip Shape,
  // Shape Dynamics, Scattering, Texture, Dual Brush, Color Dynamics,
  // Transfer, Tool Options -- plus the shelved link-matrix editor kept as a
  // tab of its own** (`BrushSettingsTab`'s own header comment on why
  // Dynamics stayed rather than moving fully behind `--advanced-dynamics`).
  // "Paint" -- naturalPaint's own stand-in for load/water/opacity, never a
  // Photoshop panel -- is no longer one of this window's tabs; the docked
  // BRUSH column still shows it via `drawBrushPaintGroup()`, unchanged
  // (`ui/BrushSettingsWindow.hpp` §2's asymmetry).
  //
  // This literal is exactly as brittle to the next group added as
  // ui/MenuModel's action count is, and that is the point: a `BrushSettingsTab`
  // enumerator added without a row in the table fails here rather than
  // drawing an empty page.
  check(kBrushSettingsTabCount == 9,
        "tabs: Photoshop's own eight brush-settings panels, plus the shelved "
        "Dynamics matrix editor");

  // ======================================================================
  std::printf("  -- B. every row carries its OWN id --\n");
  // ======================================================================
  {
    bool everyRowMatchesItsIndex = true;
    bool everyOneNamed = true;
    for (size_t i = 0; i < kBrushSettingsTabCount; ++i) {
      const BrushSettingsTab tab = static_cast<BrushSettingsTab>(i);
      if (brushSettingsTabSpec(tab).tab != tab) everyRowMatchesItsIndex = false;
      const std::string name = brushSettingsTabName(tab);
      if (name.empty() || name == "UNNAMED") everyOneNamed = false;
    }
    check(everyRowMatchesItsIndex,
          "tabs: each row's id equals its index -- a row out of order draws one "
          "group's controls under another group's name");
    check(everyOneNamed,
          "tabs: every enumerator has a spelling, so a new one cannot reach the "
          "suite's output as UNNAMED");
  }

  // ======================================================================
  std::printf("  -- C. labels and tooltips a user can tell apart --\n");
  // ======================================================================
  //
  // **This is also what catches a table that is short a row**, which is the
  // way a tab added in a hurry actually goes wrong. `std::array` does not
  // reject a short initialiser list -- the remaining rows are
  // value-initialised, and `BrushSettingsTabSpec`'s own defaults make that an
  // EMPTY label rather than a null one, so nothing crashes and a nameless tab
  // simply appears on the strip. The non-empty checks below are what turn
  // that into a red line.
  //
  // Two other ways to add a tab wrong never reach this suite at all, and are
  // recorded here rather than asserted twice: an enumerator added without a
  // `brushSettingsTabName()` case, or without a case in the window's own draw
  // switch, is caught by `-Werror,-Wswitch` at compile time. That is a
  // stronger guarantee than a test, and re-asserting it here would be an
  // assertion no compiling build could fail.
  {
    std::set<std::string> labels;
    std::set<std::string> tooltips;
    bool everyLabelNonEmpty = true;
    bool everyTooltipNonEmpty = true;
    for (size_t i = 0; i < kBrushSettingsTabCount; ++i) {
      const BrushSettingsTabSpec& spec =
          brushSettingsTabSpec(static_cast<BrushSettingsTab>(i));
      const std::string label = spec.label;
      const std::string tip = spec.tooltip;
      if (label.empty()) everyLabelNonEmpty = false;
      if (tip.empty()) everyTooltipNonEmpty = false;
      labels.insert(label);
      tooltips.insert(tip);
    }
    check(everyLabelNonEmpty && labels.size() == kBrushSettingsTabCount,
          "tabs: nine distinct, non-empty labels -- two tabs reading the same is a "
          "tab a user cannot choose deliberately");
    // Every label is also an ImGui **id** inside the tab bar, and two items
    // sharing an id in one bar is the ImGui bug where clicking one activates
    // the other. The distinctness above is what rules it out, and this says so
    // rather than leaving it as a happy consequence.
    check(everyTooltipNonEmpty && tooltips.size() == kBrushSettingsTabCount,
          "tabs: nine distinct, non-empty tooltips -- a one-word tab label like "
          "'Scattering' is not self-explanatory and this is the only place it is said");
  }

  return ok;
}

}  // namespace np
