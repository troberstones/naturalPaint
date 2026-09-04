#pragma once

#include <string>
#include <string_view>

#include "flats/Model.hpp"

// io/FlatsSerial -- the `np:flats` carrier for a `LayerKind::Flats` layer's
// content: its segmentation parameters, every recorded repair, and its
// palette. io/TextSerial's sibling for a `FlatsContent` instead of a
// `TextContent`: the same hex-string carrier (an OpenEXR string attribute
// must be text, and a `.npaint` from anywhere must be refused rather than
// crashed on), the same bit-pattern floats, the same version-in-the-prefix
// rule (`npflats1:`).
//
// docs/autoflats-migration.md §4.1 sketched three attributes (`np:flatParams`,
// `np:flatEdits`, `np:fills`). They are one here, for the reason `np:text` is
// one: the three are never valid separately -- a recolour names a palette
// slot, a carve reads the gap size -- and a file that carried two of the three
// would have a layer in a state this build cannot construct. One attribute,
// one version, one refusal.
//
// **The label field is NOT carried.** It is derived (flats/Model.hpp §1), the
// way a Text layer's glyph outlines are, and re-derives on open. The
// migration doc's `flat.id` UINT channel is the cache a future build may add
// for a large flat that is slow to re-evaluate; nothing about this carrier
// changes when it does.

namespace np {

// A hex string, `npflats1:` prefixed. Never fails.
std::string serializeFlatsContent(const FlatsContent& content);

// The inverse. On failure returns false, leaves `*contentOut` untouched, and
// -- when `errorOut` is non-null -- writes one sentence saying why: an
// unrecognised version tag, a non-hex character, a truncated payload, an
// implausible count (the allocation-bomb case).
bool deserializeFlatsContent(std::string_view value, FlatsContent* contentOut,
                             std::string* errorOut);

}  // namespace np
