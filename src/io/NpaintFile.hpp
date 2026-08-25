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
//                   np:comps        "npcomps1:<hex>"  only when the document
//                                   has layer comps; io/CompSerial owns the
//                                   encoding, and it carries the part-name to
//                                   Layer::id join as well as the comps
//                   ...plus every unrecognised np:* attribute carried
//                      forward from the file this document was loaded from
//
//   part N  "L0001"...    R G B A, HALF, tiled 128x128            (np:kind RGB)
//                         R G B A pig.c0 pig.c1 pig.c2 pig.m
//                         res.R res.G res.B, HALF                (np:kind Pigment)
//                         mask, HALF                        (np:kind Adjustment)
//                         plus a trailing `mask` channel on RGB/Pigment parts
//                         when the layer has a mask
//                         data window == the bounding box of the layer's
//                         occupied tiles, tile-aligned
//           attrs:  np:kind     "RGB", "Pigment" or "Adjustment"
//                   np:name     the user-facing name (need not be unique)
//                   np:blend    "normal"
//                   np:opacity  1.0
//                   np:visible  1
//                   np:locked   0
//                   np:parent   ""
//                   np:ops      "npops1:<hex>"   only when the stack is
//                               non-empty; io/OpSerial owns the encoding
//                   np:mask     0/1              Adjustment parts only
//                   np:clipped  1                only when the layer is
//                               clipped by the alpha of the layer below
//
//   part N  "S0001"...    coverage, HALF, tiled 128x128       (an alpha channel)
//                         data window == the bounding box of the channel's
//                         occupied tiles, tile-aligned
//           attrs:  np:kind     "selection"
//                   np:name     the channel's name, unique in the document
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
//  * ~~**Pigment/residual latents as named channels**~~ -- **delivered at
//    PLAN.md Phase 5 step 3.** A Pigment layer's part carries all eleven
//    channels docs/document-format.md names, and the seven stored ones
//    (`pig.c0 pig.c1 pig.c2 pig.m`, `res.R res.G res.B`) are
//    `core::PigmentTile`'s own half words moved with no float stage, so the
//    zero-tolerance fidelity claim above covers them exactly as it covers
//    RGBA. R/G/B/A on such a part is the baked projection the spec asks for,
//    written for other tools and **ignored on read** for the same reason
//    part 0 is. Media layers are still refused: they need per-medium
//    simulation state `core::Layer` has no member for.
//
//  * ~~**`np:basis`**~~ -- **delivered at PLAN.md Phase 5 step 15** (PRD C8).
//    It was metadata while this build wrote no latents, then a constant with
//    a refusal behind it at step 3. `core::Document` now owns the value, the
//    writer stamps the *document's* basis rather than this build's, and the
//    reader puts the file's basis back onto the document -- so a file written
//    in a basis this build has never heard of round-trips its own label
//    instead of being relabelled by whoever opened it. A save is refused only
//    when the document's basis and the file's carried basis disagree *and*
//    the document holds Pigment layers, which is the one genuinely unwritable
//    case and is docs/document-format.md §3.3's own listed one. The whole
//    argument, and the three rejected alternatives, is at kNpaintPigmentBasis.
//
//  * ~~**A `mask` channel per part / layer masks**~~ -- **delivered at
//    PLAN.md Phase 5 step 4.** An RGB layer part is `R G B A mask` and a
//    Pigment one the eleven plus `mask`, written only when the layer has one.
//
//  * ~~**Adjustment layers**~~ -- **delivered at PLAN.md Phase 5 step 5**,
//    with one rule this format has nowhere else. An Adjustment layer holds no
//    pixels, and docs/document-format.md draws such a part as "(no image
//    channels)" -- which is **not writable**, measured 2026-08-20: a
//    zero-channel `ImageSpec` makes this OpenImageIO refuse the file at
//    `open()` with "Missing or empty channel list in header". So an Adjustment
//    part carries exactly one channel, `mask`, unconditionally, and an
//    `np:mask` int attribute says whether `Layer::mask` is actually engaged --
//    the job the channel's *presence* does on every other kind. `np:mask` is
//    written on Adjustment parts and nowhere else, so no other part's bytes
//    change.
//
//  * ~~**Clipping masks**~~ -- **delivered at PLAN.md Phase 5 step 9.** One
//    `np:clipped` int attribute, 0/1, the same shape `np:visible` and
//    `np:locked` already have and one of the three types
//    docs/document-format.md measured as surviving this OpenImageIO. Written
//    **only when true**, so a document with no clipped layer produces exactly
//    the bytes it produced before the attribute existed -- the property step 4
//    established for the `mask` channel and step 5 for `np:ops`, and asserted
//    the same way rather than assumed. A clipped **bottom** layer is carried
//    rather than refused: core/Composite composites it unclipped and warns by
//    name, because a refusal here would turn a preserved attribute into the
//    thing that makes a file unopenable.
//
//  * **A `strokes` part and its `np:dabs` blob.** `LayerKind::Strokes`
//    exists as an enum value and core/Layer.hpp calls it an "inert
//    placeholder"; there is no Dab type, no dab list and no stroke record
//    anywhere in `core/`. brush/StrokePath emits dabs into the *solver*, not
//    into a document. Unblocked by a Strokes layer that actually holds dabs.
//
//  * ~~**`np:ops` (per layer)**~~ -- **delivered at PLAN.md Phase 5 step 5.**
//    The blocker was the *carrier*, not the ownership: `np:ops` is a blob in
//    docs/document-format.md and this OpenImageIO drops array-typed header
//    attributes on write. Step 5 took the fix that document itself names --
//    a hex `string` attribute -- because an **Adjustment** layer's entire
//    content is its op stack, so the step could not repeat step 3's
//    warn-and-drop without losing a whole layer on every save (PRD I11).
//    io/OpSerial owns the encoding and its versioning; this module writes the
//    attribute for every kind that carries a non-empty stack, and only for a
//    non-empty one, so a document with no grades still produces exactly the
//    bytes it produced before the step. An `np:ops` this build cannot decode
//    -- a newer version tag, most likely -- is warned about by name and
//    carried verbatim rather than dropped (PRD I10).
//
//  * **`np:docOps` (document level)** is still deferred, and now for the
//    *other* half of the reason: the carrier exists, but `core::Document` has
//    no document-level op stack to put in one (`app::AppState::opStack` is the
//    global GPU-previewed grade and stays there). Unblocked by a `Document`
//    that owns a stack.
//
//  * ~~**`np:comps` (layer comps)**~~ -- **delivered at PLAN.md Phase 5 step
//    12** (PRD C14). The blocker was the same one `np:ops` had and had the same
//    fix: the format table calls it a `<blob>`, this OpenImageIO drops
//    array-typed header attributes on write, and this module *refuses* such a
//    save by name -- so a comp list written as a blob would have made every
//    save of a document that has comps fail. io/CompSerial encodes it as the
//    hex `string` docs/document-format.md itself prescribes, and **that table
//    is corrected as part of the step** rather than left contradicting its own
//    warning. Written on part 0 and **only when the document has comps**, so a
//    document with none produces exactly the bytes it produced before -- and
//    no layer part changes in either case, because the payload carries the
//    part-name-to-`Layer::id` join itself instead of putting an `np:id` on
//    every layer. An `np:comps` this build cannot decode -- a newer version
//    tag -- is warned about by name and carried verbatim (PRD I10).
//
//  * **`np:paths`.** The paths/vector work. No in-memory representation in
//    this codebase to write out.
//
//  * **`np:medium` / `np:simParams` on a Media part.** The simulation's
//    parameters live in `sim::PaintSim`/`app::AppState`, and no Media-kind
//    Layer is constructible with content today.
//
//  * ~~**Saved selections (`S0001`, `np:kind="selection"`)**~~ -- **delivered
//    at PLAN.md Phase 7** (PRD E11, E13). The blocker was ownership, not a
//    carrier: `core::Selection` existed but nothing in `core::Document` held
//    one. `Document::channels` does now -- a `std::vector<core::AlphaChannel>`,
//    each a named coverage store -- and a *saved selection is a channel*, which
//    is why one part kind serves both requirements. core/Channels.hpp argues
//    the identity, and argues at length why a saved selection is document data
//    while the **active** selection stays session state on
//    `app::OpenDocument` (putting the live marquee in `Document` would put it
//    in every `History` snapshot and make drawing one undoable).
//
//    Four properties make this a format decision rather than a serialisation
//    detail, and each is asserted rather than assumed:
//
//     - **One `S####` part per channel, with a single `coverage` channel in
//       HALF** -- exactly the sketch docs/document-format.md has carried since
//       before anything wrote one, down to the part name, the `np:kind` string
//       and the channel name. Nothing here invents a shape; the one thing the
//       sketch did not say is the sample type, and HALF is what every other
//       part of this file uses.
//     - **HALF is lossless for this store, measured rather than assumed.** A
//       `core::SelectionTile` is uint8, so the values on the wire are the 256
//       points of the k/255 grid. All 256 survive float -> HALF -> float ->
//       `*255 + 0.5` **exactly** (0 mismatches); the worst half-rounding across
//       the grid is 2.432e-4 at k=239, against a half-grid-step of 1/510 =
//       1.961e-3 -- 8.06x of margin, and 0.0 and 1.0 are exact identities. So
//       the zero-tolerance fidelity claim this header makes for HALF layer
//       pixels extends to channels, and `--selftest` asserts a channel round
//       trip at zero tolerance rather than within a tolerance. The rejected
//       alternative was FLOAT, which would be exact for free and doubles the
//       part; the unavailable one was UINT8, which OpenEXR has no pixel type
//       for.
//     - **A document with no channels writes no `S####` part at all**, so it
//       produces exactly the bytes it produced before this step -- the property
//       `np:ops`, `np:comps` and the `mask` channel each established in turn,
//       and asserted the same way, against a file rather than by argument.
//     - **`np:version` does not move.** An older reader meeting an `S0001` part
//       does not recognise it, carries it verbatim into `NpaintCarry::rawParts`
//       and writes it back unchanged -- which is docs/document-format.md §3.2
//       working exactly as designed, and is why this is an additive change and
//       not a structural one. Bumping the version would have told every older
//       build the file was beyond it, over a part they already handle safely.
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
//  * A layer whose `kind` is none of RGB, Pigment and Adjustment. Its pixels
//    (if it ever had any) have no representation here, per the deferrals
//    above, so writing the file would drop the layer. The error names the
//    layer's index, its name and its kind.
//  * A layer whose `kind` is RGB but whose `rgbTiles` is absent, or Pigment
//    but whose `pigmentTiles` is absent, or Adjustment but which carries
//    pixel tiles anyway -- each malformed against core/Layer.hpp's own
//    contract.
//  * A document with Pigment layers whose own `pigmentBasis` and whose
//    carried `np:basis` disagree (docs/document-format.md §3.3) -- one file
//    cannot honestly declare two bases for its latents.
//  * A document whose `pigmentBasis` is **empty**. An empty string attribute
//    does not survive this OpenImageIO (see NpaintAttribute), so the file
//    would come back declaring no basis at all rather than declaring an
//    unknown one.
//  * An alpha channel with an **empty name**, or two channels sharing one
//    name. `np:name` is how a channel is referred to at all
//    (`core::loadChannelAsSelection(doc, name)`), an empty `string` attribute
//    does not survive this OpenImageIO, and a duplicate makes that lookup's
//    answer depend on vector order -- so either would produce a file holding
//    coverage nothing can ask for. Named rather than uniquified on the way out,
//    because silently renaming a user's channel during a save is a change to
//    the document made by the writer.
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
// > carrier today. **Two of them have since been given one**: `np:ops` at
// > Phase 5 step 5 (io/OpSerial) and `np:comps` at step 12 (io/CompSerial),
// > both as the hex `string` this note prescribes below and both with the
// > version in the prefix. The remaining four are still uncarried.
// > `Type::Blob` is kept in this enum because it is what the
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
  // `Channel` is an alpha channel part (`S####`), added at PLAN.md Phase 7.
  // It is in this enum rather than being appended after the layers on every
  // save for the same reason `RawPart` is: a channel that sat between two
  // layer parts in the file it came from must go back where it was, or a
  // load/save cycle silently reorders the file. Nothing *reads* channel order
  // for meaning today -- layer order is the stack, channel order is only the
  // order a panel would list them in -- but "we did not need to preserve it"
  // is not a reason to destroy it.
  enum class Kind { Layer, RawPart, Channel };
  Kind kind = Kind::Layer;
  // Index into Document::layers for Layer, into NpaintCarry::rawParts for
  // RawPart, or into Document::channels for Channel.
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

  // The file's own `np:basis`, preserved verbatim. Empty for a document that
  // was never loaded from a file, in which case saveNpaint() stamps the
  // document's own `Document::pigmentBasis`.
  //
  // **This is not a duplicate of `Document::pigmentBasis`, and the difference
  // is what the refusal below is made of.** Since PLAN.md Phase 5 step 15 the
  // document carries its own basis -- what the latents *in this Document* are
  // -- and this field carries what the *file it came from* declared. A load
  // sets both, from the same string, so they agree; they can only disagree
  // afterwards, and the disagreement means exactly one thing: something
  // changed the document's basis away from the file's. That is the pair
  // `sourceVersion` below already forms with `kNpaintFormatVersion`, for the
  // same reason -- a "what the file said" beside a "what this is" is not
  // redundancy, it is the only way to notice a change.
  //
  // So: `saveNpaint()` refuses a document that holds Pigment layers when this
  // field is non-empty and names a basis other than the document's own,
  // because a latent is only meaningful in the basis it was fitted in and a
  // file cannot honestly carry two. A document *loaded* from a foreign-basis
  // file is no longer caught by that -- it agrees with itself -- and saves
  // back out under the file's own basis, which is the point of step 15 and is
  // argued at kNpaintPigmentBasis below. An RGB-only document carries a
  // foreign basis through a load/save untouched in either case; nothing in
  // such a file depends on it. docs/document-format.md §3.3 lists "a basis
  // mismatch" alongside the other refusals for this reason.
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

// `np:basis` for latents *this build* produced. docs/document-format.md's own
// example value.
//
// **Defined as core/Document's `kPigmentBasisMixbox`, not as a second spelling
// of the literal.** Until PLAN.md Phase 5 step 15 there was no Document-level
// basis to read from and this was a constant standing in for one; the document
// now owns the value, and the writer stamps `Document::pigmentBasis` rather
// than this. What this constant is still for is the *comparison*: "is the
// document's basis one this build can interpret?" There is exactly one such
// basis, so the question has a constant answer, and it is asked in three
// places -- the reader's warning, the mismatch refusal, and `--selftest`.
//
// --- The decision this step turned on: an uninterpretable basis ------------
//
// A file declaring `np:basis "km2-v1"` -- a basis this build has never heard
// of -- **loads**, keeps that string verbatim on `Document::pigmentBasis`,
// warns by name, and **saves back out still declaring `km2-v1`**. Three
// alternatives were considered and each is worse:
//
//  * **Refuse the load.** A basis is a label on latents that are otherwise
//    perfectly good `HALF` data. Refusing would make a document unopenable
//    over a string, and it is the exact opposite of what PRD I10's verbatim
//    carry-through exists to do: an older build must be able to open a newer
//    build's document, edit it, and destroy nothing.
//  * **Load it and relabel it `mixbox-v1` on save.** The worst of the three,
//    and the only one that loses data silently: the latents would be `km2-v1`
//    numbers under this build's name, and every later reader -- including this
//    one -- would project them through the wrong model and get plausible,
//    wrong colour. Nothing about the file would say so.
//  * **Load it and refuse to save it** (what this module did between step 3
//    and step 15, when the *only* basis a document could claim was this
//    build's constant, so a foreign carry always looked like a mismatch).
//    That is not a middle ground, it is a trap: the document opens, accepts
//    edits, and can never be written back -- and because app/Journal's crash
//    checkpoint *is* a `saveNpaint()`, a foreign-basis document could not be
//    checkpointed either. A refusal that costs the user their work to protect
//    a label has the trade backwards.
//
// What survives from that third option, narrowed to the case it was actually
// right about: a save is refused when the document's basis and the file's
// carried basis **disagree** and the document holds Pigment layers. That is
// the genuinely unwritable document -- one file, two bases, no honest label --
// and it is now the only case refused.
//
// **What this does not yet close, stated rather than left to be discovered.**
// Painting into a foreign-basis document deposits `mixbox-v1` latents beside
// `km2-v1` ones, and the file is then stamped `km2-v1` for all of them. The
// check belongs at the deposit, not at the save -- `core::Document`'s own
// header states the invariant a latent writer owes the field -- and closing it
// needs brush/Deposit to consult `Document::pigmentBasis` before its first
// texel, plus a decision about what the UI offers when it disagrees (convert,
// or refuse the stroke). Neither is this step's, and neither is reachable from
// a document this build can currently produce, because nothing in this build
// writes a basis other than its own.
inline constexpr const char* kNpaintPigmentBasis = kPigmentBasisMixbox;

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
//
// `np:basis` is stamped from `doc.pigmentBasis` -- the document's own claim
// about its latents (PRD C8) -- except that a non-empty `carry->basis` wins,
// because that is the string the file this document came from declared and
// PRD I10's carry-through is verbatim by definition. The two agree after any
// load; when they disagree and the document holds Pigment layers the save is
// refused rather than resolved. See kNpaintPigmentBasis.
NpaintSaveResult saveNpaint(const Document& doc, const std::string& path,
                            const NpaintSaveOptions& options = {},
                            const NpaintCarry* carry = nullptr);

// --- Load -----------------------------------------------------------------

struct NpaintLoadResult {
  bool ok = false;
  std::string error;

  // The reconstructed document: canvas size, working space and pigment basis
  // from part 0, one Layer per `L####` part whose channels are exactly R/G/B/A
  // in HALF.
  //
  // `document.pigmentBasis` is the file's `np:basis` verbatim, whatever it
  // says -- a file declaring a basis this build cannot interpret is loaded and
  // warned about, never refused and never relabelled. A file with no
  // `np:basis` at all makes no claim, so the field keeps its default (this
  // build's), which is the same thing the writer would have stamped.
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
// A part becomes a `core::Layer` only when it is named `L####` and either
// carries `np:kind = "Adjustment"` with exactly one HALF channel named
// `mask`, or carries `np:kind = "RGB"` with a channel list of *exactly*
// R, G, B, A in
// HALF, or carries `np:kind = "Pigment"` with exactly the eleven channels
// docs/document-format.md names, in HALF, matched **by name** -- measured, the
// written order does come back intact through this OpenImageIO, but that is
// its normalisation and not OpenEXR's storage (whose `ChannelList` is a
// name-sorted map putting `res.B` first), so a positional read would be luck.
//
// A part named `S####` carrying `np:kind = "selection"` and exactly one HALF
// channel named `coverage` becomes a `core::AlphaChannel` in
// `document.channels` instead, under the name its `np:name` gives (PRD E11,
// E13).
//
// Anything else -- a Media part, a group part with
// no channels, an RGB part a newer build gave a fifth
// channel -- is carried verbatim into `carry.rawParts` instead. That rule is
// deliberately strict in the reader's own disfavour: turning a part this
// build only half-understands into a Layer would drop the half it does not,
// which is precisely what PRD I10 forbids.
NpaintLoadResult loadNpaint(const std::string& path);

}  // namespace np
