#include "app/selftest/Support.hpp"

namespace np {

bool runLayerCompTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif
  // PLAN.md §1.5. The model, the panel and the `np:comps` carrier are pure and
  // answer identically in both configurations; only the `.npaint` round trip
  // differs, and part G asserts the correct answer for each rather than being
  // compiled out of either.
  std::printf("[selftest] layer comps: NP_USE_OIIO=%s -- the model, app/CompPanel and "
              "io/CompSerial answer identically in both configurations; the `.npaint` round "
              "trip in part G is the only thing that differs, and the OFF answer is asserted "
              "rather than skipped\n",
              kOiioBuild ? "ON" : "OFF");

  // **PRD C14 asks for three things and this step delivers two.** Printed
  // rather than left in a header, because a requirement two thirds met is the
  // thing a reader most needs told outright.
  std::printf("  C14 asks for \"visibility, position and properties\". Captured: visible, "
              "opacity, blend, clipped (4 properties).\n");
  std::printf("  NOT captured -- **position**: core::Layer has no offset, origin or transform "
              "field, and tiles are keyed by absolute document TileCoord, so there is no number "
              "to capture and nothing a restore could write back. Inventing one to satisfy the "
              "wording would round-trip two zeros and claim C14 was met.\n");
  std::printf("  Also not captured, each for a stated reason (core/LayerComp.hpp): the mask (a "
              "restore would have to DISCARD mask pixels), locked (a working state, not an "
              "appearance), name (the panel's own handle on a layer), ops (unbounded authored "
              "content; core::History already holds whole states).\n");

  // --- Fixtures -----------------------------------------------------------
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto composite = [](const Document& doc) { return compositeDocumentPremultiplied(doc); };
  auto samePixels = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && !a.empty() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };
  auto sameImage = [](const DecodedImage& a, const DecodedImage& b) {
    return a.pixels.size() == b.pixels.size() && !a.pixels.empty() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0;
  };

  // Two visibly different layers over an opaque base: a comp switch has to be
  // a change in a picture, not a change in a flag.
  constexpr int32_t kW = 64, kH = 64;
  auto makeThreeLayerDoc = []() {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers[0].name = "Base";
    addLayer(doc, 1, makeRgbLayer("Red pass"));
    addLayer(doc, 2, makeRgbLayer("Green pass"));
    return doc;
  };

  // --- Part A: the four captured properties, in pixels --------------------
  {
    Document doc = makeThreeLayerDoc();
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) {
        writeRgb(doc, 0, x, y, {0.2f, 0.2f, 0.2f, 1.0f});
        writeRgb(doc, 1, x, y, {0.8f, 0.0f, 0.0f, 1.0f});
        writeRgb(doc, 2, x, y, {0.0f, 0.8f, 0.0f, 1.0f});
      }

    // State 1: everything on.
    const LayerOpResult capA = captureLayerComp(doc, "All on");
    check(capA.ok && doc.comps.size() == 1 && capA.index == 0,
          "capture: a comp is appended and its index reported");
    check(contains(capA.editLabel, "capture comp \"All on\""),
          "capture: the edit label names the comp, so a history row reads as the gesture");
    const std::vector<float> allOn = composite(doc);

    // Every layer got a distinct, nonzero id, and nothing else about them moved.
    check(doc.layers[0].id != 0 && doc.layers[1].id != 0 && doc.layers[2].id != 0 &&
              doc.layers[0].id != doc.layers[1].id && doc.layers[1].id != doc.layers[2].id &&
              doc.layers[0].id != doc.layers[2].id,
          "capture: every layer has a distinct nonzero id afterwards");
    check(doc.comps[0].layers.size() == 3 &&
              doc.comps[0].layers[1].layerId == doc.layers[1].id &&
              doc.comps[0].layers[1].nameAtCapture == "Red pass",
          "capture: entries carry the layer's id AND its name at capture");

    // State 2: the four properties, each set to something different.
    setLayerVisible(doc, 2, false);
    setLayerOpacity(doc, 1, 0.25f);
    setLayerBlend(doc, 1, BlendMode::Multiply);
    setLayerClipped(doc, 1, true);
    const LayerOpResult capB = captureLayerComp(doc, "Red only, multiplied");
    check(capB.ok && doc.comps.size() == 2, "capture: a second comp of a different state");
    const std::vector<float> variant = composite(doc);
    check(!samePixels(allOn, variant),
          "the two comps really are two different pictures, so everything below is about "
          "pixels and not about flags");

    // Restore comp 0 and get the first picture back, exactly.
    LayerCompRestoreReport report;
    const LayerOpResult back = restoreLayerComp(doc, 0, &report);
    check(back.ok && report.fullyApplied() && report.entriesApplied == 3,
          "restore: all three entries applied, nothing outstanding");
    check(samePixels(composite(doc), allOn),
          "restore: comp 0 gives back its picture BYTE-IDENTICALLY, which is the only claim "
          "that covers visible, opacity, blend and clipped at once");
    check(doc.layers[1].opacity == 1.0f && doc.layers[1].blend == "normal" &&
              !doc.layers[1].clipped && doc.layers[2].visible,
          "restore: each of the four properties individually came back");

    // And back to the other one.
    const LayerOpResult again = restoreLayerComp(doc, 1, nullptr);
    check(again.ok && samePixels(composite(doc), variant),
          "restore: switching to the other comp gives back the other picture, byte-identically");

    // Restoring twice running is a no-op that says so rather than a silent one.
    LayerCompRestoreReport twice;
    restoreLayerComp(doc, 1, &twice);
    check(twice.layersChanged == 0 && twice.entriesApplied == 3 &&
              contains(layerCompRestoreSummary(twice), "already in this comp's state"),
          "restore: restoring the comp already showing changes nothing and says why");
  }

  // --- Part B: what a comp does NOT capture, proven by not restoring it ---
  //
  // Each exclusion in core/LayerComp.hpp section 2 is a promise that clicking a
  // comp will not silently overwrite that thing. Asserted, not commented.
  {
    Document doc = makeThreeLayerDoc();
    addLayerMask(doc, 1);
    doc.layers[1].ops.add([] {
      Op op;
      op.opClass = OpClass::PointA;
      op.pointKind = PointOpKind::Exposure;
      op.exposure.stops = 1.0f;
      op.enabled = true;
      return op;
    }());
    setLayerName(doc, 1, "Red pass");
    captureLayerComp(doc, "with a mask, an op and a name");

    // Change every excluded property, then restore.
    removeLayerMask(doc, 1);
    doc.layers[1].ops.remove(0);
    setLayerName(doc, 1, "renamed by hand");
    setLayerLocked(doc, 2, true);
    restoreLayerComp(doc, 0, nullptr);

    check(!doc.layers[1].mask.has_value(),
          "excluded: the mask is NOT restored -- a restore that put one back would have had to "
          "invent its samples, and one that took it away would DISCARD them");
    check(doc.layers[1].ops.size() == 0,
          "excluded: the per-layer op stack is NOT restored, so authored grade work survives a "
          "comp click");
    check(doc.layers[1].name == "renamed by hand",
          "excluded: the name is NOT restored -- the panel's rows do not change words when the "
          "picture does");
    check(doc.layers[2].locked,
          "excluded: the lock is NOT restored -- a comp cannot unlock a layer that was locked "
          "to protect it");
  }

  // --- Part C: the layer-set mismatch, and the index-keyed alternative ----
  //
  // The design work of this step. A comp outlives the stack it was captured
  // over; every delete, add and reorder moves every index above it.
  {
    Document doc = Document::createBlank(kW, kH, WorkingSpace{});
    doc.layers[0].name = "A";
    const char* kNames[] = {"B", "C", "D", "E"};
    for (size_t i = 0; i < 4; ++i) addLayer(doc, i + 1, makeRgbLayer(kNames[i]));
    // A distinct opacity per layer, so "which layer got which state" is a
    // number rather than a judgement.
    for (size_t i = 0; i < 5; ++i)
      setLayerOpacity(doc, i, static_cast<float>(i + 1) / 10.0f);
    captureLayerComp(doc, "five layers");
    const LayerComp comp = doc.comps[0];

    // The stack moves: B is deleted and a new layer is added on top.
    removeLayer(doc, 1);
    addLayer(doc, doc.layers.size(), makeRgbLayer("New"));
    for (size_t i = 0; i < doc.layers.size(); ++i) setLayerOpacity(doc, i, 1.0f);

    // The rejected alternative, implemented here and run on the same fixture:
    // a comp keyed by *position*, which is what an implementation that stored
    // no identity would have to do.
    Document byIndex = doc;
    for (size_t i = 0; i < comp.layers.size() && i < byIndex.layers.size(); ++i) {
      byIndex.layers[i].visible = comp.layers[i].visible;
      byIndex.layers[i].opacity = comp.layers[i].opacity;
      byIndex.layers[i].blend = comp.layers[i].blend;
      byIndex.layers[i].clipped = comp.layers[i].clipped;
    }
    size_t wrongLayers = 0;
    for (size_t i = 0; i < byIndex.layers.size(); ++i) {
      // The state this layer *should* have had, found by name.
      float want = 1.0f;
      for (const LayerCompEntry& e : comp.layers)
        if (e.nameAtCapture == byIndex.layers[i].name) want = e.opacity;
      if (byIndex.layers[i].opacity != want) ++wrongLayers;
    }

    LayerCompRestoreReport report;
    const LayerOpResult r = restoreLayerComp(doc, 0, &report);
    size_t misapplied = 0;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      float want = 1.0f;
      for (const LayerCompEntry& e : comp.layers)
        if (e.nameAtCapture == doc.layers[i].name) want = e.opacity;
      if (doc.layers[i].opacity != want) ++misapplied;
    }

    std::printf("  the same comp on the same changed stack: keyed by layer id, %zu of %zu "
                "layers hold a state captured from a different layer; keyed by index, %zu do\n",
                misapplied, doc.layers.size(), wrongLayers);
    check(r.ok && misapplied == 0,
          "mismatch: keyed by id, NOT ONE layer holds another layer's captured state");
    check(wrongLayers > 0,
          "mismatch: keyed by index, several do -- the comparison above is between two real "
          "answers, not a claim about one");
    check(report.entriesApplied == 4 && report.missingLayers.size() == 1 &&
              contains(report.missingLayers[0], "B"),
          "mismatch: the deleted layer is reported by the name it had at capture");
    check(report.uncoveredLayers.size() == 1 && contains(report.uncoveredLayers[0], "New"),
          "mismatch: the layer added since the capture is reported and left untouched");
    check(doc.layers.back().opacity == 1.0f,
          "mismatch: and it really was left untouched -- a comp has no opinion about a layer "
          "that did not exist when it was captured");
    const std::string summary = layerCompRestoreSummary(report);
    check(contains(summary, "1 layer state(s) could not be restored") &&
              contains(summary, "1 layer(s) were added after this comp was captured") &&
              contains(summary, "4 state(s) that did match were applied"),
          "mismatch: the summary carries all three numbers, in the house refusal style");
  }

  // --- Part D: a reorder alone changes nothing about a restore ------------
  {
    Document doc = makeThreeLayerDoc();
    for (size_t i = 0; i < 3; ++i) setLayerOpacity(doc, i, static_cast<float>(i + 1) / 10.0f);
    captureLayerComp(doc, "before the reorder");
    moveLayer(doc, 0, 2);  // the bottom layer to the top
    for (size_t i = 0; i < 3; ++i) setLayerOpacity(doc, i, 1.0f);
    LayerCompRestoreReport report;
    restoreLayerComp(doc, 0, &report);
    bool eachOwn = true;
    for (const Layer& layer : doc.layers) {
      const float want = layer.name == "Base"    ? 0.1f
                         : layer.name == "Red pass" ? 0.2f
                                                    : 0.3f;
      if (layer.opacity != want) eachOwn = false;
    }
    check(eachOwn && report.fullyApplied(),
          "reorder: every layer gets its OWN captured state back after the stack is reordered");
  }

  // --- Part E: the refusals, each with the numbers ------------------------
  {
    Document doc = makeThreeLayerDoc();
    captureLayerComp(doc, "one");

    LayerOpResult r = restoreLayerComp(doc, 7, nullptr);
    check(!r.ok && contains(r.error, "no comp at index 7") && contains(r.error, "1 comp(s)"),
          "refusal: an out-of-range comp index, with both numbers");

    // Ambiguity: two layers with one id. Unreachable from the panel
    // (`duplicateLayer()` clears the copy's id), so it is constructed here.
    Document ambiguous = doc;
    ambiguous.layers[2].id = ambiguous.layers[1].id;
    const std::vector<float> before = composite(ambiguous);
    const float opacityBefore = ambiguous.layers[1].opacity;
    r = restoreLayerComp(ambiguous, 0, nullptr);
    check(!r.ok && contains(r.error, "both carry layer id") && contains(r.error, "layers 1 and 2"),
          "refusal: two layers sharing an id refuses the WHOLE restore, naming both");
    check(samePixels(composite(ambiguous), before) &&
              ambiguous.layers[1].opacity == opacityBefore,
          "refusal: and nothing was changed -- the ambiguity is decided before the first write");

    // A comp whose layers are all gone: a comp for another document.
    Document foreign = makeThreeLayerDoc();
    foreign.comps = doc.comps;
    for (size_t i = 0; i < foreign.layers.size(); ++i) foreign.layers[i].id = 900 + i;
    r = restoreLayerComp(foreign, 0, nullptr);
    check(!r.ok && contains(r.error, "not one of") && contains(r.error, "3 layer state(s)"),
          "refusal: a comp none of whose layers are still here, rather than a silent no-op");

    // Duplicating a layer must not produce two layers with one id.
    Document dup = doc;
    duplicateLayer(dup, 1);
    check(dup.layers[2].id == 0 && dup.layers[1].id != 0,
          "duplicate: the copy gets NO id, so the source keeps the one every comp refers to");
    check(restoreLayerComp(dup, 0, nullptr).ok,
          "duplicate: so a restore straight after a duplicate is unambiguous and succeeds");

    // An empty document cannot be captured.
    Document empty = Document::createBlank(kW, kH, WorkingSpace{});
    empty.layers.clear();
    r = captureLayerComp(empty, "nothing");
    check(!r.ok && contains(r.error, "no layers"),
          "refusal: capturing a comp of a document with no layers");
  }

  // --- Part F: the lock, and a blend this build cannot set ----------------
  {
    Document doc = makeThreeLayerDoc();
    setLayerOpacity(doc, 1, 0.25f);
    setLayerBlend(doc, 1, BlendMode::Multiply);
    setLayerVisible(doc, 1, false);
    captureLayerComp(doc, "locked-layer target");
    setLayerVisible(doc, 1, true);
    setLayerOpacity(doc, 1, 1.0f);
    setLayerBlend(doc, 1, BlendMode::Normal);
    setLayerLocked(doc, 1, true);

    LayerCompRestoreReport report;
    const LayerOpResult r = restoreLayerComp(doc, 0, &report);
    check(r.ok && report.lockedLayers.size() == 1,
          "lock: the restore succeeds and names the one locked layer it could not fully apply");
    check(!doc.layers[1].visible,
          "lock: visibility IS restored on a locked layer -- core/LayerOps allows hiding one, "
          "and a lock that froze the eye icon is the behaviour every editor agrees is wrong");
    check(doc.layers[1].opacity == 1.0f && doc.layers[1].blend == "normal",
          "lock: opacity and blend are NOT -- the restore goes through the same setters, so "
          "there is no second copy of the lock rule here to drift from core/LayerOps'");
    check(contains(layerCompRestoreSummary(report), "1 locked layer(s) kept their opacity"),
          "lock: and the summary says so with the count and what to do about it");

    // A blend name from a newer build (PRD I10's value-level carry).
    Document newer = makeThreeLayerDoc();
    newer.layers[1].blend = "linear-burn";
    captureLayerComp(newer, "from a newer build");
    newer.layers[1].blend = "normal";
    LayerCompRestoreReport blendReport;
    restoreLayerComp(newer, 0, &blendReport);
    check(blendReport.unsettableBlends.size() == 1 &&
              contains(blendReport.unsettableBlends[0], "linear-burn"),
          "blend: a mode this build has no implementation for is NAMED, never substituted");
    check(newer.layers[1].blend == "normal",
          "blend: and the layer keeps what it had rather than being quietly set to Normal");
    check(newer.comps[0].layers[1].blend == "linear-burn",
          "blend: the comp still carries the value verbatim, so a build that knows it can "
          "restore it (PRD I10 at the value level)");
  }

  // --- Part G: restoring a comp is an EDIT --------------------------------
  {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{});
    od.document.layers[0].name = "Base";
    addLayer(od.document, 1, makeRgbLayer("Over"));
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x) {
        writeRgb(od.document, 0, x, y, {0.2f, 0.2f, 0.2f, 1.0f});
        writeRgb(od.document, 1, x, y, {0.0f, 0.0f, 0.7f, 1.0f});
      }
    od.recordEdit("fill", EditKind::Content);

    const DocumentOpResult cap =
        recordLayerEdit(od, captureLayerComp(od.document, "Over showing"));
    check(cap.ok && od.document.comps.size() == 1,
          "edit: capture goes through recordLayerEdit() like every other layer edit");

    setLayerVisible(od.document, 1, false);
    od.recordEdit("hide over");
    const std::vector<float> hidden = composite(od.document);
    const uint64_t revisionBefore = od.revision;
    const size_t historyBefore = od.history.entries().size();

    const DocumentOpResult res = recordLayerEdit(od, restoreLayerComp(od.document, 0, nullptr));
    check(res.ok && od.revision == revisionBefore + 1 &&
              od.history.entries().size() == historyBefore + 1,
          "edit: a restore moves the revision by exactly one and appends exactly one history "
          "entry -- ui/DocumentTexture caches by that number, so a restore that skipped it "
          "would never reach the screen");
    check(contains(od.history.entries().back().label, "restore comp \"Over showing\""),
          "edit: and the history row names the comp");
    check(!samePixels(composite(od.document), hidden),
          "edit: the restore really did change the picture");

    const Document* undone = od.history.undo();
    check(undone != nullptr, "edit: the restore is undoable");
    if (undone != nullptr) {
      od.document = *undone;
      check(samePixels(composite(od.document), hidden),
            "edit: undo returns the pre-restore picture BYTE-IDENTICALLY");
      check(od.document.comps.size() == 1,
            "edit: and the comp itself survives the undo, because it was captured before it");
    }

    // A refused restore records nothing at all.
    const uint64_t revisionAfter = od.revision;
    const size_t historyAfter = od.history.entries().size();
    const DocumentOpResult bad = recordLayerEdit(od, restoreLayerComp(od.document, 9, nullptr));
    check(!bad.ok && od.revision == revisionAfter &&
              od.history.entries().size() == historyAfter,
          "edit: a refused restore moves neither the revision nor the history");

    // Undo takes a capture back too, which is the property that made
    // `Document::comps` the right home for the list (core/Document.hpp).
    OpenDocument od2 = makeBlankOpenDocument(kW, kH, WorkingSpace{});
    recordLayerEdit(od2, captureLayerComp(od2.document, "one"));
    const Document* beforeCapture = od2.history.undo();
    check(beforeCapture != nullptr && beforeCapture->comps.empty(),
          "edit: undoing a capture removes the comp, because the comp list is inside every "
          "history entry rather than beside them");
  }

  // --- Part H: the command list and the panel -----------------------------
  {
    bool captureListed = false;
    for (const LayerCommand c : allLayerCommands())
      if (c == LayerCommand::CaptureComp) captureListed = true;
    check(captureListed &&
              std::string(layerCommandLabel(LayerCommand::CaptureComp)) == "Capture Layer Comp",
          "panel: Capture Layer Comp is in the list the `Layer` menu and the panel both walk");

    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{});
    check(layerCommandAvailable(od.document, LayerCommand::CaptureComp, 0),
          "panel: capture is available regardless of which layer is selected");
    Document noLayers = od.document;
    noLayers.layers.clear();
    check(!layerCommandAvailable(noLayers, LayerCommand::CaptureComp, 0),
          "panel: and unavailable with no layers, which is the one case core refuses");

    const LayerEditResult cap = applyLayerCommand(od, LayerCommand::CaptureComp, 0);
    check(cap.ok && od.document.comps.size() == 1 && od.document.comps[0].name == "Comp 1" &&
              cap.selected == 0,
          "panel: the command captures under an allocated name and does not move the selection");
    applyLayerCommand(od, LayerCommand::CaptureComp, 0);
    check(od.document.comps[1].name == "Comp 2", "panel: names are allocated, not counted");
    deleteLayerComp(od.document, 0);
    check(defaultNewCompName(od.document) == "Comp 3",
          "panel: deleting Comp 1 does not make the next one Comp 2 again");

    // Rows and row text.
    Document doc = makeThreeLayerDoc();
    captureLayerComp(doc, "Full");
    captureLayerComp(doc, "");
    removeLayer(doc, 2);
    const std::vector<CompPanelRow> rows = compPanelRows(doc);
    check(rows.size() == 2 && rows[0].index == 0 && rows[0].name == "Full",
          "panel: rows are in model order -- no reversal, unlike the layers panel");
    check(rows[0].captured == 3 && rows[0].stillHere == 2 && compRowIsPartial(rows[0]),
          "panel: a row counts how many of its layers are still here, so a partial restore is "
          "legible BEFORE the click");
    check(compRowText(rows[0]) == "Full \xC2\xB7 3 layers \xC2\xB7 2 still here",
          "panel: and the row says all three numbers");
    check(compRowText(rows[1]).rfind("(unnamed comp)", 0) == 0,
          "panel: an unnamed comp reads (unnamed comp), so a row can never be blank");

    Document whole = makeThreeLayerDoc();
    captureLayerComp(whole, "Full");
    const std::vector<CompPanelRow> wholeRows = compPanelRows(whole);
    check(!compRowIsPartial(wholeRows[0]) &&
              compRowText(wholeRows[0]) == "Full \xC2\xB7 3 layers",
          "panel: a comp that still matches its document stays quiet");

    // Rename, delete and reorder.
    check(renameLayerComp(doc, 1, "Second").ok && doc.comps[1].name == "Second",
          "panel: rename");
    check(moveLayerComp(doc, 1, 0).ok && doc.comps[0].name == "Second" &&
              doc.comps[1].name == "Full",
          "panel: reorder, with moveLayer()'s own before-the-move index semantics");
    check(moveLayerComp(doc, 0, 0).ok,
          "panel: a no-op reorder succeeds and changes nothing, matching moveLayer()");
    check(deleteLayerComp(doc, 0).ok && doc.comps.size() == 1 && doc.comps[0].name == "Full",
          "panel: delete removes the right one");
    check(!deleteLayerComp(doc, 4).ok && !renameLayerComp(doc, 4, "x").ok &&
              !moveLayerComp(doc, 0, 4).ok,
          "panel: all three refuse an out-of-range index rather than clamping");

    // Which way is up. This panel is NOT reversed, so up the panel is DOWN
    // the index -- the opposite of the layers panel, which is exactly the
    // mistake app/LayerPanel.hpp warns about and exactly the one the comps
    // draw loop made before this pair of functions existed.
    check(compRowMoveUpTarget(2, 4) == 1 && compRowMoveDownTarget(2, 4) == 3,
          "panel: up the panel is DOWN the index here, the opposite of the layers panel");
    check(compRowMoveUpTarget(0, 4) == kNoCompRow &&
              compRowMoveDownTarget(3, 4) == kNoCompRow &&
              compRowMoveUpTarget(0, 0) == kNoCompRow &&
              compRowMoveDownTarget(0, 0) == kNoCompRow,
          "panel: both ends, and an empty list, report no target rather than wrapping");
    bool inverse = true;
    for (size_t r = 0; r < 4; ++r) {
      const size_t up = compRowMoveUpTarget(r, 4);
      if (up != kNoCompRow && compRowMoveDownTarget(up, 4) != r) inverse = false;
      const size_t down = compRowMoveDownTarget(r, 4);
      if (down != kNoCompRow && compRowMoveUpTarget(down, 4) != r) inverse = false;
    }
    check(inverse,
          "panel: and the two are each other's inverse wherever both are defined, so a row "
          "moved up and back down is where it started");
  }

  // --- Part I: io/CompSerial, the `np:comps` carrier ----------------------
  //
  // docs/document-format.md called `np:comps` a `<blob>`, and its own measured
  // warning says array-typed attributes are silently absent when read back
  // through this OpenImageIO -- io/NpaintFile *refuses* such a save by name, so
  // a blob comp list would have made every save of a document with comps fail.
  // The table is corrected as part of this step; this is the carrier that
  // replaces it.
  {
    LayerCompCarrier in;
    in.nextLayerId = 12;
    in.layerIds = {{"L0001", 4}, {"L0007", 11}};
    LayerComp comp;
    comp.name = "Cool variant";
    comp.layers = {LayerCompEntry{4, false, true, 0.25f, "multiply", "Sky"},
                   LayerCompEntry{11, true, false, 1.0f, "normal", ""}};
    in.comps.push_back(comp);

    const std::string encoded = serializeLayerComps(in);
    check(encoded.rfind(kLayerCompSerialPrefix, 0) == 0,
          "serial: the value begins with the version prefix, so a reader decides whether it "
          "understands the encoding before decoding a byte");
    LayerCompCarrier out;
    std::string why;
    check(deserializeLayerComps(encoded, &out, &why) && out == in,
          "serial: a carrier round-trips exactly -- ids, counter, names, all four properties");
    check(out.comps[0].layers[0].opacity == 0.25f,
          "serial: opacity travels as an IEEE-754 bit pattern, so it reopens as what was "
          "captured rather than as nearly it");

    // A payload typed out by hand, so the decoder is checked against the spec
    // and not only against this project's own encoder -- io/OpSerial.hpp's own
    // reason for choosing hex over base64, and io/NpaintFile's hand-built PSD
    // fixture's reason before that.
    const std::string hand =
        std::string(kLayerCompSerialPrefix) +
        "0400000000000000"          // nextLayerId = 4
        "0200"                      // 2 layers
        "0500" "4c30303031" "0100000000000000"   // "L0001" -> id 1
        "0500" "4c30303032" "0300000000000000"   // "L0002" -> id 3
        "0100"                      // 1 comp
        "26000000"                  // record length 38
        "00"                        // reserved
        "0400" "436f6f6c"           // name "Cool"
        "0100"                      // 1 entry
        "0300000000000000"          // layerId 3
        "00" "01"                   // visible 0, clipped 1
        "0000003f"                  // opacity 0.5
        "0800" "6d756c7469706c79"   // blend "multiply"
        "0300" "536b79";            // nameAtCapture "Sky"
    LayerCompCarrier byHand;
    const bool handOk = deserializeLayerComps(hand, &byHand, &why);
    check(handOk && byHand.nextLayerId == 4 && byHand.layerIds.size() == 2 &&
              byHand.layerIds[1].first == "L0002" && byHand.layerIds[1].second == 3,
          "serial: a hand-written payload decodes -- the header and the layer id table");
    check(handOk && byHand.comps.size() == 1 && byHand.comps[0].name == "Cool" &&
              byHand.comps[0].layers.size() == 1 && byHand.comps[0].layers[0].layerId == 3 &&
              !byHand.comps[0].layers[0].visible && byHand.comps[0].layers[0].clipped &&
              byHand.comps[0].layers[0].opacity == 0.5f &&
              byHand.comps[0].layers[0].blend == "multiply" &&
              byHand.comps[0].layers[0].nameAtCapture == "Sky",
          "serial: ...and every field of the entry inside it");
    check(handOk && serializeLayerComps(byHand) == hand,
          "serial: and re-encoding it reproduces the hand-written bytes exactly");

    // A future version is refused by name, not misread.
    std::string futureWhy;
    LayerCompCarrier ignored;
    check(!deserializeLayerComps("npcomps2:00", &ignored, &futureWhy) &&
              contains(futureWhy, "npcomps2:") && contains(futureWhy, "version 1 only") &&
              contains(futureWhy, "PRD I10"),
          "serial: a newer version tag is refused BY NAME and says it will be carried");

    // An unrecognised comp *record* survives, in position.
    std::vector<LayerComp> three = {comp, LayerComp{}, comp};
    three[1].known = false;
    three[1].unrecognised = {0x07, 0x99, 0x12};  // reserved byte 7 -> unrecognisable
    LayerCompCarrier mixed = in;
    mixed.comps = three;
    LayerCompCarrier backMixed;
    check(deserializeLayerComps(serializeLayerComps(mixed), &backMixed, &why) &&
              backMixed.comps.size() == 3 && !backMixed.comps[1].known &&
              backMixed.comps[1].unrecognised == three[1].unrecognised &&
              backMixed.comps[0].known && backMixed.comps[2].known,
          "serial: a comp record this build cannot read survives WHOLE and IN POSITION, and "
          "its neighbours still decode (PRD I10 one level below the attribute)");
    Document carriedDoc = makeThreeLayerDoc();
    carriedDoc.comps = backMixed.comps;
    const LayerOpResult carriedRestore = restoreLayerComp(carriedDoc, 1, nullptr);
    check(!carriedRestore.ok && contains(carriedRestore.error, "does not read") &&
              contains(carriedRestore.error, "3-byte record"),
          "serial: and restoring it is refused by name rather than applying an empty comp, "
          "which would look like a comp that does nothing");

    // Container-level malformations, each named.
    check(!deserializeLayerComps(std::string(kLayerCompSerialPrefix) + "abc", &ignored, &why) &&
              contains(why, "odd number"),
          "serial: an odd hex payload is refused");
    check(!deserializeLayerComps(std::string(kLayerCompSerialPrefix) + "zz", &ignored, &why) &&
              contains(why, "not a hex digit"),
          "serial: a non-hex digit is refused, naming the offset");
    check(!deserializeLayerComps(encoded + "00", &ignored, &why) &&
              contains(why, "follow the"),
          "serial: trailing bytes after the last record are refused rather than ignored");
    check(!deserializeLayerComps(std::string(kLayerCompSerialPrefix) + "0000", &ignored, &why) &&
              contains(why, "too short"),
          "serial: a truncated header is refused");

    // The measured bound this carrier costs, which is deterministic and so is
    // NOT marked [measured]: the same document always produces the same bytes.
    Document sized = Document::createBlank(kW, kH, WorkingSpace{});
    for (size_t i = 1; i < 20; ++i) addLayer(sized, i, makeRgbLayer("Layer " + std::to_string(i)));
    captureLayerComp(sized, "Comp 1");
    LayerCompCarrier big;
    big.nextLayerId = sized.nextLayerId;
    for (size_t i = 0; i < sized.layers.size(); ++i)
      big.layerIds.emplace_back("L" + std::to_string(1000 + i), sized.layers[i].id);
    big.comps = sized.comps;
    const size_t oneComp = serializeLayerComps(big).size();
    big.comps.push_back(sized.comps[0]);
    const size_t twoComps = serializeLayerComps(big).size();
    std::printf("  np:comps for a 20-layer document: %zu characters for one comp, %zu for two, "
                "so %zu per extra comp = %.1f per layer state\n",
                oneComp, twoComps, twoComps - oneComp,
                static_cast<double>(twoComps - oneComp) / 20.0);
    check(twoComps - oneComp < 2000,
          "serial: a comp of a 20-layer document costs well under 2 KB of attribute, which is "
          "why there is no compression here");
  }

  // --- Part J: persistence through io/NpaintFile --------------------------
  {
    const char* kBare = "selftest_comps_bare.npaint";
    const char* kWith = "selftest_comps.npaint";
    const char* kCleared = "selftest_comps_cleared.npaint";
    for (const char* p : {kBare, kWith, kCleared}) std::remove(p);

    auto bytesWithoutCapDate = [](const char* path) -> std::vector<unsigned char> {
      std::ifstream in(path, std::ios::binary);
      std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
      static const std::string kNeedle = "capDate";
      for (size_t i = 0; i + kNeedle.size() <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, kNeedle.data(), kNeedle.size()) != 0) continue;
        for (size_t j = i; j < std::min(i + 47, b.size()); ++j) b[j] = 0;
      }
      return b;
    };

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0].name = "Base";
    writeRgb(doc, 0, 3, 4, {0.5f, 0.25f, 0.125f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("Over"));
    writeRgb(doc, 1, 3, 4, {0.0f, 0.0f, 0.5f, 0.5f});

    // The comp-free file first: the reference for the regression boundary.
    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok == kOiioBuild && (kOiioBuild || contains(bare.error, "NP_USE_OIIO")),
          kOiioBuild ? "npaint: the two-layer fixture saves with no comps"
                     : "npaint: saving is refused in the NP_USE_OIIO=OFF build, naming the "
                       "build option, exactly as it is for every other attribute");

    setLayerVisible(doc, 1, false);
    setLayerOpacity(doc, 1, 0.25f);
    captureLayerComp(doc, "Over hidden");
    setLayerVisible(doc, 1, true);
    setLayerOpacity(doc, 1, 1.0f);
    captureLayerComp(doc, "Over showing");
    check(doc.comps.size() == 2 && doc.layers[0].id != 0,
          "npaint: the fixture has two comps and assigned layer ids");

    if (!kOiioBuild) {
      // PLAN.md §1.5: say what happens rather than compiling the test away.
      // The carrier is pure and was proven above in this same build; what is
      // missing is a writer, and it refuses by name.
      const NpaintSaveResult refused = saveNpaint(doc, kWith);
      check(!refused.ok && contains(refused.error, "NP_USE_OIIO") &&
                contains(refused.error, ".npaint"),
            "npaint: with comps, the OFF build refuses the save naming .npaint and the build "
            "option -- there is no second writer for comps and there must not be (PRD I7/I4)");
      std::printf("  NP_USE_OIIO=OFF: comps are fully live in memory and io/CompSerial "
                  "round-trips them in this build (asserted in part I); what is absent is the "
                  "`.npaint` writer itself, so nothing about comps degrades silently -- the "
                  "save refuses with the same sentence every other attribute gets\n");
    } else {
      const NpaintSaveResult saved = saveNpaint(doc, kWith);
      check(saved.ok && saved.partsWritten == 3,
            "npaint: a document with comps saves, as the same three parts as without them");
      const NpaintLoadResult back = loadNpaint(kWith);
      check(back.ok && back.warnings.empty() && back.document.comps.size() == 2,
            "npaint: and it loads back clean, with both comps");
      if (back.ok && back.document.comps.size() == 2) {
        check(back.document.comps == doc.comps,
              "npaint: the comps round-trip EXACTLY -- names, ids, all four properties and "
              "the captured layer names");
        check(back.document.layers.size() == 2 && back.document.layers[0].id == doc.layers[0].id &&
                  back.document.layers[1].id == doc.layers[1].id,
              "npaint: and so do the layer ids, joined through the EXR part name rather than "
              "through position");
        check(back.document.nextLayerId == doc.nextLayerId,
              "npaint: the id counter survives too, so a layer added after reopening cannot "
              "re-issue a dead layer's id to a comp that still names it");
        // The whole point: restoring after a reload gives the same picture.
        Document reloaded = back.document;
        LayerCompRestoreReport report;
        check(restoreLayerComp(reloaded, 0, &report).ok && report.fullyApplied(),
              "npaint: a comp restored after a save/load applies in FULL -- the identity "
              "survived the file, which is the claim the whole carrier exists for");
        Document expected = doc;
        restoreLayerComp(expected, 0, nullptr);
        check(sameImage(flattenDocumentToLinear(reloaded), flattenDocumentToLinear(expected)),
              "npaint: and it composites BYTE-IDENTICALLY to the same restore in memory");
      }

      // The regression boundary, measured against a file rather than assumed.
      Document cleared = doc;
      cleared.comps.clear();
      const NpaintSaveResult again = saveNpaint(cleared, kCleared);
      const std::vector<unsigned char> bareBytes = bytesWithoutCapDate(kBare);
      const std::vector<unsigned char> clearedBytes = bytesWithoutCapDate(kCleared);
      const std::vector<unsigned char> withBytes = bytesWithoutCapDate(kWith);
      check(again.ok && !bareBytes.empty() && bareBytes == clearedBytes,
            "npaint: clearing every comp gives back a file BYTE-IDENTICAL to the one written "
            "before any existed (OpenImageIO's capDate masked, which HEAD's own two runs "
            "differ in too) -- np:comps is written only when there are comps");
      check(withBytes.size() > bareBytes.size() && withBytes != bareBytes,
            "npaint: and the file WITH comps really is bigger and really does differ, so the "
            "comparator above is not passing because nothing was ever written");
      std::printf("  no comps: %zu bytes; two comps of a two-layer document: %zu bytes "
                  "(+%zu)\n",
                  bareBytes.size(), withBytes.size(), withBytes.size() - bareBytes.size());
      // The ids are set on `cleared`'s layers and still nothing changed, which
      // is the structural half of the claim: no layer part carries an id.
      check(cleared.layers[0].id != 0 && bareBytes == clearedBytes,
            "npaint: the layers still carry their ids in the byte-identical file, so no layer "
            "part was ever touched -- the id join lives inside np:comps alone");

      // A newer build's np:comps is carried verbatim rather than dropped.
      NpaintCarry carry;
      NpaintAttribute future;
      future.name = "np:comps";
      future.type = NpaintAttribute::Type::String;
      future.stringValue = "npcomps2:deadbeef";
      carry.documentAttributes.push_back(future);
      Document plain = Document::createBlank(64, 64, WorkingSpace{});
      writeRgb(plain, 0, 1, 1, {0.5f, 0.5f, 0.5f, 1.0f});
      const char* kFuture = "selftest_comps_future.npaint";
      std::remove(kFuture);
      const NpaintSaveResult futureSaved = saveNpaint(plain, kFuture, {}, &carry);
      const NpaintLoadResult futureBack = loadNpaint(kFuture);
      bool warnedByName = false;
      for (const std::string& w : futureBack.warnings)
        if (contains(w, "npcomps2:")) warnedByName = true;
      const NpaintAttribute* kept = nullptr;
      for (const NpaintAttribute& a : futureBack.carry.documentAttributes)
        if (a.name == "np:comps") kept = &a;
      check(futureSaved.ok && futureBack.ok && warnedByName && kept != nullptr &&
                kept->stringValue == "npcomps2:deadbeef" && futureBack.document.comps.empty(),
            "npaint: an np:comps this build cannot decode is warned about BY NAME, carried "
            "verbatim, and does not become an empty comp list (PRD I10)");
      // ...and it goes back out unchanged on the next save.
      const char* kFuture2 = "selftest_comps_future2.npaint";
      std::remove(kFuture2);
      saveNpaint(futureBack.document, kFuture2, {}, &futureBack.carry);
      const NpaintLoadResult futureAgain = loadNpaint(kFuture2);
      const NpaintAttribute* keptAgain = nullptr;
      for (const NpaintAttribute& a : futureAgain.carry.documentAttributes)
        if (a.name == "np:comps") keptAgain = &a;
      check(keptAgain != nullptr && keptAgain->stringValue == "npcomps2:deadbeef",
            "npaint: and it survives a second round trip, which is what PRD I10 actually asks "
            "for -- preserved, not merely not-crashed-on");
      std::remove(kFuture);
      std::remove(kFuture2);
    }
    for (const char* p : {kBare, kWith, kCleared}) std::remove(p);
  }

  // --- Part K: what capture and restore cost ------------------------------
  {
    Document doc = Document::createBlank(2048, 2048, WorkingSpace{});
    for (size_t i = 1; i < 20; ++i) addLayer(doc, i, makeRgbLayer("Layer " + std::to_string(i)));
    // 256 tiles on the bottom layer, io/TileResidency's realistic fixture, so
    // the cost below is measured against a document with real content rather
    // than against an empty one.
    for (int32_t ty = 0; ty < 16; ++ty)
      for (int32_t tx = 0; tx < 16; ++tx)
        writeRgb(doc, 0, tx * 128, ty * 128, {0.5f, 0.5f, 0.5f, 1.0f});

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) captureLayerComp(doc, "c");
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) restoreLayerComp(doc, 0, nullptr);
    const auto t2 = std::chrono::steady_clock::now();
    const double captureMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / 100.0;
    const double restoreMs =
        std::chrono::duration<double, std::milli>(t2 - t1).count() / 100.0;
    std::printf("  [measured] 20-layer / 256-tile document: capture %.4f ms, restore %.4f ms "
                "(%.4f%% and %.4f%% of PRD F3's 20 ms pen-to-photon budget, PRD.md:252)\n",
                captureMs, restoreMs, captureMs / 20.0 * 100.0, restoreMs / 20.0 * 100.0);
    check(captureMs < 1.0 && restoreMs < 1.0,
          "cost: both are well under a millisecond -- a comp holds four scalars per layer and "
          "no tiles, so neither touches a pixel");
    check(doc.comps.size() == 100 && doc.comps[0].layers.size() == 20,
          "cost: and the 100 comps really were captured, so the timing is of real work");
  }

  std::printf("[selftest] layer comps %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
