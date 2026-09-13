#include "app/PasteCommands.hpp"

#include <cmath>

#include "app/AppState.hpp"
#include "core/CanvasLimits.hpp"
#include "core/LayerOps.hpp"
#include "core/Mask.hpp"
#include "io/ClipboardImage.hpp"
#include "io/ImageDecode.hpp"
#include "io/ImageIO.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

namespace np {
namespace {

DocumentRegion clipboardContentRegion(const Clipboard& clip) {
  return clip.kind == LayerKind::Pigment ? pigmentContentRegion(*clip.pigmentTiles)
                                        : rgbContentRegion(*clip.rgbTiles);
}

// The mask Paste Into gives its new layer: `selection`'s own coverage,
// materialised over every tile either the selection or `layer`'s OWN content
// touches. core/Mask.hpp's own rule is why the second half matters: an
// ABSENT mask tile means REVEAL, so a tile the pasted content occupies but
// the selection does not would show through as unselected paint if left
// unallocated instead of written as an explicit all-hide tile.
MaskTileStore selectionAsLayerMask(const Selection& selection, const Layer& layer) {
  MaskTileStore out;
  auto addTile = [&](TileCoord coord) {
    if (out.find(coord) != nullptr) return;
    const SelectionTile* selTile = selection.tiles.find(coord);
    MaskTile& mt = out.getOrCreate(coord);
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const PixelCoord local{lx, ly};
        mt.writeCoverage(local, selectionTileCoverage(selTile, local));
      }
    }
  };
  for (const auto& [coord, tile] : selection.tiles) {
    (void)tile;
    addTile(coord);
  }
  if (layer.rgbTiles.has_value())
    for (const auto& [coord, tile] : *layer.rgbTiles) {
      (void)tile;
      addTile(coord);
    }
  if (layer.pigmentTiles.has_value())
    for (const auto& [coord, tile] : *layer.pigmentTiles) {
      (void)tile;
      addTile(coord);
    }
  return out;
}

}  // namespace

PasteIntoResult pasteInto(OpenDocument& od, const Clipboard& clip) {
  PasteIntoResult r;
  if (clip.empty()) {
    r.error = "Paste Into refused: the clipboard is empty.";
    return r;
  }
  if (!od.selection.has_value()) {
    r.error = "Paste Into refused: nothing is selected.";
    return r;
  }
  const std::optional<SelectionBounds> selBoundsOpt = selectionBounds(*od.selection);
  if (!selBoundsOpt) {
    r.error = "Paste Into refused: the selection covers no pixels.";
    return r;
  }

  // Above the active layer, matching plain Paste's own placement
  // (ui/MacPaintUI.cpp).
  const size_t at = std::min(od.activeLayer + 1, od.document.layers.size());
  const std::optional<size_t> inserted = pasteAsLayer(od.document, clip, at);
  if (!inserted) {
    r.error = "Paste Into refused: could not insert the pasted layer.";
    return r;
  }
  const size_t idx = *inserted;

  // Centre the pasted content on the selection's bounds, in whole pixels so
  // the move stays on PRD D15's lossless exact-translate path -- a half-pixel
  // remainder (an odd-vs-even width) is rounded rather than resampled away.
  const DocumentRegion srcBounds = clipboardContentRegion(clip);
  const SelectionBounds selBounds = *selBoundsOpt;
  const float dx = std::round((static_cast<float>(selBounds.x0) + static_cast<float>(selBounds.x1)) * 0.5f -
                              (static_cast<float>(srcBounds.x) + static_cast<float>(srcBounds.width) * 0.5f));
  const float dy = std::round((static_cast<float>(selBounds.y0) + static_cast<float>(selBounds.y1)) * 0.5f -
                              (static_cast<float>(srcBounds.y) + static_cast<float>(srcBounds.height) * 0.5f));
  if (dx != 0.0f || dy != 0.0f) {
    const LayerTransformResult moved =
        transformLayer(od.document, idx, transformTranslate(dx, dy), DocumentTransformParams{});
    if (!moved.ok) {
      removeLayer(od.document, idx);
      r.error = "Paste Into refused: " + moved.error;
      return r;
    }
  }

  // The mask IS the selection, built AFTER the centring move -- a whole-layer
  // transform moves a layer's mask along with its pixels
  // (ops/DocumentTransform.hpp), and this mask does not exist yet at that
  // point, so there is nothing for the move to carry astray.
  Layer& layer = od.document.layers[idx];
  layer.mask = selectionAsLayerMask(*od.selection, layer);

  od.activeLayer = idx;
  od.recordEdit("paste into selection", EditKind::Structural);
  r.ok = true;
  r.layerIndex = idx;
  return r;
}

std::optional<OpenDocument> buildDocumentFromClipboard(const Clipboard& clip) {
  if (clip.empty()) return std::nullopt;
  const DocumentRegion bounds = clipboardContentRegion(clip);
  if (bounds.empty()) return std::nullopt;

  OpenDocument od = makeBlankOpenDocument(static_cast<int32_t>(bounds.width),
                                          static_cast<int32_t>(bounds.height), WorkingSpace{});
  const std::optional<size_t> idx = pasteAsLayer(od.document, clip, 0);
  if (!idx) return std::nullopt;
  // pasteAsLayer() inserts ABOVE the index it is given, so createBlank()'s
  // own placeholder layer is still there, one slot up -- dropped so the new
  // document holds exactly the one layer this command promises.
  removeLayer(od.document, *idx + 1);

  // Shift the content's own top-left to the document's origin, in whole
  // pixels so the move stays on PRD D15's lossless exact-translate path.
  if (bounds.x != 0 || bounds.y != 0) {
    const LayerTransformResult moved = transformLayer(
        od.document, *idx,
        transformTranslate(static_cast<float>(-bounds.x), static_cast<float>(-bounds.y)),
        DocumentTransformParams{});
    if (!moved.ok) return std::nullopt;
  }
  od.activeLayer = *idx;
  return od;
}

PasteAsNewDocumentResult pasteAsNewDocument(AppState& st) {
  PasteAsNewDocumentResult r;
  if (!st.clipboard.empty()) {
    std::optional<OpenDocument> built = buildDocumentFromClipboard(st.clipboard);
    if (!built) {
      r.error = "Paste as New Document refused: could not place the clipboard's content.";
      return r;
    }
    OpenDocument* od = st.documents.add(std::move(*built));
    if (od == nullptr) {
      r.error = "Paste as New Document refused: could not open a new document.";
      return r;
    }
    r.ok = true;
    r.id = od->id;
    return r;
  }

  // Empty internal clipboard: fall back to the OS pasteboard, the same probe
  // ui/NewDocumentDialog.cpp's "New From Clipboard" button already uses.
  const ClipboardImageProbe probe = probeClipboardImage();
  if (probe.status != ClipboardImageStatus::Image) {
    r.error = "Paste as New Document refused: the clipboard holds nothing to paste.";
    return r;
  }
  const std::string sizeError = canvasDimensionRefusal(static_cast<int32_t>(probe.width),
                                                        static_cast<int32_t>(probe.height));
  if (!sizeError.empty()) {
    r.error = "Paste as New Document refused: " + sizeError;
    return r;
  }
  OpenDocument* od = st.documents.add(makeBlankOpenDocument(
      static_cast<int32_t>(probe.width), static_cast<int32_t>(probe.height), WorkingSpace{}));
  if (od == nullptr) {
    r.error = "Paste as New Document refused: could not open a new document.";
    return r;
  }
  DecodedImage img;
  img.width = probe.width;
  img.height = probe.height;
  img.pixels = probe.pixels;
  if (!img.valid() || !placeImageAsLayer(od->document, img)) {
    r.error = "Paste as New Document refused: could not place the clipboard image.";
    return r;
  }
  r.ok = true;
  r.id = od->id;
  return r;
}

}  // namespace np
