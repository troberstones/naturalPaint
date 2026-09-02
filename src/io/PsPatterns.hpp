#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace np {

// io/PsPatterns -- the `patt` block of a Photoshop `.abr`: the actual paper.
//
// **This block is 98-99% of every brush pack that has one** -- 36.0 MB of
// art_markers.abr's 36.3 MB, 33.6 of dry_media.abr's 34.0 -- and until this
// module existed it was stepped over without so much as a note. What is in it
// is not incidental: real scanned paper, named things like "Extra Heavy
// Canvas", "Wax Crayon on Vellum" and "ktw watercolor paper 2k17 b", 128x128
// to 900x900, which is exactly what `brush/Grain` has been approximating with
// a procedural height field. 84 of the 101 presets in the four packs measured
// switch Texture on and name one of these by UUID.
//
// **Unlike the `desc` block, this structure IS publicly documented** -- it is
// Adobe's own "Pattern" structure plus the Virtual Memory Array List, both in
// the published Photoshop File Formats Specification. That matters for more
// than confidence: `docs/brush-model-references.md` draws a hard line around
// reading GPL sources to reimplement, and here there is no need to go near
// one. Everything below is the specification's own field order, checked
// against the four real packs on this machine with `--abr-keys` and a
// throwaway walk.
//
// Three things the specification does not warn you about, all measured:
//
//   1. **`numberOfChannels` reads 24 in every record examined and is NOT a
//      channel count.** A greyscale pattern has one written channel and an RGB
//      one has three; looping `numberOfChannels` times walks off the end of
//      the record. The reliable bound is the Virtual Memory Array List's own
//      `length`, and that is what this reader uses. Trusting the field is the
//      single most likely way to get this parser wrong, which is why it has a
//      sabotage proof in `--selftest`.
//   2. **Every record ends with a four-byte short tail** after its last
//      written channel (measured: the walk reaches 22748 against a record
//      length of 22752). Tolerate it and stop; do not treat it as a truncated
//      channel.
//   3. **Records are four-byte aligned.** A walk that advances by `length`
//      alone drifts after the second record and starts decoding UTF-16 out of
//      the middle of pixel data -- which looks like a plausible pattern name,
//      not like a crash. That is how it presents when you get it wrong.
//
// Reads no byte outside the span it is given, on any input.

// One decoded pattern. `height8` is a scalar height field -- what paper tooth
// actually is -- so an RGB pattern collapses to luminance on the way in rather
// than carrying two channels nothing samples.
struct PsPattern {
  // The 36-character UUID from the record's Pascal-string id. **This is the
  // join key**: a brush's `Txtr` descriptor carries `Idnt` holding the same
  // text (verified: `63d61f21-...-bc81e4dfd608` appears in `desc` as a UTF-16BE
  // TEXT value and in `patt` as a 36-byte Pascal string), so the texture panel
  // resolves straight off it with nothing invented in between.
  std::string id;
  std::string name;  // the record's Unicode name, UTF-8 here
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> height8;  // width * height, row-major, top to bottom
};

struct PsPatternResult {
  std::vector<PsPattern> patterns;

  // Records the walk reached but would not decode -- an unsupported image
  // mode, a channel rectangle disagreeing with the pattern's own dimensions, a
  // dimension past the cap, a truncated channel. Counted rather than logged so
  // a caller can say "3 of 5 patterns arrived" instead of silently presenting
  // 3 as though it were all of them, which is the failure this whole module
  // exists to stop being.
  size_t skipped = 0;

  // True when the walk stopped early on framing it did not trust, as opposed
  // to running cleanly off the end of the block. A caller that cares about
  // completeness -- the import report does -- can tell "this file has three
  // patterns" from "this file had at least three and then stopped making
  // sense".
  bool truncated = false;
};

// Parses every pattern out of one `patt` block's BODY -- the bytes after its
// own `8BIM patt <length>` framing, which `readAbrSections()` already located.
//
// **Never refuses outright.** A zero-length `patt` is a real thing a real pack
// carries (threeOtherBrushes.abr has one) and means "no patterns", not "broken
// file". One malformed record stops the walk and keeps whatever decoded before
// it, the same discipline `parseAbrSampledTips()` already applies to `samp`.
PsPatternResult parseAbrPatterns(std::span<const uint8_t> patt);

// The largest pattern edge this reader will allocate for. 900x900 is the
// largest observed in any real pack; 4096 is the same cap
// `parseAbrSampledTips()` uses for a sampled tip, chosen for the same reason
// -- decoding an untrusted file must not become an unbounded allocation -- and
// kept identical so there is one number to remember rather than two.
inline constexpr int32_t kMaxPatternDimension = 4096;

}  // namespace np
