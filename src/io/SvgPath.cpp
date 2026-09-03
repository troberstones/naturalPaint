#include "io/SvgPath.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/PathFlatten.hpp"

namespace np {

namespace {

// --------------------------------------------------------------------------
// The shared lexer: whitespace, separators, numbers, flags.
// --------------------------------------------------------------------------

bool isAsciiSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }

void skipWsp(std::string_view s, size_t* pos) {
  while (*pos < s.size() && isAsciiSpace(s[*pos])) ++*pos;
}

// SVG's "comma-wsp": any whitespace, optionally with a single comma
// somewhere in it. Used between arguments; never required (a sign or a
// second `.` already separates two numbers -- see lexNumber()), only
// consumed when present.
void skipSeparator(std::string_view s, size_t* pos) {
  skipWsp(s, pos);
  if (*pos < s.size() && s[*pos] == ',') {
    ++*pos;
    skipWsp(s, pos);
  }
}

bool isNumberStart(char c) {
  return isAsciiDigit(c) || c == '.' || c == '+' || c == '-';
}

// Reads one number at `*pos`, advancing it past the number on success only.
// This is the piece real files break a naive lexer on -- see io/SvgPath.hpp's
// documentation of `parseSvgPathData()`. The boundary rules, in the order
// applied:
//
//   * an optional leading sign, consumed once, at the very start;
//   * digits, then optionally a `.` and more digits -- a *second* `.` is
//     simply never reached by this step, so it starts the next number
//     instead of erroring ("1.5.5" -> "1.5", ".5");
//   * an optional exponent, consumed only when `e`/`E` is followed by an
//     optional sign and at least one digit -- so a mid-number sign that does
//     NOT follow an `e`/`E` was never eligible to be consumed here and
//     therefore starts the next number instead ("10-5" -> "10", "-5"), and a
//     stray trailing `e` with no exponent digits is left for the next call
//     (which will fail on it, correctly: `e` alone is not a number).
//
// A non-finite result (a mantissa this large only arises from something like
// `1e400`) is treated as a failure to lex, at the *start* of the token, so
// callers report the refusal there rather than after silently accepting an
// infinity.
bool lexNumber(std::string_view s, size_t* pos, float* out) {
  const size_t n = s.size();
  size_t i = *pos;
  const size_t start = i;

  if (i < n && (s[i] == '+' || s[i] == '-')) ++i;

  bool sawDigit = false;
  while (i < n && isAsciiDigit(s[i])) {
    ++i;
    sawDigit = true;
  }
  if (i < n && s[i] == '.') {
    ++i;
    while (i < n && isAsciiDigit(s[i])) {
      ++i;
      sawDigit = true;
    }
  }
  if (!sawDigit) return false;

  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
    size_t k = j;
    while (k < n && isAsciiDigit(s[k])) ++k;
    if (k > j) i = k;  // else: leave the bare/broken 'e' for the next token
  }

  // `std::from_chars` for floats is not available on the deployment target
  // this build has to support, so the token goes through `strtof()` on a
  // null-terminated copy instead -- the same choice brush/BrushModelIo.cpp
  // and app/UserBrushLibrary.cpp already made, and the same reasoning: the
  // C locale, because nothing in this application calls `setlocale()`, so
  // '.' is the decimal point here too. `strtof()`'s own grammar is a
  // superset of what we bounded above (it also accepts a leading '+'), so
  // it cannot parse *more* than `buf` holds -- the `endPtr` check below is
  // what would catch it if that were ever not true.
  const size_t len = i - start;
  if (len >= 64) return false;  // no legal SVG number is remotely this long
  char buf[64];
  std::memcpy(buf, s.data() + start, len);
  buf[len] = '\0';
  char* endPtr = nullptr;
  const float value = std::strtof(buf, &endPtr);
  if (endPtr != buf + len) return false;
  if (!std::isfinite(value)) return false;

  *pos = i;
  *out = value;
  return true;
}

// A flag argument in the `A`/`a` command: exactly one `0` or `1` character,
// no sign, no further digits -- so "0,1" is two flags and, more to the
// point, "01" (no separator at all) is also two flags, which real arc
// exporters rely on.
bool lexFlag(std::string_view s, size_t* pos, bool* out) {
  size_t i = *pos;
  skipSeparator(s, &i);
  if (i < s.size() && (s[i] == '0' || s[i] == '1')) {
    *out = s[i] == '1';
    *pos = i + 1;
    return true;
  }
  return false;
}

// Reads `count` numbers, each preceded by an optional separator. `*pos` is
// left at the failure point (which may be partway into the argument list) on
// a `false` return, matching parseSvgPathData()'s "prefix, not partial
// command" contract -- callers never apply a partially-read command.
bool readNumbers(std::string_view s, size_t* pos, float* out, int count) {
  size_t i = *pos;
  for (int k = 0; k < count; ++k) {
    if (k > 0) skipSeparator(s, &i);
    if (!lexNumber(s, &i, &out[k])) return false;
  }
  *pos = i;
  return true;
}

// --------------------------------------------------------------------------
// The `d` grammar's command interpreter.
// --------------------------------------------------------------------------

// Which of the two reflectable curve families the previous command belonged
// to, so `S`/`T` know whether to reflect or fall back to the current point.
enum class LastCurve { None, CubicOrS, QuadOrT };

struct PathParseState {
  Path path;
  PathPoint current{0.0f, 0.0f};
  PathPoint subpathStart{0.0f, 0.0f};
  LastCurve lastCurve = LastCurve::None;
  // Absolute control point to reflect: the last cubic's c2 (for a following
  // S) or the last quadratic's own q1 (for a following T) -- NOT the cubic
  // c1/c2 a Q/T is elevated to, which is a different point.
  PathPoint lastControl{0.0f, 0.0f};
  // Set right after 'Z': the next drawn command opens a new subpath at
  // `subpathStart` rather than extending the (already closed) current one.
  bool needsNewSubpathAfterClose = false;
};

SubPath& currentSubpath(PathParseState* st) { return st->path.subpaths.back(); }

// Starts a fresh subpath at `p` -- for 'M'/'m', and for the subpath a
// drawing command implicitly opens right after a 'Z'.
void openSubpath(PathParseState* st, PathPoint p) {
  SubPath sub;
  Anchor a;
  a.pt = a.in = a.out = p;
  sub.anchors.push_back(a);
  st->path.subpaths.push_back(std::move(sub));
  st->current = p;
  st->subpathStart = p;
  st->lastCurve = LastCurve::None;
  st->needsNewSubpathAfterClose = false;
}

// Appends a straight segment ending at `p`. The previous anchor's `out`
// handle is left untouched -- it already defaults to that anchor's own
// point, which is what a straight line requires -- and the new anchor's
// handles are both set to `p` for the same reason.
void appendLine(PathParseState* st, PathPoint p) {
  if (st->needsNewSubpathAfterClose) {
    openSubpath(st, st->subpathStart);
    st->needsNewSubpathAfterClose = false;
  }
  Anchor a;
  a.pt = a.in = a.out = p;
  currentSubpath(st).anchors.push_back(a);
  st->current = p;
  st->lastCurve = LastCurve::None;
}

// Appends a cubic segment: sets the previous anchor's outgoing handle to
// `c1`, then a new anchor at `end` whose incoming handle is `c2`.
void appendCubic(PathParseState* st, PathPoint c1, PathPoint c2, PathPoint end) {
  if (st->needsNewSubpathAfterClose) {
    openSubpath(st, st->subpathStart);
    st->needsNewSubpathAfterClose = false;
  }
  currentSubpath(st).anchors.back().out = c1;
  Anchor a;
  a.pt = end;
  a.in = c2;
  a.out = end;
  currentSubpath(st).anchors.push_back(a);
  st->current = end;
}

PathPoint reflect(PathPoint current, PathPoint control) {
  return {2.0f * current.x - control.x, 2.0f * current.y - control.y};
}

}  // namespace

bool parseSvgPathData(std::string_view d, Path* out, size_t* errorOffset) {
  if (!out || !errorOffset) return false;
  *out = Path{};

  PathParseState st;
  size_t pos = 0;
  skipWsp(d, &pos);

  if (pos >= d.size() || (d[pos] != 'M' && d[pos] != 'm')) {
    // Covers an empty or whitespace-only `d` (pos == d.size()) and any `d`
    // whose first token is not a moveto, per SVG 1.1 8.3.1.
    *errorOffset = pos;
    return false;
  }

  while (true) {
    skipWsp(d, &pos);
    if (pos >= d.size()) break;

    char cmd = d[pos];
    bool isLetter = (cmd >= 'A' && cmd <= 'Z') || (cmd >= 'a' && cmd <= 'z');
    if (!isLetter) {
      // A number with no command in force -- e.g. a bare argument set
      // following 'Z', which takes none to repeat.
      *errorOffset = pos;
      *out = st.path;
      return false;
    }
    switch (cmd) {
      case 'M': case 'm': case 'L': case 'l': case 'H': case 'h':
      case 'V': case 'v': case 'C': case 'c': case 'S': case 's':
      case 'Q': case 'q': case 'T': case 't': case 'A': case 'a':
      case 'Z': case 'z':
        break;
      default:
        *errorOffset = pos;
        *out = st.path;
        return false;
    }
    ++pos;

    if (cmd == 'Z' || cmd == 'z') {
      currentSubpath(&st).closed = true;
      st.current = st.subpathStart;
      st.lastCurve = LastCurve::None;
      st.needsNewSubpathAfterClose = true;
      continue;
    }

    const bool relative = cmd >= 'a' && cmd <= 'z';
    char effective = cmd;  // for M/m, repeats after the first pair are L/l

    bool firstArgSet = true;
    while (true) {
      if (!firstArgSet) {
        size_t peek = pos;
        skipSeparator(d, &peek);
        if (peek >= d.size() || !isNumberStart(d[peek])) break;
        pos = peek;
      } else {
        skipSeparator(d, &pos);
      }

      const size_t argStart = pos;
      switch (effective) {
        case 'M': case 'm': case 'L': case 'l': {
          float xy[2];
          if (!readNumbers(d, &pos, xy, 2)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint p{xy[0], xy[1]};
          if (relative) { p.x += st.current.x; p.y += st.current.y; }
          if (effective == 'M' || effective == 'm') {
            openSubpath(&st, p);
            effective = relative ? 'l' : 'L';
          } else {
            appendLine(&st, p);
          }
          break;
        }
        case 'H': case 'h': {
          float x[1];
          if (!readNumbers(d, &pos, x, 1)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint p{relative ? st.current.x + x[0] : x[0], st.current.y};
          appendLine(&st, p);
          break;
        }
        case 'V': case 'v': {
          float y[1];
          if (!readNumbers(d, &pos, y, 1)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint p{st.current.x, relative ? st.current.y + y[0] : y[0]};
          appendLine(&st, p);
          break;
        }
        case 'C': case 'c': {
          float n6[6];
          if (!readNumbers(d, &pos, n6, 6)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint c1{n6[0], n6[1]}, c2{n6[2], n6[3]}, end{n6[4], n6[5]};
          if (relative) {
            c1.x += st.current.x; c1.y += st.current.y;
            c2.x += st.current.x; c2.y += st.current.y;
            end.x += st.current.x; end.y += st.current.y;
          }
          appendCubic(&st, c1, c2, end);
          st.lastCurve = LastCurve::CubicOrS;
          st.lastControl = c2;
          break;
        }
        case 'S': case 's': {
          float n4[4];
          if (!readNumbers(d, &pos, n4, 4)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint c2{n4[0], n4[1]}, end{n4[2], n4[3]};
          if (relative) {
            c2.x += st.current.x; c2.y += st.current.y;
            end.x += st.current.x; end.y += st.current.y;
          }
          PathPoint c1 = (st.lastCurve == LastCurve::CubicOrS)
                             ? reflect(st.current, st.lastControl)
                             : st.current;
          appendCubic(&st, c1, c2, end);
          st.lastCurve = LastCurve::CubicOrS;
          st.lastControl = c2;
          break;
        }
        case 'Q': case 'q': {
          float n4[4];
          if (!readNumbers(d, &pos, n4, 4)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint q1{n4[0], n4[1]}, end{n4[2], n4[3]};
          if (relative) {
            q1.x += st.current.x; q1.y += st.current.y;
            end.x += st.current.x; end.y += st.current.y;
          }
          const PathPoint p0 = st.current;
          PathPoint c1{p0.x + (2.0f / 3.0f) * (q1.x - p0.x), p0.y + (2.0f / 3.0f) * (q1.y - p0.y)};
          PathPoint c2{end.x + (2.0f / 3.0f) * (q1.x - end.x), end.y + (2.0f / 3.0f) * (q1.y - end.y)};
          appendCubic(&st, c1, c2, end);
          st.lastCurve = LastCurve::QuadOrT;
          st.lastControl = q1;
          break;
        }
        case 'T': case 't': {
          float xy[2];
          if (!readNumbers(d, &pos, xy, 2)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          PathPoint end{xy[0], xy[1]};
          if (relative) { end.x += st.current.x; end.y += st.current.y; }
          PathPoint q1 = (st.lastCurve == LastCurve::QuadOrT)
                             ? reflect(st.current, st.lastControl)
                             : st.current;
          const PathPoint p0 = st.current;
          PathPoint c1{p0.x + (2.0f / 3.0f) * (q1.x - p0.x), p0.y + (2.0f / 3.0f) * (q1.y - p0.y)};
          PathPoint c2{end.x + (2.0f / 3.0f) * (q1.x - end.x), end.y + (2.0f / 3.0f) * (q1.y - end.y)};
          appendCubic(&st, c1, c2, end);
          st.lastCurve = LastCurve::QuadOrT;
          st.lastControl = q1;
          break;
        }
        case 'A': case 'a': {
          float rrot[3];
          if (!readNumbers(d, &pos, rrot, 3)) {
            *errorOffset = argStart;
            *out = st.path;
            return false;
          }
          bool largeArc = false, sweep = false;
          if (!lexFlag(d, &pos, &largeArc)) {
            *errorOffset = pos;
            *out = st.path;
            return false;
          }
          if (!lexFlag(d, &pos, &sweep)) {
            *errorOffset = pos;
            *out = st.path;
            return false;
          }
          skipSeparator(d, &pos);
          float xy[2];
          const size_t xyStart = pos;
          if (!readNumbers(d, &pos, xy, 2)) {
            *errorOffset = xyStart;
            *out = st.path;
            return false;
          }
          PathPoint end{xy[0], xy[1]};
          if (relative) { end.x += st.current.x; end.y += st.current.y; }

          if (st.needsNewSubpathAfterClose) {
            openSubpath(&st, st.subpathStart);
            st.needsNewSubpathAfterClose = false;
          }

          // The lifetime hazard io/SvgPath.hpp warns about: `fromOut` and
          // the piece buffer must NOT alias the subpath's own anchor vector,
          // because appending to that vector inside appendCubic()-style
          // logic would be fine, but arcToCubics() itself pushes into
          // whatever vector it is given, and a pointer into that same
          // vector's storage is invalidated by its own first push_back.
          // Keeping both purely local sidesteps the question entirely.
          PathPoint fromOut;
          std::vector<Anchor> pieces;
          const bool curved = arcToCubics(st.current, rrot[0], rrot[1], rrot[2], largeArc,
                                           sweep, end, &fromOut, &pieces);
          if (curved) {
            currentSubpath(&st).anchors.back().out = fromOut;
            for (const Anchor& a : pieces) currentSubpath(&st).anchors.push_back(a);
            st.current = end;
          } else {
            // SVG's own fallback for a zero-radius or coincident-endpoint
            // arc: a line, not an error.
            appendLine(&st, end);
          }
          st.lastCurve = LastCurve::None;
          break;
        }
        default:
          // Unreachable: `effective` only ever holds a value validated by
          // the outer switch (or 'L'/'l' derived from a validated 'M'/'m').
          break;
      }
      firstArgSet = false;
    }
  }

  *out = st.path;
  return true;
}

// --------------------------------------------------------------------------
// transform / gradientTransform
// --------------------------------------------------------------------------

namespace {

// Reads one function's numeric argument list, `name(n0, n1, ...)`, into
// `nums` (sized to the maximum this grammar allows, `count` filled in with
// how many were actually present). `minArgs`/`maxArgs` bound how many are
// legal for the function already matched by name -- e.g. `translate` allows
// 1 or 2.
bool readTransformArgs(std::string_view s, size_t* pos, float* nums, int minArgs, int maxArgs,
                        int* count) {
  size_t i = *pos;
  skipWsp(s, &i);
  int n = 0;
  while (n < maxArgs) {
    if (n > 0) {
      size_t peek = i;
      skipSeparator(s, &peek);
      if (peek >= s.size() || !isNumberStart(s[peek])) break;
      i = peek;
    }
    if (!lexNumber(s, &i, &nums[n])) {
      if (n == 0) break;  // zero arguments read; caller checks minArgs
      return false;
    }
    ++n;
  }
  skipWsp(s, &i);
  if (i >= s.size() || s[i] != ')') return false;
  ++i;
  if (n < minArgs || n > maxArgs) return false;
  *pos = i;
  *count = n;
  return true;
}

}  // namespace

bool parseSvgTransform(std::string_view text, Mat3* out) {
  if (!out) return false;
  Mat3 acc = mat3Identity();
  size_t pos = 0;
  skipWsp(text, &pos);
  if (pos >= text.size()) return false;  // "" is not a valid transform list

  bool any = false;
  while (pos < text.size()) {
    size_t nameStart = pos;
    while (pos < text.size() && text[pos] >= 'a' && text[pos] <= 'z') ++pos;
    // Allow the mixed-case letters skewX/skewY actually use.
    while (pos < text.size() && text[pos] >= 'A' && text[pos] <= 'Z') ++pos;
    std::string_view name = text.substr(nameStart, pos - nameStart);
    skipWsp(text, &pos);
    if (pos >= text.size() || text[pos] != '(') return false;
    ++pos;

    float args[6];
    int count = 0;
    Mat3 m;
    if (name == "matrix") {
      if (!readTransformArgs(text, &pos, args, 6, 6, &count)) return false;
      m.m = {args[0], args[2], args[4], args[1], args[3], args[5], 0.0f, 0.0f, 1.0f};
    } else if (name == "translate") {
      if (!readTransformArgs(text, &pos, args, 1, 2, &count)) return false;
      m = transformTranslate(args[0], count == 2 ? args[1] : 0.0f);
    } else if (name == "scale") {
      if (!readTransformArgs(text, &pos, args, 1, 2, &count)) return false;
      m = transformScale(args[0], count == 2 ? args[1] : args[0]);
    } else if (name == "rotate") {
      if (!readTransformArgs(text, &pos, args, 1, 3, &count)) return false;
      if (count == 3) {
        m = transformRotateDegreesAbout(args[0], Point2{args[1], args[2]});
      } else {
        m = transformRotateDegrees(args[0]);
      }
    } else if (name == "skewX") {
      if (!readTransformArgs(text, &pos, args, 1, 1, &count)) return false;
      m = transformSkewDegrees(args[0], 0.0f);
    } else if (name == "skewY") {
      if (!readTransformArgs(text, &pos, args, 1, 1, &count)) return false;
      m = transformSkewDegrees(0.0f, args[0]);
    } else {
      return false;
    }

    // Left-to-right fold: `acc = acc * next` applies `next` first, so the
    // first-encountered (leftmost) function ends up applied last. See
    // io/SvgPath.hpp's comment on parseSvgTransform() for the derivation.
    acc = mat3Multiply(acc, m);
    any = true;

    skipSeparator(text, &pos);
  }

  if (!any) return false;
  *out = acc;
  return true;
}

// --------------------------------------------------------------------------
// Lengths and units
// --------------------------------------------------------------------------

bool parseSvgLength(std::string_view text, SvgLength* out) {
  if (!out) return false;
  size_t pos = 0;
  skipWsp(text, &pos);
  float value = 0.0f;
  if (!lexNumber(text, &pos, &value)) return false;
  skipWsp(text, &pos);

  std::string_view rest = text.substr(pos);
  SvgUnit unit;
  if (rest.empty()) {
    unit = SvgUnit::User;
  } else if (rest == "px") {
    unit = SvgUnit::Px;
  } else if (rest == "pt") {
    unit = SvgUnit::Pt;
  } else if (rest == "pc") {
    unit = SvgUnit::Pc;
  } else if (rest == "mm") {
    unit = SvgUnit::Mm;
  } else if (rest == "cm") {
    unit = SvgUnit::Cm;
  } else if (rest == "in") {
    unit = SvgUnit::In;
  } else if (rest == "em") {
    unit = SvgUnit::Em;
  } else if (rest == "ex") {
    unit = SvgUnit::Ex;
  } else if (rest == "%") {
    unit = SvgUnit::Percent;
  } else {
    return false;  // trailing garbage, or an unrecognised unit
  }

  out->value = value;
  out->unit = unit;
  return true;
}

float resolveSvgLength(SvgLength len, const SvgLengthContext& ctx) noexcept {
  // 96 dpi throughout -- see io/SvgPath.hpp's comment on this function for
  // why that is 90 dpi's spec text and a deliberate departure from it.
  switch (len.unit) {
    case SvgUnit::User: return len.value;
    case SvgUnit::Px: return len.value;
    case SvgUnit::Pt: return len.value * (96.0f / 72.0f);
    case SvgUnit::Pc: return len.value * 16.0f;
    case SvgUnit::Mm: return len.value * (96.0f / 25.4f);
    case SvgUnit::Cm: return len.value * (96.0f / 2.54f);
    case SvgUnit::In: return len.value * 96.0f;
    case SvgUnit::Em: return len.value * ctx.fontSizePx;
    case SvgUnit::Ex: return len.value * ctx.xHeightPx;
    case SvgUnit::Percent: return len.value * 0.01f * ctx.percentBasisPx;
  }
  return len.value;  // unreachable: every enumerator is handled above
}

// --------------------------------------------------------------------------
// Number lists
// --------------------------------------------------------------------------

bool parseSvgNumberList(std::string_view text, std::vector<float>* out) {
  if (!out) return false;
  out->clear();
  size_t pos = 0;
  skipWsp(text, &pos);
  while (pos < text.size()) {
    float v = 0.0f;
    if (!lexNumber(text, &pos, &v)) return false;
    out->push_back(v);
    skipSeparator(text, &pos);
  }
  return true;
}

// --------------------------------------------------------------------------
// viewBox and preserveAspectRatio
// --------------------------------------------------------------------------

bool parseSvgViewBox(std::string_view text, SvgViewBox* out) {
  if (!out) return false;
  std::vector<float> nums;
  if (!parseSvgNumberList(text, &nums)) return false;
  if (nums.size() != 4) return false;
  if (nums[2] < 0.0f || nums[3] < 0.0f) return false;  // SVG 1.1 7.7: negative is an error
  out->minX = nums[0];
  out->minY = nums[1];
  out->width = nums[2];
  out->height = nums[3];
  return true;
}

namespace {

// Splits on whitespace only -- preserveAspectRatio's grammar has no commas.
std::vector<std::string_view> splitWsp(std::string_view s) {
  std::vector<std::string_view> tokens;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && isAsciiSpace(s[i])) ++i;
    size_t start = i;
    while (i < s.size() && !isAsciiSpace(s[i])) ++i;
    if (i > start) tokens.push_back(s.substr(start, i - start));
  }
  return tokens;
}

bool alignFromName(std::string_view name, SvgAlign* out) {
  if (name == "none") { *out = SvgAlign::None; return true; }
  if (name == "xMinYMin") { *out = SvgAlign::XMinYMin; return true; }
  if (name == "xMidYMin") { *out = SvgAlign::XMidYMin; return true; }
  if (name == "xMaxYMin") { *out = SvgAlign::XMaxYMin; return true; }
  if (name == "xMinYMid") { *out = SvgAlign::XMinYMid; return true; }
  if (name == "xMidYMid") { *out = SvgAlign::XMidYMid; return true; }
  if (name == "xMaxYMid") { *out = SvgAlign::XMaxYMid; return true; }
  if (name == "xMinYMax") { *out = SvgAlign::XMinYMax; return true; }
  if (name == "xMidYMax") { *out = SvgAlign::XMidYMax; return true; }
  if (name == "xMaxYMax") { *out = SvgAlign::XMaxYMax; return true; }
  return false;
}

}  // namespace

bool parseSvgPreserveAspectRatio(std::string_view text, SvgPreserveAspectRatio* out) {
  if (!out) return false;
  std::vector<std::string_view> tokens = splitWsp(text);
  size_t i = 0;
  if (i < tokens.size() && tokens[i] == "defer") ++i;  // ignored, see the header

  if (i >= tokens.size()) return false;
  SvgAlign align;
  if (!alignFromName(tokens[i], &align)) return false;
  ++i;

  SvgMeetOrSlice meetOrSlice = SvgMeetOrSlice::Meet;
  if (i < tokens.size()) {
    if (tokens[i] == "meet") {
      meetOrSlice = SvgMeetOrSlice::Meet;
    } else if (tokens[i] == "slice") {
      meetOrSlice = SvgMeetOrSlice::Slice;
    } else {
      return false;
    }
    ++i;
  }
  if (i != tokens.size()) return false;  // trailing tokens

  out->align = align;
  out->meetOrSlice = meetOrSlice;
  return true;
}

Mat3 svgViewBoxTransform(const SvgViewBox& box, float viewportW, float viewportH,
                         const SvgPreserveAspectRatio& par) noexcept {
  if (!(box.width > 0.0f) || !(box.height > 0.0f)) return mat3Identity();

  const float scaleX = viewportW / box.width;
  const float scaleY = viewportH / box.height;
  float sx = scaleX, sy = scaleY;
  if (par.align != SvgAlign::None) {
    const float s = (par.meetOrSlice == SvgMeetOrSlice::Meet) ? std::min(scaleX, scaleY)
                                                                : std::max(scaleX, scaleY);
    sx = sy = s;
  }

  const float scaledW = box.width * sx;
  const float scaledH = box.height * sy;
  float offsetX = 0.0f, offsetY = 0.0f;
  switch (par.align) {
    case SvgAlign::None:
      break;
    case SvgAlign::XMinYMin: break;
    case SvgAlign::XMidYMin: offsetX = (viewportW - scaledW) * 0.5f; break;
    case SvgAlign::XMaxYMin: offsetX = viewportW - scaledW; break;
    case SvgAlign::XMinYMid: offsetY = (viewportH - scaledH) * 0.5f; break;
    case SvgAlign::XMidYMid: offsetX = (viewportW - scaledW) * 0.5f;
                             offsetY = (viewportH - scaledH) * 0.5f; break;
    case SvgAlign::XMaxYMid: offsetX = viewportW - scaledW;
                             offsetY = (viewportH - scaledH) * 0.5f; break;
    case SvgAlign::XMinYMax: offsetY = viewportH - scaledH; break;
    case SvgAlign::XMidYMax: offsetX = (viewportW - scaledW) * 0.5f;
                             offsetY = viewportH - scaledH; break;
    case SvgAlign::XMaxYMax: offsetX = viewportW - scaledW;
                             offsetY = viewportH - scaledH; break;
  }

  const float tx = offsetX - box.minX * sx;
  const float ty = offsetY - box.minY * sy;
  return mat3Multiply(transformTranslate(tx, ty), transformScale(sx, sy));
}

// --------------------------------------------------------------------------
// The basic shapes
// --------------------------------------------------------------------------

namespace {

Anchor straightAnchor(PathPoint p) {
  Anchor a;
  a.pt = a.in = a.out = p;
  return a;
}

// The standard 4-cubic ellipse/rounded-corner control-point offset: for a
// quarter turn of a circle of radius `r`, the handle length that best
// matches a true arc is `k * r`, with `k = 4/3 (sqrt(2) - 1)`.
constexpr float kBezierCircleKappa = 0.5522847498307936f;

}  // namespace

Path svgRectPath(float x, float y, float w, float h, float rx, float ry) {
  if (!(w > 0.0f) || !(h > 0.0f)) return Path{};

  // SVG 1.1 5.3.4: a negative argument means "not specified" in this API
  // (see io/SvgPath.hpp); resolve the pair, then clamp to the box.
  if (rx < 0.0f && ry < 0.0f) {
    rx = 0.0f;
    ry = 0.0f;
  } else if (rx < 0.0f) {
    rx = ry;
  } else if (ry < 0.0f) {
    ry = rx;
  }
  rx = std::min(rx, w * 0.5f);
  ry = std::min(ry, h * 0.5f);
  rx = std::max(rx, 0.0f);
  ry = std::max(ry, 0.0f);

  Path path;
  SubPath sub;
  sub.closed = true;
  const float x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  const float k = kBezierCircleKappa;

  if (rx <= 0.0f && ry <= 0.0f) {
    sub.anchors = {straightAnchor({x0, y0}), straightAnchor({x1, y0}),
                   straightAnchor({x1, y1}), straightAnchor({x0, y1})};
    path.subpaths.push_back(std::move(sub));
    return path;
  }

  // Eight anchors: a rounded corner is a single quarter-ellipse cubic, whose
  // two control points sit `k * radius` along each edge's own tangent from
  // its two endpoints -- see kBezierCircleKappa's comment.
  Anchor a1 = straightAnchor({x0 + rx, y0});
  Anchor a2 = straightAnchor({x1 - rx, y0});
  a2.out = {x1 - rx + k * rx, y0};
  Anchor a3 = straightAnchor({x1, y0 + ry});
  a3.in = {x1, y0 + ry - k * ry};
  Anchor a4 = straightAnchor({x1, y1 - ry});
  a4.out = {x1, y1 - ry + k * ry};
  Anchor a5 = straightAnchor({x1 - rx, y1});
  a5.in = {x1 - rx + k * rx, y1};
  Anchor a6 = straightAnchor({x0 + rx, y1});
  a6.out = {x0 + rx - k * rx, y1};
  Anchor a7 = straightAnchor({x0, y1 - ry});
  a7.in = {x0, y1 - ry + k * ry};
  Anchor a8 = straightAnchor({x0, y0 + ry});
  a8.out = {x0, y0 + ry - k * ry};
  a1.in = {x0 + rx - k * rx, y0};

  sub.anchors = {a1, a2, a3, a4, a5, a6, a7, a8};
  path.subpaths.push_back(std::move(sub));
  return path;
}

Path svgEllipsePath(float cx, float cy, float rx, float ry) {
  if (!(rx > 0.0f) || !(ry > 0.0f)) return Path{};

  const float k = kBezierCircleKappa;
  const PathPoint right{cx + rx, cy};
  const PathPoint bottom{cx, cy + ry};
  const PathPoint left{cx - rx, cy};
  const PathPoint top{cx, cy - ry};

  Anchor aRight = straightAnchor(right);
  Anchor aBottom = straightAnchor(bottom);
  Anchor aLeft = straightAnchor(left);
  Anchor aTop = straightAnchor(top);

  aRight.out = {right.x, right.y + k * ry};
  aBottom.in = {bottom.x + k * rx, bottom.y};

  aBottom.out = {bottom.x - k * rx, bottom.y};
  aLeft.in = {left.x, left.y + k * ry};

  aLeft.out = {left.x, left.y - k * ry};
  aTop.in = {top.x - k * rx, top.y};

  aTop.out = {top.x + k * rx, top.y};
  aRight.in = {right.x, right.y - k * ry};

  Path path;
  SubPath sub;
  sub.closed = true;
  sub.anchors = {aRight, aBottom, aLeft, aTop};
  path.subpaths.push_back(std::move(sub));
  return path;
}

Path svgLinePath(float x1, float y1, float x2, float y2) {
  Path path;
  SubPath sub;
  sub.closed = false;
  sub.anchors = {straightAnchor({x1, y1}), straightAnchor({x2, y2})};
  path.subpaths.push_back(std::move(sub));
  return path;
}

Path svgPolyPath(const std::vector<float>& points, bool closed) {
  const size_t pairs = points.size() / 2;  // an odd trailing value is dropped
  if (pairs < 2) return Path{};

  Path path;
  SubPath sub;
  sub.closed = closed;
  sub.anchors.reserve(pairs);
  for (size_t i = 0; i < pairs; ++i) {
    sub.anchors.push_back(straightAnchor({points[2 * i], points[2 * i + 1]}));
  }
  path.subpaths.push_back(std::move(sub));
  return path;
}

}  // namespace np
