#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/PathStroke.hpp"
#include "core/VectorShape.hpp"

// io/PsdVectorStyle -- the paint half of importing a Photoshop shape layer.
//
// The colour lives in an Action Descriptor, and THE COMMON CARRIER IS NOT THE
// OBVIOUS ONE: of the nine shape layers in Apple's `App Icon Template.psd`,
// one has a `SoCo` (solid colour) block and eight have only `vscg` ("vector
// stroke content"). A reader that knows about `SoCo` alone imports one shape
// in nine and looks like it worked. See docs/psd-vector-shapes.md.
//
// This module is also the first thing in the tree to feed real Photoshop bytes
// to io/Descriptor, whose header is still honest that it is unproven against a
// PSD. Every osType these blocks use (`Objc doub VlLs long bool UntF enum
// TEXT`) is in that parser's set and none of the four it refuses by name
// occurs -- but that is a reading of the grammar, not a run.
namespace np {

// The per-key skip before the embedded versioned Action Descriptor begins.
// `vscg` leads with a four-character fill-type tag (`SoCo` in every block seen)
// and `vogk` with a version word; `SoCo` and `vstk` start at the descriptor.
inline constexpr size_t kPsdSocoDescriptorSkip = 0;
inline constexpr size_t kPsdVscgDescriptorSkip = 4;
inline constexpr size_t kPsdVstkDescriptorSkip = 0;

// The tagged blocks a shape layer's style is spread across. Each span is that
// block's payload exactly as io/PsdImport already slices it, with no skip
// applied -- this module applies the skips above. An absent block is an empty
// span, which is normal rather than an error: six of nine layers in the
// motivating file have no `SoCo` at all.
struct PsdVectorStyleBlocks {
  std::span<const uint8_t> soco;
  std::span<const uint8_t> vscg;
  std::span<const uint8_t> vstk;
};

struct PsdVectorStyle {
  // `Paint::on == false` means the shape genuinely has no fill, which is NOT
  // the same as an alpha of zero and is the receiving field for Photoshop's
  // disabled fill. `PNG/4 - Layer.png` carries an orange colour and is
  // invisible in Photoshop because `vstk.fillEnabled` is false; a reader that
  // takes the colour without the flag paints a solid orange circle nobody
  // authored.
  Paint fill;
  Paint stroke;
  StrokeStyle strokeStyle;

  // Per-layer observations for `PsdImportResult::warnings`: a gradient or
  // pattern fill that has no receiving field, a stroke alignment this
  // codebase cannot express, an unreadable descriptor. Never fatal -- a shape
  // whose style could not be read still has its geometry.
  std::vector<std::string> warnings;
};

// Reads `blocks` into `out`.
//
// Fill precedence matches psd-tools' compositor: `SoCo`, then `PtFl`, then
// `GdFl`, then `vscg` -- all gated on `vstk.fillEnabled`, which defaults TRUE
// when there is no `vstk` block at all (a layer with a fill and no stroke data
// is filled). `strokeEnabled` gates the stroke the same way but defaults
// FALSE, because a stroke exists only if something described one.
//
// Colour arrives as `RGBC` doubles in 0..255, sRGB-encoded; `Paint::rgba` is
// linear-light straight alpha, so it goes through `color::srgbDecode()`
// exactly as io/SvgImport does. Compare an imported colour against these
// descriptor doubles and never against psd-tools' render, which drifts by up
// to 10 units of 255 on a dark fill.
//
// `GdFl` and `PtFl` have no receiving field (`Paint` is solid only, argued at
// core/VectorShape.hpp). They are a named warning with `fill.on = false`, not
// a flat colour guessed from a gradient stop. `strokeStyleLineAlignment`
// other than centre is the same kind of refusal: core/PathStroke centres every
// stroke.
//
// Returns false with `error` set only when a block is present and its bytes
// are unreadable; a wholly absent style is a success with everything off.
bool decodePsdVectorStyle(const PsdVectorStyleBlocks& blocks, PsdVectorStyle& out,
                          std::string& error);

}  // namespace np
