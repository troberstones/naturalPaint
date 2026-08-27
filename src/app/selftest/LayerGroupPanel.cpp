#include "app/selftest/Support.hpp"

#include <set>

namespace np {

// The LAYERS panel's own half of PRD C7 (task/group-ui): docs/reachability-
// audit.md's C7 was left open specifically because `LayerSetCommand::GroupLayers`
// and `UngroupLayers` existed, were modelled and tested (app/selftest/LayerGroup.cpp)
// and had NOTHING in the UI to issue either. That command-issuing half turned
// out to already exist: `ui/MacPaintUI.cpp`'s "Multi-selection" section and the
// `Layer` > Selection menu both walk `core::allLayerSetCommands()` generically,
// so the moment `GroupLayers`/`UngroupLayers` joined that list, both surfaces
// picked them up with no code change -- `--ui-multiselect-demo select:0.1,group`
// proves it end to end (main.cpp), through the identical `applyLayerSetCommand()`
// funnel a click uses.
//
// What did NOT exist is this file's subject: a flat row list where a Group
// row looked exactly like any other layer and its members did not read as
// *inside* it. This is app/LayerPanel's pure half of that -- the same split
// the rest of that file already draws between "what a row says" (testable
// here) and "how it is drawn" (ui/MacPaintUI.cpp, unreachable from
// `--selftest`).
bool runLayerGroupPanelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] LAYERS panel group nesting: depth, ancestry and the "
              "collapsed-row predicate\n");
  std::printf("  docs/reachability-audit.md's C7 stays about a specific hole: no LAYERS "
              "gesture and no menu item issued GroupLayers/UngroupLayers. That hole was "
              "already filled by ui/MacPaintUI.cpp's pre-existing generic "
              "`core::allLayerSetCommands()` walk (see main.cpp's `--ui-multiselect-demo "
              "select:0.1,group`, unchanged by this step). What this step adds is visual: a "
              "group row distinguishable from an ordinary one, its members indented to read as "
              "inside it, and a collapse/expand triangle so a twenty-layer group does not "
              "defeat the panel's bounded scroll region (T11).\n");

  auto sel = [](std::vector<size_t> idx) { return makeLayerSelection(std::move(idx)); };
  // Five named RGB layers, bottom to top -- the same fixture
  // app/selftest/LayerGroup.cpp and LayerMultiSelect.cpp both build, copied
  // rather than shared (internal linkage cannot cross a translation unit).
  auto makeFive = []() {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    doc.layers[0].name = "A";
    for (const char* n : {"B", "C", "D", "E"}) addLayer(doc, doc.layers.size(), makeRgbLayer(n));
    return doc;
  };

  // ==========================================================================
  // Part A: a document with no groups at all -- every depth is 0, nothing is
  // ever hidden, regardless of what the collapsed set names.
  // ==========================================================================
  {
    Document doc = makeFive();
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      check(layerGroupAncestry(doc, i).empty(), "flat document: every layer's ancestry is empty");
      check(layerGroupDepth(doc, i) == 0, "flat document: every layer's depth is 0");
    }
    const std::set<std::string> collapsed = {"G1", "G2", "anything"};
    for (size_t i = 0; i < doc.layers.size(); ++i)
      check(!layerHiddenByCollapsedGroup(doc, i, collapsed),
            "flat document: nothing is hidden by ANY collapsed set -- there is no group tag on "
            "any layer for one to match");
    check(layerGroupAncestry(doc, 99).empty() && layerGroupDepth(doc, 99) == 0,
          "out-of-range index: empty ancestry, depth 0, not a crash");
  }

  // ==========================================================================
  // Part B: one group of three -- depth, ancestry, and collapse hiding
  // exactly the members, never the group row that names them.
  // ==========================================================================
  size_t groupIdxB = 0;
  std::string groupTagB;
  {
    Document doc = makeFive();
    const LayerSetOpResult r = applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));
    check(r.ok, "(setup) group {A,C,E}: succeeds");
    groupIdxB = r.selection.indices[0];
    groupTagB = doc.layers[groupIdxB].groupTag;

    check(layerGroupDepth(doc, groupIdxB) == 0,
          "group row itself: depth 0 -- it is top-level, its MEMBERS are the ones one step in");
    check(layerGroupAncestry(doc, groupIdxB).empty(),
          "group row itself: empty ancestry -- a group's own tag is never its own ancestor");

    // A, C, E (whichever three indices now carry groupTagB) each read depth 1
    // with a one-entry ancestry naming exactly this group.
    size_t membersFound = 0;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      if (doc.layers[i].parent != groupTagB) continue;
      ++membersFound;
      check(layerGroupDepth(doc, i) == 1, "member of a top-level group: depth 1");
      const std::vector<std::string> anc = layerGroupAncestry(doc, i);
      check(anc.size() == 1 && anc[0] == groupTagB,
            "member of a top-level group: ancestry is exactly [this group's own tag]");
    }
    check(membersFound == 3, "(setup) exactly three layers carry the new group's tag");

    // B and D were never selected -- untouched, depth 0.
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      const Layer& l = doc.layers[i];
      if (l.name == "B" || l.name == "D") check(layerGroupDepth(doc, i) == 0, "B and D: depth 0");
    }

    // Collapse: hides every member, never the group row, never an unrelated
    // top-level layer.
    const std::set<std::string> collapsedThis = {groupTagB};
    check(!layerHiddenByCollapsedGroup(doc, groupIdxB, collapsedThis),
          "collapse: the GROUP ROW itself is never hidden by its own tag -- that is what keeps "
          "the triangle reachable to expand it again");
    size_t hiddenCount = 0;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      const bool isMember = doc.layers[i].parent == groupTagB;
      const bool hidden = layerHiddenByCollapsedGroup(doc, i, collapsedThis);
      check(hidden == isMember,
            "collapse: hidden if and only if this layer's parent IS the collapsed tag");
      if (hidden) ++hiddenCount;
    }
    check(hiddenCount == 3, "collapse: exactly the three members are hidden, not the group and "
                            "not B or D");

    // An empty collapsed set, or one naming an unrelated tag, hides nothing.
    check(!layerHiddenByCollapsedGroup(doc, 0, {}), "collapse: an empty set hides nothing");
    check(!layerHiddenByCollapsedGroup(doc, 0, {"not-a-real-tag"}),
          "collapse: a set naming a tag nobody carries hides nothing");
  }

  // ==========================================================================
  // Part C: nesting -- depth accumulates, ancestry orders immediate parent
  // first, and collapsing the OUTER group hides the inner group's row too
  // (its own row, not merely its members), while collapsing only the INNER
  // group leaves the outer group's other member visible.
  // ==========================================================================
  {
    Document doc = makeFive();
    applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({0, 2, 4}));  // Group 1: A,C,E
    const size_t group1Idx = doc.layers.size() - 1;
    const std::string group1Tag = doc.layers[group1Idx].groupTag;
    // B and D are both still top-level (core/LayerSetOps.hpp section 5 / the
    // LayerGroup.cpp fixture: grouping {A,C,E} yields [B,D,A,C,E,Group 1]);
    // this part only needs D, to pair with Group 1 for the outer grouping.
    size_t dIdx = SIZE_MAX;
    for (size_t i = 0; i < doc.layers.size(); ++i)
      if (doc.layers[i].name == "D") dIdx = i;
    check(dIdx != SIZE_MAX, "(setup) D is still findable by name after part C's own grouping");

    const LayerSetOpResult outer =
        applyLayerSetOp(doc, LayerSetCommand::GroupLayers, sel({dIdx, group1Idx}));
    check(outer.ok, "(setup) group {D, Group 1}: succeeds -- nesting");
    const size_t group2Idx = outer.selection.indices[0];
    const std::string group2Tag = doc.layers[group2Idx].groupTag;
    // group1Idx is stale (the splice moved things); re-find Group 1 by kind
    // and by NOT being group2Idx.
    size_t group1IdxNow = SIZE_MAX;
    for (size_t i = 0; i < doc.layers.size(); ++i)
      if (doc.layers[i].kind == LayerKind::Group && doc.layers[i].groupTag == group1Tag)
        group1IdxNow = i;
    check(group1IdxNow != SIZE_MAX, "(setup) Group 1 is still findable by its own tag");

    check(layerGroupDepth(doc, group2Idx) == 0, "outer group (Group 2): depth 0 -- top-level");
    check(layerGroupDepth(doc, group1IdxNow) == 1,
          "inner group (Group 1): depth 1 -- ONE indent step, because it is itself a member of "
          "Group 2 now");
    const std::vector<std::string> group1Ancestry = layerGroupAncestry(doc, group1IdxNow);
    check(group1Ancestry.size() == 1 && group1Ancestry[0] == group2Tag,
          "inner group's ancestry: [Group 2's tag] -- immediate parent, and only that one");

    // A, C and E are Group 1's members: TWO steps deep, and their ancestry
    // orders immediate parent (Group 1) before outermost (Group 2) --
    // exactly what a reader would need to walk "in, then further in".
    size_t doubleDeepFound = 0;
    for (size_t i = 0; i < doc.layers.size(); ++i) {
      if (doc.layers[i].parent != group1Tag) continue;
      ++doubleDeepFound;
      check(layerGroupDepth(doc, i) == 2,
            "A/C/E, nested two deep: depth 2 -- one for Group 1, one for Group 2");
      const std::vector<std::string> anc = layerGroupAncestry(doc, i);
      check(anc.size() == 2 && anc[0] == group1Tag && anc[1] == group2Tag,
            "A/C/E's ancestry: [Group 1's tag, Group 2's tag] -- IMMEDIATE parent first, "
            "outermost last (this is the ordering a reversed walk would get backwards)");
    }
    check(doubleDeepFound == 3, "(setup) exactly three layers are Group 1's members");

    // D is Group 2's own DIRECT member (not through Group 1): one step deep.
    check(layerGroupDepth(doc, dIdx) == 1,
          "D, Group 2's direct member: depth 1 -- one step, not two; D was never inside Group 1");

    // Collapsing the OUTER group (Group 2) hides EVERYTHING beneath it,
    // including the INNER group's own row -- a member of a hidden group is
    // still hidden even though its own immediate parent (Group 1) is not
    // itself in the collapsed set.
    const std::set<std::string> collapseOuter = {group2Tag};
    check(layerHiddenByCollapsedGroup(doc, group1IdxNow, collapseOuter),
          "collapse OUTER: the INNER GROUP'S OWN ROW is hidden too -- a member of a group that "
          "is itself hidden must not surface just because its own immediate parent is open");
    check(layerHiddenByCollapsedGroup(doc, dIdx, collapseOuter), "collapse OUTER: D is hidden");
    for (size_t i = 0; i < doc.layers.size(); ++i)
      if (doc.layers[i].parent == group1Tag)
        check(layerHiddenByCollapsedGroup(doc, i, collapseOuter),
              "collapse OUTER: A/C/E (two levels down) are hidden too -- collapsing the "
              "outermost ancestor hides the whole nest, not just its direct members");
    check(!layerHiddenByCollapsedGroup(doc, group2Idx, collapseOuter),
          "collapse OUTER: Group 2's OWN row stays visible -- collapsing a group never hides "
          "the row that collapsed it");

    // Collapsing only the INNER group (Group 1) hides A/C/E but leaves D --
    // Group 2's OTHER direct member -- and Group 1's own row visible.
    const std::set<std::string> collapseInner = {group1Tag};
    check(!layerHiddenByCollapsedGroup(doc, dIdx, collapseInner),
          "collapse INNER only: D stays visible -- it was never inside Group 1");
    check(!layerHiddenByCollapsedGroup(doc, group1IdxNow, collapseInner),
          "collapse INNER only: Group 1's own row stays visible");
    check(!layerHiddenByCollapsedGroup(doc, group2Idx, collapseInner),
          "collapse INNER only: Group 2's row stays visible -- it is an ancestor, not a "
          "descendant, of the collapsed group");
    for (size_t i = 0; i < doc.layers.size(); ++i)
      if (doc.layers[i].parent == group1Tag)
        check(layerHiddenByCollapsedGroup(doc, i, collapseInner),
              "collapse INNER only: A/C/E are hidden -- they are exactly what collapsing Group "
              "1 means to hide");
  }

  // ==========================================================================
  // Part D: cycle safety -- a hand-built cycle a real `GroupLayers` cannot
  // produce (core/LayerSetOps.hpp section 5's own argument) must still
  // terminate rather than loop, the identical guarantee
  // core::groupAncestry() gives coverage.
  // ==========================================================================
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    addLayer(doc, 1, makeRgbLayer("Child"));
    Layer g0 = makeGroupLayer(doc, "G0");
    Layer g1 = makeGroupLayer(doc, "G1");
    const std::string g0Tag = g0.groupTag;  // captured before the moves below
    g0.parent = g1.groupTag;  // G0's parent is G1
    g1.parent = g0.groupTag;  // G1's parent is G0 -- a two-cycle
    doc.layers[1].parent = g0.groupTag;
    doc.layers.push_back(std::move(g0));
    doc.layers.push_back(std::move(g1));

    const std::vector<std::string> anc = layerGroupAncestry(doc, 1);
    check(!anc.empty() && anc.size() <= doc.layers.size(),
          "hand-built cycle: the walk TERMINATES -- if it ever regressed to an unbounded loop, "
          "this call would hang the whole suite rather than return, a stronger proof than a "
          "timer");
    check(layerGroupDepth(doc, 1) == anc.size(),
          "hand-built cycle: depth still agrees with the ancestry it is derived from");
    // And collapsing the cyclic group still hides the child -- the walk does
    // not have to make sense of the cycle to answer "is G0 among this
    // layer's ancestors", only to stop looking once it has seen it once.
    check(layerHiddenByCollapsedGroup(doc, 1, {g0Tag}),
          "hand-built cycle: collapsing G0 still hides its (cyclic) child -- the cycle guard "
          "does not have to make the ancestry MEANINGFUL, only make the walk STOP");
  }

  return ok;
}

}  // namespace np
