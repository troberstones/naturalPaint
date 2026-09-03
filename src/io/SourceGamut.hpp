#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "color/Gamut.hpp"

// io/SourceGamut -- what gamut a file's numbers are actually in, read from the
// file's own bytes.
//
// ==========================================================================
// Why this reads the container and not the decoder
// ==========================================================================
//
// The obvious place to ask "what colour space is this" is the decoder, and for
// this codebase that is the one place it cannot be asked. `decodeImageLinear()`
// runs **stb first, always** -- deliberately, so PRD I1's four formats decode
// identically regardless of what OpenImageIO does -- and stb_image exposes no
// ICC profile, no `cHRM`, no `sRGB` chunk, nothing. OpenImageIO is only a
// fallback for what stb declines.
//
// So exactly the formats where this matters most -- the JPEG off a phone, the
// PNG off the screenshot key, both Display P3 by default on macOS -- are the
// formats whose decoder can never answer. Reading the container directly is
// not a workaround for a missing API; it is the only place the answer exists
// for the files that need it.
//
// It also means the answer is decoder-independent: a TIFF that happens to go
// through OpenImageIO and a PNG that goes through stb get their gamut from the
// same code, and neither can drift from the other.
//
// ==========================================================================
// What is read, in what order, and what is deliberately not
// ==========================================================================
//
// **PNG**, walking the chunk list:
//   1. `iCCP` -- an embedded ICC profile, zlib-deflated. This is what macOS
//      writes into a screenshot, and it is the authoritative answer.
//   2. `sRGB` -- an explicit "this is sRGB" marker. Rec.709 primaries, so the
//      answer is "no conversion", stated rather than assumed.
//   3. `cHRM` -- explicit chromaticities. Rare in practice but free to read
//      once the walk exists, and unambiguous when present.
// The PNG specification gives `sRGB` and `iCCP` precedence over `cHRM`, and
// says a file should not carry both `sRGB` and `iCCP`; when one does, the
// profile wins here because it is the more specific statement.
//
// **JPEG**: the `APP2` segments whose payload begins `ICC_PROFILE\0`. The
// profile is split across segments with a 1-based sequence number and a total,
// and is reassembled in sequence order before parsing. Not deflated -- JPEG
// stores the profile bytes directly.
//
// **Deliberately not read**: PNG's `gAMA`, and any profile's transfer curve.
// This module answers "which primaries", and the transfer function stays
// `io/ImageDecode`'s existing sRGB assumption. That is a real remaining gap
// and it is stated here rather than being left for someone to discover: an
// Adobe RGB (1998) file is gamma 2.2, not sRGB's curve, so its tones are
// still slightly off after this module has corrected its primaries. It is a
// much smaller error than the gamut one -- the two curves differ by under 1%
// over most of the range, where P3-as-Rec.709 misplaces a saturated red by
// more than 20% -- and closing it means a transfer-curve model this codebase
// does not have. Named so it is a known limitation with a size, not a
// surprise.
//
// **Also deliberately not read**: anything but a matrix-shaper profile. A
// look-up-table profile (`A2B0`), a CMYK profile, a device-link -- none of
// these has colorant tags, so `iccRgbToXyzD50()` returns `std::nullopt` and
// the caller falls back to "assume Rec.709", which is exactly the behaviour
// that existed before this module. This module can only ever improve on that
// baseline or match it; it has no path to making a file worse.
namespace np {

// A gamut discovered in a file, as the RGB->XYZ(D50) matrix color/Gamut
// works in.
struct SourceGamut {
  // False when the file said nothing this module understands. The caller then
  // does what it always did: treat the numbers as Rec.709.
  bool known = false;

  ColorMat3 rgbToXyzD50;

  // Where the answer came from -- "PNG iCCP", "PNG sRGB", "PNG cHRM",
  // "JPEG ICC". For diagnostics and for `--selftest` to assert that a fixture
  // was read the way it was meant to be, rather than arriving at the right
  // matrix down the wrong path.
  const char* source = "";
};

// Reads `data` as a container and returns whatever gamut it declares.
// Never throws and never reads out of bounds on a truncated or hostile file:
// every offset is bounds-checked against `size` before use, because this
// parses attacker-supplied bytes before anything has validated them.
SourceGamut sniffSourceGamut(const uint8_t* data, size_t size);

// The RGB->XYZ(D50) matrix an ICC profile's `rXYZ`/`gXYZ`/`bXYZ` colorant
// tags describe, or `std::nullopt` if this is not a matrix-shaper RGB profile
// (or is malformed).
//
// **The colorants are already D50-adapted** -- the ICC profile connection
// space is defined at D50 and a matrix-shaper profile stores its colorants in
// it. So this needs no chromatic adaptation of its own, which is the whole
// reason color/Gamut's vocabulary is D50-relative.
std::optional<ColorMat3> iccRgbToXyzD50(const uint8_t* icc, size_t size);

// Exposed for `--selftest`: the reassembled ICC profile bytes a JPEG's `APP2`
// segments carry, or empty when there are none.
std::vector<uint8_t> jpegIccProfile(const uint8_t* data, size_t size);

}  // namespace np
