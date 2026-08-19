#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "io/ImageDecode.hpp"

// io/ImageIO (PLAN.md "Phase 2 -- See a file", step 6's remaining half: "read
// PNG/JPEG/TGA/BMP, decode to linear rgba16float, write into tiles". The
// decode-to-linear-float part already lives in io/ImageDecode -- straight
// alpha, no tile knowledge, by that header's own design. This is where the
// decoded image gets premultiplied, packed to half float, and written into
// a Document's tiles, per DESIGN-imaging.md §2's "premultiplied
// (associated)" tile storage policy.
//
// Entry points, deliberately kept separate rather than one function that
// does everything:
//
//   writeDecodedImageIntoLayer -- premultiply + pack a DecodedImage into an
//     *existing* Layer's tile storage. Nothing about this function creates a
//     Document or a Layer; it just fills tiles in one that already exists.
//     The lower-level piece every entry point below reuses rather than
//     re-deriving the premultiply-and-pack loop.
//
//   openImageAsDocument -- "open a file, get a document": decodes file
//     bytes, builds a fresh Document via Document::createBlank() (reusing
//     its already-made policy decisions -- the blank layer is RGB-kind,
//     nothing pre-allocates tiles across the canvas -- rather than
//     re-deciding them here), and calls writeDecodedImageIntoLayer() to fill
//     that Document's one layer. This is the function a future File > Open
//     would call.
//
//   placeImageAsLayer (two overloads: DecodedImage, or raw file bytes) --
//     PLAN.md step 13, "place an image as a layer... distinct from opening
//     a file, which creates a document": builds a fresh RGB Layer the same
//     way createBlank() does, fills it via writeDecodedImageIntoLayer(),
//     and appends it to an *already-open* Document's layer list instead of
//     creating a new Document. This is the Document-level operation step
//     13's eventual menu-item/drag-drop UI will call; wiring that UI up to
//     a live painting canvas is a separate, still-undecided piece of work
//     (see PLAN.md) that this function doesn't attempt.
namespace np {

// Premultiplies `img`'s straight (non-premultiplied) alpha -- see
// io/ImageDecode.hpp's header comment for why that's this step's job, not
// the decoder's -- and packs the result to half float into `layer`'s RGB
// tile storage, at document pixel coordinates [0, img.width) x
// [0, img.height). Per pixel: rgb_premultiplied = rgb * a (alpha itself is
// unchanged), matching DESIGN-imaging.md §2's "premultiplied (associated)"
// tile storage policy -- every downstream op that reads a tile (compositing,
// blending, none of which exists yet) will assume this convention already
// holds, so getting it right here is what keeps that assumption true from
// the moment a file is opened.
//
// `layer.rgbTiles` must already be populated (core/Layer.hpp: "populated
// only when kind == RGB") -- this function only fills tiles in, it doesn't
// decide the layer's kind or emplace its TileStore. Callers that build a
// fresh RGB layer do that first (Document::createBlank() already does,
// which is why openImageAsDocument() below can call straight into this). A
// no-op if `img` isn't valid() or `layer.rgbTiles` isn't populated -- never
// crashes on a misused Layer.
//
// Only allocates the tiles the image's own footprint actually spans
// (TileStore::getOrCreate() is allocate-on-write) -- nothing here
// pre-allocates a grid across some larger canvas the layer might belong to.
void writeDecodedImageIntoLayer(const DecodedImage& img, Layer& layer);

// Decodes `fileData`/`fileSize` (PNG/JPEG/TGA/BMP, via decodeImageLinear())
// and returns a Document sized to the decoded image, holding one RGB layer
// whose tiles hold the image's pixels, premultiplied and half-float packed.
//
// Built on Document::createBlank(), not a second hand-rolled "make a fresh
// Document" path -- reuses its already-made policy decisions rather than
// risking the two quietly diverging. The working space passed to
// createBlank() is a default-constructed WorkingSpace (kRec709Primaries,
// color/Space.hpp's default) -- the same primaries/white point
// io/ImageDecode already decoded the file's sRGB-encoded pixels against, so
// nothing here needs a different working space.
//
// On decode failure (invalid/corrupt/truncated file bytes), returns
// std::nullopt and, if `errorOut` is non-null, forwards
// decodeImageLinear()'s error string -- never a garbage or empty Document.
std::optional<Document> openImageAsDocument(const uint8_t* fileData, size_t fileSize,
                                             std::string* errorOut = nullptr);

// PLAN.md step 13, "place an image as a layer into the open document...
// distinct from opening a file, which creates a document": the Document-
// level operation the eventual menu-item/drag-drop UI will call once the
// live-painting-canvas-to-Document bridge exists (see that step's own
// wording -- not built here; this is the operation underneath it).
//
// Builds a fresh RGB-kind Layer -- reusing Document::createBlank()'s own
// policy (`layer.kind = LayerKind::RGB; layer.rgbTiles.emplace();`), not a
// second, competing way to build one -- fills it via
// writeDecodedImageIntoLayer() above (no duplicated premultiply/pack loop),
// and appends it to `doc.layers`. core/Document.hpp documents that vector as
// "ordered, bottom-to-top", so appending puts the new layer on top of
// whatever's already there, same as every real painting app's "place
// image"/"paste as new layer". Placed at document origin (0,0) --
// writeDecodedImageIntoLayer()'s own convention -- no offset/position
// parameter; drag-to-position is real feature work for whenever the
// interactive bridge exists, not this operation's job.
//
// Works the same whether `doc.layers` starts empty or already holds layers
// -- it only ever appends one more.
//
// No-op, returns false, and leaves `doc.layers` completely untouched if
// `img` isn't valid() -- matching writeDecodedImageIntoLayer()'s own
// contract, this never partially inserts a broken layer.
bool placeImageAsLayer(Document& doc, const DecodedImage& img);

// File-bytes convenience overload: decodes `fileData`/`fileSize` (PNG/JPEG/
// TGA/BMP, via decodeImageLinear()) and calls the overload above. On decode
// failure, returns false and, if `errorOut` is non-null, forwards
// decodeImageLinear()'s error string -- same failure-handling shape as
// openImageAsDocument() -- and, like it, `doc.layers` is left unchanged
// rather than partially updated.
bool placeImageAsLayer(Document& doc, const uint8_t* fileData, size_t fileSize,
                        std::string* errorOut = nullptr);

}  // namespace np
