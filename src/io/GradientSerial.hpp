#pragma once

#include <string>
#include <string_view>

#include "core/Gradient.hpp"

// io/GradientSerial -- the on-disk encoding of `Document::gradients`, written
// as `np:gradients` on part 0, in exactly io/RegionSerial's shape and for
// exactly its reasons (io/NpaintFile.hpp's carrier convention: this project's
// OpenImageIO silently drops array attributes, so a binary payload travels as
// a hex `string`).
//
//     "npgrads1:" <hex>
//
// two lowercase hex digits per byte, little-endian, floats as IEEE-754
// binary32 **bit patterns** and not decimal renderings -- io/PathSerial's own
// reason, and it is the same reason here: a stop position or a geometry
// endpoint that shifted by one ulp per save/load cycle would drift a ramp over
// a session.
//
// --- The format ----------------------------------------------------------
//
//     u32  count
//     count x:
//       u16  nameLength, name
//       u8   kind          0 = Linear, 1 = Radial, 2 = Angular
//       u8   spread        0 = Pad,    1 = Repeat, 2 = Reflect
//       f32  x0, y0, x1, y1
//       u16  colorStopCount
//         f32 position, r, g, b, midpoint
//       u16  opacityStopCount
//         f32 position, opacity, midpoint
//
// **No id and no counter**, unlike `np:regions`. A `Paint::gradient` is a
// POSITION in this list, so the list's order *is* the identity -- see
// core/Gradient.hpp's `GradientDef` on why an entry is never erased. Writing
// an id beside the position would create a second thing that can disagree
// with the first.
//
// **The colour is what is in the document: linear-light and straight.** Not
// sRGB-encoded on the way out, for `core/Gradient.hpp` §2's reason -- an
// encoding in the file would make the saved ramp depend on a display
// transform, and the reader would have to know which one.
//
// --- Strictness ----------------------------------------------------------
//
// Whole-attribute, exactly io/RegionSerial's cut and with the same argument:
// a gradient record has no cross-reference to guess about, and this is the
// format's first version, so there is no earlier build's file to be lenient
// with. An unrecognised kind or spread byte, a truncated field or trailing
// bytes fail `deserializeGradients()` entirely and io/NpaintFile carries the
// whole `np:gradients` string verbatim (PRD I10).
//
// **The consequence is worth stating, because it is not the region case.** A
// document whose gradient table failed to decode opens with an EMPTY table,
// and its shapes' `Paint::gradient` indices then point past the end -- which
// core/VectorRaster paints as NOTHING, by `Paint::gradient`'s own contract.
// So a refused table gives a visibly unpainted shape and an unchanged
// attribute on the next save, rather than a shape painted some colour the
// file did not ask for. That is the intended direction: the file is not
// damaged and the failure is visible.
namespace np {

inline constexpr const char* kGradientSerialPrefix = "npgrads1:";

// `gradients` as an `np:gradients` attribute value. Never fails; an empty
// table serialises to a well-formed zero-count payload. io/NpaintFile still
// does not *write* the attribute when the table is empty -- a document with no
// gradients must produce the bytes it produced before this feature existed.
std::string serializeGradients(const GradientTable& gradients);

// The inverse. Returns false, leaving `*out` untouched, for a prefix this
// build does not recognise, a non-hex or odd-length payload, an out-of-range
// kind or spread byte, a truncated field, or trailing bytes after the declared
// count. `errorOut`, when non-null, receives a sentence naming what was wrong.
bool deserializeGradients(std::string_view value, GradientTable* out,
                          std::string* errorOut = nullptr);

}  // namespace np
