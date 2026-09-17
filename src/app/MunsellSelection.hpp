#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "app/AppState.hpp"

// The COLOR panel's Munsell page, as *state* rather than as pixels
// (docs/munsell-picker.md). `color/Munsell.hpp` is the arithmetic and knows
// nothing about `BrushState`; `ui/MacPaintUI.cpp` draws chips and knows
// nothing about gamut bisection. This is the layer between them, and it is a
// separate translation unit for one reason: **`--selftest` has to be able to
// change hue, move the selection and read the resulting foreground without an
// ImGui frame.** Assertions 4, 5 and 6 of docs/munsell-picker.md are all
// statements about this file.
//
// Every function here takes the whole `BrushState` rather than loose
// parameters, because the invariant they maintain is a relationship *between*
// its fields -- the selected cell, the page it is on, and `rgb` -- and a
// helper that took three of the five could leave the other two disagreeing.
namespace np {

// The page's rows are values; a row's lightness is what fixes its luminance.
double munsellRowLStar(int row, int steps) noexcept;

// The chroma of a cell, under whichever normalisation the brush has selected.
// Column 0 is always 0 -- the neutral of that row -- under both policies.
double munsellCellChroma(const BrushState& brush, int row, int col) noexcept;

// The cell's colour in **display-referred sRGB**, the encoding
// `BrushState::rgb` is in, or `std::nullopt` for a void. The encode happens
// here and nowhere else: `color/Munsell` answers linear, `BrushState::rgb` is
// sRGB, and that boundary has exactly one crossing in this feature.
std::optional<std::array<float, 3>> munsellCellSrgb(const BrushState& brush, int row,
                                                    int col) noexcept;

// Move the selection onto a live cell **without leaving its row**.
//
// A hue change can make the selected cell a void, and something has to give.
// Giving up the column costs chroma; giving up the row costs luminance --
// which is the single property the picker exists to hold, so the row is not
// available to trade. This walks left along the row to the last live column
// and never touches `munsellRow`. Also clamps `munsellSteps` into range and
// the indices into the grid, so a hand-set `BrushState` cannot index off the
// page.
void clampMunsellSelection(BrushState& brush) noexcept;

// Write the selected cell into `BrushState::rgb`. The one writer: the panel
// calls it after every click, drag, hue change and `n` change, and switching
// into the mode calls it so the foreground agrees with the highlighted chip
// on the first frame rather than after the first click. Clamps first, so a
// void selection cannot be committed.
void applyMunsellSelection(BrushState& brush) noexcept;

// `pageChroma()` for this brush's page, memo-free -- the panel calls it once
// a frame and the bisection is ~60 gamut tests per row.
double munsellPageChromaFor(const BrushState& brush) noexcept;

// The whole page's cell colours, cached against the three fields that
// determine every one of them: `munsellSteps`, `munsellHueDeg` and
// `munsellPerRowChroma`. Nothing else `BrushState` carries -- not the
// selected row/col, not `rgb`, not the pigment -- changes a single pixel of
// the grid, so a cache keyed on just those three turns "the user is dragging
// the hue bar" into one gamut sweep per *change* instead of one per *cell,
// per frame*.
//
// `munsellCellChroma()`'s own per-page branch calls `munsellPageChromaFor()`
// -- a full `steps`-row bisection sweep -- from scratch for every cell,
// which is `n` times more work than the sweep itself needs: the result is
// identical across an entire page's `n` columns. `drawMunsellPage()` used to
// pay for that `n` times over, `n` times a frame, for as long as the panel
// stayed open -- this class is what lets it pay once per actual change
// instead.
//
// Same shape as `app/StrokePreview`'s `StrokePreviewCache`: recompute only
// on a real key change, count both outcomes so `--selftest` can prove the
// cache invalidates instead of trusting it.
class MunsellPageCache {
 public:
  // Row-major, `steps * steps` entries in `munsellCellSrgb()`'s own (row,
  // col) order -- `grid[row * steps + col]`. The returned reference is only
  // valid until the next call.
  const std::vector<std::optional<std::array<float, 3>>>& gridFor(const BrushState& brush);

  uint64_t recomputes() const noexcept { return recomputes_; }
  uint64_t hits() const noexcept { return hits_; }

 private:
  bool haveKey_ = false;
  int steps_ = 0;
  float hueDeg_ = 0.0f;
  bool perRowChroma_ = false;
  std::vector<std::optional<std::array<float, 3>>> grid_;
  uint64_t recomputes_ = 0;
  uint64_t hits_ = 0;
};

}  // namespace np
