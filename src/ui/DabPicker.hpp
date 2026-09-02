#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "brush/Deposit.hpp"

// ui/DabPicker -- **the grid that lets a brush's tip be swapped for another
// one**, over app/DabLibrary's folder.
//
// ==========================================================================
// 1. Why the arithmetic is in a header and the drawing is not
// ==========================================================================
//
// `--selftest` cannot reach an ImGui dispatch site (reachability-audit F4),
// so a picker written entirely inside a draw function is a picker with no
// assertions on it at all. Everything here that can be wrong in a way a
// screenshot would not show -- how many columns fit, which cell a click
// landed in, where a thumbnail sits in the atlas, how a non-square tip is
// letterboxed into a square cell -- is a pure function taking numbers and
// returning numbers, tested directly. `drawDabPicker()` is the thin part: it
// calls these, draws what they say, and decides nothing on its own.
//
// ==========================================================================
// 2. One atlas, not one texture per dab
// ==========================================================================
//
// A folder of five hundred tips drawn as five hundred `AddImage()` calls is
// five hundred texture binds and five hundred draw calls, in a panel that
// redraws every frame. The thumbnails all go into shared 1024x1024 pages
// instead, so a page is one bind however many cells are showing, and only the
// cells intersecting the visible window are drawn at all -- so a cell's file
// is decoded the first time it is actually SEEN, not when the folder is
// scanned.
//
// The cull is a rectangle test against the window, **not**
// `ImGuiListClipper`. The clipper wants one ImGui item per row to measure and
// skip; this grid is one `InvisibleButton` over the whole thing with the cells
// painted into the draw list underneath (§ the hit test below), so there are
// no per-row items for it to work with. A rectangle test costs O(N)
// arithmetic and nothing else for the off-screen rows -- no decode, no
// upload, no draw command.
//
// **RGBA8 with the coverage in ALPHA over white, not R8.** ImGui's own
// pipeline samples RGBA and multiplies by the vertex colour, so alpha-over-
// white lets a cell be tinted by passing a colour and needs no second shader;
// an R8 atlas would. And it is not a linear-light texture -- coverage is an
// opacity, which is never gamma-encoded -- so this is one of the cases that
// may go through `AddImage()` rather than `ui/CanvasQuad`. The canvas, the
// navigator and the sim views may not; see ui/CanvasQuad.hpp.
//
// ==========================================================================
// 3. What selecting a cell does, and what it deliberately does not
// ==========================================================================
//
// It sets the tip. It does **not** resize the brush to the tip's own pixel
// size, even though the tip has one and Photoshop offers it, because a picker
// that silently changed the brush's diameter would make "try the other tip"
// an edit the user has to undo twice. "Use native size" is a separate button
// beside the grid, so resizing is something asked for rather than something
// that happens.
//
// A `.gbr` that carries its own spacing is the same case: offered on the
// selected cell, applied only on request.
namespace np {

// Where the grid puts its cells, given the width it has to work in.
//
// `desiredCell` is a preference, not a demand -- the columns that fit are
// computed from it and then the cell is grown back to fill the row exactly,
// so there is never a ragged strip of unused width on the right. One column
// is always allowed, however narrow the panel gets: a picker that vanishes
// when a pane is dragged small is worse than one with big cells.
struct DabPickerLayout {
  int columns = 1;
  int rows = 0;
  float cellSize = 0.0f;
  float spacing = 0.0f;
  float totalHeight = 0.0f;
};

DabPickerLayout dabPickerLayoutFor(int count, float availableWidth, float desiredCell,
                                   float spacing) noexcept;

// The top-left of cell `index`, relative to the grid's own origin.
void dabPickerCellOrigin(const DabPickerLayout& layout, int index, float& xOut,
                         float& yOut) noexcept;

// Which cell contains the point `(x, y)` relative to the grid origin, or -1
// for a point in the gutters or past the end. **The inverse of
// `dabPickerCellOrigin()`, and tested as one** -- a hit test derived
// independently from the layout is how a picker ends up selecting the cell
// next to the one that was clicked.
int dabPickerCellAt(const DabPickerLayout& layout, int count, float x, float y) noexcept;

// The first cell of a row. Trivial arithmetic, here so the draw site does not
// repeat it in a way that can disagree with `dabPickerCellOrigin()`.
int dabPickerFirstCellOfRow(const DabPickerLayout& layout, int row) noexcept;

// --- Thumbnails ----------------------------------------------------------

// One tip, fitted into a `cell` x `cell` RGBA8 square: white with the coverage
// in alpha, **aspect preserved and centred**, empty margin fully transparent.
//
// Nearest-neighbour, deliberately. A thumbnail of a brush tip is read for its
// SHAPE -- is this the speckled one or the smooth one -- and a box filter at
// 1/20 scale turns every speckled tip into the same grey blob. The full-size
// tip is what paints; this is an index card.
std::vector<uint8_t> dabThumbnailRgba(const BrushTipBitmap& tip, int cell);

// Where thumbnail `index` lives in the atlas: which page, and the texel rect
// within it. Pages exist so a folder bigger than one page's worth of cells
// still draws -- at two binds instead of one, which is the honest cost.
struct DabAtlasSlot {
  int page = 0;
  int x = 0;
  int y = 0;
};
DabAtlasSlot dabAtlasSlotFor(int index, int cellPx, int atlasPx) noexcept;
int dabAtlasSlotsPerPage(int cellPx, int atlasPx) noexcept;

// --- The panel -----------------------------------------------------------

// What one frame of the picker asks its caller to do. **Nothing here is done
// by the picker itself**: it draws a grid and reports, so that "clicking a
// cell changes the brush" is a decision at the call site rather than a side
// effect buried in a draw function, and so §3's rule -- selecting sets the tip
// and nothing else -- is visible where the brush is actually mutated.
struct DabPickerAction {
  bool selected = false;         // a cell was clicked
  std::string id;                // ...this one
  bool useNativeSize = false;    // "Use native size" pressed for the current tip
  int32_t nativeWidth = 0;       // ...which is this many texels across
  int32_t nativeHeight = 0;
  bool useFileSpacing = false;   // "Use its spacing" pressed
  float fileSpacing = 0.0f;      // ...this, already in radii (not the file's percent)
  bool rescanRequested = false;  // RESCAN pressed
  bool revealRequested = false;  // "Reveal in Finder" pressed
};

// Draws the grid, its header row and its empty state. `currentId` is the dab
// the brush is using now, drawn selected; empty means the procedural tip,
// which gets its own first cell so "go back to the round tip" is reachable
// from the same grid rather than from somewhere else.
//
// Takes the library non-const because a cell that becomes visible for the
// first time has to decode its file to have a thumbnail at all -- which is
// exactly the lazy cost app/DabLibrary's index exists to defer, and the
// visibility cull is what keeps it paid per visible row rather than per
// folder.
class GpuContext;
class DabLibrary;
DabPickerAction drawDabPicker(const char* id, DabLibrary& dabs, GpuContext& gpu,
                              const std::string& currentId);

}  // namespace np
