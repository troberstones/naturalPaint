#include "app/selftest/Support.hpp"

#include <chrono>
#include <cmath>
#include <string>

#include "color/Space.hpp"
#include "core/Path.hpp"
#include "core/TextContent.hpp"
#include "core/VectorShape.hpp"
#include "io/SvgImport.hpp"
#include "text/Shaper.hpp"

#ifndef NP_SVG_TEST_DIR
#error "NP_SVG_TEST_DIR must be defined by CMake -- see src/CMakeLists.txt"
#endif

namespace np {
namespace {

bool nearf(float a, float b, float tol = 1e-2f) { return std::fabs(a - b) <= tol; }

bool hasAnchorNear(const Path& p, float x, float y, float tol = 5e-2f) {
  for (const SubPath& sub : p.subpaths)
    for (const Anchor& a : sub.anchors)
      if (nearf(a.pt.x, x, tol) && nearf(a.pt.y, y, tol)) return true;
  return false;
}

size_t totalAnchors(const Path& p) {
  size_t n = 0;
  for (const SubPath& s : p.subpaths) n += s.anchors.size();
  return n;
}

bool hasRefusalContaining(const SvgImportResult& r, const std::string& needle) {
  for (const std::string& s : r.refusals)
    if (s.find(needle) != std::string::npos) return true;
  return false;
}

SvgImportResult importText(const std::string& xml) {
  return importSvg(reinterpret_cast<const uint8_t*>(xml.data()), xml.size());
}

// The shaper's OWN answers for a block, derived here without going anywhere
// near io/SvgImport -- so an assertion comparing the importer's numbers to
// these is the importer agreeing with CoreText, not with itself.
struct ShaperTruth {
  bool ok = false;
  float baselineOffset = 0.0f;  // top of the shaped block -> first baseline
  float width = 0.0f;
};

ShaperTruth shaperTruthFor(const TextContent& t) {
  ShaperTruth out;
  const ShapedText s = shapeText(t.utf8, t.style, TextFrame{}, TextAlign::Left);
  if (!s.ok || s.glyphs.empty()) return out;
  float minY = s.glyphs[0].y;
  for (const ShapedGlyph& g : s.glyphs) minY = std::min(minY, g.y);
  out.ok = true;
  out.baselineOffset = minY;
  out.width = s.widthPx;
  return out;
}

PathBounds shapesBounds(const std::vector<VectorShape>& shapes) {
  return vectorShapesBounds(shapes);
}

bool hasShapeNamed(const SvgImportResult& r, const std::string& name) {
  for (const VectorShape& s : r.shapes)
    if (s.name == name) return true;
  return false;
}

size_t indexOfShapeNamed(const SvgImportResult& r, const std::string& name) {
  for (size_t i = 0; i < r.shapes.size(); ++i)
    if (r.shapes[i].name == name) return i;
  return r.shapes.size();
}

}  // namespace

// io/SvgImport -- walking a real pugixml tree into document-space
// VectorShapes: the transform stack, the cascade wired to real elements,
// colour, clip-path, and the caps against a hostile document.
//
// Headless, GPU-free, writes no files (io/SvgImport.hpp's importSvgFile()
// is exercised at the very end against two on-disk fixtures written to look
// like real Inkscape and Illustrator output).
bool runSvgImportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. basic shapes: geometry --------------------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg' width='100' height='100'>"
        "<rect x='10' y='20' width='30' height='40' fill='red'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1, "rect: imports exactly one shape");
    if (r.shapes.size() == 1) {
      const Path& p = r.shapes[0].path;
      check(totalAnchors(p) == 4, "rect: four anchors (plain corners, no rx/ry)");
      check(hasAnchorNear(p, 10, 20) && hasAnchorNear(p, 40, 20) && hasAnchorNear(p, 40, 60) &&
                hasAnchorNear(p, 10, 60),
            "rect: all four corners present");
      check(r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[0], 1.0f) &&
                nearf(r.shapes[0].fill.rgba[1], 0.0f) && nearf(r.shapes[0].fill.rgba[2], 0.0f),
            "rect: fill='red' decodes to linear (1,0,0)");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<circle cx='50' cy='50' r='20'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1 && totalAnchors(r.shapes[0].path) == 4,
          "circle: four anchors");
    if (r.shapes.size() == 1) {
      const Path& p = r.shapes[0].path;
      check(hasAnchorNear(p, 70, 50) && hasAnchorNear(p, 50, 70) && hasAnchorNear(p, 30, 50) &&
                hasAnchorNear(p, 50, 30),
            "circle: anchors at the four cardinal points");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<ellipse cx='50' cy='50' rx='30' ry='10'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1, "ellipse: imports");
    if (r.shapes.size() == 1) {
      const Path& p = r.shapes[0].path;
      check(hasAnchorNear(p, 80, 50) && hasAnchorNear(p, 50, 60) && hasAnchorNear(p, 20, 50) &&
                hasAnchorNear(p, 50, 40),
            "ellipse: anchors at the four cardinal points (rx != ry)");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<line x1='0' y1='0' x2='10' y2='10' fill='red'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1, "line: imports");
    if (r.shapes.size() == 1) {
      check(totalAnchors(r.shapes[0].path) == 2, "line: two anchors");
      check(!r.shapes[0].fill.on, "line: fill is forced off regardless of fill= (SVG rule)");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<polyline points='0,0 10,0 10,10'/>"
        "<polygon points='0,0 10,0 10,10'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 2, "polyline+polygon: both import");
    if (r.shapes.size() == 2) {
      check(!r.shapes[0].path.subpaths.empty() && !r.shapes[0].path.subpaths[0].closed,
            "polyline: open subpath");
      check(!r.shapes[1].path.subpaths.empty() && r.shapes[1].path.subpaths[0].closed,
            "polygon: closed subpath");
      check(totalAnchors(r.shapes[0].path) == 3 && totalAnchors(r.shapes[1].path) == 3,
            "polyline/polygon: three anchors each");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<path d='M0,0 L10,0 L10,10 Z'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1, "path: imports");
    if (r.shapes.size() == 1) {
      check(totalAnchors(r.shapes[0].path) == 3, "path: three anchors (Z implies no 4th)");
      check(!r.shapes[0].path.subpaths.empty() && r.shapes[0].path.subpaths[0].closed,
            "path: Z closes the subpath");
    }
  }

  // --- 2. the transform stack composes through nested <g> --------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<g transform='translate(10,10)'>"
        "<g transform='scale(2)'>"
        "<rect x='0' y='0' width='5' height='5'/>"
        "</g></g></svg>");
    check(r.ok && r.shapes.size() == 1, "nested g: imports one shape");
    if (r.shapes.size() == 1) {
      const Path& p = r.shapes[0].path;
      // scale(2) applies to the shape's own local coords first, THEN
      // translate(10,10) -- (0,0)->(0,0)->(10,10), (5,5)->(10,10)->(20,20).
      check(hasAnchorNear(p, 10, 10) && hasAnchorNear(p, 20, 20) && hasAnchorNear(p, 20, 10) &&
                hasAnchorNear(p, 10, 20),
            "nested g: transform composition order matches SVG (innermost first)");
    }
  }

  // --- 3. viewBox scaling ------------------------------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100' viewBox='0 0 100 50'>"
        "<rect x='0' y='0' width='10' height='10'/>"
        "</svg>");
    check(r.ok, "viewBox: imports");
    check(nearf(r.widthPx, 200.0f) && nearf(r.heightPx, 100.0f), "viewBox: root viewport in px");
    if (!r.shapes.empty()) {
      check(hasAnchorNear(r.shapes[0].path, 0, 0) && hasAnchorNear(r.shapes[0].path, 20, 20),
            "viewBox: 2x uniform scale reaches the shape");
    }
  }

  // --- 4. <use> expansion, including its own transform --------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<defs><rect id='r' x='0' y='0' width='10' height='10'/></defs>"
        "<use href='#r' transform='translate(5,5)' x='2' y='3'/>"
        "<use xlink:href='#r' x='100' y='0' xmlns:xlink='http://www.w3.org/1999/xlink'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 2, "use: both a <defs> target and two <use>s produce shapes");
    if (r.shapes.size() == 2) {
      // transform(5,5) applies outside the use's own x,y=(2,3) offset:
      // (0,0)->(2,3)->(7,8); (10,10)->(12,13)->(17,18).
      check(hasAnchorNear(r.shapes[0].path, 7, 8) && hasAnchorNear(r.shapes[0].path, 17, 18),
            "use: transform= and x/y compose in the right order");
      check(hasAnchorNear(r.shapes[1].path, 100, 0) && hasAnchorNear(r.shapes[1].path, 110, 10),
            "use: xlink:href works the same as href");
    }
  }

  // --- 5. the cascade reaches a shape: a <style> rule beats a
  //        presentation attribute, end to end -----------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<style>rect{fill:blue;}</style>"
        "<rect x='0' y='0' width='1' height='1' fill='red'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1, "cascade: imports");
    if (r.shapes.size() == 1) {
      check(r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[2], 1.0f) &&
                nearf(r.shapes[0].fill.rgba[0], 0.0f),
            "cascade: <style> rect{} beats the presentation attribute fill=red -> blue wins");
    }
  }

  // --- 6. colour parsing in every supported spelling ----------------------

  {
    auto fillOf = [&](const char* fillAttr) {
      const std::string xml = std::string("<svg xmlns='http://www.w3.org/2000/svg'>"
                                          "<rect x='0' y='0' width='1' height='1' fill='") +
                              fillAttr + "'/></svg>";
      return importText(xml);
    };
    {
      const auto r = fillOf("#F00");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[0], 1.0f) &&
                nearf(r.shapes[0].fill.rgba[1], 0.0f),
            "colour: #rgb (3-digit hex)");
    }
    {
      const auto r = fillOf("#00FF00");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[1], 1.0f) &&
                nearf(r.shapes[0].fill.rgba[0], 0.0f),
            "colour: #rrggbb (6-digit hex)");
    }
    {
      const auto r = fillOf("rgb(0,0,255)");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[2], 1.0f),
            "colour: rgb() with integers");
    }
    {
      const auto r = fillOf("rgb(50%,50%,50%)");
      const float expected = srgbDecode(0.5f);
      check(r.shapes.size() == 1 && r.shapes[0].fill.on &&
                nearf(r.shapes[0].fill.rgba[0], expected, 1e-3f),
            "colour: rgb() with percentages");
    }
    {
      const auto r = fillOf("rgba(255,0,0,0.5)");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[0], 1.0f) &&
                nearf(r.shapes[0].fill.rgba[3], 0.5f),
            "colour: rgba() carries its own alpha");
    }
    {
      const auto r = fillOf("cornflowerblue");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on &&
                nearf(r.shapes[0].fill.rgba[0], srgbDecode(100 / 255.0f), 1e-3f),
            "colour: a CSS named colour");
    }
    {
      const auto r = fillOf("transparent");
      check(r.shapes.size() == 1 && r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[3], 0.0f),
            "colour: transparent is a real (zero-alpha) paint, not fill=none");
    }
  }

  // --- 7. currentColor -----------------------------------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<g color='green'>"
        "<rect x='0' y='0' width='1' height='1' fill='currentColor'/>"
        "</g></svg>");
    check(r.ok && r.shapes.size() == 1, "currentColor: imports");
    if (r.shapes.size() == 1) {
      check(r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[1], srgbDecode(128 / 255.0f), 1e-3f) &&
                nearf(r.shapes[0].fill.rgba[0], 0.0f),
            "currentColor: resolves to the inherited color: green");
    }
  }

  // --- 8. fill="none" is not the same as a real zero-alpha paint ----------

  {
    const auto rNone = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='0' y='0' width='1' height='1' fill='none'/></svg>");
    check(rNone.shapes.size() == 1 && !rNone.shapes[0].fill.on,
          "fill=none: Paint::on is false, not just alpha zero");
    const auto rZero = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<rect x='0' y='0' width='1' height='1' fill='rgba(0,0,0,0)'/></svg>");
    check(rZero.shapes.size() == 1 && rZero.shapes[0].fill.on &&
              nearf(rZero.shapes[0].fill.rgba[3], 0.0f),
          "fill=rgba(0,0,0,0): Paint::on is true, distinct from fill=none");
  }

  // --- 9. clip-path ----------------------------------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<clipPath id='c'><circle cx='50' cy='50' r='20'/></clipPath>"
        "<rect id='clipped' x='0' y='0' width='100' height='100' clip-path='url(#c)'/>"
        "</svg>");
    check(r.ok && r.shapes.size() == 1 && r.shapes[0].clip.has_value(),
          "clip-path: single-shape clipPath attaches a clip");
    if (r.shapes.size() == 1 && r.shapes[0].clip) {
      check(hasAnchorNear(*r.shapes[0].clip, 70, 50) && hasAnchorNear(*r.shapes[0].clip, 30, 50),
            "clip-path: clip geometry matches the referenced circle");
    }
  }
  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<clipPath id='c2'>"
        "<rect x='0' y='0' width='10' height='10'/>"
        "<rect x='20' y='0' width='10' height='10'/>"
        "</clipPath>"
        "<rect id='clipped2' x='0' y='0' width='100' height='100' clip-path='url(#c2)'/>"
        "</svg>");
    check(r.shapes.size() == 1 && r.shapes[0].clip.has_value() &&
              r.shapes[0].clip->subpaths.size() == 2,
          "clip-path: a multi-shape clipPath unions into one Path (two subpaths)");
  }

  // --- 10. every refusal appears, by name ---------------------------------

  {
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<filter id='f'><feGaussianBlur stdDeviation='2'/></filter>"
        "<rect x='0' y='0' width='1' height='1' filter='url(#f)'/>"
        "<pattern id='p'><rect width='1' height='1'/></pattern>"
        "<rect x='0' y='0' width='1' height='1' fill='url(#p)'/>"
        "<mask id='m'><rect width='1' height='1' fill='white'/></mask>"
        "<rect x='0' y='0' width='1' height='1' mask='url(#m)'/>"
        "<switch><rect x='0' y='0' width='1' height='1'/></switch>"
        "<foreignObject width='1' height='1'/>"
        "<image href='pic.png' x='0' y='0' width='1' height='1'/>"
        "<text x='0' y='0'>hi</text>"
        "<script>1</script>"
        "<animate attributeName='x' from='0' to='1'/>"
        "<use xlink:href='http://example.com/other.svg#thing' "
        "xmlns:xlink='http://www.w3.org/1999/xlink'/>"
        "<linearGradient id='g'><stop offset='0' stop-color='red'/></linearGradient>"
        "<rect x='0' y='0' width='1' height='1' fill='url(#g)'/>"
        "<madeup:tag/>"
        "</svg>");
    check(hasRefusalContaining(r, "filter"), "refusal: filter named");
    check(hasRefusalContaining(r, "pattern"), "refusal: pattern named");
    check(hasRefusalContaining(r, "mask"), "refusal: mask named");
    check(hasRefusalContaining(r, "switch"), "refusal: switch named");
    check(hasRefusalContaining(r, "foreignObject"), "refusal: foreignObject named");
    check(hasRefusalContaining(r, "image"), "refusal: image named");
    // `<text>` used to be on this list ("text rendering deferred to Stage 5").
    // Stage 5 shipped, so the assertion is inverted rather than deleted: the
    // same `<text x='0' y='0'>hi</text>` above must now come back as an
    // editable block. See io/SvgImport.hpp section 7 and this file's own
    // section 13. (On a build with no shaper it is still refused, and section
    // 13 checks that spelling instead.)
    check(shaperAvailable() ? (r.texts.size() == 1 && r.texts[0].content.utf8 == "hi")
                            : hasRefusalContaining(r, "text not imported"),
          "refusal: <text> is no longer refused -- it imports as an editable text block");
    check(hasRefusalContaining(r, "script"), "refusal: script named");
    check(hasRefusalContaining(r, "animate"), "refusal: animate named");
    check(hasRefusalContaining(r, "external"), "refusal: external <use> reference named");
    check(hasRefusalContaining(r, "linearGradient"), "refusal: gradient fill named");
    check(hasRefusalContaining(r, "unsupported element"), "refusal: unknown element named");
  }

  // --- 11. every cap fires ------------------------------------------------

  {
    // The classic <use> bomb: a self-referencing <use> must return promptly,
    // not hang, via the use-chain depth cap.
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg'>"
        "<defs><g id='loop'><use href='#loop'/></g></defs>"
        "<use href='#loop'/>"
        "</svg>");
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(hasRefusalContaining(r, "use-chain depth exceeded"),
          "cap: self-referencing <use> is refused by the depth cap");
    check(elapsed < 2.0, "cap: self-referencing <use> returns promptly (did not hang)");
  }
  {
    // Exponential fan-out: each level references the next TWICE. 20 levels
    // of branching factor 2 is 2^20 (~1e6) leaves if ever fully expanded;
    // the expansion-count cap must stop it long before that.
    std::string xml = "<svg xmlns='http://www.w3.org/2000/svg'><defs>";
    for (int i = 0; i < 20; ++i) {
      xml += "<g id='a" + std::to_string(i) + "'>";
      xml += "<use href='#a" + std::to_string(i + 1) + "'/>";
      xml += "<use href='#a" + std::to_string(i + 1) + "'/>";
      xml += "</g>";
    }
    xml += "<g id='a20'><rect width='1' height='1'/></g>";
    xml += "</defs><use href='#a0'/></svg>";
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = importText(xml);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(hasRefusalContaining(r, "use expansion cap exceeded"),
          "cap: exponential <use> fan-out is refused by the expansion cap");
    check(elapsed < 2.0, "cap: exponential <use> fan-out returns promptly");
  }
  {
    // Nesting depth: kMaxSvgNestingDepth + 50 levels of plain <g>.
    std::string xml = "<svg xmlns='http://www.w3.org/2000/svg'>";
    const int depth = kMaxSvgNestingDepth + 50;
    for (int i = 0; i < depth; ++i) xml += "<g>";
    xml += "<rect width='1' height='1'/>";
    for (int i = 0; i < depth; ++i) xml += "</g>";
    xml += "</svg>";
    const auto r = importText(xml);
    check(hasRefusalContaining(r, "nesting depth exceeded"), "cap: deep <g> nesting is refused");
  }
  {
    // Total element count: kMaxSvgElements + 5000 flat <rect> siblings.
    std::string xml = "<svg xmlns='http://www.w3.org/2000/svg'>";
    const size_t n = kMaxSvgElements + 5000;
    xml.reserve(xml.size() + n * 40);
    for (size_t i = 0; i < n; ++i) xml += "<rect width='1' height='1'/>";
    xml += "</svg>";
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = importText(xml);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(hasRefusalContaining(r, "element cap exceeded"), "cap: total element count is refused");
    check(r.shapes.size() <= kMaxSvgElements, "cap: element cap actually stopped shape production");
    check(elapsed < 5.0, "cap: element cap case completes promptly");
  }
  {
    // Total anchor count: one <path> whose `d` alone exceeds kMaxSvgAnchors.
    std::string d = "M0 0";
    const size_t n = kMaxSvgAnchors + 1;
    d.reserve(d.size() + n * 16);
    for (size_t i = 1; i <= n; ++i) {
      d += " L";
      d += std::to_string(i);
      d += " ";
      d += std::to_string(i);
    }
    const std::string xml =
        "<svg xmlns='http://www.w3.org/2000/svg'><path d='" + d + "'/></svg>";
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = importText(xml);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(hasRefusalContaining(r, "anchor cap exceeded"), "cap: total anchor count is refused");
    check(r.shapes.empty(), "cap: the over-cap path itself is dropped, not truncated silently");
    check(elapsed < 5.0, "cap: anchor cap case completes promptly");
  }

  // --- 12. two real-world-shaped fixtures ----------------------------------
  // Hand-written to look like real exporter output (this environment has no
  // genuine Inkscape/Illustrator install), carrying each tool's own
  // recognisable boilerplate -- see tests/svg/*.svg and this file's own
  // brief for what each one is standing in for.

  {
    const auto r = importSvgFile(NP_SVG_TEST_DIR "/illustrator-logo.svg");
    check(r.ok, "illustrator fixture: imports without a structural error");
    check(r.shapes.size() == 2, "illustrator fixture: circle + rect via CSS classes");
    check(hasRefusalContaining(r, "metadata"),
          "illustrator fixture: <metadata> boilerplate refused by name");
    if (r.shapes.size() == 2) {
      check(r.shapes[0].fill.on && nearf(r.shapes[0].fill.rgba[0], srgbDecode(0xF7 / 255.0f), 1e-3f),
            "illustrator fixture: .st0 class rule reaches the circle's fill");
      check(!r.shapes[1].fill.on && r.shapes[1].stroke.on,
            "illustrator fixture: .st1 class rule gives the frame fill:none, stroke:set");
      check(hasAnchorNear(r.shapes[1].path, 10, 10) && hasAnchorNear(r.shapes[1].path, 110, 110),
            "illustrator fixture: frame rect bounds (viewBox is 1:1 with the viewport)");
    }
  }
  {
    const auto r = importSvgFile(NP_SVG_TEST_DIR "/inkscape-badge.svg");
    check(r.ok, "inkscape fixture: imports without a structural error");
    check(r.shapes.size() == 2, "inkscape fixture: plate rect + ribbon path");
    check(hasRefusalContaining(r, "sodipodi:namedview"),
          "inkscape fixture: <sodipodi:namedview> refused by name");
    check(hasRefusalContaining(r, "metadata"), "inkscape fixture: <metadata> refused by name");
    check(hasRefusalContaining(r, "linearGradient"),
          "inkscape fixture: gradient fill on the ribbon path refused by name");
    if (r.shapes.size() == 2) {
      constexpr float kMmToPx = 96.0f / 25.4f;  // io/SvgPath.hpp's own 96dpi basis
      check(nearf(r.widthPx, 80.0f * kMmToPx, 0.05f) && nearf(r.heightPx, 60.0f * kMmToPx, 0.05f),
            "inkscape fixture: 80mm x 60mm root viewport resolves at 96dpi");
      check(hasAnchorNear(r.shapes[0].path, 10.0f * kMmToPx, 10.0f * kMmToPx, 0.1f) &&
                hasAnchorNear(r.shapes[0].path, 70.0f * kMmToPx, 50.0f * kMmToPx, 0.1f),
            "inkscape fixture: plate rect scaled from its mm viewBox correctly");
      check(!r.shapes[1].fill.on,
            "inkscape fixture: ribbon path's gradient fill drops to no-paint (no fallback given)");
    }
  }

  // --- 13. <text> ----------------------------------------------------------
  //
  // io/SvgImport.hpp section 7. Three things are checked here that nothing
  // else in this build can check for the importer: that a `<text>` becomes an
  // editable `TextContent` rather than glyph soup, that the two silent
  // conversions of section 7b are right (SVG's BASELINE -> `TextContent`'s
  // TOP-LEFT, and `text-anchor` -> an ORIGIN SHIFT rather than a `TextAlign`
  // that provably does nothing), and that the flat shape list and the text
  // list still re-interleave into the document's own PAINTING order.
  //
  // Every assertion below that touches the shaper compares against
  // `shaperTruthFor()`, which asks CoreText the same question independently.

  if (!shaperAvailable()) {
    // Not a failure: text/StubShaper.cpp builds are a supported configuration
    // and the importer refuses `<text>` by name there. That refusal is what
    // this section checks on such a build.
    const auto r = importText(
        "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
        "<text x='20' y='60'>Studio</text></svg>");
    check(r.texts.empty() && r.shapes.empty(), "text (no shaper): nothing is imported");
    check(hasRefusalContaining(r, "text not imported"),
          "text (no shaper): the missing shaper is named, not silently dropped");
  } else {
    // --- 13a. the simplest <text>: an editable block, and the baseline ---
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          // **Georgia at 30px, and neither number is an accident.**
          // `TextStyle`'s own defaults are Helvetica at 24px, so a fixture
          // written with those would leave the two assertions below green
          // even if nothing in the importer ever wrote either field -- which
          // is exactly what a sabotage of buildTextNodeCtx proved when this
          // case did use them.
          "<text x='20' y='60' font-family='Georgia' font-size='30' fill='#ff0000'>Studio"
          "</text></svg>");
      check(r.ok && r.texts.size() == 1 && r.shapes.empty(),
            "text: a plain <text> becomes ONE editable text block and no shapes");
      check(r.refusals.empty(), "text: a plain <text> refuses nothing at all");
      if (r.texts.size() == 1) {
        const TextContent& t = r.texts[0].content;
        check(t.utf8 == "Studio", "text: the string survives verbatim");
        check(t.style.fontFamily == "Georgia" && nearf(t.style.sizePx, 30.0f),
              "text: font-family and font-size reach TextStyle (neither is TextStyle's default)");
        // A negative control: these ARE TextStyle's defaults, so this line
        // only catches an importer that turns them on unasked. Section 13c
        // is where bold and italic are pinned positively.
        check(!t.style.bold && !t.style.italic, "text: no font-weight/style means neither");
        check(t.fill.on && nearf(t.fill.rgba[0], 1.0f) && nearf(t.fill.rgba[1], 0.0f),
              "text: fill decodes to linear like every other paint");
        check(nearf(t.frame.width, 0.0f), "text: an SVG <text> is POINT text (frame.width == 0)");

        const ShaperTruth truth = shaperTruthFor(t);
        check(truth.ok, "text: the reference shaping succeeded (the rest of 13a rests on it)");
        std::printf("    [measured] svg y=60, origin.y=%.4f, shaper ascent=%.4f\n",
                    static_cast<double>(t.origin.y), static_cast<double>(truth.baselineOffset));
        check(nearf(t.origin.x, 20.0f, 1e-3f), "text: x is the origin's x under text-anchor:start");
        // THE trap-1 assertion. `origin` is the block's TOP-LEFT and SVG's
        // `y` is the BASELINE; the distance between them is the font's own
        // ascent at this size, which this test asks CoreText for separately.
        check(nearf(t.origin.y, 60.0f - truth.baselineOffset, 1e-3f),
              "text: origin.y is the SVG baseline MINUS the shaper's own ascent");
        check(truth.baselineOffset > 0.5f * 30.0f,
              "text: and that ascent is a real font metric, not zero or a token offset");
        check(t.origin.y < 60.0f - 10.0f,
              "text: so origin.y sits WELL above the baseline (a no-op conversion fails here)");

        // The same fact stated without the shaper: "Studio" has no
        // descender, so its painted bottom IS the baseline the file named.
        // A conversion that skipped the ascent puts the whole block a line
        // lower and this is where that shows up.
        const PathBounds b = textContentBounds(t);
        check(b.valid, "text: the block paints something");
        if (b.valid) {
          std::printf("    [measured] painted extent y: %.4f .. %.4f (svg baseline 60)\n",
                      static_cast<double>(b.minY), static_cast<double>(b.maxY));
          check(std::fabs(b.maxY - 60.0f) < 1.0f,
                "text: a descender-free string's painted BOTTOM lands on the SVG baseline");
          check(b.minY < 60.0f - 0.5f * 30.0f,
                "text: and its painted TOP is a cap-height above that baseline");
        }
      }
    }

    // --- 13b. text-anchor is an ORIGIN SHIFT, never TextAlign ------------
    {
      auto anchored = [&](const char* anchor) {
        return importText(std::string("<svg xmlns='http://www.w3.org/2000/svg' width='200' "
                                      "height='100'><text x='100' y='50' text-anchor='") +
                          anchor + "' font-family='Helvetica' font-size='20'>Center</text></svg>");
      };
      const auto rs = anchored("start");
      const auto rm = anchored("middle");
      const auto re = anchored("end");
      check(rs.texts.size() == 1 && rm.texts.size() == 1 && re.texts.size() == 1,
            "anchor: all three text-anchor values still import as editable text");
      if (rs.texts.size() == 1 && rm.texts.size() == 1 && re.texts.size() == 1) {
        const ShaperTruth truth = shaperTruthFor(rs.texts[0].content);
        std::printf("    [measured] shaped width %.4f; origin.x start=%.4f middle=%.4f end=%.4f\n",
                    static_cast<double>(truth.width),
                    static_cast<double>(rs.texts[0].content.origin.x),
                    static_cast<double>(rm.texts[0].content.origin.x),
                    static_cast<double>(re.texts[0].content.origin.x));
        check(truth.ok && truth.width > 20.0f,
              "anchor: the reference string really does have a width to shift by");
        check(nearf(rs.texts[0].content.origin.x, 100.0f, 1e-3f),
              "anchor: text-anchor:start leaves the origin at x");
        check(nearf(rm.texts[0].content.origin.x, 100.0f - 0.5f * truth.width, 1e-2f),
              "anchor: text-anchor:middle shifts the ORIGIN left by half the shaped width");
        check(nearf(re.texts[0].content.origin.x, 100.0f - truth.width, 1e-2f),
              "anchor: text-anchor:end shifts the ORIGIN left by the whole shaped width");
        check(rm.texts[0].content.origin.x < rs.texts[0].content.origin.x - 1.0f &&
                  re.texts[0].content.origin.x < rm.texts[0].content.origin.x - 1.0f,
              "anchor: the three origins are strictly ordered (a no-op shift fails here)");
        // The other half of section 7b: `align` is a value nothing would
        // read at frame.width == 0, so storing the anchor there would be a
        // silent no-op. It must stay at its default.
        check(rs.texts[0].content.align == TextAlign::Left &&
                  rm.texts[0].content.align == TextAlign::Left &&
                  re.texts[0].content.align == TextAlign::Left,
              "anchor: TextAlign is left ALONE -- the anchor is not stored as an align value");
      }
    }

    // --- 13c. the ordinary style machinery reaches text ------------------
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<g font-family='Georgia' font-size='30'>"
          "<text x='0' y='40' font-weight='bold' font-style='italic'>B</text></g></svg>");
      check(r.texts.size() == 1, "text style: inherited font properties import");
      if (r.texts.size() == 1) {
        const TextStyle& s = r.texts[0].content.style;
        check(s.fontFamily == "Georgia" && nearf(s.sizePx, 30.0f),
              "text style: font-family/font-size inherit from an ancestor <g>");
        check(s.bold && s.italic, "text style: font-weight:bold and font-style:italic are read");
      }
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg'><style>text{font-family:\"Times New Roman\", "
          "serif;font-size:11pt}</style><text x='0' y='0' font-size='40'>x</text></svg>");
      check(r.texts.size() == 1, "text style: a <style> rule reaches a <text>");
      if (r.texts.size() == 1) {
        const TextStyle& s = r.texts[0].content.style;
        check(s.fontFamily == "Times New Roman",
              "text style: the FIRST family of a quoted font-family list is taken, unquoted");
        // io/SvgStyle.hpp's cascade: a sheet rule outranks a presentation
        // attribute, so 11pt wins over font-size='40'. 11pt at 96dpi.
        check(nearf(s.sizePx, 11.0f * 96.0f / 72.0f, 1e-2f),
              "text style: the sheet rule outranks the presentation attribute, in pt at 96dpi");
      }
    }
    {
      const auto r600 = importText(
          "<svg xmlns='http://www.w3.org/2000/svg'><text x='0' y='0' font-weight='600'>x</text>"
          "</svg>");
      const auto r500 = importText(
          "<svg xmlns='http://www.w3.org/2000/svg'><text x='0' y='0' font-weight='500'>x</text>"
          "</svg>");
      check(r600.texts.size() == 1 && r600.texts[0].content.style.bold,
            "text style: numeric font-weight 600 is bold");
      check(r500.texts.size() == 1 && !r500.texts[0].content.style.bold,
            "text style: numeric font-weight 500 is not");
    }

    // --- 13d. the transform stack: folded, or outlined ------------------
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<g transform='translate(10,20) scale(2)'>"
          "<text x='5' y='40' font-family='Helvetica' font-size='10'>Hi</text></g></svg>");
      check(r.texts.size() == 1 && r.shapes.empty(),
            "text transform: a translate+uniform scale still yields an editable block");
      if (r.texts.size() == 1) {
        const TextContent& t = r.texts[0].content;
        check(nearf(t.style.sizePx, 20.0f),
              "text transform: the uniform scale folds into sizePx (10 -> 20)");
        check(nearf(t.origin.x, 10.0f + 2.0f * 5.0f, 1e-3f),
              "text transform: and the translate folds into the origin");
        const ShaperTruth truth = shaperTruthFor(t);
        check(truth.ok && nearf(t.origin.y, 20.0f + 2.0f * 40.0f - truth.baselineOffset, 1e-2f),
              "text transform: the baseline is converted AFTER the scale, at the scaled size");
      }
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<g transform='rotate(30)'>"
          "<text x='5' y='40' font-family='Helvetica' font-size='10'>Hi</text></g></svg>");
      check(r.texts.empty() && !r.shapes.empty(),
            "text transform: a ROTATE cannot fold, so the text comes back as glyph outlines");
      check(hasRefusalContaining(r, "OUTLINES") && hasRefusalContaining(r, "rotation"),
            "text transform: and the fallback is named in the report, never silent");
    }

    // --- 13e/f. the <tspan> fallbacks, each named ------------------------
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='40' font-family='Helvetica' font-size='12'>Hello "
          "<tspan fill='red'>World</tspan></text></svg>");
      check(r.texts.empty() && !r.shapes.empty(),
            "tspan style: a styled run cannot be one TextContent, so it outlines");
      check(hasRefusalContaining(r, "carries its own style"),
            "tspan style: and the reason names the styled <tspan>");
      bool sawRed = false, sawBlack = false;
      for (const VectorShape& s : r.shapes) {
        if (s.fill.on && s.fill.rgba[0] > 0.5f && s.fill.rgba[1] < 0.1f) sawRed = true;
        if (s.fill.on && s.fill.rgba[0] < 0.1f) sawBlack = true;
      }
      check(sawRed && sawBlack,
            "tspan style: BOTH runs are outlined, each keeping its own fill");
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='20' font-family='Helvetica' font-size='12'>"
          "<tspan x='10' y='20'>one</tspan><tspan x='10' y='60'>two</tspan></text></svg>");
      check(r.texts.empty() && !r.shapes.empty(),
            "tspan position: two positioned runs outline rather than being dropped");
      check(hasRefusalContaining(r, "repositions"),
            "tspan position: and the reason names the repositioning");
      const PathBounds b = shapesBounds(r.shapes);
      check(b.valid && b.minY < 20.0f && b.maxY > 55.0f,
            "tspan position: each run really is drawn at ITS OWN baseline, 40px apart");
    }
    {
      // The shape every Inkscape export emits: one <tspan> restating the
      // <text>'s own x/y. It is one run, so it must NOT fall back.
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='14' y='40' font-family='Helvetica' font-size='12' id='t'>"
          "<tspan x='14' y='40' id='ts'>Fresh Pine</tspan></text></svg>");
      check(r.texts.size() == 1 && r.shapes.empty(),
            "tspan: a single <tspan> restating the <text>'s x/y is still ONE editable block");
      check(!hasRefusalContaining(r, "OUTLINES"),
            "tspan: and nothing is reported as a fallback, because nothing fell back");
      if (r.texts.size() == 1) {
        check(r.texts[0].content.utf8 == "Fresh Pine" && r.texts[0].name == "t",
              "tspan: the run's text and the <text> element's id both survive");
        check(nearf(r.texts[0].content.origin.x, 14.0f, 1e-3f),
              "tspan: the restated x is the same x, so the origin does not move");
      }
    }
    {
      // A bare run and an unstyled <tspan> are ONE run: every field
      // `TextContent` stores once agrees, so splitting them would force an
      // outline fallback on a file that did not need one.
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='40' font-family='Helvetica' font-size='12'>Hello "
          "<tspan>World</tspan></text></svg>");
      check(r.texts.size() == 1 && r.shapes.empty(),
            "tspan: an UNSTYLED <tspan> merges with the text round it -- one editable block");
      check(r.texts.size() == 1 && r.texts[0].content.utf8 == "Hello World",
            "tspan: and the merged string keeps the space between the two runs");
      check(!hasRefusalContaining(r, "OUTLINES"),
            "tspan: with no fallback reported, because none was needed");
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='40' font-size='12'>\n   <tspan>Hi</tspan>\n  </text></svg>");
      check(r.texts.size() == 1 && r.texts[0].content.utf8 == "Hi",
            "tspan: the whitespace a pretty-printed export puts round a run is collapsed away");
    }
    {
      // The case that actually reaches the collapser. pugixml's
      // `parse_default` DISCARDS whitespace-only PCDATA outright, so the
      // indentation between two elements never becomes text at all -- what
      // does reach it is a wrapped string, and SVG 1.1 10.15's default
      // `xml:space` collapses the newline and its indentation to one space.
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='40' font-family='Helvetica' font-size='12'>Hello\n"
          "        World</text></svg>");
      check(r.texts.size() == 1 && r.texts[0].content.utf8 == "Hello World",
            "text: a wrapped, indented string collapses to single spaces (xml:space default)");
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<text x='10' y='40' font-family='Helvetica' font-size='12'>   Hello   </text></svg>");
      check(r.texts.size() == 1 && r.texts[0].content.utf8 == "Hello",
            "text: and leading/trailing whitespace is trimmed at the element's own edges");
    }

    // --- 13g. refused BY NAME, per element ------------------------------
    {
      struct Case { const char* xml; const char* needle; const char* what; };
      const Case kCases[] = {
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0'><textPath href='#p'>curved</textPath></text></svg>",
           "textPath", "refusal: <textPath> (text on a path) is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0' writing-mode='tb'>vertical</text></svg>",
           "writing-mode", "refusal: a vertical writing-mode is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0' textLength='90'>stretched</text></svg>",
           "textLength", "refusal: textLength is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0' lengthAdjust='spacingAndGlyphs'>stretched</text></svg>",
           "lengthAdjust", "refusal: lengthAdjust is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0' rotate='15'>spun</text></svg>",
           "rotate", "refusal: per-character rotate is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0 10 20' y='0'>abc</text></svg>",
           "per-character position list",
           "refusal: a per-character x list is refused by name, not half-used"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0' font-size='120%'>relative</text></svg>",
           "font-size", "refusal: a font-size relative to an unresolved basis is refused by name"},
          {"<svg xmlns='http://www.w3.org/2000/svg'>"
           "<text x='0' y='0'>a<tref/>b</text></svg>",
           "<tref>", "refusal: an unhandled element inside <text> is refused by name"},
      };
      for (const Case& c : kCases) {
        const auto r = importText(c.xml);
        check(r.texts.empty() && r.shapes.empty() && hasRefusalContaining(r, c.needle), c.what);
      }
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg'><tspan x='0' y='0'>orphan</tspan></svg>");
      check(r.texts.empty() && r.shapes.empty() &&
                hasRefusalContaining(r, "only renders inside a <text>"),
            "refusal: a <tspan> outside any <text> is refused by name");
    }
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg'><text x='0' y='0'>  </text></svg>");
      check(r.texts.empty() && r.shapes.empty() && r.refusals.empty(),
            "text: a <text> holding only whitespace draws nothing AND reports nothing");
    }

    // --- 13h. PAINTING ORDER (io/SvgImport.hpp section 7a) --------------
    {
      const auto r = importText(
          "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
          "<rect id='under' x='0' y='0' width='10' height='10'/>"
          "<text x='20' y='60' font-family='Helvetica' font-size='24'>Studio</text>"
          "<rect id='over' x='0' y='0' width='10' height='10'/></svg>");
      check(r.shapes.size() == 2 && r.texts.size() == 1,
            "order: two shapes and one text block");
      if (r.shapes.size() == 2 && r.texts.size() == 1) {
        check(r.shapes[0].name == "under" && r.shapes[1].name == "over",
              "order: the shape list is still plain document order");
        // The one number section 7a exists for. Anything else -- 0, or 2 --
        // reassembles the document with the label on the wrong side of a
        // shape, which is invisible until something is drawn over a label.
        check(r.texts[0].shapesBefore == 1,
              "order: shapesBefore says ONE shape is painted below the text and one above");
      }
    }

    // --- 13i. two <text>-carrying fixtures, exporter-shaped -------------
    {
      const auto r = importSvgFile(NP_SVG_TEST_DIR "/illustrator-caption.svg");
      check(r.ok, "illustrator caption: imports without a structural error");
      check(r.shapes.size() == 1 && r.texts.size() == 1,
            "illustrator caption: one circle, and the caption stays editable text");
      if (r.shapes.size() == 1 && r.texts.size() == 1) {
        const TextContent& t = r.texts[0].content;
        check(t.utf8 == "Studio", "illustrator caption: the caption's string survives");
        check(t.style.fontFamily == "Helvetica" && nearf(t.style.sizePx, 18.0f),
              "illustrator caption: the .st1/.st2 CSS classes supply the font and size");
        check(t.fill.on && nearf(t.fill.rgba[0], srgbDecode(0x2B / 255.0f), 1e-3f),
              "illustrator caption: the .st3 class supplies the fill");
        // Illustrator writes the position as transform="matrix(1 0 0 1 80 58)"
        // with x/y left at zero, which is a pure translate.
        const ShaperTruth truth = shaperTruthFor(t);
        check(truth.ok && nearf(t.origin.x, 80.0f, 1e-2f) &&
                  nearf(t.origin.y, 58.0f - truth.baselineOffset, 1e-2f),
              "illustrator caption: the matrix() translate IS the baseline, converted to top-left");
      }
    }
    {
      const auto r = importSvgFile(NP_SVG_TEST_DIR "/inkscape-label.svg");
      constexpr float kMmToPx = 96.0f / 25.4f;  // io/SvgPath.hpp's own 96dpi basis
      check(r.ok, "inkscape label: imports without a structural error");
      check(r.texts.size() == 1,
            "inkscape label: the single-line title stays editable; the two-line subtitle does not");
      check(hasRefusalContaining(r, "repositions"),
            "inkscape label: and the two-line subtitle's fallback to outlines is named");
      if (r.texts.size() == 1) {
        const TextContent& t = r.texts[0].content;
        check(t.utf8 == "Fresh Pine" && r.texts[0].name == "title",
              "inkscape label: the title's string and id survive its <tspan> wrapper");
        check(nearf(t.style.sizePx, 6.35f * kMmToPx, 1e-2f),
              "inkscape label: the mm viewBox scale folds into the font size (6.35 -> 24 px)");
        check(nearf(t.origin.x, 14.0f * kMmToPx, 1e-2f),
              "inkscape label: text-anchor:start leaves the origin at the scaled x");
        // The plate rect is the only shape painted before the title.
        check(r.texts[0].shapesBefore == 1,
              "inkscape label: the title is painted directly above the plate rect");
        check(hasShapeNamed(r, "subtitle") && r.shapes.size() > 3,
              "inkscape label: the subtitle really did come back as many glyph outlines");
        // Painting order through the OUTLINE path, which section 7a does not
        // need `shapesBefore` for -- the glyphs go straight into `shapes` at
        // the point the `<text>` was encountered, so the plate is below them
        // and the cover rect is above them, exactly as the file draws it.
        check(indexOfShapeNamed(r, "plate") == 0 && indexOfShapeNamed(r, "subtitle") == 1 &&
                  indexOfShapeNamed(r, "cover") == r.shapes.size() - 1,
              "inkscape label: the outlined subtitle glyphs sit BETWEEN the plate rect and the "
              "cover rect, where the file draws them");
      }
    }
  }

  return ok;
}

}  // namespace np
