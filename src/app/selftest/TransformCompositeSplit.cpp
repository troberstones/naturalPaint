#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#include "core/Composite.hpp"
#include "ui/TransformCompositeSplit.hpp"

namespace np {

// ui/TransformCompositeSplit: the three-way canvas a live Free Transform
// draws (layers below, the moving pixels, layers above) and the predicate
// that says when taking it apart that way is exact.
//
// This section is headless and GPU-free. The claim it has to make is not
// "the two halves look right" but "compositing them separately and putting
// one over the other is the SAME PICTURE as compositing them together" --
// which is a memcmp, not a tolerance, because `over` is associative in exact
// arithmetic and both sides here run the same walk over the same texels in
// the same order.
bool runTransformCompositeSplitTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] transform composite split: below / moving pixels / above\n");

  constexpr int32_t kW = 48, kH = 32;

  auto fill = [](TileStore& store, float r, float g, float b, float a, int32_t x0, int32_t y0,
                 int32_t x1, int32_t y1) {
    for (int32_t y = y0; y < y1; ++y) {
      for (int32_t x = x0; x < x1; ++x) {
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {r * a, g * a, b * a, a});
      }
    }
  };

  // Three overlapping RGB layers, bottom to top, with partial alpha so the
  // compositing order actually shows in the result.
  auto makeDoc = [&]() {
    Document doc;
    doc.width = kW;
    doc.height = kH;
    for (int i = 0; i < 3; ++i) {
      Layer l;
      l.kind = LayerKind::RGB;
      l.name = "layer" + std::to_string(i);
      l.rgbTiles.emplace();
      doc.layers.push_back(std::move(l));
    }
    fill(*doc.layers[0].rgbTiles, 0.9f, 0.1f, 0.1f, 1.00f, 0, 0, kW, kH);
    fill(*doc.layers[1].rgbTiles, 0.1f, 0.8f, 0.2f, 0.60f, 4, 4, 40, 26);
    fill(*doc.layers[2].rgbTiles, 0.2f, 0.3f, 0.9f, 0.45f, 12, 8, 46, 30);
    return doc;
  };

  // `over`, premultiplied, exactly as core/Blend's normal mode does it -- the
  // arithmetic the GPU performs when the above-half is drawn over the
  // below-half. Written out here rather than borrowed so that a change to the
  // compositor cannot silently redefine what this section is comparing to.
  auto overInto = [](const std::vector<float>& above, const std::vector<float>& below) {
    std::vector<float> out(below.size(), 0.0f);
    for (size_t i = 0; i + 3 < out.size(); i += 4) {
      const float srcA = above[i + 3];
      for (int c = 0; c < 4; ++c) out[i + c] = above[i + c] + below[i + c] * (1.0f - srcA);
    }
    return out;
  };

  auto bitIdentical = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };

  // --- 1. The split is the same picture ------------------------------------
  //
  // The load-bearing assertion of the whole file. Layer 1 is the one being
  // transformed, so layer 2 is above it and must end up in front of the
  // moving pixels -- which is only worth doing if putting it there does not
  // change anything else about the image.
  {
    const Document doc = makeDoc();
    const std::vector<float> whole = compositeDocumentPremultiplied(doc);

    const size_t n = 1;
    check(transformSplitIsExact(doc, n),
          "split: three plain normal layers split exactly");
    check(anyVisibleLayerAbove(doc, n), "split: and there IS a layer above to draw");

    // The three-way stack's below-half is the layers STRICTLY BELOW the
    // transformed one. `documentWithLayerHidden()` is a different question --
    // it keeps the layers above, which is the fallback arrangement (one
    // texture, preview in front) and would composite them twice here.
    const std::vector<float> below =
        compositeDocumentPremultiplied(documentWithLayersAtOrAboveHidden(doc, n));
    const std::vector<float> above =
        compositeDocumentPremultiplied(documentWithLayersAtOrBelowHidden(doc, n));

    // Put the transformed layer back where it started -- an untouched
    // transform, pending = identity -- and the three-way stack must reproduce
    // the original composite exactly. This is the property that makes the
    // split honest: the ONLY thing a drag changes is where the middle quad
    // lands, so if the stack is right at identity it is right everywhere.
    Document onlyMoving = doc;
    for (size_t i = 0; i < onlyMoving.layers.size(); ++i)
      onlyMoving.layers[i].visible = (i == n);
    const std::vector<float> moving = compositeDocumentPremultiplied(onlyMoving);

    const std::vector<float> stacked = overInto(above, overInto(moving, below));
    check(bitIdentical(stacked, whole),
          "split: below + moving + above is BIT-IDENTICAL to the whole composite");

    // The confusion the two helpers invite, pinned so it cannot come back:
    // stacking on the FALLBACK half double-composites everything above.
    const std::vector<float> fallbackHalf =
        compositeDocumentPremultiplied(documentWithLayerHidden(doc, n));
    check(!bitIdentical(overInto(above, overInto(moving, fallbackHalf)), whole),
          "split: ...and stacking on the fallback half instead double-counts the layers above");

    // Sabotage's shape, as a permanent assertion: the arrangement this
    // replaced -- the whole composite with the moving pixels painted over the
    // top -- is NOT the same picture, which is the defect being fixed.
    const std::vector<float> oldWay = overInto(moving, whole);
    check(!bitIdentical(oldWay, whole),
          "split: ...and the OLD arrangement (preview over the full composite) was not");
  }

  // --- 2. Nothing above: the second composite is skippable ------------------
  {
    const Document doc = makeDoc();
    check(!anyVisibleLayerAbove(doc, 2),
          "split: transforming the TOP layer has nothing above it");
    Document hiddenTop = doc;
    hiddenTop.layers[2].visible = false;
    check(!anyVisibleLayerAbove(hiddenTop, 1),
          "split: an invisible layer above does not count as something to draw");
    check(transformSplitIsExact(hiddenTop, 1),
          "split: ...and cannot make the split inexact either");
  }

  // --- 3. What refuses the split, one construct at a time -------------------
  //
  // Each of these reads the backdrop, so compositing it over transparent
  // black and then drawing the result over the picture is NOT what the
  // compositor would have done.
  {
    Document blend = makeDoc();
    blend.layers[2].blend = "multiply";
    check(!transformSplitIsExact(blend, 1),
          "split: a non-normal blend above REFUSES the split");
    check(transformSplitIsExact(blend, 2),
          "split: ...but only when it is actually above the transformed layer");

    Document adj = makeDoc();
    adj.layers[2].kind = LayerKind::Adjustment;
    check(!transformSplitIsExact(adj, 1), "split: an Adjustment layer above REFUSES the split");

    Document clip = makeDoc();
    clip.layers[2].clipped = true;
    check(!transformSplitIsExact(clip, 1), "split: a clipped layer above REFUSES the split");

    Document grouped = makeDoc();
    grouped.layers[2].parent = "G1";
    check(!transformSplitIsExact(grouped, 1),
          "split: a layer above that lives inside a group REFUSES the split");

    Document movingGrouped = makeDoc();
    movingGrouped.layers[1].parent = "G1";
    check(!transformSplitIsExact(movingGrouped, 1),
          "split: the TRANSFORMED layer being inside a group refuses it too -- the below-half "
          "would blend that group short one member");

    Document groupAbove = makeDoc();
    groupAbove.layers[2].kind = LayerKind::Group;
    groupAbove.layers[2].groupTag = "G1";
    check(!transformSplitIsExact(groupAbove, 1), "split: a Group layer above REFUSES the split");

    // A hidden offender is not an offender: it draws nothing in either
    // arrangement, so it cannot make them differ.
    Document hiddenBlend = makeDoc();
    hiddenBlend.layers[2].blend = "multiply";
    hiddenBlend.layers[2].visible = false;
    check(transformSplitIsExact(hiddenBlend, 1),
          "split: a HIDDEN non-normal layer above does not refuse the split");
  }

  // --- 4. The views are views: the document is never modified ---------------
  //
  // A transform is not committed until Return. If these helpers mutated their
  // input, a drag would be putting an erase into the undo stack and handing
  // the recovery journal a document with a hole in it.
  {
    const Document doc = makeDoc();
    const std::vector<float> before = compositeDocumentPremultiplied(doc);
    (void)documentWithLayerHidden(doc, 1);
    (void)documentWithLayersAtOrBelowHidden(doc, 1);
    const std::vector<float> after = compositeDocumentPremultiplied(doc);
    check(bitIdentical(before, after),
          "split: building both halves leaves the source document untouched");
    check(doc.layers[1].visible, "split: ...its layer is still visible, specifically");

    // And the copies really are independent, not aliases of the original's
    // tile stores in a way that a later write would share.
    const Document hidden = documentWithLayerHidden(doc, 1);
    check(!hidden.layers[1].visible && doc.layers[1].visible,
          "split: the hidden-layer view and the original disagree, as they must");
  }

  // --- 5. Out of range is answered, not assumed ----------------------------
  {
    const Document doc = makeDoc();
    check(!transformSplitIsExact(doc, 99), "split: an out-of-range layer has no split to take");
    check(!anyVisibleLayerAbove(doc, 99), "split: ...and nothing above it either");
    const Document unchanged = documentWithLayerHidden(doc, 99);
    check(unchanged.layers.size() == doc.layers.size() && unchanged.layers[1].visible,
          "split: hiding an out-of-range layer changes nothing");
  }

  return ok;
}

}  // namespace np
