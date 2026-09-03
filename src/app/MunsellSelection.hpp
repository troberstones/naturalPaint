#pragma once
#include <array>
#include <optional>

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

}  // namespace np
