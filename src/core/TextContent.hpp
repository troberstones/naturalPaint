#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Path.hpp"
#include "core/VectorShape.hpp"
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
// `frame.width == 0` is point text: one line, no wrapping, and -- per
// text/Shaper.hpp's own rule -- no alignment, because there is nothing to
// align against. Any positive width is paragraph text: CoreText wraps to it
// and `align` means something.
//
// A separate `bool paragraph` was considered and rejected: it would be a
// second copy of a fact `frame.width` already carries, and the two could
// disagree (a `paragraph == true` block with `width == 0` has no defined
// meaning). One number, one reading.
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

  // Where the block's top-left sits in DOCUMENT coordinates. Shaping happens
  // in text-space with its own origin at the block's top-left
  // (text/Shaper.hpp), and this is the single translation applied on the way
  // out -- so moving a text block is one field, not a walk over glyphs.
  PathPoint origin;

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
PathBounds textContentBounds(const TextContent& text);

// Whether this text block would draw anything: non-empty after shaping AND at
// least one of fill/stroke on. The tool uses it to tell "the user has not
// typed yet" from "the user has typed and set the colour to invisible", which
// are the same picture and different problems.
bool textContentDraws(const TextContent& text);

}  // namespace np
