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
#include "io/SvgPath.hpp"
#include "io/SvgStyle.hpp"
#include "ops/Transform.hpp"
#include "pugixml/pugixml.hpp"

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
      {"text", "text rendering deferred to Stage 5"},
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
