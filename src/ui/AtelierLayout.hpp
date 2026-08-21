#pragma once
#include <cstddef>

// ui/AtelierLayout -- docs/ui.md section 2's window layout, as arithmetic.
//
// The ASCII diagram in that section is dimensioned, and those dimensions are
// the whole content of this file: bands 36 / 34 / 46 / 26 px tall, a 104 px
// tool palette, a 322 px right-hand column, and 2 px rules between major
// regions. What the outgoing chrome had instead were three constants invented
// in place -- a 78 px palette (`kToolCol * kToolSize + 18`), a 300 px controls
// column and a 62 px swatch strip along the bottom that the design does not
// have at all.
//
// Deliberately free of ImGui: the geometry is the part worth testing, and a
// test that needs a window, a device and a font atlas to check that four bands
// tile a rectangle is a test nobody runs. `--selftest` computes layouts at
// several window sizes and asserts they tile exactly, with no ImGui context
// anywhere near it.
namespace np {

struct AtelierRect {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

  constexpr float right() const noexcept { return x + w; }
  constexpr float bottom() const noexcept { return y + h; }
  constexpr bool empty() const noexcept { return w <= 0.0f || h <= 0.0f; }
};

// docs/ui.md section 2. The trailing numbers in the diagram are heights; the
// two under it (104, 322) are the column widths.
constexpr float kTitleBarH   = 36.0f;
constexpr float kTabStripH   = 34.0f;
constexpr float kOptionsBarH = 46.0f;
constexpr float kStatusBarH  = 26.0f;
constexpr float kToolPaletteW = 104.0f;
constexpr float kRightColumnW = 322.0f;

// "Tool cells are 50px in a 2-wide grid -- generous desktop targets, and the
// palette scrolls, so the tool count is not layout-constrained." 2 x 50 = 100
// leaves 4 px, which is the palette's own padding rather than a gap the cells
// have to absorb.
constexpr float kToolCellSize = 50.0f;
constexpr int   kToolGridCols = 2;
static_assert(kToolGridCols * kToolCellSize <= kToolPaletteW,
              "docs/ui.md section 2: a 2-wide grid of 50px cells has to fit the 104px palette");

// The six regions of docs/ui.md section 2, plus the rules between them.
//
// Every rect is in the same coordinate space as the value handed to
// `atelierLayout()` -- ImGui viewport work coordinates at the call site, but
// this file does not know that and does not care.
struct AtelierBands {
  AtelierRect titleBar;     // wordmark, menus, undo/redo/panels
  AtelierRect tabStrip;     // documents; empty until PLAN.md Phase 5 step 14
  AtelierRect optionsBar;   // the active tool's options
  AtelierRect toolPalette;  // left column, 2 x n grid + FG/BG swatch
  AtelierRect canvas;       // canvas + rulers + navigator
  AtelierRect rightColumn;  // COLOR / BRUSH / LAYERS / HISTORY / ...
  AtelierRect statusBar;    // zoom, working space, resident/budget, view state

  // 2 px `#f3f2f2` between major regions (docs/ui.md section 1). Four
  // horizontal and two vertical at most; a rule adjacent to an empty band is
  // itself empty, so that suppressing the tab strip suppresses exactly one
  // rule and the bands still tile.
  static constexpr size_t kMaxRules = 6;
  AtelierRect rules[kMaxRules];
  size_t ruleCount = 0;
};

// Lay out a window of `w` x `h` at origin (`x`, `y`).
//
// `showTabStrip` is false until documents present as tabs (PRD A5, PLAN.md
// Phase 5 step 14). It is a parameter rather than a `constexpr false` because
// the band belongs to the layout now -- what is missing is the thing that
// fills it, and a layout that cannot express the design's own diagram would
// have to be rewritten rather than switched on.
//
// Undersized windows: the bands are absolute and the canvas is the remainder,
// so a window shorter than the bands would give the canvas a negative height.
// The canvas is clamped at zero and the bands keep their sizes -- chrome that
// shrinks is chrome that lies about its hit targets, and the honest failure
// mode for a window too small to hold the design is a canvas you cannot see.
AtelierBands atelierLayout(float x, float y, float w, float h, bool showTabStrip);

// ------------------------------------------------------------- navigator
//
// docs/ui.md section 2 draws a NAVIGATOR floating over the bottom-right of the
// canvas. Its geometry is here with the rest of the layout, and for the same
// reason: it is arithmetic, and arithmetic checked by looking at a screenshot
// is arithmetic nobody has checked.
constexpr float kNavigatorMaxW = 180.0f;
constexpr float kNavigatorMaxH = 140.0f;
constexpr float kNavigatorInset = 16.0f;

// The navigator's box: the document's aspect fitted inside
// kNavigatorMaxW x kNavigatorMaxH, inset from the canvas region's bottom-right
// corner.
//
// Returns an empty rect when the document has no area, or when the canvas is
// too small to hold the box and its inset without covering the paint area --
// a navigator that hides the picture it navigates is worse than none, and at
// that size the user is already scrolled into a corner of a window that cannot
// hold the design.
AtelierRect atelierNavigatorRect(const AtelierRect& canvas, float docW, float docH);

// Map a document-space rect (`x0,y0`-`x1,y1`, in document pixels) onto the
// navigator box. Used for the viewport indicator; clamped to the box, because
// the visible region of a zoomed-out view extends past the paper on every side
// and an unclamped indicator would draw outside the navigator.
AtelierRect atelierNavigatorMap(const AtelierRect& nav, float docW, float docH, float x0,
                                float y0, float x1, float y1);

}  // namespace np
