#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/VectorShape.hpp"

// io/PathSerial -- a `LayerKind::Vector` layer's geometry on disk, as the
// per-layer `np:vector` attribute (PLAN.md phase 13; PRD I10, I11).
//
// ==========================================================================
// Why this is a hex string and not a blob
// ==========================================================================
//
// docs/document-format.md's part listing calls the vector payload a `<blob>`,
// and that is not writable. The same document's measured warning says so:
// written through this project's OpenImageIO, a `TypeDesc(UINT8, n)` header
// attribute is simply **absent** when the file is read back -- no error, no
// warning -- and the same holds for every array type tried, while `string`,
// `int` and `float` all survive.
//
// So the carrier is a `string`, and the encoding is the one io/OpSerial chose
// for `np:ops` and io/CompSerial for `np:comps`:
//
//     "npvec1:" <hex>
//
// two lowercase hex digits per byte, little-endian, floats as IEEE-754
// binary32 **bit patterns** rather than decimal renderings -- reopening a
// document must give back the coordinate that was saved, exactly rather than
// nearly, and an anchor that moved by one ulp on every save/load cycle would
// drift a shape over a session.
//
// ==========================================================================
// Forward compatibility: the version is the prefix
// ==========================================================================
//
// The version is read before a single byte is decoded, so a build meeting
// `npvec2:` says so by name and refuses rather than misreading a payload whose
// framing changed. io/NpaintFile then leaves the attribute in
// `NpaintCarry::layerAttributes` and writes it back verbatim, which is PRD
// I10 -- an older build cannot destroy a newer document's geometry.
//
// **This is deliberately coarser than io/CompSerial's per-record rule**, and
// the difference is worth stating because the two files otherwise look alike.
// `np:comps` carries records whose *kinds* a newer build might extend, so it
// can usefully understand some records and carry others. A shape list is
// homogeneous: any extension adds a field to every shape, which changes the
// framing and therefore the version. Per-record carry-forward would buy
// nothing and would put an `unrecognised` byte-bag on `core::VectorShape`,
// which is an I/O concern in a domain type.
//
// Records are still length-prefixed, because that localises a corruption to
// one shape and makes the exact-length rule checkable per record rather than
// only over the whole payload.
namespace np {

// The version tag every value produced here begins with, including its colon.
// Exposed so io/NpaintFile and `--selftest` can name it rather than spelling
// the literal a second time -- io/OpSerial's and io/CompSerial's precedent.
inline constexpr const char* kVectorShapeSerialPrefix = "npvec1:";

// `shapes` as an `np:vector` attribute value. Never fails and never returns an
// empty string -- an empty list serialises to a well-formed zero-count
// payload. io/NpaintFile still does not *write* the attribute for a layer with
// no shapes, so a document without vector content produces exactly the bytes
// it produced before this step existed.
std::string serializeVectorShapes(const std::vector<VectorShape>& shapes,
                                  uint64_t nextShapeId);

// The inverse. Returns false and leaves `*shapesOut` untouched when the value
// is malformed or carries a version this build does not know; `errorOut`, when
// non-null, receives a sentence naming what was wrong, in the io/Export
// refusal style.
//
// Every read is bounds-checked and every count is bounded by the bytes that
// actually remain, so a corrupt length cannot make this reserve gigabytes
// before failing. That matters more here than in io/CompSerial: a `.npaint`
// file can arrive from anywhere, and a shape list is the one part of it whose
// declared counts are large.
bool deserializeVectorShapes(std::string_view value, std::vector<VectorShape>* shapesOut,
                             uint64_t* nextShapeIdOut, std::string* errorOut = nullptr);

}  // namespace np
