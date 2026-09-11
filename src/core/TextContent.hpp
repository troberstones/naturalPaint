#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Path.hpp"
#include "core/VectorShape.hpp"
#include "ops/Transform.hpp"
#include "text/Shaper.hpp"

// core/TextContent -- what a `LayerKind::Text` layer holds (PLAN.md phase 14;
// PRD K1-K3).
//
// ==========================================================================
// 1. A Text layer is a Vector layer that has not been typed out yet
// ==========================================================================
//
// The decision this whole file rests on: **text is not a second rendering
// path.** `textContentToShapes()` turns a `TextContent` into exactly the
// `std::vector<VectorShape>` a Vector layer already holds, and everything
// downstream -- core/PathRaster, core/VectorRaster's cache and materialised
// view, the compositor -- is untouched. There is one rasteriser in this build
// and text goes through it.
//
// That is PLAN.md phase 14's own wording ("rasterised at evaluation, so it
// stays editable and parametric like an Adjustment layer") and it is also what
// makes the feature small: a Text layer costs a content struct, a shaping
// call, and one `case` in core/VectorRaster. It costs no new tile format, no
// new blend path, and no glyph atlas.
//
// **What is deliberately NOT stored:** the shapes. They are derived, and
// core/VectorRaster section 1 already argues at length why derived rasters do
// not live on the layer; the same argument applies one level up to derived
// geometry. Storing shaped glyph outlines would double the layer's size,
// would go stale the moment the string or the font changed, and would make
// "editable and parametric" a promise rather than a property.
//
// ==========================================================================
// 2. Point text and paragraph text, told apart by one number
// ==========================================================================
//
// `frame.width == 0` is point text: **no wrapping, but hard breaks still
// break** -- Return in a point block starts a new line, the lines are spaced
// by whatever CoreText spaces paragraph lines by at the same size, and only
// the width is left to the text. Per text/Shaper.hpp's own rule there is
// still no alignment, because a line has nothing to align against when the
// box is not the user's. Any positive width is paragraph text: CoreText
// wraps to it as well, and `align` means something.
//
// "No wrapping" and "no line breaks" were the same thing here until the
// shaper stopped using `CTLineCreateWithAttributedString`, which does not
// break lines at all: a point block holding "Hi\nYo" drew "HiYo", so Return
// in point text inserted a character that could never become visible.
//
// A separate `bool paragraph` was considered and rejected: it would be a
// second copy of a fact `frame.width` already carries, and the two could
// disagree (a `paragraph == true` block with `width == 0` has no defined
// meaning). One number, one reading.
//
// ==========================================================================
// 2b. What `origin` pins: the BASELINE for point text, the frame for paragraph
// ==========================================================================
//
// Point text has no frame, so the only stable thing to pin it by is the first
// line's **baseline, at its left end** -- and `origin` is exactly that. Make
// the type bigger and it grows UP and to the right, out of a bottom-left
// corner that stays put, which is what a baseline means and what every type
// tool does.
//
// The alternative -- pinning the top of the line box -- is what this used to
// do, and it was wrong twice over. Enlarging the type pushed the block DOWN
// the page, away from the corner the user had placed. And an empty block drew
// its caret at `origin` while the first glyph's baseline landed an ascent
// lower, so **the caret jumped down by a whole ascent on the first
// keystroke** -- the text did not appear where the insertion point had been
// promising it would. Both are the same mistake, and pinning the baseline
// removes both: the caret sits on the baseline before anything is typed, and
// the first glyph arrives on that same baseline.
//
// Paragraph text keeps `origin` at its **frame's top-left**, because there a
// box is exactly what the user dragged out and can reason about. Type inside
// a frame that got bigger fills further DOWN the frame; the frame does not
// move. Pinning a paragraph by its first baseline would slide the whole box
// up the page whenever the type size changed, which is not what any frame in
// any layout program does.
//
// One field with two readings, keyed off `frame.width` -- the same single
// number section 2 already uses to tell the two kinds of block apart. It is
// read in exactly one place, `shapedOrigin()` in this file's .cpp, which
// every geometric query goes through.
//
// ==========================================================================
// 3. Colour is linear and straight, like every other core/ colour
// ==========================================================================
//
// `fill` is a `core/VectorShape.hpp` `Paint`: linear-light, straight alpha.
// It is reused rather than re-declared precisely because
// `textContentToShapes()` hands it straight to a `VectorShape` -- a second
// colour type here would exist only to be converted into that one.
//
// A `stroke` is carried for the same reason and with the same default (`on ==
// false`): outlined text is one `Paint` away once a UI wants it, and leaving
// the field out would change the serialised framing later.
//
// ==========================================================================
// 4. `transform`: how a block scales and rotates and stays EDITABLE
// ==========================================================================
//
// A text block has to be able to sit at an angle, or twice its drawn size,
// without stopping being text. The alternative -- rasterise on the first
// rotation -- is what makes type in most paint programs a one-way door: the
// moment you turn it, the string, the font and the size are gone.
//
// So a Text layer carries a full `Mat3`, and section 1's rule is unchanged:
// nothing downstream learns a new concept. `textContentToShapes()` maps every
// glyph outline through it on the way out, exactly where it already applied
// `origin`, and the rasteriser, the cache, the compositor and the exporter
// see the same `std::vector<VectorShape>` they always did.
//
// **The composition is `document = transform * (origin + penPosition)`.**
// Shaping is untouched by it -- the block wraps to `frame.width` and breaks
// its lines in TEXT space, then the whole shaped result is mapped. That is
// what makes the transform non-destructive: dragging a rotation handle cannot
// change where the words break, because the line breaking has already
// happened by the time the matrix is applied.
//
// **A `Mat3`, not a scale factor plus an angle.** The decomposed spelling
// cannot represent the composition of two drags (a rotate, then a
// non-uniform scale, is a shear -- there is no angle and no pair of scale
// factors that says so), so it would have to either refuse the second drag or
// silently drop the shear. `ops/Transform.hpp` already owns this type and the
// composition rules; storing anything else here would be a second, weaker
// spelling of it.
//
// **Every geometric query below maps through it**, and the inverse is what
// click-to-place-caret needs -- see section 5. A degenerate matrix (a handle
// dragged to zero width) has no inverse, and `ops/DocumentTransform.hpp`'s
// `transformTextLayer()` refuses to store one for exactly that reason: a
// block you cannot click is a block you cannot get back.
namespace np {

// One text block. The whole content of a `LayerKind::Text` layer.
struct TextContent {
  // UTF-8. Not validated here -- `text/Shaper.hpp`'s `shapeText()` is the one
  // place that can answer whether a byte sequence is text, and it reports
  // invalid UTF-8 as `ok == false` with a sentence rather than crashing.
  std::string utf8;

  // Family, size, tracking, leading, bold, italic. text/Shaper.hpp owns this
  // struct; it is not re-declared here for the reason section 3 gives about
  // `Paint`.
  TextStyle style;

  // `width == 0` is point text (section 2). `height == 0` under paragraph
  // text means "as tall as the lines need", never "clip to nothing".
  TextFrame frame;

  // Meaningful only for paragraph text (section 2 and text/Shaper.hpp).
  TextAlign align = TextAlign::Left;

  // Where the block is pinned in DOCUMENT coordinates -- section 2b for which
  // corner that is, because it differs between point and paragraph text.
  // Shaping happens in text-space with its own origin at the block's top-left
  // (text/Shaper.hpp), and this is the single translation applied on the way
  // out -- so moving a text block is one field, not a walk over glyphs.
  PathPoint origin;

  // Section 4. Identity by default, so a block that has never been scaled or
  // rotated behaves exactly as it did before this field existed -- and
  // serialises to the older on-disk form, which is what lets an existing
  // document still open in an older build (io/TextSerial.hpp).
  Mat3 transform;

  // Section 3. `fill.on` defaults false on `Paint` itself, so a `TextContent`
  // built by aggregate initialisation and never painted draws nothing; every
  // maker below turns it on.
  Paint fill;
  Paint stroke;
  StrokeStyle strokeStyle;
};

// A ready-to-use black 24 px block at `origin`, which is what clicking with
// `Tool::Text` on an empty canvas should produce. Exists so the tool, the
// selftest and any future importer agree on the default rather than each
// spelling out six fields.
TextContent makeTextContent(std::string utf8, PathPoint origin);

// core/VectorShape.hpp's `vectorContentHash()`, for text: a hash over
// everything that affects the rasterised result, so core/VectorRaster's cache
// cannot go stale.
//
// **Every field above is in it, including `utf8` and `style.fontFamily`.**
// The failure mode this guards is a user changing the font and seeing the old
// one, which is indistinguishable from the font picker being broken.
//
// Floats are hashed by bit pattern, exactly as `vectorContentHash()` does, so
// a size that differs in the last ulp re-rasterises -- the safe direction.
uint64_t textContentHash(const TextContent& text) noexcept;

// Shape `text` and convert every glyph to a filled `VectorShape`.
//
// **One shape per GLYPH, not one per block**, and the reason is the fill rule.
// A glyph like "o" is two contours wound so that non-zero fill leaves the
// counter open; merging every glyph of a line into one `Path` would still fill
// correctly under non-zero, but it would make a future per-glyph anything
// (a colour run, a per-glyph transform, a click-to-select) a re-shape rather
// than an index. The cost is a `VectorShape` header per glyph, which is tens
// of bytes against a rasterisation that is milliseconds.
//
// Every returned shape carries `text.fill`, `text.stroke` and
// `text.strokeStyle`, so the caller hands the result to
// `rasterizeVectorLayer()` unchanged.
//
// **Returns an empty vector, not a failure, for empty text** -- an empty
// string is a legitimate state for a Text layer a user has just created and
// not yet typed into, and a refusal there would make the layer unusable at the
// exact moment it is created.
//
// `errorOut`, when non-null, receives a sentence for the cases that ARE
// failures: invalid UTF-8, and a build with no shaper
// (`shaperAvailable() == false`, i.e. the non-Apple stub). Both leave the
// returned vector empty, and the distinction from the empty-text case is that
// `errorOut` is non-empty.
std::vector<VectorShape> textContentToShapes(const TextContent& text,
                                             std::string* errorOut = nullptr);

// The block's bounds in document coordinates: the union of the shaped glyph
// outlines, translated by `origin`, plus any stroke outset -- i.e. exactly
// `vectorShapesBounds(textContentToShapes(text))`, which is what this returns.
//
// **Not the frame, and not the shaper's `widthPx`/`heightPx`.** Those are the
// LAYOUT box; a glyph routinely paints outside it (an italic's overhang, an
// "f"'s hook, a stroke). A caller allocating tiles or hit-testing a click
// needs the painted extent, and the difference is visible the first time a
// descender is clipped.
//
// `valid == false` for empty text, which is `PathBounds`'s own answer for
// nothing at all rather than a zero-area box at `origin`.
//
// **The axis-aligned box of the TRANSFORMED geometry** (section 4), because
// it is derived from `textContentToShapes()` and always has been. For a
// rotated block that box is larger than the text -- which is correct for what
// callers use it for (the tiles to allocate, the extent to fit) and loose for
// a hit test, where `textOffsetAtPoint()` is exact and should be preferred.
PathBounds textContentBounds(const TextContent& text);

// Whether this text block would draw anything: non-empty after shaping AND at
// least one of fill/stroke on. The tool uses it to tell "the user has not
// typed yet" from "the user has typed and set the colour to invisible", which
// are the same picture and different problems.
bool textContentDraws(const TextContent& text);

// ==========================================================================
// 5. The caret and the selection, in document coordinates
// ==========================================================================
//
// `app/TextTool` owns the caret as a BYTE OFFSET into `utf8` and knows nothing
// about where that lands on screen; `ui/` has to draw a blinking bar there.
// These two functions are the bridge, and they live here rather than in
// app/TextTool because they need to SHAPE the text, which is this file's job
// and not that one's (app/TextTool is deliberately free of any shaping
// dependency -- see its own header).
//
// Both work off `ShapedGlyph::cluster`, which text/Shaper.hpp guarantees is a
// byte offset into the original UTF-8 and "never off a UTF-8 character
// boundary" even under bidi reordering and font fallback. That guarantee is
// what makes a byte offset a usable caret unit at all.

// A caret is a SEGMENT, not a point and a height.
//
// It was the latter until `transform` existed, and the caller reconstructed
// the bar by stepping up and down in y. Under a rotation that is wrong in a
// way that looks like a rendering bug: the text turns and the insertion point
// stays vertical. Two mapped endpoints turn with the block for free.
//
// The `top`/`bottom` split is the conventional 0.8/0.2 of the line box around
// the baseline, and it now lives HERE rather than in the drawing code,
// because `textSelectionQuads()` needs the identical split -- a caret and a
// highlight that disagreed by a pixel would look like a rendering bug on
// every screenshot. One definition, two readers.
struct TextCaretSegment {
  PathPoint top{0.0f, 0.0f};
  PathPoint bottom{0.0f, 0.0f};
};

// Where the caret sits, in document coordinates: the pen position of the
// first glyph whose cluster is at or after `caretByte`, or the trailing edge
// of the last glyph when the caret is at the end -- then mapped through
// `transform`.
//
// **Approximate in exactly one way, and it is stated rather than hidden:** the
// caret lands on a glyph BOUNDARY, so a caret between two glyphs of one
// cluster (a combining sequence, or a ligature) draws at the cluster's start.
// A caret cannot be placed inside a grapheme by `app/TextTool` either -- it
// steps by code point -- so the two agree, and doing better means a cursor
// model that knows about ligature carets, which is its own piece of work.
TextCaretSegment textCaretSegment(const TextContent& text, size_t caretByte);

// Four corners of one line's selection highlight, in document coordinates and
// in the order top-left, top-right, bottom-right, bottom-left **of the block's
// own text space** -- so under a rotation they are still that block's corners,
// wound consistently, and a caller can hand them straight to a quad drawer.
//
// A quad rather than `PathBounds` for the reason the caret is a segment: the
// axis-aligned box of a rotated line's highlight is visibly not the highlight,
// and it is larger than the text it claims to cover.
struct TextQuad {
  PathPoint corner[4];
};

// The quads to paint behind a selected range `[loByte, hiByte)` -- one per
// LINE the range covers.
//
// One per line rather than one per glyph because that is what a selection
// looks like, and because per-glyph rectangles of a proportional font leave
// hairline seams between them at fractional zoom. Each line's quad spans
// from the leftmost selected pen position on that line to the rightmost
// TRAILING edge (`x + advance`, text/Shaper.hpp) -- the same field the caret
// needs, for the same reason: without it the highlight stops in front of the
// last selected character.
//
// Empty when the range is empty, when the block is empty, or when shaping
// fails -- all three are "nothing to paint" rather than errors, matching what
// `textContentToShapes()` does with the same conditions.
//
// **Lines are grouped by the glyphs' baseline `y`**, and a selection that is
// contiguous in BYTES can be discontiguous on a line under bidi -- a
// right-to-left run inside a left-to-right paragraph. Each line still gets
// exactly one quad here, spanning the extremes, so such a selection paints
// wider than it strictly covers. That is the standard simplification and the
// alternative (a run-splitting pass over the reordered glyphs) is not worth
// building until this application has a bidi document to test it on.
std::vector<TextQuad> textSelectionQuads(const TextContent& text, size_t loByte, size_t hiByte);

// The four corners of the block's own frame -- the paragraph box for
// paragraph text, and the ink bounds for point text -- mapped through
// `transform`, for the outline the Text tool draws around the block it is
// editing.
//
// Exists so the drawing code does not rebuild `origin + frame` itself and
// then forget to map it: that is precisely the bug that leaves a frame
// sitting square while the type inside it is at an angle. `valid == false`
// for an empty block, matching `textContentBounds()`.
bool textFrameQuad(const TextContent& text, TextQuad* out);

// ==========================================================================
// 4b. Resizing the frame: eight handles, and what dragging one means
// ==========================================================================
//
// A paragraph frame is dragged out before a word of it is typed, so the size
// chosen is a guess. These are the affordance for changing that guess
// afterwards, with the text reflowing to the new width -- which costs nothing
// to implement, because `frame.width` is already an input to shaping and
// nothing caches a layout across a change to it.
//
// **Paragraph text only.** Point text has no frame -- section 2 -- so there
// is no box to resize and no handles are reported for it. Dragging a handle
// could in principle CONVERT a point block to a paragraph one, and that is a
// real feature, but it is not this one: `origin` means different things for
// the two (section 2b), so the conversion has to move the origin as well as
// set a width, and doing it silently as a side effect of grabbing a handle is
// how a caption ends up somewhere the user did not put it.
enum class TextFrameHandle : uint8_t {
  None,
  TopLeft,
  TopCenter,
  TopRight,
  MiddleLeft,
  MiddleRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};

// The eight handle positions in DOCUMENT space, mapped through the block's
// transform like everything else here -- so a rotated block's handles sit on
// its rotated corners rather than on the corners of its bounding box.
//
// Indexed by `TextFrameHandle` minus one; `count` is always 8 when this
// returns true. False for point text and for a block whose frame cannot be
// determined, with `*out` untouched.
struct TextFrameHandles {
  PathPoint at[8];
};
bool textFrameHandles(const TextContent& text, TextFrameHandles* out);

// Which handle is within `radiusDoc` of `atDoc`, or `None`.
//
// `radiusDoc` is a DOCUMENT-space distance, so the caller converts its screen
// pixel target through the zoom -- the same rule the glyph hit test and the
// Pen's anchors already follow, and the reason a handle stays equally
// grabbable at every magnification.
//
// Ties go to the handle earliest in the enum, which puts the corners ahead of
// the edge midpoints: at a small frame size the two overlap, and a corner --
// which resizes both axes -- is the more useful of the two to land on.
TextFrameHandle textFrameHandleAt(const TextContent& text, PathPoint atDoc, float radiusDoc);

// Drag `handle` to `toDoc`, rewriting `frame` (and `origin`, for the handles
// that move the frame's top or left edge).
//
// **The pointer is mapped into text space through the block's own transform**,
// so dragging the right edge of a rotated block widens it along ITS right,
// not along the document's x axis.
//
// **A drag that would collapse the frame is refused**, leaving `*text`
// untouched and returning false, rather than clamping: a zero-width frame
// wraps every character onto its own line, and a frame dragged through itself
// would otherwise flip inside out under the cursor. `minSizeDoc` is that
// floor, in document units.
//
// Height stays AUTOMATIC (`frame.height == 0`, section 2) unless a handle
// that owns the vertical is dragged. Widening a block that had been sizing
// its own height must not silently pin that height, or the next line typed
// would be clipped by a box the user never set.
bool textFrameResize(TextContent* text, TextFrameHandle handle, PathPoint toDoc, float minSizeDoc);

// The byte offset nearest `at` (document coordinates) -- click-to-place-caret.
//
// **`at` is mapped through the INVERSE of `transform` first**, so a click
// lands where the user sees the character, not where it would have been
// unrotated. This is the one place the inverse is needed, and it is why
// `transformTextLayer()` refuses a degenerate matrix: with no inverse there
// is no way to turn a click back into a byte, and the block becomes
// permanently uneditable.
//
// Nearest by the glyph's pen position, then snapped to that glyph's own
// cluster, so the returned offset is always a real UTF-8 boundary that
// `app/TextTool`'s operations can act on without clamping. A click past the
// last glyph on a line returns the end of the string, which is what makes
// "click to the right of the text and start typing" work.
//
// Returns 0 for empty text, a shaping failure, or a non-invertible
// `transform` -- there is one position in an empty string and it is 0.
size_t textOffsetAtPoint(const TextContent& text, PathPoint at);

}  // namespace np
