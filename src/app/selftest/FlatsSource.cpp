#include "app/selftest/Support.hpp"

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "core/LayerOps.hpp"
#include "flats/FlatsLayer.hpp"

namespace np {

// ---------------------------------------------------------------------------
// flats/FlatsLayer's source resolution -- WHAT a flats evaluation reads.
//
// A Flats layer used to read the composite of every layer beneath it, full
// stop. `core/Layer.hpp`'s `flatsReference` narrows that to the layers a
// painter named, and the narrowing is a **performance** feature at least as
// much as a correctness one: the evaluation cache is keyed on a signature of
// everything the evaluation read, so with the default source, editing a
// colour rough that happens to sit under the inks re-runs a segmentation that
// costs ~215 ms. Naming the line art makes that edit free.
//
// That is exactly why the signature deserves assertions of its own. Every way
// this can be wrong is silent:
//
//   * a reference marked ABOVE the Flats layer counting as a source would ask
//     the layer to segment its own result -- and it would still produce a
//     picture, just a nonsensical one that changes as the flats change.
//   * the fallback firing the wrong way. Nothing marked must mean "everything
//     below", not "nothing": an empty source segments a blank page, which
//     looks like a broken document rather than a missing rule.
//   * the signature covering more than the source. If it still walked every
//     layer beneath, marking a reference would fix the segmentation and fix
//     none of the cost -- the feature would look like it worked while the
//     drag stayed at four frames a second, and no other assertion here would
//     notice.
//   * the DEFAULT changing. The whole point of the fallback is that a
//     document with nothing marked flats exactly as it did before this
//     existed, so the default source is asserted to be the literal prefix.
namespace {

Document stackDocument(int layers) {
  Document doc = Document::createBlank(16, 16, WorkingSpace{});
  for (int i = 1; i < layers; ++i) addLayer(doc, static_cast<size_t>(i), makeRgbLayer("art"));
  return doc;
}

void paint(Document& doc, size_t layer, int x, int y) {
  const PixelCoord at{x, y};
  doc.layers[layer].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at),
                                                                     {0.0f, 0.0f, 0.0f, 1.0f});
}

}  // namespace

bool runFlatsSourceTest() {
  bool ok = true;
  auto check = [&ok](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Part A: which layers are the source -------------------------------
  {
    // Four RGB layers, a Flats layer on top of the first three.
    Document doc = stackDocument(4);
    addLayer(doc, 3, makeFlatsLayer("Flats"));
    check(doc.layers.size() == 5 && doc.layers[3].kind == LayerKind::Flats,
          "flats source: the fixture is three RGB layers, a Flats layer, then one more above");

    const std::vector<size_t> none = flatsSourceLayers(doc, 3);
    check(none == std::vector<size_t>({0, 1, 2}),
          "flats source: with nothing marked the source is EVERY layer below, in stack order -- "
          "byte for byte what it was before a reference existed");

    doc.layers[1].flatsReference = true;
    check(flatsSourceLayers(doc, 3) == std::vector<size_t>({1}),
          "flats source: one marked layer below replaces the whole prefix");

    doc.layers[0].flatsReference = true;
    check(flatsSourceLayers(doc, 3) == std::vector<size_t>({0, 1}),
          "flats source: two marked layers are BOTH read, in stack order -- the flag is per-layer, "
          "so N references cost nothing to allow");

    // The one marked layer is above the Flats layer, so it is out of scope
    // and the fallback must fire rather than the source being empty.
    doc.layers[0].flatsReference = false;
    doc.layers[1].flatsReference = false;
    doc.layers[4].flatsReference = true;
    check(flatsSourceLayers(doc, 3) == std::vector<size_t>({0, 1, 2}),
          "flats source: a reference marked ABOVE the Flats layer is not a source, and falls back "
          "to everything below rather than to nothing");
  }

  // --- Part B: the signature covers the source and nothing else ----------
  //
  // This is the assertion the performance claim rests on.
  {
    Document doc = stackDocument(3);
    addLayer(doc, 2, makeFlatsLayer("Flats"));
    doc.layers[0].flatsReference = true;

    const std::vector<size_t> source = flatsSourceLayers(doc, 2);
    check(source == std::vector<size_t>({0}), "flats source: layer 0 is the only reference");
    const uint64_t before = flatsSourceSignature(doc, source);

    paint(doc, 1, 4, 4);
    check(flatsSourceSignature(doc, flatsSourceLayers(doc, 2)) == before,
          "flats source: painting on a layer that is NOT the reference leaves the signature alone "
          "-- so it does not re-flat, which is the whole point of naming one");

    paint(doc, 0, 4, 4);
    check(flatsSourceSignature(doc, flatsSourceLayers(doc, 2)) != before,
          "flats source: painting on the reference DOES change the signature");

    // Visibility is identity, not content: hiding a source must be a change.
    const uint64_t painted = flatsSourceSignature(doc, flatsSourceLayers(doc, 2));
    doc.layers[0].visible = false;
    check(flatsSourceSignature(doc, flatsSourceLayers(doc, 2)) != painted,
          "flats source: hiding a source layer changes the signature");
  }

  // --- Part B2: the composite the source produces -------------------------
  //
  // The pixels, not the index list. `flatsSourceRgba8()` builds a document
  // holding only the source layers, and a subset breaks the two
  // relationships that reach across layers -- a clip reads whatever is
  // directly below it, and a group member names its Group. An earlier version
  // of this cleared both flags for every source, on the reasoning that the
  // default source is a contiguous prefix and so could not be affected. That
  // was wrong in the most invisible possible way: a clipped layer inside the
  // prefix DOES still have its base inside the prefix, so clearing the flag
  // stopped it clipping and silently changed the segmentation of every
  // document that used one -- and no assertion in this suite noticed.
  {
    // Layer 0 opaque black over the whole image; layer 1 white, but clipped
    // to layer 2's alpha... which does not exist, so the honest fixture is:
    // layer 0 covers a corner, layer 1 is white everywhere and CLIPPED to it.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) paint(doc, 0, x, y);  // black, top-left quadrant only
    addLayer(doc, 1, makeRgbLayer("clipped white"));
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) {
        const PixelCoord at{x, y};
        doc.layers[1].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at),
                                                                        {1.f, 1.f, 1.f, 1.f});
      }
    doc.layers[1].clipped = true;
    addLayer(doc, 2, makeFlatsLayer("Flats"));

    const std::vector<uint8_t> clipped = flatsSourceRgba8(doc, flatsSourceLayers(doc, 2));
    doc.layers[1].clipped = false;
    const std::vector<uint8_t> unclipped = flatsSourceRgba8(doc, flatsSourceLayers(doc, 2));
    check(clipped.size() == 8 * 8 * 4 && clipped != unclipped,
          "flats source: the DEFAULT source honours a clip -- clearing the flag changes the "
          "pixels the segmentation reads, so the composite really is going through the clip and "
          "not around it");
    // The clip's own signature: outside layer 0's quadrant the white is
    // masked away, so those texels are transparent rather than white.
    const size_t outside = (static_cast<size_t>(6) * 8 + 6) * 4;
    check(clipped[outside + 3] == 0 && unclipped[outside + 3] == 255,
          "flats source: ...and it is the clip specifically -- a texel outside the base's alpha "
          "is transparent with the flag on and opaque white with it off");
  }

  // --- Part C: the bake's three source modes -----------------------------
  //
  // `ExcludeTarget` is the default because the old behaviour was a defect:
  // the bake segmented the whole composite INCLUDING the layer it was about
  // to write into, so a second fill saw the first fill's pixels as line art.
  {
    Document doc = stackDocument(3);
    check(flatsBakeSourceLayers(doc, 1, FlatsBakeSource::AllLayers) ==
              std::vector<size_t>({0, 1, 2}),
          "flats bake: All layers segments the whole composite, the target included");
    check(flatsBakeSourceLayers(doc, 1, FlatsBakeSource::ExcludeTarget) ==
              std::vector<size_t>({0, 2}),
          "flats bake: Below fill drops the target, so the last fill's own pixels are not read "
          "back as line art by the next one");

    check(flatsBakeSourceLayers(doc, 1, FlatsBakeSource::ReferenceLayer) ==
              std::vector<size_t>({0, 1, 2}),
          "flats bake: Reference with nothing marked falls back to the whole composite rather "
          "than segmenting a blank page");
    doc.layers[2].flatsReference = true;
    check(flatsBakeSourceLayers(doc, 1, FlatsBakeSource::ReferenceLayer) ==
              std::vector<size_t>({2}),
          "flats bake: Reference reads exactly the marked layers -- and unlike a Flats layer's "
          "own source, a mark ABOVE the target counts, because a bake has no 'beneath'");
  }

  // --- Part D: the layer command ------------------------------------------
  {
    OpenDocument od;
    od.document = stackDocument(2);
    addLayer(od.document, 2, makeFlatsLayer("Flats"));
    od.history.begin("open", od.document);

    LayerEditResult r = applyLayerCommand(od, LayerCommand::ToggleFlatsReference, 0);
    check(r.ok && od.document.layers[0].flatsReference,
          "flats reference: the command marks the layer");
    r = applyLayerCommand(od, LayerCommand::ToggleFlatsReference, 0);
    check(r.ok && !od.document.layers[0].flatsReference,
          "flats reference: and the same command clears it again");

    // A Flats layer nominating itself would ask it to read its own result.
    r = applyLayerCommand(od, LayerCommand::ToggleFlatsReference, 2);
    check(!r.ok && !od.document.layers[2].flatsReference,
          "flats reference: a Flats layer refuses to be its own reference, with a sentence rather "
          "than a control that silently does nothing");
    // Refused going ON, always allowed going OFF -- the rule every other flag
    // in core/LayerOps follows, so a hand-edited file can always be escaped.
    od.document.layers[2].flatsReference = true;
    r = applyLayerCommand(od, LayerCommand::ToggleFlatsReference, 2);
    check(r.ok && !od.document.layers[2].flatsReference,
          "flats reference: ...but unmarking a Flats layer that somehow carries the flag is "
          "always allowed");

    check(layerCommandAvailable(od.document, LayerCommand::ToggleFlatsReference, 0),
          "flats reference: the command is offered on an ordinary layer");
  }

  std::printf("[selftest] flats source %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
