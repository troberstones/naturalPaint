#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Region.hpp"

// io/RegionSerial -- the on-disk encoding of `Document::regions`, written as
// `np:regions` on part 0 (io/NpaintFile.hpp's carrier convention: "no
// working blob carrier today", so this is a hex `string` attribute, exactly
// the fix `io/OpSerial` and `io/CompSerial` already took for the same
// OpenImageIO limitation).
//
// The attribute's value is
//
//     "npregions1:" <hex>
//
// two lowercase hex digits per byte, little-endian, `io/CompSerial.hpp`'s
// three inherited properties: the version is the prefix and read before
// anything is decoded; a future version is refused by name; and nothing here
// half-reads a payload it does not fully recognise.
//
// --- The format --------------------------------------------------------
//
//     u64  nextRegionId
//     u16  count
//     count x:
//       u64  id
//       u8   kind          0 = Frame, 1 = Slice
//       i32  x
//       i32  y
//       u32  width
//       u32  height
//       u16  nameLength, name
//
// --- What this format deliberately does NOT do, and why -----------------
//
// `io/CompSerial.hpp` decodes a comp record it cannot fully parse into a
// `LayerComp` with `known = false`, so **one** bad record does not cost the
// rest of the list -- because a comp entry also carries a layer-id join that
// a half-read would have to guess at, and because comps are old enough to
// have an install base that already produced files this build cannot fully
// interpret.
//
// This format skips that granularity **on purpose, as a scoped cut rather
// than an oversight**: a region record has no cross-reference to guess about
// (it is five self-contained numbers and a name), and this is the format's
// first version -- there is no earlier build's file to be lenient with yet.
// So the strictness lives at the **whole-attribute** level only, exactly
// where `io/CompSerial`'s own outer version-prefix check already lives: a
// payload this build cannot fully parse -- a bad kind byte, a truncated
// field, trailing bytes -- fails `deserializeRegions()` entirely, and
// io/NpaintFile carries the **whole** `np:regions` string verbatim
// (PRD I10) rather than decoding the records it can and dropping the one it
// cannot. The document opens with **no** regions from that attribute rather
// than a partial list silently missing one. If a second on-disk version of
// this format is ever needed, add the record-level carry then, against a
// real cross-build compatibility case rather than a hypothetical one.
namespace np {

inline constexpr const char* kRegionSerialPrefix = "npregions1:";

// Everything `np:regions` carries.
struct RegionCarrier {
  uint64_t nextRegionId = 1;
  std::vector<Region> regions;

  friend bool operator==(const RegionCarrier&, const RegionCarrier&) = default;
};

// `in` as an `np:regions` attribute value. Never fails; an empty carrier
// serialises to a well-formed zero-count payload. io/NpaintFile still does
// not *write* the attribute when there are no regions -- a document with
// none must produce the bytes it produced before this feature existed.
std::string serializeRegions(const RegionCarrier& in);

// The inverse. Returns false, leaving `*out` untouched, for a prefix this
// build does not recognise, non-hex or odd-length payload, an out-of-range
// `kind` byte, a truncated field, or trailing bytes after the declared
// count -- see this header's "what this deliberately does not do" section
// for why a bad record fails the whole payload rather than being carried in
// place.
//
// `errorOut`, when non-null, receives a sentence naming what was wrong.
bool deserializeRegions(std::string_view value, RegionCarrier* out,
                        std::string* errorOut = nullptr);

}  // namespace np
