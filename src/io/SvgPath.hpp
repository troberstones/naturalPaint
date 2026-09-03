#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "core/Path.hpp"
#include "ops/Transform.hpp"

// io/SvgPath -- SVG's text-to-geometry grammars, and nothing else.
//
// This file is pure parsing: `std::string_view` in, geometry out. No XML, no
// file I/O, no GPU, no dependency on an SVG document tree -- a future
// io/SvgImport hands this file the *values* of attributes it has already
// found (`d="..."`, `transform="..."`, `viewBox="..."`) and gets back
// `core::Path`, `Mat3`, or plain numbers. Keeping the grammars here, decoupled
// from XML parsing, is what lets each one be tested in isolation against the
// SVG 1.1 / SVG2 text rather than against a hand-built test document.
//
// ==========================================================================
// Everything downstream sees exactly one curve type
// ==========================================================================
//
// core/Path.hpp section 1 states the rule this file exists to satisfy:
// quadratics are elevated to cubics and elliptical arcs are converted to
// cubics **here**, so nothing past this boundary needs a second code path for
// a second curve kind. `arcToCubics()` (core/PathFlatten.hpp) already does
// the arc half of that; this file does the quadratic-elevation half and
// drives both from the `d` grammar's command letters.
//
// ==========================================================================
// The `d` grammar's error contract
// ==========================================================================
//
// SVG 1.1 section 8.3.1 requires a conforming reader to render a path up to
// the point a grammar error is found, not to discard the whole path. That is
// why `parseSvgPathData()` returns a `bool` rather than an `std::optional`:
// on `false`, `*out` is not empty and not garbage -- it holds every complete
// command successfully applied **before** the offending byte, exactly as a
// browser would rasterise a `d` string a lint tool has not yet caught. A
// caller that wants "reject the whole path on any error" gets that for free
// by checking the return value and discarding `*out` itself; a caller that
// wants the lenient behaviour SVG asks for does not have to re-parse.
//
// ==========================================================================
// mat3Invert() is not used anywhere in this file
// ==========================================================================
//
// Every matrix this file builds is a forward map assembled from translate,
// scale, rotate and skew primitives (`ops/Transform.hpp`'s builders) and
// composed with `mat3Multiply()`. None of the eight functions below ever
// needs the *inverse* of one of those maps, and ops/Transform.hpp's own
// comment on `mat3Invert()` documents a defect at large translations that
// this file has no reason to inherit for a capability it does not need.
namespace np {

// --------------------------------------------------------------------------
// The `d` attribute
// --------------------------------------------------------------------------
//
// Supports every command letter in both cases: `M m L l H h V v C c S s Q q
// T t A a Z z`. Separators between numbers are any run of whitespace and/or
// a single comma; a sign or a second `.` is also an implicit separator (see
// below), matching real exporter output rather than only the strict grammar.
//
// **Implicit repetition.** After the first coordinate pair of a `moveto`,
// further coordinate pairs in the same command are implicit `lineto`
// (matching case) -- this is SVG 1.1 8.3.2's own rule and the single most
// commonly mis-implemented part of this grammar, because it is tempting to
// treat every extra pair after `M` as another `moveto`. After every other
// command letter, further argument sets repeat that same command: `L 1 1 2
// 2` is two linetos, `C ... ...` with two sets of six numbers is two cubics.
//
// **The number lexer is where real files break a naive implementation.**
// `.5`, `-.5`, `+5`, `1e3` and `1.5e-3` are all one valid number each. But
// `1.5.5` is **two** numbers, `1.5` then `.5` -- a second `.` closes the
// number in progress rather than erroring, and real exporters emit exactly
// this when they concatenate coordinates without a separator. Likewise
// `10-5` is **two** numbers, `10` then `-5` -- a sign closes the number in
// progress *unless* it immediately follows an `e`/`E`, in which case it is
// the exponent's own sign (`1.5e-3` stays one number). Every claim in this
// paragraph has a --selftest assertion; see app/selftest/SvgPath.cpp.
//
// **Quadratics are elevated to cubics at parse time**, per SVG 1.1 8.3.6, so
// that core/Path.hpp's "exactly one segment type" holds. For a quadratic
// control point `q` from `p0` to `p2`: `c1 = p0 + (2/3)(q - p0)`,
// `c2 = p2 + (2/3)(q - p2)`.
//
// **`S`/`T` reflect** the previous curve's second control point through the
// current point: `S`'s implicit first control point (and `T`'s implicit
// control point) is `2*current - lastControl`. If the immediately preceding
// command was **not** a matching curve type (`C` or `S` for a following `S`;
// `Q` or `T` for a following `T`) -- including when it is the first command
// of a subpath, or a `Z`, or any line command -- the reflected point **is**
// the current point, per spec, not an error.
//
// **Arcs** (`A`/`a`) are converted via `arcToCubics()` (core/PathFlatten.hpp)
// using this file's own tracked current point as `from`. A negative `rx` or
// `ry` is corrected to its absolute value (SVG2's own rule; `arcToCubics()`
// does this too, redundantly and harmlessly). When `arcToCubics()` returns
// `false` -- zero radius, or a coincident start/end -- that is SVG's own
// documented fallback ("draw a line"), applied here, not an error.
//
// **`Z`/`z`** closes the current subpath (the closing edge from the last
// anchor back to the first is implied, matching core/Path.hpp section 3 --
// no repeated vertex is written). A command that follows `Z` starts a *new*
// subpath at the point the closed subpath began, per SVG 1.1 8.3.1's own
// "as if a new subpath had been started" text.
//
// **Refusals**, always as a `false` return with a prefix preserved: an
// unknown command letter; a command whose argument list runs out before its
// required count (including at the very end of the string); a number that
// lexes but is not finite (an exponent large enough to overflow to
// infinity); and -- since SVG requires every path to begin with a `moveto`
// -- any `d` whose first non-whitespace token is not `M`/`m`, which also
// covers an empty or whitespace-only `d`.
//
// `errorOffset` is only written on a `false` return, to the byte offset of
// the token that could not be consumed.
bool parseSvgPathData(std::string_view d, Path* out, size_t* errorOffset);

// --------------------------------------------------------------------------
// The `transform` / `gradientTransform` attribute
// --------------------------------------------------------------------------
//
// `matrix(a b c d e f)`, `translate(tx [ty])` (`ty` defaults to 0),
// `scale(sx [sy])` (`sy` defaults to `sx`), `rotate(a [cx cy])`, `skewX(a)`,
// `skewY(a)`. Several may appear in one attribute, separated by whitespace
// and/or commas: `"translate(10,20) rotate(45)"`.
//
// **Composition order.** SVG's own semantics: the transform list is applied
// to a point right-to-left, i.e. the **leftmost function in the text is
// applied last**. `mat3Multiply(a, b)` in ops/Transform.hpp is `a * b`,
// which "applies `b` first, then `a`" (its own comment) -- so building the
// result by folding left to right, `acc = mat3Multiply(acc, next)` starting
// from identity, produces exactly `M1 * M2 * ... * Mn` for a text ordering
// `M1 M2 ... Mn`, which is SVG's rule stated in this file's terms. Proved by
// mapping a concrete point through `mat3MapPoint()` against hand arithmetic
// in --selftest, not asserted from the algebra alone.
//
// **`rotate`.** `transformRotateDegrees()` builds exactly SVG's own rotation
// matrix -- both are `[[cos,-sin],[sin,cos]]` applied in a y-down space, so a
// positive angle turns visually clockwise on screen in both, with no sign
// flip needed to match SVG. Its 90-degree snap (ops/Transform.hpp section 4)
// is **kept, not bypassed**: snapping only replaces a `cosf`/`sinf` result
// that is off by ~4e-8 with the exact 0/+-1 it should have been, which can
// only make an SVG `rotate(90 ...)` more correct, never less. The sign
// convention is asserted directly in --selftest by mapping `(1, 0)` through
// `rotate(90)` and checking it lands at `(0, 1)` -- right rotates to down,
// which is clockwise on screen.
//
// **`skewX`/`skewY`** reuse `transformSkewDegrees(xDegrees, yDegrees)`
// directly: its comment states `xDegrees` "slants vertical lines (x gains a
// multiple of y)", which is precisely `skewX`'s `x' = x + tan(a)*y`, so
// `skewX(a)` is `transformSkewDegrees(a, 0)` and `skewY(a)` is
// `transformSkewDegrees(0, a)`. Its clamp short of the tangent's pole at
// +-89.9 degrees is inherited along with it -- a 90-degree skew is a
// genuine, not merely extreme, degeneracy, and this file has no better
// answer than the one already chosen for every other caller of that builder.
bool parseSvgTransform(std::string_view text, Mat3* out);

// --------------------------------------------------------------------------
// Lengths and units
// --------------------------------------------------------------------------
//
// `SvgLength` is a number plus the unit its text was written in, kept apart
// so a caller can defer resolution until it knows the font size or the
// percentage basis a `%` needs -- `parseSvgLength()` never guesses either.
enum class SvgUnit { User, Px, Pt, Pc, Mm, Cm, In, Em, Ex, Percent };

struct SvgLength {
  float value = 0.0f;
  SvgUnit unit = SvgUnit::User;
};

// Parses a number immediately followed (no space) by one of `px pt pc mm cm
// in em ex %`, or by nothing at all (`SvgUnit::User`, SVG's own "unitless
// number" case). Refuses trailing garbage after a recognised or absent unit.
bool parseSvgLength(std::string_view text, SvgLength* out);

// What `resolveSvgLength()` needs to turn `em`, `ex` and `%` into pixels. Not
// needed for the five physical/absolute units, which resolve the same
// regardless of context.
struct SvgLengthContext {
  float fontSizePx = 16.0f;
  float xHeightPx = 8.0f;
  float percentBasisPx = 0.0f;
};

// **This file uses the CSS/SVG2 96 dpi basis, not SVG 1.1's original 90
// dpi.** `1in = 96px`, so `1pt (= 1/72 in) = 96/72 px`, `1pc (= 12pt) = 16px`,
// `1cm = 96/2.54 px`, `1mm = 96/25.4 px`. This is a deliberate, stated
// discrepancy with the SVG 1.1 spec text: 96 dpi is what every real browser
// and Inkscape 0.92+ has emitted and read for physical units since roughly
// 2010, and reading a file at 90 dpi against a 96 dpi authoring tool is a
// silent 6.7% scale error on every `in`/`cm`/`mm`/`pt`/`pc` length in it --
// exactly the kind of importer bug that is invisible until a print-sized
// document comes out the wrong size a year later. `User` and `Px` both
// resolve to `value` unchanged; `Em`/`Ex`/`Percent` multiply by the matching
// `SvgLengthContext` field (`Percent` also divides by 100).
float resolveSvgLength(SvgLength len, const SvgLengthContext& ctx) noexcept;

// --------------------------------------------------------------------------
// Number / coordinate lists (`points`, and any other comma-or-space list)
// --------------------------------------------------------------------------
//
// The same lexer `parseSvgPathData()` uses for its arguments, exposed on its
// own for attributes that are only ever a flat number list -- `points` on
// `<polyline>`/`<polygon>` chief among them. Refuses any leftover text that
// is not a valid number or a separator; an empty or whitespace-only `text`
// parses to an empty, valid list rather than an error, matching a `points`
// attribute that is legally present and empty.
bool parseSvgNumberList(std::string_view text, std::vector<float>* out);

// --------------------------------------------------------------------------
// viewBox and preserveAspectRatio
// --------------------------------------------------------------------------

struct SvgViewBox {
  float minX = 0.0f;
  float minY = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// Four numbers: `minX minY width height`. Refuses a negative `width` or
// `height` -- SVG 1.1 7.7 calls that an error, distinct from a *zero* width
// or height, which is a legal value handled by `svgViewBoxTransform()` below.
bool parseSvgViewBox(std::string_view text, SvgViewBox* out);

enum class SvgAlign {
  None,
  XMinYMin, XMidYMin, XMaxYMin,
  XMinYMid, XMidYMid, XMaxYMid,
  XMinYMax, XMidYMax, XMaxYMax,
};

enum class SvgMeetOrSlice { Meet, Slice };

struct SvgPreserveAspectRatio {
  SvgAlign align = SvgAlign::XMidYMid;
  SvgMeetOrSlice meetOrSlice = SvgMeetOrSlice::Meet;
};

// `[defer] <align> [<meet-or-slice>]`. `defer` is accepted and ignored -- it
// only ever affected which of two documents' aspect ratios wins when an
// external image is still loading, which has no meaning for a value this
// file just parses into numbers.
bool parseSvgPreserveAspectRatio(std::string_view text, SvgPreserveAspectRatio* out);

// SVG 1.1 section 7.7's viewBox-to-viewport algorithm: scale uniformly by
// the smaller axis ratio for `meet` or the larger for `slice`, scale each
// axis independently for `SvgAlign::None`, then translate so the named edge
// or centre of the (scaled) viewBox lands on the matching edge or centre of
// the viewport.
//
// A `box` with `width <= 0` or `height <= 0` **disables rendering** per the
// same spec section -- this returns `mat3Identity()` rather than dividing by
// zero or a negative scale, and callers that care about "should this even be
// drawn" ask `box.width > 0 && box.height > 0` themselves rather than
// inferring it from getting identity back (identity is also, coincidentally,
// a perfectly legal *answer* when width/height already match the viewport).
Mat3 svgViewBoxTransform(const SvgViewBox& box, float viewportW, float viewportH,
                         const SvgPreserveAspectRatio& par) noexcept;

// --------------------------------------------------------------------------
// The basic shapes, as paths
// --------------------------------------------------------------------------
//
// Each of these degenerates to an **empty** `Path` (no subpaths) for input
// that SVG specifies as "nothing is rendered" -- a non-positive width or
// height, or a non-positive radius -- rather than emitting a zero-area shape
// that a rasteriser would then have to no-op on anyway.

// `rx`/`ry` follow SVG 1.1 5.3.4's resolution: **a negative value means "not
// specified"**, the convention this API uses in place of `std::optional`
// because a real corner radius is never negative. If exactly one of `rx`/
// `ry` is non-negative, both take that value; if neither is, both are zero
// (a plain rectangle); either way the resolved pair is then clamped to
// `rx <= w/2`, `ry <= h/2`. A plain rectangle (resolved `rx == ry == 0`) is
// built as four straight anchors, not four zero-length arcs.
Path svgRectPath(float x, float y, float w, float h, float rx, float ry);

// Four cubic quarter-arcs, using the standard `k = 4/3 (sqrt(2) - 1)`
// control-point offset -- the same construction `svgRectPath()` uses for its
// rounded corners, so the two never visibly disagree on what an ellipse arc
// looks like.
Path svgEllipsePath(float cx, float cy, float rx, float ry);

// One open, straight subpath. `<line>` has no fill in SVG, but this file
// only produces geometry -- what a caller strokes or fills it with is not
// its concern.
Path svgLinePath(float x1, float y1, float x2, float y2);

// `points` already parsed to a flat `x0 y0 x1 y1 ...` list (see
// `parseSvgNumberList()`). An odd trailing coordinate with no partner is
// dropped -- SVG2's own error recovery for `points` is to use the longest
// parseable prefix, and a lone coordinate cannot be a vertex. Fewer than two
// complete pairs produces an empty `Path`, matching a shape with no edges.
Path svgPolyPath(const std::vector<float>& points, bool closed);

}  // namespace np
