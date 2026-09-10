#include "app/TilePreview.hpp"

#include "app/AppState.hpp"  // CanvasView

namespace np {

size_t tilePreviewTiles(const TilePreviewState& tile,
                        TileOffset out[kTilePreviewMaxTiles]) noexcept {
  if (!tile.active) {
    out[0] = TileOffset{0, 0};
    return 1;
  }
  // The eight repeats, then the document. Ordering argued in the header
  // (section 1): the subject is drawn last so nothing can land on top of it.
  size_t n = 0;
  const int reach = (kTilePreviewSpan - 1) / 2;
  for (int row = -reach; row <= reach; ++row)
    for (int col = -reach; col <= reach; ++col)
      if (col != 0 || row != 0) out[n++] = TileOffset{col, row};
  out[n++] = TileOffset{0, 0};
  return n;
}

int tilePreviewSpan(const TilePreviewState& tile) noexcept {
  return tile.active ? kTilePreviewSpan : 1;
}

TileFieldRect tilePreviewField(const TilePreviewState& tile, float texW, float texH) noexcept {
  // Derived from the span rather than written out twice, so the off case and
  // the on case cannot disagree: at span 1 `reach` is 0 and this is exactly
  // the document's own rectangle.
  const float reach = static_cast<float>((tilePreviewSpan(tile) - 1) / 2);
  return TileFieldRect{-reach * texW, -reach * texH, (reach + 1.0f) * texW,
                       (reach + 1.0f) * texH};
}

void setTilePreview(TilePreviewState& tile, CanvasView& view, bool& requestFit, bool on) noexcept {
  if (tile.active == on) return;
  if (on) {
    tile.savedZoom = view.zoom;
    tile.savedPanX = view.panX;
    tile.savedPanY = view.panY;
    tile.hasSaved = true;
  } else if (tile.hasSaved) {
    // Three fields, not the whole `CanvasView`: entering the preview changed
    // only these three, so these three are all that may be put back. The
    // header's section 4 has the argument.
    view.zoom = tile.savedZoom;
    view.panX = tile.savedPanX;
    view.panY = tile.savedPanY;
  }
  tile.active = on;
  // Both directions. Leaving restores the zoom and pan directly above, so the
  // fit is redundant there -- except when the preview was never entered
  // through this function (a session restored with it already on, say), where
  // `hasSaved` is false and a fit is the only sensible view to land on.
  if (on || !tile.hasSaved) requestFit = true;
}

}  // namespace np
