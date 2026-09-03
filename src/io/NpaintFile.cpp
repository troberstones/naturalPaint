#include "io/NpaintFile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>

#include "core/Channels.hpp"
#include "core/Composite.hpp"
#include "core/Half.hpp"
#include "core/Layer.hpp"
#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/CompSerial.hpp"
#include "io/Export.hpp"
#include "io/OpSerial.hpp"
#include "io/PathSerial.hpp"
#include "io/TextSerial.hpp"

// io/OiioBackend is the only translation unit that may include an
// OpenImageIO header, so this file reaches it the same way io/Export.cpp
// and io/Capabilities.cpp already do.
#include "io/OiioBackend.hpp"

namespace np {
namespace {

// --- Attribute names, in one place ---------------------------------------
//
// Spelled once so the writer and the reader cannot disagree about them, and
// so the "is this attribute recognised?" test below is the same list the
// writer emits rather than a second, hand-kept copy that can drift.
constexpr const char* kAttrVersion = "np:version";
constexpr const char* kAttrBasis = "np:basis";
constexpr const char* kAttrTileSize = "np:tileSize";
// The document's layer comps (PLAN.md Phase 5 step 12, PRD C14). A `string`,
// not the `<blob>` docs/document-format.md's table used to claim -- io/CompSerial
// owns the encoding and that header carries the correction. Written on part 0,
// where document-level attributes live, and **only when the document has
// comps**, so a document with none produces exactly the bytes it produced
// before this attribute existed.
constexpr const char* kAttrComps = "np:comps";

constexpr const char* kAttrKind = "np:kind";
constexpr const char* kAttrName = "np:name";
constexpr const char* kAttrBlend = "np:blend";
constexpr const char* kAttrOpacity = "np:opacity";
constexpr const char* kAttrVisible = "np:visible";
constexpr const char* kAttrLocked = "np:locked";
constexpr const char* kAttrParent = "np:parent";
// The per-layer op stack (PLAN.md Phase 5 step 5). A `string` -- the base64/
// hex carrier docs/document-format.md names as the cheap fix for its blob
// problem -- produced and consumed by io/OpSerial, which owns the encoding.
constexpr const char* kAttrOps = "np:ops";
// Adjustment parts only: whether `Layer::mask` is engaged. Every other kind
// answers that question with the *presence* of the `mask` channel, which an
// Adjustment part cannot do -- see buildAdjustmentLayerPart().
constexpr const char* kAttrMask = "np:mask";
// Whether the layer is clipped by the alpha of the layer below (PRD C9,
// PLAN.md Phase 5 step 9). An `int` 0/1, the type `np:visible` and `np:locked`
// already use and one of the three docs/document-format.md measured as
// surviving this OpenImageIO. Written **only when true** -- see the writer.
constexpr const char* kAttrClipped = "np:clipped";
// Whether the layer's alpha is frozen while its colour can still change
// ("alpha lock", core/Layer.hpp's `alphaLocked`). An `int` 0/1, `np:clipped`'s
// own type and rule: written **only when true** -- see the writer -- so a
// document with no alpha-locked layer produces exactly the bytes it produced
// before this attribute existed.
constexpr const char* kAttrAlphaLocked = "np:alphaLocked";
// **A Group layer's own stable identity** (PLAN.md Phase 5's C7/C12
// follow-on; core/Layer.hpp's `groupTag`). Written **only on a Group-kind
// part**, `np:mask`'s own rule and reason: meaningless on any other kind, so
// writing it everywhere would cost every non-grouped document a byte-identity
// regression for no reader. `np:parent` (below) is what a MEMBER carries --
// this is what the GROUP ITSELF carries, and the two are read back verbatim
// and joined by string equality, never translated through an EXR part name
// (see `Layer::parent`'s own comment for why a part name cannot be this
// join's key).
constexpr const char* kAttrGroupId = "np:groupId";

// PLAN.md phase 13. A Vector layer's whole content: its shapes, as
// io/PathSerial's `npvec1:<hex>` string. A *string*, not a blob, for the
// measured reason docs/document-format.md records and io/CompSerial restates
// -- array-typed EXR attributes silently vanish through this OpenImageIO.
//
// Written only when the layer has shapes, so a document whose Vector layer is
// empty produces the bytes it produced before this attribute existed --
// `np:ops`' and `np:label`'s rule.
constexpr const char* kAttrVector = "np:vector";
constexpr const char* kAttrText = "np:text";
// The colour label and the link group (PLAN.md Phase 5 step 11; PRD C15).
//
// **Scalars, and deliberately not a third string carrier.**
// docs/document-format.md's measured warning is about *array* attributes --
// `UINT8[n]` reads back silently absent through this OpenImageIO -- which is
// why `np:ops` and `np:comps` had to become hex `string`s. A label is one name
// and a link group is one number, so each fits a type already measured as
// surviving: `string`, as `np:name`, `np:blend` and `np:parent` do, and `int`,
// as `np:visible`, `np:locked` and `np:clipped` do. A per-layer carrier for two
// scalars would have been a third encoding to keep in step for no gain.
//
// Both are written **only when set**, which is `np:clipped`'s rule: a document
// with no labels and no links produces exactly the bytes it produced before
// this step, and `--selftest` asserts that against a file rather than assuming
// it. Absent therefore reads as each member's own default -- unlabelled, and
// unlinked.
//
// **`np:link` is an `int`, so a group number above `INT32_MAX` is refused by
// name** rather than written truncated (PRD I11, the rule an out-of-range
// opacity already follows). `core::nextLinkGroupId()` hands out one above the
// highest present, so reaching that would take 2^31 link gestures on one
// document -- but a `Layer` is a plain aggregate and a test can construct the
// state directly, so the guard is real and asserted rather than assumed
// unreachable.
constexpr const char* kAttrLabel = "np:label";
constexpr const char* kAttrLink = "np:link";

constexpr const char* kCompositePartName = "composite";

// docs/document-format.md's channel names for a Pigment part, in its own
// order: the baked RGBA projection any other tool renders, then the four
// `pig.*` channels, then the three `res.*` ones. Seven latent channels, which
// is core/Pigment's `PigmentTile::kChannels`, plus the four derived ones.
//
// The **reader matches by name, never by position.**
//
// > **Measured, 2026-08-19, while implementing this: the order does survive a
// > round trip through this OpenImageIO, and it survives for a reason nobody
// > should rely on.** A part written with the eleven names above reads back as
// > exactly `R G B A pig.c0 pig.c1 pig.c2 pig.m res.R res.G res.B`. That is
// > *not* what OpenEXR stores -- `Imf::ChannelList` is a name-sorted map, in
// > which `res.B` precedes `res.G` precedes `res.R` -- so what comes back is
// > OpenImageIO's own normalisation, which appears to apply its RGBA-ordering
// > heuristic per EXR layer name (`res.R/G/B` gets sorted like an RGB triple,
// > `pig.c0..m` alphabetically, which happens to match).
//
// So a positional read would work today, by luck, on this version of this
// library, and would silently swap the residual's red and blue the moment any
// of that changed -- with no crash and no warning, only wrong colour. Matching
// by name costs one `std::find` per channel, once per part.
constexpr const char* kPigmentChannelNames[] = {"R",       "G",       "B",     "A",
                                                "pig.c0",  "pig.c1",  "pig.c2", "pig.m",
                                                "res.R",   "res.G",   "res.B"};
constexpr size_t kPigmentChannelCount = sizeof(kPigmentChannelNames) / sizeof(char*);
static_assert(kPigmentChannelCount == 11,
              "a Pigment part is R G B A plus core/Pigment's seven stored channels");
// Where each of PigmentTile's seven stored channels sits in the list above.
constexpr size_t kPigmentLatentFirst = 4;

// --- The `mask` channel (PLAN.md Phase 5 step 4) --------------------------
//
// docs/document-format.md's layer part already listed it, one line under
// `res.R res.G res.B`, and this step gives it data. Three decisions live in
// this constant and are worth having in one place:
//
//  1. **It is written only when the layer actually has a mask.** A layer with
//     `Layer::mask == nullopt` produces exactly the part this module produced
//     before masks existed -- same channel list, same data window, same
//     `sampleTypeName`, same bytes. That is not an optimisation, it is the
//     property that makes this step's format change safe to ship: measured, an
//     RGB-only document written by HEAD's binary and by this one differ only
//     inside OpenImageIO's `capDate` header string, which HEAD's own two
//     consecutive runs differ inside as well.
//  2. **It goes last, and the reader matches it by name** like every other
//     channel here -- `R G B A mask` for an RGB layer, the eleven plus `mask`
//     for a Pigment one. Appending keeps the existing prefix identical, and
//     matching by name is `kPigmentChannelNames`' own argument (OpenEXR stores
//     a name-sorted channel map; position is OpenImageIO's normalisation and
//     not a contract).
//  3. **`HALF`, like every other channel of the part.** `NpaintRawPart` carries
//     one `sampleTypeName` for the whole part, so a mask stored as `uint8` in
//     memory would be quantised on every load and the "HALF in, HALF out, no
//     conversion" claim would stop holding for one channel of a layer part.
//     core/Mask.hpp derives the quality half of the same decision.
//
// The **drop rule for a mask tile is "every sample is exactly 1.0"**, the
// mirror of the "every word is zero" rule the content unpackers apply, because
// 1.0 is what an unallocated mask tile means (core/Mask.hpp). Same rule, same
// reason -- a tile indistinguishable from an unallocated one must not be
// allocated, or a document's resident cost would grow on every save-and-reopen
// -- with the identity element the channel actually has.
constexpr const char* kMaskChannelName = "mask";

// --- An alpha channel's part (PLAN.md Phase 7; PRD E11, E13) ---------------
//
// docs/document-format.md has drawn this part since before anything wrote one:
//
//     part 4   "S0001"          coverage                   <- a saved selection
//              attrs:  np:kind        "selection"
//
// and all three strings below are taken from it verbatim rather than chosen
// here. Two notes on that:
//
//  * **`"selection"` is lower-case where every other `np:kind` is
//    capitalised** ("RGB", "Pigment", "Adjustment"). That is the published
//    sketch's own spelling, and matching a document that predates the code is
//    worth more than matching a naming habit -- anyone who implemented a reader
//    against that table would have matched the lower-case string. There is no
//    `LayerKind` for it to collide with: a channel is not a layer, and
//    `layerKindName()` never produces this value.
//  * **A channel part is not a layer part**, so it deliberately does not reuse
//    `layerPartName()`'s `L` prefix. The reader's first test is the name, and
//    keeping the two namespaces disjoint means an `S####` part can never be
//    mistaken for a layer whose channels happen to be unreadable.
//
// The sample type is **HALF**, which the sketch did not specify, for the reason
// the `mask` channel gives one line up: `NpaintRawPart` carries one
// `sampleTypeName` per part, and every part this module writes is HALF. That is
// only acceptable because it is *lossless here*, which is measured rather than
// assumed -- io/NpaintFile.hpp carries the numbers (all 256 points of the
// uint8 k/255 grid survive exactly; worst half-rounding 2.432e-4 against a
// half-grid-step of 1.961e-3, 8.06x of margin). OpenEXR has no 8-bit pixel
// type, so storing the tile's own bytes was never on the table; FLOAT would
// have been exact for free and twice the size, for a channel whose values live
// on a 256-point grid.
//
// **The drop rule on read is "every sample is exactly zero"** -- the same rule
// the RGB unpacker uses, and the *opposite* of the `mask` channel's "every
// sample is exactly 1.0" one directly above. Both are "the identity element the
// channel actually has", and for selection coverage that element is 0.0: an
// absent tile is unselected (core/SelectionMask.hpp, and core/Channels.hpp on
// why a channel inherits the selection default and not the layer-mask one).
// Getting this backwards would make a channel painted on one tile of a
// four-tile document come back selecting the other three.
constexpr const char* kChannelKindName = "selection";
constexpr const char* kCoverageChannelName = "coverage";

// Where each of `names` sits in `part.channelNames`, or an empty optional when
// any of them is missing.
//
// This is the by-name lookup `kPigmentChannelNames`' comment insists on,
// generalised so the three layouts that need one -- `R G B A mask`, the eleven,
// and the eleven plus `mask` -- share an implementation rather than growing
// three. The bare four-channel `R G B A` layout deliberately keeps its own
// positional check below, because its unpacker is a memcpy per tile row that
// depends on exactly that order and byte-identity with what this module wrote
// before masks existed is measured rather than argued.
std::optional<std::vector<size_t>> channelIndicesByName(const NpaintRawPart& part,
                                                        const std::vector<std::string>& names) {
  std::vector<size_t> idx;
  idx.reserve(names.size());
  for (const std::string& want : names) {
    const auto it = std::find(part.channelNames.begin(), part.channelNames.end(), want);
    if (it == part.channelNames.end()) return std::nullopt;
    idx.push_back(static_cast<size_t>(it - part.channelNames.begin()));
  }
  return idx;
}

std::vector<std::string> rgbaChannelNames(bool withMask) {
  std::vector<std::string> names{"R", "G", "B", "A"};
  if (withMask) names.emplace_back(kMaskChannelName);
  return names;
}

std::vector<std::string> pigmentChannelNames(bool withMask) {
  std::vector<std::string> names(kPigmentChannelNames,
                                 kPigmentChannelNames + kPigmentChannelCount);
  if (withMask) names.emplace_back(kMaskChannelName);
  return names;
}

bool isDocumentAttributeRecognised(const std::string& name) {
  return name == kAttrVersion || name == kAttrBasis || name == kAttrTileSize ||
         name == kAttrComps;
}

bool isLayerAttributeRecognised(const std::string& name) {
  return name == kAttrKind || name == kAttrName || name == kAttrBlend ||
         name == kAttrOpacity || name == kAttrVisible || name == kAttrLocked ||
         name == kAttrParent || name == kAttrOps || name == kAttrMask ||
         name == kAttrClipped || name == kAttrLabel || name == kAttrLink ||
         name == kAttrGroupId || name == kAttrAlphaLocked || name == kAttrVector ||
         name == kAttrText;
}

NpaintAttribute stringAttr(const char* name, std::string value) {
  NpaintAttribute a;
  a.name = name;
  a.type = NpaintAttribute::Type::String;
  a.stringValue = std::move(value);
  return a;
}
NpaintAttribute intAttr(const char* name, int32_t value) {
  NpaintAttribute a;
  a.name = name;
  a.type = NpaintAttribute::Type::Int;
  a.intValue = value;
  return a;
}
NpaintAttribute floatAttr(const char* name, float value) {
  NpaintAttribute a;
  a.name = name;
  a.type = NpaintAttribute::Type::Float;
  a.floatValue = value;
  return a;
}

const NpaintAttribute* findAttr(const std::vector<NpaintAttribute>& attrs, const char* name) {
  for (const NpaintAttribute& a : attrs) {
    if (a.name == name) return &a;
  }
  return nullptr;
}

// --- Part naming ---------------------------------------------------------
//
// docs/document-format.md: part names "must be unique, and EXR requires a
// `name` on every part in a multi-part file. Layer names are not unique --
// two layers may both be 'Layer 1' -- so the part name is a stable synthetic
// id (`L0001`) and the user-facing name lives in `np:name`."
std::string layerPartName(size_t oneBasedIndex) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "L%04zu", oneBasedIndex);
  return buf;
}

// True for exactly the `L` + four-or-more digits shape layerPartName()
// produces. Used by the reader to decide whether a part is even a candidate
// for becoming a Layer -- the far stricter channel/type/attribute test comes
// after.
bool isLayerPartName(const std::string& name) {
  if (name.size() < 2 || name[0] != 'L') return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

// The channel analogue, one-based and in `Document::channels` order.
// docs/document-format.md's own `S0001`.
std::string channelPartName(size_t oneBasedIndex) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "S%04zu", oneBasedIndex);
  return buf;
}

bool isChannelPartName(const std::string& name) {
  if (name.size() < 2 || name[0] != 'S') return false;
  for (size_t i = 1; i < name.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

std::string lowerNoLevel(std::string_view name) {
  // OpenEXR compressor names carry an optional `:level` suffix (`dwaa:45`,
  // `zip:9`). The level never changes whether the compressor is lossy, so it
  // is stripped before matching -- otherwise `dwaa:45` would slip past a
  // refusal that catches bare `dwaa`.
  const size_t colon = name.find(':');
  std::string s(colon == std::string_view::npos ? name : name.substr(0, colon));
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Every OpenEXR compressor that is lossless for every sample type a
// `.npaint` part can hold. `pxr24` is deliberately absent -- see
// npaintCompressionIsLossy().
constexpr const char* kLosslessCompressors[] = {"none", "rle", "zips", "zip", "piz"};

bool isLosslessCompressor(const std::string& lowered) {
  for (const char* c : kLosslessCompressors) {
    if (lowered == c) return true;
  }
  return false;
}

std::string losslessCompressorList() {
  std::string s;
  for (const char* c : kLosslessCompressors) {
    if (!s.empty()) s += ", ";
    s += c;
  }
  return s;
}

// --- Half-word helpers ----------------------------------------------------

// docs/document-format.md §6: "HALF maxes out around 65504. Fine for
// scene-linear values, but a saturating operation could produce `inf`, which
// EXR stores happily and which then poisons any downstream average. Clamp on
// write."
//
// Applied to the *composite* only. The layer parts are the tiles' own half
// words moved verbatim -- clamping those would be an edit, and would break
// the bit-exactness this format's whole fidelity claim rests on. The
// composite is a regenerated, derived product summed in float, which is
// exactly where a value can newly exceed half's range, so it is the right
// and only place for this.
constexpr float kHalfMax = 65504.0f;
uint16_t compositeFloatToHalf(float v) {
  if (std::isnan(v)) return 0;
  if (v > kHalfMax) v = kHalfMax;
  if (v < -kHalfMax) v = -kHalfMax;
  return floatToHalf(v);
}

// --- Layer part geometry --------------------------------------------------
//
// "Layers allocate only where content exists" maps onto EXR's per-part data
// window (docs/document-format.md §1's table). The data window is the
// tile-aligned bounding box of the layer's occupied tiles, so an unpainted
// region costs nothing on disk either -- and, because it is tile-aligned,
// reading it back is a straight tile-by-tile copy with no resampling and no
// partial tiles.
struct TileBounds {
  bool any = false;
  int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;  // inclusive, in tile coords
};

template <class StoreT>
TileBounds occupiedTileBounds(const StoreT& tiles) {
  TileBounds b;
  for (const auto& [coord, tile] : tiles) {
    (void)tile;
    if (!b.any) {
      b.any = true;
      b.minX = b.maxX = coord.x;
      b.minY = b.maxY = coord.y;
      continue;
    }
    b.minX = std::min(b.minX, coord.x);
    b.maxX = std::max(b.maxX, coord.x);
    b.minY = std::min(b.minY, coord.y);
    b.maxY = std::max(b.maxY, coord.y);
  }
  return b;
}

// The tile-aligned bounding box that covers **both** stores. A mask may hold
// tiles where the layer holds no content (a mask painted over a region that was
// later erased is the obvious way to get one), and those tiles are the user's
// data: dropping them because the *content* bounds do not reach them would be
// silent loss. Where the content is absent inside the widened window the part
// simply carries an all-zero content tile, which the reader drops again.
TileBounds unionTileBounds(TileBounds a, const TileBounds& b) {
  if (!b.any) return a;
  if (!a.any) return b;
  a.minX = std::min(a.minX, b.minX);
  a.maxX = std::max(a.maxX, b.maxX);
  a.minY = std::min(a.minY, b.minY);
  a.maxY = std::max(a.maxY, b.maxY);
  return a;
}

// Writes the part's `mask` channel: `MaskTile::kRevealWord` everywhere the
// layer has no mask tile, and the tile's own raw half words where it does.
//
// The reveal fill is what makes an *absent* mask tile round-trip as an absent
// one: a rectangular EXR data window has no way to encode a hole, so the hole
// is spelled with the value the hole means, and the reader drops any tile that
// comes back saying only that.
void writeMaskChannel(const MaskTileStore& mask, int32_t tileX0, int32_t tileY0, int32_t width,
                      int32_t height, size_t channels, size_t maskChannel, uint16_t* words) {
  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
  for (size_t i = 0; i < texels; ++i) words[i * channels + maskChannel] = MaskTile::kRevealWord;
  const size_t rowWords = static_cast<size_t>(width) * channels;
  for (const auto& [coord, tile] : mask) {
    const size_t col0 = static_cast<size_t>(coord.x - tileX0) * kTileSize;
    const size_t row0 = static_cast<size_t>(coord.y - tileY0) * kTileSize;
    const uint16_t* src = tile.data();
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      uint16_t* row = words + (row0 + static_cast<size_t>(ty)) * rowWords;
      for (int32_t tx = 0; tx < kTileSize; ++tx) {
        row[(col0 + static_cast<size_t>(tx)) * channels + maskChannel] =
            src[static_cast<size_t>(ty) * kTileSize + static_cast<size_t>(tx)];
      }
    }
  }
}

// Packs a layer's tiles into one part's pixel buffer.
//
// **This is the function the "byte-identical, no conversion" claim lives
// or dies by**: it moves `uint16_t` half words out of core::Tile's own
// storage into the byte buffer OpenImageIO writes as TypeDesc::HALF. No
// float appears anywhere in it. A tile-aligned data window means each tile
// row is a contiguous run of 128*4 half words landing at a contiguous
// offset, so this is a memcpy per tile row.
//
// **Phase 5 step 4 added the `mask` channel, and left this function's
// mask-free path untouched on purpose.** When `layer.mask` is absent the part
// is four channels and the copy is still a memcpy per tile row, byte for byte
// what it was; when a mask is present the RGBA words are no longer contiguous
// within a row (the stride is five) so the copy becomes four words per texel,
// and the mask channel is filled separately. Two loops rather than one general
// one, because the general one would have cost the mask-free case its memcpy
// and this module's byte-identity claim for mask-free documents is measured
// against HEAD rather than argued.
NpaintRawPart buildLayerPart(const Layer& layer, const std::string& partName) {
  const MaskTileStore* mask = layer.mask.has_value() ? &*layer.mask : nullptr;

  NpaintRawPart part;
  part.name = partName;
  part.channelNames = rgbaChannelNames(mask != nullptr);
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;

  const TileStore& tiles = *layer.rgbTiles;
  const TileBounds b =
      mask ? unionTileBounds(occupiedTileBounds(tiles), occupiedTileBounds(*mask))
           : occupiedTileBounds(tiles);
  // A layer with no painted tiles still needs a non-empty data window --
  // EXR has no representation for a zero-area part. One all-zero tile at the
  // origin is the minimal legal answer, and it round-trips back to zero
  // tiles: the reader drops all-zero tiles, which carry no information under
  // core::Tile's own "an unwritten texel is transparent black" contract.
  const int32_t tileX0 = b.any ? b.minX : 0;
  const int32_t tileY0 = b.any ? b.minY : 0;
  const int32_t tilesW = b.any ? (b.maxX - b.minX + 1) : 1;
  const int32_t tilesH = b.any ? (b.maxY - b.minY + 1) : 1;

  part.x = tileX0 * kTileSize;
  part.y = tileY0 * kTileSize;
  part.width = tilesW * kTileSize;
  part.height = tilesH * kTileSize;

  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  part.rawPixels.assign(rowWords * static_cast<size_t>(part.height) * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());

  if (mask == nullptr) {
    for (const auto& [coord, tile] : tiles) {
      const size_t colWord = static_cast<size_t>(coord.x - tileX0) * kTileSize * 4;
      const size_t row0 = static_cast<size_t>(coord.y - tileY0) * kTileSize;
      for (int32_t ty = 0; ty < kTileSize; ++ty) {
        std::memcpy(words + (row0 + static_cast<size_t>(ty)) * rowWords + colWord,
                    tile.data() + static_cast<size_t>(ty) * kTileSize * 4,
                    static_cast<size_t>(kTileSize) * 4 * sizeof(uint16_t));
      }
    }
    return part;
  }

  for (const auto& [coord, tile] : tiles) {
    const size_t col0 = static_cast<size_t>(coord.x - tileX0) * kTileSize;
    const size_t row0 = static_cast<size_t>(coord.y - tileY0) * kTileSize;
    const uint16_t* src = tile.data();
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      uint16_t* row = words + (row0 + static_cast<size_t>(ty)) * rowWords;
      for (int32_t tx = 0; tx < kTileSize; ++tx) {
        std::memcpy(row + (col0 + static_cast<size_t>(tx)) * channels,
                    src + (static_cast<size_t>(ty) * kTileSize + static_cast<size_t>(tx)) * 4,
                    4 * sizeof(uint16_t));
      }
    }
  }
  writeMaskChannel(*mask, tileX0, tileY0, part.width, part.height, channels, 4, words);
  return part;
}

// Packs a Pigment layer's tiles into one 11-channel part.
//
// **The fidelity claim splits in two here, and the split is the point.** The
// seven `pig.*`/`res.*` channels are `core::PigmentTile`'s own `uint16_t` half
// words, moved without a float ever appearing -- the same "HALF in, HALF out,
// no conversion" property `buildLayerPart()` above has for RGBA, so
// `--selftest` asserts a Pigment layer's latents round-trip with **zero**
// tolerance. The four R/G/B/A channels are a *derived* baked projection
// (docs/document-format.md's "R G B A <- baked projection"), computed in float
// and clamped on write like part 0 is, and the reader **ignores them
// completely** for exactly the reason it ignores part 0: a derived product
// treated as a source makes someone else's stale bake into this
// application's truth.
//
// The bake is the projection of the stored latents and deliberately does
// **not** include the layer's op stack, even though part 0's composite does.
// The op stack is not persisted (see the header's deferral list), so baking a
// grade into a layer part would put a look in the file that the reloaded
// document could not reproduce or undo.
NpaintRawPart buildPigmentLayerPart(const Layer& layer, const std::string& partName) {
  const MaskTileStore* mask = layer.mask.has_value() ? &*layer.mask : nullptr;

  NpaintRawPart part;
  part.name = partName;
  part.channelNames = pigmentChannelNames(mask != nullptr);
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;

  const PigmentTileStore& tiles = *layer.pigmentTiles;
  const TileBounds b =
      mask ? unionTileBounds(occupiedTileBounds(tiles), occupiedTileBounds(*mask))
           : occupiedTileBounds(tiles);
  const int32_t tileX0 = b.any ? b.minX : 0;
  const int32_t tileY0 = b.any ? b.minY : 0;
  const int32_t tilesW = b.any ? (b.maxX - b.minX + 1) : 1;
  const int32_t tilesH = b.any ? (b.maxY - b.minY + 1) : 1;

  part.x = tileX0 * kTileSize;
  part.y = tileY0 * kTileSize;
  part.width = tilesW * kTileSize;
  part.height = tilesH * kTileSize;

  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  part.rawPixels.assign(rowWords * static_cast<size_t>(part.height) * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());

  for (const auto& [coord, tile] : tiles) {
    const size_t col0 = static_cast<size_t>(coord.x - tileX0) * kTileSize;
    const size_t row0 = static_cast<size_t>(coord.y - tileY0) * kTileSize;
    const uint16_t* src = tile.data();
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      uint16_t* row = words + (row0 + static_cast<size_t>(ty)) * rowWords;
      for (int32_t tx = 0; tx < kTileSize; ++tx) {
        uint16_t* out = row + (col0 + static_cast<size_t>(tx)) * channels;
        const uint16_t* stored =
            src + (static_cast<size_t>(ty) * kTileSize + static_cast<size_t>(tx)) *
                      PigmentTile::kChannels;
        // The seven stored channels: raw half words, in PigmentTile's own
        // order, which is the format's own order.
        for (size_t c = 0; c < static_cast<size_t>(PigmentTile::kChannels); ++c)
          out[kPigmentLatentFirst + c] = stored[c];
        // The four derived ones.
        const std::array<float, 4> rgba =
            projectPigmentTexel(tile.readTexel(PixelCoord{tx, ty}));
        for (int c = 0; c < 4; ++c) out[c] = compositeFloatToHalf(rgba[c]);
      }
    }
  }
  // The baked `R G B A` above is deliberately **unmasked**, for the same
  // reason it is ungraded: it is a projection of what the layer stores, and
  // the mask is stored beside it in its own channel. Another tool reading the
  // four RGBA channels of a layer part gets the layer; part 0 is where it gets
  // the composite, and part 0 *is* masked because it comes from the flattener.
  if (mask) writeMaskChannel(*mask, tileX0, tileY0, part.width, part.height, channels,
                             kPigmentChannelCount, words);
  return part;
}

// An **Adjustment** layer's part (PLAN.md Phase 5 step 5, PRD C5).
//
// This kind holds no pixels at all -- its entire content is `Layer::ops`,
// which travels in the `np:ops` string attribute -- so docs/document-format.md
// draws its analogue (a `strokes` part) with "(no image channels)". **That is
// not writable, measured 2026-08-20**: an `ImageSpec` with zero channels makes
// this OpenImageIO's OpenEXR plugin refuse the whole file at `open()` with
// "Missing or empty channel list in header", before a byte is written. So an
// Adjustment part carries **exactly one** channel, and `mask` is the only one
// that means anything on a layer with no colour of its own.
//
// **The consequence, stated because it is a rule this format has nowhere
// else**: for an RGB or Pigment part, the *presence* of the `mask` channel is
// what says whether `Layer::mask` is engaged (core/Mask.hpp separates absent
// from all-1.0, and step 4 made "add a mask, save, reopen" keep the mask). An
// Adjustment part's `mask` channel is always present, so it cannot carry that
// distinction, and an explicit `np:mask` int attribute does instead. It is
// written on Adjustment parts and nowhere else -- making it universal would
// have changed the bytes of every mask-free file this build already writes,
// and that byte-identity is a measured property step 4 established.
//
// The unmasked case fills the channel with `kRevealWord`, which is what the
// reader would read for an absent tile anyway, so the two agree whatever
// `np:mask` says.
NpaintRawPart buildAdjustmentLayerPart(const Layer& layer, const std::string& partName) {
  const MaskTileStore* mask = layer.mask.has_value() ? &*layer.mask : nullptr;

  NpaintRawPart part;
  part.name = partName;
  part.channelNames = {kMaskChannelName};
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;

  const TileBounds b = mask ? occupiedTileBounds(*mask) : TileBounds{};
  const int32_t tileX0 = b.any ? b.minX : 0;
  const int32_t tileY0 = b.any ? b.minY : 0;
  const int32_t tilesW = b.any ? (b.maxX - b.minX + 1) : 1;
  const int32_t tilesH = b.any ? (b.maxY - b.minY + 1) : 1;

  part.x = tileX0 * kTileSize;
  part.y = tileY0 * kTileSize;
  part.width = tilesW * kTileSize;
  part.height = tilesH * kTileSize;

  const size_t rowWords = static_cast<size_t>(part.width);
  part.rawPixels.assign(rowWords * static_cast<size_t>(part.height) * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());
  if (mask) {
    writeMaskChannel(*mask, tileX0, tileY0, part.width, part.height, 1, 0, words);
  } else {
    for (size_t i = 0; i < rowWords * static_cast<size_t>(part.height); ++i)
      words[i] = MaskTile::kRevealWord;
  }
  return part;
}

// A **Group** layer's part (PLAN.md Phase 5's C7/C12 follow-on; PRD C7).
//
// docs/document-format.md's own sketch for a group predates this build's
// implementation and says "no image channels" -- and that sketch is wrong for
// the identical, already-measured reason `buildAdjustmentLayerPart()`'s own
// comment gives: an `ImageSpec` with zero channels makes this OpenImageIO's
// OpenEXR plugin refuse the whole file at `open()` with "Missing or empty
// channel list in header", before a byte is written. Rather than re-measure
// what Adjustment already proved, this function reuses its exact answer: one
// `mask` channel, unconditionally, real content when the layer actually has
// a mask and `MaskTile::kRevealWord` filling it when not -- and, exactly as
// for Adjustment, `np:mask` (not the channel's presence) is what says which.
//
// A Group has no `ops` for `np:mask`'s sibling `np:ops` to carry (see
// `makeGroupLayer()`), so nothing else about this part differs from
// Adjustment's shape at all; it is a thin wrapper rather than a shared helper
// because the two are one call each and a shared helper would need a kind
// parameter purely to pick the return value's `part.channelNames` (identical
// either way) -- not worth the indirection for the one line that differs
// between them, `np:kind`, which the caller writes.
NpaintRawPart buildGroupLayerPart(const Layer& layer, const std::string& partName) {
  return buildAdjustmentLayerPart(layer, partName);
}

// A Vector part, PLAN.md phase 13. Identical shape to Adjustment's and Group's
// and for the identical measured reason: a zero-channel `ImageSpec` makes this
// OpenImageIO's OpenEXR plugin refuse the whole FILE at open() with "Missing
// or empty channel list in header", so a layer that holds no pixels still
// carries the one `mask` channel EXR requires. Its content lives in the
// `np:vector` attribute, not in a channel.
NpaintRawPart buildVectorLayerPart(const Layer& layer, const std::string& partName) {
  return buildAdjustmentLayerPart(layer, partName);
}

// A Text layer's part. Adjustment's shape for the third time, and for the same
// two reasons: a Text layer holds no pixels (its content is `np:text`), and a
// zero-channel `ImageSpec` makes this OpenEXR plugin refuse the WHOLE FILE at
// `open()` -- so the one `mask` channel is not optional even though nothing
// reads it.
NpaintRawPart buildTextLayerPart(const Layer& layer, const std::string& partName) {
  return buildAdjustmentLayerPart(layer, partName);
}

// An alpha channel's part (PRD E11, E13): one `coverage` channel over the
// tile-aligned bounding box of the channel's occupied tiles.
//
// The same "allocate only where content exists" rule the layer parts follow,
// with the one difference that makes it *cheaper* here than for a mask: a
// rectangular data window cannot encode a hole, so the hole has to be spelled
// with the value it means -- and for coverage that value is **zero**, which the
// zero-fill from `assign()` has already written. `writeMaskChannel()` needs an
// explicit reveal pass because 1.0 is not what a zeroed buffer holds; this
// needs nothing.
//
// A texel is `floatToHalf(coverageAt(...))`, i.e. `floatToHalf(v * (1/255))`.
// There is no clamp on the way out -- `SelectionTile` cannot hold a value
// outside [0,1] by construction, which is `writeCoverage()`'s own guarantee, so
// a clamp here would be dead code claiming a hazard that does not exist. It is
// on the way *in* that a foreign sample has to be handled, and it is.
NpaintRawPart buildChannelPart(const AlphaChannel& channel, const std::string& partName) {
  NpaintRawPart part;
  part.name = partName;
  part.channelNames = {kCoverageChannelName};
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;

  const TileBounds b = occupiedTileBounds(channel.tiles);
  // An empty channel still needs a non-empty data window -- EXR has no
  // representation for a zero-area part, the same wall `buildLayerPart()` hits.
  // One all-zero tile at the origin is the minimal legal answer and it
  // round-trips back to zero tiles, because the reader drops all-zero tiles
  // exactly as it drops all-zero pixel tiles. So "a channel the user saved and
  // then erased" survives as a named, empty channel rather than disappearing,
  // which is the honest answer: the channel is document data, its emptiness is
  // not.
  const int32_t tileX0 = b.any ? b.minX : 0;
  const int32_t tileY0 = b.any ? b.minY : 0;
  const int32_t tilesW = b.any ? (b.maxX - b.minX + 1) : 1;
  const int32_t tilesH = b.any ? (b.maxY - b.minY + 1) : 1;

  part.x = tileX0 * kTileSize;
  part.y = tileY0 * kTileSize;
  part.width = tilesW * kTileSize;
  part.height = tilesH * kTileSize;

  const size_t rowWords = static_cast<size_t>(part.width);
  part.rawPixels.assign(rowWords * static_cast<size_t>(part.height) * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());

  for (const auto& [coord, tile] : channel.tiles) {
    const size_t col0 = static_cast<size_t>(coord.x - tileX0) * kTileSize;
    const size_t row0 = static_cast<size_t>(coord.y - tileY0) * kTileSize;
    for (int32_t ty = 0; ty < kTileSize; ++ty) {
      uint16_t* row = words + (row0 + static_cast<size_t>(ty)) * rowWords;
      for (int32_t tx = 0; tx < kTileSize; ++tx)
        row[col0 + static_cast<size_t>(tx)] = floatToHalf(tile.coverageAt(PixelCoord{tx, ty}));
    }
  }
  return part;
}

// The inverse. Returns how many samples were outside [0,1] or NaN and had to be
// clamped, so the caller can name a count in a warning (PRD I11) rather than
// absorbing a malformed channel in silence -- the rule the `mask` channel
// already follows, for the same reason: a bad sample in a coverage channel does
// not show up as a wrong pixel, it shows up as an edit that lands somewhere
// unexpected.
//
// **NaN clamps to 0.0, not to 1.0.** Both are defensible readings of "this
// sample means nothing"; 0.0 is chosen because it is the value an absent tile
// already has, so a NaN-filled tile drops out entirely rather than becoming a
// tile that selects everything. Selecting nothing on bad data refuses edits;
// selecting everything applies them. The first is recoverable.
size_t unpackChannelPart(const NpaintRawPart& part, size_t coverageIdx,
                         SelectionTileStore* tiles) {
  const int32_t tileX0 = part.x / kTileSize;
  const int32_t tileY0 = part.y / kTileSize;
  const int32_t tilesW = part.width / kTileSize;
  const int32_t tilesH = part.height / kTileSize;
  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  const auto* words = reinterpret_cast<const uint16_t*>(part.rawPixels.data());

  size_t clamped = 0;
  SelectionTile scratch;
  for (int32_t ty = 0; ty < tilesH; ++ty) {
    for (int32_t tx = 0; tx < tilesW; ++tx) {
      const size_t col0 = static_cast<size_t>(tx) * kTileSize;
      const size_t row0 = static_cast<size_t>(ty) * kTileSize;
      for (int32_t r = 0; r < kTileSize; ++r) {
        const uint16_t* row = words + (row0 + static_cast<size_t>(r)) * rowWords;
        for (int32_t x = 0; x < kTileSize; ++x) {
          float v = halfToFloat(row[(col0 + static_cast<size_t>(x)) * channels + coverageIdx]);
          if (!(v >= 0.0f && v <= 1.0f)) {  // false for NaN, for < 0 and for > 1
            v = (v > 1.0f) ? 1.0f : 0.0f;   // NaN and < 0 both land on 0.0
            ++clamped;
          }
          // Back onto the uint8 grid through the store's own quantiser, so a
          // value that came from this build's own writer lands on exactly the
          // byte it left as -- measured over all 256 grid points, and the
          // reason this round trip is asserted at zero tolerance rather than
          // within one (io/NpaintFile.hpp carries the numbers).
          scratch.writeCoverage(PixelCoord{x, r}, v);
        }
      }
      // An all-zero tile is dropped rather than allocated, for the reason the
      // RGB unpacker drops an all-transparent one and the mask unpacker drops
      // an all-reveal one: under core/Channels.hpp's convention it is
      // indistinguishable from an absent tile, so allocating it would grow a
      // document's resident cost on every save-and-reopen and change nothing a
      // reader can observe.
      if (scratch.selectsNothing()) continue;
      tiles->getOrCreate(TileCoord{tileX0 + tx, tileY0 + ty}) = scratch;
    }
  }
  return clamped;
}

// The exact inverse. `part` has already been checked to be R/G/B/A HALF with
// a tile-aligned data window.
void unpackLayerPart(const NpaintRawPart& part, TileStore* tiles) {
  const int32_t tileX0 = part.x / kTileSize;
  const int32_t tileY0 = part.y / kTileSize;
  const int32_t tilesW = part.width / kTileSize;
  const int32_t tilesH = part.height / kTileSize;
  const size_t rowWords = static_cast<size_t>(part.width) * 4;
  const auto* words = reinterpret_cast<const uint16_t*>(part.rawPixels.data());
  constexpr size_t kTileWords = static_cast<size_t>(kTileSize) * kTileSize * 4;

  std::vector<uint16_t> scratch(kTileWords);
  for (int32_t ty = 0; ty < tilesH; ++ty) {
    for (int32_t tx = 0; tx < tilesW; ++tx) {
      const size_t colWord = static_cast<size_t>(tx) * kTileSize * 4;
      const size_t row0 = static_cast<size_t>(ty) * kTileSize;
      for (int32_t r = 0; r < kTileSize; ++r) {
        std::memcpy(scratch.data() + static_cast<size_t>(r) * kTileSize * 4,
                    words + (row0 + static_cast<size_t>(r)) * rowWords + colWord,
                    static_cast<size_t>(kTileSize) * 4 * sizeof(uint16_t));
      }
      // An all-zero tile is dropped rather than allocated. This is not a
      // shortcut, it is the only behaviour consistent with core/TileStore's
      // contract: a tile is allocated on write, an unwritten texel reads
      // transparent black, and a tile whose every texel is transparent black
      // is therefore indistinguishable from one that was never allocated.
      // Keeping it would make occupiedTileCount() -- and therefore the
      // document's resident cost -- grow every time a sparse document was
      // saved and reopened, because a rectangular EXR data window has no way
      // to encode a hole.
      bool anyNonZero = false;
      for (uint16_t w : scratch) {
        if (w != 0) {
          anyNonZero = true;
          break;
        }
      }
      if (!anyNonZero) continue;
      Tile& tile = tiles->getOrCreate(TileCoord{tileX0 + tx, tileY0 + ty});
      std::memcpy(tile.data(), scratch.data(), kTileWords * sizeof(uint16_t));
    }
  }
}

// The same inverse for a part whose channels are **not** exactly four -- an
// `R G B A mask` part, where the four content channels are strided rather than
// contiguous. Split from unpackLayerPart() above rather than folded into it so
// that the mask-free path keeps its memcpy per tile row and its byte-for-byte
// equivalence with what this module wrote before masks existed.
//
// `idx` is where R, G, B and A sit, by name.
void unpackLayerPartStrided(const NpaintRawPart& part, const std::vector<size_t>& idx,
                            TileStore* tiles) {
  const int32_t tileX0 = part.x / kTileSize;
  const int32_t tileY0 = part.y / kTileSize;
  const int32_t tilesW = part.width / kTileSize;
  const int32_t tilesH = part.height / kTileSize;
  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  const auto* words = reinterpret_cast<const uint16_t*>(part.rawPixels.data());
  constexpr size_t kTileWords = static_cast<size_t>(kTileSize) * kTileSize * 4;

  std::vector<uint16_t> scratch(kTileWords);
  for (int32_t ty = 0; ty < tilesH; ++ty) {
    for (int32_t tx = 0; tx < tilesW; ++tx) {
      const size_t col0 = static_cast<size_t>(tx) * kTileSize;
      const size_t row0 = static_cast<size_t>(ty) * kTileSize;
      for (int32_t r = 0; r < kTileSize; ++r) {
        const uint16_t* row = words + (row0 + static_cast<size_t>(r)) * rowWords;
        for (int32_t x = 0; x < kTileSize; ++x) {
          const uint16_t* in = row + (col0 + static_cast<size_t>(x)) * channels;
          uint16_t* out = scratch.data() +
                          (static_cast<size_t>(r) * kTileSize + static_cast<size_t>(x)) * 4;
          for (size_t c = 0; c < 4; ++c) out[c] = in[idx[c]];
        }
      }
      bool anyNonZero = false;
      for (uint16_t w : scratch) {
        if (w != 0) {
          anyNonZero = true;
          break;
        }
      }
      if (!anyNonZero) continue;  // same drop rule, same reason as above
      Tile& tile = tiles->getOrCreate(TileCoord{tileX0 + tx, tileY0 + ty});
      std::memcpy(tile.data(), scratch.data(), kTileWords * sizeof(uint16_t));
    }
  }
}

// Reads a part's `mask` channel into a mask store, and returns **how many
// samples had to be clamped** -- i.e. how many stored half words were NaN or
// outside [0,1].
//
// Three things happen here that do not happen in the content unpackers, and
// each is deliberate:
//
//  1. **The drop rule is "all reveal", not "all zero"** (see kMaskChannelName).
//     An all-1.0 mask tile is what an *absent* mask tile means, so allocating
//     one would grow a document's resident cost on every save-and-reopen.
//  2. **Out-of-range words are rewritten to their clamped value**, not merely
//     clamped on the way out. `MaskTile::readCoverage()` clamps anyway, so the
//     composite is safe either way; rewriting means the value stored, the
//     value rendered and the value the next save writes are the same number.
//     The alternative -- carry the NaN verbatim for PRD I10's sake -- would
//     preserve a value no reader can act on, in a channel this build owns and
//     defines, and would keep re-warning about it forever.
//  3. **The count comes back so the caller can warn by name.** A silent clamp
//     of data the user did not author is exactly what PRD I11 forbids, and a
//     mask is the one channel where a bad value can make a whole layer vanish.
size_t unpackMaskChannel(const NpaintRawPart& part, size_t maskIdx, MaskTileStore* tiles) {
  const int32_t tileX0 = part.x / kTileSize;
  const int32_t tileY0 = part.y / kTileSize;
  const int32_t tilesW = part.width / kTileSize;
  const int32_t tilesH = part.height / kTileSize;
  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  const auto* words = reinterpret_cast<const uint16_t*>(part.rawPixels.data());

  size_t clamped = 0;
  std::vector<uint16_t> scratch(MaskTile::kTexelCount);
  for (int32_t ty = 0; ty < tilesH; ++ty) {
    for (int32_t tx = 0; tx < tilesW; ++tx) {
      const size_t col0 = static_cast<size_t>(tx) * kTileSize;
      const size_t row0 = static_cast<size_t>(ty) * kTileSize;
      bool allReveal = true;
      for (int32_t r = 0; r < kTileSize; ++r) {
        const uint16_t* row = words + (row0 + static_cast<size_t>(r)) * rowWords;
        for (int32_t x = 0; x < kTileSize; ++x) {
          uint16_t w = row[(col0 + static_cast<size_t>(x)) * channels + maskIdx];
          const float v = halfToFloat(w);
          if (!(v >= 0.0f && v <= 1.0f)) {  // false for NaN, for < 0 and for > 1
            w = floatToHalf(maskCoverageClamp(v));
            ++clamped;
          }
          if (w != MaskTile::kRevealWord) allReveal = false;
          scratch[static_cast<size_t>(r) * kTileSize + static_cast<size_t>(x)] = w;
        }
      }
      if (allReveal) continue;
      MaskTile& tile = tiles->getOrCreate(TileCoord{tileX0 + tx, tileY0 + ty});
      std::memcpy(tile.data(), scratch.data(), MaskTile::kTexelCount * sizeof(uint16_t));
    }
  }
  return clamped;
}

// The inverse of buildPigmentLayerPart(). Reads only the seven stored
// channels; R/G/B/A are a derived bake and are deliberately discarded, exactly
// as part 0 is. `idx` comes from `channelIndicesByName()` -- OpenEXR sorts a
// part's channels by name, so the read-back order before OpenImageIO's RGBA
// normalisation is `A B G R pig.c0 ... res.B res.G res.R`, and positional
// indexing would swap the residual's red and blue with no symptom until
// someone compared colours.
void unpackPigmentLayerPart(const NpaintRawPart& part, const std::vector<size_t>& idx,
                            PigmentTileStore* tiles) {
  const int32_t tileX0 = part.x / kTileSize;
  const int32_t tileY0 = part.y / kTileSize;
  const int32_t tilesW = part.width / kTileSize;
  const int32_t tilesH = part.height / kTileSize;
  const size_t channels = part.channelNames.size();
  const size_t rowWords = static_cast<size_t>(part.width) * channels;
  const auto* words = reinterpret_cast<const uint16_t*>(part.rawPixels.data());
  constexpr size_t kTileWords =
      static_cast<size_t>(kTileSize) * kTileSize * PigmentTile::kChannels;

  std::vector<uint16_t> scratch(kTileWords);
  for (int32_t ty = 0; ty < tilesH; ++ty) {
    for (int32_t tx = 0; tx < tilesW; ++tx) {
      const size_t col0 = static_cast<size_t>(tx) * kTileSize;
      const size_t row0 = static_cast<size_t>(ty) * kTileSize;
      for (int32_t r = 0; r < kTileSize; ++r) {
        const uint16_t* row = words + (row0 + static_cast<size_t>(r)) * rowWords;
        for (int32_t x = 0; x < kTileSize; ++x) {
          const uint16_t* in = row + (col0 + static_cast<size_t>(x)) * channels;
          uint16_t* out = scratch.data() + (static_cast<size_t>(r) * kTileSize +
                                            static_cast<size_t>(x)) *
                                               PigmentTile::kChannels;
          for (size_t c = 0; c < static_cast<size_t>(PigmentTile::kChannels); ++c)
            out[c] = in[idx[kPigmentLatentFirst + c]];
        }
      }
      // Same rule, same reason as the RGB unpacker: an all-zero pigment tile
      // is mass 0 with no latent anywhere, which is exactly what an
      // unallocated tile means, so keeping it would grow a sparse document's
      // resident cost on every save-and-reopen.
      bool anyNonZero = false;
      for (uint16_t w : scratch) {
        if (w != 0) {
          anyNonZero = true;
          break;
        }
      }
      if (!anyNonZero) continue;
      PigmentTile& tile = tiles->getOrCreate(TileCoord{tileX0 + tx, tileY0 + ty});
      std::memcpy(tile.data(), scratch.data(), kTileWords * sizeof(uint16_t));
    }
  }
}

// Part 0. Regenerated on every save, unconditionally (PRD I12,
// docs/document-format.md §3.4: "Part 0 is what every other tool renders. If
// it disagrees with the layer parts, other tools show something subtly wrong
// and nobody notices for months. Regenerate on every save,
// unconditionally.").
//
// io/Export's flattenDocumentToLinear() is called rather than reimplemented.
// There must be exactly one flattener in this binary: the moment the
// composite written into the document and the image io/Export writes could
// be produced by two different code paths, they can disagree, and PRD I5b
// ("Part 0 is a correct flattened composite, so any EXR reader shows the
// right image") would become a claim about two implementations staying in
// sync rather than about one being correct. Its signature needed no
// extending -- `DecodedImage flattenDocumentToLinear(const Document&)` is
// exactly the operation wanted here.
//
// It returns *straight* alpha (its documented contract, shared with
// io/ImageDecode), and EXR is the associated-alpha side of this codebase's
// boundary (io/Export.hpp's Alpha section, and the OpenEXR spec), so the
// re-association happens here, in linear light, before the half conversion.
//
// **Phase 5 step 1**: the flattener now really composites (`over`, honouring
// `visible` and `opacity`) instead of summing, so part 0 is a composite in the
// full sense for the first time. It can also, for the first time, be an
// *approximation*: a layer whose `np:blend` this build does not implement is
// composited as `over`. That is not silent -- `warningsOut` carries
// core/Composite's sentence into `NpaintSaveResult::warnings`, which is the
// field whose own doc comment is exactly this case ("the save went ahead, and
// the caller is told precisely what about it is approximate"). The layer
// part's `np:blend` string still goes to disk untouched (PRD I10), so nothing
// is lost -- only part 0's preview of it is provisional.
NpaintRawPart buildCompositePart(const Document& doc, std::vector<std::string>* warningsOut) {
  NpaintRawPart part;
  part.name = kCompositePartName;
  part.channelNames = {"R", "G", "B", "A"};
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;
  part.x = 0;
  part.y = 0;
  part.width = doc.width;
  part.height = doc.height;

  const DecodedImage flat = flattenDocumentToLinear(doc, warningsOut);
  const size_t sampleCount = static_cast<size_t>(doc.width) * static_cast<size_t>(doc.height) * 4;
  part.rawPixels.assign(sampleCount * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());
  if (!flat.valid()) return part;  // caller has already refused a zero-sized canvas

  for (size_t i = 0; i < sampleCount; i += 4) {
    const float a = flat.pixels[i + 3];
    words[i + 0] = compositeFloatToHalf(flat.pixels[i + 0] * a);
    words[i + 1] = compositeFloatToHalf(flat.pixels[i + 1] * a);
    words[i + 2] = compositeFloatToHalf(flat.pixels[i + 2] * a);
    words[i + 3] = compositeFloatToHalf(a);
  }
  return part;
}

// Part 0 back to io/ImageDecode's DecodedImage contract (straight alpha,
// linear float RGBA). Inspection only -- see NpaintLoadResult::composite.
DecodedImage decodeCompositePart(const NpaintRawPart& part) {
  DecodedImage img;
  const size_t channels = part.channelNames.size();
  if (channels != 4 || part.width <= 0 || part.height <= 0) return img;
  const bool isHalf = part.sampleTypeName == "half";
  const bool isFloat = part.sampleTypeName == "float";
  if (!isHalf && !isFloat) return img;

  const size_t texels = static_cast<size_t>(part.width) * static_cast<size_t>(part.height);
  img.width = static_cast<uint32_t>(part.width);
  img.height = static_cast<uint32_t>(part.height);
  img.pixels.resize(texels * 4);
  for (size_t i = 0; i < texels; ++i) {
    float v[4];
    for (int c = 0; c < 4; ++c) {
      if (isHalf) {
        v[c] = halfToFloat(reinterpret_cast<const uint16_t*>(part.rawPixels.data())[i * 4 + c]);
      } else {
        v[c] = reinterpret_cast<const float*>(part.rawPixels.data())[i * 4 + c];
      }
    }
    // Un-associate with the same `a <= 0 -> {0,0,0,0}` guard core/Probe.cpp,
    // io/Export.cpp and io/OiioBackend.cpp all use.
    const float a = v[3];
    if (a <= 0.0f) {
      v[0] = v[1] = v[2] = 0.0f;
    } else {
      v[0] /= a;
      v[1] /= a;
      v[2] /= a;
    }
    std::copy(v, v + 4, img.pixels.begin() + static_cast<ptrdiff_t>(i * 4));
  }
  return img;
}

// --- PRD I13 helpers: temp-file naming and the round-trip comparator ------

// True when every stored word is exactly zero -- the identity `Tile` and
// `PigmentTile` share (both are interleaved half-float channels with no
// distinguished "reveal" value, unlike `MaskTile`), and therefore the value
// an unallocated tile of either type already reads as. Used by
// verifyNpaintRoundTrip() below, not by the writer or reader -- those have
// their own copies of this exact test (unpackRgbLayerPart(),
// unpackPigmentLayerPart()) that this deliberately does not call, because a
// bug shared between the writer's drop rule and the verifier's identity rule
// would make the two agree about the wrong thing and the whole check would
// prove nothing.
bool allStoredWordsZero(const uint16_t* data, size_t count) noexcept {
  for (size_t i = 0; i < count; ++i) {
    if (data[i] != 0) return false;
  }
  return true;
}

// Compares two tile stores of the same type under the round-trip identity
// rule verifyNpaintRoundTrip() argues in io/NpaintFile.hpp: a tile present on
// exactly one side is only a mismatch if it is not that store's identity
// value, because the writer and reader are both allowed to drop one. Every
// tile present on *both* sides is compared bit-for-bit at zero tolerance --
// no rounding stage separates a tile in memory from the same tile decoded
// back out of the file, so anything but equality there is a real defect.
//
// `isIdentity` is the per-type predicate (`allStoredWordsZero` for `Tile` and
// `PigmentTile`, `MaskTile::isFullyRevealed()`, `SelectionTile::
// selectsNothing()`) -- passed in rather than dispatched on `TileType` so
// this stays one function for four unrelated types instead of four near-
// copies of it.
template <typename Store, typename IsIdentity>
bool tileStoresRoundTripEqual(const Store& memory, const Store& reloaded, IsIdentity isIdentity,
                              std::string* mismatch) {
  using TileType = typename Store::TileType;
  std::unordered_set<TileCoord> coords;
  coords.reserve(memory.occupiedTileCount() + reloaded.occupiedTileCount());
  for (const auto& [coord, tile] : memory) coords.insert(coord);
  for (const auto& [coord, tile] : reloaded) coords.insert(coord);

  for (const TileCoord& coord : coords) {
    const TileType* a = memory.find(coord);
    const TileType* b = reloaded.find(coord);
    if (a && b) {
      if (std::memcmp(a->data(), b->data(), sizeof(TileType)) != 0) {
        if (mismatch) {
          *mismatch = "tile (" + std::to_string(coord.x) + "," + std::to_string(coord.y) +
                      ") holds different pixels after the round trip";
        }
        return false;
      }
      continue;
    }
    // Present on exactly one side. Allowed only when the side that has it
    // holds nothing but the identity value -- see this function's own doc
    // comment and io/NpaintFile.hpp's verifyNpaintRoundTrip() for the four
    // named cases this covers.
    const TileType& only = a ? *a : *b;
    if (!isIdentity(only)) {
      if (mismatch) {
        *mismatch = "tile (" + std::to_string(coord.x) + "," + std::to_string(coord.y) + ") is " +
                    (a ? "allocated in the document being saved but absent after the round trip"
                       : "absent from the document being saved but present after the round trip") +
                    ", and is not empty -- an allocated-but-empty tile is allowed to change "
                    "presence (io/NpaintFile.cpp's own drop-on-read rule), a non-empty one is not";
      }
      return false;
    }
  }
  return true;
}

// Where saveNpaint() writes when `options.verifyReadback` is set: a sibling
// of `path`, in the same directory (so the final rename is same-filesystem
// and therefore atomic rather than a copy), with `.tmp` spliced in before the
// extension rather than appended after it.
//
// The splice, not a simple `path + ".tmp"` suffix, is deliberate and copies
// app/Journal.cpp's own established convention for exactly the reason its
// comment there gives: `loadNpaint()`'s read side opens the temp file through
// `OIIO::ImageInput::open(path)`, which tries format detection by extension
// before falling back to sniffing file contents, and a name ending in only
// `.tmp` is one that lookup has never heard of. `foo.npaint` becomes
// `foo.tmp.npaint`, `foo.exr` becomes `foo.tmp.exr` -- either way the
// extension a working reader keys on survives, and content sniffing (EXR has
// a magic number) would likely have caught even a bare `.tmp` suffix, but
// there is no reason to lean on "likely" when the exact-suffix form is
// already proven in this codebase.
std::string npaintTempSiblingPath(const std::string& path) {
  const std::filesystem::path p(path);
  const std::string tempName = p.stem().string() + ".tmp" + p.extension().string();
  return (p.parent_path() / tempName).string();
}

}  // namespace

bool npaintCompressionIsLossy(std::string_view name, std::string* whyOut) {
  const std::string c = lowerNoLevel(name);
  auto lossy = [&](const char* what, const char* alternative) {
    if (whyOut) {
      *whyOut = std::string("compression '") + c + "' is lossy: " + what +
                ". PRD I7 and docs/document-format.md §2 forbid it in a native file -- \"a "
                "working file is the one place that is unacceptable\" -- because the pixels "
                "read back are not the pixels that were painted. Use " + alternative + ".";
    }
    return true;
  };
  if (c == "dwaa" || c == "dwab") {
    return lossy("DWA is a DCT-based lossy compressor, like JPEG's",
                 "zip (the default) or piz for grainy content");
  }
  if (c == "b44" || c == "b44a") {
    return lossy("B44 quantizes each 4x4 block of half values to a shared exponent plus 6-bit "
                 "offsets, discarding low-order mantissa bits",
                 "zip (the default) or piz for grainy content");
  }
  if (c == "pxr24") {
    // Classified lossy deliberately, and it is the one entry here that is a
    // judgement call rather than a fact. PXR24 rounds 32-bit float down to
    // 24 bits and is genuinely lossless for HALF and 32-bit integer data --
    // which is every part *this build writes*. It is refused anyway because
    // a `.npaint` file can also contain FLOAT parts carried verbatim from a
    // newer writer (PRD I10), and a compressor whose losslessness depends on
    // what a future version happened to put in the file is not a compressor
    // a P0 "lossless only" rule can allow.
    return lossy("PXR24 truncates 32-bit float samples to 24 bits (it is lossless only for "
                 "half and integer data, and a `.npaint` file can carry float parts forward "
                 "from a newer writer)",
                 "zip (the default), which is lossless for every sample type");
  }
  if (whyOut) whyOut->clear();
  return false;
}

NpaintVerifyResult verifyNpaintRoundTrip(const Document& doc, const std::string& path) {
  NpaintVerifyResult result;
  auto fail = [&](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    return result;
  };

  // The whole point: this is loadNpaint(), the same function File > Open
  // calls, not a second reduced-effort reader written for this check alone.
  const NpaintLoadResult loaded = loadNpaint(path);
  if (!loaded.ok) {
    return fail("'" + path +
                "' was written but could not be read back through the same reader File > Open "
                "uses (" + loaded.error + "). The bytes on disk cannot be trusted as a document.");
  }
  const Document& back = loaded.document;

  if (back.width != doc.width || back.height != doc.height) {
    return fail("'" + path + "' reads back as " + std::to_string(back.width) + "x" +
                std::to_string(back.height) + ", not the " + std::to_string(doc.width) + "x" +
                std::to_string(doc.height) + " canvas that was saved.");
  }
  if (back.pigmentBasis != doc.pigmentBasis) {
    return fail("'" + path + "' reads back declaring pigment basis \"" + back.pigmentBasis +
                "\", not \"" + doc.pigmentBasis + "\" as the document being saved does.");
  }
  // Chromaticities: a native FLOAT[8] EXR attribute with no HALF stage, so
  // this is held to the same zero-tolerance standard as the pixel channels
  // rather than given a numeric tolerance it does not need.
  {
    const Primaries& a = doc.workingSpace.primaries;
    const Primaries& b = back.workingSpace.primaries;
    if (a.redX != b.redX || a.redY != b.redY || a.greenX != b.greenX || a.greenY != b.greenY ||
        a.blueX != b.blueX || a.blueY != b.blueY || a.whiteX != b.whiteX ||
        a.whiteY != b.whiteY) {
      return fail("'" + path +
                  "' reads back declaring different chromaticities than the document being "
                  "saved carries.");
    }
  }

  if (back.layers.size() != doc.layers.size()) {
    return fail("'" + path + "' reads back " + std::to_string(back.layers.size()) +
                " layer(s), not the " + std::to_string(doc.layers.size()) +
                " the document being saved holds.");
  }
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& a = doc.layers[i];
    const Layer& b = back.layers[i];
    const std::string where = "layer " + std::to_string(i) + " (\"" + a.name + "\")";
    if (a.kind != b.kind) {
      return fail(where + " reads back as a " + layerKindName(b.kind) + " layer, not " +
                  layerKindName(a.kind) + ".");
    }
    if (a.name != b.name) return fail(where + " reads back named \"" + b.name + "\".");
    if (a.blend != b.blend) {
      return fail(where + " reads back with blend \"" + b.blend + "\", not \"" + a.blend + "\".");
    }
    if (a.opacity != b.opacity) {
      return fail(where + " reads back at opacity " + std::to_string(b.opacity) + ", not " +
                  std::to_string(a.opacity) + ".");
    }
    if (a.visible != b.visible) {
      return fail(where + " reads back " + (b.visible ? "visible" : "hidden") +
                  ", the opposite of what was saved.");
    }
    if (a.locked != b.locked) {
      return fail(where + " reads back " + (b.locked ? "locked" : "unlocked") +
                  ", the opposite of what was saved.");
    }
    if (a.clipped != b.clipped) {
      return fail(where + " reads back " + (b.clipped ? "clipped" : "unclipped") +
                  ", the opposite of what was saved.");
    }
    if (a.parent != b.parent) {
      return fail(where + " reads back with parent \"" + b.parent + "\", not \"" + a.parent +
                  "\".");
    }
    if (a.colorLabel != b.colorLabel) {
      return fail(where + " reads back with colour label \"" + b.colorLabel + "\", not \"" +
                  a.colorLabel + "\".");
    }
    if (a.linkGroup != b.linkGroup) {
      return fail(where + " reads back in link group " + std::to_string(b.linkGroup) +
                  ", not " + std::to_string(a.linkGroup) + ".");
    }
    // io/OpSerial's own byte encoding, not a field-by-field walk of
    // core::Op's dozen union-like param blocks -- this is what np:ops
    // actually persists, so comparing it directly is comparing the save
    // rather than reimplementing a second opinion about it.
    if (serializeOpStack(a.ops) != serializeOpStack(b.ops)) {
      return fail(where + "'s op stack reads back different from what was saved.");
    }

    if (a.rgbTiles.has_value() != b.rgbTiles.has_value()) {
      return fail(where + (b.rgbTiles.has_value() ? " reads back with RGB tile storage it did "
                                                     "not have"
                                                   : " reads back with its RGB tile storage "
                                                     "missing entirely") +
                  ".");
    }
    if (a.rgbTiles) {
      std::string why;
      if (!tileStoresRoundTripEqual(
              *a.rgbTiles, *b.rgbTiles,
              [](const Tile& t) { return allStoredWordsZero(t.data(), Tile::kTexelCount); },
              &why)) {
        return fail(where + ": " + why + ".");
      }
    }
    if (a.pigmentTiles.has_value() != b.pigmentTiles.has_value()) {
      return fail(where + (b.pigmentTiles.has_value() ? " reads back with pigment tile storage "
                                                         "it did not have"
                                                       : " reads back with its pigment tile "
                                                         "storage missing entirely") +
                  ".");
    }
    if (a.pigmentTiles) {
      std::string why;
      if (!tileStoresRoundTripEqual(*a.pigmentTiles, *b.pigmentTiles,
                                    [](const PigmentTile& t) {
                                      return allStoredWordsZero(t.data(), PigmentTile::kTexelCount);
                                    },
                                    &why)) {
        return fail(where + ": " + why + ".");
      }
    }
    if (a.mask.has_value() != b.mask.has_value()) {
      return fail(where +
                  (b.mask.has_value() ? " reads back with a layer mask it did not have"
                                       : " reads back with its layer mask missing entirely") +
                  ".");
    }
    if (a.mask) {
      std::string why;
      if (!tileStoresRoundTripEqual(
              *a.mask, *b.mask, [](const MaskTile& t) { return t.isFullyRevealed(); }, &why)) {
        return fail(where + ": " + why + ".");
      }
    }
  }

  // Layer comps (PRD C14): compared whole, with LayerComp's own generated
  // operator==. This is also where Layer::id actually surfaces -- see
  // verifyNpaintRoundTrip()'s header comment on why the id is not compared
  // per layer.
  if (back.comps != doc.comps) {
    return fail("'" + path +
                "' reads back with different layer comps than the document being saved holds.");
  }

  if (back.channels.size() != doc.channels.size()) {
    return fail("'" + path + "' reads back " + std::to_string(back.channels.size()) +
                " alpha channel(s), not the " + std::to_string(doc.channels.size()) +
                " the document being saved holds.");
  }
  for (size_t i = 0; i < doc.channels.size(); ++i) {
    const AlphaChannel& a = doc.channels[i];
    const AlphaChannel& b = back.channels[i];
    if (a.name != b.name) {
      return fail("alpha channel " + std::to_string(i) + " reads back named \"" + b.name +
                  "\", not \"" + a.name + "\".");
    }
    std::string why;
    if (!tileStoresRoundTripEqual(
            a.tiles, b.tiles, [](const SelectionTile& t) { return t.selectsNothing(); }, &why)) {
      return fail("alpha channel \"" + a.name + "\": " + why + ".");
    }
  }

  result.ok = true;
  return result;
}

NpaintSaveResult saveNpaint(const Document& doc, const std::string& path,
                            const NpaintSaveOptions& options, const NpaintCarry* carry) {
  NpaintSaveResult result;
  auto fail = [&](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    return result;
  };

  // --- Validate the request first ---------------------------------------
  //
  // Deliberately before anything touches OpenImageIO, so a malformed request
  // is refused for its own reason rather than folded into whatever error the
  // backend happens to produce once it gets there. --selftest exercises
  // every PRD I11 refusal directly because of this ordering.
  if (doc.width <= 0 || doc.height <= 0) {
    return fail("save refused: the document has no canvas (" + std::to_string(doc.width) + "x" +
                std::to_string(doc.height) +
                "). A `.npaint` file's display window is the canvas, and EXR has no "
                "representation for a zero-area one.");
  }

  std::string why;
  const std::string compression = lowerNoLevel(options.compression);
  if (npaintCompressionIsLossy(options.compression, &why)) {
    return fail("save refused: " + why);
  }
  if (!isLosslessCompressor(compression)) {
    return fail("save refused: '" + options.compression +
                "' is not a compressor this build recognises, and an unrecognised name cannot "
                "be assumed lossless -- PRD I7 allows native files no lossy compression at "
                "all. Use one of: " + losslessCompressorList() + ".");
  }

  bool anyPigmentLayer = false;
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& layer = doc.layers[i];
    if (layer.kind != LayerKind::RGB && layer.kind != LayerKind::Pigment &&
        layer.kind != LayerKind::Adjustment && layer.kind != LayerKind::Group &&
        layer.kind != LayerKind::Vector && layer.kind != LayerKind::Text) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is a " + layerKindName(layer.kind) +
                  " layer, and this build has no on-disk representation for that kind -- "
                  "docs/document-format.md stores Media layers as `pig.*`/`res.*` latent "
                  "channels plus an `np:simParams` blob and Strokes layers as an `np:dabs` "
                  "blob, and core::Layer has neither per-medium simulation state nor a dab "
                  "list. Saving would drop the layer entirely, so nothing was written. Remove "
                  "the layer, or convert it to an RGB or Pigment layer, to save this "
                  "document.");
    }
    if (layer.kind == LayerKind::Group &&
        (layer.rgbTiles.has_value() || layer.pigmentTiles.has_value())) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is Group-kind but carries pixel tile storage, which core/Layer.hpp's "
                  "own contract says cannot happen -- a Group holds no pixels of its own. "
                  "Its part in the file has no channel to put them in, so writing it would "
                  "drop them. Nothing was written; this is a malformed document rather than "
                  "an unsupported one.");
    }
    if (layer.kind == LayerKind::Group && layer.groupTag.empty()) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is Group-kind but has no `groupTag` -- every Group `core::makeGroupLayer()` "
                  "creates gets one immediately, so an empty tag here means this Layer was "
                  "constructed directly rather than through it. Without a tag no member's "
                  "`np:parent` could ever name this part, which would silently ungroup every "
                  "member on the next load. Nothing was written.");
    }
    if (layer.kind == LayerKind::RGB && !layer.rgbTiles.has_value()) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is RGB-kind but has no tile storage at all (`rgbTiles` is absent), "
                  "which core/Layer.hpp's own contract says cannot happen for an RGB layer. "
                  "Nothing was written; this is a malformed document rather than an "
                  "unsupported one.");
    }
    if (layer.kind == LayerKind::Adjustment &&
        (layer.rgbTiles.has_value() || layer.pigmentTiles.has_value())) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is Adjustment-kind but carries pixel tile storage, which "
                  "core/Layer.hpp's own contract says cannot happen -- an Adjustment layer "
                  "holds no pixels of its own, only an op stack. Its part in the file has no "
                  "channel to put them in, so writing it would drop them. Nothing was "
                  "written; this is a malformed document rather than an unsupported one.");
    }
    if (layer.kind == LayerKind::Pigment) {
      if (!layer.pigmentTiles.has_value()) {
        return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                    "\") is Pigment-kind but has no tile storage at all (`pigmentTiles` is "
                    "absent), which core/Layer.hpp's own contract says cannot happen for a "
                    "Pigment layer. Nothing was written; this is a malformed document rather "
                    "than an unsupported one.");
      }
      anyPigmentLayer = true;
    }
    // **The op stack is written now, so there is nothing left to warn about
    // here.** Until PLAN.md Phase 5 step 5 this block pushed a warning naming
    // the layer, because `np:ops` is a blob in docs/document-format.md and
    // this OpenImageIO drops array-typed header attributes on write. That step
    // took the fix the spec itself names -- a hex `string` carrier, io/OpSerial
    // -- because an Adjustment layer's entire content *is* its stack, so
    // warning-and-dropping would have meant losing the whole layer on every
    // save. The carrier is not Adjustment-specific: every kind's `Layer::ops`
    // now round-trips.
    if (!(layer.opacity >= 0.0f) || layer.opacity > 1.0f) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") has opacity " + std::to_string(layer.opacity) +
                  ", outside [0, 1]. `np:opacity` is a float attribute every reader will act "
                  "on literally, so writing an out-of-range value would put a number in the "
                  "file that no reader -- including this one -- can honour.");
    }
    // PLAN.md Phase 5 step 11. `np:link` is an `int`, so a group number this
    // build could only truncate is refused by name -- the same rule the
    // opacity check above follows, and for the same reason: a truncated group
    // number would silently link two layers that were never linked.
    if (layer.linkGroup > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is in link group " + std::to_string(layer.linkGroup) +
                  ", which does not fit `np:link`'s int attribute (max " +
                  std::to_string(std::numeric_limits<int32_t>::max()) +
                  "). Writing it truncated would silently link this layer to a different "
                  "group on reload.");
    }
  }

  // --- Alpha channels (PLAN.md Phase 7; PRD E11, E13) --------------------
  //
  // Two refusals, and both are about the *name*, because the name is the only
  // handle a channel has: `core::loadChannelAsSelection(doc, name)` is how a
  // saved selection is restored, and there is no synthetic id standing behind
  // it the way `L####` stands behind a layer's non-unique name.
  //
  // Refused rather than repaired -- not uniquified on the way out, not given a
  // stand-in name -- because either repair is the writer editing the user's
  // document during a save, and the user would find a channel called something
  // they never typed with no record of when it changed.
  for (size_t i = 0; i < doc.channels.size(); ++i) {
    const AlphaChannel& channel = doc.channels[i];
    if (channel.name.empty()) {
      return fail("save refused: alpha channel " + std::to_string(i) +
                  " has no name, and `np:name` is the only way anything refers to a channel "
                  "(loadChannelAsSelection() takes a name). An empty string attribute does not "
                  "survive this OpenImageIO either, so the part would come back nameless. "
                  "Nothing was written.");
    }
    for (size_t j = i + 1; j < doc.channels.size(); ++j) {
      if (doc.channels[j].name != channel.name) continue;
      return fail("save refused: alpha channels " + std::to_string(i) + " and " +
                  std::to_string(j) + " are both named \"" + channel.name +
                  "\", and a channel is looked up by name -- two would make "
                  "loadChannelAsSelection() depend on list order, which is not something a "
                  "user can see. Rename one (core::uniqueChannelName() produces a free name). "
                  "Nothing was written.");
    }
  }

  // PRD C8: "the file records which pigment basis produced them". The value
  // stamped is the *document's* (PLAN.md Phase 5 step 15), not this build's
  // constant, so a document loaded from a file written in another basis is
  // written back declaring that basis rather than relabelled with this one. A
  // non-empty carried basis wins, because that is the string the source file
  // actually held and PRD I10's carry-through is verbatim by definition; after
  // any load the two are the same string, so the precedence only decides a
  // case that a load cannot produce.
  if (doc.pigmentBasis.empty()) {
    return fail(std::string("save refused: the document declares no pigment basis (its "
                            "`pigmentBasis` is empty), so this file would record nothing about "
                            "which model produced its latents (PRD C8). An empty `np:basis` is "
                            "not even distinguishable from an absent one -- this OpenImageIO "
                            "drops empty string attributes on write. Nothing was written. Set it "
                            "to \"") +
                kNpaintPigmentBasis + "\" for latents this build produced.");
  }

  // docs/document-format.md §3.3: "a basis mismatch" is one of the things a
  // save must name rather than degrade silently. The mismatch that matters is
  // between what the document says its latents are and what the file it came
  // from said they were: writing latents fitted in one basis into a file
  // stamped with another produces a document that is wrong and says it is
  // right. Refused rather than warned, because unlike an unimplemented blend
  // this is not an approximation of the pixels -- it is a mislabelling of what
  // the numbers *are*.
  //
  // Narrower than it was before step 15, and deliberately: it used to compare
  // the carry against this build's constant, which caught every foreign-basis
  // document rather than only the incoherent ones, and so made a legitimately
  // loaded document impossible to save at all -- including by app/Journal's
  // crash checkpoint. A document loaded from a foreign-basis file now agrees
  // with itself and saves; only something that *changed* one of the two can
  // land here. See io/NpaintFile.hpp's kNpaintPigmentBasis for the three
  // rejected alternatives.
  //
  // Only when the document actually has a Pigment layer. An RGB-only document
  // loaded from a foreign-basis file still round-trips its `np:basis`
  // untouched (PRD I10), exactly as it did before this step.
  if (anyPigmentLayer && carry != nullptr && !carry->basis.empty() &&
      carry->basis != doc.pigmentBasis) {
    return fail(std::string("save refused: this document has Pigment layers whose latents it "
                            "declares to be in the \"") +
                doc.pigmentBasis + "\" basis, but it was loaded from a file declaring np:basis \"" +
                carry->basis +
                "\", which is preserved verbatim on save (PRD I10). A latent is only "
                "meaningful in the basis it was fitted in, and silently so -- writing both "
                "into one file would produce a document no reader could interpret correctly. "
                "Nothing was written. docs/document-format.md §3.3 lists exactly this case.");
  }

  // Attribute types and part layouts this OpenImageIO cannot actually write
  // -- both measured against this build, both refused by name rather than
  // written out short. See NpaintAttribute and NpaintRawPart::tileWidth for
  // the measurements. Checked here, with the rest of the request validation,
  // so the refusal is identical in both build configurations.
  if (carry) {
    auto firstBlob = [](const std::vector<NpaintAttribute>& attrs) -> const NpaintAttribute* {
      for (const NpaintAttribute& a : attrs) {
        if (a.type == NpaintAttribute::Type::Blob) return &a;
      }
      return nullptr;
    };
    std::string where = "part 0";
    const NpaintAttribute* blob = firstBlob(carry->documentAttributes);
    for (size_t i = 0; i < carry->layerAttributes.size() && !blob; ++i) {
      blob = firstBlob(carry->layerAttributes[i]);
      if (blob) where = "layer " + std::to_string(i);
    }
    for (size_t i = 0; i < carry->rawParts.size() && !blob; ++i) {
      blob = firstBlob(carry->rawParts[i].attributes);
      if (blob) where = "part '" + carry->rawParts[i].name + "'";
    }
    if (blob) {
      return fail("save refused: attribute '" + blob->name + "' on " + where +
                  " is a " + std::to_string(blob->blobValue.size()) +
                  "-byte UINT8[n] blob, and this OpenImageIO's OpenEXR writer silently drops "
                  "array-typed header attributes -- measured: a UINT8[5] attribute written "
                  "through it is simply absent when the file is read back. Writing the file "
                  "would lose the blob without saying so, which is exactly what PRD I10 and "
                  "I11 forbid. Encode the payload as a base64 or hex `string` attribute "
                  "instead; string, int and float all survive.");
    }
    // Empty strings are dropped too (measured), but unlike blobs that is
    // sometimes harmless -- it depends entirely on what the *reader* of the
    // attribute defaults to, which is knowable for this build's own
    // attributes and unknowable for a carried one. So: a warning naming the
    // attribute, not a refusal.
    auto warnEmpty = [&](const std::vector<NpaintAttribute>& attrs, const std::string& where) {
      for (const NpaintAttribute& a : attrs) {
        if (a.type != NpaintAttribute::Type::String || !a.stringValue.empty()) continue;
        result.warnings.push_back(
            "carried attribute '" + a.name + "' on " + where +
            " has an empty string value, and this OpenImageIO's OpenEXR writer drops empty "
            "string attributes (measured). It will be absent from the file rather than "
            "present-and-empty; whether that loses anything depends on what the build that "
            "wrote it defaults the attribute to.");
      }
    };
    warnEmpty(carry->documentAttributes, "part 0");
    for (size_t i = 0; i < carry->layerAttributes.size(); ++i)
      warnEmpty(carry->layerAttributes[i], "layer " + std::to_string(i));
    for (const NpaintRawPart& raw : carry->rawParts)
      warnEmpty(raw.attributes, "part '" + raw.name + "'");

    for (const NpaintRawPart& raw : carry->rawParts) {
      if (raw.tileWidth <= 0 || raw.tileHeight <= 0) {
        return fail("save refused: carried part '" + raw.name +
                    "' is scanline-stored, and this OpenImageIO cannot write a multi-part EXR "
                    "that mixes scanline and tiled parts (it fails partway through with "
                    "\"Can't build a TiledOutputFile from a type-mismatched part\"). Every "
                    "part this build writes is tiled, because PRD I4's native format is a "
                    "multi-part *tiled* EXR. Nothing was written -- retiling the carried part "
                    "would change bytes this build promised to leave alone.");
      }
    }
  }

  // --- Assemble the parts ------------------------------------------------
  OiioExrWriteRequest request;
  request.path = path;
  request.compression = compression;
  request.displayWidth = doc.width;
  request.displayHeight = doc.height;
  // PRD I6: "Primaries are declared by the standard `chromaticities`
  // attribute." OpenEXR's own order, red/green/blue/white x,y.
  request.hasChromaticities = true;
  request.chromaticities = {doc.workingSpace.primaries.redX,   doc.workingSpace.primaries.redY,
                            doc.workingSpace.primaries.greenX, doc.workingSpace.primaries.greenY,
                            doc.workingSpace.primaries.blueX,  doc.workingSpace.primaries.blueY,
                            doc.workingSpace.primaries.whiteX, doc.workingSpace.primaries.whiteY};

  NpaintRawPart composite = buildCompositePart(doc, &result.warnings);
  composite.attributes.push_back(intAttr(kAttrVersion, kNpaintFormatVersion));
  // PRD C8, and the document's own claim rather than this build's constant --
  // see the basis checks in the request validation above, and
  // io/NpaintFile.hpp's kNpaintPigmentBasis for why a foreign value is stamped
  // back out rather than replaced. Refused above if it were empty, so this
  // cannot write an attribute the writer would silently drop.
  composite.attributes.push_back(stringAttr(
      kAttrBasis, (carry && !carry->basis.empty()) ? carry->basis : doc.pigmentBasis));
  composite.attributes.push_back(intAttr(kAttrTileSize, kTileSize));
  if (carry) {
    // PRD I10, the write half. These are the np:* attributes the reader did
    // not recognise; they go back out exactly as they came in. The reader
    // guarantees none of them collides with a recognised name, so no
    // de-duplication is needed here -- and if one ever did, OpenImageIO's
    // last-write-wins would silently drop the one this build wrote, which is
    // why the split happens on the read side rather than here.
    for (const NpaintAttribute& a : carry->documentAttributes) {
      // The one exception to "a recognised name is this build's to write", and
      // it is `np:ops`' own exception one level up: an `np:comps` this build
      // could not *parse* was carried verbatim by the loader (PRD I10), and
      // this document has no comps of its own to write in its place, so it goes
      // back out unchanged. The `doc.comps` test makes the two mutually
      // exclusive, so part 0 can never end up with two.
      if (isDocumentAttributeRecognised(a.name) &&
          !(a.name == kAttrComps && doc.comps.empty()))
        continue;
      composite.attributes.push_back(a);
    }
  }
  request.parts.push_back(std::move(composite));

  // Layer part names: the id this layer already had, when it came from a
  // file, so `np:parent` links and any external reference to `L0003` keep
  // meaning the same layer across a round trip. Otherwise the first unused
  // `L####`, skipping every name a carried part has claimed -- EXR requires
  // part names to be unique and a carried Pigment part may well be sitting
  // on `L0002`.
  std::vector<std::string> takenNames;
  if (carry) {
    for (const NpaintRawPart& raw : carry->rawParts) takenNames.push_back(raw.name);
  }
  std::vector<std::string> layerNames(doc.layers.size());
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (carry && i < carry->layerPartNames.size() && !carry->layerPartNames[i].empty()) {
      layerNames[i] = carry->layerPartNames[i];
    }
  }
  size_t nextId = 1;
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (!layerNames[i].empty()) continue;
    for (;;) {
      std::string candidate = layerPartName(nextId++);
      if (std::find(takenNames.begin(), takenNames.end(), candidate) != takenNames.end())
        continue;
      if (std::find(layerNames.begin(), layerNames.end(), candidate) != layerNames.end())
        continue;
      layerNames[i] = std::move(candidate);
      break;
    }
  }

  // Channel part names (`S####`), in `Document::channels` order.
  //
  // **Allocated fresh on every save, not carried like `layerPartNames`**, and
  // that asymmetry is deliberate rather than an omission. A layer's part name
  // has to survive a round trip because `np:parent` links name a part, and a
  // carried part written by a newer build may hold a reference to `L0003` this
  // build cannot see inside -- renumbering would repoint it. **Nothing in this
  // format references a channel part name at all**: a channel is referred to by
  // its `np:name`, which is document data and does round-trip. So a carry
  // vector for these would be a field to keep in step with no reader. If a
  // future attribute ever does reference `S####` -- a layer masked by a channel
  // would be the obvious one -- this is the decision that has to be revisited,
  // and it is named here rather than left to be discovered then.
  //
  // Names taken by *carried* parts are still skipped, which is not theoretical:
  // a newer build's channel part carrying a second channel this build does not
  // understand is carried verbatim under its own `S0001`, and EXR requires
  // unique part names.
  std::vector<std::string> channelNames(doc.channels.size());
  {
    size_t nextChannelId = 1;
    for (size_t i = 0; i < doc.channels.size(); ++i) {
      for (;;) {
        std::string candidate = channelPartName(nextChannelId++);
        if (std::find(takenNames.begin(), takenNames.end(), candidate) != takenNames.end())
          continue;
        channelNames[i] = std::move(candidate);
        break;
      }
    }
  }

  // --- `np:comps` (PLAN.md Phase 5 step 12, PRD C14) ----------------------
  //
  // Assembled here, after the part names exist, because the payload's whole
  // job is to join the *format's* stable layer id (the part name) to the
  // in-memory one (`Layer::id`) that a comp entry names -- io/CompSerial.hpp
  // argues why the join lives in this one attribute rather than as an
  // `np:id` on every layer part. `request.parts[0]` is part 0, already
  // pushed above; appending to it here rather than reordering the block keeps
  // every other attribute exactly where it was.
  //
  // **Written only when the document has comps.** An empty list produces no
  // attribute at all rather than a well-formed zero-count payload, for the two
  // reasons `np:ops` gives: an empty `string` attribute is dropped by this
  // OpenImageIO anyway (measured; see NpaintAttribute), and a document with no
  // comps has to keep producing exactly the bytes this build produced before
  // the attribute existed -- which `--selftest` asserts against a file rather
  // than assuming.
  //
  // No layer part is touched by any of this, in either direction, which is
  // what makes that regression boundary structural rather than careful.
  if (!doc.comps.empty()) {
    LayerCompCarrier comps;
    comps.nextLayerId = doc.nextLayerId;
    comps.layerIds.reserve(doc.layers.size());
    for (size_t i = 0; i < doc.layers.size(); ++i)
      comps.layerIds.emplace_back(layerNames[i], doc.layers[i].id);
    comps.comps = doc.comps;
    request.parts[0].attributes.push_back(
        stringAttr(kAttrComps, serializeLayerComps(comps)));
  }

  auto appendLayerPart = [&](size_t i) {
    const Layer& layer = doc.layers[i];
    NpaintRawPart part;
    switch (layer.kind) {
      case LayerKind::Pigment: part = buildPigmentLayerPart(layer, layerNames[i]); break;
      case LayerKind::Adjustment: part = buildAdjustmentLayerPart(layer, layerNames[i]); break;
      case LayerKind::Group: part = buildGroupLayerPart(layer, layerNames[i]); break;
      case LayerKind::Vector: part = buildVectorLayerPart(layer, layerNames[i]); break;
      case LayerKind::Text: part = buildTextLayerPart(layer, layerNames[i]); break;
      default: part = buildLayerPart(layer, layerNames[i]); break;
    }
    part.attributes.push_back(stringAttr(kAttrKind, layerKindName(layer.kind)));
    part.attributes.push_back(stringAttr(kAttrName, layer.name));
    part.attributes.push_back(stringAttr(kAttrBlend, layer.blend));
    part.attributes.push_back(floatAttr(kAttrOpacity, layer.opacity));
    part.attributes.push_back(intAttr(kAttrVisible, layer.visible ? 1 : 0));
    part.attributes.push_back(intAttr(kAttrLocked, layer.locked ? 1 : 0));
    part.attributes.push_back(stringAttr(kAttrParent, layer.parent));
    // Written only when there is a stack. An empty one produces no attribute
    // at all rather than a well-formed zero-count payload, for two reasons:
    // an empty `string` attribute is dropped by this OpenImageIO anyway
    // (measured; see NpaintAttribute), and every `.npaint` this build wrote
    // before PLAN.md Phase 5 step 5 has to keep producing the same bytes --
    // which `--selftest` asserts against HEAD rather than assuming.
    if (layer.ops.size() > 0)
      part.attributes.push_back(stringAttr(kAttrOps, serializeOpStack(layer.ops)));
    // Adjustment AND Group parts: both reuse `buildAdjustmentLayerPart()`'s
    // shape (a `mask` channel always present, `buildGroupLayerPart()` is a
    // thin wrapper over it), so its presence cannot say whether the layer has
    // a mask on either kind. See buildAdjustmentLayerPart().
    if (layer.kind == LayerKind::Adjustment || layer.kind == LayerKind::Group)
      part.attributes.push_back(intAttr(kAttrMask, layer.mask.has_value() ? 1 : 0));
    // Group parts only: the group's own stable identity, verbatim -- see
    // `kAttrGroupId`'s own comment for why no translation happens here.
    if (layer.kind == LayerKind::Group)
      part.attributes.push_back(stringAttr(kAttrGroupId, layer.groupTag));
    // Vector parts only, and only when there is something to write: an empty
    // Vector layer must produce the bytes it produced before this attribute
    // existed, which is also what makes the `layer.shapes.empty()` carry
    // exception below sound.
    if (layer.kind == LayerKind::Vector && !layer.shapes.empty())
      part.attributes.push_back(
          stringAttr(kAttrVector, serializeVectorShapes(layer.shapes, layer.nextShapeId)));
    // **Text parts, UNCONDITIONALLY -- and that is a deliberate divergence
    // from the line above.** `np:vector`'s `!shapes.empty()` guard exists so
    // that a document with no vector content keeps producing the exact bytes
    // this build produced before that attribute existed. No build before this
    // one could save a Text layer AT ALL (the save refusal above listed the
    // kind), so there are no earlier bytes to match and the argument does not
    // apply.
    //
    // What the guard would cost here is real: an empty string with a chosen
    // font, size and colour is a legitimate state -- it is the state a layer
    // is in between being created and being typed into -- and skipping the
    // write would silently discard the user's font choice on save. `np:vector`
    // has no equivalent, because a shape list with no shapes carries no
    // settings.
    //
    // The mutual exclusion with a carried `np:text` is handled where the carry
    // is replayed, a few lines below, rather than by a predicate here: an
    // empty `TextContent` is a legitimate value and therefore is NOT evidence
    // that a decode failed, so `shapes.empty()`'s trick has no analogue.
    const bool writesOwnText = layer.kind == LayerKind::Text;
    if (writesOwnText)
      part.attributes.push_back(stringAttr(kAttrText, serializeTextContent(layer.text)));
    // **Written only when the layer is actually clipped** (PLAN.md Phase 5
    // step 9), for the reason `np:ops` and `np:mask` each state in their own
    // way: a document with no clipped layer has to keep producing the bytes
    // this build produced before the attribute existed, and `--selftest`
    // asserts that against a file written before any clip flag is set rather
    // than assuming it. Absent therefore reads as `false`, which is
    // `Layer::clipped`'s own default -- the same "the identity element is the
    // absent case" rule the mask channel uses for 1.0.
    if (layer.clipped) part.attributes.push_back(intAttr(kAttrClipped, 1));
    // **Written only when the layer is actually alpha-locked**, `np:clipped`'s
    // own rule and reason: a document with no locked alpha has to keep
    // producing the bytes this build produced before the attribute existed,
    // measured against a save of this same fixture with every `alphaLocked`
    // left false rather than assumed (--selftest, io/NpaintFile.cpp's own
    // test). Absent therefore reads as `false`, `Layer::alphaLocked`'s own
    // default.
    if (layer.alphaLocked) part.attributes.push_back(intAttr(kAttrAlphaLocked, 1));
    // PLAN.md Phase 5 step 11 / PRD C15. Written only when set; see the
    // attribute names at the top of this file for why these two are scalars
    // rather than a carrier, and why the range check on the link group is a
    // refusal rather than a truncation.
    if (!layer.colorLabel.empty())
      part.attributes.push_back(stringAttr(kAttrLabel, layer.colorLabel));
    if (layer.linkGroup != 0)
      part.attributes.push_back(intAttr(kAttrLink, static_cast<int32_t>(layer.linkGroup)));
    if (carry && i < carry->layerAttributes.size()) {
      for (const NpaintAttribute& a : carry->layerAttributes[i]) {
        // The one exception to "a recognised name is this build's to write":
        // an `np:ops` this build could not *parse* was carried verbatim by the
        // loader (PRD I10), and this layer has no stack of its own to write in
        // its place, so it goes back out unchanged. The `layer.ops` test makes
        // the two mutually exclusive, so a part can never end up with two.
        if (isLayerAttributeRecognised(a.name) &&
            !(a.name == kAttrOps && layer.ops.size() == 0) &&
            // A carried `np:vector` this build could not parse is written back
            // verbatim, but ONLY when this build has nothing of its own to put
            // in its place. Without this test a layer would emit TWO
            // `np:vector` attributes and OpenImageIO's last-write-wins would
            // silently pick one -- io/CompSerial's `doc.comps.empty()` rule.
            !(a.name == kAttrVector && layer.shapes.empty()) &&
            // A carried `np:text` this build could not parse is written back
            // verbatim, but only when this build did not just write one of its
            // own -- which, per the block above, is exactly when the layer is
            // not a Text layer. Two `np:text` attributes on one part would
            // leave OpenImageIO's last-write-wins to pick between them.
            !(a.name == kAttrText && !writesOwnText))
          continue;
        part.attributes.push_back(a);
      }
    }
    request.parts.push_back(std::move(part));
  };

  // An alpha channel's part (PRD E11, E13). Two attributes and one channel of
  // pixels -- there is nothing else a channel is. No `np:blend`, `np:opacity`
  // or `np:visible`: a channel is not composited, it is coverage a command
  // loads. Writing them anyway "for symmetry" would put values in the file that
  // nothing reads and that a later reader might believe.
  auto appendChannelPart = [&](size_t i) {
    NpaintRawPart part = buildChannelPart(doc.channels[i], channelNames[i]);
    part.attributes.push_back(stringAttr(kAttrKind, kChannelKindName));
    part.attributes.push_back(stringAttr(kAttrName, doc.channels[i].name));
    request.parts.push_back(std::move(part));
  };

  // Part order after part 0. The carried order is replayed first so a
  // carried part that sat *between* two layers stays between them -- see
  // NpaintPartSlot on why appending them at the end would be data loss in
  // the ordering rather than in the bytes.
  std::vector<bool> layerWritten(doc.layers.size(), false);
  std::vector<bool> rawWritten(carry ? carry->rawParts.size() : 0, false);
  std::vector<bool> channelWritten(doc.channels.size(), false);
  if (carry) {
    for (const NpaintPartSlot& slot : carry->partOrder) {
      switch (slot.kind) {
        case NpaintPartSlot::Kind::Layer:
          if (slot.index >= doc.layers.size() || layerWritten[slot.index]) continue;
          layerWritten[slot.index] = true;
          appendLayerPart(slot.index);
          break;
        case NpaintPartSlot::Kind::Channel:
          if (slot.index >= doc.channels.size() || channelWritten[slot.index]) continue;
          channelWritten[slot.index] = true;
          appendChannelPart(slot.index);
          break;
        case NpaintPartSlot::Kind::RawPart:
          if (slot.index >= carry->rawParts.size() || rawWritten[slot.index]) continue;
          rawWritten[slot.index] = true;
          request.parts.push_back(carry->rawParts[slot.index]);
          break;
      }
    }
  }
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (layerWritten[i]) continue;
    appendLayerPart(i);
  }
  // Channels created since the load, and every channel of a document that was
  // never loaded, go after the layers -- which is docs/document-format.md's own
  // sketch, where `S0001` follows the `L####` parts. A channel that came in
  // from a file has already been written above, in the position it held there.
  for (size_t i = 0; i < doc.channels.size(); ++i) {
    if (channelWritten[i]) continue;
    appendChannelPart(i);
  }
  if (carry) {
    for (size_t i = 0; i < carry->rawParts.size(); ++i) {
      if (rawWritten[i]) continue;
      request.parts.push_back(carry->rawParts[i]);
    }
  }

  // EXR requires unique part names in a multi-part file. Defensive: the name
  // allocation above cannot produce a collision, but a carried part could
  // arrive with a duplicate name from a malformed file, and OpenImageIO's
  // own diagnostic for that would name neither the document nor the layer.
  for (size_t i = 0; i < request.parts.size(); ++i) {
    for (size_t j = i + 1; j < request.parts.size(); ++j) {
      if (request.parts[i].name == request.parts[j].name) {
        return fail("save refused: two parts would be named '" + request.parts[i].name +
                    "', and EXR requires every part of a multi-part file to have a unique "
                    "name. Nothing was written.");
      }
    }
  }

  const int32_t partsWritten = static_cast<int32_t>(request.parts.size());

  if (!options.verifyReadback) {
    // Unchanged from before this option existed: open `path` directly and
    // write straight into it. Every existing caller -- app/Journal's crash
    // checkpoint, and every `--selftest` call that predates PRD I13 -- takes
    // this branch, byte for byte the same as it did before. See
    // NpaintSaveOptions::verifyReadback for why this in-place write is a real
    // hazard (a failure partway can destroy a previously-good file at `path`)
    // and why that hazard is reported rather than silently fixed for callers
    // that have not opted into the fix.
    std::string error;
    if (!oiioWriteMultiPartExr(request, &error)) return fail(error);
    result.ok = true;
    result.partsWritten = partsWritten;
    return result;
  }

  // --- Verified save: write beside `path`, read it back, only then replace -
  //
  // This is the whole of PRD I13's "before the original leaves memory": at
  // every one of the four places this can fail below -- the write itself,
  // the reload, the comparison, the final rename -- `path` has not been
  // opened for writing, so whatever was there when this call started is
  // exactly what is there if it returns `!ok`. See
  // NpaintSaveOptions::verifyReadback for the cost argument and why only some
  // callers set this.
  namespace fs = std::filesystem;
  const std::string tempPath = npaintTempSiblingPath(path);
  request.path = tempPath;

  std::string writeError;
  if (!oiioWriteMultiPartExr(request, &writeError)) {
    // oiioWriteMultiPartExr()'s own failure path already removed `tempPath`
    // ("never leave a partial document behind") -- `path` itself was never
    // touched.
    return fail("save refused: writing '" + path + "' failed before it could even be verified (" +
                writeError +
                "). The file that was at '" + path + "' before this save, if any, was not "
                "touched.");
  }

  // What verifyNpaintRoundTrip() is asked to confirm is what was actually
  // WRITTEN, not `doc` read back to the letter -- and those two are not
  // always the same string. `np:basis` above is stamped from `carry->basis`
  // when it is non-empty, exactly the precedence kNpaintPigmentBasis and the
  // basis-mismatch refusal already argue: a non-empty carried basis is what
  // the file this document came from declared, and PRD I10 carries it
  // verbatim. An RGB-only document loaded from a foreign-basis file is the
  // legitimate case that reaches here with `doc.pigmentBasis` and
  // `carry->basis` genuinely different (the mismatch refusal above only
  // fires when the document holds Pigment layers) -- comparing the reload
  // against `doc.pigmentBasis` unmodified would make every such save fail
  // its own verification, which is not a corrupted save, it is this
  // function forgetting its own precedence rule. `expected` is a cheap
  // copy -- `core::Document`'s tile stores are copy-on-write, so this is
  // refcount increments, not a second copy of any pixel -- with only the
  // one field corrected.
  Document expected = doc;
  if (carry && !carry->basis.empty()) expected.pigmentBasis = carry->basis;
  const NpaintVerifyResult verified = verifyNpaintRoundTrip(expected, tempPath);
  if (!verified.ok) {
    std::error_code removeEc;
    fs::remove(tempPath, removeEc);
    result.ok = false;
    result.verificationFailed = true;
    // Names the file (twice -- the path the user asked to save to, and what
    // went wrong reading it back), says the previous version is intact, and
    // says what to do next, matching app/StrokeSession.hpp's PixelOpRefusal
    // and the bucket refusals' house style: name the problem, name the fix.
    result.error =
        "save refused: '" + path +
        "' did not verify after being written, so the write was discarded and the file at '" +
        path + "' is exactly as it was before this save -- your previous version is safe. " +
        verified.error +
        " Try saving again; if this keeps happening, check available disk space and that '" +
        path + "' and its folder are writable, then report it -- a save that writes but does "
               "not verify usually means a bug in the writer, not a full disk.";
    return result;
  }

  std::error_code renameEc;
  fs::rename(tempPath, path, renameEc);
  if (renameEc) {
    std::error_code removeEc;
    fs::remove(tempPath, removeEc);
    return fail("save refused: '" + path +
                "' was written and verified correctly, but could not be put in place (" +
                renameEc.message() +
                "). The file that was at '" + path + "' before this save, if any, was not "
                "touched; nothing was lost, but nothing new was saved either. Check that '" +
                path + "' and its folder are writable and try again.");
  }

  result.ok = true;
  result.partsWritten = partsWritten;
  return result;
}

NpaintLoadResult loadNpaint(const std::string& path) {
  NpaintLoadResult result;
  const OiioExrReadResult read = oiioReadMultiPartExr(path);
  if (!read.ok) {
    result.error = read.error;
    return result;
  }
  result.warnings = read.warnings;

  // --- Part 0: the document's own header --------------------------------
  const NpaintRawPart& part0 = read.parts[0];
  if (part0.name != kCompositePartName && !part0.name.empty()) {
    result.warnings.push_back("part 0 is named '" + part0.name + "' rather than '" +
                              kCompositePartName +
                              "'; it is treated as the composite regardless, because "
                              "docs/document-format.md defines part 0 as the composite and "
                              "every other EXR reader shows it as the image.");
  }
  result.document.width = read.displayWidth;
  result.document.height = read.displayHeight;
  if (result.document.width <= 0 || result.document.height <= 0) {
    // No display window (a file written by a tool that only set data
    // windows). Fall back to part 0's data window, which is the canvas for
    // every file this module writes.
    result.document.width = part0.width;
    result.document.height = part0.height;
    result.warnings.push_back(
        "'" + path +
        "' has no display window; the canvas size was taken from part 0's data window "
        "instead.");
  }
  if (read.hasChromaticities) {
    Primaries& p = result.document.workingSpace.primaries;
    p.redX = read.chromaticities[0];
    p.redY = read.chromaticities[1];
    p.greenX = read.chromaticities[2];
    p.greenY = read.chromaticities[3];
    p.blueX = read.chromaticities[4];
    p.blueY = read.chromaticities[5];
    p.whiteX = read.chromaticities[6];
    p.whiteY = read.chromaticities[7];
  }
  result.composite = decodeCompositePart(part0);
  if (!result.composite.valid()) {
    result.warnings.push_back(
        "part 0 is " + std::to_string(part0.channelNames.size()) + "-channel " +
        part0.sampleTypeName +
        "; this reader decodes the composite for inspection only from 4-channel half or float "
        "parts, so no composite preview was produced. The document itself is unaffected -- it "
        "is reconstructed from the layer parts, never from part 0.");
  }

  // Filled by the `np:comps` branch below and applied after the layer loop;
  // see there for why it cannot be applied in place.
  LayerCompCarrier comps;
  bool compsDecoded = false;
  bool compsCarried = false;

  for (const NpaintAttribute& a : part0.attributes) {
    if (a.name == kAttrVersion && a.type == NpaintAttribute::Type::Int) {
      result.carry.sourceVersion = a.intValue;
      if (a.intValue > kNpaintFormatVersion) {
        result.warnings.push_back(
            "'" + path + "' declares np:version " + std::to_string(a.intValue) +
            ", newer than this build's " + std::to_string(kNpaintFormatVersion) +
            ". It was opened anyway -- that is what PRD I10's verbatim carry-through of "
            "unrecognised attributes and parts exists for -- but saving it will stamp "
            "np:version " + std::to_string(kNpaintFormatVersion) +
            ", because the file produced will be one this build wrote.");
      }
      continue;
    }
    if (a.name == kAttrBasis && a.type == NpaintAttribute::Type::String) {
      // Both halves of the pair, from the same string (io/NpaintFile.hpp,
      // NpaintCarry::basis): the carry records what the *file* declared, and
      // the document records what *its own* latents are. PRD C8's "the file
      // records which pigment basis produced them" is only half a promise
      // until the reader puts it back on the document -- otherwise every
      // reader relabels what it opens.
      //
      // An empty `np:basis` cannot arrive here (this OpenImageIO drops empty
      // string attributes), and an absent one leaves both at their defaults:
      // an empty carry, and this build's basis on the document, which is what
      // the writer would have stamped anyway.
      result.carry.basis = a.stringValue;
      if (!a.stringValue.empty()) result.document.pigmentBasis = a.stringValue;
      if (a.stringValue != kNpaintPigmentBasis) {
        result.warnings.push_back(
            "'" + path + "' declares np:basis \"" + a.stringValue + "\", not this build's \"" +
            kNpaintPigmentBasis +
            "\". Any pigment latents in this file were fitted in that basis, so their "
            "pig.c0/c1/c2 name different pigments from the ones this build's Mixbox model "
            "would; the document was opened anyway and keeps that basis verbatim, so saving "
            "it writes np:basis \"" + a.stringValue +
            "\" back rather than relabelling it. Painting pigment into this document would "
            "mix two bases under one label, which is the case docs/document-format.md §3.3 "
            "names and the case a save refuses.");
      }
      continue;
    }
    if (a.name == kAttrTileSize && a.type == NpaintAttribute::Type::Int) {
      if (a.intValue != kTileSize) {
        result.warnings.push_back(
            "'" + path + "' declares np:tileSize " + std::to_string(a.intValue) +
            ", but this build's kTileSize is " + std::to_string(kTileSize) +
            ". docs/document-format.md §6 says the two should match so a load is a direct read "
            "of the tiles needed; a mismatch is not fatal here because layer parts are read "
            "whole, but it means the on-disk layout is not this build's.");
      }
      continue;
    }
    // The document's layer comps (PLAN.md Phase 5 step 12). io/CompSerial owns
    // the encoding; this is the one place a `.npaint` reaches it. Decoded here
    // but **applied after the layer loop below**, because the payload names
    // layers by EXR part name and the part names are not known until the layer
    // parts have been read.
    if (a.name == kAttrComps) {
      std::string why;
      if (a.type != NpaintAttribute::Type::String) {
        compsCarried = true;
        result.warnings.push_back(
            "'" + path +
            "' has an np:comps attribute that is not a string; this build's layer-comp carrier "
            "is a hex `string` (io/CompSerial), so the value could not be decoded. The document "
            "opened with no comps and the attribute is written back unchanged (PRD I10).");
      } else if (deserializeLayerComps(a.stringValue, &comps, &why)) {
        compsDecoded = true;
      } else {
        compsCarried = true;
        // Carried, not discarded: a comp list this build cannot read -- a newer
        // `npcomps2:`, most likely -- is exactly the case PRD I10's verbatim
        // preservation exists for.
        result.warnings.push_back("'" + path + "': " + why);
      }
      if (!compsCarried) continue;
    }
    if (isDocumentAttributeRecognised(a.name) && !(compsCarried && a.name == kAttrComps))
      continue;  // recognised name, unexpected type
    result.carry.documentAttributes.push_back(a);
  }

  // --- Every other part: a Layer, or carried verbatim --------------------
  for (size_t i = 1; i < read.parts.size(); ++i) {
    const NpaintRawPart& part = read.parts[i];

    // The recognition test, deliberately strict in the reader's own
    // disfavour: anything this build only half-understands is carried
    // whole rather than turned into a Layer whose missing half is gone.
    const NpaintAttribute* kind = findAttr(part.attributes, kAttrKind);
    const bool rgbaHalf = part.sampleTypeName == "half" && part.channelNames.size() == 4 &&
                          part.channelNames[0] == "R" && part.channelNames[1] == "G" &&
                          part.channelNames[2] == "B" && part.channelNames[3] == "A";
    const bool tileAligned = part.width > 0 && part.height > 0 && part.width % kTileSize == 0 &&
                             part.height % kTileSize == 0 && part.x % kTileSize == 0 &&
                             part.y % kTileSize == 0;
    const bool isHalf = part.sampleTypeName == "half";
    // A Pigment part: `np:kind = "Pigment"` and, by *name*, every one of
    // docs/document-format.md's eleven channels in half -- twelve when the
    // layer carries a mask. Matched by name and not by position for the reason
    // kPigmentChannelNames gives.
    const std::optional<std::vector<size_t>> pigmentIdx =
        (isHalf && part.channelNames.size() == kPigmentChannelCount)
            ? channelIndicesByName(part, pigmentChannelNames(false))
            : std::nullopt;
    // The masked variants. One extra channel, matched by name like the rest;
    // an unmasked layer's part is unchanged, which is what keeps a mask-free
    // document's bytes identical to what this module wrote before this step.
    const std::optional<std::vector<size_t>> rgbaMaskIdx =
        (isHalf && part.channelNames.size() == 5)
            ? channelIndicesByName(part, rgbaChannelNames(true))
            : std::nullopt;
    const std::optional<std::vector<size_t>> pigmentMaskIdx =
        (isHalf && part.channelNames.size() == kPigmentChannelCount + 1)
            ? channelIndicesByName(part, pigmentChannelNames(true))
            : std::nullopt;
    // An Adjustment part: exactly one channel, named `mask`, in half. See
    // buildAdjustmentLayerPart() on why the channel is unconditional and why
    // `np:mask` -- not the channel's presence -- says whether the layer has a
    // mask.
    const bool adjustmentChannels =
        isHalf && part.channelNames.size() == 1 && part.channelNames[0] == kMaskChannelName;
    const bool namedKind = kind != nullptr && kind->type == NpaintAttribute::Type::String;
    const bool isRgbLayer = isLayerPartName(part.name) && namedKind &&
                            kind->stringValue == layerKindName(LayerKind::RGB) &&
                            (rgbaHalf || rgbaMaskIdx.has_value()) && tileAligned;
    const bool isPigmentLayer = isLayerPartName(part.name) && namedKind &&
                                kind->stringValue == layerKindName(LayerKind::Pigment) &&
                                (pigmentIdx.has_value() || pigmentMaskIdx.has_value()) &&
                                tileAligned;
    const bool isAdjustmentLayer = isLayerPartName(part.name) && namedKind &&
                                   kind->stringValue == layerKindName(LayerKind::Adjustment) &&
                                   adjustmentChannels && tileAligned;
    // A Group part: `np:kind = "group"` and Adjustment's own one-channel
    // shape -- `buildGroupLayerPart()` is a thin wrapper over
    // `buildAdjustmentLayerPart()`, so the reader's test is the identical
    // `adjustmentChannels` predicate under a different `np:kind`.
    const bool isGroupLayer = isLayerPartName(part.name) && namedKind &&
                              kind->stringValue == layerKindName(LayerKind::Group) &&
                              adjustmentChannels && tileAligned;
    // A Vector part: `np:kind = "Vector"` and the same one-channel shape --
    // `buildVectorLayerPart()` is a thin wrapper over
    // `buildAdjustmentLayerPart()`, so the reader's test is the identical
    // `adjustmentChannels` predicate under a third `np:kind`.
    const bool isVectorLayer = isLayerPartName(part.name) && namedKind &&
                               kind->stringValue == layerKindName(LayerKind::Vector) &&
                               adjustmentChannels && tileAligned;
    // A Text part: the same one-channel shape under a fourth `np:kind`.
    // `buildTextLayerPart()` is a third thin wrapper over
    // `buildAdjustmentLayerPart()`, so the reader's test is again the
    // identical `adjustmentChannels` predicate.
    const bool isTextLayer = isLayerPartName(part.name) && namedKind &&
                             kind->stringValue == layerKindName(LayerKind::Text) &&
                             adjustmentChannels && tileAligned;
    // An alpha channel part (PRD E11, E13): `S####`, `np:kind = "selection"`,
    // and exactly one HALF channel named `coverage`. Matched by name like every
    // other channel here even though there is only one of them -- a part whose
    // single channel is called something else is a part this build does not
    // understand, and carrying it is cheaper than guessing.
    const std::optional<std::vector<size_t>> coverageIdx =
        (isHalf && part.channelNames.size() == 1)
            ? channelIndicesByName(part, {kCoverageChannelName})
            : std::nullopt;
    const bool isAlphaChannel = isChannelPartName(part.name) && namedKind &&
                                kind->stringValue == kChannelKindName &&
                                coverageIdx.has_value() && tileAligned;

    if (isAlphaChannel) {
      if (part.tileWidth != kTileSize || part.tileHeight != kTileSize) {
        result.warnings.push_back(
            "channel part '" + part.name + "' is stored with " +
            std::to_string(part.tileWidth) + "x" + std::to_string(part.tileHeight) +
            " on-disk tiles rather than this build's " + std::to_string(kTileSize) +
            "; docs/document-format.md §6 asks that these match. The coverage is "
            "unaffected.");
      }

      AlphaChannel channel;
      const NpaintAttribute* nameAttr = findAttr(part.attributes, kAttrName);
      const std::string wanted =
          (nameAttr != nullptr && nameAttr->type == NpaintAttribute::Type::String)
              ? nameAttr->stringValue
              : std::string();

      // **A name collision is repaired on load, not refused, and this is the
      // one place the reader edits what it read.** A channel is looked up by
      // name, so two channels called "Alpha 1" make that lookup depend on list
      // order -- and `saveNpaint()` refuses such a document, which means a file
      // holding one (hand-built, or written by a tool that did not enforce it)
      // would open, accept edits, and never be writable again. This header's
      // own basis section calls that shape a trap and says why: "a refusal that
      // costs the user their work to protect a label has the trade backwards".
      //
      // So the *coverage* is preserved exactly and only the label moves, the
      // warning names both spellings, and the next save is possible. Same
      // treatment for a channel with no name at all, which cannot round-trip
      // through an EXR header anyway (an empty string attribute is dropped by
      // this OpenImageIO -- see NpaintAttribute).
      channel.name = uniqueChannelName(result.document, wanted);
      if (channel.name != wanted) {
        result.warnings.push_back(
            "channel part '" + part.name + "' declares np:name \"" + wanted +
            "\", which " + (wanted.empty() ? "is empty" : "another channel in this file already "
                                                          "uses") +
            ". It was opened as \"" + channel.name +
            "\" so the document stays saveable -- a channel is looked up by name, and a "
            "duplicate would make that lookup depend on list order. Its coverage is "
            "unchanged.");
      }

      const size_t clamped = unpackChannelPart(part, (*coverageIdx)[0], &channel.tiles);
      if (clamped != 0) {
        result.warnings.push_back(
            "channel part '" + part.name + "' (\"" + channel.name + "\") had " +
            std::to_string(clamped) +
            " coverage sample(s) outside [0,1] or NaN. They were clamped into range (NaN and "
            "negatives to 0.0, which is 'not selected'), and the clamped values are what the "
            "next save writes. Coverage weights every edit, so a bad sample here does not "
            "show as a wrong pixel -- it shows as an edit landing somewhere unexpected.");
      }

      result.carry.partOrder.push_back(
          NpaintPartSlot{NpaintPartSlot::Kind::Channel, result.document.channels.size()});
      result.document.channels.push_back(std::move(channel));
      continue;
    }

    if (!isRgbLayer && !isPigmentLayer && !isAdjustmentLayer && !isGroupLayer &&
        !isVectorLayer && !isTextLayer) {
      std::string reason;
      if (isChannelPartName(part.name)) {
        // An `S####` part that failed the test above. Worth its own sentence
        // rather than falling into "its name is not the L#### form", which
        // would be true and unhelpful.
        reason = "its name is the S#### form this build gives alpha channel parts, but it is "
                 "not one -- a channel part carries np:kind \"selection\" and exactly one "
                 "channel named coverage, in half, on a tile-aligned data window";
      } else if (!isLayerPartName(part.name)) {
        reason = "its name is not the L#### form this build gives layer parts";
      } else if (kind == nullptr) {
        reason = "it has no np:kind attribute";
      } else if (kind->type != NpaintAttribute::Type::String) {
        reason = "its np:kind attribute is not a string";
      } else if (kind->stringValue != layerKindName(LayerKind::RGB) &&
                 kind->stringValue != layerKindName(LayerKind::Pigment) &&
                 kind->stringValue != layerKindName(LayerKind::Adjustment) &&
                 kind->stringValue != layerKindName(LayerKind::Group) &&
                 kind->stringValue != layerKindName(LayerKind::Vector) &&
                 kind->stringValue != layerKindName(LayerKind::Text)) {
        reason = "its np:kind is \"" + kind->stringValue +
                 "\", and this build can only hold RGB, Pigment, Adjustment, group, Vector "
                 "and Text layers (see io/NpaintFile.hpp's deferrals)";
      } else if (kind->stringValue == layerKindName(LayerKind::Adjustment)) {
        reason = "it declares np:kind \"Adjustment\" but its channels are not exactly one "
                 "named mask, in half -- an Adjustment layer holds no pixels, so its part "
                 "carries only the one channel EXR requires it to have";
      } else if (kind->stringValue == layerKindName(LayerKind::Vector)) {
        reason = "it declares np:kind \"Vector\" but its channels are not exactly one named "
                 "mask, in half -- a Vector layer holds no pixels, so its part carries only "
                 "the one channel EXR requires it to have (buildVectorLayerPart() shares "
                 "Adjustment's shape), and its geometry lives in np:vector";
      } else if (kind->stringValue == layerKindName(LayerKind::Text)) {
        reason = "it declares np:kind \"Text\" but its channels are not exactly one named "
                 "mask, in half -- a Text layer holds no pixels either, so its part carries "
                 "only the one channel EXR requires it to have (buildTextLayerPart() shares "
                 "Adjustment's shape), and its text lives in np:text";
      } else if (kind->stringValue == layerKindName(LayerKind::Group)) {
        reason = "it declares np:kind \"group\" but its channels are not exactly one named "
                 "mask, in half -- a Group holds no pixels, so its part carries only the one "
                 "channel EXR requires it to have (buildGroupLayerPart() shares Adjustment's "
                 "shape)";
      } else if (kind->stringValue == layerKindName(LayerKind::Pigment)) {
        reason = "it declares np:kind \"Pigment\" but its channels are not exactly "
                 "docs/document-format.md's eleven -- R, G, B, A, pig.c0, pig.c1, pig.c2, "
                 "pig.m, res.R, res.G, res.B -- in half, with an optional twelfth named "
                 "mask";
      } else if (!rgbaHalf && !rgbaMaskIdx.has_value()) {
        reason = "its channels are not exactly R/G/B/A in half, with an optional fifth named "
                 "mask";
      } else {
        reason = "its data window (" + std::to_string(part.width) + "x" +
                 std::to_string(part.height) + " at " + std::to_string(part.x) + "," +
                 std::to_string(part.y) + ") is not aligned to this build's " +
                 std::to_string(kTileSize) + "-pixel tile grid";
      }
      result.warnings.push_back("part '" + part.name +
                                "' was not turned into a layer because " + reason +
                                ". It is carried verbatim and will be written back unchanged "
                                "on the next save (PRD I10).");
      if (part.tileWidth <= 0 || part.tileHeight <= 0) {
        result.warnings.push_back(
            "carried part '" + part.name +
            "' is scanline-stored. Saving this document will be refused for that reason -- "
            "this OpenImageIO cannot write a multi-part EXR mixing scanline and tiled parts, "
            "and every part this build writes is tiled (PRD I4). Said here, at load, rather "
            "than left to surface as an OpenEXR exception at save time.");
      }
      result.carry.partOrder.push_back(
          NpaintPartSlot{NpaintPartSlot::Kind::RawPart, result.carry.rawParts.size()});
      result.carry.rawParts.push_back(part);
      continue;
    }

    if (part.tileWidth != kTileSize || part.tileHeight != kTileSize) {
      result.warnings.push_back(
          "part '" + part.name + "' is stored with " + std::to_string(part.tileWidth) + "x" +
          std::to_string(part.tileHeight) +
          " on-disk tiles rather than this build's " + std::to_string(kTileSize) +
          "; docs/document-format.md §6 asks that these match so a load is a direct read of "
          "the tiles needed. The pixels are unaffected.");
    }

    Layer layer;
    // The `mask` channel's index in this part, when it has one. Engaging
    // `layer.mask` is decided by the **channel's presence**, not by whether it
    // holds anything: a mask that reveals everything is a mask the user added,
    // it is what `core::addLayerMask()` creates, and the layers panel says
    // `MASK` for it. Dropping it on load because it happens to be all-1.0
    // would make "add a mask, save, reopen" lose the mask (core/Mask.hpp
    // separates absent from all-1.0 for exactly this reason).
    size_t maskIdx = 0;
    bool hasMaskChannel = false;
    // Set when an `np:vector` attribute was present but could NOT be decoded,
    // so it must stay in the carry and be written back verbatim -- `opsCarried`
    // below is the same idea for `np:ops`, and the naming follows it.
    bool vectorCarried = false;
    bool textCarried = false;
    if (isAdjustmentLayer) {
      layer.kind = LayerKind::Adjustment;
      // No tile storage of any kind is engaged: that is the kind's definition,
      // not an omission. The one channel is the mask, and `np:mask` -- not its
      // presence -- decides whether this layer has one.
      const NpaintAttribute* hasMask = findAttr(part.attributes, kAttrMask);
      if (hasMask != nullptr && hasMask->type == NpaintAttribute::Type::Int &&
          hasMask->intValue != 0) {
        maskIdx = 0;
        hasMaskChannel = true;
      }
    } else if (isPigmentLayer) {
      layer.kind = LayerKind::Pigment;
      layer.pigmentTiles.emplace();
      const std::vector<size_t>& idx = pigmentIdx.has_value() ? *pigmentIdx : *pigmentMaskIdx;
      unpackPigmentLayerPart(part, idx, &*layer.pigmentTiles);
      if (pigmentMaskIdx.has_value()) {
        maskIdx = (*pigmentMaskIdx)[kPigmentChannelCount];
        hasMaskChannel = true;
      }
    } else if (isVectorLayer) {
      layer.kind = LayerKind::Vector;
      // Adjustment's own reading of the one channel: `np:mask`, not the
      // channel's presence, says whether the layer has one.
      const NpaintAttribute* hasMask = findAttr(part.attributes, kAttrMask);
      if (hasMask != nullptr && hasMask->type == NpaintAttribute::Type::Int &&
          hasMask->intValue != 0) {
        maskIdx = 0;
        hasMaskChannel = true;
      }
      // The geometry. A payload this build cannot decode -- a future
      // `npvec2:`, or a corrupt one -- is NOT an error and is NOT guessed at:
      // the layer comes back with no shapes and the attribute stays in the
      // carry, so saving writes it back verbatim (PRD I10). The warning names
      // what happened, because a Vector layer that silently arrived empty
      // would look like a lost document rather than a version gap.
      if (const NpaintAttribute* v = findAttr(part.attributes, kAttrVector);
          v != nullptr && v->type == NpaintAttribute::Type::String) {
        std::string why;
        if (!deserializeVectorShapes(v->stringValue, &layer.shapes, &layer.nextShapeId,
                                     &why)) {
          vectorCarried = true;
          result.warnings.push_back("part '" + part.name + "': " + why);
        }
      } else if (v != nullptr) {
        vectorCarried = true;
        result.warnings.push_back(
            "part '" + part.name +
            "' has an np:vector attribute that is not a string; this build's geometry carrier "
            "is a hex `string` (io/PathSerial), so the value could not be decoded. The layer "
            "opened with no shapes and the attribute is written back unchanged (PRD I10).");
      }
    } else if (isTextLayer) {
      layer.kind = LayerKind::Text;
      // Adjustment's reading of the one channel, a third time.
      const NpaintAttribute* hasMask = findAttr(part.attributes, kAttrMask);
      if (hasMask != nullptr && hasMask->type == NpaintAttribute::Type::Int &&
          hasMask->intValue != 0) {
        maskIdx = 0;
        hasMaskChannel = true;
      }
      // The text block, on `np:vector`'s rules: a future `nptext2:` or a
      // corrupt payload is NOT an error and is NOT guessed at, the layer comes
      // back with a default `TextContent`, and the attribute stays in the
      // carry so saving writes it back verbatim (PRD I10).
      //
      // **`textCarried` is set on the ABSENT case too, unlike `np:vector`.**
      // A Text part with no `np:text` at all is a part this build wrote in an
      // impossible state or a part something else wrote -- either way the
      // layer has no content, and the warning is the only thing that
      // distinguishes "an empty text layer" from "a text layer whose content
      // did not survive". A missing `np:vector` is silent because an empty
      // shape list is exactly what this build writes for an empty Vector
      // layer; a missing `np:text` is not, because this build always writes
      // one for a Text layer.
      if (const NpaintAttribute* t = findAttr(part.attributes, kAttrText);
          t != nullptr && t->type == NpaintAttribute::Type::String) {
        std::string why;
        if (!deserializeTextContent(t->stringValue, &layer.text, &why)) {
          textCarried = true;
          result.warnings.push_back("part '" + part.name + "': " + why);
        }
      } else if (t != nullptr) {
        textCarried = true;
        result.warnings.push_back(
            "part '" + part.name +
            "' has an np:text attribute that is not a string; this build's text carrier is a "
            "hex `string` (io/TextSerial), so the value could not be decoded. The layer "
            "opened empty and the attribute is written back unchanged (PRD I10).");
      } else {
        result.warnings.push_back(
            "part '" + part.name +
            "' declares np:kind \"Text\" but carries no np:text attribute, so there is no "
            "text to read. The layer opened empty.");
      }
    } else if (isGroupLayer) {
      layer.kind = LayerKind::Group;
      // Adjustment's own reading of the one channel: `np:mask`, not the
      // channel's presence, says whether the layer has one.
      const NpaintAttribute* hasMask = findAttr(part.attributes, kAttrMask);
      if (hasMask != nullptr && hasMask->type == NpaintAttribute::Type::Int &&
          hasMask->intValue != 0) {
        maskIdx = 0;
        hasMaskChannel = true;
      }
      // The group's own stable identity, read back verbatim -- no
      // translation, `kAttrGroupId`'s own comment. A Group part with no
      // `np:groupId` at all (carried from a hand-built fixture, say) reads
      // as an empty tag, which `core::groupAncestry()` treats as "resolves
      // to nothing" for any member that happened to name it, the same
      // "absent means neutral" rule as an unresolvable `np:parent`.
      if (const NpaintAttribute* g = findAttr(part.attributes, kAttrGroupId);
          g != nullptr && g->type == NpaintAttribute::Type::String) {
        layer.groupTag = g->stringValue;
      }
    } else {
      layer.kind = LayerKind::RGB;
      layer.rgbTiles.emplace();
      if (rgbaHalf) {
        unpackLayerPart(part, &*layer.rgbTiles);
      } else {
        unpackLayerPartStrided(part, *rgbaMaskIdx, &*layer.rgbTiles);
        maskIdx = (*rgbaMaskIdx)[4];
        hasMaskChannel = true;
      }
    }
    if (hasMaskChannel) {
      layer.mask.emplace();
      const size_t clamped = unpackMaskChannel(part, maskIdx, &*layer.mask);
      if (clamped > 0) {
        // PRD I11's discipline applied to the read side: a value this build
        // had to change is named with a count, never absorbed silently. A mask
        // is the one channel where a bad sample makes a layer disappear, so the
        // failure mode this warning prevents is "the layer went black and
        // nothing said why".
        result.warnings.push_back(
            "part '" + part.name + "' has " + std::to_string(clamped) +
            " mask sample(s) that are NaN or outside [0, 1]; a layer mask is a coverage, so "
            "each was clamped into [0, 1] (NaN to 0) exactly as core::layerCoverage() clamps "
            "an out-of-range np:opacity. The clamped values are what this document now holds "
            "and what the next save will write.");
      }
    }

    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrName);
        a && a->type == NpaintAttribute::Type::String)
      layer.name = a->stringValue;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrBlend);
        a && a->type == NpaintAttribute::Type::String)
      layer.blend = a->stringValue;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrOpacity);
        a && a->type == NpaintAttribute::Type::Float)
      layer.opacity = a->floatValue;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrVisible);
        a && a->type == NpaintAttribute::Type::Int)
      layer.visible = a->intValue != 0;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrLocked);
        a && a->type == NpaintAttribute::Type::Int)
      layer.locked = a->intValue != 0;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrParent);
        a && a->type == NpaintAttribute::Type::String)
      layer.parent = a->stringValue;
    // PLAN.md Phase 5 step 9. Absent means `false`, which is the default the
    // member already has, so a `.npaint` written before this step loads with
    // no clipped layers without the reader having to know that. A clipped
    // *bottom* layer is not refused here: the flag is carried exactly as
    // written (PRD I10) and core/Composite composites the layer unclipped and
    // warns by name, because refusing on load would make a preserved attribute
    // the thing that makes a file unopenable.
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrClipped);
        a && a->type == NpaintAttribute::Type::Int)
      layer.clipped = a->intValue != 0;
    // Absent means `false`, `Layer::alphaLocked`'s own default, `np:clipped`'s
    // own reason: a `.npaint` written before this step loads with no
    // alpha-locked layers without the reader having to know that. Carried
    // verbatim even on a Pigment layer (PRD I10) -- `core::setLayerAlphaLocked()`
    // refuses to *set* it there, but a foreign or hand-edited file that already
    // holds the combination is not refused on load, matching `np:clipped`'s own
    // "a preserved attribute must never be the thing that makes a file
    // unopenable".
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrAlphaLocked);
        a && a->type == NpaintAttribute::Type::Int)
      layer.alphaLocked = a->intValue != 0;
    // PLAN.md Phase 5 step 11 / PRD C15. Absent means each member's own default
    // -- unlabelled, unlinked -- so a `.npaint` written before this step loads
    // with neither, without the reader having to know that. The label is taken
    // **verbatim**, including a name this build has no swatch for (PRD I10);
    // app/LayerPanel shows an unknown name as text rather than mapping it onto
    // some default colour. A negative `np:link` -- which this build never
    // writes -- is ignored rather than carried, because `Layer::linkGroup` is
    // unsigned and a negative number has no group it could mean.
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrLabel);
        a && a->type == NpaintAttribute::Type::String)
      layer.colorLabel = a->stringValue;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrLink);
        a && a->type == NpaintAttribute::Type::Int && a->intValue > 0)
      layer.linkGroup = static_cast<uint64_t>(a->intValue);

    // The per-layer op stack (PLAN.md Phase 5 step 5). io/OpSerial owns the
    // encoding; this is the one place a `.npaint` reaches it.
    bool opsCarried = false;
    if (const NpaintAttribute* a = findAttr(part.attributes, kAttrOps)) {
      std::string why;
      if (a->type != NpaintAttribute::Type::String) {
        opsCarried = true;
        result.warnings.push_back(
            "part '" + part.name +
            "' has an np:ops attribute that is not a string; this build's op-stack carrier is "
            "a hex `string` (io/OpSerial), so the value could not be decoded. The layer opened "
            "with no op stack and the attribute is written back unchanged (PRD I10).");
      } else if (!deserializeOpStack(a->stringValue, &layer.ops, &why)) {
        opsCarried = true;
        // Carried, not discarded: an op stack this build cannot read is
        // exactly the case PRD I10's verbatim preservation exists for, and it
        // is the whole content of an Adjustment layer.
        result.warnings.push_back("part '" + part.name + "': " + why);
      }
    }

    std::vector<NpaintAttribute> unknown;
    for (const NpaintAttribute& a : part.attributes) {
      if (isLayerAttributeRecognised(a.name) && !(opsCarried && a.name == kAttrOps) &&
          !(vectorCarried && a.name == kAttrVector) && !(textCarried && a.name == kAttrText))
        continue;
      unknown.push_back(a);
    }

    result.carry.partOrder.push_back(
        NpaintPartSlot{NpaintPartSlot::Kind::Layer, result.document.layers.size()});
    result.carry.layerPartNames.push_back(part.name);
    result.carry.layerAttributes.push_back(std::move(unknown));
    result.document.layers.push_back(std::move(layer));
  }

  // --- Layer comps, now that the part names are known --------------------
  //
  // The payload joins the format's stable layer id (the part name) to the
  // in-memory one a comp entry carries, so this cannot run until
  // `carry.layerPartNames` is populated. **The join is by name and never by
  // position**, for io/CompSerial.hpp's stated reason: a part this build
  // carries verbatim rather than turning into a `Layer` makes position N in
  // the file and position N in `Document::layers` different layers.
  //
  // A part name in the table with no layer here is not an error and not even a
  // warning -- it is what a carried foreign part looks like from this side, and
  // the comp entries that name its id simply do not match, which
  // `core::restoreLayerComp()` already reports with numbers when the comp is
  // used. A layer whose part name is *absent* from the table keeps id 0 and is
  // therefore in no comp, which is the correct answer for a layer written
  // before the comps were captured.
  if (compsDecoded) {
    result.document.comps = std::move(comps.comps);
    result.document.nextLayerId = comps.nextLayerId;
    for (size_t i = 0; i < result.document.layers.size(); ++i) {
      const std::string& partName = result.carry.layerPartNames[i];
      for (const std::pair<std::string, uint64_t>& entry : comps.layerIds) {
        if (entry.first != partName) continue;
        result.document.layers[i].id = entry.second;
        break;
      }
    }
    // The counter is raised past anything the table actually assigned, so a
    // file whose `nextLayerId` disagrees with its own ids -- written by hand,
    // or by a tool that edited one and not the other -- still cannot re-issue a
    // live layer's id. `core::normalizeLayerIds()` holds the same invariant
    // from the other end.
    for (const Layer& layer : result.document.layers)
      if (layer.id >= result.document.nextLayerId) result.document.nextLayerId = layer.id + 1;
  }

  if (result.document.layers.empty()) {
    result.warnings.push_back(
        "'" + path +
        "' contains no layer parts this build recognises, so the document opened with no "
        "layers. A plain single-part EXR is a flattened image, not a `.npaint` document -- "
        "io/ImageIO's openImageAsDocument() is the entry point for those.");
  }
  result.ok = true;
  return result;
}

}  // namespace np
