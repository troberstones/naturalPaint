#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "brush/BrushModel.hpp"
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

// One decoded sample: the id its `samp` record carries (the 36-character
// UUID text, with the on-disk record's leading `$` stripped -- see this
// file's header) and the bitmap, or a null bitmap when the record's own
// header did not describe something this reader trusts (§ this file's header
// again, the three failure modes).
struct AbrSampledTip {
  std::string id;
  std::shared_ptr<const BrushTipBitmap> bitmap;
};

struct AbrImportResult {
  bool ok = false;
  std::string error;  // set when ok is false, and then presets is empty

  std::vector<BrushPreset> presets;

  // The same brushes as `presets`, read into Photoshop's own panel structure
  // (brush/BrushModel.hpp) rather than flattened onto the link matrix. One
  // entry per preset, in the same order.
  //
  // **Filled alongside `presets`, and read by nothing that paints -- yet.**
  // Keeping both while the engine moves over means the switchover is one
  // commit changing what CONSUMES this data, rather than one changing producer
  // and consumer in the same breath. Today its whole job is to let
  // `--abr-report` say what is actually in the file: Texture, Transfer,
  // Scatter Count, the Dual Brush's own cadence and the tool options were all
  // dropped without a note before it existed.
  std::vector<BrushModel> models;

  // The `samp` block's tips, flat and in file order, beside the presets that
  // reference them.
  //
  // **Exposed so the tips can be written somewhere durable.**
  // brush/Library.hpp is blunt about the alternative: a preset's `tipBitmap`
  // lives exactly as long as the library stays loaded, so a duplicated
  // sampled-tip brush "reloads next launch as the round procedural tip".
  // Writing them out is the app layer's job -- `io/` does not reach into
  // `app/` -- and app/DabLibrary's `extractAbrTips()` needs the tips to do it,
  // which until now only existed inside `importAbrBrushes()`'s own scope.
  //
  // Named `tipSamples` and not `sampledTips` because `sampledTips` further
  // down is already a COUNT of the ones that FAILED, and two members one
  // letter apart meaning opposite things is a defect waiting to be written.
  std::vector<AbrSampledTip> tipSamples;

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

  // Brushes with Dual Brush switched ON where this import could build NO
  // second tip at all -- either the `dualBrush` object carries no usable
  // `Brsh`, or its `BlnM` could not even be read as an enumerated value.
  // Counted separately from `unmappedControls` because it is not a control at
  // all -- it is a whole second tip, and it changes the mark far more than
  // any single dynamics row does.
  //
  // **Not** incremented for a `BlnM` that WAS read but names a blend mode
  // this build does not composite -- see `dualBrushUnsupportedBlend` below,
  // kept apart on purpose: a reader of `--abr-report` should be able to tell
  // "no second tip arrived at all" from "a second tip arrived and this build
  // still would not draw it as Photoshop does."
  size_t dualBrushes = 0;

  // Dual Brush switched ON, a second tip WAS built (Multiply, Overlay, Color
  // Burn or Hard Mix -- `brush/Deposit.hpp` §2d), but its `BlnM` named a blend
  // mode this build
  // does not implement compositing for. The brush paints with the primary
  // tip alone, exactly as `dualBrushes` above, but the diagnosis differs and
  // is worth telling apart: this is "we understood the request and refuse to
  // guess," not "nothing came across."
  size_t dualBrushUnsupportedBlend = 0;

  // Dual Brush switched ON with a blend mode this build DOES composite, but
  // the second tip's own spacing, scatter and count (`useScatter`, `Cnt `,
  // `bothAxes`, `countDynamics`, `scatterDynamics`) are not honoured --
  // `brush/Deposit.hpp` §2d stamps the second tip once, centred on every dab
  // of the first, rather than its own number of times with its own jitter.
  // **Not** counted for a brush whose Dual Brush asks for exactly that
  // (Count 1, scatter off): for that one configuration, stamping once,
  // centred, is not an approximation, it is the exact answer, and a note that
  // fired on it would carry no information -- the same discipline
  // `useDualBrush`'s own "present but off" case already applies below.
  size_t dualBrushCadenceNotHonoured = 0;

  // --- The Texture panel -----------------------------------------------
  //
  // Patterns found in this file's `patt` block, and those its framing
  // reached but would not decode (io/PsPatterns.hpp names the four ways).
  // A pack with no `patt` at all -- threeOtherBrushes.abr carries one of
  // length zero -- reports 0 and 0, which is a different fact from a pack
  // whose patterns failed.
  size_t patternsDecoded = 0;
  size_t patternsSkipped = 0;

  // Brushes whose Texture is ON and whose paper reached `BrushPreset::grain`,
  // against those where it did not -- an id this file's `patt` does not
  // contain, or a blend mode with no formula (`linearHeight`). A brush
  // counted in the second paints on no paper at all and says so, rather than
  // silently reading smoother than the original, which is what every one of
  // the 84 textured presets did before this existed.
  size_t texturesApplied = 0;
  size_t texturesNotApplied = 0;
};

// Parse a whole `.abr` file.
//
// Reads no byte outside `bytes`, for any content whatsoever -- the same
// contract io/Descriptor.hpp holds itself to, and for the same reason: this
// parses a file format from the internet.
AbrImportResult importAbrBrushes(std::span<const uint8_t> bytes);

// --- The `samp` block, exposed so it can be tested without a whole `.abr` --

// `AbrSampledTip` is declared ABOVE `AbrImportResult`, which carries a vector
// of them -- see there.

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

// The same class of bug as `abrSpacingToRadii()` above, for Scatter
// (docs/reachability-audit.md B5): Photoshop's Scatter jitter is a
// percentage of the tip's DIAMETER; `BrushTip::scatter` is in units of the
// RADIUS (brush/Dynamics.hpp: "in radii"). Getting this wrong scatters every
// imported brush at exactly half the distance the artist set -- plausible,
// in range, and invisible without comparing against the original.
//
// Takes the ALREADY-CLAMPED [0,1] fraction-of-diameter `addDynamicsLinks()`
// resolves a Scatter link's range to (that function's own shared angular-
// jitter math, correct as-is for Angle in degrees and Hue in turns, the two
// other Add targets it serves), not a raw percent -- so a jitter above 50%
// of the diameter is not silently capped at the point THIS conversion would
// otherwise introduce were the doubling applied before Photoshop's own
// 0-100% jitter clamp instead of after it.
float abrScatterFractionToRadii(float fractionOfDiameter) noexcept;

// --- The `8BIM` section table, exposed so tooling need not re-walk ---------

// One top-level `8BIM` section as the walk found it: its four-character key,
// and where its BODY begins and how long it is. Offsets are into the same span
// `readAbrSections()` was handed, so a caller subspans rather than re-walks.
struct AbrSection {
  std::string key;  // "samp", "patt", "desc", "phry", or whatever else is there
  size_t at = 0;      // offset of the body -- past the 12 bytes of framing
  size_t length = 0;
};

// Every `8BIM` section in one pass, plus the file's own two version words.
//
// Split out of `importAbrBrushes()` -- which now calls it rather than keeping
// its own copy -- for two reasons: `--abr-report` can print the section table
// without a second walk that could disagree with the importer's, and a reader
// of `patt` or `phry` does not have to reimplement the framing to find its
// block. The rules are unchanged from the walk this replaces: it stops at the
// first tag that is not `8BIM` (no resync -- a file whose framing has already
// gone wrong is not one to guess further into), a length running past the end
// of the buffer stops the walk rather than being clamped (a clamped block
// would parse as a shorter descriptor and silently import half a library),
// and sections are word-aligned to 2 bytes -- **not** `samp`'s own internal
// 4-byte record alignment, which is a different number in a different place
// and has been confused before.
//
// Note the two asymmetric tie-breaks `importAbrBrushes()` applies over this
// table, preserved from the walk it replaces: the FIRST `desc` wins and the
// LAST `samp` wins. Neither has ever been exercised by a real file -- no pack
// examined carries two of either -- so they are the previous code's behaviour
// kept bit-for-bit rather than a rule anything is known to depend on.
struct AbrSectionTable {
  bool ok = false;
  std::string error;  // set when ok is false, and then `sections` is empty
  uint16_t version = 0;
  uint16_t subversion = 0;
  std::vector<AbrSection> sections;
};

// Reads no byte outside `bytes`, the same contract everything else in this
// header holds itself to and for the same reason.
AbrSectionTable readAbrSections(std::span<const uint8_t> bytes);

}  // namespace np
