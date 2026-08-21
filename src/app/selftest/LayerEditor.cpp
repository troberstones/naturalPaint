#include "app/selftest/Support.hpp"

namespace np {

// UI detour step 3, problem 2: the five built features that had no entry
// point, and the one rule that keeps the layer editor honest.
//
// app/LayerEditor is the surface the `Layer` menu and the LAYERS panel buttons
// both go through, so this section is the test of what those controls *do* --
// which is the half of a UI a `--selftest` can check. The half it cannot is
// what they look like, and this section does not pretend otherwise.
//
// The rule, asserted rather than described: **every mutation moves
// `OpenDocument::revision`**. ui/DocumentTexture caches the composite by that
// number, so an edit that does not move it is an edit the screen never shows.
// The trap is demonstrated directly here, by writing a tile the way a
// non-recorded path would and showing the revision does not move.
bool runLayerEditorTest() {
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
  // PLAN.md §1.5. Nothing here reaches a file, an encoder or the GPU, so every
  // assertion below has the same correct answer in both configurations; said
  // in the output rather than assumed.
  std::printf("[selftest] layer editor: NP_USE_OIIO=%s -- every assertion in this section has the "
              "same correct answer in both configurations; nothing here reaches a file, an "
              "encoder or the GPU\n",
              kOiioBuild ? "ON" : "OFF");

  // A small document with real pixels in the bottom layer, so a composite
  // difference is a difference in a picture rather than in a flag.
  constexpr int32_t kW = 64, kH = 64;
  auto makeDoc = []() {
    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{});
    Tile& tile = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kH; ++y)
      for (int32_t x = 0; x < kW; ++x)
        tile.writePixel(PixelCoord{x, y}, {0.4f, 0.2f, 0.1f, 1.0f});
    od.document.layers[0].name = "Base";
    od.recordEdit("fill base", EditKind::Content);
    return od;
  };
  auto composite = [](const Document& doc) { return compositeDocumentPremultiplied(doc); };
  auto sameComposite = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };

  // --- Part A: every command is reachable ---------------------------------
  //
  // The failure this step exists to fix was not a broken command, it was a
  // command with no way to issue it. So the first thing asserted is coverage:
  // every enumerator is in the list the menu walks, and every one has a menu
  // label. The scan below casts integers rather than listing enumerators, so a
  // command added to the enum and forgotten in the list fails here -- a
  // scoped enumeration's underlying type is `int`, so every value in that
  // range is well defined to cast.
  {
    const std::vector<LayerCommand>& all = allLayerCommands();
    check(all.size() == 18, "the menu walks all 18 commands");
    bool everyValueListed = true;
    size_t named = 0;
    for (int v = 0; v < 64; ++v) {
      const LayerCommand c = static_cast<LayerCommand>(v);
      const std::string label = layerCommandLabel(c);
      if (label == "?") continue;  // not an enumerator this build defines
      ++named;
      bool inList = false;
      for (const LayerCommand listed : all)
        if (listed == c) inList = true;
      if (!inList) everyValueListed = false;
    }
    check(everyValueListed && named == all.size(),
          "every command with a label is in the menu's list");
    bool labelsUnique = true;
    for (size_t i = 0; i < all.size(); ++i)
      for (size_t j = i + 1; j < all.size(); ++j)
        if (std::string(layerCommandLabel(all[i])) == layerCommandLabel(all[j]))
          labelsUnique = false;
    check(labelsUnique, "every command's menu label is distinct");
  }

  // --- Part B: what each creation command does ----------------------------
  //
  // The three kinds, and the storage shape each one is defined by. A Pigment
  // layer with `rgbTiles` engaged, or an Adjustment layer with any tiles at
  // all, would be the same bug in three different disguises.
  {
    OpenDocument od = makeDoc();
    const uint64_t before = od.revision;
    const LayerEditResult rgb = applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
    check(rgb.ok && rgb.selected == 1 && od.document.layers.size() == 2,
          "New RGB Layer inserts above the selection and selects it");
    check(od.document.layers[1].kind == LayerKind::RGB &&
              od.document.layers[1].rgbTiles.has_value() &&
              !od.document.layers[1].pigmentTiles.has_value(),
          "the new RGB layer has rgb tiles and no pigment tiles");
    check(od.revision == before + 1, "it moved the revision exactly once");

    const LayerEditResult pig = applyLayerCommand(od, LayerCommand::NewPigmentLayer, rgb.selected);
    check(pig.ok && od.document.layers[2].kind == LayerKind::Pigment &&
              od.document.layers[2].pigmentTiles.has_value() &&
              !od.document.layers[2].rgbTiles.has_value(),
          "New Pigment Layer makes a Pigment layer with pigment tiles");
    const LayerEditResult adj =
        applyLayerCommand(od, LayerCommand::NewAdjustmentLayer, pig.selected);
    check(adj.ok && od.document.layers[3].kind == LayerKind::Adjustment &&
              !od.document.layers[3].rgbTiles.has_value() &&
              !od.document.layers[3].pigmentTiles.has_value() &&
              od.document.layers[3].ops.size() == 0,
          "New Adjustment Layer makes a layer with no tiles and no ops");
    // Three layers added, three names allocated, none of them colliding: the
    // panel would otherwise show two rows with the same name after two clicks.
    check(od.document.layers[1].name != od.document.layers[2].name &&
              od.document.layers[2].name != od.document.layers[3].name,
          "each new layer gets its own default name");
    // A fresh Adjustment layer is an exact no-op, which is the kind's whole
    // definition -- asserted in pixels, not in `ops.size()`.
    OpenDocument plain = makeDoc();
    check(sameComposite(composite(od.document), composite(od.document)) &&
              !composite(od.document).empty(),
          "the composite is computable for the built stack");
    const std::vector<float> withAdjustment = composite(od.document);
    Document withoutAdjustment = od.document;
    withoutAdjustment.layers.pop_back();
    check(sameComposite(withAdjustment, composite(withoutAdjustment)),
          "an Adjustment layer with an empty stack changes no pixel");
  }

  // --- Part C: the mask, the clip, and the flags --------------------------
  {
    OpenDocument od = makeDoc();
    applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
    check(!od.document.layers[1].mask.has_value(), "a new layer starts with no mask");
    const LayerEditResult add = applyLayerCommand(od, LayerCommand::AddMask, 1);
    check(add.ok && od.document.layers[1].mask.has_value() &&
              od.document.layers[1].mask->occupiedTileCount() == 0,
          "Add Layer Mask engages a reveal-all mask with no tiles allocated");
    const std::vector<float> masked = composite(od.document);
    Document noMask = od.document;
    noMask.layers[1].mask.reset();
    check(sameComposite(masked, composite(noMask)),
          "a reveal-all mask composites identically to no mask");
    const LayerEditResult twice = applyLayerCommand(od, LayerCommand::AddMask, 1);
    check(!twice.ok && contains(twice.error, "already has a mask"),
          "a second Add Layer Mask is refused by name, not silently replaced");
    check(!layerCommandAvailable(od.document, LayerCommand::AddMask, 1) &&
              layerCommandAvailable(od.document, LayerCommand::RemoveMask, 1),
          "the menu greys the one that cannot apply");
    const LayerEditResult rm = applyLayerCommand(od, LayerCommand::RemoveMask, 1);
    check(rm.ok && !od.document.layers[1].mask.has_value(), "Remove Layer Mask discards it");

    // The clip, and the refusal the bottom row exists for.
    const LayerEditResult clipBottom = applyLayerCommand(od, LayerCommand::ToggleClipped, 0);
    check(!clipBottom.ok && contains(clipBottom.error, "bottom layer") &&
              !layerCommandAvailable(od.document, LayerCommand::ToggleClipped, 0),
          "the bottom layer cannot be clipped, and the menu says so first");
    const LayerEditResult clip = applyLayerCommand(od, LayerCommand::ToggleClipped, 1);
    check(clip.ok && od.document.layers[1].clipped, "Clip to Layer Below sets the flag");
    const LayerEditResult unclip = applyLayerCommand(od, LayerCommand::ToggleClipped, 1);
    check(unclip.ok && !od.document.layers[1].clipped, "and toggles it back off");

    const bool wasVisible = od.document.layers[1].visible;
    applyLayerCommand(od, LayerCommand::ToggleVisible, 1);
    check(od.document.layers[1].visible != wasVisible, "Toggle Visibility flips visibility");
    applyLayerCommand(od, LayerCommand::ToggleLocked, 1);
    check(od.document.layers[1].locked, "Toggle Lock locks the layer");
    // The lock, through the editor rather than through core/LayerOps
    // directly: this is the path a user can actually reach.
    const LayerEditResult del = applyLayerCommand(od, LayerCommand::DeleteLayer, 1);
    check(!del.ok && contains(del.error, "locked") && od.document.layers.size() == 2,
          "a locked layer refuses Delete, with core/LayerOps' own sentence");
    const LayerEditResult vis = applyLayerCommand(od, LayerCommand::ToggleVisible, 1);
    check(vis.ok, "and still allows Toggle Visibility, which changes nothing about it");
    applyLayerCommand(od, LayerCommand::ToggleLocked, 1);
    check(!od.document.layers[1].locked, "the lock can always be taken off again");
  }

  // --- Part D: reorder, duplicate, delete, and where the selection lands ---
  {
    OpenDocument od = makeDoc();
    applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
    od.document.layers[1].name = "Top";
    const LayerEditResult up = applyLayerCommand(od, LayerCommand::MoveLayerUp, 0);
    check(up.ok && up.selected == 1 && od.document.layers[1].name == "Base",
          "Move Layer Up moves the bottom layer up and the selection follows it");
    const LayerEditResult atTop = applyLayerCommand(od, LayerCommand::MoveLayerUp, 1);
    check(!atTop.ok && !layerCommandAvailable(od.document, LayerCommand::MoveLayerUp, 1),
          "the top layer refuses Move Layer Up and the menu greys it");
    const LayerEditResult atBottom = applyLayerCommand(od, LayerCommand::MoveLayerDown, 0);
    check(!atBottom.ok && contains(atBottom.error, "bottom"),
          "the bottom layer refuses Move Layer Down by name rather than wrapping");

    const LayerEditResult dup = applyLayerCommand(od, LayerCommand::DuplicateLayer, 0);
    check(dup.ok && dup.selected == 1 && od.document.layers.size() == 3,
          "Duplicate Layer puts the copy directly above and selects the copy");
    check(contains(od.document.layers[1].name, "copy"), "the copy's name says so");
    const LayerEditResult del = applyLayerCommand(od, LayerCommand::DeleteLayer, 1);
    check(del.ok && del.selected == 0 && od.document.layers.size() == 2,
          "Delete Layer takes the row below as the new selection");
  }

  // --- Part E: an unavailable command always refuses ----------------------
  //
  // The one property that ties the greyed-out menu item to the model: a
  // command the menu will not offer is a command the editor will not perform.
  // The converse is deliberately NOT claimed -- Delete is offered on a locked
  // layer and refused, because the sentence explains and a greyed item does
  // not (app/LayerEditor.hpp).
  {
    size_t attempted = 0;
    bool everySilentlySucceeded = true;
    for (int variant = 0; variant < 4; ++variant) {
      for (const LayerCommand command : allLayerCommands()) {
        OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{});
        // Four documents: empty, one layer, two layers with the top clipped,
        // and a selection past the end.
        size_t selected = 0;
        if (variant == 0) od.document.layers.clear();
        if (variant >= 2) {
          applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
          selected = 1;
        }
        if (variant == 2) applyLayerCommand(od, LayerCommand::ToggleClipped, 1);
        if (variant == 3) selected = 99;
        if (layerCommandAvailable(od.document, command, selected)) continue;
        ++attempted;
        const LayerEditResult r = applyLayerCommand(od, command, selected);
        if (r.ok || r.error.empty()) everySilentlySucceeded = false;
      }
    }
    std::printf("  %zu unavailable command/document pairs, each attempted anyway\n", attempted);
    check(attempted > 0 && everySilentlySucceeded,
          "an unavailable command always refuses, with a sentence");
  }

  // --- Part F: the per-layer op stack -------------------------------------
  //
  // `Layer::ops` composited and persisted since Phase 5 steps 3 and 5, and
  // reachable from a control for the first time here. The five operations are
  // core/LayerOps'; what is checked is that each does its OpStack's job, keeps
  // this file's two guards, refuses an out-of-range op index with a sentence
  // instead of the `std::out_of_range` OpStack throws, and is recorded.
  {
    OpenDocument od = makeDoc();
    applyLayerCommand(od, LayerCommand::NewAdjustmentLayer, 0);
    const size_t adj = 1;
    const std::vector<float> before = composite(od.document);

    const uint64_t rev0 = od.revision;
    const uint64_t ver0 = od.document.layers[adj].ops.version();
    DocumentOpResult r =
        recordLayerEdit(od, addLayerOp(od.document, adj, makeNewOp(PointOpKind::Exposure)));
    check(r.ok && od.document.layers[adj].ops.size() == 1, "+ Add puts one op on the layer");
    check(!od.document.layers[adj].ops.at(0).enabled,
          "a new op arrives disabled, so adding one changes nothing on screen");
    check(sameComposite(before, composite(od.document)),
          "and the composite is byte-identical, which is what that means in pixels");
    check(od.revision == rev0 + 1 && od.document.layers[adj].ops.version() > ver0,
          "the edit moved the document's revision and the stack's version");
    check(!od.history.entries().empty() &&
              contains(od.history.entries().back().label, "Exposure"),
          "and it is in history under the op's own name");

    Op edited = od.document.layers[adj].ops.at(0);
    edited.exposure.stops = -1.5f;
    r = recordLayerEdit(od, setLayerOp(od.document, adj, 0, edited));
    check(r.ok && od.document.layers[adj].ops.at(0).exposure.stops == -1.5f,
          "the Stops slider writes the whole op back");
    check(sameComposite(before, composite(od.document)),
          "still no pixel change: the op is still disabled");

    r = recordLayerEdit(od, setLayerOpEnabled(od.document, adj, 0, true));
    check(r.ok && !sameComposite(before, composite(od.document)),
          "enabling it is what changes the picture");
    const std::vector<float> graded = composite(od.document);

    // A second op, then reorder and delete.
    recordLayerEdit(od, addLayerOp(od.document, adj, makeNewOp(PointOpKind::Saturation)));
    check(od.document.layers[adj].ops.size() == 2, "a second op appends above the first");
    r = recordLayerEdit(od, moveLayerOp(od.document, adj, 1, 0));
    check(r.ok && od.document.layers[adj].ops.at(0).pointKind == PointOpKind::Saturation,
          "Up/Down reorder the stack");
    r = recordLayerEdit(od, moveLayerOp(od.document, adj, 0, 0));
    check(r.ok, "a move to where it already is succeeds and changes nothing");
    r = recordLayerEdit(od, removeLayerOp(od.document, adj, 0));
    check(r.ok && od.document.layers[adj].ops.size() == 1 &&
              sameComposite(graded, composite(od.document)),
          "Delete removes the op it names and leaves the other one working");

    // The refusals. Each names the number it refused, which is what a stale
    // panel row needs to read.
    const LayerOpResult badOp = removeLayerOp(od.document, adj, 7);
    check(!badOp.ok && contains(badOp.error, "index 7") && contains(badOp.error, "1 op"),
          "an out-of-range op index is a sentence, never OpStack's exception");
    const LayerOpResult badLayer = addLayerOp(od.document, 99, makeNewOp(PointOpKind::Levels));
    check(!badLayer.ok && contains(badLayer.error, "no layer at index 99"),
          "an out-of-range layer index is refused the same way");
    recordLayerEdit(od, setLayerLocked(od.document, adj, true));
    const LayerOpResult locked = addLayerOp(od.document, adj, makeNewOp(PointOpKind::Levels));
    const LayerOpResult lockedToggle = setLayerOpEnabled(od.document, adj, 0, false);
    check(!locked.ok && contains(locked.error, "locked") && !lockedToggle.ok,
          "a locked layer refuses op edits: its op stack is part of how it looks");
    recordLayerEdit(od, setLayerLocked(od.document, adj, false));

    // Undo, which is the other half of "it went through recordLayerEdit". One
    // more op is added and then taken back, in the only way a user can: the
    // history entry the edit appended.
    const size_t opsBeforeAdd = od.document.layers[adj].ops.size();
    recordLayerEdit(od, addLayerOp(od.document, adj, makeNewOp(PointOpKind::Grayscale)));
    check(od.document.layers[adj].ops.size() == opsBeforeAdd + 1, "one more op on the layer");
    const Document* undone = od.history.undo();
    check(undone != nullptr, "undo moves the cursor back one edit");
    if (undone != nullptr) od.document = *undone;
    check(od.document.layers[adj].ops.size() == opsBeforeAdd,
          "and the op is gone -- a layer op edit is undoable because it was recorded");
  }

  // --- Part G: the trap ui/DocumentTexture documents ----------------------
  //
  // A tile written without a recorded edit does not move `revision`, so the
  // composite cache does not notice and the screen keeps the previous picture.
  // Demonstrated rather than described, and then the same write is shown to be
  // visible the moment it goes through the funnel every control here uses.
  {
    OpenDocument od = makeDoc();
    const uint64_t before = od.revision;
    Tile& tile = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    tile.writePixel(PixelCoord{1, 1}, {1.0f, 1.0f, 1.0f, 1.0f});
    check(od.revision == before, "a raw tile write does NOT move the revision -- the trap");
    od.recordEdit("paint", EditKind::Content);
    check(od.revision == before + 1, "recording it is what makes the composite cache re-read");

    // And the property this whole file rests on, over every command: each one
    // that succeeds moves the revision by exactly one.
    OpenDocument doc2 = makeDoc();
    applyLayerCommand(doc2, LayerCommand::NewRgbLayer, 0);
    size_t selected = 1;
    size_t succeeded = 0;
    bool alwaysMoved = true;
    for (const LayerCommand command : allLayerCommands()) {
      const uint64_t rev = doc2.revision;
      const size_t entries = doc2.history.entries().size();
      const LayerEditResult r = applyLayerCommand(doc2, command, selected);
      if (!r.ok) {
        if (doc2.revision != rev) alwaysMoved = false;  // a refusal must change nothing
        continue;
      }
      ++succeeded;
      selected = r.selected;
      if (doc2.revision != rev + 1) alwaysMoved = false;
      if (doc2.history.entries().size() != entries + 1) alwaysMoved = false;
    }
    std::printf("  %zu of %zu commands succeeded on one document; each moved the revision "
                "exactly once\n",
                succeeded, allLayerCommands().size());
    check(succeeded >= 8 && alwaysMoved,
          "every successful command records exactly one edit; a refusal records none");
  }

  std::printf("[selftest] layer editor %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
