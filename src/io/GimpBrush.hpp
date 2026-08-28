#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace np {

// io/GimpBrush -- GIMP's `.gbr` (one brush) and `.gih` (an "image hose", N of
// them), read for their tip bitmaps.
//
// **Why these two and not more formats.** The dab library is a watched folder:
// anything dropped in becomes a tip. Images already arrive free through
// io/ImageDecode. `.gbr`/`.gih` are the one widely-distributed brush corpus
// that is neither an image nor a `.abr`, both formats are tiny, and both are
// **published** -- developer.gimp.org carries the field-by-field standard.
//
// **Implemented from that specification page, not from GIMP's source.**
// docs/brush-model-references.md draws this line around Krita's `kis_brush`
// and it applies identically here: GIMP is GPL, this project is not, and a
// derived work would force it to be. The specification is documentation and
// carries no such condition. Nothing in this file was read out of GIMP.
//
// **Never run against a file GIMP itself wrote.** No `.gbr` or `.gih` was
// available on the machine where this was written, so every byte of coverage
// is a hand-built fixture -- the same caveat io/AbrBrushes.hpp carried for
// `.abr` until real packs turned up, stated here with the same force. The
// FRAMING is quoted from a published standard and is not the risk; the one
// genuine inference is polarity, below.

// One decoded brush: the tip, in `BrushTipBitmap`'s own convention.
struct GimpBrushTip {
  std::string name;
  int32_t width = 0;
  int32_t height = 0;
  // Row-major, top to bottom, **255 = full coverage** -- the same polarity
  // `BrushTipBitmap::alpha` uses, so a caller does not have to remember which
  // way round this particular format is.
  //
  // **Polarity is the one inference in this module.** A `bytes == 1` brush
  // stores a mask, and this reader takes that byte as coverage directly (255
  // paints); a `bytes == 4` brush stores RGBA and this reader takes its alpha.
  // Both readings follow from "the file stores a mask, not a picture", which
  // is what the standard's own field name says -- but neither has been checked
  // against a brush GIMP wrote. **What would settle it:** open any stock GIMP
  // `.gbr` here and in GIMP, and compare which end of the stroke is solid. If
  // it is inverted, one line changes and this comment goes with it.
  std::vector<uint8_t> alpha;
  // The file's own default spacing, as a percentage of the brush WIDTH.
  // Absent from version 1, which is why it is optional rather than defaulted
  // silently to something that would look like the file's opinion.
  bool haveSpacing = false;
  uint32_t spacingPercent = 0;
};

struct GimpBrushResult {
  bool ok = false;
  std::string error;  // set when ok is false, and then `tips` is empty
  std::vector<GimpBrushTip> tips;
};

// One `.gbr`. Versions 1 and 2 both: version 1's header stops after `bytes`
// (no magic, no spacing) and version 2 adds the `GIMP` magic and a spacing
// field, which is the whole difference.
GimpBrushResult readGimpBrush(std::span<const uint8_t> bytes);

// One `.gih`: a UTF-8 name line, a parameter line, then `ncells` complete
// `.gbr` records concatenated.
//
// **The hose's animation semantics are deliberately not implemented.** The
// parameter line describes how GIMP picks a cell per dab -- by angle, by
// pressure, incrementally, across up to four ranked dimensions. This build has
// one tip per brush, so the cells arrive as N independent dabs named
// `<name> 01`, `<name> 02`, and the placement rule is dropped. That is a real
// loss and it is stated rather than left to be discovered: a `.gih` imported
// here is a set of tips, not an animated brush.
GimpBrushResult readGimpBrushPipe(std::span<const uint8_t> bytes);

// The largest edge this reader will allocate for -- the same cap
// `parseAbrSampledTips()` and `parseAbrPatterns()` use, kept identical so
// there is one number to remember rather than three.
inline constexpr int32_t kMaxGimpBrushDimension = 4096;

}  // namespace np
