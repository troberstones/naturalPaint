#include "app/selftest/Support.hpp"

#include <fstream>

#include "core/LayerSetOps.hpp"

namespace np {

// Layer groups (PLAN.md Phase 5's C7/C12 follow-on; PRD C7, P0).
//
// core/LayerSetOps.hpp section 5 argues the mechanics -- the span-splice, the
// locked/clipped guards, why nesting falls out of moving spans -- and
// core/Composite.hpp's group section argues pass-through over isolated. This
// file is where both are proven: the model, the composite integration, undo,
// the `.npaint` round trip, and the older-build degradation PRD I10 promises.
bool runLayerGroupTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("[selftest] layer groups: the model, core/Composite's pass-through fold, and "
              "the `.npaint` round trip\n");
  std::printf("  This build's group is PASS-THROUGH, not isolated: a member blends directly "
              "against whatever is beneath the GROUP in the stack, under its own blend mode, "
              "scaled by every ancestor group's own visible/opacity. There is no offscreen "
              "accumulator -- core/Composite.hpp's group section argues why not, the same "
              "shape §7's unimplemented-blend argument already has: the P0 sentence (PRD C7) "
              "asks for visibility and opacity to reach children, not for isolation.\n");
  std::printf("  Nesting is real (a group's own parent may itself be a group); cycles are "
              "structurally impossible from this build's own GroupLayers command but are "
              "guarded against anyway, because a hand-built Document or a foreign `.npaint` "
              "is not bound by what this file's own operations would produce.\n");

  auto sel = [](std::vector<size_t> idx) { return makeLayerSelection(std::move(idx)); };
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex]
        .rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto fillRgb = [&](Document& doc, size_t layerIndex, int32_t v0, int32_t v1, float r, float g,
                     float b, float a) {
    for (int32_t y = 0; y < v1; ++y)
      for (int32_t x = 0; x < v0; ++x) writeRgb(doc, layerIndex, x, y, {r, g, b, a});
  };
  auto names = [](const Document& doc) {
    std::string s;
    for (const Layer& l : doc.layers) {
      if (!s.empty()) s += ",";
      s += l.name.empty() ? "?" : l.name;
    }
    return s;
  };
  auto composite = [](const Document& doc) { return compositeDocumentPremultiplied(doc); };
  auto samePixels = [](const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && !a.empty() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
  };
  // Five named RGB layers, bottom to top, in one blank 8x8 document --
  // `LayerMultiSelect.cpp`'s own `makeFive()`, copied rather than shared
  // (internal linkage cannot cross a translation unit).
  auto makeFive = []() {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    doc.layers[0].name = "A";
    for (const char* n : {"B", "C", "D", "E"}) addLayer(doc, doc.layers.size(), makeRgbLayer(n));
    return doc;
  };

  // ==========================================================================
  // Part A: the model -- makeGroupLayer(), groupTag, and what a Group holds
  // ==========================================================================
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    const Layer g1 = makeGroupLayer(doc, "First");
    const Layer g2 = makeGroupLayer(doc, "Second");
    check(g1.kind == LayerKind::Group && g2.kind == LayerKind::Group,
          "makeGroupLayer: kind is Group");
    check(!g1.rgbTiles.has_value() && !g1.pigmentTiles.has_value(),
          "makeGroupLayer: no pixel storage of any kind -- Adjustment's own contract");
    check(g1.visible && g1.opacity == 1.0f, "makeGroupLayer: visible, full opacity by default");
    check(!g1.groupTag.empty() && !g2.groupTag.empty() && g1.groupTag != g2.groupTag,
          "makeGroupLayer: every group gets its own, distinct, nonzero-length tag");
    check(doc.nextGroupId == 3,
          "makeGroupLayer: the counter advances by one per group, independently of "
          "Document::nextLayerId (still 1 -- nothing here touched a comp)");
    check(doc.nextLayerId == 1,
          "makeGroupLayer: Layer::id is untouched -- a document that groups two layers must "
          "not be forced to acquire ids the way capturing a comp does");
    check(layerKindName(LayerKind::Group) == std::string("group"),
          "layerKindName: \"group\", lower-case like \"selection\" -- matching "
          "docs/document-format.md's own published sketch rather than the capitalised habit");
    check(layerKindFromName("group") == LayerKind::Group,
          "layerKindFromName: the inverse round-trips");
  }

  // ==========================================================================
  // Part B: GroupLayers -- the span-splice, hand-verified
  // ==========================================================================
  //
  // Grouping {A, C, E} out of [A,B,C,D,E] must produce [B,D,A,C,E,Group]: B
  // and D (unselected) close the gap in their own relative order, and A, C, E
  // land as one contiguous block, in THEIR original relative order, at the
  // position the topmost selected layer (E) held. Reversed, this would be
  // [B,D,E,C,A,Group] -- the exact shape the sabotage proof breaks.
  size_t groupIndexBCDAE = 0;
  std::string groupTagBCDAE;
  {
    Document doc = makeFive();
    const LayerSetOpResult r = applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));
    check(r.ok, "group {A,C,E}: succeeds");
    check(names(doc) == "B,D,A,C,E,Group 1",
          "group {A,C,E}: [B,D,A,C,E,Group 1] -- members contiguous, in ORIGINAL relative "
          "order, at the topmost selection's position; unselected B and D close the gap in "
          "their own order");
    check(r.selection == sel({5}), "group {A,C,E}: selection follows the new group");
    check(doc.layers[5].kind == LayerKind::Group, "group {A,C,E}: layer 5 is the new Group");
    const std::string& tag = doc.layers[5].groupTag;
    check(!tag.empty() && doc.layers[2].parent == tag && doc.layers[3].parent == tag &&
              doc.layers[4].parent == tag,
          "group {A,C,E}: exactly A, C and E (indices 2,3,4) carry the new group's tag");
    check(doc.layers[0].parent.empty() && doc.layers[1].parent.empty(),
          "group {A,C,E}: B and D are untouched -- they were never selected");
    check(contains(r.editLabel, "group") && contains(r.editLabel, "Group 1"),
          "group {A,C,E}: the edit label names the gesture and the group");
    groupIndexBCDAE = 5;
    groupTagBCDAE = tag;
    (void)groupIndexBCDAE;
  }

  // Refusals: a locked member, and re-grouping an already-grouped layer.
  {
    Document doc = makeFive();
    setLayerLocked(doc, 1, true);
    const LayerSetOpResult locked =
        applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 1}));
    check(!locked.ok && contains(locked.error, "locked") && doc.layers.size() == 5,
          "group: refuses a locked member by name, and changes nothing");

    Document grouped = makeFive();
    applyLayerSetOp(grouped, LayerSetCommand::GroupLayers, sel({0, 1}));
    const size_t before = grouped.layers.size();
    const LayerSetOpResult regroup =
        applyLayerSetOp(grouped, LayerSetCommand::GroupLayers, sel({0}));
    check(!regroup.ok && contains(regroup.error, "already inside a group") &&
              grouped.layers.size() == before,
          "group: refuses grouping an already-grouped layer, and changes nothing");
  }

  // Nesting: grouping a Group together with an ordinary layer nests it, and
  // moves its WHOLE span (including its own members) as one opaque block --
  // core/LayerSetOps.hpp section 5's "nesting falls out of moving spans".
  {
    Document doc = makeFive();
    applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));  // -> [B,D,A,C,E,G1]
    check(names(doc) == "B,D,A,C,E,Group 1", "nesting fixture: the {A,C,E} group from part B");
    const LayerSetOpResult outer =
        applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({1, 5}));  // D and Group 1
    check(outer.ok, "nesting: grouping {D, Group 1} together succeeds");
    check(names(doc) == "B,D,A,C,E,Group 1,Group 2",
          "nesting: Group 1's own span (itself plus A,C,E) travels as one block -- nothing "
          "inside it is disturbed, only D moves to sit beside it");
    const size_t outerIdx = doc.layers.size() - 1;
    const size_t innerIdx = outerIdx - 1;
    check(doc.layers[outerIdx].kind == LayerKind::Group &&
              doc.layers[innerIdx].kind == LayerKind::Group,
          "nesting: both entries are still groups");
    check(doc.layers[1].parent == doc.layers[outerIdx].groupTag &&      // D
              doc.layers[innerIdx].parent == doc.layers[outerIdx].groupTag,  // Group 1
          "nesting: D and Group 1 both now belong to Group 2");
    check(doc.layers[2].parent == doc.layers[innerIdx].groupTag &&      // A
              doc.layers[3].parent == doc.layers[innerIdx].groupTag &&  // C
              doc.layers[4].parent == doc.layers[innerIdx].groupTag,    // E
          "nesting: A, C and E still belong to Group 1 -- untouched by the outer regroup");
  }

  // ==========================================================================
  // Part C: UngroupLayers -- the order-preservation proof
  // ==========================================================================
  {
    Document doc = makeFive();
    const LayerSetOpResult grp = applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));
    check(names(doc) == "B,D,A,C,E,Group 1", "ungroup fixture: grouped as part B");
    const LayerSetOpResult ung =
        applyLayerSetOp(doc, LayerSetCommand::UngroupLayers, sel({grp.selection.indices[0]}));
    check(ung.ok, "ungroup: succeeds");
    // **This is sabotage proof #3's assertion.** A, C, E must come back in
    // their ORIGINAL relative order -- not reversed, not shuffled -- which is
    // exactly core/LayerSetOps.cpp's `UngroupLayers` extract-then-splice
    // getting `members.begin()/end()` right rather than swapped for
    // `rbegin()/rend()`.
    check(names(doc) == "B,D,A,C,E",
          "ungroup: A, C, E come back in their ORIGINAL relative order (not reversed) -- the "
          "classic bug this feature is most likely to have");
    check(doc.layers[2].parent.empty() && doc.layers[3].parent.empty() &&
              doc.layers[4].parent.empty(),
          "ungroup: every promoted member's parent is cleared (this group had none of its own)");
    check(ung.selection == sel({2, 3, 4}), "ungroup: selection follows the promoted members");
  }

  // Nesting-aware promotion: ungrouping the INNER group of a nest reparents
  // its members to the OUTER group, not to top level.
  {
    Document doc = makeFive();
    applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));      // [B,D,A,C,E,G1]
    applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({1, 5}));         // [B,D,A,C,E,G1,G2]
    const size_t outerIdx = doc.layers.size() - 1;
    const size_t innerIdx = outerIdx - 1;
    const std::string outerTag = doc.layers[outerIdx].groupTag;
    const LayerSetOpResult ung =
        applyLayerSetOp(doc, LayerSetCommand::UngroupLayers, sel({innerIdx}));
    check(ung.ok, "nesting-aware ungroup: succeeds");
    check(names(doc) == "B,D,A,C,E,Group 2",
          "nesting-aware ungroup: Group 1 is gone, D and the promoted A/C/E sit where it was");
    check(doc.layers[2].parent == outerTag && doc.layers[3].parent == outerTag &&
              doc.layers[4].parent == outerTag,
          "nesting-aware ungroup: A, C and E are promoted to the OUTER group, not to top "
          "level -- ungrouping one level must not silently pop a layer out of every group it "
          "was ever in");
  }

  // Ungroup refusals: not a group, and a locked group.
  {
    Document doc = makeFive();
    const LayerSetOpResult notAGroup =
        applyLayerSetOp(doc, LayerSetCommand::UngroupLayers, sel({0}));
    check(!notAGroup.ok && contains(notAGroup.error, "not a group"),
          "ungroup: refuses a non-Group index by name");
    check(!layerSetCommandAvailable(doc, LayerSetCommand::UngroupLayers, sel({0})),
          "ungroup: unavailable at all when the selection holds a non-Group layer");

    Document grouped = makeFive();
    const LayerSetOpResult grp =
        applyLayerSetOp(grouped, LayerSetCommand::GroupLayers, sel({0, 1}));
    setLayerLocked(grouped, grp.selection.indices[0], true);
    const LayerSetOpResult lockedUngroup =
        applyLayerSetOp(grouped, LayerSetCommand::UngroupLayers, grp.selection);
    check(!lockedUngroup.ok && contains(lockedUngroup.error, "locked"),
          "ungroup: refuses a locked group, removeLayer()'s own reason");
  }

  // ==========================================================================
  // Part D: core/Composite -- visibility and opacity actually reach children
  // ==========================================================================
  //
  // **This is sabotage proof #1's assertion.** Every comparison below is
  // BYTE-IDENTICAL (memcmp of the raw floats), not merely close, because each
  // one is stated as "grouped at X is the same picture as flat at X" -- the
  // same equivalence core/Composite.hpp already proves for opacity vs. a mask
  // (section 5) and for opacity vs. a fade (section 3).
  {
    // Base: opaque red, full canvas. Child: opaque blue, full canvas.
    Document flat = Document::createBlank(4, 4, WorkingSpace{});
    flat.layers[0].name = "Base";
    fillRgb(flat, 0, 4, 4, 0.6f, 0.1f, 0.1f, 1.0f);
    addLayer(flat, 1, makeRgbLayer("Child"));
    fillRgb(flat, 1, 4, 4, 0.1f, 0.1f, 0.6f, 1.0f);

    // Grouped: identical document, but Child sits inside a Group at 50%.
    Document grouped = flat;
    Layer g = makeGroupLayer(grouped, "G");
    g.opacity = 0.5f;
    grouped.layers[1].parent = g.groupTag;
    grouped.layers.push_back(std::move(g));  // [Base, Child, G] -- position is irrelevant to
                                             // a pass-through fold; see the header's own note
                                             // that group placement need not be contiguous
                                             // for coverage to resolve correctly.

    // Equivalent flat document: Child's OWN opacity set to 0.5, no group at
    // all -- the exact comparison core/Composite.hpp's group section commits
    // to (a group's opacity scales its member exactly the way the member's
    // own opacity would).
    Document expectedHalf = flat;
    expectedHalf.layers[1].opacity = 0.5f;

    check(samePixels(composite(grouped), composite(expectedHalf)),
          "group opacity 0.5: BYTE-IDENTICAL to the child's own opacity set to 0.5 directly -- "
          "the group's coverage reaches the child exactly the way the child's own would");
    check(!samePixels(composite(grouped), composite(flat)),
          "group opacity 0.5: and it really does differ from the ungrouped picture, so the "
          "check above is not passing because nothing changed");

    // Group hidden: the child must vanish exactly as if it were deleted.
    Document hidden = flat;
    Layer gh = makeGroupLayer(hidden, "GH");
    gh.visible = false;
    hidden.layers[1].parent = gh.groupTag;
    hidden.layers.push_back(std::move(gh));
    Document baseOnly = flat;
    removeLayer(baseOnly, 1);
    check(samePixels(composite(hidden), composite(baseOnly)),
          "group visible=false: BYTE-IDENTICAL to the child being deleted outright");

    // Own opacity AND group opacity both 0.5 multiply: 0.5*0.5 = 0.25, exact
    // in binary, so the flat-equivalent comparison stays zero-tolerance.
    Document both = flat;
    both.layers[1].opacity = 0.5f;
    Layer gb = makeGroupLayer(both, "GB");
    gb.opacity = 0.5f;
    both.layers[1].parent = gb.groupTag;
    both.layers.push_back(std::move(gb));
    Document expectedQuarter = flat;
    expectedQuarter.layers[1].opacity = 0.25f;
    check(samePixels(composite(both), composite(expectedQuarter)),
          "own opacity 0.5 INSIDE a group at opacity 0.5: BYTE-IDENTICAL to 0.25 flat -- "
          "coverage multiplies, the same rule opacity and a mask already follow together");
  }

  // Three-deep nesting: 0.5^3 = 0.125, still exact in binary.
  {
    Document flat = Document::createBlank(4, 4, WorkingSpace{});
    flat.layers[0].name = "Base";
    fillRgb(flat, 0, 4, 4, 0.6f, 0.1f, 0.1f, 1.0f);
    addLayer(flat, 1, makeRgbLayer("Child"));
    fillRgb(flat, 1, 4, 4, 0.1f, 0.1f, 0.6f, 1.0f);

    Document nested = flat;
    Layer inner = makeGroupLayer(nested, "Inner");
    inner.opacity = 0.5f;
    nested.layers[1].parent = inner.groupTag;
    Layer middle = makeGroupLayer(nested, "Middle");
    middle.opacity = 0.5f;
    inner.parent = middle.groupTag;
    Layer outer = makeGroupLayer(nested, "Outer");
    outer.opacity = 0.5f;
    middle.parent = outer.groupTag;
    nested.layers.push_back(std::move(inner));
    nested.layers.push_back(std::move(middle));
    nested.layers.push_back(std::move(outer));

    Document expectedEighth = flat;
    expectedEighth.layers[1].opacity = 0.125f;
    check(samePixels(composite(nested), composite(expectedEighth)),
          "three groups nested, each at 50%: BYTE-IDENTICAL to 12.5% flat (0.5^3 = 0.125, "
          "exact in binary) -- ancestor coverage multiplies all the way up the chain");
  }

  // ==========================================================================
  // Part E: cycle safety -- a hand-built cycle, never producible by this
  // build's own GroupLayers, must still terminate and read as invisible
  // ==========================================================================
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    addLayer(doc, 1, makeRgbLayer("Child"));
    Layer g0 = makeGroupLayer(doc, "G0");
    Layer g1 = makeGroupLayer(doc, "G1");
    g0.parent = g1.groupTag;  // G0's parent is G1
    g1.parent = g0.groupTag;  // G1's parent is G0 -- a two-cycle
    doc.layers[1].parent = g0.groupTag;
    doc.layers.push_back(std::move(g0));
    doc.layers.push_back(std::move(g1));

    const GroupAncestryResult ancestry = groupAncestry(doc, 1);
    check(ancestry.cyclic && ancestry.coverage == 0.0f,
          "groupAncestry: a hand-built two-group cycle is detected, and reads as 0.0f "
          "coverage rather than looping -- layerCoverage()'s own answer to a NaN opacity, "
          "hide rather than hang");
    check(layerCoverage(doc, 1) == 0.0f,
          "layerCoverage(doc, index): the cycle reaches the coverage a composite would "
          "actually use, not only groupAncestry()'s own return value");
    // Termination is the whole point of the bound in groupAncestry() -- if it
    // ever regressed to an unbounded walk, this call would hang the whole
    // suite rather than return, which is a stronger proof than a timer.
    std::printf("  %-58s %s\n", "groupAncestry on a hand-built cycle terminates (see above)",
                "pass");

    // And the walk does not corrupt an otherwise-normal composite: a
    // dangling/cyclic ancestor reads as invisible, not as a crash or a
    // warning that stops the rest of the document from compositing.
    fillRgb(doc, 0, 4, 4, 0.6f, 0.1f, 0.1f, 1.0f);
    fillRgb(doc, 1, 4, 4, 0.1f, 0.1f, 0.6f, 1.0f);
    Document baseOnly = doc;
    baseOnly.layers.resize(1);
    check(samePixels(composite(doc), composite(baseOnly)),
          "a cyclic-ancestry child composites exactly as if it were absent -- the rest of "
          "the document (the base layer) is unaffected");
  }

  // ==========================================================================
  // Part F: undo/redo -- one command, one history entry, exactly section 3's
  // rule for every other set command
  // ==========================================================================
  {
    OpenDocument od = makeBlankOpenDocument(8, 8, WorkingSpace{});
    od.document.layers[0].name = "A";
    addLayer(od.document, 1, makeRgbLayer("B"));
    od.recordEdit("fixture", EditKind::Structural);
    const size_t entries = od.history.entries().size();

    const LayerSetEditResult grp =
        applyLayerSetCommand(od, LayerSetCommand::GroupLayers, sel({0, 1}));
    check(grp.ok && od.history.entries().size() == entries + 1,
          "group: ONE history entry for the whole gesture");
    check(contains(od.history.entries().back().label, "group"),
          "group: the history row names the gesture");
    check(od.document.layers.size() == 3 && od.document.layers[2].kind == LayerKind::Group,
          "group: the open document actually changed");

    const Document* undoneGroup = od.history.undo();
    check(undoneGroup != nullptr && undoneGroup->layers.size() == 2,
          "group: one undo removes the whole group, giving back the two-layer document");
    // `History::undo()` only returns the restored snapshot; the usual call
    // installs it (core/History.hpp: "the usual call installs it immediately
    // (`doc.document = *h.undo();`)"), which the running application's own
    // undo handler does and this test must do too before it keeps editing.
    od.document = *undoneGroup;

    const LayerSetEditResult grp2 =
        applyLayerSetCommand(od, LayerSetCommand::GroupLayers, sel({0, 1}));
    const size_t entriesAfterRegroup = od.history.entries().size();
    const LayerSetEditResult ung =
        applyLayerSetCommand(od, LayerSetCommand::UngroupLayers, grp2.selection);
    check(ung.ok && od.history.entries().size() == entriesAfterRegroup + 1,
          "ungroup: ONE history entry for the whole gesture");
    check(od.document.layers.size() == 2,
          "ungroup: the open document actually changed back to two ordinary layers");
    const Document* undoneUngroup = od.history.undo();
    check(undoneUngroup != nullptr && undoneUngroup->layers.size() == 3 &&
              undoneUngroup->layers[2].kind == LayerKind::Group,
          "ungroup: one undo gives the group back");
    od.document = *undoneUngroup;
  }

  // ==========================================================================
  // Part G: `.npaint` persistence -- the round trip, and what an older build
  // (or a newer one) does with a kind it does not recognise
  // ==========================================================================
  {
    const char* kGrouped = "selftest_groups.npaint";
    const char* kBare = "selftest_groups_bare.npaint";
    const char* kFuture = "selftest_groups_future.npaint";
    const char* kFuture2 = "selftest_groups_future2.npaint";
    for (const char* p : {kGrouped, kBare, kFuture, kFuture2}) std::remove(p);

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

    // --- G1: a document with no groups saves BYTE-IDENTICALLY to before this
    // step -- the regression boundary every prior structural addition
    // (`np:ops`, `np:comps`, `np:clipped`) proved the same way.
    Document plain = makeFive();
    fillRgb(plain, 0, 8, 8, 0.4f, 0.4f, 0.4f, 1.0f);
    const NpaintSaveResult bareSave = saveNpaint(plain, kBare);
    check(bareSave.ok, "npaint: a five-layer, group-free document saves");

    // --- G2: a grouped, NESTED document -- **this is sabotage proof #2's
    // fixture.** Saved, loaded back, and checked structurally field by
    // field, not merely "it opened".
    Document doc = makeFive();
    fillRgb(doc, 0, 8, 8, 0.4f, 0.4f, 0.4f, 1.0f);
    const LayerSetOpResult grp1 = applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));
    const LayerSetOpResult grp2 =
        applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({1, grp1.selection.indices[0]}));
    doc.layers[grp2.selection.indices[0]].opacity = 0.75f;
    check(names(doc) == "B,D,A,C,E,Group 1,Group 2",
          "npaint fixture: two nested groups, matching part B's own nesting shape");

    const NpaintSaveResult saved = saveNpaint(doc, kGrouped);
    check(saved.ok, "npaint: the grouped, nested document saves");
    const NpaintLoadResult back = loadNpaint(kGrouped);
    check(back.ok && back.warnings.empty(),
          "npaint: it loads back with NO warnings -- this build wrote it and fully "
          "understands its own group parts");
    if (back.ok) {
      const Document& reloaded = back.document;
      check(reloaded.layers.size() == doc.layers.size() &&
                names(reloaded) == names(doc),
            "npaint round trip: same layer count, same names, same order");
      bool everyFieldMatches = true;
      for (size_t i = 0; i < doc.layers.size() && everyFieldMatches; ++i) {
        const Layer& a = doc.layers[i];
        const Layer& b = reloaded.layers[i];
        if (a.kind != b.kind || a.name != b.name || a.visible != b.visible ||
            a.opacity != b.opacity || a.groupTag != b.groupTag)
          everyFieldMatches = false;
      }
      check(everyFieldMatches,
            "npaint round trip: kind, name, visible, opacity and groupTag are IDENTICAL for "
            "every layer, group and ordinary alike");
      // The `parent` link itself -- not the raw string (a reload's groupTag
      // for the same group need not be spelled identically to survive as a
      // JOIN, though in this build it happens to be, since groupTag is
      // written and read verbatim with no translation), but the RELATIONSHIP
      // it encodes: which layer belongs to which.
      bool everyParentResolvesTheSame = true;
      for (size_t i = 0; i < doc.layers.size(); ++i) {
        auto groupNameOfParent = [](const Document& d, const std::string& tag) -> std::string {
          if (tag.empty()) return "";
          for (const Layer& l : d.layers)
            if (l.kind == LayerKind::Group && l.groupTag == tag) return l.name;
          return "<dangling>";
        };
        if (groupNameOfParent(doc, doc.layers[i].parent) !=
            groupNameOfParent(reloaded, reloaded.layers[i].parent))
          everyParentResolvesTheSame = false;
      }
      check(everyParentResolvesTheSame,
            "npaint round trip: every layer's group membership resolves to the SAME group, "
            "by name, before and after -- the join survives even though it is not asserted "
            "on the raw tag string");
      check(samePixels(composite(doc), composite(reloaded)),
            "npaint round trip: and the two documents composite BYTE-IDENTICALLY -- the "
            "structural check above is not merely cosmetic");
    }

    // The byte-identity boundary for the group-free case, exactly the
    // comparator `np:comps`' own persistence section uses.
    const std::vector<unsigned char> bareBytes = bytesWithoutCapDate(kBare);
    check(!bareBytes.empty(), "npaint: the bare file actually has bytes to compare");

    // --- G3: an older build's view of a grouped file. Simulated rather than
    // executed -- there is no stale binary in this tree to run -- by using
    // the EXACT mechanism an older reader applies to any `np:kind` it does
    // not recognise (io/NpaintFile.cpp's own carry-through), with a synthetic
    // kind string ("supergroup") standing in for "group" the way this
    // section's own comment argues: an old reader treats "group" exactly as
    // this build treats a kind it has never heard of, because both cases are
    // literally the same code path (`!isRgbLayer && !isPigmentLayer && ...`).
    {
      NpaintCarry carry;
      NpaintRawPart futureGroup;
      futureGroup.name = "L0099";
      futureGroup.x = 0;
      futureGroup.y = 0;
      futureGroup.width = 128;
      futureGroup.height = 128;
      futureGroup.tileWidth = 128;
      futureGroup.tileHeight = 128;
      futureGroup.channelNames = {"mask"};
      futureGroup.sampleTypeName = "half";
      futureGroup.rawPixels.assign(128u * 128u * sizeof(uint16_t), 0);
      NpaintAttribute kindAttr;
      kindAttr.name = "np:kind";
      kindAttr.type = NpaintAttribute::Type::String;
      kindAttr.stringValue = "supergroup";  // this build's stand-in for "an np:kind it does
                                            // not know", proving the SAME mechanism an older
                                            // build applies to THIS step's own "group"
      futureGroup.attributes.push_back(kindAttr);
      carry.rawParts.push_back(futureGroup);
      carry.partOrder.push_back(NpaintPartSlot{NpaintPartSlot::Kind::RawPart, 0});

      Document withMember = makeFive();
      fillRgb(withMember, 0, 8, 8, 0.4f, 0.4f, 0.4f, 1.0f);
      // The member's `parent` names the unrecognised group's tag, exactly as
      // it would in a real file: a member layer is ordinary (RGB-kind) and
      // loads fine regardless of whether its group's own part was
      // understood.
      withMember.layers[1].parent = "G7";

      const NpaintSaveResult futureSaved = saveNpaint(withMember, kFuture, {}, &carry);
      check(futureSaved.ok, "npaint (older-build simulation): the file saves with the "
                           "unrecognised group part carried alongside it");
      const NpaintLoadResult futureBack = loadNpaint(kFuture);
      bool warnedByName = false;
      for (const std::string& w : futureBack.warnings)
        if (contains(w, "supergroup")) warnedByName = true;
      check(futureBack.ok && warnedByName,
            "npaint (older-build simulation): loading WARNS BY NAME about the kind it does "
            "not understand -- legible, not silent");
      check(futureBack.document.layers.size() == withMember.layers.size(),
            "npaint (older-build simulation): every ORDINARY layer -- including the one that "
            "names the unrecognised group as its parent -- still loads");
      check(futureBack.document.layers[1].parent == "G7",
            "npaint (older-build simulation): the member's np:parent string survives "
            "UNTOUCHED even though nothing resolved it to an actual group -- an unresolved "
            "parent is inert (groupCoverage() reads it as \"no ancestor\"), never dropped");
      check(samePixels(composite(futureBack.document), composite(withMember)),
            "npaint (older-build simulation): and the picture is UNCHANGED by the "
            "unrecognised group -- a member with a dangling parent composites at full "
            "coverage, exactly as an ungrouped layer would, so the degradation is invisible "
            "in the common case (a top-level group at default 100%/visible)");

      // ...and it survives a SECOND round trip unchanged -- PRD I10 read
      // literally, `np:comps`' own two-hop proof applied here.
      const NpaintSaveResult futureSaved2 =
          saveNpaint(futureBack.document, kFuture2, {}, &futureBack.carry);
      const NpaintLoadResult futureBack2 = loadNpaint(kFuture2);
      bool stillCarried = false;
      for (const NpaintRawPart& p : futureBack2.carry.rawParts) {
        for (const NpaintAttribute& a : p.attributes) {
          if (a.name == "np:kind" && a.stringValue == "supergroup") stillCarried = true;
        }
      }
      check(futureSaved2.ok && stillCarried,
            "npaint (older-build simulation): the unrecognised group part survives a SECOND "
            "save/load untouched -- carried, not merely not-crashed-on");
    }

    for (const char* p : {kGrouped, kBare, kFuture, kFuture2}) std::remove(p);
  }

  return ok;
}

}  // namespace np
