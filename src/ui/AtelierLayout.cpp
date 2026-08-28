#include "ui/AtelierLayout.hpp"

#include <algorithm>
#include <cmath>

#include "ui/AtelierTheme.hpp"  // kRuleThickness

namespace np {
namespace {

// A horizontal rule spanning the full width, consuming `cursor`.
AtelierRect hRule(float x, float w, float& cursor) {
  const AtelierRect r{x, cursor, w, kRuleThickness};
  cursor += kRuleThickness;
  return r;
}

}  // namespace

float atelierToolCellSize(float paletteH) noexcept {
  const float gridH = paletteH - kToolSwatchAreaH;
  // `floor`, not round: a cell one pixel *smaller* than the exact fit still
  // fits with a pixel to spare, and a cell one pixel *larger* is the
  // clipping bug this file's own kToolPaletteW comment already tells the
  // story of, in a new place.
  const float raw = std::floor((gridH - kToolSeparatorsH) /
                               static_cast<float>(kToolCellCount));
  return std::clamp(raw, kToolCellMin, kToolCellMax);
}

AtelierToolGrid atelierToolGrid(float availW, float availH) noexcept {
  AtelierToolGrid g;
  // Largest first, so the first fit is the best fit.
  for (float cell = kToolCellMax; cell >= kToolCellMin; cell -= 1.0f) {
    const int cols = static_cast<int>(std::floor(availW / cell));
    if (cols < 1) continue;
    const int rows = (kToolCellCount + cols - 1) / cols;
    // The separator rules are drawn between groups down the column, so they
    // cost height once per row-break at most -- charging all four regardless
    // is the conservative reading and keeps this arithmetic independent of
    // which group boundary lands on which row.
    if (static_cast<float>(rows) * cell + kToolSeparatorsH > availH) continue;
    g.cell = cell;
    g.columns = cols;
    g.rows = rows;
    g.overflows = false;
    return g;
  }
  // Nothing in range fits. The disclosed fallback, matching the column's:
  // smallest legible cell, as many columns as the width allows, and the wheel
  // reaches the rest.
  g.cell = kToolCellMin;
  g.columns = std::max(1, static_cast<int>(std::floor(availW / kToolCellMin)));
  g.rows = (kToolCellCount + g.columns - 1) / g.columns;
  g.overflows = true;
  return g;
}

AtelierBands atelierLayout(float x, float y, float w, float h, bool showTabStrip) {
  return atelierLayout(x, y, w, h, showTabStrip, kDefaultDockExtents);
}

AtelierBands atelierLayout(float x, float y, float w, float h, bool showTabStrip,
                           const AtelierDockExtents& docks) {
  AtelierBands b;

  const auto addRule = [&b](AtelierRect r) {
    if (r.empty()) return;
    b.rules[b.ruleCount++] = r;
  };

  float cy = y;

  b.titleBar = AtelierRect{x, cy, w, kTitleBarH};
  cy += kTitleBarH;
  addRule(hRule(x, w, cy));

  // The tab strip and *its* rule vanish together. Reserving 34 px of empty
  // chrome for a feature that does not exist yet would be worse than either
  // shipping it or leaving it out: the user would see a band that never
  // does anything, and the layout would still have to change when it did.
  if (showTabStrip) {
    b.tabStrip = AtelierRect{x, cy, w, kTabStripH};
    cy += kTabStripH;
    addRule(hRule(x, w, cy));
  } else {
    b.tabStrip = AtelierRect{x, cy, w, 0.0f};
  }

  // The top dock, where the outgoing chrome had a permanent options bar. Same
  // 46 px by default and the same rule under it -- and now the same
  // disappearing act the tab strip does when it is empty, which is the whole
  // difference: a user who moves OPTIONS to the bottom or into a flyout gets
  // those 46 px back rather than an empty band.
  if (docks.top > 0.0f) {
    b.topDock = AtelierRect{x, cy, w, docks.top};
    cy += docks.top;
    addRule(hRule(x, w, cy));
  } else {
    b.topDock = AtelierRect{x, cy, w, 0.0f};
  }

  // The status bar is placed from the bottom, so every rounding error in the
  // bands above lands in the canvas rather than in a 26 px bar that would
  // then not sit flush with the window edge.
  const float statusTop = y + h - kStatusBarH;
  b.statusBar = AtelierRect{x, statusTop, w, kStatusBarH};
  const AtelierRect statusRule{x, statusTop - kRuleThickness, w, kRuleThickness};

  // The bottom dock sits between the status bar and the middle row, and is
  // likewise placed from the bottom upwards for the reason above: the canvas
  // is the remainder, so the canvas is where any slack belongs.
  float midBottom = statusRule.y;
  AtelierRect bottomRule{};
  if (docks.bottom > 0.0f) {
    const float bottomTop = std::max(y, midBottom - docks.bottom);
    b.bottomDock = AtelierRect{x, bottomTop, w, midBottom - bottomTop};
    bottomRule = AtelierRect{x, bottomTop - kRuleThickness, w, kRuleThickness};
    midBottom = bottomRule.y;
  } else {
    b.bottomDock = AtelierRect{x, midBottom, w, 0.0f};
  }

  // The middle row: left dock | rule | canvas | rule | right dock. Either
  // dock may be absent, and when one is, so is its rule and so is the seam it
  // would have left behind.
  const float midTop = cy;
  const float midH = std::max(0.0f, midBottom - midTop);

  AtelierRect leftRule{};
  float canvasX = x;
  if (docks.left > 0.0f) {
    b.leftDock = AtelierRect{x, midTop, docks.left, midH};
    leftRule = AtelierRect{x + docks.left, midTop, kRuleThickness, midH};
    canvasX = leftRule.right();
  } else {
    b.leftDock = AtelierRect{x, midTop, 0.0f, midH};
  }

  AtelierRect rightRule{};
  float canvasRight = x + w;
  if (docks.right > 0.0f) {
    const float rightX = x + w - docks.right;
    b.rightDock = AtelierRect{rightX, midTop, docks.right, midH};
    rightRule = AtelierRect{rightX - kRuleThickness, midTop, kRuleThickness, midH};
    canvasRight = rightRule.x;
  } else {
    b.rightDock = AtelierRect{x + w, midTop, 0.0f, midH};
  }

  b.canvas = AtelierRect{canvasX, midTop, std::max(0.0f, canvasRight - canvasX), midH};

  addRule(leftRule);
  addRule(rightRule);
  addRule(bottomRule);
  addRule(statusRule);

  return b;
}

AtelierPanes atelierSplitPanes(const AtelierRect& canvas, AtelierSplit split) {
  AtelierPanes panes;
  panes.pane[0] = canvas;
  panes.count = 1;
  if (split == AtelierSplit::Single || canvas.empty()) return panes;

  // The halves are computed from the *remainder* after the rule, and the
  // second pane is placed from the far edge, so a canvas of odd width still
  // tiles exactly: every rounding error lands in the first pane rather than
  // in a one-pixel seam between them.
  if (split == AtelierSplit::Columns) {
    const float usable = canvas.w - kRuleThickness;
    if (usable < kMinPaneW * 2.0f) return panes;
    const float firstW = std::floor(usable * 0.5f);
    panes.pane[0] = AtelierRect{canvas.x, canvas.y, firstW, canvas.h};
    panes.divider = AtelierRect{canvas.x + firstW, canvas.y, kRuleThickness, canvas.h};
    const float secondX = panes.divider.right();
    panes.pane[1] = AtelierRect{secondX, canvas.y, canvas.right() - secondX, canvas.h};
  } else {
    const float usable = canvas.h - kRuleThickness;
    if (usable < kMinPaneH * 2.0f) return panes;
    const float firstH = std::floor(usable * 0.5f);
    panes.pane[0] = AtelierRect{canvas.x, canvas.y, canvas.w, firstH};
    panes.divider = AtelierRect{canvas.x, canvas.y + firstH, canvas.w, kRuleThickness};
    const float secondY = panes.divider.bottom();
    panes.pane[1] = AtelierRect{canvas.x, secondY, canvas.w, canvas.bottom() - secondY};
  }
  panes.count = 2;
  return panes;
}

AtelierRect atelierNavigatorRect(const AtelierRect& canvas, float docW, float docH) {
  if (docW <= 0.0f || docH <= 0.0f) return AtelierRect{};

  const float scale = std::min(kNavigatorMaxW / docW, kNavigatorMaxH / docH);
  const float w = docW * scale;
  const float h = docH * scale;

  // Room for the box, its inset, and as much again of canvas left over. The
  // second half is the part that matters: the first is only "it fits".
  if (canvas.w < (w + kNavigatorInset) * 2.0f || canvas.h < (h + kNavigatorInset) * 2.0f)
    return AtelierRect{};

  return AtelierRect{canvas.right() - kNavigatorInset - w, canvas.bottom() - kNavigatorInset - h,
                     w, h};
}

AtelierRect atelierNavigatorMap(const AtelierRect& nav, float docW, float docH, float x0,
                                float y0, float x1, float y1) {
  if (nav.empty() || docW <= 0.0f || docH <= 0.0f) return AtelierRect{};
  const auto mapX = [&](float x) {
    return nav.x + std::clamp(x / docW, 0.0f, 1.0f) * nav.w;
  };
  const auto mapY = [&](float y) {
    return nav.y + std::clamp(y / docH, 0.0f, 1.0f) * nav.h;
  };
  const float ax = mapX(std::min(x0, x1)), bx = mapX(std::max(x0, x1));
  const float ay = mapY(std::min(y0, y1)), by = mapY(std::max(y0, y1));
  return AtelierRect{ax, ay, bx - ax, by - ay};
}

}  // namespace np
