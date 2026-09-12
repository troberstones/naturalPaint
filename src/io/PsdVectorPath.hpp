#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/Path.hpp"

// io/PsdVectorPath -- the geometry half of importing a Photoshop shape layer.
//
// A shape layer keeps its outline in a `vsms` ("vector shape mask") or `vmsk`
// ("vector mask") tagged block, NOT in its channel data: such a layer's raster
// is routinely 0x0, which is why Apple's `App Icon Template.psd` opens today
// with twelve of its thirteen layers empty. See docs/psd-vector-shapes.md.
//
// Two stages, deliberately separate because they fail differently. Decoding is
// byte work on untrusted input and either parses or does not; composing is a
// modelling decision -- PSD has four boolean operations and this codebase has
// no boolean path ops at all -- and one of the four cannot be honoured.
namespace np {

// The block payload after the 8-byte header: `version` then `flags`, both
// uint32. Every block seen so far is version 3, flags 0.
inline constexpr size_t kPsdPathBlockHeaderBytes = 8;

// Every path record is this wide, including the ones this module skips. The
// block length is padded to a multiple of 4, so the record count is the
// payload length DIVIDED by this with the remainder discarded -- a block of
// 192 bytes holds 7 records and 2 bytes of pad, not "192/26 does not divide
// evenly, therefore the block is corrupt".
inline constexpr size_t kPsdPathRecordBytes = 26;

// How a subpath combines with the ones before it. The value is an int16 at
// offset 4 of the *subpath length* record -- it is not in any descriptor, and
// grepping a 6.5 MB shape-heavy file for `pathOperation` finds nothing.
//
// The numbering is psd-tools', and only `Subtract` has been confirmed against
// a render here. `kPsdPathOpMergeWithPrevious` (-1) means "no operation of its
// own"; Photoshop writes it for the second and later subpaths of one drawn
// figure, such as a letter and its counter.
// The enumerators carry PSD's OWN numbering, so the values in this enum and
// the values in the file are the same numbers. They were sequential 0..4 when
// this header was first written, which made `MergeWithPrevious` 4 while the
// comment beside it said -1 -- true of the format and false of the code, in
// the one place a reader would check.
enum class PsdPathOp : int16_t {
  Exclude = 0,
  Union = 1,
  Subtract = 2,
  Intersect = 3,
  MergeWithPrevious = -1,
};

// One decoded subpath: the geometry, and what the file said to do with it.
struct PsdSubPath {
  SubPath sub;
  PsdPathOp op = PsdPathOp::Union;
  // False when the file's int16 matched none of the five values above. The
  // geometry is still good; only the combination rule is unknown, and the
  // caller decides whether that is fatal.
  bool opKnown = true;
  int16_t rawOp = 1;
};

struct PsdPathStream {
  std::vector<PsdSubPath> subpaths;

  // True when the block carried an OPEN subpath (record selector 3, with
  // knots 4/5). Open subpaths decode normally -- `SubPath::closed` is false --
  // but a shape layer's fill of an open path is a question this module does
  // not answer, so it is flagged rather than hidden.
  bool sawOpenSubPath = false;

  // Recoverable observations, each naming what was skipped and why. Records of
  // type 6 (path fill rule) and 8 (initial fill rule) are NOT warnings: both
  // are all-zero in every file examined, and neither carries the even-odd /
  // nonzero choice, which comes from the boolean ops instead.
  std::vector<std::string> warnings;
};

// Decodes a whole `vsms` / `vmsk` payload, INCLUDING its 8-byte header.
//
// Coordinates are signed 8.24 fixed point: each int32 divided by 2^24 is a
// fraction of a document dimension, VERTICAL BY HEIGHT and horizontal by
// width. Both halves of that sentence are measured, not assumed --
// `testNonSquareWithShapesOffPage.psd` is 768x512 and its `Ellipse 1` sits
// entirely above the canvas, so the divisor split and the sign are each
// exercised by a layer that decodes to the wrong place if either is wrong.
//
// A knot record's six int32 are ordered (in.y, in.x, pt.y, pt.x, out.y,
// out.x) -- VERTICAL FIRST, which is the field order most likely to be
// transposed by someone working from memory. Handles are absolute positions,
// which is what `Anchor` already stores.
//
// Reads no byte outside `block`, for any content of `block` whatsoever.
// Returns false with `error` set only for framing this module cannot step
// over; anything recoverable lands in `out.warnings`.
bool decodePsdPathRecords(std::span<const uint8_t> block, uint32_t docWidth, uint32_t docHeight,
                          PsdPathStream& out, std::string& error);

// The result of folding a stream's boolean operations into the one thing this
// codebase can draw: a single compound `Path` with a single fill rule.
struct PsdComposedPath {
  bool ok = false;
  Path path;
  // Non-empty exactly when !ok: why this layer cannot be expressed, naming the
  // operation. The caller turns it into a per-layer warning; it is never a
  // reason to refuse the whole file.
  std::string refusal;
  // Set when the result is sound but lossy in a way worth saying out loud.
  std::vector<std::string> warnings;
};

// Folds `stream` into one `Path`.
//
// naturalPaint has one fill rule per path and no boolean path operations
// (app/PathOps' eleven verbs are `Close`..`MakeCompound`; none is union or
// subtract). That is enough for three of PSD's four operations:
//
//   all Union            -> one compound path, NonZero
//   Union + Subtract     -> compound path, NonZero, subtracted subpaths
//                           REVERSED (the font and SVG hole convention)
//   all Exclude          -> compound path, EvenOdd -- exclude *is* XOR
//   any Intersect        -> nothing here expresses it: REFUSE by name
//
// A layer mixing Exclude with the others is refused for the same reason: the
// two rules cannot both apply to one path.
PsdComposedPath composePsdSubPaths(const PsdPathStream& stream);

}  // namespace np
