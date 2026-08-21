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

AtelierBands atelierLayout(float x, float y, float w, float h, bool showTabStrip) {
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

  b.optionsBar = AtelierRect{x, cy, w, kOptionsBarH};
  cy += kOptionsBarH;
  addRule(hRule(x, w, cy));

  // The status bar is placed from the bottom, so every rounding error in the
  // bands above lands in the canvas rather than in a 26 px bar that would
  // then not sit flush with the window edge.
  const float statusTop = y + h - kStatusBarH;
  b.statusBar = AtelierRect{x, statusTop, w, kStatusBarH};
  const AtelierRect statusRule{x, statusTop - kRuleThickness, w, kRuleThickness};

  // The middle row: palette | rule | canvas | rule | column.
  const float midTop = cy;
  const float midH = std::max(0.0f, statusRule.y - midTop);

  b.toolPalette = AtelierRect{x, midTop, kToolPaletteW, midH};
  const AtelierRect leftRule{x + kToolPaletteW, midTop, kRuleThickness, midH};

  const float rightX = x + w - kRightColumnW;
  b.rightColumn = AtelierRect{rightX, midTop, kRightColumnW, midH};
  const AtelierRect rightRule{rightX - kRuleThickness, midTop, kRuleThickness, midH};

  const float canvasX = leftRule.right();
  b.canvas = AtelierRect{canvasX, midTop, std::max(0.0f, rightRule.x - canvasX), midH};

  addRule(leftRule);
  addRule(rightRule);
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
