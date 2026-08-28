#include "app/selftest/Support.hpp"

#include "ui/DabPicker.hpp"

namespace np {

// ---------------------------------------------------------------------------
// ui/DabPicker's arithmetic -- the half of a picker that can be wrong in a way
// a screenshot would not show.
//
// `--selftest` cannot reach an ImGui dispatch site (reachability-audit F4), so
// a grid written entirely inside a draw function carries no assertions at all.
// Everything checked here is a pure function of numbers: how many columns fit,
// where a cell sits, **which cell a click landed in**, where a thumbnail goes
// in the atlas, and how a non-square tip is letterboxed into a square.
//
// The load-bearing one is section C. `dabPickerCellAt()` is the INVERSE of
// `dabPickerCellOrigin()`, and a hit test derived independently from the
// layout is exactly how a picker ends up selecting the cell next to the one
// that was clicked -- a bug that looks like a rendering glitch and is not one.
// So the two are checked against each other over every cell of several
// layouts, rather than each against a hand-computed table.
// ---------------------------------------------------------------------------
bool runDabPickerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ======================================================================
  std::printf("  -- A. columns fit, and the row is filled exactly --\n");
  // ======================================================================
  {
    // 240 wide, 56 desired, 4 spacing: (240 + 4) / 60 = 4.06 -> 4 columns.
    const DabPickerLayout l = dabPickerLayoutFor(20, 240.0f, 56.0f, 4.0f);
    check(l.columns == 4, "picker/layout: four 56 px cells with 4 px gutters fit in 240");
    // Grown back to fill: 4 cells + 3 gutters = 240, so cell = (240 - 12) / 4 = 57.
    check(std::fabs(l.cellSize - 57.0f) < 1e-4f,
          "picker/layout: and the cell grows to fill the row exactly -- no ragged right edge");
    check(l.rows == 5 && std::fabs(l.totalHeight - (5 * 57.0f + 4 * 4.0f)) < 1e-3f,
          "picker/layout: 20 cells in 4 columns is 5 rows, and the height counts 4 gutters");

    // A partial last row still counts as a row -- the cells in it have to be
    // somewhere.
    const DabPickerLayout partial = dabPickerLayoutFor(17, 240.0f, 56.0f, 4.0f);
    check(partial.rows == 5, "picker/layout: 17 cells in 4 columns is 5 rows, not 4");

    // Empty is zero rows and zero height, not one empty row: a picker with no
    // dabs should leave room for the message that says so, not for a blank
    // grid.
    const DabPickerLayout empty = dabPickerLayoutFor(0, 240.0f, 56.0f, 4.0f);
    check(empty.rows == 0 && empty.totalHeight == 0.0f,
          "picker/layout: no dabs is no rows and no height, leaving room for the reason");

    // **One column is always allowed.** A pane dragged narrower than one cell
    // must still show a picker, with cells too big for it, rather than a
    // divide-by-zero or an empty column count.
    const DabPickerLayout narrow = dabPickerLayoutFor(9, 20.0f, 56.0f, 4.0f);
    check(narrow.columns == 1 && narrow.rows == 9 && narrow.cellSize > 0.0f,
          "picker/layout: a pane narrower than one cell still gets one column");
    const DabPickerLayout degenerate = dabPickerLayoutFor(4, 0.0f, 0.0f, -3.0f);
    check(degenerate.columns >= 1 && degenerate.cellSize > 0.0f && degenerate.spacing >= 0.0f &&
              std::isfinite(degenerate.totalHeight),
          "picker/layout: a zero width, zero cell and negative spacing give a usable answer");

    // **A case where "cells fit" and "cells and gutters fit" disagree.** At
    // 240 wide with 48 px cells and 8 px gutters, counting cells alone says 5
    // -- and five cells plus four gutters is 272, which overflows the panel by
    // a whole gutter and a bit. The 240/56/4 case above cannot tell the two
    // formulas apart (both say 4), so it was passing a sabotage that dropped
    // the gutter term entirely.
    const DabPickerLayout gutters = dabPickerLayoutFor(12, 240.0f, 48.0f, 8.0f);
    check(gutters.columns == 4,
          "picker/layout: the gutters count toward the fit -- 5 x 48 + 4 x 8 does NOT fit in 240");
    check(gutters.cellSize * 4.0f + gutters.spacing * 3.0f <= 240.0f + 1e-3f,
          "picker/layout: and whatever it chose, the row it lays out fits the width it was given");

    // Spacing of zero is a legal look, not an edge to be defended against.
    const DabPickerLayout tight = dabPickerLayoutFor(8, 200.0f, 50.0f, 0.0f);
    check(tight.columns == 4 && std::fabs(tight.cellSize - 50.0f) < 1e-4f,
          "picker/layout: zero spacing packs cells edge to edge with no leftover");
  }

  // ======================================================================
  std::printf("  -- B. cells sit where the layout says --\n");
  // ======================================================================
  {
    const DabPickerLayout l = dabPickerLayoutFor(10, 240.0f, 56.0f, 4.0f);
    float x = 0.0f, y = 0.0f;
    dabPickerCellOrigin(l, 0, x, y);
    check(x == 0.0f && y == 0.0f, "picker/cells: cell 0 is at the grid origin");
    dabPickerCellOrigin(l, 3, x, y);
    check(std::fabs(x - 3.0f * (l.cellSize + l.spacing)) < 1e-4f && y == 0.0f,
          "picker/cells: the last cell of row 0 is three strides across and still on row 0");
    dabPickerCellOrigin(l, 4, x, y);
    check(x == 0.0f && std::fabs(y - (l.cellSize + l.spacing)) < 1e-4f,
          "picker/cells: cell 4 wraps to the start of row 1");
    check(dabPickerFirstCellOfRow(l, 2) == 8,
          "picker/cells: row 2 begins at cell 8, which is what the clipper is told");

    // The right edge of the last column lands exactly on the available width.
    dabPickerCellOrigin(l, l.columns - 1, x, y);
    check(std::fabs((x + l.cellSize) - 240.0f) < 1e-3f,
          "picker/cells: the last column's right edge is the panel's, to the pixel");
  }

  // ======================================================================
  std::printf("  -- C. the hit test is the layout's inverse, cell for cell --\n");
  // ======================================================================
  {
    bool allRoundTrip = true;
    bool gutterRejected = true;
    bool pastEndRejected = true;
    for (const float width : {240.0f, 137.0f, 61.0f, 800.0f}) {
      for (const float spacing : {0.0f, 4.0f, 11.0f}) {
        const int count = 23;
        const DabPickerLayout l = dabPickerLayoutFor(count, width, 56.0f, spacing);
        for (int i = 0; i < count; ++i) {
          float cx = 0.0f, cy = 0.0f;
          dabPickerCellOrigin(l, i, cx, cy);
          // The cell's own centre, its top-left, and just inside its
          // bottom-right must all name cell i.
          const float half = l.cellSize * 0.5f;
          if (dabPickerCellAt(l, count, cx + half, cy + half) != i) allRoundTrip = false;
          if (dabPickerCellAt(l, count, cx + 0.01f, cy + 0.01f) != i) allRoundTrip = false;
          if (dabPickerCellAt(l, count, cx + l.cellSize - 0.01f, cy + l.cellSize - 0.01f) != i)
            allRoundTrip = false;

          // A point in the gutter to the right of a cell belongs to NEITHER
          // it nor its neighbour.
          if (spacing > 0.0f && (i % l.columns) + 1 < l.columns) {
            const float inGutter = cx + l.cellSize + spacing * 0.5f;
            if (dabPickerCellAt(l, count, inGutter, cy + half) != -1) gutterRejected = false;
          }
        }
        // Past the last cell of a partly-filled final row, and below the grid.
        if (dabPickerCellAt(l, count, width * 0.5f, l.totalHeight + 10.0f) != -1)
          pastEndRejected = false;
        if (dabPickerCellAt(l, count, -1.0f, 5.0f) != -1) pastEndRejected = false;
      }
    }
    check(allRoundTrip,
          "picker/hit: every cell of 12 layouts round-trips origin -> hit test -> same index");
    check(gutterRejected,
          "picker/hit: a click in a gutter selects NEITHER neighbour -- it selects nothing");
    check(pastEndRejected,
          "picker/hit: a point past the last cell, or left of the grid, selects nothing");

    // The trailing gap on a partly-filled last row is empty space, not the
    // first cell of a row that does not exist.
    const DabPickerLayout l = dabPickerLayoutFor(5, 240.0f, 56.0f, 4.0f);
    float cx = 0.0f, cy = 0.0f;
    dabPickerCellOrigin(l, 5, cx, cy);  // where cell 5 WOULD be
    check(dabPickerCellAt(l, 5, cx + 2.0f, cy + 2.0f) == -1,
          "picker/hit: the empty half of a partly-filled last row selects nothing");
  }

  // ======================================================================
  std::printf("  -- D. a thumbnail keeps its shape --\n");
  // ======================================================================
  {
    // A 4-wide, 1-tall bar. Squashed into a square it would be indis-
    // tinguishable from a round tip, which is the one thing the thumbnail
    // exists to tell apart.
    BrushTipBitmap bar;
    bar.width = 4;
    bar.height = 1;
    bar.alpha = {255, 255, 255, 255};
    const std::vector<uint8_t> thumb = dabThumbnailRgba(bar, 16);
    check(thumb.size() == 16u * 16u * 4u, "picker/thumb: the buffer is exactly cell x cell RGBA");

    int opaqueRows = 0;
    int opaqueInRow[16] = {0};
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 16; ++x)
        if (thumb[(static_cast<size_t>(y) * 16 + x) * 4 + 3] > 0) ++opaqueInRow[y];
      if (opaqueInRow[y] > 0) ++opaqueRows;
    }
    // 4:1 into a 16 px cell is 16 wide and 4 tall, centred: rows 6..9.
    check(opaqueRows == 4 && opaqueInRow[7] == 16 && opaqueInRow[0] == 0,
          "picker/thumb: a 4:1 bar stays 4:1 -- the cell is letterboxed, not squashed");
    check(opaqueInRow[6] > 0 && opaqueInRow[9] > 0 && opaqueInRow[5] == 0 &&
              opaqueInRow[10] == 0,
          "picker/thumb: and is centred in the cell rather than pinned to a corner");

    // White under the alpha, so ImGui's vertex colour tints it and no second
    // shader is needed (header §2). Checked on a PARTIALLY covered texel, not
    // a fully covered one: at full coverage "white with alpha 255" and
    // "coverage in RGB with alpha 255" are the same four bytes, so the fully
    // covered texel cannot tell the two apart -- and a sabotage that put the
    // coverage in RGB survived the version of this that only looked there.
    BrushTipBitmap halfTip;
    halfTip.width = halfTip.height = 2;
    halfTip.alpha = {64, 64, 64, 64};
    const std::vector<uint8_t> halfThumb = dabThumbnailRgba(halfTip, 8);
    check(halfThumb[0] == 255 && halfThumb[1] == 255 && halfThumb[2] == 255 &&
              halfThumb[3] == 64,
          "picker/thumb: coverage is in ALPHA over white, which is what lets a cell be tinted");
    // The margin is fully transparent, so a non-square tip does not appear to
    // sit on a white card.
    check(thumb[3] == 0 && thumb[(15u * 16 + 15) * 4 + 3] == 0,
          "picker/thumb: the letterbox margin is transparent, not white");

    // Coverage survives the resample rather than being thresholded.
    BrushTipBitmap ramp;
    ramp.width = 4;
    ramp.height = 4;
    ramp.alpha.assign(16, 0);
    for (int i = 0; i < 16; ++i) ramp.alpha[i] = static_cast<uint8_t>(i * 17);
    const std::vector<uint8_t> rampThumb = dabThumbnailRgba(ramp, 8);
    bool sawMid = false;
    for (size_t i = 3; i < rampThumb.size(); i += 4)
      if (rampThumb[i] > 0 && rampThumb[i] < 255) sawMid = true;
    check(sawMid, "picker/thumb: partial coverage stays partial -- no threshold to black or white");

    // A tip with nothing in it is a transparent cell, not a crash and not a
    // white square. An empty `samp` record can produce one.
    const std::vector<uint8_t> emptyThumb = dabThumbnailRgba(BrushTipBitmap{}, 12);
    bool allClear = emptyThumb.size() == 12u * 12u * 4u;
    for (size_t i = 3; i < emptyThumb.size(); i += 4)
      if (emptyThumb[i] != 0) allClear = false;
    check(allClear, "picker/thumb: a tip with no pixels is an empty cell, not a white one");
  }

  // ======================================================================
  std::printf("  -- E. the atlas packs, and pages when it has to --\n");
  // ======================================================================
  {
    // 64 px cells in a 1024 px page: 16 across, 256 per page.
    check(dabAtlasSlotsPerPage(64, 1024) == 256,
          "picker/atlas: 64 px cells give 256 slots in a 1024 px page");
    const DabAtlasSlot first = dabAtlasSlotFor(0, 64, 1024);
    check(first.page == 0 && first.x == 0 && first.y == 0,
          "picker/atlas: slot 0 is the page's own corner");
    const DabAtlasSlot wrap = dabAtlasSlotFor(16, 64, 1024);
    check(wrap.page == 0 && wrap.x == 0 && wrap.y == 64,
          "picker/atlas: slot 16 wraps to the second row of the same page");
    const DabAtlasSlot last = dabAtlasSlotFor(255, 64, 1024);
    check(last.page == 0 && last.x == 960 && last.y == 960,
          "picker/atlas: slot 255 is the page's last cell, fully inside it");
    // **Pages, so a folder bigger than one page still draws** -- at two binds
    // instead of one, which is the honest cost and not a refusal.
    const DabAtlasSlot next = dabAtlasSlotFor(256, 64, 1024);
    check(next.page == 1 && next.x == 0 && next.y == 0,
          "picker/atlas: slot 256 starts a second page rather than overwriting the first");
    const DabAtlasSlot far = dabAtlasSlotFor(500, 64, 1024);
    check(far.page == 1 && far.x == (500 - 256) % 16 * 64 && far.y == (500 - 256) / 16 * 64,
          "picker/atlas: a 500-dab folder is two pages, and slot 500 is where it should be");

    // No slot ever runs off the end of its page, at any cell size.
    bool inBounds = true;
    for (const int cell : {16, 48, 64, 100, 1024, 2048}) {
      for (int i = 0; i < 400; ++i) {
        const DabAtlasSlot s = dabAtlasSlotFor(i, cell, 1024);
        if (s.x < 0 || s.y < 0 || s.page < 0) inBounds = false;
        if (s.x + std::min(cell, 1024) > 1024 || s.y + std::min(cell, 1024) > 1024)
          inBounds = false;
      }
    }
    check(inBounds, "picker/atlas: no slot runs off its page, at any cell size including >page");
  }

  return ok;
}

}  // namespace np
