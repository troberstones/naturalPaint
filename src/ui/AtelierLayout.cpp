#include "ui/AtelierLayout.hpp"

#include <algorithm>

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

}  // namespace np
