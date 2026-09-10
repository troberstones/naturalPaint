#include "app/selftest/Support.hpp"

#include "ui/MacPaintUI.hpp"

namespace np {

// The LAYERS panel's list box stopped being sized by its contents.
//
// It used to be `min(visibleRows, rowsThatFit) * rowH` plus padding: the box
// grew one row taller with every layer added and one row shorter with every
// one deleted, and because everything the panel draws below it -- the NEW +
// command row, the icon row, the Multi-selection header -- follows the ImGui
// cursor, all of it walked up and down the panel too. A button that is not
// where it was two clicks ago is the defect; a fixed box that scrolls is the
// fix.
//
// The property that fix rests on is a negative one, which is why it is
// asserted here rather than left to the golden images: **the row count is not
// an input to the height.** A screenshot can only ever photograph one row
// count, so `layers` passing proves the box looks right with three layers in
// it, not that it would be the same size with thirty. This sweeps the count
// instead.
//
// `layerRowsChildHeight()` takes `rowCount` and ignores it precisely so this
// can be written. Sabotaged by making it use the count -- `return
// std::max(rowH + 2.0f * windowPaddingY, std::min(availY - reserveBelowY,
// rowCount * rowH))`, which is the old behaviour restored -- the sweep below
// fails on the first count whose rows do not already fill the dock, as it
// should.
bool runLayerListHeightTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // A dock with room for about nine rows, and the panel's own reserve for the
  // two command rows and the collapsed Multi-selection header below the box.
  const float rowH = 44.0f;
  const float padY = 4.0f;
  const float reserve = 70.0f;
  const float availY = 470.0f;

  const float empty = layerRowsChildHeight(availY, reserve, rowH, padY, 0);
  check(std::abs(empty - (availY - reserve)) < 0.001f,
        "empty document: the box still fills the dock's spare room");

  bool constantAcrossCounts = true;
  for (std::size_t n = 0; n <= 200; ++n)
    if (std::abs(layerRowsChildHeight(availY, reserve, rowH, padY, n) - empty) > 0.001f)
      constantAcrossCounts = false;
  check(constantAcrossCounts,
        "0..200 layers in the same dock: one height, not 201 of them");

  // The controls below the box are what this is really about: the box plus the
  // reserve is the whole dock, so the command row lands on the same pixel no
  // matter how many layers there are.
  bool controlsPinned = true;
  for (std::size_t n = 0; n <= 200; ++n)
    if (std::abs((layerRowsChildHeight(availY, reserve, rowH, padY, n) + reserve) - availY) >
        0.001f)
      controlsPinned = false;
  check(controlsPinned, "command row sits at the dock's bottom for every count");

  // Resizing the dock IS allowed to change it, and in the obvious direction --
  // the one input that is not ignored.
  const float taller = layerRowsChildHeight(availY + 120.0f, reserve, rowH, padY, 3);
  const float shorter = layerRowsChildHeight(availY - 120.0f, reserve, rowH, padY, 3);
  check(std::abs(taller - (empty + 120.0f)) < 0.001f,
        "a dock dragged 120 px taller gives the box 120 px more");
  check(shorter < empty && shorter > 0.0f, "a dock dragged shorter gives the box less");

  // The floor is not defensive padding: `BeginChild()` reads 0 as *fill the
  // rest of the column* and a negative as a reserve off the bottom, so a dock
  // squeezed below its own controls must still ask for a real height.
  const float squeezed = layerRowsChildHeight(20.0f, reserve, rowH, padY, 5);
  check(squeezed >= rowH + 2.0f * padY,
        "dock shorter than its controls: one row's box, never 0 or negative");
  const float degenerate = layerRowsChildHeight(0.0f, 0.0f, rowH, padY, 0);
  check(degenerate > 0.0f, "zero available room still yields a positive height");

  std::printf("[selftest] layer list height %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
