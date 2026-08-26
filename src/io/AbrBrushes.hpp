#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "brush/Library.hpp"

namespace np {

// Reading Photoshop `.abr` brush libraries into `brush/Library`'s presets --
// the parameters AND, now, the tips.
//
// **A `.abr` from any modern brush pack is mostly a set of SAMPLED BITMAP
// tips** -- the `samp` block, a UUID-keyed greyscale bitmap per brush,
// usually PackBits-compressed. This reader decodes it and attaches the
// result to the preset as a `BrushTipBitmap` (`brush/Deposit.hpp` §2c), which
// `dabCoverage()` samples in place of its procedural radial profile. Ten of
// Kyle Webster's twelve Runny Inkers carry one; before this, all ten painted
// with this application's round tip regardless of what the brush pack drew.
//
// **Three ways a sampled tip can still fail to arrive**, and each is counted
// in `AbrImportResult::sampledTips` and named in `notes` exactly as before --
// the brush then paints with the round procedural tip, and the import says
// so rather than leaving that to be discovered:
//
//   * the `desc` block's `sampledData` id names no sample this file's `samp`
//     block actually contains (a corrupt or hand-edited file);
//   * the sample decoded to something this reader does not trust -- a
//     truncated PackBits stream, a degenerate `width`/`height`, dimensions
//     large enough that decoding one would be an unbounded allocation from an
//     untrusted file;
//   * the sample's depth is not 8 bits. Every sample found by direct
//     inspection of a real Kyle Webster pack (see below) is 8-bit greyscale,
//     and the openly-published `abrupng` reader this format was checked
//     against refuses anything else outright -- but that is corroboration
//     from one file and one other reader, not proof of Adobe's own rule, so a
//     different depth is refused by name rather than reinterpreted.
//
// A `Dmtr` (diameter) of `#Prc` -- a percentage rather than a pixel count --
// used to be refused outright: a percentage of WHAT is not knowable from the
// descriptor alone. It is knowable once the sample's own pixel dimensions are
// known, so a `#Prc` diameter on a brush whose sample decoded successfully is
// now resolved against it (see `presetFromDescriptor()`'s own comment); one
// with no usable sample is still refused, for the same reason as before.
//
// The format: a 2-byte version and 2-byte subversion, then a series of `8BIM`
// sections -- `samp` (the bitmap tips), `patt` (patterns), `desc` (the brush
// parameters, as one Action Descriptor) and `phry` (the hierarchy), in that
// order in every file this reader has been driven against, though the walk
// below does not assume it. Only version 6 is read; 1 and 2 are a different,
// much older layout that no modern pack uses, and are refused by name rather
// than guessed at. The `samp` record framing this reader implements --
// per-brush length, a 37-byte `$`-prefixed UUID key, a subversion-dependent
// header skip (47 bytes for subversion 1, 301 for subversion 2), a
// `top`/`left`/`bottom`/`right` bounds rectangle, a depth and a compression
// flag, 4-byte-aligned between records -- is **not** published by Adobe.
// It was derived two ways and cross-checked against each other: reading the
// openly-published Rust `abrupng` project's parser (github.com/scurest/abrupng,
// `src/abr/abr6.rs`, MIT-licensed, no relation to this codebase and not
// vendored into it), and a byte-for-byte walk, in a throwaway script outside
// this build, of a real 2.4 MB Kyle Webster pack -- the same file
// `io/Descriptor.hpp`'s own header says was not available when THAT module
// was written; it was, by the time this one was. Both gave the same field
// offsets and the same three decoded sample dimensions (37x52, 14x14,
// 120x93), and the PackBits-decoded byte counts matched `width * height`
// exactly for all three, which is what "cross-checked" means concretely here.
// **What that does NOT establish**: this reader's own C++ has never been run
// against that file or any other real `.abr` -- this build's PLAN.md forbids
// compiling during this step, so agreement was checked in the derivation, not
// by executing the code below. `--selftest`'s coverage of this file is
// therefore entirely synthetic fixtures, same as every other section of this
// module, and the caveat `io/Descriptor.hpp`'s own header states applies here
// with the same force it always did.

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
  // Brushes whose `sampledData` named a bitmap tip this import could NOT
  // bring across (see this file's header for the three ways that happens).
  // **Not** every brush with a sampled tip -- the whole point of this step
  // was making most of those succeed. A brush counted here paints with the
  // round procedural tip; every other sampled-tip brush paints with its own
  // bitmap.
  size_t sampledTips = 0;
  size_t unmappedControls = 0;  // dynamics whose control has no source here
  // Brushes with Dual Brush switched ON, which this build has no second tip
  // to honour. Counted separately from `unmappedControls` because it is not a
  // control at all -- it is a whole second tip, and it changes the mark far
  // more than any single dynamics row does.
  size_t dualBrushes = 0;
};

// Parse a whole `.abr` file.
//
// Reads no byte outside `bytes`, for any content whatsoever -- the same
// contract io/Descriptor.hpp holds itself to, and for the same reason: this
// parses a file format from the internet.
AbrImportResult importAbrBrushes(std::span<const uint8_t> bytes);

// --- The `samp` block, exposed so it can be tested without a whole `.abr` --

// One decoded sample: the id its `samp` record carries (the 36-character
// UUID text, with the on-disk record's leading `$` stripped -- see this
// file's header) and the bitmap, or a null bitmap when the record's own
// header did not describe something this reader trusts (§ this file's header
// again, the three failure modes).
struct AbrSampledTip {
  std::string id;
  std::shared_ptr<const BrushTipBitmap> bitmap;
};

// Parses every brush sample out of one `samp` block's body (the bytes AFTER
// its own `8BIM samp <length>` framing -- `importAbrBrushes()` hands this the
// same subspan its top-level 8BIM walk already located). `subversion` is the
// `.abr`'s own subversion word, because it changes the fixed-size header this
// reader skips between a sample's UUID key and its bounds rectangle -- 47
// bytes for subversion 1, 301 for anything else, matching `abrupng`'s own
// `abr6.rs` (this file's header names it).
//
// **Never refuses outright.** A `samp` block is Photoshop's own internal
// format, not something a hostile actor hand-crafts the way a `.abr`'s
// user-visible `desc` names and numbers invite -- but it is still bytes from
// a file this build did not write, so one malformed record is skipped (its id
// omitted if even the 37-byte key does not fit) and the walk resumes at the
// next 4-byte-aligned boundary, rather than abandoning every sample the file
// describes for one bad one. Reads no byte outside `samp`, for the same
// reason and by the same discipline as `importAbrBrushes()` itself.
std::vector<AbrSampledTip> parseAbrSampledTips(std::span<const uint8_t> samp,
                                               uint16_t subversion);

// --- The mapping, exposed so it can be tested without a file ---------------

// Photoshop's brush dynamics "Control" dropdown, the `bVTy` integer.
//
// Named rather than left as a magic number because `StylusWheel` has no
// counterpart here and the import has to say so -- it is a device axis SDL
// does not report. `InitialDirection` and `Direction` each map onto their
// own `DynamicSource` (`abrControlToSource()`'s own comment on why these are
// two rows in the matrix rather than one, matching Photoshop's own two-entry
// control list).
//
// **6 is Direction and 7 is Initial Direction, and this pair was the other
// way round until it was checked against Photoshop itself.** The evidence for
// 6 is direct: Kyle Webster's "Blot Bot Perfecto" carries `angleDynamics`
// `bVTy = 6` in the file, and Photoshop's own Shape Dynamics panel shows that
// brush's Angle Control as **Direction**. Three further readings from the same
// brush pin the rest of the scale in place -- `szVr` 2 displays as "Pen
// Pressure", `roundnessDynamics` 0 as "Off", and its Size/Roundness jitter
// percentages land exactly where this importer computes them -- so the
// disagreement is specific to this pair rather than a general off-by-one.
//
// **7 is inferred, not observed.** Nothing in either sample library uses it:
// all twelve Runny Inkers and all three Spatter/Concept brushes that carry an
// angle control carry 6. It is placed here by elimination, since Photoshop
// offers exactly these two direction entries and one of them is now spoken
// for. Treat a file that actually uses 7 as the first real test of that
// guess, and re-check it against the panel before trusting it.
enum class AbrControl {
  Off = 0,
  Fade = 1,
  PenPressure = 2,
  PenTilt = 3,
  StylusWheel = 4,
  Rotation = 5,
  Direction = 6,
  InitialDirection = 7,
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
