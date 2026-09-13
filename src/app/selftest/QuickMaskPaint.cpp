#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/DocumentLifecycle.hpp"
#include "brush/QuickMaskPaint.hpp"
#include "color/Space.hpp"
#include "core/SelectionOps.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {
namespace {

// A one-layer RGB document with known pixel content, the same shape
// app/selftest/CommandsOpStack.cpp's `makeOpStackDocument()` uses -- what
// matters here is a Tile this file can snapshot and diff, not what colour it
// holds.
OpenDocument makeQuickMaskFixture() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "quickmask");
  od.document.layers[0].name = "Base";
  Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
  for (int32_t y = 0; y < kTileSize; ++y)
    for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, {0.2f, 0.4f, 0.6f, 1.0f});
  od.recordEdit("quick mask fixture", EditKind::Content);
  return od;
}

// A hard disc, so `dabCoverage()` is exactly 1.0 over the whole core and every
// number below is about the quick-mask arithmetic rather than about the
// falloff -- brush/MaskPaint's own selftest (app/selftest/MaskTarget.cpp)
// idiom, copied rather than reinvented.
BrushTip discTip(float radius, float flow, float opacity) {
  BrushTip t;
  t.radius = radius;
  t.hardness = 1.0f;
  t.flow = flow;
  t.opacity = opacity;
  return t;
}

}  // namespace

// PRD E12: quick mask, the app-level half. core/Channels.hpp's QuickMask
// engine (quickMaskFromSelection/selectionFromQuickMask/paintQuickMask) is
// already proven texel-for-texel by app/selftest/Channels.cpp -- this section
// does not repeat that. What it proves is the wiring on top: `toggleQuickMask()`
// (ui/MacPaintUI.hpp), and brush/QuickMaskPaint's dab arithmetic that a real
// stroke would feed it.
bool runQuickMaskPaintTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  // core/SelectionMask.hpp's own store is uint8, so a written coverage of
  // exactly 0.0 or 1.0 round-trips bit-exact but anything in between is one
  // quantization step (1/255) from the value that was asked for.
  constexpr float kQuantStep = 1.0f / 255.0f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  std::printf("[selftest] quick mask paint: toggleQuickMask() and brush/QuickMaskPaint's dab "
              "arithmetic\n");

  // --- A. Q twice, with nothing painted in between, round-trips exactly ---
  {
    OpenDocument od = makeQuickMaskFixture();
    od.selection = selectRectangle(4.0f, 4.0f, 20.0f, 20.0f);
    const Selection original = *od.selection;

    toggleQuickMask(od);
    check(od.quickMask.has_value() && !od.selection.has_value(),
          "quick mask: entering engages the mask and suspends the active selection");

    toggleQuickMask(od);
    check(!od.quickMask.has_value(), "quick mask: leaving disengages the mask");
    bool sameEverywhere = od.selection.has_value();
    for (int32_t y = 0; sameEverywhere && y < 64; ++y)
      for (int32_t x = 0; x < 64; ++x)
        if (selectionCoverageAt(&*od.selection, PixelCoord{x, y}) !=
            selectionCoverageAt(&original, PixelCoord{x, y})) {
          sameEverywhere = false;
          break;
        }
    check(sameEverywhere,
          "quick mask: Q twice with nothing painted round-trips the selection texel for texel");
  }

  // --- B. Entering with no selection gives an empty, engaged mask ---------
  {
    OpenDocument od = makeQuickMaskFixture();
    toggleQuickMask(od);
    check(od.quickMask.has_value() && quickMaskCoverageAt(*od.quickMask, PixelCoord{5, 5}) == 0.0f,
          "quick mask: entering with no selection gives a blank overlay, not a full one");
    toggleQuickMask(od);
    check(!od.selection.has_value(),
          "quick mask: leaving an untouched blank mask gives no selection, not an "
          "engaged-but-empty one");
  }

  // --- C. A brush dab adds coverage and leaves the layer bit-identical ----
  {
    OpenDocument od = makeQuickMaskFixture();
    const Tile before = *od.document.layers[0].rgbTiles->find(TileCoord{0, 0});

    toggleQuickMask(od);
    const BrushTip tip = discTip(6.0f, 1.0f, 1.0f);
    const size_t written =
        paintQuickMaskDab(*od.quickMask, tip, Vec2{16.0f, 16.0f}, 64, 64, /*erase=*/false);
    check(written > 0, "quick mask: a brush dab reports texels written");
    check(quickMaskCoverageAt(*od.quickMask, PixelCoord{16, 16}) == 1.0f,
          "quick mask: a full-flow, full-opacity dab centre reaches full coverage");

    const Tile* after = od.document.layers[0].rgbTiles->find(TileCoord{0, 0});
    check(after != nullptr &&
              std::memcmp(before.data(), after->data(),
                          Tile::kTexelCount * sizeof(uint16_t)) == 0,
          "quick mask: the stroke that painted the mask left the layer's own tile "
          "bit-identical");
  }

  // --- D. Opacity is a real ceiling, not merely a rate limiter ------------
  //
  // brush/RgbErase.hpp's own argument (core/Channels.hpp's brush/MaskPaint
  // cross-reference): gating only the per-dab RATE still converges to full
  // coverage given enough dabs. `max()`/`min()` do not have that failure
  // mode, and this is the assertion that would catch it if a future edit
  // multiplied opacity in some OTHER way that did.
  {
    OpenDocument od = makeQuickMaskFixture();
    toggleQuickMask(od);
    const BrushTip half = discTip(6.0f, 1.0f, 0.4f);
    for (int i = 0; i < 50; ++i)
      paintQuickMaskDab(*od.quickMask, half, Vec2{16.0f, 16.0f}, 64, 64, /*erase=*/false);
    check(near(quickMaskCoverageAt(*od.quickMask, PixelCoord{16, 16}), 0.4f, kQuantStep),
          "quick mask: fifty overlapping dabs at 40% opacity still cap at ~0.4, not 1.0");
  }

  // --- E. The eraser removes coverage the brush laid down -----------------
  {
    OpenDocument od = makeQuickMaskFixture();
    toggleQuickMask(od);
    const BrushTip full = discTip(6.0f, 1.0f, 1.0f);
    paintQuickMaskDab(*od.quickMask, full, Vec2{16.0f, 16.0f}, 64, 64, /*erase=*/false);
    const float painted = quickMaskCoverageAt(*od.quickMask, PixelCoord{16, 16});
    check(painted == 1.0f, "quick mask: fixture for E painted full coverage first");

    const size_t erased =
        paintQuickMaskDab(*od.quickMask, discTip(6.0f, 0.5f, 1.0f), Vec2{16.0f, 16.0f}, 64, 64,
                          /*erase=*/true);
    const float afterErase = quickMaskCoverageAt(*od.quickMask, PixelCoord{16, 16});
    check(erased > 0 && afterErase < painted,
          "quick mask: an eraser dab over painted coverage reduces it");
    check(near(afterErase, 0.5f, kQuantStep),
          "quick mask: Subtract's min(1-flow) rule -- half-flow erases to ~0.5 here");
  }

  return ok;
}

}  // namespace np
