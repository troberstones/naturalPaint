#pragma once

#include <string>
#include <string_view>

#include "core/StrokesContent.hpp"

// io/StrokesSerial -- the `np:dabs` carrier for a `LayerKind::Strokes`
// layer's content: every dab record, and the id allocator beside them.
//
// **This is the deferral io/NpaintFile.hpp reserved by name.** That header's
// list said: "A `strokes` part and its `np:dabs` blob. `LayerKind::Strokes`
// exists as an enum value and core/Layer.hpp calls it an 'inert placeholder';
// there is no Dab type, no dab list and no stroke record anywhere in `core/`.
// brush/StrokePath emits dabs into the *solver*, not into a document.
// Unblocked by a Strokes layer that actually holds dabs." core/StrokesContent
// is that layer's content, so the deferral is paid off here.
//
// io/FlatsSerial's and io/TextSerial's sibling for a `StrokesContent`, and
// deliberately the same carrier as both: a hex `string`, not the `<blob>`
// docs/document-format.md's table names.
// **That is not a preference, it is the measured constraint the other two
// carriers record**: this OpenImageIO drops array-typed EXR header attributes
// silently on write, so a dab list written as a blob would vanish on every
// save with nothing to notice it. Same bit-pattern floats, and the same
// version-in-the-prefix rule (`npdabs1:`), so a payload from a build that
// wrote `npdabs2:` is refused by name and carried verbatim rather than
// half-decoded (PRD I10).
//
// **What is deliberately NOT carried: the rasterised pixels.** They are
// derived -- from the records AND from the composite beneath the layer, which
// is not in this file and must not be (core/StrokesContent §2) -- so writing
// them would bake a copy of the layers underneath into the layer above them
// and defeat PRD D6 on the next reopen. This is the same argument
// io/FlatsSerial makes about the label field and io/TextSerial about glyph
// outlines, with one extra term.

namespace np {

// A hex string, `npdabs1:` prefixed. Never fails.
std::string serializeStrokesContent(const StrokesContent& content);

// The inverse. On failure returns false, leaves `*contentOut` untouched, and
// -- when `errorOut` is non-null -- writes one sentence saying why: an
// unrecognised version tag, a non-hex character, a truncated payload, or an
// implausible dab count (the allocation-bomb case).
bool deserializeStrokesContent(std::string_view value, StrokesContent* contentOut,
                               std::string* errorOut);

}  // namespace np
