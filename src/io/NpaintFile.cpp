#include "io/NpaintFile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Half.hpp"
#include "core/Layer.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/Export.hpp"

// io/OiioBackend is the only translation unit that may include an
// OpenImageIO header, so this file reaches it the same guarded way
// io/Export.cpp and io/Capabilities.cpp already do. With NP_USE_OIIO=OFF the
// include disappears, the two entry points below take their refusal branch,
// and nothing in this file names an OpenImageIO symbol.
#if defined(NP_USE_OIIO)
#include "io/OiioBackend.hpp"
#endif

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

constexpr const char* kAttrKind = "np:kind";
constexpr const char* kAttrName = "np:name";
constexpr const char* kAttrBlend = "np:blend";
constexpr const char* kAttrOpacity = "np:opacity";
constexpr const char* kAttrVisible = "np:visible";
constexpr const char* kAttrLocked = "np:locked";
constexpr const char* kAttrParent = "np:parent";

constexpr const char* kCompositePartName = "composite";

bool isDocumentAttributeRecognised(const std::string& name) {
  return name == kAttrVersion || name == kAttrBasis || name == kAttrTileSize;
}

bool isLayerAttributeRecognised(const std::string& name) {
  return name == kAttrKind || name == kAttrName || name == kAttrBlend ||
         name == kAttrOpacity || name == kAttrVisible || name == kAttrLocked ||
         name == kAttrParent;
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

TileBounds occupiedTileBounds(const TileStore& tiles) {
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

// Packs a layer's tiles into one part's pixel buffer.
//
// **This is the function the "byte-identical, no conversion" claim lives
// or dies by**: it moves `uint16_t` half words out of core::Tile's own
// storage into the byte buffer OpenImageIO writes as TypeDesc::HALF. No
// float appears anywhere in it. A tile-aligned data window means each tile
// row is a contiguous run of 128*4 half words landing at a contiguous
// offset, so this is a memcpy per tile row.
NpaintRawPart buildLayerPart(const Layer& layer, const std::string& partName) {
  NpaintRawPart part;
  part.name = partName;
  part.channelNames = {"R", "G", "B", "A"};
  part.sampleTypeName = "half";
  part.tileWidth = kTileSize;
  part.tileHeight = kTileSize;

  const TileStore& tiles = *layer.rgbTiles;
  const TileBounds b = occupiedTileBounds(tiles);
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

  const size_t rowWords = static_cast<size_t>(part.width) * 4;
  part.rawPixels.assign(rowWords * static_cast<size_t>(part.height) * sizeof(uint16_t), 0);
  auto* words = reinterpret_cast<uint16_t*>(part.rawPixels.data());

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

// The one place the NP_USE_OIIO=OFF refusal is worded, so save and load say
// the same thing about the same cause. PRD I11's discipline and
// io/Export.cpp's house style: name the thing, name the reason, name the
// alternative.
[[maybe_unused]] std::string noBackendError(const char* verb) {
  return std::string("`.npaint` ") + verb +
         " requires OpenImageIO, and this binary was built without it (NP_USE_OIIO=OFF), so it "
         "contains no OpenEXR reader or writer at all. The native document format is a "
         "multi-part tiled OpenEXR file (docs/document-format.md; PRD I4) and there is "
         "deliberately no fallback: writing some other container under a `.npaint` name would "
         "produce a file this application could not read back, and reading one is impossible "
         "without an EXR parser. Rebuild with:  cmake -S . -B build-oiio -DNP_USE_OIIO=ON "
         "-DCMAKE_PREFIX_PATH=\"$HOME/.local/openimageio\"  . To move pixels in or out of this "
         "build instead, io/Export's exportDocumentToFile() writes PNG/JPEG/TGA/BMP with no "
         "optional dependency (PRD I1) -- flattened, without layers, metadata or above-white "
         "highlights.";
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
  // Deliberately before the backend gate below, so that a malformed request
  // is refused identically in both build configurations. A caller who fixes
  // the refusal this reports has fixed it everywhere; a caller told only
  // "this build has no OpenImageIO" would fix that, rebuild, and hit the
  // real problem second. It also means --selftest exercises every PRD I11
  // refusal in the NP_USE_OIIO=OFF build too -- PLAN.md §1.5's "an
  // unexercised build option is not a seam", applied to the refusals rather
  // than to the option.
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

  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& layer = doc.layers[i];
    if (layer.kind != LayerKind::RGB) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is a " + layerKindName(layer.kind) +
                  " layer, and this build has no on-disk representation for that kind -- "
                  "docs/document-format.md stores Pigment and Media layers as `pig.*`/`res.*` "
                  "latent channels and Strokes layers as an `np:dabs` blob, and core::Layer "
                  "has neither latent tiles nor a dab list yet (Phase 5 steps 3 and 4). "
                  "Saving would drop the layer entirely, so nothing was written. Remove the "
                  "layer, or convert it to an RGB layer, to save this document.");
    }
    if (!layer.rgbTiles.has_value()) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") is RGB-kind but has no tile storage at all (`rgbTiles` is absent), "
                  "which core/Layer.hpp's own contract says cannot happen for an RGB layer. "
                  "Nothing was written; this is a malformed document rather than an "
                  "unsupported one.");
    }
    if (!(layer.opacity >= 0.0f) || layer.opacity > 1.0f) {
      return fail("save refused: layer " + std::to_string(i) + " (\"" + layer.name +
                  "\") has opacity " + std::to_string(layer.opacity) +
                  ", outside [0, 1]. `np:opacity` is a float attribute every reader will act "
                  "on literally, so writing an out-of-range value would put a number in the "
                  "file that no reader -- including this one -- can honour.");
    }
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

#if !defined(NP_USE_OIIO)
  (void)path;
  return fail(noBackendError("save"));
#else
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
  composite.attributes.push_back(stringAttr(
      kAttrBasis, (carry && !carry->basis.empty()) ? carry->basis : kNpaintPigmentBasis));
  composite.attributes.push_back(intAttr(kAttrTileSize, kTileSize));
  if (carry) {
    // PRD I10, the write half. These are the np:* attributes the reader did
    // not recognise; they go back out exactly as they came in. The reader
    // guarantees none of them collides with a recognised name, so no
    // de-duplication is needed here -- and if one ever did, OpenImageIO's
    // last-write-wins would silently drop the one this build wrote, which is
    // why the split happens on the read side rather than here.
    for (const NpaintAttribute& a : carry->documentAttributes) {
      if (isDocumentAttributeRecognised(a.name)) continue;
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

  auto appendLayerPart = [&](size_t i) {
    const Layer& layer = doc.layers[i];
    NpaintRawPart part = buildLayerPart(layer, layerNames[i]);
    part.attributes.push_back(stringAttr(kAttrKind, layerKindName(layer.kind)));
    part.attributes.push_back(stringAttr(kAttrName, layer.name));
    part.attributes.push_back(stringAttr(kAttrBlend, layer.blend));
    part.attributes.push_back(floatAttr(kAttrOpacity, layer.opacity));
    part.attributes.push_back(intAttr(kAttrVisible, layer.visible ? 1 : 0));
    part.attributes.push_back(intAttr(kAttrLocked, layer.locked ? 1 : 0));
    part.attributes.push_back(stringAttr(kAttrParent, layer.parent));
    if (carry && i < carry->layerAttributes.size()) {
      for (const NpaintAttribute& a : carry->layerAttributes[i]) {
        if (isLayerAttributeRecognised(a.name)) continue;
        part.attributes.push_back(a);
      }
    }
    request.parts.push_back(std::move(part));
  };

  // Part order after part 0. The carried order is replayed first so a
  // carried part that sat *between* two layers stays between them -- see
  // NpaintPartSlot on why appending them at the end would be data loss in
  // the ordering rather than in the bytes.
  std::vector<bool> layerWritten(doc.layers.size(), false);
  std::vector<bool> rawWritten(carry ? carry->rawParts.size() : 0, false);
  if (carry) {
    for (const NpaintPartSlot& slot : carry->partOrder) {
      if (slot.kind == NpaintPartSlot::Kind::Layer) {
        if (slot.index >= doc.layers.size() || layerWritten[slot.index]) continue;
        layerWritten[slot.index] = true;
        appendLayerPart(slot.index);
      } else {
        if (slot.index >= carry->rawParts.size() || rawWritten[slot.index]) continue;
        rawWritten[slot.index] = true;
        request.parts.push_back(carry->rawParts[slot.index]);
      }
    }
  }
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (layerWritten[i]) continue;
    appendLayerPart(i);
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

  std::string error;
  if (!oiioWriteMultiPartExr(request, &error)) return fail(error);
  result.ok = true;
  result.partsWritten = static_cast<int32_t>(request.parts.size());
  return result;
#endif
}

NpaintLoadResult loadNpaint(const std::string& path) {
  NpaintLoadResult result;
#if !defined(NP_USE_OIIO)
  (void)path;
  result.error = noBackendError("load");
  return result;
#else
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
      result.carry.basis = a.stringValue;
      if (a.stringValue != kNpaintPigmentBasis) {
        result.warnings.push_back(
            "'" + path + "' declares np:basis \"" + a.stringValue + "\", not this build's \"" +
            kNpaintPigmentBasis +
            "\". Nothing is lost today -- this build writes no pigment latents, so no channel "
            "in the file depends on the basis -- and the value is preserved verbatim on save. "
            "It becomes a real refusal once Phase 5 makes latents real "
            "(docs/document-format.md §3.3 lists a basis mismatch among the things a save "
            "must name).");
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
    if (isDocumentAttributeRecognised(a.name)) continue;  // recognised name, unexpected type
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
    const bool isRgbLayer = isLayerPartName(part.name) && kind != nullptr &&
                            kind->type == NpaintAttribute::Type::String &&
                            kind->stringValue == layerKindName(LayerKind::RGB) && rgbaHalf &&
                            tileAligned;

    if (!isRgbLayer) {
      std::string reason;
      if (!isLayerPartName(part.name)) {
        reason = "its name is not the L#### form this build gives layer parts";
      } else if (kind == nullptr) {
        reason = "it has no np:kind attribute";
      } else if (kind->type != NpaintAttribute::Type::String) {
        reason = "its np:kind attribute is not a string";
      } else if (kind->stringValue != layerKindName(LayerKind::RGB)) {
        reason = "its np:kind is \"" + kind->stringValue +
                 "\", and this build can only hold RGB layers (see io/NpaintFile.hpp's "
                 "deferrals)";
      } else if (!rgbaHalf) {
        reason = "its channels are not exactly R/G/B/A in half";
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
    layer.kind = LayerKind::RGB;
    layer.rgbTiles.emplace();
    unpackLayerPart(part, &*layer.rgbTiles);

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

    std::vector<NpaintAttribute> unknown;
    for (const NpaintAttribute& a : part.attributes) {
      if (isLayerAttributeRecognised(a.name)) continue;
      unknown.push_back(a);
    }

    result.carry.partOrder.push_back(
        NpaintPartSlot{NpaintPartSlot::Kind::Layer, result.document.layers.size()});
    result.carry.layerPartNames.push_back(part.name);
    result.carry.layerAttributes.push_back(std::move(unknown));
    result.document.layers.push_back(std::move(layer));
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
#endif
}

}  // namespace np
