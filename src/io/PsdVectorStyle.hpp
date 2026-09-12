#pragma once

#include <cstdint>
#include <optional>
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
//
// ==========================================================================
// `GdFl`: what is measured, and what is NOT -- read this before trusting it
// ==========================================================================
//
// docs/psd-vector-shapes.md S2 gave gradients a receiving field, and this
// module decodes `GdFl` into it. **No file on the machine this was written on
// carries a `GdFl` block at all** -- both sample PSDs were grepped, and both
// have zero occurrences -- so unlike `SoCo`, `vscg` and `vstk`, whose every
// fixture is a hex dump of real Photoshop bytes, nothing below was checked
// against Photoshop. It is Adobe's published descriptor structure plus
// psd-tools' reading of it, and it is named as such here rather than left to
// look verified, exactly as docs/psd-vector-shapes.md names the path-operation
// numbering it took on psd-tools' authority.
//
// What that means in practice, split by how badly each half would fail:
//
//  * **The ramp** -- `Grad/Clrs` and `Grad/Trns`, their `Lctn` in 0..4096,
//    `Mdpn` in 0..100, `Opct` as a `#Prc`, and `Clr ` as the same `RGBC`
//    triple `SoCo` uses. This is the half that decides what colours appear,
//    and a wrong key name here produces an EMPTY ramp, which paints nothing
//    and warns -- a visible failure, not a plausible one.
//  * **The placement** -- `Type`, `Angl`, `Scl `, `Ofst`, `Algn`. This is the
//    half that decides WHERE the ramp sits, and getting it wrong produces a
//    gradient that appears with the right colours in the wrong place, which
//    is exactly the "opens without error and is confidently wrong" failure
//    this project's refusal discipline exists against. It is therefore split
//    out into `PsdGradientPlacement` -- what the file SAID -- and
//    `psdGradientGeometryFor()`, which resolves it against a shape's extent,
//    so the formula is one pure function that can be hand-checked and
//    re-derived against a real file when one turns up, rather than being
//    spread through the decoder.
//
// `psdGradientGeometryFor()`'s own comment states the formula and each thing
// in it that is a reading rather than a measurement.
namespace np {

// The per-key skip before the embedded versioned Action Descriptor begins.
// `vscg` leads with a four-character fill-type tag (`SoCo` in every block seen)
// and `vogk` with a version word; `SoCo` and `vstk` start at the descriptor.
inline constexpr size_t kPsdSocoDescriptorSkip = 0;
inline constexpr size_t kPsdVscgDescriptorSkip = 4;
inline constexpr size_t kPsdVstkDescriptorSkip = 0;
// A standalone `GdFl` starts at its descriptor, like `SoCo`. Inside `vscg` it
// is the same descriptor after that block's own four-character tag, which
// `kPsdVscgDescriptorSkip` already covers.
inline constexpr size_t kPsdGdflDescriptorSkip = 0;

// The tagged blocks a shape layer's style is spread across. Each span is that
// block's payload exactly as io/PsdImport already slices it, with no skip
// applied -- this module applies the skips above. An absent block is an empty
// span, which is normal rather than an error: six of nine layers in the
// motivating file have no `SoCo` at all.
struct PsdVectorStyleBlocks {
  std::span<const uint8_t> soco;
  std::span<const uint8_t> vscg;
  std::span<const uint8_t> vstk;

  // A standalone pattern or gradient fill, the two carriers that sit where
  // `SoCo` would. `GdFl` is decoded into `PsdVectorStyle::fillGradient`;
  // `PtFl` still has no receiving field and is NAMED in a warning rather than
  // decoded -- without it a top-level `PtFl` layer would report "no fill
  // block" and read as an accident instead of an unsupported fill. Both start
  // at their descriptor, skip 0.
  std::span<const uint8_t> ptfl;
  std::span<const uint8_t> gdfl;
};

// Photoshop's gradient PLACEMENT, exactly as the descriptor states it and
// before any shape's extent is involved.
//
// Kept raw rather than resolved inside the decoder so that "what the file
// said" and "where that puts the ramp" are two separately checkable things --
// see this header's `GdFl` section on why the placement half is the dangerous
// one.
struct PsdGradientPlacement {
  GradientKind kind = GradientKind::Linear;
  GradientSpread spread = GradientSpread::Pad;
  // `Angl`, a `#Ang` in degrees. Photoshop measures it counter-clockwise from
  // the +x axis on a y-UP screen: 0 is left-to-right, 90 is bottom-to-top.
  double angleDegrees = 0.0;
  // `Scl `, a `#Prc`. 100 means the ramp exactly spans the shape.
  double scalePercent = 100.0;
  // `Ofst/Hrzn` and `Ofst/Vrtc`, both `#Prc` of the shape's own width and
  // height, moving the ramp's centre off the shape's centre.
  double offsetXPercent = 0.0;
  double offsetYPercent = 0.0;
};

// One decoded `GdFl`: the ramp, where Photoshop said to put it, and the name
// it carried.
//
// **The caller owns the table index.** This module has no `core::Document`, so
// it cannot append to `Document::gradients` and cannot fill in
// `Paint::gradient`. io/PsdImport does both, which keeps the index with
// exactly one writer.
struct PsdGradientFill {
  std::string name;
  // Already reversed when `Rvrs` was set, so a consumer never has to know.
  GradientStops stops;
  PsdGradientPlacement placement;
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

  // Engaged when the layer's fill is a `GdFl` this module could decode AND
  // `vstk.fillEnabled` allowed it. `fill.on` is left FALSE in that case: the
  // caller turns it on once it has appended the gradient to the document's
  // table and knows the index, so there is never a moment where `fill.kind` is
  // `Gradient` and `fill.gradient` is a placeholder.
  std::optional<PsdGradientFill> fillGradient;

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
// `GdFl` decodes into `out.fillGradient` -- see this header's `GdFl` section
// for what about that is measured and what is not. `PtFl` still has no
// receiving field and is a named warning with `fill.on = false`, never a flat
// colour guessed from a pattern. A `GdFl` this module cannot make a ramp out
// of -- a noise gradient (`GrdF` = `ClNs`), a `Dmnd` (diamond) type, an empty
// `Clrs` list -- is the same kind of named refusal rather than a guess.
// `strokeStyleLineAlignment` other than centre is one too: core/PathStroke
// centres every stroke.
//
// **A gradient STROKE is not decoded**, and that is a scope cut rather than an
// absence: `vstk.strokeStyleContent` can carry a `GdFl` exactly as it carries
// a `Clr `, and `Paint` can now receive it, but no sample file has one and the
// stroke path would need its own placement resolution against the stroke's
// outline rather than the fill's. It warns by name, and the stroke is left
// off.
//
// Returns false with `error` set only when a block is present and its bytes
// are unreadable; a wholly absent style is a success with everything off.
bool decodePsdVectorStyle(const PsdVectorStyleBlocks& blocks, PsdVectorStyle& out,
                          std::string& error);

// Where `placement` puts its ramp on a shape whose tight bounds are `bounds`,
// in DOCUMENT TEXEL coordinates -- the same space `VectorShape::path` is in.
//
// **The formula, stated so it can be argued with rather than reverse-engineered:**
//
//   centre  = bounds centre, moved by (Hrzn % of width, Vrtc % of height)
//   dir     = (cos a, -sin a)      -- the y negation is document space being
//                                     y-DOWN while Photoshop's angle is y-UP
//   length  = (|w cos a| + |h sin a|) * Scl/100
//                                  -- the bounds projected onto `dir`, i.e.
//                                     the span a ramp needs to cross the shape
//                                     at that angle
//
//   Linear     p0 = centre - dir*length/2, p1 = centre + dir*length/2, Pad
//   Reflected  p0 = centre,                p1 = centre + dir*length/2, Reflect
//   Radial     p0 = centre,                p1 = centre + (radius, 0), Pad,
//              radius = |(w, h)|/2 * Scl/100 -- half the DIAGONAL, so the ramp
//              reaches the corners rather than stopping at the edge midpoints.
//              The angle is meaningless for a radial and is not used.
//   Angular    p0 = centre,                p1 = centre + dir  -- only the
//              direction matters; `length` and `spread` do not apply.
//
// **Three of those are readings rather than measurements**, and they are the
// ones to check first against the first real file that turns up: the angle's
// sign convention, the projected-length rule, and the radial's use of half the
// diagonal. The rest -- the centre, the offset percentages being of width and
// height respectively, the scale being a plain multiplier -- follow from the
// field names and units.
//
// Degenerate bounds (invalid, or zero in both axes) give a zero-length
// geometry, which core/Gradient renders as a flat fill of the first stop
// rather than as NaN -- `gradientParameterAt()`'s own documented behaviour for
// a zero-length drag.
GradientGeometry psdGradientGeometryFor(const PsdGradientPlacement& placement,
                                        const PathBounds& bounds);

}  // namespace np
