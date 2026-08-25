#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "app/DocumentLifecycle.hpp"

// app/ImportImage -- File > Import Image..., i.e. "put this picture into the
// document I already have open, as a new layer".
//
// --- The gap this closes ---------------------------------------------------
//
// `io/ImageIO`'s `placeImageAsLayer()` has done the hard half since PLAN.md
// Phase 2 step 13: decode, premultiply, pack to half float, build an RGB-kind
// Layer with its `rgbTiles` engaged, append it. Its own header says it is "the
// Document-level operation step 13's eventual menu-item/drag-drop UI will
// call".
//
// **That UI was never built.** Before this module the only caller of
// `placeImageAsLayer()` in the whole binary was `--selftest`; io/Export.hpp
// even describes it as having been written "for step 13's drag-and-drop", and
// there is no `SDL_EVENT_DROP_FILE` handler anywhere in `src/`. So a feature
// that was finished, tested and correct was unreachable from the running
// application -- the same shape of defect as the close box that refused
// silently, one layer further out.
//
// --- Why a module rather than eight lines in the File menu -----------------
//
// The same three reasons app/CloseDecision gives, and one more.
//
//  1. **There will be more than one entry point.** A drag-and-drop handler is
//     the obvious next one, and it must not be a second importer with its own
//     idea of where the layer lands or its own error wording. One function,
//     and the menu item is a caller of it like any other.
//  2. **The failures are the interesting part** and they are exactly what a
//     screenshot cannot witness. Everything here is free of ImGui and of
//     `AppState`, so `--selftest` asserts that a missing file adds no layer and
//     no history entry rather than a human noticing that nothing happened.
//  3. **The decision about *where* the layer lands is a decision**, and it has
//     to be written down once, next to the code that makes it.
//  4. The read-the-file half is where "loud failure" actually lives. A path
//     that does not exist never reaches a decoder, so a module that only took
//     bytes could not produce the message the user needs most.
//
// --- Where the layer lands: the top of the stack, and why not "above the
//     active layer" ---------------------------------------------------------
//
// `placeImageAsLayer()` appends, and `core/Document.hpp` documents `layers` as
// "ordered, bottom-to-top", so appending is the top. This module deliberately
// does **not** insert above the active layer instead, for two reasons:
//
//  * **Visibility.** An imported image inserted just above the active layer
//    can be completely hidden by whatever opaque layers sit above it, and a
//    user who imports a photograph and sees no change concludes the import
//    failed. The top of the stack is the one position where the result is
//    always visible, which is what makes the operation legible.
//  * It is what "place" does in every application that has one, and it is
//    what the function already does. Re-deciding it here would mean either
//    changing `placeImageAsLayer()` -- whose contract `--selftest` already
//    asserts -- or inserting a second, competing placement rule beside it.
//
// The new layer is then made **active**, which is not decoration: the layers
// panel is where a user confirms that an import happened at all, and a layer
// that arrives without the selection moving reads as a layer that did not
// arrive.
//
// --- An image larger than the document ------------------------------------
//
// Stated rather than handled, because the honest answer is a surprise.
//
// `writeDecodedImageIntoLayer()` writes the image at document coordinates
// [0,w) x [0,h) through `TileStore::getOrCreate()`, which is allocate-on-
// write and has no idea what the document's canvas size is. A `Layer` has no
// bounds of its own. So importing a 4000x3000 photograph into a 512x512
// document **keeps every one of those twelve million pixels**: nothing is
// cropped and nothing is resampled.
//
// What then happens to the pixels past the canvas edge is the surprising
// part. `compositeDocumentPremultiplied()` walks exactly `doc.width` x
// `doc.height` (io/Export.cpp's `flattenDocumentToLinear()` is built on it),
// so the overhang is **never composited, never drawn and never exported** --
// but it is still resident in the tile store at 128 KiB per tile, and a
// `.npaint` save writes the tiles that exist. The example above costs ~750
// tiles, ~94 MiB, of pixels the user cannot see.
//
// This module does not crop that away and does not resize to fit. Both would
// be destroying or altering the user's data on their behalf, which is the one
// thing an import must not do -- and a resample would additionally be a
// quality decision (`ops/Resample`'s filter choice) that belongs to a
// transform command the user invokes deliberately. Instead the result carries
// a **warning that names both sizes**, so the cost is visible at the moment it
// is incurred. Cropping, scaling to fit and offsetting the placement are real
// feature work; silently doing one of them is not.
namespace np {

// What an import did, or refused to do.
struct ImportImageResult {
  bool ok = false;

  // One sentence for the status line. On success it names the file, the pixel
  // dimensions and the layer index; on failure it names the file and the
  // reason, and the reason is the lower layer's own words wherever there is a
  // lower layer to quote (`decodeImageLinear()`'s, or the C library's
  // `strerror`). **Never empty**, in either direction: a silent no-op is the
  // defect this module exists to stop shipping.
  std::string status;

  // Non-fatal things the user still has to be told -- today only the oversize
  // note above. Separate from `status` because a warning is not a failure and
  // must not be coloured or read as one.
  std::vector<std::string> warnings;

  // Where the new layer landed in `doc.document.layers`, when `ok`. Always
  // `layers.size() - 1`; returned rather than recomputed by the caller so a
  // test asserts the same number the UI would read.
  size_t layerIndex = 0;

  // The decoded image's size, when `ok`. Zero otherwise.
  uint32_t imageWidth = 0;
  uint32_t imageHeight = 0;
};

// The label `recordEdit()` is called with, and therefore what the History
// panel shows. Exposed so `--selftest` asserts the entry by the same constant
// the implementation uses rather than by a copy of the string that can drift.
//
// The noun form app/DocumentLifecycle.hpp asks for ("place image as layer",
// "duplicate", not "Imported"), with the file's own name appended -- a history
// list of four identical "import image" rows is a list nobody can navigate.
std::string importImageEditLabel(const std::string& path);

// Reads `path`, decodes it, and appends it to `doc` as a new RGB layer, which
// becomes the active layer.
//
// **Exactly one history entry**, recorded through `OpenDocument::recordEdit()`
// with `EditKind::Structural` -- app/DocumentLifecycle.hpp's own default and
// the correct kind here, because adding a layer changes the shape of the
// stack, which is what ADR-0008 wants journalled at once rather than on the
// next timer tick. `EditKind::Content` is reserved for the pixel-level stroke
// bridge and would be wrong: an import lost to a crash before the journal's
// interval elapsed is exactly the failure PRD O5 exists to prevent.
//
// **Every failure is loud, and every failure changes nothing.** An empty path,
// a path that is not a regular file, a file that cannot be read, an empty
// file, and a file no decoder in this build accepts -- including an EXR or a
// TIFF in an `NP_USE_OIIO=OFF` build, where the refusal carries
// io/ImageDecode's own forwarded reason -- each return `ok == false` with a
// sentence naming the file. On any of them `doc.document.layers` is untouched,
// no revision is bumped, no history entry is appended and the active layer
// does not move. There is no partial import.
//
// The decode is `decodeImageLinear()`'s, so the formats are exactly the ones
// io/ImageDecode already supports and no format list is restated here: PNG,
// JPEG, TGA and BMP in every build, plus EXR/TIFF/HDR/DPX/flattened PSD when
// OpenImageIO is linked in.
ImportImageResult importImageAsLayer(OpenDocument& doc, const std::string& path);

}  // namespace np
