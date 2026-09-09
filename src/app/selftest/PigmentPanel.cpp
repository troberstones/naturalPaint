#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/PanelLayout.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

// Every ControlsSection enumerator and its default placement, written out by
// hand for the same reason app/selftest/PanelLayout.cpp's own `kAllSections`
// is: a section this table missed would be missing from the check that finds
// it, not caught by it. Derived from `app/PanelLayout.cpp`'s own
// `defaultPlacementFor()` rule -- Tools -> Left, Options -> Top, Pigment ->
// Hidden (this track's change), everything else by role
// (View/Simulation -> Flyout, Tool/Document -> Right) -- and cross-checked
// against `app/ControlsLayout.cpp`'s own role table for every entry that
// isn't Tools/Options/Pigment.
struct ExpectedPlacement {
  ControlsSection section;
  PanelPlacement placement;
};
constexpr ExpectedPlacement kExpected[] = {
    {ControlsSection::Tools, PanelPlacement::Left},
    {ControlsSection::Options, PanelPlacement::Top},
    {ControlsSection::Color, PanelPlacement::Right},
    {ControlsSection::Layers, PanelPlacement::Right},
    {ControlsSection::History, PanelPlacement::Right},
    {ControlsSection::Comps, PanelPlacement::Right},
    {ControlsSection::Grade, PanelPlacement::Flyout},
    {ControlsSection::Histogram, PanelPlacement::Flyout},
    {ControlsSection::BrushLibrary, PanelPlacement::Right},
    {ControlsSection::Brush, PanelPlacement::Right},
    {ControlsSection::Pigment, PanelPlacement::Hidden},
    {ControlsSection::Medium, PanelPlacement::Flyout},
    {ControlsSection::BoardTilt, PanelPlacement::Flyout},
    {ControlsSection::Grid, PanelPlacement::Flyout},
    {ControlsSection::Solver, PanelPlacement::Flyout},
};

}  // namespace

// A real, opt-in PIGMENT panel: `AppState::pigmentOverride`, `effectivePigmentConstants()`
// and `selectPigment()` (app/AppState.hpp), the Hidden-by-default placement
// (app/PanelLayout.cpp's `defaultPlacementFor()`), and the Window > Pigment
// check item that raises it (`MenuAction::Pigment`, ui/MenuModel.{hpp,cpp} and
// `performMenuAction()`'s Pigment case, ui/MacPaintUI.cpp). Headless, GPU-free
// and ImGui-free -- nothing here opens a frame or drives a click; the swatch
// click itself is exercised indirectly, through the one function
// (`selectPigment()`) it is required to call.
bool runPigmentPanelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. defaultPlacementFor(): Pigment is Hidden, nothing else moved ----
  {
    PanelLayout layout;  // the constructor is resetToDefault()
    bool allMatch = true;
    for (const ExpectedPlacement& e : kExpected)
      if (layout.placementOf(e.section) != e.placement) allMatch = false;
    check(allMatch,
          "default placement: Pigment is Hidden and no other section's default changed");
    check(layout.placementOf(ControlsSection::Pigment) == PanelPlacement::Hidden,
          "default placement: Pigment specifically -- Hidden, not its Simulation role's "
          "usual Flyout");
  }

  // --- 2. Window menu: MenuAction::Pigment's check mirrors placement ------
  {
    MenuContext on;
    on.showPigmentPanel = true;
    MenuContext off;
    off.showPigmentPanel = false;
    auto checkedState = [](const MenuContext& c) {
      for (const MenuNode& menu : buildMenuModel(c))
        for (const MenuNode& n : menu.children)
          if (n.action == MenuAction::Pigment) return n.checked;
      return false;
    };
    check(checkedState(on) && !checkedState(off),
          "Window menu: Pigment's tick follows ctx.showPigmentPanel both ways");

    // The predicate ui/MacPaintUI.cpp's frame loop uses to populate that
    // context field: `placementOf(Pigment) != Hidden`. A fresh AppState
    // starts Hidden (false); moving the section anywhere else flips it true,
    // the same way BrushSettings' own window-open flag does.
    AppState st;
    check(st.panels.placementOf(ControlsSection::Pigment) == PanelPlacement::Hidden,
          "a fresh AppState starts with Pigment Hidden, matching the default table above");
    st.panels.setPlacement(ControlsSection::Pigment, PanelPlacement::Flyout);
    check(st.panels.placementOf(ControlsSection::Pigment) != PanelPlacement::Hidden,
          "moving Pigment out of Hidden is what the frame loop's "
          "ctx.showPigmentPanel = (placementOf() != Hidden) reads as \"on\"");
  }

  // --- 3. effectivePigmentConstants() and selectPigment() -----------------
  {
    AppState st;
    st.brush.pigment = 6;  // Ultramarine Blue, BrushState's own default
    const Pigment& loaded = defaultPalette()[6];

    check(!st.pigmentOverride.active, "a fresh AppState's pigment override starts inactive");
    const PigmentConstants inactive = effectivePigmentConstants(st);
    check(inactive.density == loaded.density && inactive.staining == loaded.staining &&
              inactive.granulation == loaded.granulation,
          "effectivePigmentConstants(): inactive override reads the loaded pigment's own "
          "constants");

    st.pigmentOverride.active = true;
    st.pigmentOverride.density = 0.11f;
    st.pigmentOverride.staining = 0.22f;
    st.pigmentOverride.granulation = 0.33f;
    const PigmentConstants active = effectivePigmentConstants(st);
    check(active.density == 0.11f && active.staining == 0.22f && active.granulation == 0.33f,
          "effectivePigmentConstants(): active override reads its own three floats, not "
          "the loaded pigment's");

    // The single write site of BrushState::pigment (ui/MacPaintUI.cpp's
    // swatch click calls exactly this function) clears a stale override in
    // the same call that changes the paint.
    selectPigment(st, 3);
    check(st.brush.pigment == 3 && !st.pigmentOverride.active,
          "selectPigment(): picking a different pigment clears a stale override");
    const PigmentConstants afterSelect = effectivePigmentConstants(st);
    const Pigment& newLoaded = defaultPalette()[3];
    check(afterSelect.density == newLoaded.density &&
              afterSelect.staining == newLoaded.staining &&
              afterSelect.granulation == newLoaded.granulation,
          "effectivePigmentConstants(): after the clear, reads the newly-selected pigment "
          "instead of the stale override");
  }

  std::printf("[selftest] pigment panel %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
