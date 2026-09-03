#pragma once

#include <string>
#include <string_view>

#include "core/TextContent.hpp"

// io/TextSerial -- a `LayerKind::Text` layer's content on disk, as the
// per-layer `np:text` attribute (PLAN.md phase 14; PRD K1-K3, I10, I11).
//
// This is io/PathSerial's sibling for a different payload, and it is built to
// the same design for the same reasons. Read io/PathSerial.hpp first; this
// header cites it rather than re-deriving what it already argues, and calls
// out only the two places a `TextContent` payload actually differs from a
// shape list.
//
// ==========================================================================
// Same carrier, same reason: hex string, bit-pattern floats
// ==========================================================================
//
// io/PathSerial.hpp's measured warning applies unchanged: this project's
// OpenImageIO silently drops an array-typed EXR header attribute on readback
// (no error, no warning), while `string` survives, so the carrier is
//
//     "nptext1:" <hex>
//
// two lowercase hex digits per byte, little-endian, floats as IEEE-754
// binary32 **bit patterns**. A decimal round trip is a *nearly* exact one,
// and `TextContent::style.sizePx`/`tracking`/`leading` and `origin` all feed
// `text/Shaper.hpp` positions directly -- a size that drifted by an ulp on
// every save/load cycle would eventually re-wrap a paragraph differently for
// no reason a user did.
//
// ==========================================================================
// Forward compatibility: the version is the prefix, not per-field tolerance
// ==========================================================================
//
// The version is read before a single byte is decoded, exactly as
// io/PathSerial does, so a build meeting `nptext2:` refuses BY NAME instead
// of misreading a payload whose framing changed. io/NpaintFile then carries
// the attribute through unchanged (PRD I10), the same wiring PathSerial
// already has -- see that module's own comment for why this belongs to the
// integrator and not to this file.
//
// **This is the coarse, whole-payload rule, not io/OpSerial's or
// io/CompSerial's per-record carry-forward, and for io/PathSerial.hpp's exact
// reason: a `TextContent` is homogeneous.** An op stack or a comp list is a
// sequence of independently-typed records, so a newer build can add a record
// KIND this one has never heard of and still usefully carry the rest verbatim.
// A `TextContent` is one struct with a fixed field list; any extension changes
// the framing of the WHOLE payload, not one record in it, so a per-field
// "unrecognised but carried" byte-bag would buy nothing and would put an I/O
// concern on `core::TextContent` the way io/PathSerial.hpp rejects putting one
// on `core::VectorShape`.
//
// The one place a per-value choice remains is `TextAlign`, which crosses the
// wire as an explicit number (0 Left, 1 Center, 2 Right, 3 Justified -- the
// C++ enumerators' own ordinals, but written as literals here so that
// reordering `text/Shaper.hpp`'s enum can never silently renumber a value
// already on disk). An align byte outside that range is refused the same way
// io/PathSerial.cpp's `readPath()` refuses an out-of-range `FillRule`,
// `LineCap` or `LineJoin`: **the whole decode fails**, it does not clamp to
// `Left` and it does not carry the field forward unread. Clamping would
// silently re-align a paragraph the user set some other way; carrying only
// this one field forward would need the same per-field infrastructure the
// previous paragraph just argued is not worth building for a single enum.
// Refusing the whole payload is consistent with everything else in this file
// being all-or-nothing, and it is not a real loss: PRD I10's guarantee is that
// an older build does not DESTROY a newer document, and NpaintFile meets that
// here the same way it does for `npvec2:` -- by carrying the whole attribute
// through unread rather than this module inventing a way to partially read it.
//
// ==========================================================================
// The one payload difference from io/PathSerial: a single struct, not a list
// ==========================================================================
//
// There is no record count and no length-prefixed record, because there is
// exactly one `TextContent` per attribute -- a Text layer has one content
// block, not a list of them (core/TextContent.hpp section 1). What IS
// length-prefixed, and bounded by the bytes actually remaining before a
// single byte is reserved, is `utf8` and `style.fontFamily`: both are
// user-controlled strings that can be arbitrarily long, and a `.npaint` can
// arrive from anywhere, so a corrupt length must fail immediately rather than
// reserve gigabytes first. io/PathSerial.cpp's `Reader::str()` already does
// exactly this bound; TextSerial.cpp's reader does the same check.
//
// ==========================================================================
// The format
// ==========================================================================
//
//     u32  utf8Length, utf8                 length-prefixed, NOT null-terminated
//     u16  fontFamilyLength, fontFamily
//     f32  sizePx, tracking, leading
//     u8   bold, italic                     0 or 1
//     f32  frame.width, frame.height
//     u8   align                            0 Left / 1 Center / 2 Right / 3 Justified
//     f32  origin.x, origin.y
//     paint fill, paint stroke              u8 on; f32 rgba x4
//     f32  strokeStyle.width
//     u8   strokeStyle.cap, strokeStyle.join
//     f32  strokeStyle.miterLimit
//     u16  dashCount
//     dashCount x f32                       strokeStyle.dashes
//     f32  strokeStyle.dashOffset
//
// `utf8Length` is a `u32`, unlike `fontFamilyLength`'s `u16`: a font family
// name is realistically a handful of words, but a text block's own content is
// exactly what a user might paste a page of, and capping it at 65 535 bytes
// would silently truncate a first paragraph that happens to run long.
//
// ==========================================================================
// Where this attaches, and what this file deliberately does not do
// ==========================================================================
//
// Wiring `np:text` into the actual `.npaint` container -- reading and writing
// the attribute on a `LayerKind::Text` layer's part -- is io/NpaintFile.cpp's
// job, matching how `np:vector` attaches (io/PathSerial.hpp's own note that
// this belongs to the integrator). That file is deliberately not touched
// here.
namespace np {

// The version tag every value produced here begins with, including its
// colon. Exposed so io/NpaintFile and `--selftest` can name it rather than
// spelling the literal a second time -- io/PathSerial's `kVectorShapeSerialPrefix`
// precedent.
inline constexpr const char* kTextContentSerialPrefix = "nptext1:";

// `text` as an `np:text` attribute value. Never fails and never returns an
// empty string -- an empty `TextContent` (the state a freshly-created, not
// yet typed-into Text layer is in) serialises to a well-formed payload with a
// zero-length `utf8`.
std::string serializeTextContent(const TextContent& text);

// The inverse. Returns false and leaves `*textOut` untouched when the value
// is malformed or carries a version this build does not know; `errorOut`,
// when non-null, receives a sentence naming what was wrong, in the io/Export
// refusal style every other refusal in this codebase follows.
//
// Every read is bounds-checked and every length is bounded by the bytes that
// actually remain before anything is reserved or resized, so a corrupt
// `utf8Length` or `fontFamilyLength` cannot make this allocate gigabytes
// before failing -- see the header note above on why that risk is concrete
// here and not hypothetical.
bool deserializeTextContent(std::string_view value, TextContent* textOut,
                            std::string* errorOut = nullptr);

}  // namespace np
