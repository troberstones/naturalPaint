#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Document.hpp"
#include "io/ImageDecode.hpp"

// io/NpaintFile (PLAN.md "Phase 4 -- Write it out", step 4: "Native
// `.npaint` save and load -- multi-part tiled EXR via OIIO. No bespoke
// writer: one part per layer, `HALF` channels, latents as named channels,
// `np:*` typed attributes"). The spec being implemented is
// docs/document-format.md; PRD I4, I5b, I6, I7, I10, I11, I12.
//
// **No OpenImageIO header is included here, and none may be.** Same hard
// rule io/OiioBackend.hpp states and for the same reason: io/OiioBackend.cpp
// is the only translation unit in this project permitted to `#include
// <OpenImageIO/...>`, and src/CMakeLists.txt adds it to the target only when
// NP_USE_OIIO is ON. NpaintFile.cpp is compiled in *both* configurations and
// guards its own `#include "io/OiioBackend.hpp"` with `#if
// defined(NP_USE_OIIO)`, exactly as io/Export.cpp and io/Capabilities.cpp
// already do. Everything below is therefore declared unconditionally, and
// the entry points exist -- and refuse loudly, by name -- in a build with no
// OpenImageIO in it at all. See the NP_USE_OIIO=OFF section at the bottom.
//
// --- What this module is, and what it is not -----------------------------
//
// `.npaint` is not an export. io/Export encodes a *flattened, display-ready*
// image at a chosen target space and bit depth, and every stage of it is
// lossy by intent (a transfer function, a quantization, one layer's worth of
// output). This module is the opposite end: it persists the *document
// model*, losslessly, so that opening what was saved gives back the same
// document -- the same layers, the same tiles, bit for bit, and the same
// per-layer metadata. The two share exactly one thing, `io/Export`'s
// `flattenDocumentToLinear()`, which produces part 0's composite; see
// saveNpaint() below on why that reuse is required rather than convenient.
//
// --- File layout, as actually written ------------------------------------
//
//   part 0  "composite"   R G B A, HALF, tiled 128x128
//                         data window == display window == the canvas
//           attrs:  chromaticities  (PRD I6, from the document's WorkingSpace)
//                   np:version      1
//                   np:basis        "mixbox-v1"
//                   np:tileSize     128
//                   ...plus every unrecognised np:* attribute carried
//                      forward from the file this document was loaded from
//
//   part N  "L0001"...    R G B A, HALF, tiled 128x128
//                         data window == the bounding box of the layer's
//                         occupied tiles, tile-aligned
//           attrs:  np:kind     "RGB"
//                   np:name     the user-facing name (need not be unique)
//                   np:blend    "normal"
//                   np:opacity  1.0
//                   np:visible  1
//                   np:locked   0
//                   np:parent   ""
//
// Part names are the stable synthetic ids docs/document-format.md requires
// (`L0001`, one-based, in layer order): "layer names are not unique -- two
// layers may both be 'Layer 1' -- so the part name is a stable synthetic id
// and the user-facing name lives in `np:name`". EXR additionally *requires* a
// unique `name` on every part of a multi-part file, so this is not a style
// choice.
//
// Part order after part 0 is layer order, bottom to top, with carried
// unrecognised parts kept in their original positions -- see NpaintCarry.
//
// --- Why the HALF claim is asserted exactly, not within a tolerance -------
//
// docs/document-format.md's opening table justifies EXR with "Working space
// is `rgba16float` -> HALF channels -- byte-identical, no conversion". This
// module takes that literally: a layer part's pixels are the tile's own
// `uint16_t` half words, handed to OpenImageIO as TypeDesc::HALF and read
// back as TypeDesc::HALF. There is no float intermediate, no transfer
// function, no association/un-association (tiles are already premultiplied,
// which is also EXR's own convention -- see io/Export.hpp's Alpha section),
// and no clamp. Not one stage of that chain rounds, so --selftest asserts
// bit equality with zero tolerance. A tolerance there would let a real
// regression through, and it is the same claim, and the same reasoning,
// runFormatSupportTest() already applies to the EXR *export* round trip.
//
// --- Deliberate deferrals ------------------------------------------------
//
// docs/document-format.md describes more than this module writes. Each
// omission below is a deferral with a reason and an unblocking condition,
// not a silent gap -- and PRD I10's carry-through (NpaintCarry) is what makes
// each one *safe*: a future writer's `np:comps` blob, or a whole `pig.*`
// part, survives a load/save through today's build untouched.
//
//  * **Pigment/residual latents as named channels** (`pig.c0 pig.c1 pig.c2
//    pig.m`, `res.R res.G res.B`). Not written, because `core::Layer` has no
//    latent storage of any kind: `rgbTiles` is a
//    `std::optional<TileStore>` of 4-channel rgba16float tiles and
//    core/Layer.hpp says outright that Pigment/Media need "a *different*
//    shape -- 7 channels ... i.e. not a `core::TileStore<core::Tile>` at
//    all", which is Phase 5 step 3's work. Writing seven latent channels
//    today would mean synthesising numbers no part of this application
//    computed. Unblocked by Phase 5 step 3 giving Layer real latent tiles.
//
//  * **A `mask` channel per part / layer masks.** Same reason: Phase 5 step 4
//    ("Layer masks -- single-channel tile store"). There is no mask storage
//    to persist.
//
//  * **A `strokes` part and its `np:dabs` blob.** `LayerKind::Strokes`
//    exists as an enum value and core/Layer.hpp calls it an "inert
//    placeholder"; there is no Dab type, no dab list and no stroke record
//    anywhere in `core/`. brush/StrokePath emits dabs into the *solver*, not
//    into a document. Unblocked by a Strokes layer that actually holds dabs.
//
//  * **`np:ops` (per layer) and `np:docOps` (document level).** `core::OpStack`
//    is real, but it lives on `app::AppState`, not on `core::Layer` or
//    `core::Document` -- so there is no per-layer or per-document op stack to
//    serialise, and hanging one off Layer here purely to have something to
//    write would be inventing the very ownership decision Phase 5 step 3
//    ("Per-layer op stack applies *after* the latent->RGB projection") has to
//    make. It would also need a blob encoding for `core::Op`, which is a
//    format decision in its own right. Unblocked by Layer/Document owning an
//    OpStack.
//
//  * **`np:comps` (layer comps) and `np:paths`.** Phase 5 step 12 and the
//    paths/vector work respectively. Neither has any in-memory
//    representation in this codebase to write out.
//
//  * **`np:medium` / `np:simParams` on a Media part.** The simulation's
//    parameters live in `sim::PaintSim`/`app::AppState`, and no Media-kind
//    Layer is constructible with content today.
//
//  * **Saved selections (`S0001`, `np:kind="selection"`).**
//    `core::SelectionMask` exists but `core::Document` holds no selection.
//
//  * **The mip pyramid.** docs/document-format.md's §1 table pairs "128^2
//    tiles + a display mip pyramid" with EXR's mip-mapped storage, and §6
//    notes tile size should match `kTileSize` (this module writes 128 and
//    asserts it on read). The pyramid itself is *not* written: the only mip
//    pyramid in this codebase is `ui/NaturalPaintUI`'s viewport pyramid,
//    which is a display-side GPU resource rebuilt from tiles, not part of the
//    document model -- there is nothing on `Document` to persist. It becomes
//    worth writing when PLAN.md step 5 wires OIIO's `ImageCache` as the
//    residency layer, because that is when a coarse level is something the
//    reader would actually *fetch* rather than recompute.
//
// --- What is refused, and why refusing is the honest option ---------------
//
// PRD I11 (P0): "A save that would lose data names exactly what, rather than
// degrading silently." io/Export.cpp already established the house style for
// this -- name the thing, name the reason, name the alternative -- and every
// refusal below follows it:
//
//  * A layer whose `kind` is not RGB. Its pixels (if it ever had any) have no
//    representation here, per the deferrals above, so writing the file would
//    drop the layer. The error names the layer's index, its name and its
//    kind.
//  * A layer whose `kind` is RGB but whose `rgbTiles` is absent -- malformed
//    against core/Layer.hpp's own contract.
//  * An `opacity` outside [0,1].
//  * A lossy EXR compression, by name. See NpaintSaveOptions::compression.
//  * A canvas with a non-positive width or height.
//  * A `UINT8[n]` blob attribute, which this OpenImageIO drops on write --
//    see NpaintAttribute.
//  * A carried **scanline** part, which this OpenImageIO cannot write
//    alongside tiled ones -- see NpaintRawPart::tileWidth.
//
// --- NP_USE_OIIO=OFF ------------------------------------------------------
//
// Both entry points exist, both return false, and both produce an error
// naming `.npaint`, naming NP_USE_OIIO, and naming the cmake line that
// enables it. They are deliberately not compiled out: PLAN.md §1.5's "an
// unexercised build option is not a seam" applies here the same way it does
// to io/Capabilities, and --selftest's npaint section runs in both
// configurations with a single `kOiioBuild` constant carrying which build it
// is, asserting the correct answer for each.
namespace np {

// --- Attribute carriage (PRD I10) ----------------------------------------

// One typed EXR header attribute, restricted to the four types
// docs/document-format.md permits: "Use only OIIO-representable attribute
// types: `string`, `int`, `float`, and `UINT8[n]` for blobs. This avoids
// registering custom EXR attribute types, which OIIO would otherwise skip on
// read."
//
// An attribute of any *other* type read out of a file cannot be carried
// through this struct. That is reported rather than dropped silently -- see
// NpaintLoadResult::warnings.
//
// > **Measured correction to the spec, 2026-08-19.** Three of those four
// > types survive a write/read cycle through this OpenImageIO's OpenEXR
// > plugin. `UINT8[n]` does **not**: an attribute written as
// > `TypeDesc(UINT8, 5)` is simply absent from the header when the file is
// > read back (so are `INT32[n]` and every other array type tried). The
// > spec's claim that `UINT8[n]` is the blob type is therefore wrong for
// > this build, and every blob attribute it lists -- `np:ops`, `np:dabs`,
// > `np:comps`, `np:paths`, `np:docOps`, `np:simParams` -- has no working
// > carrier today. `Type::Blob` is kept in this enum because it is what the
// > spec asks for and because the fix belongs to whoever first needs a blob,
// > but **saveNpaint() refuses a Blob attribute by name rather than writing
// > a file that quietly lacks it** (PRD I11). When a blob is genuinely
// > needed, the cheap fix is a base64 or hex `string` attribute, which is
// > proven to survive; the expensive one is bypassing OpenImageIO for the
// > header.
// >
// > A second, smaller measured surprise: an **empty** string attribute is
// > also dropped. That is harmless for the two attributes this module can
// > legitimately write empty (`np:name` and `np:parent`), because the
// > reader's defaults for both are the empty string, so an absent attribute
// > and an empty one reconstruct identically -- but it is luck rather than
// > design, and it is why saveNpaint() warns when a *carried* attribute
// > (whose reader's defaults it cannot know) is an empty string.
struct NpaintAttribute {
  enum class Type { String, Int, Float, Blob };

  std::string name;
  Type type = Type::String;
  std::string stringValue;
  int32_t intValue = 0;
  float floatValue = 0.0f;
  std::vector<uint8_t> blobValue;

  friend bool operator==(const NpaintAttribute&, const NpaintAttribute&) = default;
};

// One EXR part in this module's own terms -- no OpenImageIO type anywhere in
// it, so it crosses the io/OiioBackend boundary in both directions.
//
// It has two jobs, and they are the same job seen from two sides. It is the
// transport io/OiioBackend reads parts into and writes parts out of; and it
// is how a part this reader could *not* turn into a `core::Layer` is kept,
// byte for byte, so the next save can put it back.
//
// This is the mechanism behind PRD I10 and docs/document-format.md §3.2
// ("The important rule. Any `np:*` attribute or part the reader does not
// recognise is retained and written back unchanged"), and it is what makes
// every deferral in this header's list *safe* rather than merely
// documented: a newer build's Pigment part, with its `pig.*` latent
// channels, survives being opened and re-saved by this build.
//
// Pixels are kept as raw bytes in the part's own sample type rather than
// converted to float, so a UINT8 or UINT32 part -- or a HALF part with
// values a float conversion would be exact for but a conversion *back* would
// not -- is genuinely unchanged rather than approximately unchanged.
struct NpaintRawPart {
  std::string name;
  // Data window, in the same coordinate frame as the display window.
  int32_t x = 0, y = 0;
  int32_t width = 0, height = 0;
  // 0 for a scanline part; otherwise the part's own on-disk tile size, which
  // is written back unchanged (it need not be kTileSize -- only the parts
  // *this* module writes are held to that).
  //
  // Measured constraint worth knowing before carrying a foreign part: this
  // OpenImageIO cannot write a multi-part EXR that mixes tiled and scanline
  // parts. It opens the file and then fails on the first mismatched part
  // with "Can't build a TiledOutputFile from a type-mismatched part". Since
  // every part this module writes is tiled (PRD I4 says *tiled* EXR), a
  // carried **scanline** part would make the next save fail -- so
  // saveNpaint() refuses it up front by name, and loadNpaint() warns as soon
  // as it reads one, rather than letting the failure surface at save time
  // wearing OpenEXR's wording.
  int32_t tileWidth = 0, tileHeight = 0;
  std::vector<std::string> channelNames;
  // OpenImageIO's own TypeDesc spelling ("half", "float", "uint8", ...),
  // round-tripped as a string so this struct needs no OpenImageIO type.
  std::string sampleTypeName;
  // width * height * channelNames.size() * sizeof(sampleType) bytes,
  // row-major top-to-bottom, exactly as read.
  std::vector<uint8_t> rawPixels;
  // The part's np:* attributes, carried the same way part 0's are.
  std::vector<NpaintAttribute> attributes;
};

// Where one part sits in the file's part order after part 0.
//
// docs/document-format.md: "**Part order is layer order**, bottom to top,
// after part 0." If a carried part were simply appended at the end of the
// next save, a newer build's Pigment layer that sat *between* two RGB layers
// would silently move to the top of the stack -- which is data loss of
// exactly the kind PRD I10 exists to prevent, just in the ordering rather
// than in the bytes. So the order is carried too.
struct NpaintPartSlot {
  enum class Kind { Layer, RawPart };
  Kind kind = Kind::Layer;
  // Index into Document::layers for Layer, or into NpaintCarry::rawParts for
  // RawPart.
  size_t index = 0;
};

// Everything a load kept that a `core::Document` has nowhere to hold.
//
// Deliberately NOT a member of `core::Document`. `core/` is the domain model
// and knows nothing about EXR, parts, or attribute types -- putting a bag of
// OpenEXR header attributes on it would make every consumer of Document
// depend on a file format's shape. So the carry travels alongside the
// document instead: loadNpaint() returns one, saveNpaint() optionally takes
// one, and when PLAN.md step 8's document lifecycle lands it is the open
// document's record that owns the pair. A save with no carry writes a file
// with nothing but what this build knows about, which is the correct
// behaviour for a document that was never loaded from a file.
struct NpaintCarry {
  // Unrecognised `np:*` attributes from part 0. Written back onto part 0.
  //
  // Scope, stated so it is not mistaken for a bug: only attributes whose
  // name begins with `np:` are carried. docs/document-format.md's rule is
  // written in exactly those terms ("Any `np:*` attribute or part"), and the
  // attributes that do *not* begin with `np:` in an EXR header are the
  // container's own -- `compression`, `chromaticities`, `screenWindowWidth`,
  // `PixelAspectRatio`, OpenImageIO's `oiio:*` bookkeeping -- which this
  // module and OpenImageIO regenerate on every write. Writing those back
  // verbatim would fight the writer rather than preserve the document.
  std::vector<NpaintAttribute> documentAttributes;

  // Unrecognised `np:*` attributes from each layer part, parallel to
  // `Document::layers`. Entry i belongs to layer i; entries beyond the
  // layer count are ignored on save.
  std::vector<std::vector<NpaintAttribute>> layerAttributes;

  // The EXR part name each layer came in under (`L0003`), parallel to
  // `Document::layers`. Reused on save rather than reallocated, so a layer's
  // synthetic id survives a round trip.
  //
  // That matters beyond tidiness: `np:parent` links name a *part*, and a
  // carried part written by a newer build may hold a reference to `L0003`
  // that this build cannot see inside. Renumbering layers on every save
  // would quietly repoint or break those references. A layer with no entry
  // here -- one created since the load, or a document that was never loaded
  // -- gets the first unused `L####`.
  std::vector<std::string> layerPartNames;

  // Parts that did not become layers, kept whole.
  std::vector<NpaintRawPart> rawParts;

  // The order of the parts after part 0. Empty for a document that was never
  // loaded, in which case saveNpaint() writes the layers in order and then
  // any rawParts.
  std::vector<NpaintPartSlot> partOrder;

  // The file's own `np:basis`, preserved verbatim. Empty means "this build's
  // own" (kNpaintPigmentBasis).
  //
  // Preserving rather than overwriting is the right call *today* precisely
  // because this build writes no latents: `np:basis` names the pigment basis
  // the latent channels are expressed in, and a file with no latent channels
  // has nothing whose meaning depends on it. When Phase 5 step 3 makes
  // latents real, a basis mismatch stops being metadata and becomes a
  // genuine "this save would lose data" case -- docs/document-format.md §3.3
  // lists "a basis mismatch" alongside the other refusals for that reason --
  // and this field is where that check will read from.
  std::string basis;

  // The file's own `np:version`, as read. Reported, not written back:
  // saveNpaint() always stamps kNpaintFormatVersion, because the file it
  // produces is one this build wrote. A newer version number here is
  // surfaced through NpaintLoadResult::warnings.
  int32_t sourceVersion = 0;
};

// --- Constants the format pins -------------------------------------------

// `np:version`. Bumped when a *structural* change makes an older reader
// wrong about the file -- not when an attribute is added, which is what the
// carry-through in NpaintCarry exists to make survivable without a bump.
inline constexpr int32_t kNpaintFormatVersion = 1;

// `np:basis`. docs/document-format.md's own example value. There is no
// Document-level basis field to read this from yet (Phase 5 step 15, "native
// save/load carrying layers and latents, with the pigment basis stamped"),
// and this build has exactly one pigment model, so it is a constant here
// rather than a guess dressed up as data.
inline constexpr const char* kNpaintPigmentBasis = "mixbox-v1";

// The conventional extension. `.exr` is the same container under a different
// name (PRD I8), and nothing in this module inspects the extension -- both
// the writer and the reader work off content and the OpenImageIO plugin
// selected by the path, so saving as `.exr` produces a byte-identical file.
inline constexpr const char* kNpaintExtension = ".npaint";

// --- Compression (PRD I7) -------------------------------------------------

// True when `name` selects one of OpenEXR's *lossy* compressors, which PRD
// I7 ("Native files use lossless compression only -- never DWAA/DWAB/B44")
// and docs/document-format.md §2 both forbid outright: "Never `DWAA`/`DWAB`/
// `B44` in a working file -- they are lossy, and a working file is the one
// place that is unacceptable."
//
// Takes a *name* rather than an enum on purpose. An enum containing only
// lossless values would make I7 true by construction and therefore
// untestable -- there would be no way to express the request that has to be
// refused. More practically, the EXR `compression` attribute genuinely is a
// string, including the `name:level` form (`dwaa:45`), so a name is what a
// future settings dialog, a preset file, or a document loaded from disk
// would actually be carrying.
//
// The level suffix is stripped before matching, and matching is
// case-insensitive, so `DWAA:45` is refused exactly as `dwaa` is.
//
// If `whyOut` is non-null it receives a sentence naming the compressor, what
// it does to the pixels, and which lossless compressor to use instead.
bool npaintCompressionIsLossy(std::string_view name, std::string* whyOut = nullptr);

// --- Save -----------------------------------------------------------------

struct NpaintSaveOptions {
  // An OpenEXR compressor name. `zip` is docs/document-format.md's stated
  // default ("`ZIP` for general use, `PIZ` for grainy content"); `piz`,
  // `zips`, `rle` and `none` are the other lossless choices. Anything
  // npaintCompressionIsLossy() recognises is refused by name before a byte
  // is written.
  std::string compression = "zip";
};

struct NpaintSaveResult {
  bool ok = false;
  // Non-empty exactly when !ok, and always names the specific thing that was
  // refused -- which layer, which value, which compressor -- per PRD I11.
  std::string error;
  // How many parts were written, part 0 included. Reported so a caller (and
  // --selftest) can check the structure without reopening the file.
  int32_t partsWritten = 0;
  // Non-fatal, but each one names something the written file does not hold
  // exactly as it was handed over. PRD I11 in its softer form: the save went
  // ahead, and the caller is told precisely what about it is approximate.
  std::vector<std::string> warnings;
};

// Writes `doc` to `path` as a multi-part tiled EXR.
//
// **Part 0 is regenerated unconditionally, on every save (PRD I12).** It is
// produced by io/Export's `flattenDocumentToLinear()` -- the same flattener
// the export path uses, called rather than reimplemented, so the composite
// every other EXR reader sees and the image io/Export writes can never
// disagree about what this document looks like. That function returns
// *straight* alpha (its documented contract), so this module re-associates
// before writing, EXR's alpha being associated by spec and by this
// codebase's own convention (io/Export.hpp's Alpha section). The layer parts
// do not go through it at all -- they are the tiles' own half words -- so the
// composite's float round trip cannot affect the fidelity claim above.
//
// `carry` may be null. When non-null, its unrecognised attributes and parts
// are written back verbatim, in their original order (PRD I10).
NpaintSaveResult saveNpaint(const Document& doc, const std::string& path,
                            const NpaintSaveOptions& options = {},
                            const NpaintCarry* carry = nullptr);

// --- Load -----------------------------------------------------------------

struct NpaintLoadResult {
  bool ok = false;
  std::string error;

  // The reconstructed document: canvas size and working space from part 0,
  // one Layer per `L####` part whose channels are exactly R/G/B/A in HALF.
  Document document;

  // Everything the Document has nowhere to hold. Feed this straight back to
  // saveNpaint() to satisfy PRD I10.
  NpaintCarry carry;

  // Part 0, decoded to io/ImageDecode's DecodedImage contract (straight
  // alpha, linear float RGBA).
  //
  // Read for inspection only -- a thumbnail, a PRD I13 "read the save back
  // and verify it" check, or --selftest proving PRD I12's regeneration --
  // and *never* used to reconstruct the document, which comes entirely from
  // the layer parts. Part 0 is a derived product; treating it as a source
  // would make a stale composite in someone else's file silently become this
  // application's truth.
  DecodedImage composite;

  // Non-fatal observations, each naming exactly what was seen. PRD I11's
  // "names exactly what" applied to the read direction: a newer np:version,
  // an on-disk tile size that is not kTileSize, an attribute whose EXR type
  // this module cannot carry, a part that could not be turned into a layer
  // and was carried verbatim instead.
  std::vector<std::string> warnings;
};

// Reads `path` back into a Document plus its carry.
//
// A part becomes a `core::Layer` only when it is named `L####`, carries
// `np:kind = "RGB"`, and its channel list is *exactly* R, G, B, A in HALF.
// Anything else -- a Pigment part with `pig.*` channels, a group part with no
// channels, a saved selection, an RGB part a newer build gave a fifth
// channel -- is carried verbatim into `carry.rawParts` instead. That rule is
// deliberately strict in the reader's own disfavour: turning a part this
// build only half-understands into a Layer would drop the half it does not,
// which is precisely what PRD I10 forbids.
NpaintLoadResult loadNpaint(const std::string& path);

}  // namespace np
