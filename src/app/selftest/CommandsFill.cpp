#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/Command.hpp"
#include "app/CommandsFill.hpp"
#include "app/PixelOpBridge.hpp"
#include "core/Blend.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "ops/Fill.hpp"
#include "ops/Pattern.hpp"

// app/selftest/CommandsFill -- PRD D26: `fill` and `stroke`, through
// `ops/Fill`'s one engine and the `fill`/`stroke` command rows.
//
// Sections, in the order reach-fill.md asks for them: inside/outside a
// selection and its antialiased edge (B), no-selection whole-layer fill (A),
// blend and opacity arithmetic (C), a gradient's endpoint colours and angle
// (D), a pattern's tiling origin (E), stroke width and Inside/Center/Outside
// on a known rectangle (F), the refusals (G), and a recorded `fill` replaying
// to the same texels as the direct call (H).
//
// **What this section does not re-derive.** `core/SelectionRefine.hpp`'s
// `growSelection()`/`shrinkSelection()` already have their own exhaustive
// selftest of the distance-transform arithmetic; section F uses them only to
// state the EXPECTED band at a few probe points on a hard-edged rectangle,
// where an integer grow/shrink is documented to keep a hard edge hard
// (`core/SelectionRefine.hpp` §2) -- so the expectation is a plain rectangle
// comparison, not a second copy of the distance transform.
namespace np {
namespace {

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25
bool nearHalf(float got, float want) {
  return std::fabs(got - want) <= std::fabs(want) * kHalfRel + kHalfFloor;
}

std::array<float, 4> readAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

// A small pattern this file controls end to end: a 4x4 chequer, red at (0,0)
// and every texel whose (x+y) is even, green otherwise. Built by hand rather
// than through `definePattern()` so the tiling-origin claim in section E is
// about `patternSourceTexel()`'s own modulus, not about the extraction that
// feeds it.
Pattern makeCheckerPattern() {
  Pattern p;
  p.id = "checker";
  p.name = "checker";
  p.width = 4;
  p.height = 4;
  p.px.assign(p.sampleCount(), 0.0f);
  for (uint32_t y = 0; y < p.height; ++y)
    for (uint32_t x = 0; x < p.width; ++x) {
      float* d = p.px.data() + (static_cast<size_t>(y) * p.width + x) * 4u;
      const bool red = ((x + y) % 2u) == 0u;
      d[0] = red ? 1.0f : 0.0f;
      d[1] = red ? 0.0f : 1.0f;
      d[2] = 0.0f;
      d[3] = 1.0f;
    }
  return p;
}

}  // namespace

bool runCommandsFillTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
    return cond;
  };

  constexpr int32_t kW = 128, kH = 128;
  const std::array<float, 3> kRed{1.0f, 0.0f, 0.0f};
  const std::array<float, 3> kBlue{0.0f, 0.0f, 1.0f};

  std::printf("  -- A. no selection fills the whole layer --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill A");
    FillParams p;
    p.source = FillSource::Color;
    p.color = kRed;
    const FilterOpResult r = applyPixelFilter(od, fillTiles, p, "fill");
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged == static_cast<size_t>(kW) * kH,
          "fill: no selection changes every texel of a blank layer");
    check(readAt(*od.document.layers[0].rgbTiles, 0, 0) == std::array<float, 4>{1, 0, 0, 1} &&
              readAt(*od.document.layers[0].rgbTiles, kW - 1, kH - 1) ==
                  std::array<float, 4>{1, 0, 0, 1},
          "fill: the stored texel is the colour, premultiplied at alpha 1 -- corner to corner");
  }

  std::printf("  -- B. a selection bounds the fill, antialiased at its edge --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill B");
    // A hard rectangle plus one FRACTIONAL edge (x1 = 64.5), so one column of
    // texels is genuinely half-covered rather than everything landing on a
    // texel boundary.
    od.selection = selectRectangle(32.0f, 32.0f, 64.5f, 96.0f);
    FillParams p;
    p.source = FillSource::Color;
    p.color = kRed;
    const FilterOpResult r = applyPixelFilter(od, fillTiles, p, "fill");
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0, "fill: a bounded fill runs");
    const TileStore& store = *od.document.layers[0].rgbTiles;
    check(readAt(store, 40, 60) == std::array<float, 4>{1, 0, 0, 1},
          "fill: well inside the rectangle is fully filled");
    check(readAt(store, 10, 10) == std::array<float, 4>{0, 0, 0, 0} &&
              readAt(store, 90, 60) == std::array<float, 4>{0, 0, 0, 0},
          "fill: outside the rectangle is untouched, exactly");
    // Texel 64 spans [64, 65); the rectangle's right edge at 64.5 covers
    // exactly its left half, so `selectionCoverageAt()` reports 0.5 there and
    // the composite is the half-way lerp between "untouched" (all zero) and
    // "fully filled" -- i.e. half the fill colour's own premultiplied value.
    const std::array<float, 4> edge = readAt(store, 64, 60);
    check(nearHalf(edge[0], 0.5f) && nearHalf(edge[1], 0.0f) && nearHalf(edge[2], 0.0f) &&
              nearHalf(edge[3], 0.5f),
          "fill: the fractional edge texel is the coverage-weighted lerp, not a hard cut");
  }

  std::printf("  -- C. blend mode and opacity arithmetic --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill C");
    // A known opaque backdrop (green) to blend the fill against -- Multiply
    // against transparent black would be indistinguishable from Normal.
    {
      FillParams backdrop;
      backdrop.source = FillSource::Color;
      backdrop.color = {0.0f, 1.0f, 0.0f};
      applyPixelFilter(od, fillTiles, backdrop, "fill");
    }
    FillParams p;
    p.source = FillSource::Color;
    p.color = kRed;
    p.blend = BlendMode::Multiply;
    p.opacity = 0.5f;
    const FilterOpResult r = applyPixelFilter(od, fillTiles, p, "fill");
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "fill: a Multiply fill at half opacity runs");
    // The independently-computed expectation: the source is {1,0,0} at
    // opacity 0.5, premultiplied to {0.5, 0, 0, 0.5}; `core/Blend.hpp`'s own
    // `blendPixel()` -- the identical function `fillTiles()` calls -- blended
    // against the opaque green backdrop, at full selection coverage (there is
    // none, so coverage is 1 and the composite writes the engine's own
    // value verbatim).
    const std::array<float, 4> want =
        blendPixel(BlendMode::Multiply, {0.5f, 0.0f, 0.0f, 0.5f}, {0.0f, 1.0f, 0.0f, 1.0f});
    const std::array<float, 4> got = readAt(*od.document.layers[0].rgbTiles, 64, 64);
    check(nearHalf(got[0], want[0]) && nearHalf(got[1], want[1]) && nearHalf(got[2], want[2]) &&
              nearHalf(got[3], want[3]),
          "fill: the stored texel matches core/Blend's own blendPixel() at the same arguments");
  }

  std::printf("  -- D. a gradient's endpoint colours and angle --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill D");
    FillParams p;
    p.source = FillSource::Gradient;
    p.gradientGeometry.kind = GradientKind::Linear;
    p.gradientGeometry.x0 = 0.0f;
    p.gradientGeometry.y0 = 64.0f;
    p.gradientGeometry.x1 = 127.0f;
    p.gradientGeometry.y1 = 64.0f;
    ColorStop a, b;
    a.position = 0.0f;
    a.color = kRed;
    b.position = 1.0f;
    b.color = kBlue;
    p.gradientStops.colorStops = {a, b};
    const FilterOpResult r = applyPixelFilter(od, fillTiles, p, "fill");
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0, "fill: a gradient fill runs");
    const TileStore& store = *od.document.layers[0].rgbTiles;
    check(nearHalf(readAt(store, 0, 64)[0], 1.0f) && nearHalf(readAt(store, 0, 64)[2], 0.0f),
          "fill: the gradient's start endpoint is stop A's colour");
    check(nearHalf(readAt(store, 127, 64)[2], 1.0f) && nearHalf(readAt(store, 127, 64)[0], 0.0f),
          "fill: the gradient's end endpoint is stop B's colour");
    // A vertical gradient (angle rotated 90 degrees from the horizontal one
    // above) is constant along x and varies along y -- the ramp actually
    // follows the geometry rather than always running left to right.
    OpenDocument odV = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill D vertical");
    FillParams pv = p;
    pv.gradientGeometry.x0 = 64.0f;
    pv.gradientGeometry.y0 = 0.0f;
    pv.gradientGeometry.x1 = 64.0f;
    pv.gradientGeometry.y1 = 127.0f;
    applyPixelFilter(odV, fillTiles, pv, "fill");
    const TileStore& storeV = *odV.document.layers[0].rgbTiles;
    check(readAt(storeV, 10, 5) == readAt(storeV, 100, 5),
          "fill: a vertical gradient is constant across a row -- the angle, not just the "
          "endpoint colours, is honoured");
    check(!(readAt(storeV, 64, 5) == readAt(storeV, 64, 120)),
          "fill: and it varies down a column -- so the two checks above are not both "
          "trivially true of a solid fill");
  }

  std::printf("  -- E. a pattern's tiling origin --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill E");
    const Pattern checker = makeCheckerPattern();
    FillParams p;
    p.source = FillSource::Pattern;
    p.pattern = &checker;
    p.patternOriginX = 10;
    p.patternOriginY = 10;
    const FilterOpResult r = applyPixelFilter(od, fillTiles, p, "fill");
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0, "fill: a pattern fill runs");
    const TileStore& store = *od.document.layers[0].rgbTiles;
    // The origin lands pattern texel (0,0) -- red -- at document (10,10), and
    // every 4-texel step from there repeats it exactly (ops/Pattern.hpp
    // section 3's own seam assertion, one origin over).
    check(readAt(store, 10, 10) == std::array<float, 4>{1, 0, 0, 1},
          "pattern: the origin texel is the pattern's own (0,0)");
    check(readAt(store, 14, 10) == std::array<float, 4>{1, 0, 0, 1} &&
              readAt(store, 6, 10) == std::array<float, 4>{1, 0, 0, 1},
          "pattern: it repeats exactly one tile width away in both directions");
    // One texel off the origin, in the direction the checker actually
    // alternates, must NOT be the same colour -- otherwise the origin claim
    // above would pass against a solid fill that ignored the pattern.
    check(!(readAt(store, 11, 10) == readAt(store, 10, 10)),
          "pattern: the tile is not a solid colour, so the origin match above is real");
  }

  std::printf("  -- F. stroke width and Inside/Center/Outside on a known rectangle --\n");
  {
    // A 64x64 hard-edged rectangle, integer corners: `core/SelectionRefine.hpp`
    // section 2 says an integer-radius grow/shrink of a hard edge stays hard,
    // so a stroke's band edges land on exact texel boundaries and every probe
    // below is a plain in/out fact, never a coverage fraction.
    const auto makeDoc = [&] {
      OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "stroke F");
      od.selection = selectRectangle(32.0f, 32.0f, 96.0f, 96.0f);
      return od;
    };
    const auto strokeIt = [&](OpenDocument& od, StrokeLocation loc, float width) {
      const Command c = strokeCommand(StrokeParams{
          FillParams{FillSource::Color, kRed, nullptr, 0, 0, {}, {}, BlendMode::Normal, 1.0f},
          width, loc});
      return applyCommand(od, c);
    };
    const std::array<float, 4> kUntouched{0, 0, 0, 0};
    const std::array<float, 4> kFilled{1, 0, 0, 1};

    {
      OpenDocument od = makeDoc();
      const CommandResult r = strokeIt(od, StrokeLocation::Outside, 8.0f);
      check(r.ok && r.texelsChanged > 0, "stroke: Outside runs and changes texels");
      const TileStore& s = *od.document.layers[0].rgbTiles;
      check(readAt(s, 28, 64) == kFilled, "stroke Outside: 4px outside the edge is in the band");
      check(readAt(s, 64, 64) == kUntouched,
            "stroke Outside: the rectangle's own interior is untouched");
      check(readAt(s, 10, 10) == kUntouched, "stroke Outside: far outside is untouched");
    }
    {
      OpenDocument od = makeDoc();
      const CommandResult r = strokeIt(od, StrokeLocation::Inside, 8.0f);
      check(r.ok && r.texelsChanged > 0, "stroke: Inside runs and changes texels");
      const TileStore& s = *od.document.layers[0].rgbTiles;
      check(readAt(s, 34, 64) == kFilled, "stroke Inside: 2px inside the edge is in the band");
      check(readAt(s, 64, 64) == kUntouched,
            "stroke Inside: the rectangle's own centre is beyond the band");
      check(readAt(s, 10, 10) == kUntouched, "stroke Inside: outside the selection is untouched");
    }
    {
      OpenDocument od = makeDoc();
      const CommandResult r = strokeIt(od, StrokeLocation::Center, 8.0f);
      check(r.ok && r.texelsChanged > 0, "stroke: Center runs and changes texels");
      const TileStore& s = *od.document.layers[0].rgbTiles;
      // Center splits the 8px width 4 either side of the edge at x=32: the
      // band spans [28, 36).
      check(readAt(s, 30, 64) == kFilled, "stroke Center: 2px outside the edge is in the band");
      check(readAt(s, 34, 64) == kFilled, "stroke Center: 2px inside the edge is in the band");
      check(readAt(s, 64, 64) == kUntouched, "stroke Center: the centre is beyond the band");
      check(readAt(s, 10, 10) == kUntouched, "stroke Center: far outside is untouched");
    }
  }

  std::printf("  -- G. refusals --\n");
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill refusals");
    const size_t at = od.document.layers.size();
    recordLayerEdit(od, addLayer(od.document, at, makePigmentLayer("Sky")));
    od.activeLayer = at;

    Command fillC = fillCommand(FillParams{FillSource::Color, kRed, nullptr, 0, 0, {}, {},
                                           BlendMode::Normal, 1.0f});
    CommandResult r = applyCommand(od, fillC);
    check(!r.ok && contains(r.status, "Sky"),
          "refusal: fill on a Pigment layer is refused, naming the layer");

    OpenDocument od2 = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill refusals 2");
    Command bad = fillC;
    bad.params.set("source", JsonValue::string("crayon"));
    r = applyCommand(od2, bad);
    check(!r.ok && contains(r.status, "crayon"), "refusal: an unknown fill source is named");

    Command mixC = fillCommand(FillParams{FillSource::Color, kRed, nullptr, 0, 0, {}, {},
                                          BlendMode::Mix, 1.0f});
    r = applyCommand(od2, mixC);
    check(!r.ok && contains(r.status, "mix"), "refusal: fill's blend cannot be \"mix\"");

    Command noPattern = fillCommand(
        FillParams{FillSource::Pattern, {}, nullptr, 0, 0, {}, {}, BlendMode::Normal, 1.0f});
    noPattern.params.set("pattern", JsonValue::string("no such pattern"));
    r = applyCommand(od2, noPattern);
    check(!r.ok && contains(r.status, "no such pattern"),
          "refusal: fill_with a pattern that was never defined is named");

    Command strokeNoSel = strokeCommand(StrokeParams{
        FillParams{FillSource::Color, kRed, nullptr, 0, 0, {}, {}, BlendMode::Normal, 1.0f}, 4.0f,
        StrokeLocation::Center});
    r = applyCommand(od2, strokeNoSel);
    check(!r.ok && contains(r.status, "selection"),
          "refusal: stroke with no active selection is refused");

    od2.selection = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);
    Command strokeZero = strokeCommand(StrokeParams{
        FillParams{FillSource::Color, kRed, nullptr, 0, 0, {}, {}, BlendMode::Normal, 1.0f}, 0.0f,
        StrokeLocation::Center});
    r = applyCommand(od2, strokeZero);
    check(!r.ok && contains(r.status, "width"), "refusal: a zero-width stroke is refused");
  }

  std::printf("  -- H. a recorded fill replays to the same texels as the direct call --\n");
  {
    FillParams p;
    p.source = FillSource::Gradient;
    p.gradientGeometry.kind = GradientKind::Radial;
    p.gradientGeometry.x0 = 64.0f;
    p.gradientGeometry.y0 = 64.0f;
    p.gradientGeometry.x1 = 100.0f;
    p.gradientGeometry.y1 = 64.0f;
    ColorStop a, b;
    a.position = 0.0f;
    a.color = kRed;
    b.position = 1.0f;
    b.color = kBlue;
    p.gradientStops.colorStops = {a, b};
    p.blend = BlendMode::Screen;
    p.opacity = 0.75f;

    OpenDocument direct = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill H direct");
    direct.selection = selectRectangle(20.0f, 20.0f, 108.0f, 108.0f);
    applyPixelFilter(direct, fillTiles, p, "fill");

    OpenDocument replayed = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "fill H replayed");
    replayed.selection = selectRectangle(20.0f, 20.0f, 108.0f, 108.0f);
    const Command c = fillCommand(p);
    const CommandResult r = applyCommand(replayed, c);
    check(r.ok && r.texelsChanged > 0, "fill: the recorded command runs");

    bool identical = true;
    for (int32_t y = 0; y < kH && identical; ++y)
      for (int32_t x = 0; x < kW; ++x)
        if (readAt(*direct.document.layers[0].rgbTiles, x, y) !=
            readAt(*replayed.document.layers[0].rgbTiles, x, y)) {
          identical = false;
          break;
        }
    check(identical,
          "fill: every texel of the direct engine call and the encoded-then-replayed command "
          "match exactly -- the command's JSON round trip lost nothing the engine reads");
  }

  std::printf("[selftest] commands fill %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
