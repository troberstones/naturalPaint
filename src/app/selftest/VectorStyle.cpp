#include "app/selftest/Support.hpp"

#include "app/PenTool.hpp"
#include "app/VectorStyle.hpp"
#include "color/Space.hpp"
#include "ui/MacPaintUI.hpp"  // foregroundLinearRgba(), penVectorStyle()

namespace np {

namespace {

// A shape with a deliberately DISTINCT style in every field, so that "the
// readout took this shape's value" cannot be confused with "the readout took
// the default's". Every number here differs from `VectorStyle{}`'s and from
// every other fixture's.
VectorShape styledShape(uint64_t id, float width, std::array<float, 4> strokeRgba, bool strokeOn,
                        bool fillOn) {
  VectorShape s;
  s.id = id;
  SubPath sub;
  sub.closed = false;
  Anchor a;
  a.pt = PathPoint{static_cast<float>(id) * 10.0f, 0.0f};
  a.in = a.pt;
  a.out = a.pt;
  sub.anchors.push_back(a);
  Anchor b = a;
  b.pt = PathPoint{static_cast<float>(id) * 10.0f + 5.0f, 5.0f};
  b.in = b.pt;
  b.out = b.pt;
  sub.anchors.push_back(b);
  s.path.subpaths.push_back(std::move(sub));
  s.strokeStyle.width = width;
  s.stroke.on = strokeOn;
  s.stroke.rgba = strokeRgba;
  s.fill.on = fillOn;
  s.fill.rgba = {0.11f, 0.22f, 0.33f, 1.0f};
  return s;
}

}  // namespace

// app/VectorStyle -- the Pen's paint (docs/path-editing-plan.md section 2).
//
// Headless and GPU-free like every other section: no ImGui, no `AppState`
// beyond the one struct `penVectorStyle()` needs, building
// `std::vector<VectorShape>` directly and calling the same functions the
// options bar and the canvas gesture block call.
bool runVectorStyleTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; };

  // =======================================================================
  // 1. The defaults: stroke ON, fill OFF
  // =======================================================================
  //
  // This is the whole defect in one assertion. `Paint::on` defaults to false
  // on BOTH members of a `VectorShape` -- correct for that struct, since
  // SVG's `fill="none"` is a real state -- and `pathEditBeginPen()` used to
  // hand the rasteriser exactly that. `core/VectorRaster.cpp`'s two gates are
  // `shape.fill.on` and `shape.stroke.on && strokeStyle.width > 0`, so
  // neither branch ever ran and every pen-drawn path rasterised to nothing.
  {
    const VectorStyle def;
    check(def.stroke.on == true, "the default vector style has its STROKE on -- a pen is a line");
    check(def.fill.on == false,
          "...and its FILL off: filling an open path means implicitly closing it");
    check(near(def.strokeStyle.width, 1.0f),
          "the default stroke width is StrokeStyle's own 1.0, not a second invented answer");
    // The defect this replaces, asserted from the other side so that the
    // difference between the two is on the record rather than assumed.
    const VectorShape bare;
    check(bare.fill.on == false && bare.stroke.on == false,
          "a default-constructed VectorShape paints NEITHER -- what the Pen used to make");
  }

  // =======================================================================
  // 2. A placed shape carries the style
  // =======================================================================
  {
    VectorStyle style;
    style.strokeStyle.width = 7.25f;  // NOT 1.0 -- a forgotten strokeStyle would read 1.0
    style.stroke.on = true;
    style.stroke.rgba = {0.125f, 0.375f, 0.625f, 0.5f};
    style.fill.on = true;
    style.fill.rgba = {0.875f, 0.0625f, 0.25f, 0.75f};

    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState st;
    const PenPressResult r =
        pathEditBeginPen(&st, &shapes, &nextId, PathPoint{10, 20}, 4.0f, 1, /*curveMode=*/false, style);
    check(r == PenPressResult::Placed && shapes.size() == 1, "the first press places a shape");
    const bool made = shapes.size() == 1;
    check(made && shapes[0].stroke.on == true,
          "the placed shape's stroke is ON -- it is no longer invisible");
    check(made && near(shapes[0].strokeStyle.width, 7.25f),
          "the placed shape carries the style's stroke WIDTH, not StrokeStyle's default");
    check(made && shapes[0].stroke.rgba[0] == 0.125f && shapes[0].stroke.rgba[1] == 0.375f &&
              shapes[0].stroke.rgba[2] == 0.625f,
          "the placed shape's stroke colour is STRAIGHT -- alpha 0.5 does not scale the RGB");
    check(made && shapes[0].stroke.rgba[3] == 0.5f, "...and the alpha itself arrives intact");
    check(made && shapes[0].fill.on == true && shapes[0].fill.rgba[0] == 0.875f &&
              shapes[0].fill.rgba[3] == 0.75f,
          "the placed shape carries the style's fill, on-flag and alpha together");

    // Extending the same open path must not re-stamp or lose the style: the
    // second press appends an anchor to a shape that already has its paint.
    pathEditEnd(&st, shapes);
    pathEditBeginPen(&st, &shapes, &nextId, PathPoint{300, 400}, 4.0f, 1, /*curveMode=*/false, VectorStyle{});
    check(shapes.size() == 1 && near(shapes[0].strokeStyle.width, 7.25f),
          "extending an open path leaves the shape's own style alone (7.25, not the new 1.0)");
  }

  // =======================================================================
  // 3. Selection first, else default -- Shape mode
  // =======================================================================
  //
  // The fixture puts the shapes in an order that is NOT id order (ids 40, 10,
  // 25 down the list), which is the state a reordered layer is in. A target
  // resolution that binary-searched the layer-ordered list, or that reported
  // the selection in click order, would agree with a sorted fixture and
  // disagree with this one.
  {
    std::vector<VectorShape> shapes;
    shapes.push_back(styledShape(40, 2.0f, {1.0f, 0.0f, 0.0f, 1.0f}, true, false));
    shapes.push_back(styledShape(10, 3.0f, {0.0f, 1.0f, 0.0f, 1.0f}, true, false));
    shapes.push_back(styledShape(25, 4.0f, {0.0f, 0.0f, 1.0f, 1.0f}, false, true));

    VectorStyle def;
    def.strokeStyle.width = 99.0f;  // distinct from every shape's

    // -- no selection: the default is written, the document is not ---------
    {
      std::vector<VectorShape> work = shapes;
      VectorStyle d = def;
      PathSelection sel;  // Shape mode, empty
      const size_t n = applyVectorStyleEdit(&work, sel, &d, [](VectorStyle& s) {
        s.strokeStyle.width = 12.0f;
      });
      check(n == 0, "with nothing selected the edit touches zero shapes");
      check(near(d.strokeStyle.width, 12.0f), "...and writes the TOOL DEFAULT instead");
      check(near(work[0].strokeStyle.width, 2.0f) && near(work[1].strokeStyle.width, 3.0f) &&
                near(work[2].strokeStyle.width, 4.0f),
            "...leaving every shape exactly as it was");
    }

    // -- one shape selected: that shape, and NOT the default ---------------
    {
      std::vector<VectorShape> work = shapes;
      VectorStyle d = def;
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {10};
      const size_t n = applyVectorStyleEdit(&work, sel, &d, [](VectorStyle& s) {
        s.strokeStyle.width = 12.0f;
      });
      check(n == 1, "one selected shape means one shape changed");
      check(near(work[1].strokeStyle.width, 12.0f), "...the selected one");
      check(near(work[0].strokeStyle.width, 2.0f) && near(work[2].strokeStyle.width, 4.0f),
            "...and only it");
      check(near(d.strokeStyle.width, 99.0f),
            "the tool default is UNTOUCHED while a selection exists");
    }

    // -- two shapes selected -----------------------------------------------
    {
      std::vector<VectorShape> work = shapes;
      VectorStyle d = def;
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {25, 40};
      const size_t n = applyVectorStyleEdit(&work, sel, &d, [](VectorStyle& s) {
        s.strokeStyle.width = 12.0f;
      });
      check(n == 2, "two selected shapes means two shapes changed");
      check(near(work[0].strokeStyle.width, 12.0f) && near(work[2].strokeStyle.width, 12.0f) &&
                near(work[1].strokeStyle.width, 3.0f),
            "...exactly those two, across the layer's own order");
    }

    // -- Component mode: anchors resolve to their shapes --------------------
    {
      std::vector<VectorShape> work = shapes;
      VectorStyle d = def;
      PathSelection sel;
      sel.mode = PathSelectMode::Component;
      ComponentRef c;
      c.shapeId = 25;
      c.subPath = 0;
      c.anchor = 1;
      c.part = AnchorPart::OutHandle;
      sel.components.push_back(c);
      c.anchor = 0;
      c.part = AnchorPart::Point;
      sel.components.push_back(c);  // the SAME shape, twice
      const size_t n = applyVectorStyleEdit(&work, sel, &d, [](VectorStyle& s) {
        s.strokeStyle.width = 12.0f;
      });
      check(n == 1, "two anchors of one shape resolve to ONE shape, not two");
      check(near(work[2].strokeStyle.width, 12.0f), "...and it is the shape they sit on");
      check(near(d.strokeStyle.width, 99.0f),
            "a component selection also counts as a selection: the default stays put");
    }

    // -- a stale selection naming a shape the layer no longer holds ---------
    //
    // A shape deleted from under a selection. Treating its id as a target
    // would make the row edit nothing while claiming to have edited, and the
    // readout show a style nothing has.
    {
      std::vector<VectorShape> work = shapes;
      VectorStyle d = def;
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {777};
      const size_t n = applyVectorStyleEdit(&work, sel, &d, [](VectorStyle& s) {
        s.strokeStyle.width = 12.0f;
      });
      check(n == 0, "a selection naming only ids the layer does not hold targets nothing");
      check(near(d.strokeStyle.width, 12.0f), "...and falls back to the default, per the rule");
    }

    // -- target ORDER is layer order, not selection order -------------------
    {
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {25, 10, 40};  // deliberately neither layer nor id order
      const std::vector<uint64_t> t = vectorStyleTargets(shapes, sel);
      check(t.size() == 3 && t[0] == 40 && t[1] == 10 && t[2] == 25,
            "targets come back in LAYER order (40, 10, 25), not id or click order");
    }
  }

  // =======================================================================
  // 4. The readout shows the SELECTION, and says when it is mixed
  // =======================================================================
  {
    std::vector<VectorShape> shapes;
    shapes.push_back(styledShape(40, 2.0f, {1.0f, 0.0f, 0.0f, 1.0f}, true, false));
    shapes.push_back(styledShape(10, 3.0f, {0.0f, 1.0f, 0.0f, 1.0f}, true, false));
    VectorStyle def;
    def.strokeStyle.width = 99.0f;
    def.stroke.rgba = {0.5f, 0.5f, 0.5f, 1.0f};

    {
      PathSelection sel;  // empty
      const VectorStyleReadout r = vectorStyleReadout(shapes, sel, def);
      check(!r.fromSelection && r.targetCount == 0 && near(r.style.strokeStyle.width, 99.0f),
            "with nothing selected the row reads the TOOL DEFAULT");
      check(!r.mixedStrokeWidth && !r.mixedStrokeColor && !r.mixedFillOn,
            "...and nothing is mixed");
    }
    {
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {10};
      const VectorStyleReadout r = vectorStyleReadout(shapes, sel, def);
      check(r.fromSelection && r.targetCount == 1 && near(r.style.strokeStyle.width, 3.0f),
            "with one shape selected the row reads THAT SHAPE (3.0), not the default (99.0)");
      check(r.style.stroke.rgba[1] == 1.0f && r.style.stroke.rgba[0] == 0.0f,
            "...its colour too");
      check(!r.mixedStrokeWidth, "one shape is never mixed");
    }
    {
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {10, 40};
      const VectorStyleReadout r = vectorStyleReadout(shapes, sel, def);
      check(r.fromSelection && r.targetCount == 2, "two shapes selected");
      check(near(r.style.strokeStyle.width, 2.0f),
            "the readout shows the FIRST in layer order (shape 40's 2.0)");
      check(r.mixedStrokeWidth, "two different widths report MIXED");
      check(r.mixedStrokeColor, "two different stroke colours report MIXED");
      check(!r.mixedStrokeOn && !r.mixedFillOn,
            "...while the flags they agree on do NOT report mixed");
    }
    {
      // Two shapes that agree about everything: mixed must be false, or the
      // affordance is on permanently and says nothing.
      std::vector<VectorShape> same;
      same.push_back(styledShape(1, 5.0f, {0.25f, 0.5f, 0.75f, 1.0f}, true, true));
      same.push_back(styledShape(2, 5.0f, {0.25f, 0.5f, 0.75f, 1.0f}, true, true));
      PathSelection sel;
      sel.mode = PathSelectMode::Shape;
      sel.shapes = {1, 2};
      const VectorStyleReadout r = vectorStyleReadout(same, sel, def);
      check(r.targetCount == 2 && !r.mixedStrokeWidth && !r.mixedStrokeColor &&
                !r.mixedStrokeOn && !r.mixedFillOn && !r.mixedFillColor,
            "two shapes that agree report nothing mixed");
    }
  }

  // =======================================================================
  // 5. The colour path is LINEAR, not sRGB
  // =======================================================================
  //
  // `Paint::rgba` is linear-light straight alpha (core/VectorShape.hpp
  // section 1). `penVectorStyle()` takes the foreground through
  // `foregroundLinearRgba()`, which decodes; assigning the ENCODED triple
  // instead is a one-line slip that makes every pen stroke read about twice
  // as dark as the swatch promising it, and which nothing on screen labels.
  // The fixture avoids 0.0 and 1.0 on purpose: those are the two values where
  // the encode and the decode agree, so a fixture built from them would pass
  // under the sabotage.
  {
    BrushState brush;
    brush.colorMode = ColorMode::Rgb;
    brush.rgb = {0.5f, 0.25f, 0.75f};

    const std::array<float, 4> fg = foregroundLinearRgba(brush);
    check(near(fg[0], srgbDecode(0.5f)) && near(fg[1], srgbDecode(0.25f)) &&
              near(fg[2], srgbDecode(0.75f)),
          "foregroundLinearRgba() DECODES the foreground -- the swatch is sRGB, Paint is linear");
    check(!near(fg[0], 0.5f, 0.05f),
          "...and the decode is not a no-op: 0.5 encoded is nowhere near 0.5 linear");

    AppState st;
    st.brush = brush;
    st.vectorStyle.strokeStyle.width = 6.5f;
    st.vectorStyle.fill.on = true;
    const VectorStyle made = penVectorStyle(st);
    check(near(made.stroke.rgba[0], fg[0]) && near(made.stroke.rgba[1], fg[1]) &&
              near(made.stroke.rgba[2], fg[2]),
          "a new pen shape's STROKE takes the linear foreground");
    check(near(made.fill.rgba[0], fg[0]) && near(made.fill.rgba[2], fg[2]),
          "...and so does its FILL, for the one-foreground-colour rule");
    check(near(made.strokeStyle.width, 6.5f) && made.fill.on == true && made.stroke.on == true,
          "penVectorStyle() keeps the tool default's width and on-flags -- only colour is "
          "overridden");

    // Alpha survives: `foregroundLinearRgba()` always answers 1.0 (the
    // foreground well has no opacity of its own), so a translucent stroke
    // dialled on the options bar must not be flattened back to opaque.
    st.vectorStyle.stroke.rgba[3] = 0.4f;
    const VectorStyle translucent = penVectorStyle(st);
    check(near(translucent.stroke.rgba[3], 0.4f),
          "the options bar's stroke ALPHA survives the foreground overwrite");

    // End to end: the colour that reaches the shape is the linear one.
    std::vector<VectorShape> shapes;
    uint64_t nextId = 1;
    PathEditState pe;
    pathEditBeginPen(&pe, &shapes, &nextId, PathPoint{1, 1}, 4.0f, 1, false, made);
    check(shapes.size() == 1 && near(shapes[0].stroke.rgba[0], srgbDecode(0.5f)),
          "and the shape the Pen actually creates holds the LINEAR value");
  }

  return ok;
}

}  // namespace np
