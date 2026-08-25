#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "brush/Library.hpp"

namespace np {

// Reading Photoshop `.abr` brush libraries into `brush/Library`'s presets.
//
// The parameters, not the tips. **A `.abr` from any modern brush pack is
// mostly a set of SAMPLED BITMAP tips** -- the `samp` block -- and this build
// has no bitmap tip: `brush/Deposit.hpp`'s `dabCoverage()` is a procedural
// radial profile. So an imported brush arrives with its name, its size, its
// spacing, its roundness and angle, and its whole dynamics graph, and it
// paints with this application's round tip rather than with the shape that
// makes the original recognisable.
//
// That is worth being blunt about, because it decides whether the feature is
// useful to you: importing Kyle Webster's inkers gives you twelve brushes that
// behave like the originals and do not look like them. The reader below
// reports the tip it could not bring across, per brush, rather than leaving
// that to be discovered.
//
// The format: a 2-byte version and 2-byte subversion, then a series of `8BIM`
// sections -- `samp` (the bitmap tips), `patt` (patterns), `desc` (the brush
// parameters, as one Action Descriptor) and `phry` (the hierarchy). Only
// version 6 is read; 1 and 2 are a different, much older layout that no modern
// pack uses, and are refused by name rather than guessed at.

// What a single brush lost on the way in. One line per brush that lost
// something, so the import can say what it did rather than only what it took.
struct AbrImportNote {
  std::string brushName;
  std::string what;
};

struct AbrImportResult {
  bool ok = false;
  std::string error;  // set when ok is false, and then presets is empty

  std::vector<BrushPreset> presets;
  std::vector<AbrImportNote> notes;

  // Counters for the summary line, so a caller does not have to walk `notes`
  // to say something useful.
  size_t sampledTips = 0;    // brushes whose tip is a bitmap we cannot use
  size_t unmappedControls = 0;  // dynamics whose control has no source here
};

// Parse a whole `.abr` file.
//
// Reads no byte outside `bytes`, for any content whatsoever -- the same
// contract io/Descriptor.hpp holds itself to, and for the same reason: this
// parses a file format from the internet.
AbrImportResult importAbrBrushes(std::span<const uint8_t> bytes);

// --- The mapping, exposed so it can be tested without a file ---------------

// Photoshop's brush dynamics "Control" dropdown, the `bVTy` integer.
//
// Named rather than left as a magic number because three of the eight have no
// counterpart here and the import has to say so: Photoshop can drive a
// parameter from the stroke's DIRECTION, which this build does not compute,
// and from a stylus wheel, which is a device axis SDL does not report.
enum class AbrControl {
  Off = 0,
  Fade = 1,
  PenPressure = 2,
  PenTilt = 3,
  StylusWheel = 4,
  Rotation = 5,
  InitialDirection = 6,
  Direction = 7,
};

const char* abrControlName(int bVTy) noexcept;

// The source a control maps to, or false when it has none.
//
// `Rotation` takes Barrel and `StylusWheel` does not, even though both are
// "the pen twisted": barrel rotation is a real SDL axis (`SDL_PEN_AXIS_ROTATION`,
// see app/PenAxes.hpp) and the airbrush wheel is not. Mapping both onto Barrel
// would put two different physical inputs in one matrix cell and silently make
// one of them a lie.
bool abrControlToSource(int bVTy, DynamicSource& out) noexcept;

// Photoshop's spacing is a percentage of the tip's DIAMETER; `BrushTip::spacing`
// is in units of its RADIUS. The factor of two is the whole conversion, and
// getting it wrong halves or doubles every imported brush's dab count --
// which reads as "the import made everything grainy" rather than as a unit bug.
float abrSpacingToRadii(double percentOfDiameter) noexcept;

}  // namespace np
