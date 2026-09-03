#include "app/selftest/Support.hpp"

#include <chrono>
#include <cmath>
#include <string>

#include "color/Space.hpp"
#include "core/Path.hpp"
#include "core/VectorShape.hpp"
#include "io/SvgImport.hpp"

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
    check(hasRefusalContaining(r, "text"), "refusal: text named");
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

  return ok;
}

}  // namespace np
