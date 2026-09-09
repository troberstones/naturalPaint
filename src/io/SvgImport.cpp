#include "io/SvgImport.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "color/Space.hpp"
#include "core/TextContent.hpp"
#include "io/SvgPath.hpp"
#include "io/SvgStyle.hpp"
#include "ops/Transform.hpp"
#include "pugixml/pugixml.hpp"
#include "text/Shaper.hpp"

// See io/SvgImport.hpp for the design. This file is the walk: build the
// SvgElementView chain io/SvgStyle.hpp wants, call io/SvgPath.hpp's grammars
// on the attribute text found, flatten into document-space VectorShapes.
namespace np {
namespace {

// --------------------------------------------------------------------------
// Small text helpers
// --------------------------------------------------------------------------

std::string toLowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string trimCopy(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> splitTokens(const std::string& s, bool commaOrSpace) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    const bool isSep = std::isspace(static_cast<unsigned char>(c)) || (commaOrSpace && c == ',');
    if (isSep) {
      if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::string attrStr(const pugi::xml_node& n, const char* name, const char* def = "") {
  pugi::xml_attribute a = n.attribute(name);
  return a ? std::string(a.value()) : std::string(def);
}

std::string labelFor(const pugi::xml_node& n, const char* tag) {
  const std::string id = attrStr(n, "id");
  return id.empty() ? ("<" + std::string(tag) + ">") : ("<" + std::string(tag) + ">#" + id);
}

// Root-or-nested `<svg>`, including a namespace-prefixed `xxx:svg` -- pugixml
// keeps the prefix in the element name verbatim (see io/FileKind.hpp's own
// sniff for the same convention), so this is a suffix check rather than an
// equality check.
bool isSvgTag(const std::string& tag) {
  if (tag == "svg") return true;
  return tag.size() > 4 && tag.compare(tag.size() - 4, 4, ":svg") == 0;
}

// --------------------------------------------------------------------------
// Colour: hex, rgb()/rgba(), the CSS named colours, transparent, currentColor
// --------------------------------------------------------------------------

struct SrgbColor {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

bool hexNibble(char c, int* v) {
  if (c >= '0' && c <= '9') { *v = c - '0'; return true; }
  const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (l >= 'a' && l <= 'f') { *v = 10 + (l - 'a'); return true; }
  return false;
}

// `#rgb` and `#rrggbb` only -- SVG 1.1's own two forms. CSS Color 4's
// `#rgba`/`#rrggbbaa` are a later addition this importer does not parse;
// they fail here and the caller reports "unrecognized colour".
bool parseHexColor(const std::string& v, SrgbColor* out) {
  if (v.empty() || v[0] != '#') return false;
  const std::string hex = v.substr(1);
  if (hex.size() == 3) {
    int r, g, b;
    if (!hexNibble(hex[0], &r) || !hexNibble(hex[1], &g) || !hexNibble(hex[2], &b)) return false;
    out->r = static_cast<float>(r * 17) / 255.0f;
    out->g = static_cast<float>(g * 17) / 255.0f;
    out->b = static_cast<float>(b * 17) / 255.0f;
    out->a = 1.0f;
    return true;
  }
  if (hex.size() == 6) {
    int v6[6];
    for (int i = 0; i < 6; ++i)
      if (!hexNibble(hex[static_cast<size_t>(i)], &v6[i])) return false;
    const int r = v6[0] * 16 + v6[1];
    const int g = v6[2] * 16 + v6[3];
    const int b = v6[4] * 16 + v6[5];
    out->r = static_cast<float>(r) / 255.0f;
    out->g = static_cast<float>(g) / 255.0f;
    out->b = static_cast<float>(b) / 255.0f;
    out->a = 1.0f;
    return true;
  }
  return false;
}

// Parses one `rgb()`/`rgba()` component: an integer or a percentage. Returns
// a negative sentinel on a lexing failure (a real component is never
// negative), which the caller treats as "malformed".
float parseComponent(const std::string& tok, bool* isPercent) {
  std::string s = tok;
  *isPercent = !s.empty() && s.back() == '%';
  if (*isPercent) s.pop_back();
  if (s.empty()) return -1.0f;
  char* end = nullptr;
  const float f = std::strtof(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return -1.0f;
  return f;
}

// `rgb(r, g, b)` / `rgba(r, g, b, a)`, integers 0-255 or percentages, comma
// separated -- the brief's two forms, plus `rgba()` (identical grammar with
// a fourth alpha component) because real exporters reach for it exactly as
// often as `rgb()` and the cost of accepting it is one extra component.
bool parseRgbFunction(const std::string& v, SrgbColor* out) {
  const auto open = v.find('(');
  const auto close = v.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return false;
  const std::string inner = v.substr(open + 1, close - open - 1);
  const std::vector<std::string> parts = splitTokens(inner, /*commaOrSpace=*/true);
  if (parts.size() < 3) return false;

  bool pr, pg, pb;
  const float r = parseComponent(parts[0], &pr);
  const float g = parseComponent(parts[1], &pg);
  const float b = parseComponent(parts[2], &pb);
  if (r < 0.0f || g < 0.0f || b < 0.0f) return false;

  auto norm = [](float val, bool pct) {
    float v2 = pct ? val / 100.0f : val / 255.0f;
    return std::clamp(v2, 0.0f, 1.0f);
  };
  out->r = norm(r, pr);
  out->g = norm(g, pg);
  out->b = norm(b, pb);
  out->a = 1.0f;
  if (parts.size() >= 4) {
    bool pa;
    const float a = parseComponent(parts[3], &pa);
    if (a >= 0.0f) out->a = pa ? std::clamp(a / 100.0f, 0.0f, 1.0f) : std::clamp(a, 0.0f, 1.0f);
  }
  return true;
}

// The 147 SVG 1.1 / CSS3 extended named colours (SVG 1.1 section 4.4's own
// table). Lower-case; lookup lower-cases its input first.
struct NamedColor { const char* name; uint8_t r, g, b; };
constexpr NamedColor kNamedColors[] = {
  {"aliceblue",240,248,255}, {"antiquewhite",250,235,215}, {"aqua",0,255,255},
  {"aquamarine",127,255,212}, {"azure",240,255,255}, {"beige",245,245,220},
  {"bisque",255,228,196}, {"black",0,0,0}, {"blanchedalmond",255,235,205},
  {"blue",0,0,255}, {"blueviolet",138,43,226}, {"brown",165,42,42},
  {"burlywood",222,184,135}, {"cadetblue",95,158,160}, {"chartreuse",127,255,0},
  {"chocolate",210,105,30}, {"coral",255,127,80}, {"cornflowerblue",100,149,237},
  {"cornsilk",255,248,220}, {"crimson",220,20,60}, {"cyan",0,255,255},
  {"darkblue",0,0,139}, {"darkcyan",0,139,139}, {"darkgoldenrod",184,134,11},
  {"darkgray",169,169,169}, {"darkgreen",0,100,0}, {"darkgrey",169,169,169},
  {"darkkhaki",189,183,107}, {"darkmagenta",139,0,139}, {"darkolivegreen",85,107,47},
  {"darkorange",255,140,0}, {"darkorchid",153,50,204}, {"darkred",139,0,0},
  {"darksalmon",233,150,122}, {"darkseagreen",143,188,143}, {"darkslateblue",72,61,139},
  {"darkslategray",47,79,79}, {"darkslategrey",47,79,79}, {"darkturquoise",0,206,209},
  {"darkviolet",148,0,211}, {"deeppink",255,20,147}, {"deepskyblue",0,191,255},
  {"dimgray",105,105,105}, {"dimgrey",105,105,105}, {"dodgerblue",30,144,255},
  {"firebrick",178,34,34}, {"floralwhite",255,250,240}, {"forestgreen",34,139,34},
  {"fuchsia",255,0,255}, {"gainsboro",220,220,220}, {"ghostwhite",248,248,255},
  {"gold",255,215,0}, {"goldenrod",218,165,32}, {"gray",128,128,128},
  {"grey",128,128,128}, {"green",0,128,0}, {"greenyellow",173,255,47},
  {"honeydew",240,255,240}, {"hotpink",255,105,180}, {"indianred",205,92,92},
  {"indigo",75,0,130}, {"ivory",255,255,240}, {"khaki",240,230,140},
  {"lavender",230,230,250}, {"lavenderblush",255,240,245}, {"lawngreen",124,252,0},
  {"lemonchiffon",255,250,205}, {"lightblue",173,216,230}, {"lightcoral",240,128,128},
  {"lightcyan",224,255,255}, {"lightgoldenrodyellow",250,250,210}, {"lightgray",211,211,211},
  {"lightgreen",144,238,144}, {"lightgrey",211,211,211}, {"lightpink",255,182,193},
  {"lightsalmon",255,160,122}, {"lightseagreen",32,178,170}, {"lightskyblue",135,206,250},
  {"lightslategray",119,136,153}, {"lightslategrey",119,136,153}, {"lightsteelblue",176,196,222},
  {"lightyellow",255,255,224}, {"lime",0,255,0}, {"limegreen",50,205,50},
  {"linen",250,240,230}, {"magenta",255,0,255}, {"maroon",128,0,0},
  {"mediumaquamarine",102,205,170}, {"mediumblue",0,0,205}, {"mediumorchid",186,85,211},
  {"mediumpurple",147,112,219}, {"mediumseagreen",60,179,113}, {"mediumslateblue",123,104,238},
  {"mediumspringgreen",0,250,154}, {"mediumturquoise",72,209,204}, {"mediumvioletred",199,21,133},
  {"midnightblue",25,25,112}, {"mintcream",245,255,250}, {"mistyrose",255,228,225},
  {"moccasin",255,228,181}, {"navajowhite",255,222,173}, {"navy",0,0,128},
  {"oldlace",253,245,230}, {"olive",128,128,0}, {"olivedrab",107,142,35},
  {"orange",255,165,0}, {"orangered",255,69,0}, {"orchid",218,112,214},
  {"palegoldenrod",238,232,170}, {"palegreen",152,251,152}, {"paleturquoise",175,238,238},
  {"palevioletred",219,112,147}, {"papayawhip",255,239,213}, {"peachpuff",255,218,185},
  {"peru",205,133,63}, {"pink",255,192,203}, {"plum",221,160,221},
  {"powderblue",176,224,230}, {"purple",128,0,128}, {"red",255,0,0},
  {"rosybrown",188,143,143}, {"royalblue",65,105,225}, {"saddlebrown",139,69,19},
  {"salmon",250,128,114}, {"sandybrown",244,164,96}, {"seagreen",46,139,87},
  {"seashell",255,245,238}, {"sienna",160,82,45}, {"silver",192,192,192},
  {"skyblue",135,206,235}, {"slateblue",106,90,205}, {"slategray",112,128,144},
  {"slategrey",112,128,144}, {"snow",255,250,250}, {"springgreen",0,255,127},
  {"steelblue",70,130,180}, {"tan",210,180,140}, {"teal",0,128,128},
  {"thistle",216,191,216}, {"tomato",255,99,71}, {"turquoise",64,224,208},
  {"violet",238,130,238}, {"wheat",245,222,179}, {"white",255,255,255},
  {"whitesmoke",245,245,245}, {"yellow",255,255,0}, {"yellowgreen",154,205,50},
};

bool namedColorLookup(const std::string& lname, uint8_t* r, uint8_t* g, uint8_t* b) {
  for (const NamedColor& c : kNamedColors) {
    if (lname == c.name) { *r = c.r; *g = c.g; *b = c.b; return true; }
  }
  return false;
}

// Parses a colour VALUE -- never "none", never "url(...)", both handled by
// the caller (a paint, not a colour, in the first case; a reference, not a
// literal, in the second). `currentColor` substitutes the already-resolved
// value of this element's own `color` property.
bool resolveColorText(const std::string& raw, const SrgbColor& currentColor, SrgbColor* out) {
  const std::string v = trimCopy(raw);
  const std::string vl = toLowerCopy(v);
  if (vl == "currentcolor") { *out = currentColor; return true; }
  if (vl == "transparent") { *out = SrgbColor{0, 0, 0, 0}; return true; }
  if (!v.empty() && v[0] == '#') return parseHexColor(v, out);
  if (vl.rfind("rgb(", 0) == 0 || vl.rfind("rgba(", 0) == 0) return parseRgbFunction(v, out);
  uint8_t r, g, b;
  if (namedColorLookup(vl, &r, &g, &b)) {
    out->r = r / 255.0f; out->g = g / 255.0f; out->b = b / 255.0f; out->a = 1.0f;
    return true;
  }
  return false;
}

// `color`'s own resolved value, used to substitute `currentColor` elsewhere
// on the same element. A literal "currentColor" on `color` itself, or an
// unparseable value, falls back to black -- SVG's own UA-default initial
// value for `color` (CSS Color 3 section 4.4) -- rather than recursing.
SrgbColor resolveCurrentColor(const std::map<std::string, std::string>& style) {
  std::string v = style.count("color") ? style.at("color") : "black";
  if (toLowerCopy(trimCopy(v)) == "currentcolor") v = "black";
  SrgbColor c{0, 0, 0, 1};
  const SrgbColor unused{0, 0, 0, 1};
  if (!resolveColorText(v, unused, &c)) return SrgbColor{0, 0, 0, 1};
  return c;
}

// `url(#id)` or `url(#id) fallback...` (SVG2's own fallback-colour syntax).
// Returns false when `v` is not a `url(...)` at all. `*inner` keeps a
// leading `#` when present, so the caller can tell a local reference from
// an external one without a second parse.
bool extractUrlRef(const std::string& v, std::string* inner, std::string* rest) {
  if (v.rfind("url(", 0) != 0) return false;
  const auto close = v.find(')');
  if (close == std::string::npos) return false;
  std::string in = trimCopy(v.substr(4, close - 4));
  if (!in.empty() && (in.front() == '"' || in.front() == '\'')) in.erase(in.begin());
  if (!in.empty() && (in.back() == '"' || in.back() == '\'')) in.pop_back();
  *inner = in;
  if (rest) *rest = trimCopy(v.substr(close + 1));
  return true;
}

float parseOpacityValue(const std::string& s, float def) {
  const std::string t0 = trimCopy(s);
  if (t0.empty()) return def;
  std::string t = t0;
  const bool pct = !t.empty() && t.back() == '%';
  if (pct) t.pop_back();
  char* end = nullptr;
  const float v = std::strtof(t.c_str(), &end);
  if (end == t.c_str()) return def;
  return std::clamp(pct ? v / 100.0f : v, 0.0f, 1.0f);
}

// --------------------------------------------------------------------------
// Length / viewport plumbing
// --------------------------------------------------------------------------

struct Viewport { float w = 0.0f, h = 0.0f; };

float resolveLenAxis(const std::string& text, char axis, const Viewport& vp, float fallback) {
  if (text.empty()) return fallback;
  SvgLength len;
  if (!parseSvgLength(text, &len)) return fallback;
  SvgLengthContext ctx;
  if (axis == 'x') ctx.percentBasisPx = vp.w;
  else if (axis == 'y') ctx.percentBasisPx = vp.h;
  else ctx.percentBasisPx = std::sqrt(vp.w * vp.w + vp.h * vp.h) / std::sqrt(2.0f);
  return resolveSvgLength(len, ctx);
}

float transformScaleFactor(const Mat3& m) {
  const float det = m.m[0] * m.m[4] - m.m[1] * m.m[3];
  return std::sqrt(std::fabs(det));
}

Point2 toPoint2(PathPoint p) { return Point2{p.x, p.y}; }
PathPoint toPathPoint(Point2 p) { return PathPoint{p.x, p.y}; }

void transformPathInPlace(Path& path, const Mat3& m) {
  for (SubPath& sub : path.subpaths) {
    for (Anchor& a : sub.anchors) {
      a.pt = toPathPoint(mat3MapPoint(m, toPoint2(a.pt)));
      a.in = toPathPoint(mat3MapPoint(m, toPoint2(a.in)));
      a.out = toPathPoint(mat3MapPoint(m, toPoint2(a.out)));
    }
  }
}

size_t countAnchors(const Path& p) {
  size_t n = 0;
  for (const SubPath& s : p.subpaths) n += s.anchors.size();
  return n;
}

// --------------------------------------------------------------------------
// Basic-shape and `<path>` geometry, in LOCAL (pre-transform) coordinates
// --------------------------------------------------------------------------

bool buildBasicShapeLocalPath(const pugi::xml_node& n, const std::string& tag, const Viewport& vp,
                               Path* out) {
  if (tag == "rect") {
    const float x = resolveLenAxis(attrStr(n, "x", "0"), 'x', vp, 0.0f);
    const float y = resolveLenAxis(attrStr(n, "y", "0"), 'y', vp, 0.0f);
    const float w = resolveLenAxis(attrStr(n, "width", "0"), 'x', vp, 0.0f);
    const float h = resolveLenAxis(attrStr(n, "height", "0"), 'y', vp, 0.0f);
    const float rx = n.attribute("rx") ? resolveLenAxis(attrStr(n, "rx"), 'x', vp, -1.0f) : -1.0f;
    const float ry = n.attribute("ry") ? resolveLenAxis(attrStr(n, "ry"), 'y', vp, -1.0f) : -1.0f;
    *out = svgRectPath(x, y, w, h, rx, ry);
    return true;
  }
  if (tag == "circle") {
    const float cx = resolveLenAxis(attrStr(n, "cx", "0"), 'x', vp, 0.0f);
    const float cy = resolveLenAxis(attrStr(n, "cy", "0"), 'y', vp, 0.0f);
    const float r = resolveLenAxis(attrStr(n, "r", "0"), 'd', vp, 0.0f);
    *out = svgEllipsePath(cx, cy, r, r);
    return true;
  }
  if (tag == "ellipse") {
    const float cx = resolveLenAxis(attrStr(n, "cx", "0"), 'x', vp, 0.0f);
    const float cy = resolveLenAxis(attrStr(n, "cy", "0"), 'y', vp, 0.0f);
    const float rx = resolveLenAxis(attrStr(n, "rx", "0"), 'x', vp, 0.0f);
    const float ry = resolveLenAxis(attrStr(n, "ry", "0"), 'y', vp, 0.0f);
    *out = svgEllipsePath(cx, cy, rx, ry);
    return true;
  }
  if (tag == "line") {
    const float x1 = resolveLenAxis(attrStr(n, "x1", "0"), 'x', vp, 0.0f);
    const float y1 = resolveLenAxis(attrStr(n, "y1", "0"), 'y', vp, 0.0f);
    const float x2 = resolveLenAxis(attrStr(n, "x2", "0"), 'x', vp, 0.0f);
    const float y2 = resolveLenAxis(attrStr(n, "y2", "0"), 'y', vp, 0.0f);
    *out = svgLinePath(x1, y1, x2, y2);
    return true;
  }
  if (tag == "polyline" || tag == "polygon") {
    std::vector<float> pts;
    parseSvgNumberList(attrStr(n, "points"), &pts);  // lenient: uses the valid prefix on failure
    *out = svgPolyPath(pts, tag == "polygon");
    return true;
  }
  if (tag == "path") {
    size_t errOffset = 0;
    Path p;
    parseSvgPathData(attrStr(n, "d"), &p, &errOffset);  // lenient: keeps every command before the error
    *out = p;
    return true;
  }
  return false;
}

// --------------------------------------------------------------------------
// Presentation-attribute recognition and the style computation wrapper
// --------------------------------------------------------------------------

bool isPresentationProp(const std::string& name) {
  static const std::set<std::string> kProps = {
      "fill", "stroke", "fill-opacity", "stroke-opacity", "opacity", "fill-rule",
      "stroke-width", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit",
      "stroke-dasharray", "stroke-dashoffset", "color", "clip-path", "display",
      "mask", "filter",
      // The text properties (io/SvgImport.hpp section 7). They are ordinary
      // inheriting presentation attributes -- io/SvgStyle.cpp's own
      // `svgPropertyInherits()` already lists every one -- so they belong in
      // the cascade for every element, not only inside a <text>: a
      // `font-size` on a <g> reaches the <text> inside it exactly the way a
      // `fill` does.
      "font-family", "font-size", "font-style", "font-weight", "text-anchor",
      "letter-spacing", "writing-mode",
  };
  return kProps.count(name) != 0;
}

struct ElemStyle {
  SvgElementView view;
  std::map<std::string, std::string> style;
  std::map<std::string, std::string> inheritedForChildren;
};

ElemStyle computeElemStyle(const pugi::xml_node& node, const std::string& tag,
                            const SvgElementView* parentView,
                            const std::map<std::string, std::string>& inheritedFromParent,
                            const SvgStyleSheet& sheet) {
  ElemStyle es;
  es.view.tag = tag;
  es.view.id = attrStr(node, "id");
  es.view.classes = splitTokens(attrStr(node, "class"), /*commaOrSpace=*/false);
  es.view.parent = parentView;

  std::vector<SvgDeclaration> presentation;
  for (pugi::xml_attribute a : node.attributes()) {
    const std::string name = a.name();
    if (isPresentationProp(name)) presentation.push_back(SvgDeclaration{name, a.value(), false});
  }
  std::vector<SvgDeclaration> inlineDecls;
  if (node.attribute("style")) parseSvgInlineStyle(node.attribute("style").value(), &inlineDecls);

  es.style = svgComputeStyle(es.view, presentation, inlineDecls, sheet, inheritedFromParent);
  for (const auto& [k, v] : es.style)
    if (svgPropertyInherits(k)) es.inheritedForChildren[k] = v;
  return es;
}

// --------------------------------------------------------------------------
// Walk context and the two caps that are checked eagerly (section 6)
// --------------------------------------------------------------------------

struct Ctx {
  std::unordered_map<std::string, pugi::xml_node> idIndex;
  SvgImportResult* result = nullptr;
  size_t elementCount = 0;
  size_t anchorCount = 0;
  size_t useExpansionCount = 0;
  bool aborted = false;
  bool cappedElements = false;
  bool cappedAnchors = false;
  bool cappedDepth = false;
  bool cappedUseExpansions = false;
};

bool addAnchorsWithCap(Ctx& ctx, size_t n) {
  ctx.anchorCount += n;
  if (ctx.anchorCount > kMaxSvgAnchors) {
    if (!ctx.cappedAnchors) {
      ctx.result->refusals.push_back("anchor cap exceeded (" + std::to_string(kMaxSvgAnchors) +
                                     "); import stopped");
      ctx.cappedAnchors = true;
    }
    ctx.aborted = true;
    return false;
  }
  return true;
}

void checkMaskFilter(Ctx& ctx, const std::map<std::string, std::string>& style,
                      const std::string& label) {
  auto m = style.find("mask");
  if (m != style.end() && toLowerCopy(trimCopy(m->second)) != "none" &&
      !trimCopy(m->second).empty()) {
    ctx.result->refusals.push_back("mask on " + label +
                                   ": masks not supported (see io/SvgImport.hpp section 5)");
  }
  auto f = style.find("filter");
  if (f != style.end() && toLowerCopy(trimCopy(f->second)) != "none" &&
      !trimCopy(f->second).empty()) {
    ctx.result->refusals.push_back("filter on " + label +
                                   ": filters not supported (see io/SvgImport.hpp section 5)");
  }
}

// clip-path cannot be represented on a container (<g>/<svg>/<use>): doing so
// correctly needs the intersection of the container's clip with whatever
// clip its descendants carry, i.e. boolean path intersection, which this
// codebase has no general implementation of. Only a leaf shape's own
// clip-path is honoured -- see io/SvgImport.hpp section 4.
void checkContainerClipPath(Ctx& ctx, const std::map<std::string, std::string>& style,
                             const std::string& label) {
  auto it = style.find("clip-path");
  if (it == style.end()) return;
  const std::string v = trimCopy(it->second);
  if (v.empty() || toLowerCopy(v) == "none") return;
  ctx.result->refusals.push_back(
      "clip-path on " + label +
      ": not supported on containers (<g>/<svg>/<use>) -- only on individual shapes");
}

// --------------------------------------------------------------------------
// Paint resolution (fill / stroke)
// --------------------------------------------------------------------------

Paint resolvePaintProperty(Ctx& ctx, const std::map<std::string, std::string>& style,
                            const char* prop, const char* defaultValue,
                            const SrgbColor& currentColor, float opacityMul,
                            const std::string& label, const char* propLabel) {
  Paint paint;
  const std::string value = style.count(prop) ? style.at(prop) : defaultValue;
  const std::string v = trimCopy(value);
  const std::string vl = toLowerCopy(v);
  if (v.empty() || vl == "none") { paint.on = false; return paint; }

  std::string inner, rest;
  if (extractUrlRef(v, &inner, &rest)) {
    if (!inner.empty() && inner[0] == '#') {
      const std::string idref = inner.substr(1);
      auto it = ctx.idIndex.find(idref);
      if (it == ctx.idIndex.end()) {
        ctx.result->refusals.push_back(std::string(propLabel) + " on " + label + ": url(#" +
                                       idref + ") -- unknown id");
      } else {
        const std::string reftag = it->second.name();
        if (reftag == "linearGradient" || reftag == "radialGradient" || reftag == "pattern") {
          ctx.result->refusals.push_back(
              std::string(propLabel) + " on " + label + ": url(#" + idref + ") is a <" + reftag +
              ">; gradients/patterns are not supported (see io/SvgImport.hpp section 3)");
        } else {
          ctx.result->refusals.push_back(std::string(propLabel) + " on " + label + ": url(#" +
                                         idref + ") does not refer to a paint server");
        }
      }
    } else {
      ctx.result->refusals.push_back(std::string(propLabel) + " on " + label +
                                     ": external paint-server reference not supported");
    }
    if (!rest.empty()) {
      SrgbColor c;
      if (resolveColorText(rest, currentColor, &c)) {
        paint.on = true;
        paint.rgba = {srgbDecode(c.r), srgbDecode(c.g), srgbDecode(c.b),
                     std::clamp(c.a * opacityMul, 0.0f, 1.0f)};
        return paint;
      }
    }
    paint.on = false;
    return paint;
  }

  SrgbColor c;
  if (!resolveColorText(v, currentColor, &c)) {
    ctx.result->refusals.push_back(std::string(propLabel) + " on " + label +
                                   ": unrecognized colour '" + v + "'");
    paint.on = false;
    return paint;
  }
  paint.on = true;
  paint.rgba = {srgbDecode(c.r), srgbDecode(c.g), srgbDecode(c.b),
               std::clamp(c.a * opacityMul, 0.0f, 1.0f)};
  return paint;
}

StrokeStyle resolveStrokeStyle(const std::map<std::string, std::string>& style,
                                const Viewport& vp, float scaleFactor) {
  StrokeStyle st;
  const std::string w = style.count("stroke-width") ? style.at("stroke-width") : "1";
  st.width = resolveLenAxis(w, 'd', vp, 1.0f) * scaleFactor;

  const std::string cap = style.count("stroke-linecap") ? style.at("stroke-linecap") : "butt";
  st.cap = cap == "round" ? LineCap::Round : cap == "square" ? LineCap::Square : LineCap::Butt;

  const std::string join = style.count("stroke-linejoin") ? style.at("stroke-linejoin") : "miter";
  st.join = join == "round" ? LineJoin::Round : join == "bevel" ? LineJoin::Bevel : LineJoin::Miter;

  const std::string miter = style.count("stroke-miterlimit") ? style.at("stroke-miterlimit") : "4";
  char* end = nullptr;
  const float ml = std::strtof(miter.c_str(), &end);
  st.miterLimit = (end != miter.c_str() && ml > 0.0f) ? ml : 4.0f;

  const std::string dashArr = style.count("stroke-dasharray") ? style.at("stroke-dasharray") : "none";
  if (toLowerCopy(trimCopy(dashArr)) != "none" && !trimCopy(dashArr).empty()) {
    for (const std::string& tok : splitTokens(dashArr, /*commaOrSpace=*/true))
      st.dashes.push_back(resolveLenAxis(tok, 'd', vp, 0.0f) * scaleFactor);
    if (st.dashes.size() % 2 == 1) {
      const std::vector<float> copy = st.dashes;
      st.dashes.insert(st.dashes.end(), copy.begin(), copy.end());
    }
  }
  const std::string dashOff = style.count("stroke-dashoffset") ? style.at("stroke-dashoffset") : "0";
  st.dashOffset = resolveLenAxis(dashOff, 'd', vp, 0.0f) * scaleFactor;
  return st;
}

// --------------------------------------------------------------------------
// clipPath resolution -- see io/SvgImport.hpp section 4
// --------------------------------------------------------------------------

void resolveClipPath(Ctx& ctx, const std::map<std::string, std::string>& style, const Mat3& accum,
                      const Viewport& vp, const std::string& label, std::optional<Path>* outClip) {
  auto it = style.find("clip-path");
  if (it == style.end()) return;
  const std::string v = trimCopy(it->second);
  if (v.empty() || toLowerCopy(v) == "none") return;

  std::string inner, rest;
  if (!extractUrlRef(v, &inner, &rest)) {
    ctx.result->refusals.push_back("clip-path on " + label + ": unsupported value '" + v +
                                   "' (only url(#id) is supported)");
    return;
  }
  if (inner.empty() || inner[0] != '#') {
    ctx.result->refusals.push_back("clip-path on " + label + ": external reference not supported");
    return;
  }
  const std::string idref = inner.substr(1);
  auto found = ctx.idIndex.find(idref);
  if (found == ctx.idIndex.end()) {
    ctx.result->refusals.push_back("clip-path on " + label + ": unknown id #" + idref);
    return;
  }
  pugi::xml_node cpNode = found->second;
  if (std::string(cpNode.name()) != "clipPath") {
    ctx.result->refusals.push_back("clip-path on " + label + ": #" + idref +
                                   " is not a <clipPath>");
    return;
  }
  const std::string cpUnits = cpNode.attribute("clipPathUnits")
                                  ? cpNode.attribute("clipPathUnits").value()
                                  : "userSpaceOnUse";
  if (cpUnits != "userSpaceOnUse") {
    ctx.result->refusals.push_back("clip-path on " + label + ": clipPathUnits=\"" + cpUnits +
                                   "\" not supported (only userSpaceOnUse)");
    return;
  }

  Mat3 cpAccum = accum;
  if (cpNode.attribute("transform")) {
    Mat3 t;
    if (parseSvgTransform(cpNode.attribute("transform").value(), &t)) cpAccum = mat3Multiply(accum, t);
  }

  Path clip;
  clip.rule = FillRule::NonZero;
  for (pugi::xml_node child = cpNode.first_child(); child; child = child.next_sibling()) {
    if (child.type() != pugi::node_element) continue;
    const std::string ctag = child.name();
    if (ctag == "title" || ctag == "desc") continue;
    Path childLocal;
    if (!buildBasicShapeLocalPath(child, ctag, vp, &childLocal)) {
      ctx.result->refusals.push_back("clip-path on " + label + ": <" + ctag + "> inside #" +
                                     idref + " not supported (only basic shapes and <path>)");
      continue;
    }
    Mat3 childAccum = cpAccum;
    if (child.attribute("transform")) {
      Mat3 t;
      if (parseSvgTransform(child.attribute("transform").value(), &t))
        childAccum = mat3Multiply(cpAccum, t);
    }
    transformPathInPlace(childLocal, childAccum);
    for (SubPath& sub : childLocal.subpaths) clip.subpaths.push_back(std::move(sub));
  }
  if (clip.subpaths.empty()) return;
  if (!addAnchorsWithCap(ctx, countAnchors(clip))) return;
  *outClip = std::move(clip);
}

// --------------------------------------------------------------------------
// One leaf shape element -> zero or one VectorShape
// --------------------------------------------------------------------------

void processShapeElement(Ctx& ctx, const pugi::xml_node& node, const std::string& tag,
                          const Mat3& accum, const Viewport& vp,
                          const std::map<std::string, std::string>& style,
                          const SrgbColor& currentColor, const std::string& label) {
  Path localPath;
  if (!buildBasicShapeLocalPath(node, tag, vp, &localPath)) return;

  const std::string fr = style.count("fill-rule") ? style.at("fill-rule") : "nonzero";
  localPath.rule = (fr == "evenodd") ? FillRule::EvenOdd : FillRule::NonZero;
  transformPathInPlace(localPath, accum);

  const float elementOpacity = parseOpacityValue(style.count("opacity") ? style.at("opacity") : "1", 1.0f);
  const float fillOpacity =
      parseOpacityValue(style.count("fill-opacity") ? style.at("fill-opacity") : "1", 1.0f);
  const float strokeOpacity =
      parseOpacityValue(style.count("stroke-opacity") ? style.at("stroke-opacity") : "1", 1.0f);

  VectorShape shape;
  shape.path = localPath;
  shape.fill = resolvePaintProperty(ctx, style, "fill", "black", currentColor,
                                    fillOpacity * elementOpacity, label, "fill");
  shape.stroke = resolvePaintProperty(ctx, style, "stroke", "none", currentColor,
                                      strokeOpacity * elementOpacity, label, "stroke");
  if (tag == "line") shape.fill.on = false;  // SVG: <line> cannot be filled

  shape.strokeStyle = resolveStrokeStyle(style, vp, transformScaleFactor(accum));
  resolveClipPath(ctx, style, accum, vp, label, &shape.clip);
  shape.name = attrStr(node, "id");

  if (!addAnchorsWithCap(ctx, countAnchors(shape.path))) return;
  ctx.result->shapes.push_back(std::move(shape));
}

// --------------------------------------------------------------------------
// <text> -- see io/SvgImport.hpp section 7 for every decision below
// --------------------------------------------------------------------------

// SVG's own three values. Deliberately NOT `TextAlign`: text/Shaper.hpp is
// explicit that alignment does nothing at `frame.width == 0`, and an SVG
// `<text>` is point text (io/SvgImport.hpp section 7b). This becomes an
// origin shift, never an align value.
enum class TextAnchor { Start, Middle, End };

// Nesting cap inside one `<text>`: `<tspan>`s wrap `<tspan>`s in real
// exports, but never deeply. kMaxSvgNestingDepth already bounds the outer
// walk; this bounds the inner one on its own terms.
constexpr int kMaxTextNesting = 16;

// XML whitespace collapsing, SVG 1.1 10.15's `xml:space="default"` minus the
// leading/trailing trim, which the caller does once across the whole element
// rather than per text node (a space BETWEEN two runs is significant; the
// same space before the first run is not).
std::string collapseWhitespace(std::string_view s) {
  std::string out;
  bool pendingSpace = false;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) { pendingSpace = true; continue; }
    if (pendingSpace) { out.push_back(' '); pendingSpace = false; }
    out.push_back(c);
  }
  if (pendingSpace) out.push_back(' ');
  return out;
}

void ltrimSpaces(std::string* s) {
  size_t b = 0;
  while (b < s->size() && (*s)[b] == ' ') ++b;
  s->erase(0, b);
}

void rtrimSpaces(std::string* s) {
  while (!s->empty() && s->back() == ' ') s->pop_back();
}

// The FIRST family of a CSS font-family list, unquoted. text/Shaper.hpp's
// `shapeText()` takes one family and substitutes the platform default for a
// name it does not have, so walking the rest of the list to find one that
// exists would duplicate a fallback CoreText already performs -- and would
// do it worse, because it cannot see per-glyph coverage.
std::string firstFontFamily(const std::string& raw) {
  std::string s = trimCopy(raw);
  const auto comma = s.find(',');
  if (comma != std::string::npos) s = trimCopy(s.substr(0, comma));
  if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"') && s.back() == s.front())
    s = trimCopy(s.substr(1, s.size() - 2));
  return s;
}

// CSS font-weight -> `TextStyle::bold`. `TextStyle` is one bit, so the whole
// 100-900 axis collapses at 600 (CSS's own "bold" is 700; 600 is semibold and
// reads bold in a two-state model). `bolder`/`lighter` are relative to an
// inherited weight this walk does not track numerically, so `bolder` is taken
// as bold and `lighter` as not -- the direction each one means.
bool weightIsBold(const std::string& raw) {
  const std::string v = toLowerCopy(trimCopy(raw));
  if (v.empty() || v == "normal" || v == "lighter") return false;
  if (v == "bold" || v == "bolder") return true;
  char* end = nullptr;
  const float n = std::strtof(v.c_str(), &end);
  if (end != v.c_str() && *end == '\0') return n >= 600.0f;
  return false;
}

// `font-size` in pixels. Returns false for a value in a unit relative to a
// basis the caller says it does not have (`basisKnown == false`) -- on the
// `<text>` element itself, where an `em`/`%` would resolve against an
// ancestor font-size no element in this walk ever turned into a number.
bool resolveFontSizePx(const std::string& raw, float basisPx, bool basisKnown, float* out) {
  const std::string v = trimCopy(raw);
  if (v.empty()) { *out = basisKnown ? basisPx : 16.0f; return true; }
  const std::string vl = toLowerCopy(v);
  // CSS's absolute-size keywords, at the ratios CSS 2.1 15.7 tabulates
  // against a 16px medium. Real exporters rarely emit them; a hand-written
  // or CSS-styled document does.
  static const std::map<std::string, float> kKeywords = {
      {"xx-small", 9.0f}, {"x-small", 10.0f}, {"small", 13.0f}, {"medium", 16.0f},
      {"large", 18.0f},   {"x-large", 24.0f}, {"xx-large", 32.0f},
  };
  const auto kw = kKeywords.find(vl);
  if (kw != kKeywords.end()) { *out = kw->second; return true; }

  SvgLength len;
  if (!parseSvgLength(v, &len)) return false;
  SvgLengthContext lc;
  if (len.unit == SvgUnit::Em || len.unit == SvgUnit::Ex || len.unit == SvgUnit::Percent) {
    if (!basisKnown) return false;
    lc.fontSizePx = basisPx;
    lc.xHeightPx = basisPx * 0.5f;
    lc.percentBasisPx = basisPx;
  }
  *out = resolveSvgLength(len, lc);
  return std::isfinite(*out) && *out > 0.0f;
}

// A translation plus a POSITIVE UNIFORM scale, or nothing: the scale folds
// into `sizePx` and the translation into `origin`. Rotation, skew and
// mirroring do not fold, and the caller outlines those to paths instead.
//
// **This is now the importer's own limit, not the model's.** It read "a
// `TextContent` has no matrix" until one was added for the Move tool
// (core/TextContent.hpp section 4), so a rotated `<text>` could in principle
// come in as live text carrying that matrix rather than as outlines. Doing
// it is a real, bounded follow-up -- it needs the accumulated CTM split into
// the part that folds into the type size and the part that stays a matrix,
// and it needs deciding what a MIRRORED block should mean -- and it is not
// done here, so this stays as it was rather than being half-changed.
bool decomposeTranslateScale(const Mat3& m, float* s, float* tx, float* ty) {
  if (std::fabs(m.m[6]) > 1e-6f || std::fabs(m.m[7]) > 1e-6f ||
      std::fabs(m.m[8] - 1.0f) > 1e-6f)
    return false;
  const float a = m.m[0], c = m.m[1], b = m.m[3], d = m.m[4];
  const float mag = std::max(std::max(std::fabs(a), std::fabs(d)),
                             std::max(std::max(std::fabs(b), std::fabs(c)), 1.0f));
  if (std::fabs(b) > 1e-4f * mag || std::fabs(c) > 1e-4f * mag) return false;
  if (std::fabs(a - d) > 1e-4f * mag) return false;
  if (!(a > 0.0f) || !std::isfinite(a)) return false;
  *s = a;
  *tx = m.m[2];
  *ty = m.m[5];
  return true;
}

// One shaped-alike stretch of text: everything between two style changes or
// two position commands.
struct TextRun {
  std::string utf8;
  TextStyle style;
  Paint fill;
  Paint stroke;
  StrokeStyle strokeStyle;
  TextAnchor anchor = TextAnchor::Start;

  bool hasAbsX = false, hasAbsY = false;
  float absX = 0.0f, absY = 0.0f;
  float dx = 0.0f, dy = 0.0f;

  // Filled in by layoutRuns().
  size_t chunk = 0;
  float penX = 0.0f, penY = 0.0f;  // the run's BASELINE start
  float width = 0.0f;              // shaped advance
};

// Everything about one element inside a `<text>` that a run inherits.
struct TextNodeCtx {
  ElemStyle es;
  TextStyle style;
  Paint fill;
  Paint stroke;
  StrokeStyle strokeStyle;
  TextAnchor anchor = TextAnchor::Start;
};

struct TextCollect {
  std::vector<TextRun> runs;
  bool refused = false;
  bool started = false;  // a non-empty run has been emitted (leading-trim gate)
  bool pendAbsX = false, pendAbsY = false;
  float pendX = 0.0f, pendY = 0.0f;
  float pendDx = 0.0f, pendDy = 0.0f;
};

// A single-valued length attribute. `x`/`y`/`dx`/`dy` on `<text>`/`<tspan>`
// are LISTS in SVG (one value per character); this importer lays out runs,
// not characters, so a list of more than one is refused by name rather than
// silently using its first entry and drawing the rest in the wrong place.
bool singleLenAttr(Ctx& ctx, const pugi::xml_node& n, const char* name, char axis,
                    const Viewport& vp, const std::string& label, bool* present, float* out) {
  *present = false;
  if (!n.attribute(name)) return true;
  const std::string raw = attrStr(n, name);
  if (splitTokens(raw, /*commaOrSpace=*/true).size() > 1) {
    ctx.result->refusals.push_back(
        label + ": " + name + "=\"" + raw +
        "\" is a per-character position list; only a single value is supported, so the whole "
        "element is dropped");
    return false;
  }
  *present = true;
  *out = resolveLenAxis(raw, axis, vp, 0.0f);
  return true;
}

// The attributes inside a `<text>` this file refuses outright, on either
// `<text>` or a `<tspan>` -- each one is layout this importer does not
// perform, and performing it wrongly would look like a font problem.
bool checkTextRefusedAttrs(Ctx& ctx, const pugi::xml_node& n, const std::string& elemLabel,
                            const std::string& textLabel) {
  struct Bad { const char* attr; const char* why; };
  static const Bad kBad[] = {
      {"rotate", "per-character rotation is not supported"},
      {"textLength", "textLength/lengthAdjust (fitting text to a given length) is not supported"},
      {"lengthAdjust",
       "textLength/lengthAdjust (fitting text to a given length) is not supported"},
  };
  for (const Bad& b : kBad) {
    if (n.attribute(b.attr)) {
      ctx.result->refusals.push_back(textLabel + ": " + elemLabel + " has " + b.attr + " -- " +
                                     b.why + "; the whole element is dropped");
      return false;
    }
  }
  return true;
}

bool buildTextNodeCtx(Ctx& ctx, const pugi::xml_node& node, const std::string& tag,
                       const SvgElementView* parentView,
                       const std::map<std::string, std::string>& inherited,
                       const SvgStyleSheet& sheet, const Viewport& vp, float basisFontPx,
                       bool basisKnown, const std::string& textLabel, TextNodeCtx* out) {
  out->es = computeElemStyle(node, tag, parentView, inherited, sheet);
  const std::map<std::string, std::string>& st = out->es.style;
  auto get = [&](const char* k, const char* def) -> std::string {
    auto it = st.find(k);
    return it == st.end() ? std::string(def) : it->second;
  };

  // Vertical text is a different layout algorithm end to end -- glyph
  // orientation, line progression, and what "advance" even means -- not a
  // transform of the horizontal one.
  const std::string wm = toLowerCopy(trimCopy(get("writing-mode", "horizontal-tb")));
  if (!(wm.empty() || wm == "horizontal-tb" || wm == "lr" || wm == "lr-tb" || wm == "rl" ||
        wm == "rl-tb")) {
    ctx.result->refusals.push_back(textLabel + ": writing-mode=\"" + wm +
                                   "\" -- vertical text is not supported; the whole element is "
                                   "dropped");
    return false;
  }

  const std::string fsText = trimCopy(get("font-size", ""));
  float sizePx = 16.0f;
  if (!resolveFontSizePx(fsText, basisFontPx, basisKnown, &sizePx)) {
    ctx.result->refusals.push_back(
        textLabel + ": font-size=\"" + fsText +
        "\" is relative to an inherited size this importer never resolved to a number; the "
        "whole element is dropped");
    return false;
  }
  out->style.sizePx = sizePx;

  const std::string fam = firstFontFamily(get("font-family", ""));
  if (!fam.empty()) out->style.fontFamily = fam;  // else TextStyle's own default
  out->style.bold = weightIsBold(get("font-weight", "normal"));
  const std::string fstyle = toLowerCopy(trimCopy(get("font-style", "normal")));
  out->style.italic = (fstyle == "italic" || fstyle == "oblique");
  const std::string ls = trimCopy(get("letter-spacing", "normal"));
  out->style.tracking =
      (ls.empty() || toLowerCopy(ls) == "normal") ? 0.0f : resolveLenAxis(ls, 'd', vp, 0.0f);

  const std::string anchor = toLowerCopy(trimCopy(get("text-anchor", "start")));
  out->anchor = anchor == "middle" ? TextAnchor::Middle
                : anchor == "end"  ? TextAnchor::End
                                   : TextAnchor::Start;

  const float elementOpacity = parseOpacityValue(get("opacity", "1"), 1.0f);
  const float fillOpacity = parseOpacityValue(get("fill-opacity", "1"), 1.0f);
  const float strokeOpacity = parseOpacityValue(get("stroke-opacity", "1"), 1.0f);
  const SrgbColor currentColor = resolveCurrentColor(st);
  const std::string label = labelFor(node, tag.c_str());
  out->fill = resolvePaintProperty(ctx, st, "fill", "black", currentColor,
                                   fillOpacity * elementOpacity, label, "fill");
  out->stroke = resolvePaintProperty(ctx, st, "stroke", "none", currentColor,
                                     strokeOpacity * elementOpacity, label, "stroke");
  // Scale 1: layoutRuns() applies the accumulated scale to the stroke at the
  // same moment it applies it to `sizePx`, so both come from one number.
  out->strokeStyle = resolveStrokeStyle(st, vp, 1.0f);
  return true;
}

void emitTextRun(TextCollect& tc, const TextNodeCtx& nc, std::string text) {
  if (!tc.started) ltrimSpaces(&text);
  if (text.empty()) return;  // pending position commands survive to the next run
  TextRun r;
  r.utf8 = std::move(text);
  r.style = nc.style;
  r.fill = nc.fill;
  r.stroke = nc.stroke;
  r.strokeStyle = nc.strokeStyle;
  r.anchor = nc.anchor;
  r.hasAbsX = tc.pendAbsX;
  r.absX = tc.pendX;
  r.hasAbsY = tc.pendAbsY;
  r.absY = tc.pendY;
  r.dx = tc.pendDx;
  r.dy = tc.pendDy;
  tc.pendAbsX = tc.pendAbsY = false;
  tc.pendDx = tc.pendDy = 0.0f;
  tc.started = true;
  tc.runs.push_back(std::move(r));
}

void collectTextRuns(Ctx& ctx, const pugi::xml_node& node, const TextNodeCtx& nc, int depth,
                      const Viewport& vp, const SvgStyleSheet& sheet, const std::string& textLabel,
                      TextCollect& tc) {
  if (tc.refused) return;
  if (depth > kMaxTextNesting) {
    ctx.result->refusals.push_back(textLabel + ": <tspan> nesting deeper than " +
                                   std::to_string(kMaxTextNesting) +
                                   "; the whole element is dropped");
    tc.refused = true;
    return;
  }
  for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
    if (tc.refused) return;
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
      emitTextRun(tc, nc, collapseWhitespace(child.value()));
      continue;
    }
    if (child.type() != pugi::node_element) continue;
    const std::string ctag = child.name();
    if (ctag == "title" || ctag == "desc") continue;
    if (ctag != "tspan") {
      // `<textPath>` and `<tref>` land here by name, and so does anything
      // else. Dropping only the child would silently lose part of a string
      // while the rest imports, which reads as a truncation bug rather than
      // an unsupported feature -- so the whole `<text>` goes.
      ctx.result->refusals.push_back(
          textLabel + ": <" + ctag + "> inside <text> is not supported" +
          (ctag == "textPath" ? " (text on a path)" : "") + "; the whole element is dropped");
      tc.refused = true;
      return;
    }

    const std::string childLabel = labelFor(child, "tspan");
    if (!checkTextRefusedAttrs(ctx, child, childLabel, textLabel)) { tc.refused = true; return; }

    TextNodeCtx childCtx;
    if (!buildTextNodeCtx(ctx, child, "tspan", &nc.es.view, nc.es.inheritedForChildren, sheet, vp,
                          nc.style.sizePx, /*basisKnown=*/true, textLabel, &childCtx)) {
      tc.refused = true;
      return;
    }

    bool has = false;
    float v = 0.0f;
    if (!singleLenAttr(ctx, child, "x", 'x', vp, textLabel, &has, &v)) { tc.refused = true; return; }
    if (has) { tc.pendAbsX = true; tc.pendX = v; }
    if (!singleLenAttr(ctx, child, "y", 'y', vp, textLabel, &has, &v)) { tc.refused = true; return; }
    if (has) { tc.pendAbsY = true; tc.pendY = v; }
    if (!singleLenAttr(ctx, child, "dx", 'x', vp, textLabel, &has, &v)) { tc.refused = true; return; }
    if (has) tc.pendDx += v;
    if (!singleLenAttr(ctx, child, "dy", 'y', vp, textLabel, &has, &v)) { tc.refused = true; return; }
    if (has) tc.pendDy += v;

    collectTextRuns(ctx, child, childCtx, depth + 1, vp, sheet, textLabel, tc);
  }
}

bool paintEq(const Paint& a, const Paint& b) {
  return a.on == b.on && a.rgba == b.rgba;
}

// Whether two adjacent runs are one run. Every field here is a field
// `TextContent` stores exactly once for a whole block, so two runs that agree
// on all of them are indistinguishable from one run holding both strings --
// which is what makes `<text x=.. y=..><tspan>Label</tspan></text>`, the shape
// every Inkscape export emits, still reduce to a single editable Text layer.
bool sameRunStyle(const TextRun& a, const TextRun& b) {
  return a.style.fontFamily == b.style.fontFamily && a.style.sizePx == b.style.sizePx &&
         a.style.tracking == b.style.tracking && a.style.leading == b.style.leading &&
         a.style.bold == b.style.bold && a.style.italic == b.style.italic &&
         paintEq(a.fill, b.fill) && paintEq(a.stroke, b.stroke) &&
         a.strokeStyle.width == b.strokeStyle.width && a.anchor == b.anchor;
}

void mergeTextRuns(std::vector<TextRun>* runs) {
  std::vector<TextRun> merged;
  for (TextRun& r : *runs) {
    if (!merged.empty() && !r.hasAbsX && !r.hasAbsY && r.dx == 0.0f && r.dy == 0.0f &&
        sameRunStyle(merged.back(), r)) {
      merged.back().utf8 += r.utf8;
    } else {
      merged.push_back(std::move(r));
    }
  }
  *runs = std::move(merged);
}

// Places every run's BASELINE start, in the target space `(s, tx, ty)` maps
// local SVG user units into -- `(1, 0, 0)` for the outline path, which lays
// out in local units and transforms the glyph geometry afterwards.
//
// Then the anchor, per CHUNK: SVG starts a new text chunk at every absolute
// position, and `text-anchor` applies to a chunk's total advance. This is an
// ORIGIN SHIFT, not `TextAlign` -- io/SvgImport.hpp section 7b.
void layoutRuns(std::vector<TextRun>* runs, float textX, float textY, float s, float tx,
                 float ty) {
  float penX = s * textX + tx;
  float penY = s * textY + ty;
  size_t chunk = 0;
  for (size_t i = 0; i < runs->size(); ++i) {
    TextRun& r = (*runs)[i];
    r.style.sizePx *= s;
    r.style.tracking *= s;
    r.strokeStyle.width *= s;
    for (float& d : r.strokeStyle.dashes) d *= s;
    r.strokeStyle.dashOffset *= s;

    if ((r.hasAbsX || r.hasAbsY) && i > 0) ++chunk;
    if (r.hasAbsX) penX = s * r.absX + tx;
    if (r.hasAbsY) penY = s * r.absY + ty;
    penX += s * r.dx;
    penY += s * r.dy;

    r.chunk = chunk;
    r.penX = penX;
    r.penY = penY;

    const ShapedText shaped = shapeText(r.utf8, r.style, TextFrame{}, TextAlign::Left);
    r.width = shaped.ok ? shaped.widthPx : 0.0f;
    penX += r.width;
  }

  size_t i = 0;
  while (i < runs->size()) {
    size_t j = i;
    float total = 0.0f;
    while (j < runs->size() && (*runs)[j].chunk == (*runs)[i].chunk) {
      total += (*runs)[j].width;
      ++j;
    }
    float shift = 0.0f;
    if ((*runs)[i].anchor == TextAnchor::Middle) shift = -0.5f * total;
    else if ((*runs)[i].anchor == TextAnchor::End) shift = -total;
    if (shift != 0.0f)
      for (size_t k = i; k < j; ++k) (*runs)[k].penX += shift;
    i = j;
  }
}

// One laid-out run as a point-text `TextContent`.
//
// **There is no baseline conversion here any more, and that is the point.**
// SVG's `x`/`y` on a `<text>` is the baseline (SVG 1.1 10.4), and a point-text
// `TextContent::origin` is now the baseline too (core/TextContent.hpp section
// 2b), so the two agree and `penY` is stored as it arrived. This used to
// subtract a shaped ascent to reach a top-left origin; leaving that in after
// the model changed would have raised every imported label by an ascent.
TextContent runToTextContent(const TextRun& r) {
  TextContent t;
  t.utf8 = r.utf8;
  t.style = r.style;
  t.frame = TextFrame{};        // point text: no width, hence no alignment
  t.align = TextAlign::Left;    // section 7b: the anchor is already in penX
  t.origin = PathPoint{r.penX, r.penY};
  t.fill = r.fill;
  t.stroke = r.stroke;
  t.strokeStyle = r.strokeStyle;
  return t;
}

void processTextElement(Ctx& ctx, const pugi::xml_node& node, const Mat3& accum,
                         const Viewport& vp, const SvgElementView* parentView,
                         const std::map<std::string, std::string>& inherited,
                         const SvgStyleSheet& sheet) {
  const std::string label = labelFor(node, "text");

  // No shaper, no text -- and no outline fallback either, since outlining
  // needs the same `shapeText()` call. Named rather than dropped.
  if (!shaperAvailable()) {
    ctx.result->refusals.push_back(label + ": text not imported -- " +
                                   std::string(shaperUnavailableReason()));
    return;
  }
  if (!checkTextRefusedAttrs(ctx, node, "<text>", label)) return;

  TextNodeCtx nc;
  if (!buildTextNodeCtx(ctx, node, "text", parentView, inherited, sheet, vp, 16.0f,
                        /*basisKnown=*/false, label, &nc))
    return;
  checkMaskFilter(ctx, nc.es.style, label);

  Mat3 ownT = mat3Identity();
  if (node.attribute("transform")) {
    Mat3 t;
    if (parseSvgTransform(node.attribute("transform").value(), &t)) ownT = t;
  }
  const Mat3 accumAtText = mat3Multiply(accum, ownT);

  bool has = false;
  float textX = 0.0f, textY = 0.0f;
  if (!singleLenAttr(ctx, node, "x", 'x', vp, label, &has, &textX)) return;
  if (!singleLenAttr(ctx, node, "y", 'y', vp, label, &has, &textY)) return;
  float dxAttr = 0.0f, dyAttr = 0.0f;
  if (!singleLenAttr(ctx, node, "dx", 'x', vp, label, &has, &dxAttr)) return;
  if (has) textX += dxAttr;
  if (!singleLenAttr(ctx, node, "dy", 'y', vp, label, &has, &dyAttr)) return;
  if (has) textY += dyAttr;

  TextCollect tc;
  collectTextRuns(ctx, node, nc, 0, vp, sheet, label, tc);
  if (tc.refused) return;
  while (!tc.runs.empty()) {  // trailing whitespace, across however many runs it spans
    rtrimSpaces(&tc.runs.back().utf8);
    if (!tc.runs.back().utf8.empty()) break;
    tc.runs.pop_back();
  }
  mergeTextRuns(&tc.runs);
  if (tc.runs.empty()) return;  // `<text/>` with nothing in it: nothing drawn, nothing to say

  // A clip-path cannot ride on a `TextContent` -- it is a `VectorShape` field
  // -- so a clipped `<text>` takes the outline path, where every glyph is a
  // shape that CAN carry one.
  const bool hasClip = [&] {
    auto it = nc.es.style.find("clip-path");
    if (it == nc.es.style.end()) return false;
    const std::string v = trimCopy(it->second);
    return !v.empty() && toLowerCopy(v) != "none";
  }();

  float s = 1.0f, tx = 0.0f, ty = 0.0f;
  const bool similarity = decomposeTranslateScale(accumAtText, &s, &tx, &ty);
  const bool oneRun = tc.runs.size() == 1;

  if (oneRun && similarity && !hasClip) {
    layoutRuns(&tc.runs, textX, textY, s, tx, ty);
    SvgTextBlock blk;
    blk.shapesBefore = ctx.result->shapes.size();
    blk.name = attrStr(node, "id");
    blk.content = runToTextContent(tc.runs[0]);
    ctx.result->texts.push_back(std::move(blk));
    return;
  }

  // The fallback, named. io/SvgImport.hpp section 7: never silent.
  std::string why;
  if (!oneRun) {
    bool reposition = false, delta = false;
    for (size_t i = 1; i < tc.runs.size(); ++i) {
      if (tc.runs[i].hasAbsX || tc.runs[i].hasAbsY) reposition = true;
      if (tc.runs[i].dx != 0.0f || tc.runs[i].dy != 0.0f) delta = true;
    }
    why = reposition ? "a <tspan> repositions the text"
          : delta    ? "a <tspan> offsets the text with dx/dy"
                     : "a <tspan> carries its own style, and one TextContent holds one style";
    why += " (" + std::to_string(tc.runs.size()) + " runs)";
  }
  if (!similarity) {
    if (!why.empty()) why += "; and ";
    why += "its transform has rotation, skew or a mirror in it, which a TextContent has no "
           "matrix to carry";
  }
  if (hasClip) {
    if (!why.empty()) why += "; and ";
    why += "it carries a clip-path, which only a shape can hold";
  }
  ctx.result->refusals.push_back(
      label + ": imported as glyph OUTLINES, not an editable Text layer -- " + why);

  layoutRuns(&tc.runs, textX, textY, 1.0f, 0.0f, 0.0f);
  const float sf = transformScaleFactor(accumAtText);
  const std::string name = attrStr(node, "id");
  std::optional<Path> clip;
  resolveClipPath(ctx, nc.es.style, accumAtText, vp, label, &clip);
  if (ctx.aborted) return;

  for (const TextRun& r : tc.runs) {
    std::string err;
    std::vector<VectorShape> glyphs = textContentToShapes(runToTextContent(r), &err);
    if (!err.empty()) {
      ctx.result->refusals.push_back(label + ": " + err);
      return;
    }
    for (VectorShape& g : glyphs) {
      transformPathInPlace(g.path, accumAtText);
      g.strokeStyle.width *= sf;
      for (float& d : g.strokeStyle.dashes) d *= sf;
      g.strokeStyle.dashOffset *= sf;
      g.clip = clip;
      g.id = 0;  // assigned by whoever puts these in a layer -- see OpenAnyFile
      g.name = name;
      if (!addAnchorsWithCap(ctx, countAnchors(g.path))) return;
      ctx.result->shapes.push_back(std::move(g));
    }
  }
}

// --------------------------------------------------------------------------
// The recursive walk
// --------------------------------------------------------------------------

void visit(Ctx& ctx, pugi::xml_node node, int depth, int useDepth, const Mat3& accum,
           const SvgElementView* parentView, const std::map<std::string, std::string>& inheritedFromParent,
           const SvgStyleSheet& sheet, Viewport vp) {
  if (ctx.aborted) return;

  if (depth > kMaxSvgNestingDepth) {
    if (!ctx.cappedDepth) {
      ctx.result->refusals.push_back("nesting depth exceeded (" +
                                     std::to_string(kMaxSvgNestingDepth) + "); import stopped");
      ctx.cappedDepth = true;
    }
    ctx.aborted = true;
    return;
  }

  ++ctx.elementCount;
  if (ctx.elementCount > kMaxSvgElements) {
    if (!ctx.cappedElements) {
      ctx.result->refusals.push_back("element cap exceeded (" + std::to_string(kMaxSvgElements) +
                                     "); import stopped");
      ctx.cappedElements = true;
    }
    ctx.aborted = true;
    return;
  }

  const std::string tag = node.name();

  if (tag == "title" || tag == "desc") return;  // ignored, not refused -- per the brief

  // Non-rendering definitions: their content is only reachable through a
  // reference (`url(#id)`, `<use>`), never by being visited structurally.
  // Encountering one of these directly in the tree is ordinary, legal SVG
  // (most authors do not bother wrapping every definition in <defs>), so it
  // is skipped silently rather than refused.
  static const std::set<std::string> kSilentContainers = {
      "defs", "linearGradient", "radialGradient", "clipPath",
      "pattern", "mask", "filter", "symbol", "style",
  };
  if (kSilentContainers.count(tag)) return;

  static const std::unordered_map<std::string, std::string> kRefuseNoDescend = {
      {"switch", "conditional rendering not supported"},
      {"foreignObject", "embedded foreign content not supported"},
      {"image", "raster/external image embedding not supported"},
      {"script", "scripts are not evaluated by this importer"},
      {"animate", "animation elements are not supported"},
      {"animateTransform", "animation elements are not supported"},
      {"animateMotion", "animation elements are not supported"},
      {"animateColor", "animation elements are not supported"},
      {"set", "animation elements are not supported"},
      {"a", "hyperlink elements are not supported"},
  };
  {
    auto rit = kRefuseNoDescend.find(tag);
    if (rit != kRefuseNoDescend.end()) {
      ctx.result->refusals.push_back(labelFor(node, tag.c_str()) + ": " + rit->second);
      return;
    }
  }

  if (isSvgTag(tag)) {
    ElemStyle es = computeElemStyle(node, tag, parentView, inheritedFromParent, sheet);
    const std::string label = labelFor(node, "svg");
    checkMaskFilter(ctx, es.style, label);
    checkContainerClipPath(ctx, es.style, label);

    Mat3 ownT = mat3Identity();
    if (node.attribute("transform")) {
      Mat3 t;
      if (parseSvgTransform(node.attribute("transform").value(), &t)) ownT = t;
    }
    Mat3 accumAtElem = mat3Multiply(accum, ownT);

    const bool isRoot = (depth == 0);
    const float x = (!isRoot && node.attribute("x")) ? resolveLenAxis(attrStr(node, "x"), 'x', vp, 0.0f) : 0.0f;
    const float y = (!isRoot && node.attribute("y")) ? resolveLenAxis(attrStr(node, "y"), 'y', vp, 0.0f) : 0.0f;
    accumAtElem = mat3Multiply(accumAtElem, transformTranslate(x, y));

    float w = node.attribute("width") ? resolveLenAxis(attrStr(node, "width"), 'x', vp, 0.0f)
                                       : (isRoot ? 0.0f : vp.w);
    float h = node.attribute("height") ? resolveLenAxis(attrStr(node, "height"), 'y', vp, 0.0f)
                                        : (isRoot ? 0.0f : vp.h);

    SvgViewBox box{};
    const bool hasBox = node.attribute("viewBox") && parseSvgViewBox(attrStr(node, "viewBox"), &box);

    if (isRoot) {
      if (w <= 0.0f && hasBox) w = box.width;
      if (h <= 0.0f && hasBox) h = box.height;
      if (w <= 0.0f) w = 300.0f;  // SVG's own UA default replaced-element size
      if (h <= 0.0f) h = 150.0f;
      ctx.result->widthPx = w;
      ctx.result->heightPx = h;
    }

    Viewport childVp{w, h};
    Mat3 accumForChildren = accumAtElem;
    if (hasBox) {
      if (box.width <= 0.0f || box.height <= 0.0f) return;  // spec: disables rendering
      SvgPreserveAspectRatio par;
      if (node.attribute("preserveAspectRatio"))
        parseSvgPreserveAspectRatio(attrStr(node, "preserveAspectRatio"), &par);
      accumForChildren = mat3Multiply(accumForChildren, svgViewBoxTransform(box, w, h, par));
      childVp = Viewport{box.width, box.height};
    }

    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
      if (ctx.aborted) return;
      if (child.type() != pugi::node_element) continue;
      visit(ctx, child, depth + 1, useDepth, accumForChildren, &es.view, es.inheritedForChildren,
            sheet, childVp);
    }
    return;
  }

  if (tag == "g") {
    ElemStyle es = computeElemStyle(node, tag, parentView, inheritedFromParent, sheet);
    const std::string label = labelFor(node, "g");
    checkMaskFilter(ctx, es.style, label);
    checkContainerClipPath(ctx, es.style, label);

    Mat3 ownT = mat3Identity();
    if (node.attribute("transform")) {
      Mat3 t;
      if (parseSvgTransform(node.attribute("transform").value(), &t)) ownT = t;
    }
    const Mat3 accumAtElem = mat3Multiply(accum, ownT);

    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
      if (ctx.aborted) return;
      if (child.type() != pugi::node_element) continue;
      visit(ctx, child, depth + 1, useDepth, accumAtElem, &es.view, es.inheritedForChildren, sheet, vp);
    }
    return;
  }

  if (tag == "use") {
    ElemStyle es = computeElemStyle(node, tag, parentView, inheritedFromParent, sheet);
    const std::string label = labelFor(node, "use");
    checkMaskFilter(ctx, es.style, label);
    checkContainerClipPath(ctx, es.style, label);

    Mat3 ownT = mat3Identity();
    if (node.attribute("transform")) {
      Mat3 t;
      if (parseSvgTransform(node.attribute("transform").value(), &t)) ownT = t;
    }
    const Mat3 accumAtUse = mat3Multiply(accum, ownT);
    const float ux = resolveLenAxis(attrStr(node, "x", "0"), 'x', vp, 0.0f);
    const float uy = resolveLenAxis(attrStr(node, "y", "0"), 'y', vp, 0.0f);
    const Mat3 accumForTarget = mat3Multiply(accumAtUse, transformTranslate(ux, uy));

    std::string href = attrStr(node, "href");
    if (href.empty()) href = attrStr(node, "xlink:href");
    if (href.empty()) {
      ctx.result->refusals.push_back("use on " + label + ": missing href");
      return;
    }
    if (href[0] != '#') {
      ctx.result->refusals.push_back("use on " + label + ": external reference '" + href +
                                     "' not supported");
      return;
    }
    const std::string idref = href.substr(1);

    // Depth cap: checked before recursing, so a self-referencing <use>
    // (id="a" containing <use href="#a"/>) returns after kMaxSvgUseDepth
    // frames rather than recursing forever.
    if (useDepth + 1 > kMaxSvgUseDepth) {
      ctx.result->refusals.push_back("use on " + label + ": use-chain depth exceeded (" +
                                     std::to_string(kMaxSvgUseDepth) + "); this branch stopped");
      return;
    }
    // Total-expansion cap: checked before recursing, so an exponential fan-
    // out (each level's target containing several more <use>s) is refused
    // after this many expansions regardless of how deep the fan-out could
    // otherwise go -- the check bounds total work, not just one chain.
    ++ctx.useExpansionCount;
    if (ctx.useExpansionCount > kMaxSvgUseExpansions) {
      if (!ctx.cappedUseExpansions) {
        ctx.result->refusals.push_back("use expansion cap exceeded (" +
                                       std::to_string(kMaxSvgUseExpansions) + "); import stopped");
        ctx.cappedUseExpansions = true;
      }
      ctx.aborted = true;
      return;
    }

    auto it = ctx.idIndex.find(idref);
    if (it == ctx.idIndex.end()) {
      ctx.result->refusals.push_back("use on " + label + ": unknown id #" + idref);
      return;
    }
    pugi::xml_node target = it->second;
    if (std::string(target.name()) == "symbol") {
      ctx.result->refusals.push_back("use on " + label + ": target #" + idref +
                                     " is a <symbol> (not supported)");
      return;
    }

    // Modelled as if the referenced element were reparented directly under
    // the <use> element -- the use's own computed style becomes the
    // inherited context for the target, and the use's own view becomes the
    // ancestor selectors see. The real spec generates an invisible shadow
    // <g>; this is a stated, simpler approximation that gets the cascade
    // and the transform right and differs only for a selector written
    // specifically against a synthetic "g" ancestor, which no real file
    // can do (there is no way to address the shadow node from author CSS).
    visit(ctx, target, depth + 1, useDepth + 1, accumForTarget, &es.view, es.inheritedForChildren,
          sheet, vp);
    return;
  }

  // <text> owns its own subtree: processTextElement() walks the `<tspan>`s
  // itself, because a run's position depends on the runs before it and this
  // generic walk has nowhere to keep a pen. See io/SvgImport.hpp section 7.
  if (tag == "text") {
    processTextElement(ctx, node, accum, vp, parentView, inheritedFromParent, sheet);
    return;
  }
  if (tag == "tspan" || tag == "textPath" || tag == "tref") {
    // Only reachable OUTSIDE a `<text>` -- inside one these never come back
    // here. SVG defines no rendering for them there, so this is not a
    // capability refusal but a malformed-document one, and it says so.
    ctx.result->refusals.push_back(labelFor(node, tag.c_str()) +
                                   ": only renders inside a <text> element");
    return;
  }

  static const std::set<std::string> kBasicShapes = {
      "rect", "circle", "ellipse", "line", "polyline", "polygon", "path",
  };
  if (kBasicShapes.count(tag)) {
    ElemStyle es = computeElemStyle(node, tag, parentView, inheritedFromParent, sheet);
    const std::string label = labelFor(node, tag.c_str());
    checkMaskFilter(ctx, es.style, label);

    Mat3 ownT = mat3Identity();
    if (node.attribute("transform")) {
      Mat3 t;
      if (parseSvgTransform(node.attribute("transform").value(), &t)) ownT = t;
    }
    const Mat3 accumAtShape = mat3Multiply(accum, ownT);
    const SrgbColor currentColor = resolveCurrentColor(es.style);
    processShapeElement(ctx, node, tag, accumAtShape, vp, es.style, currentColor, label);
    return;
  }

  // Anything else: a vendor extension (`sodipodi:namedview`, `metadata`),
  // a typo, or a future SVG element this file has never heard of. Refused
  // by name rather than silently dropped or guessed at -- see section 5.
  ctx.result->refusals.push_back(labelFor(node, tag.c_str()) + ": unsupported element");
}

// --------------------------------------------------------------------------
// The id index and <style> text prescan
// --------------------------------------------------------------------------

void prescan(const pugi::xml_node& node, int depth, std::unordered_map<std::string, pugi::xml_node>* idIndex,
             std::string* styleText) {
  if (depth > kMaxSvgNestingDepth) return;
  if (node.type() == pugi::node_element) {
    const std::string id = attrStr(node, "id");
    if (!id.empty() && idIndex->find(id) == idIndex->end()) (*idIndex)[id] = node;
    if (std::string(node.name()) == "style") {
      *styleText += node.child_value();
      *styleText += "\n";
    }
  }
  for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling())
    prescan(child, depth + 1, idIndex, styleText);
}

}  // namespace

SvgImportResult importSvg(const uint8_t* data, size_t size) {
  SvgImportResult result;
  if (!data || size == 0) {
    result.error = "empty buffer";
    return result;
  }

  pugi::xml_document doc;
  const pugi::xml_parse_result pr = doc.load_buffer(data, size, pugi::parse_default, pugi::encoding_utf8);
  if (!pr) {
    result.error = std::string("XML parse error: ") + pr.description();
    return result;
  }

  pugi::xml_node root = doc.first_child();
  while (root && root.type() != pugi::node_element) root = root.next_sibling();
  if (!root) {
    result.error = "no root element";
    return result;
  }
  const std::string rootTag = root.name();
  if (!isSvgTag(rootTag)) {
    result.error = "root element is not <svg> (found <" + rootTag + ">)";
    return result;
  }

  Ctx ctx;
  ctx.result = &result;
  std::string styleText;
  prescan(root, 0, &ctx.idIndex, &styleText);

  SvgStyleSheet sheet;
  std::vector<std::string> styleRefusals;
  parseSvgStyleSheet(styleText, &sheet, &styleRefusals);
  for (const std::string& r : styleRefusals) result.refusals.push_back("<style>: " + r);

  visit(ctx, root, 0, 0, mat3Identity(), nullptr, {}, sheet, Viewport{0.0f, 0.0f});

  // Reaching here means the file parsed as XML and had an <svg> root.
  // Every cap trip and every refused construct above is reported through
  // `refusals`, not through `ok` -- `ok == false` is reserved for the two
  // structural failures checked above (see this file's own header for the
  // rationale, matching io/PsdImport.hpp's `ok`/`warnings` split).
  result.ok = true;
  return result;
}

SvgImportResult importSvgFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    SvgImportResult r;
    r.error = std::string("cannot open ") + path;
    return r;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return importSvg(bytes.data(), bytes.size());
}

}  // namespace np
