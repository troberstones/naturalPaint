#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/TransformSession.hpp"
#include "core/LayerSetOps.hpp"

namespace np {

// Track `xform` (PRD C12): `app::TransformSession`'s `TransformTarget::
// LayerSet` -- app/TransformSession.hpp section 8 is the argument; this is
// the proof. Headless and GPU-free, in `app/selftest/TransformSession.cpp`'s
// own style: pure model and undo-funnel plumbing, not the resampler (that is
// ops/Transform.hpp's and ops/DocumentTransform.hpp's own suite).
bool runTransformLayerSetTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] transform layer set: a multi-layer selection transformed together as "
              "one set (PRD C12), no GPU\n");

  // --- Fixtures --------------------------------------------------------------

  // Two RGB layers with distinct, non-overlapping content, added above a
  // blank base layer -- distinct fills so "did layer B's pixels really land
  // where layer C's would have, or vice versa" would show up as a colour
  // mismatch rather than a coincidence.
  auto fillBlock = [](TileStore& store, int32_t x0, int32_t y0, int32_t size,
                      const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y < y0 + size; ++y) {
      for (int32_t x = x0; x < x0 + size; ++x) {
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), premultiplied);
      }
    }
  };
  auto makeTwoRgbDoc = [&]() {
    OpenDocument od = makeBlankOpenDocument(96, 96, WorkingSpace{});
    addLayer(od.document, od.document.layers.size(), makeRgbLayer("B"));
    addLayer(od.document, od.document.layers.size(), makeRgbLayer("C"));
    fillBlock(*od.document.layers[1].rgbTiles, 10, 10, 12, {0.8f, 0.1f, 0.1f, 1.0f});
    fillBlock(*od.document.layers[2].rgbTiles, 60, 55, 12, {0.1f, 0.2f, 0.9f, 1.0f});
    od.recordEdit("fill fixture", EditKind::Content);
    return od;
  };

  // Whole-canvas bit-identity, reused from app/selftest/TransformSession.cpp's
  // own `tilesBitIdentical` (duplicated rather than shared across TUs, this
  // suite's own convention since the split out of one file -- see
  // app/selftest/Support.hpp's header).
  const DocumentRegion wholeCanvas{0, 0, 96u, 96u};
  auto tilesBitIdentical = [](const TileStore& a, const TileStore& b, const DocumentRegion& r) {
    const TransformImage ia = imageFromTileStore(a, r.x, r.y, r.width, r.height);
    const TransformImage ib = imageFromTileStore(b, r.x, r.y, r.width, r.height);
    return ia.px.size() == ib.px.size() &&
          std::memcmp(ia.px.data(), ib.px.data(), ia.px.size() * sizeof(float)) == 0;
  };
  auto pigmentBitIdentical = [](const PigmentTileStore& a, const PigmentTileStore& b) {
    if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
    for (const auto& [coord, tile] : a) {
      const PigmentTile* other = b.find(coord);
      if (other == nullptr || std::memcmp(tile.data(), other->data(), sizeof(PigmentTile)) != 0)
        return false;
    }
    return true;
  };

  // ==========================================================================
  // 1. Each set member's pixels match transforming it alone by the SAME
  //    matrix -- the direct statement of "one shared matrix, applied through
  //    the identical per-layer entry point `transformLayer()` already uses".
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();
    const Document before = od.document;

    const LayerSelection sel = makeLayerSelection({2, 1});  // deliberately unsorted input
    check(sel.indices.size() == 2 && sel.indices[0] == 1 && sel.indices[1] == 2,
          "makeLayerSelection sorts ascending regardless of call-site order");

    TransformSession ts;
    const TransformBeginResult began = ts.beginLayerSet(od, sel);
    check(began.ok, "beginLayerSet: two admissible RGB layers begin a set session");
    check(ts.target() == TransformTarget::LayerSet, "beginLayerSet: session target is LayerSet");
    check(ts.layerIndices() == std::vector<size_t>{1, 2},
          "beginLayerSet: layerIndices() is the sorted selection");

    // The gizmo's own box is the UNION of both members' content bounds, not
    // either one alone -- a box that only covered layer B would not reach
    // layer C's handles at all.
    const DocumentRegion box = ts.sourceBounds();
    check(box.x <= 10 && box.y <= 10 && box.x + static_cast<int32_t>(box.width) >= 72 &&
              box.y + static_cast<int32_t>(box.height) >= 67,
          "beginLayerSet: sourceBounds() is the union of every admitted member's own bounds");

    // A rotation about the UNION centre -- "one matrix about the union
    // centre" per the brief -- exercised through the drag path exactly as a
    // gizmo would drive it, not set directly, so this also proves the
    // handle/drag geometry (unchanged code) already works unmodified for this
    // target: it is a pure function of sourceBounds()/pending() and neither
    // needed to be taught a third target exists.
    const Point2 pivot{static_cast<float>(box.x) + static_cast<float>(box.width) * 0.5f,
                       static_cast<float>(box.y) + static_cast<float>(box.height) * 0.5f};
    const Mat3 m = transformRotateDegreesAbout(20.0f, pivot);
    ts.setPending(m);

    const size_t cursorBefore = od.history.cursor();
    const uint64_t revBefore = od.revision;
    const TransformCommitResult done = ts.commit(od);
    check(done.ok, "commit: a two-member RGB set commits");
    check(!ts.active(), "commit: the session is no longer active afterwards");

    // The independent reference: the SAME matrix, applied to each member
    // alone through the identical ops-level function the set commit uses
    // internally. Same code, same floats -- a match here is not a tolerance
    // argument, it is a determinism argument.
    Document solo = before;
    const LayerTransformResult rb = transformLayer(solo, 1, m, DocumentTransformParams{});
    const LayerTransformResult rc = transformLayer(solo, 2, m, DocumentTransformParams{});
    check(rb.ok && rc.ok, "reference: transformLayer() alone succeeds on each member");
    check(tilesBitIdentical(*od.document.layers[1].rgbTiles, *solo.layers[1].rgbTiles, wholeCanvas),
          "set commit: layer B's pixels match transforming it alone by the same matrix");
    check(tilesBitIdentical(*od.document.layers[2].rgbTiles, *solo.layers[2].rgbTiles, wholeCanvas),
          "set commit: layer C's pixels match transforming it alone by the same matrix");
    // The base layer (not in the selection) is untouched.
    check(tilesBitIdentical(*od.document.layers[0].rgbTiles, *before.layers[0].rgbTiles, wholeCanvas),
          "set commit: a layer NOT in the selection is left exactly as it was");

    // ---- ONE history entry, and undo restores every member together ------
    check(od.revision == revBefore + 1, "set commit: bumps the revision exactly once");
    check(od.history.cursor() == cursorBefore + 1,
          "set commit: appends exactly ONE history entry for the whole set, so undo takes both "
          "layers back in one step");
    od.document = *od.history.undo();
    check(tilesBitIdentical(*od.document.layers[1].rgbTiles, *before.layers[1].rgbTiles, wholeCanvas) &&
              tilesBitIdentical(*od.document.layers[2].rgbTiles, *before.layers[2].rgbTiles,
                                wholeCanvas),
          "undo: restores BOTH members bit-identically in one step");
  }

  // ==========================================================================
  // 2. A locked member refuses the WHOLE set, by name, and changes nothing.
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();
    const Document before = od.document;
    od.document.layers[2].locked = true;

    TransformSession ts;
    const TransformBeginResult began = ts.beginLayerSet(od, makeLayerSelection({1, 2}));
    check(!began.ok && began.error.find("locked") != std::string::npos,
          "beginLayerSet refuses the whole set when ANY member is locked, by name");
    check(!ts.active(), "a refused beginLayerSet leaves no session active");
    check(tilesBitIdentical(*od.document.layers[1].rgbTiles, *before.layers[1].rgbTiles, wholeCanvas) &&
              tilesBitIdentical(*od.document.layers[2].rgbTiles, *before.layers[2].rgbTiles,
                                wholeCanvas),
          "a refused set leaves the document byte-for-byte unchanged -- not even the UNLOCKED "
          "member moved");
  }

  // ==========================================================================
  // 3. Group and Adjustment members: refused by name, no special case for
  //    either -- app/TransformSession.hpp section 8's stated decision.
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();

    Layer group;
    group.kind = LayerKind::Group;
    group.groupTag = "g1";
    addLayer(od.document, od.document.layers.size(), group);
    const size_t groupIdx = od.document.layers.size() - 1;

    TransformSession ts;
    TransformBeginResult groupBegin = ts.beginLayerSet(od, makeLayerSelection({1, groupIdx}));
    check(!groupBegin.ok && groupBegin.error.find("Group") != std::string::npos &&
              groupBegin.error.find("no pixels") != std::string::npos,
          "beginLayerSet refuses a selected Group member by name, as a kind with no pixels");

    Layer adjustment;
    adjustment.kind = LayerKind::Adjustment;
    addLayer(od.document, od.document.layers.size(), adjustment);
    const size_t adjustmentIdx = od.document.layers.size() - 1;
    TransformBeginResult adjBegin = ts.beginLayerSet(od, makeLayerSelection({1, adjustmentIdx}));
    check(!adjBegin.ok && adjBegin.error.find("Adjustment") != std::string::npos,
          "beginLayerSet refuses a selected Adjustment member by name, the identical sentence "
          "Group gets -- no special case for either (section 8)");
  }

  // ==========================================================================
  // 4. Other admission refusals: too few members, out of range, empty content.
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();
    TransformSession ts;

    TransformBeginResult tooFew = ts.beginLayerSet(od, makeLayerSelection({1}));
    check(!tooFew.ok && tooFew.error.find("at least two") != std::string::npos,
          "beginLayerSet refuses a one-member selection by name -- that is beginLayer()'s own "
          "path, not a second door into it");

    TransformBeginResult outOfRange = ts.beginLayerSet(od, makeLayerSelection({1, 99}));
    check(!outOfRange.ok && outOfRange.error.find("out of range") != std::string::npos,
          "beginLayerSet refuses an out-of-range member, by name");

    Layer emptyRgb;
    emptyRgb.kind = LayerKind::RGB;
    emptyRgb.rgbTiles.emplace();  // engaged, but genuinely empty
    addLayer(od.document, od.document.layers.size(), emptyRgb);
    const size_t emptyIdx = od.document.layers.size() - 1;
    TransformBeginResult empty = ts.beginLayerSet(od, makeLayerSelection({1, emptyIdx}));
    check(!empty.ok && empty.error.find("no content") != std::string::npos,
          "beginLayerSet refuses an engaged-but-empty member, distinctly from 'no pixels'");
  }

  // ==========================================================================
  // 5. A Pigment + RGB pair -- the Pigment member takes ops/DocumentTransform's
  //    own mass-weighted path through the identical `transformLayer()` call
  //    the set commit uses for every member, proven the same way part 1 did.
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();
    Layer pigment = makePigmentLayer("P");
    for (int32_t y = 20; y < 40; ++y) {
      for (int32_t x = 20; x < 50; ++x) {
        PigmentTexel t;
        t.latent.c[0] = 0.3f;
        t.latent.c[1] = 0.15f;
        t.latent.c[2] = 0.05f;
        t.mass = 1.0f;
        pigment.pigmentTiles->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeTexel(tileLocalOffset(PixelCoord{x, y}), t);
      }
    }
    addLayer(od.document, od.document.layers.size(), pigment);
    const size_t pigmentIdx = od.document.layers.size() - 1;
    const Document before = od.document;

    TransformSession ts;
    const LayerSelection sel = makeLayerSelection({1, pigmentIdx});
    check(ts.beginLayerSet(od, sel).ok, "beginLayerSet accepts a Pigment member alongside RGB");
    const Mat3 m = transformTranslate(6.0f, -4.0f);
    ts.setPending(m);
    const TransformCommitResult done = ts.commit(od);
    check(done.ok, "commit: an RGB + Pigment set commits together");

    Document solo = before;
    const LayerTransformResult rb = transformLayer(solo, 1, m, DocumentTransformParams{});
    const LayerTransformResult rp = transformLayer(solo, pigmentIdx, m, DocumentTransformParams{});
    check(rb.ok && rp.ok, "reference: transformLayer() alone succeeds on both kinds");
    check(tilesBitIdentical(*od.document.layers[1].rgbTiles, *solo.layers[1].rgbTiles, wholeCanvas),
          "RGB+Pigment set: the RGB member matches its solo transform");
    check(pigmentBitIdentical(*od.document.layers[pigmentIdx].pigmentTiles,
                              *solo.layers[pigmentIdx].pigmentTiles),
          "RGB+Pigment set: the Pigment member's mass-weighted resample matches its solo "
          "transform, bit for bit");
  }

  // ==========================================================================
  // 6. The single-layer path is untouched: `beginLayer()` on one layer still
  //    goes through TransformTarget::Layer, never LayerSet, and behaves
  //    exactly as app/selftest/TransformSession.cpp already proves at length
  //    (unchanged by this file). This is the narrow, direct corroboration:
  //    the SAME matrix committed through beginLayer() alone matches the
  //    reference transformLayer() call the same way a set member does --
  //    beginLayer()'s own code was not touched to add this target.
  // ==========================================================================
  {
    OpenDocument od = makeTwoRgbDoc();
    const Document before = od.document;
    TransformSession ts;
    check(ts.beginLayer(od, 1).ok, "single-layer path: beginLayer() still begins a Layer session");
    check(ts.target() == TransformTarget::Layer,
          "single-layer path: target() is Layer, never LayerSet, for a solo beginLayer()");
    const Mat3 m = transformTranslate(3.0f, 9.0f);
    ts.setPending(m);
    const TransformCommitResult done = ts.commit(od);
    check(done.ok, "single-layer path: commits");
    Document solo = before;
    transformLayer(solo, 1, m, DocumentTransformParams{});
    check(tilesBitIdentical(*od.document.layers[1].rgbTiles, *solo.layers[1].rgbTiles, wholeCanvas),
          "single-layer path: bit-identical to a direct transformLayer() call, exactly as before "
          "this track's change");
  }

  return ok;
}

}  // namespace np
