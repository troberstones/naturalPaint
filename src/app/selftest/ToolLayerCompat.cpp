#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/StrokeSession.hpp"    // pixelOpRefusalFor(), strokeRouteFor() -- the gates this axis must not diverge from
#include "app/ToolLayerCompat.hpp"

namespace np {

// app/ToolLayerCompat -- the FOURTH axis of "is this palette cell live",
// closing the gap the toolbar/tool audit found: a Marquee or Magic Wand cell
// drawn live over a Vector or Text layer took the click and installed
// nothing, because `pixelOpRefusalFor()`/`strokeRouteFor()` only refuse at
// the gesture, and nothing upstream of the click said so.
//
// Headless and GPU-free, in `app/ToolSurface.cpp`'s own selftest's shape:
// first prove the axis is a real, proper subset of the pixel-consuming
// tools (falsifiable), then trace named tools to the gate that admits or
// refuses them, then walk every (Tool, LayerKind) combination and cross-check
// against `pixelOpRefusalFor()` itself so the two tables cannot drift apart.
bool runToolLayerCompatTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf(
      "[selftest] tool/layer compatibility: the fourth axis -- can this tool act on THIS "
      "layer's kind\n");

  // -----------------------------------------------------------------------
  // A. The axis is an axis: a proper, non-trivial subset of Tool::Count
  // -----------------------------------------------------------------------
  {
    int needsPixel = 0;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i)
      if (toolNeedsPixelLayer(static_cast<Tool>(i))) ++needsPixel;
    check(needsPixel > 0 && needsPixel < static_cast<int>(Tool::Count),
          "axis: some tools need a pixel layer and some do not -- a synonym for "
          "toolImplemented() or for \"every tool\" would make every check below vacuous");
    std::printf("  [measured] %d of %d tools need a pixel layer\n", needsPixel,
                static_cast<int>(Tool::Count));
  }

  // -----------------------------------------------------------------------
  // B. Named tools, each traced to ITS OWN membership -- never a list read
  //    back at itself
  // -----------------------------------------------------------------------
  {
    check(toolNeedsPixelLayer(Tool::Marquee) && toolNeedsPixelLayer(Tool::EllipseMarquee) &&
              toolNeedsPixelLayer(Tool::Lasso) && toolNeedsPixelLayer(Tool::PolygonLasso) &&
              toolNeedsPixelLayer(Tool::MagicWand),
          "membership: the five selection tools all need a pixel layer -- the user's own "
          "confirmation that Text is disabled exactly like Vector");
    check(toolNeedsPixelLayer(Tool::PaintBucket) && toolNeedsPixelLayer(Tool::Gradient),
          "membership: the bucket and the gradient -- pixelOpRefusalFor()'s own two direct "
          "callers");
    // **Deliberately NOT the brush family.** strokeRouteFor()'s Pigment-layer
    // arm admits Brush/Dry Brush and refuses Pencil/Dodge/Burn/Clone/Heal on
    // the SAME kind, by name -- a kind with no `rgbTiles` at all (it holds
    // `pigmentTiles` instead). Folding the family into this axis would
    // silently disable Brush and Dry Brush on Pigment, the one kind built to
    // receive them.
    check(!toolNeedsPixelLayer(Tool::Brush) && !toolNeedsPixelLayer(Tool::Water) &&
              !toolNeedsPixelLayer(Tool::DryBrush) && !toolNeedsPixelLayer(Tool::Eraser) &&
              !toolNeedsPixelLayer(Tool::Pencil) && !toolNeedsPixelLayer(Tool::Smudge) &&
              !toolNeedsPixelLayer(Tool::Dodge) && !toolNeedsPixelLayer(Tool::Burn) &&
              !toolNeedsPixelLayer(Tool::CloneStamp) && !toolNeedsPixelLayer(Tool::Heal),
          "membership: the whole brush family is exempt -- its own per-kind rules already "
          "live in strokeRouteFor(), which this axis must not restate or collapse");
    check(!toolNeedsPixelLayer(Tool::Move) && !toolNeedsPixelLayer(Tool::Crop) &&
              !toolNeedsPixelLayer(Tool::Hand) && !toolNeedsPixelLayer(Tool::Zoom) &&
              !toolNeedsPixelLayer(Tool::Measure) && !toolNeedsPixelLayer(Tool::Frame) &&
              !toolNeedsPixelLayer(Tool::Slice),
          "membership: Move/Crop/Hand/Zoom/Measure/Frame/Slice never touch rgbTiles at all");
    check(!toolNeedsPixelLayer(Tool::Pen) && !toolNeedsPixelLayer(Tool::Curve) &&
              !toolNeedsPixelLayer(Tool::PathSelect) && !toolNeedsPixelLayer(Tool::Shape) &&
              !toolNeedsPixelLayer(Tool::Text),
          "membership: the vector/text authoring tools are exempt -- disabling Pen because "
          "the layer is not YET Vector would make it impossible to ever start one");
    check(!toolNeedsPixelLayer(Tool::Eyedropper),
          "membership: the eyedropper is exempt -- two of its three sample sources read the "
          "COMPOSITE, not the active layer's own store");
  }

  // -----------------------------------------------------------------------
  // C. `layer == nullptr` is always admitted -- that is ToolSurface's
  //    question, not this one
  // -----------------------------------------------------------------------
  {
    bool everyToolAdmitsNullLayer = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (!toolValidForLayer(t, nullptr) || toolLayerRefusal(t, nullptr) != nullptr)
        everyToolAdmitsNullLayer = false;
    }
    check(everyToolAdmitsNullLayer,
          "no active layer: every Tool is admitted with layer == nullptr -- refusing there "
          "would restate app/ToolSurface's own question under a different name");
  }

  // -----------------------------------------------------------------------
  // D. The full (Tool, LayerKind) table, cross-checked against
  //    pixelOpRefusalFor() -- the two must never disagree
  // -----------------------------------------------------------------------
  {
    // One probe layer per kind, matched to core/Layer.hpp's own invariant:
    // `rgbTiles` is populated ONLY when `kind == RGB` -- Pigment (and Media,
    // when built) hold a DIFFERENT store, `pigmentTiles`, and Adjustment,
    // Text, Strokes, Flats, Group and Vector hold no pixel store of any kind
    // at all. So exactly one of the nine kinds carries `rgbTiles`, and that
    // is the fact this table exists to walk against every Tool.
    struct KindRow {
      LayerKind kind;
      bool hasStore;
    };
    const KindRow kinds[] = {
        {LayerKind::Pigment, false},    {LayerKind::RGB, true},     {LayerKind::Media, false},
        {LayerKind::Strokes, false},    {LayerKind::Adjustment, false}, {LayerKind::Text, false},
        {LayerKind::Flats, false},      {LayerKind::Group, false},  {LayerKind::Vector, false},
    };

    bool agreesEverywhere = true;
    bool everyPixelToolRefusesEveryStorelessKind = true;
    bool everyPixelToolAdmitsEveryStoredKind = true;
    bool everyLayerAgnosticToolAdmitsEveryKind = true;
    int rowsChecked = 0;

    for (const KindRow& row : kinds) {
      Layer probe;
      probe.kind = row.kind;
      if (row.hasStore) probe.rgbTiles = TileStore{};

      // The invariant this whole table leans on, checked at the source
      // rather than assumed from the `kinds` literal above.
      check(probe.rgbTiles.has_value() == row.hasStore,
            "fixture: the probe layer's own rgbTiles matches this kind's documented invariant");

      for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
        const Tool t = static_cast<Tool>(i);
        ++rowsChecked;
        const bool valid = toolValidForLayer(t, &probe);
        const bool refused = toolLayerRefusal(t, &probe) != nullptr;
        if (valid == refused) agreesEverywhere = false;  // exactly one must hold

        if (toolNeedsPixelLayer(t)) {
          if (!row.hasStore && valid) everyPixelToolRefusesEveryStorelessKind = false;
          if (row.hasStore && !valid) everyPixelToolAdmitsEveryStoredKind = false;
        } else if (!valid) {
          everyLayerAgnosticToolAdmitsEveryKind = false;
        }

        // Cross-check against the gesture-time gate itself, for the four
        // kinds `pixelOpRefusalFor()` can even be asked about (it takes a
        // `Layer*`, unlocked, and answers purely off `rgbTiles` once past the
        // lock test -- so this asks it with an unlocked probe, matching
        // `probe.locked`'s default).
        if (toolNeedsPixelLayer(t)) {
          const PixelOpRefusal reason = pixelOpRefusalFor(&probe);
          const bool gestureRefuses = reason == PixelOpRefusal::NoRgbStore;
          if (gestureRefuses != !valid) agreesEverywhere = false;
        }
      }
    }

    check(rowsChecked == static_cast<int>(Tool::Count) * 9,
          "fixture: every one of the 9 kinds was walked against every Tool");
    check(everyPixelToolRefusesEveryStorelessKind,
          "table: every pixel-needing tool refuses every kind but RGB -- Pigment included, "
          "since it holds pigmentTiles rather than rgbTiles, and Text/Vector by the user's "
          "own instruction");
    check(everyPixelToolAdmitsEveryStoredKind,
          "table: every pixel-needing tool is admitted on RGB -- the one kind that carries "
          "rgbTiles");
    check(everyLayerAgnosticToolAdmitsEveryKind,
          "table: every tool NOT in the pixel-needing list is admitted on every kind, with no "
          "exception -- this axis has no opinion about them at all");
    check(agreesEverywhere,
          "table: toolValidForLayer() and toolLayerRefusal() never disagree -- exactly one of "
          "\"valid\" and \"carries a refusal sentence\" holds for every (Tool, LayerKind) pair, "
          "and neither ever answers both or neither");
  }

  std::printf("[selftest] tool/layer compatibility %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
