#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/VectorShape.hpp"

// io/SvgImport -- an SVG document, in document-space `VectorShape`s.
//
// io/SvgPath.hpp parses the grammars (a `d` string, a `transform`, a length,
// a `viewBox`) and io/SvgStyle.hpp resolves the cascade (which declaration
// wins). Neither touches XML. This file is the third piece: it walks a real
// document with pugixml, builds the `SvgElementView` chain io/SvgStyle.hpp
// wants, calls io/SvgPath.hpp's grammars on the attribute values it finds,
// and flattens the result into `core::VectorShape`s a `LayerKind::Vector`
// layer can hold directly (`core::makeVectorLayer()`).
//
// Mirrors io/PsdImport.hpp's shape: a `Result` struct with `ok`/`error`, the
// imported content, and a list of everything dropped, named. app/SvgReport
// is this file's app/PsdReport -- run it against a real exporter's output and
// read the report against the file open in an editor.
//
// ==========================================================================
// 1. Every shape comes out flat, in document coordinates
// ==========================================================================
//
// `core::VectorShape` stores no per-shape matrix (core/VectorShape.hpp's own
// section 2, and docs/vector-editing.md section 1 argues the point at
// length: a per-shape transform would touch the rasteriser, bounds,
// hit-testing, io/PathSerial and vectorContentHash, for a "cleaner" model
// nothing downstream needs). So the transform stack -- every `transform`
// attribute from the root `<svg>` down to the shape, PLUS `<use>`'s own
// generated `translate(x,y)`, PLUS the `viewBox` -> viewport map at every
// `<svg>` (root or nested) -- is composed into one `Mat3` per shape and
// applied to every anchor before the shape is ever appended to the result.
// There is no second representation anywhere in this file; a shape's `path`
// IS its document-space geometry from the moment it exists.
//
// A consequence worth stating because it is easy to get backwards:
// `StrokeStyle::width` is a single scalar, so it cannot represent a
// non-uniformly scaled pen (an ellipse-shaped stroke under `scale(2,1)`).
// This importer scales `stroke-width` (and every `stroke-dasharray` length)
// by `sqrt(|det|)` of the shape's own accumulated 2x2 -- exact for a pure
// translate/rotate/uniform-scale stack, and a stated approximation
// otherwise. Getting the non-uniform case exactly right needs an elliptical
// pen in core/PathStroke, which nothing in this codebase has asked for yet.
//
// ==========================================================================
// 2. Colour: decoded through color/Space on the way in, once
// ==========================================================================
//
// core/VectorShape.hpp's section 1 states the rule: `Paint::rgba` is
// linear-light, straight alpha. An SVG colour (`#rrggbb`, `rgb(...)`, a
// named colour) is sRGB-encoded 8-bit-equivalent, so every RGB channel is
// decoded through `color::srgbDecode()` here, exactly once, before it is
// ever stored on a `Paint`. Alpha (from `fill-opacity`/`stroke-opacity`/
// `opacity`, or a colour function's own alpha) is never gamma-encoded and
// is stored as-is, clamped to [0, 1] -- the same convention
// io/PsdImport.hpp and io/ImageDecode.cpp already use for every other
// format this codebase reads.
//
// `opacity` (the *element's* opacity, as opposed to `fill-opacity`/
// `stroke-opacity`) is folded into both `fill.rgba[3]` and `stroke.rgba[3]`
// by multiplication. That is an approximation, stated: SVG's actual
// semantics composite the whole element (fill AND stroke together) as one
// group at that opacity, so a half-opaque stroke drawn over a half-opaque
// fill should show a seam at their shared edge where the two would
// otherwise double up. `VectorShape` has no group-opacity field to hang the
// exact semantics on, and adding one is a bigger change than this track's
// scope -- the visible error is confined to strokes with a genuinely
// different colour from their fill at partial `opacity`, which is not the
// common case.
//
// ==========================================================================
// 3. Gradients: refused by name, not half-built -- read this before adding
//    one
// ==========================================================================
//
// core/VectorShape.hpp's section 2 already states the intended design: a
// paint kind plus an index into a document-level gradient table reusing
// ops/Gradient.hpp's `GradientKind`/`GradientSpread`/`GradientStops`, kept
// out of `Paint` itself because `VectorShape` is reachable from
// `core::Layer` and pulling ops/Gradient.hpp's tile-store dependency into
// every translation unit for a field nothing can populate yet would be the
// wrong trade.
//
// That table does not exist. Building it is not a contained change: it
// needs a new document-level type threaded through `core::Layer` (or
// `core::Document`), a version bump in io/PathSerial for the shapes that
// reference it, and a decision about a table entry's lifetime when every
// shape referencing it is deleted -- none of which is this track's file
// list (`core/VectorShape.hpp`, `core/LayerOps.hpp` and io/PathSerial are
// explicitly not in it). Half-building it -- adding a `GradientPaint`
// struct here with nowhere real to put the table, or reusing `Paint::rgba`
// to store an average colour -- would produce a shape that opens without
// error and renders wrong, which is exactly the failure mode this
// project's refusal discipline (io/PsdImport.hpp, io/Descriptor.hpp) exists
// to avoid.
//
// **`<use>` is modelled as if its target were reparented directly under the
// `<use>` element itself**, rather than the spec's own "invisible shadow
// `<g>`": the use's own computed style becomes the inherited context the
// target's cascade sees, and the use's own `SvgElementView` becomes the
// ancestor a descendant selector matches against. This differs from the
// spec only for a selector written specifically against the shadow node's
// synthetic tag -- something no real author's stylesheet can do, since
// there is no way to address it from outside the UA -- so nothing a real
// file contains can tell the two apart.
//
// So: `<linearGradient>`, `<radialGradient>` and `<pattern>` are recognised
// (parsed enough to be skipped correctly, so they do not fall into the
// generic "unsupported element" refusal and swallow their `<stop>`
// children as if those were shapes) but never produce a paint. A shape
// whose `fill`/`stroke` is `url(#id)` and `id` names one of them is refused
// by name, and falls back to the paint server's own SVG2 fallback colour
// (`fill="url(#g) red"`) when the author supplied one, else to no paint at
// all -- never to a guessed flat colour this importer invented.
//
// What the table would need, for whoever builds it: one `GradientStops`
// (already exactly the right shape) plus a `GradientGeometry` per gradient
// element resolved into the SAME flattened document space every shape's
// `path` is in (a `<linearGradient>`'s own `gradientTransform` composes
// with whatever accumulated transform was in effect at the *referencing*
// shape, exactly like this file's `<clipPath>` handling below), keyed by
// the element's `id` so multiple shapes can share one table entry the way
// they share one `url(#id)`.
//
// ==========================================================================
// 4. clipPath: a single child maps directly, several are UNIONED
// ==========================================================================
//
// `VectorShape::clip` is one `optional<Path>`, not a list, so a `<clipPath>`
// with more than one child shape has to become one `Path` somehow.
// **This importer unions them**: every child's geometry becomes a subpath
// of one `Path`, all sharing `FillRule::NonZero`. For clip shapes that do
// not overlap -- overwhelmingly the common case, an icon's clip built from
// two or three disjoint rectangles -- nonzero winding over disjoint,
// consistently-wound subpaths IS the union, exactly. It stops being exact
// only where the clip shapes themselves overlap AND wind oppositely, which
// nonzero would then read as a hole rather than a union; refusing multi-
// shape clipPaths instead was the rejected alternative, and was rejected
// because a union is correct far more often than it is wrong, while a
// refusal would be wrong (as a report line saying "no clip applied" when a
// clip visibly should be) on every ordinary multi-piece clip icon.
//
// A `<clipPath>`'s own children are resolved in the coordinate system of
// the ELEMENT REFERENCING IT via `clip-path="url(#id)"` (SVG 1.1 14.3.5,
// `clipPathUnits="userSpaceOnUse"`, this importer's only supported value --
// `objectBoundingBox` is refused by name), composed with the `<clipPath>`
// element's own `transform` if it has one. It is NOT the coordinate system
// wherever the `<clipPath>` happens to sit in the tree (typically inside
// `<defs>`, which is never itself rendered and has no position).
//
// A `<clipPath>` child that is not a basic shape or `<path>` (a nested `<g>`
// or `<use>` inside a `<clipPath>`) is refused by name, per shape, and
// contributes nothing to the union -- supporting arbitrary nesting inside a
// clip is a straightforward extension of this same code but was judged not
// worth the surface for a first landing; real exporters overwhelmingly emit
// flat shape lists inside `<clipPath>`.
//
// ==========================================================================
// 5. What "refused by name" actually refuses
// ==========================================================================
//
// Every element this importer does not render skips its own subtree and
// adds one line to `refusals` naming the tag (and, where useful, the `id`):
// `<filter>`, `<pattern>`, `<mask>`, `<switch>`, `<foreignObject>`,
// `<image>`, `<text>` (and everything inside it -- Stage 5's job, named
// here rather than attempted), `<script>`, every animation element
// (`<animate>`, `<animateTransform>`, `<animateMotion>`, `<animateColor>`,
// `<set>`), `<symbol>`, `<a>`, and any element tag this file has never
// heard of -- a vendor extension, a typo, or a future SVG addition all get
// the same honest "not handled" rather than being silently dropped or,
// worse, misread as something else. `<title>` and `<desc>` are the one
// documented exception: SVG explicitly defines them as non-rendering
// accessibility metadata, so they are skipped WITHOUT a refusal line,
// exactly as the brief for this track states.
//
// **Attributes are a narrower net than elements, deliberately.** Only an
// attribute that would change what is DRAWN and that this importer does
// not implement is refused by name: `mask=`, `filter=` (as attributes, not
// only as elements), a `clip-path` value that is not `url(#id)` (a CSS
// Basic Shape function like `circle(50%)`), and an `xlink:href`/`href`
// that does not start with `#` (a reference to another file -- refused,
// never fetched: this importer opens no second file and makes no network
// request for any input). Purely descriptive or editor-round-trip
// attributes that have NO rendering effect -- `sodipodi:*`, `inkscape:*`,
// `xmlns:*`, `enable-background`, `data-*`, `aria-*`, `xml:space`,
// `version` -- are silently ignored, the same way every browser ignores
// vendor metadata it does not recognise. The alternative (refuse every
// attribute this file does not specifically parse) was rejected because a
// real Inkscape or Illustrator export carries dozens of these per element;
// refusing each one would bury the refusals that matter (a real dropped
// gradient, a real dropped filter) in noise nobody would read past.
//
// ==========================================================================
// 6. Security: three caps, all against a file this build did not write
// ==========================================================================
//
// pugixml has no DTD and no custom-entity support at all (see
// src/CMakeLists.txt's own comment on this vendoring decision), so XXE and
// the classic billion-laughs entity bomb are impossible by construction,
// not by anything this file adds. What is still this file's own job to
// bound is structural, not textual: `<use>` recursion, plain nesting depth,
// and the total amount of geometry produced. See the `kMaxSvg*` constants
// below for the exact numbers and what each one is proof against; every one
// of them is checked BEFORE the walk does further work on the strength of
// it, not after, so a bomb is refused promptly rather than part-processed.
namespace np {

// See section 6. Checked before a `<use>` is expanded (i.e. before its
// target is visited): together they mean a `<use>` bomb is refused in
// O(depth) or O(this many expansions), never in time proportional to the
// exponential a naive walk would compute.
inline constexpr int kMaxSvgUseDepth = 32;
inline constexpr size_t kMaxSvgUseExpansions = 5000;

// See section 6. The general recursion-depth cap over `<g>`/`<svg>`/
// `<clipPath>` nesting, independent of `<use>` -- checked at the top of
// every recursive call, so the walk cannot recurse past this many stack
// frames regardless of how the document is shaped.
inline constexpr int kMaxSvgNestingDepth = 200;

// See section 6. Total elements this importer VISITS while walking the
// (possibly `<use>`-expanded) tree -- the one number that ultimately bounds
// the whole walk's cost, because it is checked at the top of every visit
// and the walk stops, with a refusal, the instant it is exceeded.
inline constexpr size_t kMaxSvgElements = 20000;

// See section 6. Total anchor count summed across every shape this
// importer produces (including clip-path geometry). Catches the one attack
// the caps above do not: a single `<path d="...">` whose `d` string is
// enormous, which never recurses and never visits a second element.
inline constexpr size_t kMaxSvgAnchors = 200000;

// Mirrors `PsdImportResult`'s shape (io/PsdImport.hpp).
struct SvgImportResult {
  bool ok = false;

  // Set only when `!ok`: the buffer was not a well-formed XML document (a
  // pugixml parse error), or its root element was not `<svg>` (or an
  // `xxx:svg` in some namespace) -- io/FileKind.hpp's `sniffFileKind()`
  // should have already ruled the second case out for anything reaching
  // `File > Open`, but this function makes no assumption about its caller
  // and checks for itself.
  std::string error;

  // Every shape this importer produced, in DOCUMENT-order (the order shapes
  // are encountered walking the tree depth-first, which is also painting
  // order -- later elements are drawn over earlier ones, exactly as
  // `core::Layer::shapes`' own "bottom to top" convention expects), already
  // flattened to document coordinates per section 1.
  std::vector<VectorShape> shapes;

  // One line per refused element, attribute, or tripped cap -- section 5
  // and 6 name exactly what ends up here and why. Never a silent drop.
  std::vector<std::string> refusals;

  // The root `<svg>`'s resolved viewport, in pixels: `width`/`height` if
  // present (resolved through `resolveSvgLength()`), else the `viewBox`
  // dimensions if `width`/`height` are absent, else SVG's own UA default
  // replaced-element size of 300x150. Zero only when `!ok`.
  float widthPx = 0.0f;
  float heightPx = 0.0f;
};

// Parses `data`/`size` as an SVG document and returns its shapes in document
// coordinates. Reads no byte outside `[data, data + size)`, for any content
// whatsoever -- the same contract io/PsdImport.hpp and io/Descriptor.hpp
// hold themselves to, for the same reason: this parses a file the build did
// not write.
SvgImportResult importSvg(const uint8_t* data, size_t size);

// Reads `path` and calls `importSvg()`. `error` names the file on an I/O
// failure (mirrors `io/PsdImport.hpp`'s file-level callers, e.g.
// app/PsdReport.cpp, which each open the file themselves -- kept here
// instead because unlike `.psd`'s report tool, an SVG file is small enough
// that "read it whole" belongs in the library function, not repeated at
// every call site).
SvgImportResult importSvgFile(const char* path);

}  // namespace np
