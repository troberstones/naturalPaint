#include "app/selftest/Support.hpp"

#include <set>
#include <utility>

#include "app/AppState.hpp"
#include "app/TilePreview.hpp"
#include "app/ViewTransform.hpp"
#include "ui/MenuModel.hpp"

namespace np {

// PRD D8 / PLAN.md Phase 9: the 3x3 repeat preview.
//
// Headless throughout. `app/TilePreview` is pure by construction (its header
// says why), the menu model is data, and `performMenuAction()` on a bare
// `AppState` needs no window -- so this section exercises the functions
// `ui/MacPaintUI.cpp`'s canvas block actually calls rather than restating the
// arithmetic beside them.
//
// What is NOT reachable from here, said plainly: the canvas block itself
// (docs/reachability-audit.md F4 -- no ImGui frame, no GPU adapter), which is
// where the nine quads are handed to `addCanvasQuad()`. Two other things cover
// that. The transfer function is `ui/CanvasQuad`'s own, asserted end to end by
// `runPresentTransferTest()`, and the preview adds no second drawing path for
// it to be wrong in -- it calls that one function nine times. And whether the
// nine quads survive `kMaxQuads` is what `tools/golden/run_golden.sh`'s
// `tile_preview` view photographs: a dropped quad is a missing tile, which
// that view fails on and this one could not see.
bool runTilePreviewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // ==========================================================================
  // (a) The offsets: one tile off, nine on, and the document drawn last.
  // ==========================================================================
  {
    TilePreviewState off;
    TileOffset tiles[kTilePreviewMaxTiles];
    const size_t n = tilePreviewTiles(off, tiles);
    check(n == 1 && tiles[0].col == 0 && tiles[0].row == 0,
          "tiles: preview OFF yields exactly one copy, the document itself -- the draw "
          "loop with it off is the single-quad path it replaced, not a second one");
    check(tilePreviewSpan(off) == 1,
          "tiles: span is 1 with the preview off, so fit-to-window is unchanged");
  }
  {
    TilePreviewState on;
    on.active = true;
    TileOffset tiles[kTilePreviewMaxTiles];
    const size_t n = tilePreviewTiles(on, tiles);
    check(n == 9, "tiles: preview ON yields nine copies -- three per axis (PRD D8)");
    check(tilePreviewSpan(on) == 3, "tiles: span is 3 with the preview on");

    // Every one of the nine positions, exactly once. A duplicate would draw
    // one neighbour twice and leave a hole where the other belonged, and the
    // hole is the shape of the defect this preview exists to find.
    std::set<std::pair<int, int>> seen;
    bool inRange = true;
    for (size_t i = 0; i < n; ++i) {
      seen.insert({tiles[i].col, tiles[i].row});
      if (tiles[i].col < -1 || tiles[i].col > 1 || tiles[i].row < -1 || tiles[i].row > 1)
        inRange = false;
    }
    check(seen.size() == 9 && inRange,
          "tiles: the nine are the full -1..1 grid, each exactly once -- a duplicate "
          "would draw one neighbour twice and leave a hole where the other belonged");

    // The centre LAST. `addCanvasQuad()` draws in queue order, so this is what
    // guarantees no repeat can land on top of the document.
    check(n > 0 && tiles[n - 1].col == 0 && tiles[n - 1].row == 0,
          "tiles: the document is the LAST copy queued -- drawn in order, so no repeat "
          "can ever cover the one tile that is the actual document");
    bool centreOnlyLast = true;
    for (size_t i = 0; i + 1 < n; ++i)
      if (tiles[i].col == 0 && tiles[i].row == 0) centreOnlyLast = false;
    check(centreOnlyLast, "tiles: the document appears once, and only at the end");
  }

  // ==========================================================================
  // (b) The field: what the drop shadow goes behind.
  // ==========================================================================
  {
    TilePreviewState off;
    const TileFieldRect f = tilePreviewField(off, 800.0f, 600.0f);
    check(near(f.x0, 0.0f, 1e-4f) && near(f.y0, 0.0f, 1e-4f) && near(f.x1, 800.0f, 1e-4f) &&
              near(f.y1, 600.0f, 1e-4f),
          "field: with the preview off the field IS the document's rectangle, so the "
          "drop shadow is the quad it has always been");

    TilePreviewState on;
    on.active = true;
    const TileFieldRect g = tilePreviewField(on, 800.0f, 600.0f);
    check(near(g.x0, -800.0f, 1e-4f) && near(g.y0, -600.0f, 1e-4f) &&
              near(g.x1, 1600.0f, 1e-4f) && near(g.y1, 1200.0f, 1e-4f),
          "field: with it on the field is the 3x3 block -- the shadow goes behind the "
          "whole thing, not through the middle of the picture");
    check(near(g.x1 - g.x0, 3.0f * 800.0f, 1e-3f) && near(g.y1 - g.y0, 3.0f * 600.0f, 1e-3f),
          "field: the field is exactly span x the document on each axis");
    // Centred on the document: the same amount of repeat on both sides, which
    // is what makes fit-to-window's re-centre put the document in the middle.
    check(near(-g.x0, g.x1 - 800.0f, 1e-3f) && near(-g.y0, g.y1 - 600.0f, 1e-3f),
          "field: the document sits centred in the field -- equal reach on both sides, "
          "so the existing centre-the-quad fit needs no second re-centring");
  }

  // ==========================================================================
  // (c) The copies actually abut, through the real ViewTransform.
  // ==========================================================================
  //
  // The property that makes this a *tiling* and not nine pictures near each
  // other: tile {0,0}'s right edge and tile {1,0}'s left edge must be the same
  // two screen points. Asserted through `app/ViewTransform` -- the same object
  // the canvas block builds -- and under a rotated, mirrored, zoomed view,
  // because a preview that only lines up at identity is a preview that comes
  // apart the moment somebody presses F.
  {
    const float texW = 640.0f;
    const float texH = 480.0f;
    struct Case {
      float zoom, rotation;
      bool mirrorX, mirrorY;
      const char* what;
    };
    const Case cases[] = {
        {1.0f, 0.0f, false, false, "identity"},
        {0.37f, 0.0f, false, false, "zoomed out"},
        {2.5f, 0.6f, false, false, "zoomed in and rotated"},
        {1.0f, 0.0f, true, false, "mirrored left/right"},
        {1.3f, -1.1f, true, true, "rotated and mirrored both axes"},
    };
    bool abut = true;
    bool spansField = true;
    for (const Case& c : cases) {
      CanvasView v;
      v.zoom = c.zoom;
      v.panX = 17.0f;
      v.panY = -9.0f;
      v.rotation = c.rotation;
      v.mirrorX = c.mirrorX;
      v.mirrorY = c.mirrorY;
      const ViewTransform xf(v, Vec2{texW * 0.5f, texH * 0.5f}, Vec2{500.0f, 400.0f});
      const auto corner = [&](const TileOffset& t, float u, float w) {
        return xf.toScreen(Vec2{static_cast<float>(t.col) * texW + u * texW,
                                static_cast<float>(t.row) * texH + w * texH});
      };
      const TileOffset centre{0, 0};
      const TileOffset right{1, 0};
      const TileOffset below{0, 1};
      const Vec2 a0 = corner(centre, 1.0f, 0.0f);
      const Vec2 a1 = corner(centre, 1.0f, 1.0f);
      const Vec2 b0 = corner(right, 0.0f, 0.0f);
      const Vec2 b1 = corner(right, 0.0f, 1.0f);
      if (!near(a0.x, b0.x, 1e-2f) || !near(a0.y, b0.y, 1e-2f) || !near(a1.x, b1.x, 1e-2f) ||
          !near(a1.y, b1.y, 1e-2f))
        abut = false;
      const Vec2 c0 = corner(centre, 0.0f, 1.0f);
      const Vec2 d0 = corner(below, 0.0f, 0.0f);
      if (!near(c0.x, d0.x, 1e-2f) || !near(c0.y, d0.y, 1e-2f)) abut = false;

      // And the nine together fill exactly `tilePreviewField()` -- the corner
      // of tile {-1,-1} is the field's own corner, so the shadow behind the
      // field cannot be a different rectangle from the tiles in front of it.
      TilePreviewState on;
      on.active = true;
      const TileFieldRect fr = tilePreviewField(on, texW, texH);
      const Vec2 tlTile = corner(TileOffset{-1, -1}, 0.0f, 0.0f);
      const Vec2 brTile = corner(TileOffset{1, 1}, 1.0f, 1.0f);
      const Vec2 tlField = xf.toScreen(Vec2{fr.x0, fr.y0});
      const Vec2 brField = xf.toScreen(Vec2{fr.x1, fr.y1});
      if (!near(tlTile.x, tlField.x, 1e-2f) || !near(tlTile.y, tlField.y, 1e-2f) ||
          !near(brTile.x, brField.x, 1e-2f) || !near(brTile.y, brField.y, 1e-2f))
        spansField = false;
    }
    check(abut,
          "geometry: adjacent copies share their edge EXACTLY, through the real "
          "ViewTransform and under zoom/rotation/mirror -- a gap or an overlap here "
          "would read as a seam in a document that has none");
    check(spansField,
          "geometry: the nine copies fill exactly the rectangle the drop shadow is "
          "drawn behind -- one field, not two that could drift apart");
  }

  // ==========================================================================
  // (d) Entering and leaving give the view back.
  // ==========================================================================
  {
    CanvasView v;
    v.zoom = 2.75f;
    v.panX = 40.0f;
    v.panY = -12.0f;
    TilePreviewState tile;
    bool fit = false;

    setTilePreview(tile, v, fit, true);
    check(tile.active && fit,
          "enter: turning the preview on raises the fit request -- without it the user "
          "sees the same view scaled, with eight of the nine copies off the window");
    check(tile.hasSaved && near(tile.savedZoom, 2.75f, 1e-6f) &&
              near(tile.savedPanX, 40.0f, 1e-6f) && near(tile.savedPanY, -12.0f, 1e-6f),
          "enter: the zoom and pan the user was in are saved on the way in");

    // The frame that follows would fit; simulate the view the fit leaves.
    v.zoom = 0.31f;
    v.panX = 0.0f;
    v.panY = 0.0f;
    // ...and a mirror flipped *while inside the preview*, deliberately.
    v.mirrorX = true;
    v.rotation = 0.4f;
    v.grayscale = true;

    fit = false;
    setTilePreview(tile, v, fit, false);
    check(!tile.active && near(v.zoom, 2.75f, 1e-6f) && near(v.panX, 40.0f, 1e-6f) &&
              near(v.panY, -12.0f, 1e-6f),
          "leave: the view the user had comes back -- zoom and both pans, the way a "
          "spring-loaded tool gives back the tool you had (docs/ui.md:205)");
    check(v.mirrorX && near(v.rotation, 0.4f, 1e-6f) && v.grayscale,
          "leave: a mirror, rotation or grayscale flipped INSIDE the preview survives "
          "leaving it -- restoring those would revert an edit this feature never made");
    check(!fit,
          "leave: no fit is requested on the way out -- the restored zoom and pan ARE "
          "the answer, and a fit would throw them away one frame later");
  }
  {
    // The ratchet. `ui/MacPaintUI.cpp` builds the menu every frame and a
    // careless wiring could call this with the state it is already in; if that
    // re-saved, one frame inside the preview would overwrite the view being
    // held for the user with the preview's own fitted one, and leaving would
    // land nowhere.
    CanvasView v;
    v.zoom = 3.0f;
    v.panX = 8.0f;
    TilePreviewState tile;
    bool fit = false;
    setTilePreview(tile, v, fit, true);
    v.zoom = 0.2f;
    v.panX = 0.0f;
    for (int i = 0; i < 5; ++i) setTilePreview(tile, v, fit, true);
    fit = false;
    setTilePreview(tile, v, fit, false);
    check(near(v.zoom, 3.0f, 1e-6f) && near(v.panX, 8.0f, 1e-6f),
          "enter: setting the preview to the state it is already in changes nothing -- "
          "a per-frame re-entry would otherwise overwrite the saved view with the "
          "preview's own and lose it");
  }
  {
    // `requestFit` is a request another part of the frame consumes. Leaving
    // the preview must not swallow a Fit to Window the user asked for in the
    // same frame.
    CanvasView v;
    TilePreviewState tile;
    bool fit = false;
    setTilePreview(tile, v, fit, true);
    fit = true;
    setTilePreview(tile, v, fit, false);
    check(fit,
          "enter/leave: a pending Fit to Window survives leaving the preview -- this "
          "flag is only ever raised here, never cleared");
  }

  // ==========================================================================
  // (e) The menu, end to end through the production dispatch.
  // ==========================================================================
  {
    AppState st;
    st.view.zoom = 4.0f;
    st.view.panX = 55.0f;
    st.view.panY = 66.0f;
    st.requestFitWindow = false;

    performMenuAction(st, MenuAction::TilePreview, 0, 64, 64);
    check(st.tilePreview.active && st.requestFitWindow,
          "menu: View > 3x3 Repeat Preview turns it on and asks for the re-fit -- the "
          "real dispatch, not a second copy of the toggle");

    st.view.zoom = 0.5f;
    st.view.panX = 0.0f;
    st.view.panY = 0.0f;
    performMenuAction(st, MenuAction::TilePreview, 0, 64, 64);
    check(!st.tilePreview.active && near(st.view.zoom, 4.0f, 1e-6f) &&
              near(st.view.panX, 55.0f, 1e-6f) && near(st.view.panY, 66.0f, 1e-6f),
          "menu: picking it a second time turns it off and gives the view back");
    check(!st.view.grayscale,
          "menu: the tile preview and the grayscale preview are separate flags -- one "
          "item writing the other's state is the failure a shared `bool*` used to hide");
  }
  {
    // The item exists, is a Check, and its tick follows the context both ways.
    // A Command here would be an item that could turn the preview on and never
    // admit it was on.
    size_t occurrences = 0;
    auto viewNode = [&occurrences](const MenuContext& c, MenuAction a, bool& found) {
      MenuNode result;
      found = false;
      occurrences = 0;
      for (const MenuNode& menu : buildMenuModel(c))
        for (const MenuNode& n : menu.children)
          if (n.action == a) {
            result = n;
            found = true;
            ++occurrences;
          }
      return result;
    };
    MenuContext ctxOn;
    ctxOn.tilePreview = true;
    MenuContext ctxOff;
    bool foundOn = false;
    bool foundOff = false;
    const MenuNode nodeOn = viewNode(ctxOn, MenuAction::TilePreview, foundOn);
    const size_t onceOnly = occurrences;
    const MenuNode nodeOff = viewNode(ctxOff, MenuAction::TilePreview, foundOff);
    check(foundOn && foundOff && onceOnly == 1,
          "menu: the item is in the tree exactly once -- an action with no item is a "
          "built feature with no way to invoke it, and two would resolve to the first");
    check(foundOn && nodeOn.kind == MenuNodeKind::Check,
          "menu: it is a Check, not a Command -- it is a state the user leaves on, so "
          "the menu has to be able to say whether it is on");
    check(foundOn && foundOff && nodeOn.checked && !nodeOff.checked,
          "menu: the tick follows MenuContext::tilePreview both ways");
    check(std::string(menuItemSpec(MenuAction::TilePreview).label).find("3x3") !=
              std::string::npos,
          "menu: the label names what it does -- '3x3' is the whole of the affordance, "
          "and PRD.md:211 calls this a requirement rather than a nicety");
  }

  return ok;
}

}  // namespace np
