#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/LayerComp.hpp"

// io/CompSerial (PLAN.md "Phase 5 -- Stack it", step 12: layer comps,
// "persisted in the document as an `np:comps` blob on part 0"). PRD C14, I10,
// I11.
//
// ==========================================================================
// `np:comps` CANNOT BE A BLOB, and the format table saying it is, is wrong
// ==========================================================================
//
// docs/document-format.md §2's part-0 listing gives `np:comps <blob>`, and the
// same document's measured warning fifty lines below contradicts it outright:
//
//   > **Measured, 2026-08-19 ... `UINT8[n]` does not work.** Written through
//   > this project's OpenImageIO, a `TypeDesc(UINT8, 5)` header attribute is
//   > simply **absent** when the file is read back -- no error, no warning ...
//   > So **there is no working blob carrier today**, and every blob this
//   > document names -- `np:ops`, `np:dabs`, `np:comps`, `np:paths`,
//   > `np:docOps`, `np:simParams` -- needs one before it can be written.
//
// `io/NpaintFile` does not merely fail to write such an attribute: it
// **refuses the save by name** (PRD I11), so a comp list written as a blob
// would have made every save of a document with comps fail. The table's own
// prescribed fix is "a base64 or hex `string` attribute", which is what
// io/OpSerial took for `np:ops` at Phase 5 step 5 and what this module takes
// for `np:comps`. **The table is corrected as part of this step** rather than
// left contradicting its own warning; a format table that describes an
// unwritable attribute is worse than one with a gap in it, because a reader
// believes it.
//
// --- The carrier, and the three properties inherited from io/OpSerial ------
//
// The attribute's value is
//
//     "npcomps1:" <hex>
//
// two lowercase hex digits per byte, little-endian, floats as IEEE-754
// binary32 bit patterns. Hex rather than base64 for io/OpSerial.hpp's three
// stated reasons; the load-bearing one here is the first, that `--selftest`
// decodes a payload typed out by hand so the test does not share the encoder's
// assumptions.
//
// Three properties are copied deliberately, not incidentally:
//
//  1. **The version is the prefix, so it is read before anything is decoded.**
//     A build that meets `npcomps2:` says so by name and does not misread a
//     payload whose framing changed. Bumping the format means changing the
//     `1`.
//  2. **A future version is refused by name and carried verbatim** (PRD I10).
//     `deserializeLayerComps()` returns false with a sentence naming what it
//     saw; io/NpaintFile then leaves the attribute in `NpaintCarry::
//     documentAttributes` and writes it back unchanged, exactly as it already
//     does for an `np:ops` it cannot parse.
//  3. **An unrecognised comp *record* survives in position** rather than being
//     dropped, as `OpClass::Unknown` does for an op. It becomes a `LayerComp`
//     with `known == false` holding its bytes, sitting where it sat, and the
//     next save emits those bytes unchanged. `core::restoreLayerComp()`
//     refuses it by name rather than applying an empty comp that would look
//     like a comp which does nothing.
//
// --- The format ------------------------------------------------------------
//
//     u64  nextLayerId          Document::nextLayerId
//     u16  layerCount
//     layerCount x:
//       u16  partNameLength     bytes of the EXR part name that follow
//       ...  partName           "L0001"
//       u64  layerId            that layer's Layer::id
//     u16  compCount
//     compCount x:
//       u32  recordLength       bytes of `record` that follow
//       record:
//         u8   reserved         written 0; non-zero makes the record
//                               unrecognised (see below)
//         u16  nameLength, name
//         u16  entryCount
//         entryCount x:
//           u64  layerId
//           u8   visible        0 or 1
//           u8   clipped        0 or 1
//           f32  opacity
//           u16  blendLength, blend
//           u16  nameLength, nameAtCapture
//
// **Why the layer table is here and not an `np:id` attribute on each layer
// part.** A comp names layers by `core::Layer::id`, which is an in-memory
// identity with no meaning to any other tool; the *file's* stable layer id is
// the part name, which docs/document-format.md already fixes ("the part name
// is a stable synthetic id (`L0001`)") and which `NpaintCarry::layerPartNames`
// already round-trips. So the join between the two lives in one place, inside
// the one attribute the feature owns. Two consequences make that the right
// call rather than the compact one:
//
//   * **Every layer part's bytes are untouched, always.** A document with
//     comps writes exactly the layer parts it wrote without them, so the
//     regression boundary this step has to hold ("a document with no comps
//     saves byte-identically") is not a property of one conditional in the
//     writer -- there is no layer-part code path to get wrong at all.
//   * **It is legible.** `exrheader` on a `.npaint` shows one string in which
//     the part names and the comp names are readable, which is
//     docs/document-format.md §3.5's whole argument for this container.
//
// A part-name join rather than a positional one for the reason
// core/Layer.hpp gives about indices: a newer build may write a part between
// two layers that this build carries verbatim and does not turn into a
// `Layer`, at which point position N in the file and position N in
// `Document::layers` are different layers. A name cannot drift that way.
//
// --- What makes a record unrecognised -------------------------------------
//
// Deliberately strict in the reader's own disfavour, io/OpSerial's rule
// exactly:
//
//   * `reserved` is not 0 (a newer build used the byte for something);
//   * the record's length is not **exactly** what this build's parse of it
//     consumes -- so a newer build that adds a field to an entry produces a
//     record this build carries whole rather than half-reads.
//
// Half-reading would be guessing that the leading fields kept their meaning
// and would silently drop the new one on the next save. Carrying it whole
// loses nothing and claims nothing.
//
// --- What is deliberately not here ----------------------------------------
//
// **Export.** PLAN.md step 13 (PRD I16/I17) writes one file per comp through
// Export As presets. Nothing about this encoding is shaped around it and
// nothing about it makes it harder: a comp is a set of layer states, and an
// exporter restores each into a scratch document and hands it to io/Export.
//
// **Compression.** A comp of a twenty-layer document is a few hundred bytes.
namespace np {

// The version tag every value produced here begins with, including its colon.
// Exposed so io/NpaintFile and `--selftest` can name it rather than spelling
// the literal a second time -- io/OpSerial's `kOpStackSerialPrefix` precedent.
inline constexpr const char* kLayerCompSerialPrefix = "npcomps1:";

// Everything `np:comps` carries: the comps themselves plus the two pieces of
// identity state that would otherwise have nowhere to live -- the counter that
// makes `Layer::id` non-reusable, and the part-name-to-id join that survives a
// round trip.
struct LayerCompCarrier {
  uint64_t nextLayerId = 1;
  // (EXR part name, `Layer::id`) for every layer, in document order. A layer
  // with no part name -- which cannot happen on the write side, since
  // saveNpaint() names every part before it builds this -- is skipped by the
  // reader rather than matched positionally.
  std::vector<std::pair<std::string, uint64_t>> layerIds;
  std::vector<LayerComp> comps;

  friend bool operator==(const LayerCompCarrier&, const LayerCompCarrier&) = default;
};

// `in` as an `np:comps` attribute value: `kLayerCompSerialPrefix` followed by
// the payload in lowercase hex. Never fails and never returns an empty string
// -- an empty carrier serialises to a well-formed zero-count payload.
// io/NpaintFile still does not *write* the attribute when there are no comps,
// because a document with none must produce the bytes it produced before this
// step existed.
std::string serializeLayerComps(const LayerCompCarrier& in);

// The inverse. Returns false, leaving `*out` untouched, only when the value is
// malformed *as a container*: a prefix this build does not recognise, an odd or
// non-hex payload, a truncated record, or trailing bytes after the declared
// comp count. A comp *record* this build cannot interpret is **not** a failure
// -- it decodes to a `LayerComp` with `known == false` carrying its bytes, per
// this header's PRD I10 rule.
//
// `errorOut`, when non-null, receives a sentence naming what was wrong and
// where, in the io/Export refusal style every other refusal in this codebase
// follows.
bool deserializeLayerComps(std::string_view value, LayerCompCarrier* out,
                           std::string* errorOut = nullptr);

}  // namespace np
