#include "io/PsdLayerSection.hpp"

#include "io/PsdBlendKeys.hpp"
#include "io/PsdLayerExtras.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>

#include "color/Space.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "core/VectorRaster.hpp"
#include "io/PackBits.hpp"
#include "io/PsdVectorWrite.hpp"

namespace np {
namespace {

// PSD's own per-channel compression words. Only RLE is written: raw would
// need a second framing (no row-count table) for no benefit on any real
// layer, and io/PackBits.hpp is explicit that a compressed row growing past
// its raw size is expected and must NOT trigger a per-row fallback, because
// the compression word is per channel.
constexpr uint16_t kCompressionRle = 1;

// Bit 1 SET means HIDDEN. See io/PsdLayerSection.hpp's own header for the
// evidence; the constant is spelled the same way io/PsdImport.cpp:496
// spells it so the two are greppable together.
constexpr uint8_t kFlagHidden = 0x02;

// The one flags bit every real file sets alongside it. Photoshop writes
// bit 3 ("obsolete" per Adobe's table, universally 1 in files written since
// Photoshop 5) on every record; psd-tools names it `pixel_data_irrelevant`'s
// neighbour and ignores it, and io/PsdImport.cpp ignores it too. Written
// because every real record carries it, not because anything reads it.
constexpr uint8_t kFlagObsolete = 0x08;

// A layer's straight, linear RGBA over one rectangle, plus what was lost
// getting there.
struct RectSamples {
  std::vector<float> straightRgba;  // width*height*4, row-major
  size_t clippedSamples = 0;        // linear values outside [0,1] before the clamp
};

// The tight bounding box of the tiles of `tiles` that hold at least one
// sample with alpha > 0, in document coordinates, half-open.
//
// Tiles rather than pixels: io/PsdLayerSection.hpp argues the granularity.
// "Occupied" means "has a visible sample", not "is allocated" -- a store can
// hold a tile whose every sample is transparent (nothing in this codebase
// writes one deliberately, but a copy-on-write share or a fully erased
// stroke can leave one), and treating that as occupied would give a fully
// transparent layer a non-empty rect and a screenful of zero channels.
struct Rect {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return right <= left || bottom <= top; }
};

Rect occupiedTileBounds(const TileStore& tiles) {
  bool any = false;
  int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;  // tile coordinates, inclusive
  for (const auto& [coord, tile] : tiles) {
    bool visible = false;
    for (int32_t y = 0; y < kTileSize && !visible; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        if (tile.readPixel(PixelCoord{x, y})[3] > 0.0f) {
          visible = true;
          break;
        }
      }
    }
    if (!visible) continue;
    if (!any) {
      any = true;
      minX = maxX = coord.x;
      minY = maxY = coord.y;
    } else {
      minX = std::min(minX, coord.x);
      maxX = std::max(maxX, coord.x);
      minY = std::min(minY, coord.y);
      maxY = std::max(maxY, coord.y);
    }
  }
  if (!any) return Rect{};
  return Rect{minX * kTileSize, minY * kTileSize, (maxX + 1) * kTileSize,
              (maxY + 1) * kTileSize};
}

// How many samples with alpha > 0 sit outside `rect`. Only ever called when
// the canvas clip actually narrowed the rect, so the cost is paid on the
// layers that are about to be warned about and on no others.
size_t visibleSamplesOutside(const TileStore& tiles, const Rect& rect) {
  size_t dropped = 0;
  for (const auto& [coord, tile] : tiles) {
    const int32_t ox = coord.x * kTileSize;
    const int32_t oy = coord.y * kTileSize;
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const int32_t dx = ox + x;
        const int32_t dy = oy + y;
        if (dx >= rect.left && dx < rect.right && dy >= rect.top && dy < rect.bottom) continue;
        if (tile.readPixel(PixelCoord{x, y})[3] > 0.0f) ++dropped;
      }
    }
  }
  return dropped;
}

// Un-premultiplies `rect`'s worth of tile samples into straight linear RGBA.
//
// The division is guarded at `a == 0`, where the straight colour is not
// merely imprecise but undefined: io/PsdImport.cpp's own `writeLayerPixelsAt`
// never writes a zero-alpha sample at all (a premultiplied {0,0,0,0} is
// indistinguishable from an absent tile), so the samples that come back out
// at alpha 0 are exactly the ones that were never written, and 0 is the
// value they went in as.
RectSamples readRectStraight(const TileStore& tiles, const Rect& rect) {
  RectSamples out;
  const size_t width = static_cast<size_t>(rect.right - rect.left);
  const size_t height = static_cast<size_t>(rect.bottom - rect.top);
  out.straightRgba.assign(width * height * 4, 0.0f);
  for (size_t row = 0; row < height; ++row) {
    for (size_t col = 0; col < width; ++col) {
      const PixelCoord doc{rect.left + static_cast<int32_t>(col),
                           rect.top + static_cast<int32_t>(row)};
      const Tile* tile = tiles.find(tileCoordAt(doc));
      if (tile == nullptr) continue;
      const std::array<float, 4> px = tile->readPixel(tileLocalOffset(doc));
      const float a = px[3];
      float* dst = &out.straightRgba[(row * width + col) * 4];
      dst[3] = a;
      if (a <= 0.0f) continue;
      const float inv = 1.0f / a;
      for (int ch = 0; ch < 3; ++ch) {
        const float straight = px[static_cast<size_t>(ch)] * inv;
        if (straight < 0.0f || straight > 1.0f) ++out.clippedSamples;
        dst[ch] = straight;
      }
    }
  }
  return out;
}

// Linear -> sRGB-encoded 8-bit. The exact inverse of the `srgbDecode()`
// io/PsdImport.cpp applies on the way in, with the clamp the 8-bit
// destination forces (counted by the caller, warned about by name).
uint8_t encodeColorByte(float linear) {
  const float encoded = srgbEncode(std::clamp(linear, 0.0f, 1.0f));
  return static_cast<uint8_t>(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

// Alpha is linear opacity and is never gamma-encoded -- io/PsdImport.cpp's
// decode passes it straight through, and this is that from the other side.
uint8_t encodeAlphaByte(float a) {
  return static_cast<uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
}

// One channel's whole block: the `u16` compression word, then `height`
// big-endian `u16` compressed row lengths, then the rows.
//
// The row-count table is **per channel** here, unlike the merged Image Data
// Section where one table covers every channel -- io/PsdImport.cpp's
// `decodeChannelData()` reads it that way (it is handed a slice of exactly
// one channel's declared length and finds a table at the front of it).
std::vector<uint8_t> encodeChannelBlock(std::span<const uint8_t> samples, size_t width,
                                        size_t height) {
  std::vector<uint8_t> block;
  if (width == 0 || height == 0) return block;

  std::vector<std::vector<uint8_t>> rows;
  rows.reserve(height);
  size_t total = 0;
  for (size_t y = 0; y < height; ++y) {
    rows.push_back(encodePackBits(samples.subspan(y * width, width)));
    total += rows.back().size();
  }

  block.reserve(2 + height * 2 + total);
  block.push_back(static_cast<uint8_t>((kCompressionRle >> 8) & 0xFFu));
  block.push_back(static_cast<uint8_t>(kCompressionRle & 0xFFu));
  for (const std::vector<uint8_t>& row : rows) {
    block.push_back(static_cast<uint8_t>((row.size() >> 8) & 0xFFu));
    block.push_back(static_cast<uint8_t>(row.size() & 0xFFu));
  }
  for (const std::vector<uint8_t>& row : rows)
    block.insert(block.end(), row.begin(), row.end());
  return block;
}

std::string quoted(const std::string& name) {
  return "'" + (name.empty() ? std::string("(unnamed)") : name) + "'";
}

// What each kind loses on the way into an ordinary raster PSD layer, and
// nothing about whether it has pixels -- the caller pairs this with the
// "and it has no raster to fall back on" half.
const char* lostContentFor(LayerKind kind) {
  switch (kind) {
    case LayerKind::Pigment: return "its pigment latents";
    case LayerKind::Media: return "its medium state";
    case LayerKind::Strokes: return "its dab records";
    case LayerKind::Adjustment: return "its op stack";
    case LayerKind::Text: return "its editability (it is pixels, not a 'TySh' type layer)";
    case LayerKind::Flats: return "its fill table and adjacency";
    // **Vector is deliberately absent from this table.** Every other kind
    // here loses its parametric content on the way into PSD; a Vector layer
    // does not, since docs/psd-vector-shapes.md's S1 closed -- its geometry
    // and fill go out as a real `vsms` + `SoCo` shape layer
    // (`attachVectorShape()` below). What it *can* still lose is per-layer
    // and per-shape rather than per-kind (a second shape, a stroke, a clip),
    // so those are named there, with numbers, instead of by one sentence
    // that would be wrong for the common case.
    case LayerKind::Vector: return "";
    case LayerKind::RGB: return "";
    case LayerKind::Group: return "";
  }
  return "";
}

// A Vector layer's rasterised cache, or an empty store.
//
// **The same `rasterizeVectorLayer()` the compositor reaches through
// `MaterializedDocument`**, called directly rather than through it: that class
// rewrites Text, Flats and Strokes layers too, and giving those a raster here
// would change what three other kinds export as a side effect of fixing S1.
// What matters for agreement is that this is the one rasteriser -- the merged
// composite comes from `flattenDocumentToLinear()`, which materialises through
// the same function, so the record's pixels and the file's preview cannot
// disagree about what the shape looks like.
TileStore rasterizeVectorCache(const Layer& layer, const Document& doc) {
  if (layer.shapes.empty()) return TileStore{};
  return rasterizeVectorLayer(layer.shapes, doc.gradients, doc.width, doc.height);
}

// Appends this layer's `lclr` block, or nothing. Section 3 of
// io/PsdLayerExtras.hpp argues both halves of "or nothing".
void attachColorLabel(const Layer& layer, PsdLayerRecord& rec,
                      std::vector<std::string>& warnings) {
  if (layer.colorLabel.empty()) return;  // no label, and therefore no block
  uint16_t index = 0;
  if (!psdLayerColorLabelIndex(layer.colorLabel, index)) {
    warnings.push_back("layer " + quoted(layer.name) + ": its colour label '" +
                       layer.colorLabel +
                       "' is not one of the seven PSD has an index for, so no sheet colour is "
                       "written -- a guessed index would put a colour on the layer that "
                       "nobody chose.");
    return;
  }
  PsdWriter w;
  writePsdTaggedBlock(w, "lclr", encodePsdLclrBlock(index));
  const std::vector<uint8_t>& bytes = w.bytes();
  if (w.ok()) rec.extraBlocks.insert(rec.extraBlocks.end(), bytes.begin(), bytes.end());
}

// Appends this layer's `vsms` (geometry) and `SoCo` (fill colour) blocks --
// docs/psd-vector-shapes.md S1's editable half. The raster half is the
// ordinary channel data every record carries, fed from
// `rasterizeVectorCache()` above.
//
// **Everything this cannot carry is named with a number**, rather than by the
// one blanket "its Bezier geometry could not be carried" sentence that stood
// here before S1 closed and that is now false for the common case. A PSD shape
// layer is exactly one path and one fill; a `core::Layer` is a list of shapes
// each with a fill, a stroke, a stroke style and an optional clip.
void attachVectorShape(const Layer& layer, const Document& doc, PsdLayerRecord& rec,
                       std::vector<std::string>& warnings) {
  const std::string named = "layer " + quoted(layer.name) + " (Vector)";
  if (layer.shapes.empty()) {
    warnings.push_back(named +
                       ": it holds no shapes at all, so it exports as an empty layer record.");
    return;
  }

  const VectorShape& shape = layer.shapes.front();
  const PsdVectorShapeMask mask =
      encodePsdVectorShapeMask(shape.path, doc.width, doc.height);
  if (mask.bytes.empty()) {
    warnings.push_back(named +
                       ": its first shape's path has no subpath of two or more anchors, so it "
                       "encloses nothing PSD can carry and no vector outline is written; the "
                       "layer exports as its rasterised copy alone.");
    return;
  }

  PsdWriter w;
  writePsdTaggedBlock(w, "vsms", mask.bytes);
  // **No `SoCo` for a fill that is off.** `Paint::on == false` is genuinely
  // "no fill", and PSD carries that only in `vstk`'s `fillEnabled` flag,
  // which this build does not write -- so writing a black `SoCo` would paint
  // a shape nobody authored, exactly the `PNG/4 - Layer.png` trap
  // io/PsdVectorStyle.hpp names from the reading side.
  if (shape.fill.on)
    writePsdTaggedBlock(w, "SoCo", encodePsdSolidColorBlock(shape.fill.rgba));
  if (!w.ok()) {
    warnings.push_back(named +
                       ": the byte writer rejected a field while framing its vector blocks, so "
                       "none were written; the layer exports as its rasterised copy alone.");
    return;
  }
  const std::vector<uint8_t>& bytes = w.bytes();
  rec.extraBlocks.insert(rec.extraBlocks.end(), bytes.begin(), bytes.end());

  if (layer.shapes.size() > 1)
    warnings.push_back(named + ": a PSD shape layer carries exactly one path, so " +
                       std::to_string(layer.shapes.size() - 1) + " of its " +
                       std::to_string(layer.shapes.size()) +
                       " shapes are in the rasterised copy written beside the geometry and are "
                       "not editable in PSD.");
  if (mask.subpathsWritten < shape.path.subpaths.size())
    warnings.push_back(named + ": " +
                       std::to_string(shape.path.subpaths.size() - mask.subpathsWritten) +
                       " of its first shape's subpath(s) have fewer than two anchors, enclose "
                       "no area, and were not written.");
  if (mask.saturatedCoords > 0)
    warnings.push_back(named + ": " + std::to_string(mask.saturatedCoords) +
                       " path coordinate(s) lie more than 128 canvas-widths off the document "
                       "and were clamped to the limit of PSD's 8.24 fixed-point field.");
  if (!shape.fill.on)
    warnings.push_back(named +
                       ": its first shape has no fill, and PSD records 'no fill' only in a "
                       "'vstk' descriptor this build does not write -- so a reader that decides "
                       "a layer is a shape layer by looking for a fill block will take the "
                       "rasterised copy instead of the geometry.");
  else if (shape.fill.rgba[3] < 1.0f)
    warnings.push_back(named +
                       ": its fill colour's alpha is not carried -- PSD's shape colour has no "
                       "alpha field, so the shape exports fully opaque and the layer's own "
                       "opacity is the only transparency in the file.");
  if (shape.stroke.on)
    warnings.push_back(named +
                       ": its stroke is not carried into the shape layer (that needs a 'vstk' "
                       "descriptor this build does not write); it is drawn in the rasterised "
                       "copy beside the geometry.");
  if (shape.clip.has_value())
    warnings.push_back(named +
                       ": its clip path is not carried into the shape layer; it is applied in "
                       "the rasterised copy beside the geometry.");
}

}  // namespace

const char* psdBlendKeyFor(BlendMode mode, bool& exactMatch) {
  // Wired at gather to io/PsdBlendKeys' shared table -- the same 26 rows
  // io/PsdImport reads on the way IN, so a file this build writes and reads
  // back cannot disagree with itself about what a mode is called. The
  // three-row stub this replaced is gone; see io/PsdBlendKeys.hpp for why
  // one table read in both directions is the point.
  //
  // `nullptr` from that table means this build has no Photoshop key for the
  // mode at all -- today exactly `BlendMode::Mix`, the Kubelka-Munk latent
  // lerp Photoshop has no concept of. That becomes `"norm"` here with
  // `exactMatch` false, which is what makes the caller name the layer in a
  // warning rather than substituting silently.
  const char* key = np::psdBlendKeyFor(mode);
  exactMatch = key != nullptr;
  return exactMatch ? key : "norm";
}

bool buildPsdLayerRecord(const Layer& layer, const Document& doc, PsdLayerRecord& out,
                         std::vector<std::string>& warnings) {
  if (layer.kind == LayerKind::Group) {
    warnings.push_back("layer " + quoted(layer.name) +
                       ": a layer group's structure is not carried into this PSD -- its "
                       "members are written as ordinary top-level layers.");
    return false;
  }

  out = PsdLayerRecord{};
  out.name = layer.name;
  out.opacity = static_cast<uint8_t>(
      std::lround(std::clamp(layer.opacity, 0.0f, 1.0f) * 255.0f));
  out.clipped = layer.clipped;
  // The one inverted bit. See this module's header.
  out.hidden = !layer.visible;

  bool exactBlend = false;
  const std::optional<BlendMode> mode = blendModeFromName(layer.blend);
  if (!mode.has_value()) {
    // A name outside core/Blend's own set -- core/Blend.hpp is explicit that
    // `Layer::blend` can hold one (a newer build's mode read from a
    // `.npaint`). Normal, and named, rather than guessed at.
    out.blendKey = "norm";
    warnings.push_back("layer " + quoted(layer.name) + ": blend mode '" + layer.blend +
                       "' is not a mode this build knows; written as Normal.");
  } else {
    out.blendKey = psdBlendKeyFor(*mode, exactBlend);
    if (!exactBlend)
      warnings.push_back("layer " + quoted(layer.name) + ": blend mode '" + layer.blend +
                         "' has no PSD key in this landing and was written as Normal.");
  }

  if (layer.mask.has_value())
    warnings.push_back("layer " + quoted(layer.name) +
                       ": its layer mask is not carried into this PSD.");

  // **A Vector layer's pixels are not on the layer.** core/VectorRaster.hpp
  // section 1 argues at length why a Vector layer stores geometry and no
  // tiles, so the raster that goes in this record's channel data is built
  // here -- the Maximize-Compatibility copy that keeps the artwork visible to
  // every reader that does not understand `vsms`.
  TileStore vectorRaster;
  if (layer.kind == LayerKind::Vector) vectorRaster = rasterizeVectorCache(layer, doc);
  const TileStore* rasterSource =
      layer.rgbTiles.has_value()
          ? &*layer.rgbTiles
          : (layer.kind == LayerKind::Vector ? &vectorRaster : nullptr);

  const char* lost = lostContentFor(layer.kind);
  const bool hasRaster = rasterSource != nullptr;
  if (lost[0] != '\0') {
    if (hasRaster) {
      warnings.push_back("layer " + quoted(layer.name) + " (" + layerKindName(layer.kind) +
                         "): " + lost + " could not be carried into PSD; it exports as its "
                         "rasterised RGB.");
    } else {
      warnings.push_back("layer " + quoted(layer.name) + " (" + layerKindName(layer.kind) +
                         "): " + lost +
                         " could not be carried into PSD, and this build has no raster for "
                         "this kind to fall back on -- the layer exports EMPTY.");
    }
  }

  Rect rect;
  RectSamples samples;
  if (hasRaster) {
    const TileStore& tiles = *rasterSource;
    const Rect bounds = occupiedTileBounds(tiles);
    if (!bounds.empty()) {
      rect.left = std::max(bounds.left, 0);
      rect.top = std::max(bounds.top, 0);
      rect.right = std::min(bounds.right, doc.width);
      rect.bottom = std::min(bounds.bottom, doc.height);
      if (rect.empty()) rect = Rect{};
      // Only pay for the off-canvas scan when the clip actually bit.
      if (bounds.left < rect.left || bounds.top < rect.top || bounds.right > rect.right ||
          bounds.bottom > rect.bottom) {
        const size_t dropped = visibleSamplesOutside(tiles, rect);
        if (dropped > 0)
          warnings.push_back("layer " + quoted(layer.name) + ": " + std::to_string(dropped) +
                             " pixel(s) of content lie outside the canvas and were dropped "
                             "-- this writer clips every layer to the document bounds.");
      }
    }
    if (!rect.empty()) samples = readRectStraight(tiles, rect);
  }

  out.left = rect.left;
  out.top = rect.top;
  out.right = rect.right;
  out.bottom = rect.bottom;

  const size_t width = static_cast<size_t>(rect.right - rect.left);
  const size_t height = static_cast<size_t>(rect.bottom - rect.top);
  const size_t pixels = width * height;

  // Alpha FIRST, then R, G, B -- read off real Photoshop output, see this
  // module's header. Our own reader dispatches on the id and cannot tell.
  std::vector<uint8_t> plane(pixels);
  const std::array<int16_t, 4> ids{-1, 0, 1, 2};
  for (const int16_t id : ids) {
    for (size_t i = 0; i < pixels; ++i) {
      const float* px = &samples.straightRgba[i * 4];
      plane[i] = id < 0 ? encodeAlphaByte(px[3])
                        : encodeColorByte(px[static_cast<size_t>(id)]);
    }
    out.channels.push_back(PsdChannelBlock{id, encodeChannelBlock(plane, width, height)});
  }

  if (samples.clippedSamples > 0)
    warnings.push_back("layer " + quoted(layer.name) + ": " +
                       std::to_string(samples.clippedSamples) +
                       " colour sample(s) outside [0,1] in linear light were clipped by the "
                       "8-bit destination.");

  // The Additional Layer Information blocks, appended in the order Photoshop
  // writes them: the small fixed-shape ones first, then the descriptor-bearing
  // ones. Nothing reads them positionally -- io/PsdImport walks the region
  // block by block and dispatches on the key -- so the order is a convention
  // rather than a requirement.
  attachColorLabel(layer, out, warnings);
  if (layer.kind == LayerKind::Vector) attachVectorShape(layer, doc, out, warnings);

  return true;
}

void writePsdLayerRecord(PsdWriter& w, const PsdLayerRecord& rec) {
  w.i32(rec.top);
  w.i32(rec.left);
  w.i32(rec.bottom);
  w.i32(rec.right);

  w.u16(static_cast<uint16_t>(rec.channels.size()));
  for (const PsdChannelBlock& ch : rec.channels) {
    w.i16(ch.id);
    w.u32(static_cast<uint32_t>(ch.data.size()));
  }

  w.fourcc("8BIM");
  w.fourcc(rec.blendKey.c_str());  // a length other than 4 trips PsdWriter::ok()
  w.u8(rec.opacity);
  w.u8(rec.clipped ? 1u : 0u);
  w.u8(static_cast<uint8_t>(kFlagObsolete | (rec.hidden ? kFlagHidden : 0u)));
  w.u8(0);  // filler

  const size_t extraMarker = w.beginLengthU32();

  // Layer mask / adjustment data. Empty here in this landing: `u32 0`.
  w.u32(static_cast<uint32_t>(rec.maskBlock.size()));
  w.raw(rec.maskBlock);

  // Layer blending ranges. Always zero-length: this codebase has no
  // per-channel blend-range concept to carry, and an invented one would be
  // a claim rather than a translation.
  w.u32(0);

  // The legacy Pascal name, padded to 4 counting its own length byte.
  w.pascalString(rec.name, 4);

  // ...and the Unicode one. Photoshop writes both and prefers `luni` on
  // read, and so does io/PsdImport.cpp. `unicodeString()` writes a `u32`
  // code-unit count then that many UTF-16BE units, so the payload length is
  // always even and this block never needs a pad byte -- which matters
  // because io/PsdImport.cpp's additional-block walk has no way to tell a
  // pad byte from the start of the next block's signature.
  //
  // No trailing NUL is written inside the count. Photoshop writes one;
  // io/PsdImport.cpp strips trailing NULs on read specifically so a name
  // does not grow a NUL glyph per round trip, and psd-tools strips them
  // too. Writing one would be copying an artifact of Photoshop's own
  // internals into a file nothing needs it in.
  w.fourcc("8BIM");
  w.fourcc("luni");
  const size_t luniMarker = w.beginLengthU32();
  w.unicodeString(rec.name);
  w.endLengthU32(luniMarker);

  w.raw(rec.extraBlocks);

  w.endLengthU32(extraMarker);
}

void writePsdLayerChannelData(PsdWriter& w, const PsdLayerRecord& rec) {
  for (const PsdChannelBlock& ch : rec.channels) w.raw(ch.data);
}

namespace {

// The 20-byte mask record and its `-2` channel, from io/PsdLayerExtras.
//
// **One `psdMaskRect()` answer drives both**, which that module's header
// asks for by name: a record that writes the block but not the channel (or
// the reverse) declares a channel table that does not match its own extra
// data, and our own reader would walk one straight into the other.
//
// `writePsdMaskBlock()` emits the block's own `u32` length as well as its
// content, while `PsdLayerRecord::maskBlock` is content-only -- so the four
// length bytes are dropped here rather than the layout being written out a
// second time in this file. The size is asserted rather than assumed,
// because "strip a prefix" is exactly the kind of coupling that survives a
// change to the thing it strips.
void attachMask(const Layer& layer, PsdLayerRecord& rec) {
  PsdMaskRect mrect;
  if (!psdMaskRect(layer, mrect)) return;  // no mask, or a fully-revealed one

  PsdWriter mw;
  writePsdMaskBlock(mw, &mrect);
  const std::vector<uint8_t>& framed = mw.bytes();
  if (!mw.ok() || framed.size() != 24) return;  // 4 length + 20 content
  rec.maskBlock.assign(framed.begin() + 4, framed.end());

  // Channel -2 goes FIRST, ahead of the alpha channel, which is the order
  // io/PsdImport.cpp walks and the order real files carry.
  rec.channels.insert(rec.channels.begin(),
                      PsdChannelBlock{-2, encodePsdMaskChannel(layer, mrect)});
}

// One of the two synthetic records that bracket a group.
//
// Both are empty layers -- zero-size rect, and four channels carrying
// nothing but their own compression word, which is what Photoshop itself
// writes for a record with no pixels. The header record is the one that
// carries the group's name, opacity and visibility; the divider carries
// Photoshop's own `</Layer group>` and nothing else.
//
// **The divider is written BEFORE the members and the header AFTER them**,
// which is the opposite of the intuitive reading and is the trap
// docs/psd-import-gaps.md section 3 records. That ordering is
// `planPsdRecords()`' responsibility, not this function's; this only fills
// in whichever record it is handed.
PsdLayerRecord makeGroupBoundaryRecord(PsdRecordRole role, const Layer& group) {
  PsdLayerRecord rec;
  const bool header = role == PsdRecordRole::kGroupHeader;

  rec.name = header ? group.name : std::string(kPsdGroupDividerName);
  // `pass` on the header record matches what Photoshop writes for a
  // pass-through group, and core/Composite.hpp makes a Group pass-through
  // here too, so it is the truthful key rather than a default. The divider
  // carries `norm`, as both verified real files do.
  rec.blendKey = header ? "pass" : "norm";
  rec.opacity = header ? static_cast<uint8_t>(std::lround(
                             std::clamp(group.opacity, 0.0f, 1.0f) * 255.0f))
                       : uint8_t{255};
  rec.hidden = header ? !group.visible : false;

  const std::vector<uint8_t> emptyChannel = {0x00, 0x00};  // compression 0, no rows
  for (const int16_t id : {int16_t{-1}, int16_t{0}, int16_t{1}, int16_t{2}})
    rec.channels.push_back(PsdChannelBlock{id, emptyChannel});

  PsdWriter lw;
  // `openFolder` is true because a `Document` alone carries no collapsed
  // state -- that lives in app/LayerPanel's `collapsedGroupTags`, which an
  // export from a document cannot consult. io/PsdLayerExtras.hpp says so.
  writePsdLsctBlock(lw, role, /*openFolder=*/true);
  rec.extraBlocks = lw.take();
  return rec;
}

}  // namespace

PsdLayerSectionResult writePsdLayerAndMaskInfo(PsdWriter& w, const Document& doc,
                                               const PsdLayerSectionOptions& options) {
  PsdLayerSectionResult result;

  // The record ORDER comes from io/PsdLayerExtras, which expands every
  // `LayerKind::Group` into the two synthetic records PSD uses to bracket
  // it. A document with no groups plans to exactly one entry per layer, in
  // `Document::layers` order.
  //
  // **No reversal, anywhere.** `Document::layers` index 0 is the bottom of
  // the stack and so is the first PSD layer record; see this module's header
  // for the evidence and for why the guess in either direction is wrong.
  std::vector<PsdRecordPlan> plan;
  std::string planError;
  if (!planPsdRecords(doc, plan, planError)) {
    // A refusal is total. `planPsdRecords()` refuses only group shapes PSD
    // genuinely cannot express -- non-contiguous members, a group whose
    // members sit above it, a `parent` cycle -- and emitting a plausible
    // order for one of those would produce a file that opens with the wrong
    // layers inside the wrong groups.
    result.error = planError;
    return result;
  }

  std::vector<PsdLayerRecord> records;
  records.reserve(plan.size());
  for (const PsdRecordPlan& entry : plan) {
    const Layer& layer = doc.layers[entry.layerIndex];

    if (entry.role != PsdRecordRole::kLayer) {
      records.push_back(makeGroupBoundaryRecord(entry.role, layer));
      continue;
    }

    PsdLayerRecord rec;
    if (!buildPsdLayerRecord(layer, doc, rec, result.warnings)) continue;
    attachMask(layer, rec);
    records.push_back(std::move(rec));
  }

  // The count is an `i16` and the sign carries a different meaning, so the
  // ceiling is 32767 records, not 65535. Refused by name before a byte is
  // written rather than truncated -- a truncated count would produce a file
  // whose remaining records read as channel data.
  if (records.size() > 32767) {
    result.error = "this document has " + std::to_string(records.size()) +
                   " layers; a PSD's layer count is a signed 16-bit field and cannot "
                   "exceed 32,767.";
    return result;
  }

  const size_t layerMaskMarker = w.beginLengthU32();
  {
    const size_t layerInfoMarker = w.beginLengthU32();
    const size_t layerInfoStart = w.size();

    const int16_t count = static_cast<int16_t>(records.size());
    w.i16(options.compositeCarriesTransparency && count > 0 ? static_cast<int16_t>(-count)
                                                            : count);

    for (const PsdLayerRecord& rec : records) writePsdLayerRecord(w, rec);
    // Channel image data for every record, after every record, in the same
    // order -- both the record order and, within a record, the channel
    // order its own table declared.
    for (const PsdLayerRecord& rec : records) writePsdLayerChannelData(w, rec);

    // The layer info section is padded to an even length, and the pad is
    // inside the declared length (which is why it is written before
    // `endLengthU32`).
    w.padTo(2, layerInfoStart);
    w.endLengthU32(layerInfoMarker);
  }

  // Global layer mask info: zero-length. It carries an overlay colour space
  // and opacity for the "quick mask" overlay, which this build has no
  // concept of; a fabricated one would be a claim rather than a
  // translation.
  w.u32(0);

  w.endLengthU32(layerMaskMarker);

  if (!w.ok()) {
    result.error =
        "the PSD writer rejected a field while writing the layer section -- the bytes "
        "written so far declare lengths they do not fill and must not be saved.";
    return result;
  }

  result.ok = true;
  result.layersWritten = records.size();
  return result;
}

}  // namespace np
