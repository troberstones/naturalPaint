#include "app/selftest/Support.hpp"

#include "app/PanelLayout.hpp"
#include "app/PathsPanel.hpp"
#include "app/PenTool.hpp"

namespace np {
namespace {

// One shape, one open subpath through `pts`, every handle DISTINCT from its
// own anchor -- `app/selftest/PathOps.cpp`'s convention and for its reason: a
// verb that loses a handle cannot hide behind `in == out == pt`.
VectorShape lineShape(uint64_t id, std::vector<PathPoint> pts) {
  VectorShape s;
  s.id = id;
  SubPath sub;
  sub.closed = false;
  for (const PathPoint& p : pts) {
    Anchor a;
    a.pt = p;
    a.in = PathPoint{p.x - 1.0f, p.y};
    a.out = PathPoint{p.x + 1.0f, p.y};
    sub.anchors.push_back(a);
  }
  s.path.subpaths.push_back(std::move(sub));
  return s;
}

PathSelection shapeSel(std::vector<uint64_t> ids) {
  PathSelection sel;
  sel.mode = PathSelectMode::Shape;
  sel.shapes = std::move(ids);
  return sel;
}

PathSelection componentSel(std::vector<ComponentRef> refs) {
  PathSelection sel;
  sel.mode = PathSelectMode::Component;
  sel.components = std::move(refs);
  return sel;
}

ComponentRef ref(uint64_t shape, uint32_t sub, uint32_t anchor) {
  ComponentRef c;
  c.shapeId = shape;
  c.subPath = sub;
  c.anchor = anchor;
  c.part = AnchorPart::Point;
  return c;
}

Layer rgbLayer(const char* name) {
  Layer l;
  l.kind = LayerKind::RGB;
  l.name = name;
  l.rgbTiles = TileStore{};
  return l;
}

Layer pigmentLayer(const char* name) {
  Layer l;
  l.kind = LayerKind::Pigment;
  l.name = name;
  l.pigmentTiles = PigmentTileStore{};
  return l;
}

Layer vectorLayer(const char* name) {
  Layer l;
  l.kind = LayerKind::Vector;
  l.name = name;
  return l;
}

}  // namespace

// The PATHS panel (docs/path-editing-plan.md section 4): its registration, the
// rule that decides what it greys, the repair it owes `PathEditState` after
// every verb, and the target rule its two painting buttons need and no other
// paint command in this build does.
//
// Headless and GPU-free. Writes no files, opens no window, and touches no
// `ui/` file -- which is the whole reason `app/PathsPanel` exists as a module
// rather than as thirty lines inside an ImGui function.
//
// **The fourth registration table is checked by the COMPILER, not from here.**
// `drawPanelBody()`'s switch over `ControlsSection` has no `default:` arm and
// `src/CMakeLists.txt` builds with `-Werror=switch`, so a new enumerator with
// no case is a build failure with the enumerator named in it. An assertion
// here could only re-read the source text, which is a weaker check than the
// one already in force.
bool runPathsPanelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  std::printf("[selftest] paths panel: no window, no GPU, no file\n");

  // =======================================================================
  // 1. Registration -- three tables, asserted; the fourth is -Werror=switch
  // =======================================================================
  {
    const ControlsSectionSpec& spec = controlsSectionSpec(ControlsSection::Paths);
    check(spec.section == ControlsSection::Paths && std::string(spec.title) == "PATHS" &&
              spec.role == ControlsSectionRole::Tool,
          "paths panel: PATHS is registered with a Tool role and its own title");

    // The role sequence stays non-decreasing -- `app/selftest/ControlsLayout.
    // cpp` asserts that globally, and this pins the neighbour PATHS was
    // inserted next to, which is the thing an edit here is most likely to
    // break.
    const std::vector<ControlsSectionSpec>& all = controlsSections();
    size_t idx = all.size();
    for (size_t i = 0; i < all.size(); ++i)
      if (all[i].section == ControlsSection::Paths) idx = i;
    check(idx > 0 && idx + 1 < all.size() &&
              all[idx - 1].section == ControlsSection::FlatsTools &&
              all[idx].role == ControlsSectionRole::Tool &&
              all[idx + 1].role == ControlsSectionRole::Document,
          "paths panel: PATHS is the LAST Tool-role section, straight after FLATS TOOLS");

    // The persistence key, in both directions. A key that serialises but does
    // not parse would put the panel back on its default placement on the next
    // launch and lose wherever the user docked it.
    check(std::string(controlsSectionKey(ControlsSection::Paths)) == "paths",
          "paths panel: the layout file spells it \"paths\"");
    ControlsSection back = ControlsSection::Tools;
    check(controlsSectionFromKey("paths", &back) && back == ControlsSection::Paths,
          "paths panel: \"paths\" reads back as ControlsSection::Paths");

    // The placement, through a real default layout rather than through the
    // file-local `defaultPlacementFor()`: what matters is where the panel
    // actually starts.
    PanelLayout layout;
    check(layout.placementOf(ControlsSection::Paths) == PanelPlacement::Flyout,
          "paths panel: PATHS starts on the flyout rail, not in the right dock");
  }

  // =======================================================================
  // 2. The sweep covers every verb, and maps each to its OWN answer
  // =======================================================================
  {
    // Hand-written, deliberately -- deriving this from `kPathPanelOps` would
    // make the check agree with a bug instead of catching it (app/selftest/
    // ControlsLayout.cpp's own stated reason for its `kAll`).
    const PathOp kEveryOp[] = {
        PathOp::Close,        PathOp::Open,         PathOp::Join,
        PathOp::Reverse,      PathOp::Smooth,       PathOp::Corner,
        PathOp::Break,        PathOp::InsertAnchor, PathOp::DeleteAnchor,
        PathOp::MakeCompound, PathOp::ReleaseCompound,
        PathOp::Unite,        PathOp::Intersect,    PathOp::Subtract,
        PathOp::Exclude,
    };
    bool eachOnce = kPathPanelOps.size() == std::size(kEveryOp);
    for (const PathOp op : kEveryOp) {
      size_t seen = 0;
      for (const PathOp p : kPathPanelOps)
        if (p == op) ++seen;
      if (seen != 1) eachOnce = false;
    }
    check(eachOnce, "paths panel: the frame sweep lists every PathOp exactly once");
  }

  // The greying rule IS `pathOpCanRun()`, and the panel asks it through the
  // sweep. Checked per op against the verb's own answer, so a sweep that
  // returned the RIGHT set of refusals against the WRONG slots -- an off-by-
  // one in `refusalFor()`, the likeliest defect in this file -- is caught.
  {
    struct Fixture {
      const char* what;
      std::vector<VectorShape> shapes;
      PathSelection selection;
    };
    std::vector<Fixture> fixtures;
    fixtures.push_back({"one open path, whole-shape selection",
                        {lineShape(1, {{0, 0}, {10, 0}, {20, 0}})},
                        shapeSel({1})});
    fixtures.push_back({"two endpoint anchors of one open path",
                        {lineShape(1, {{0, 0}, {10, 0}, {20, 0}})},
                        componentSel({ref(1, 0, 0), ref(1, 0, 2)})});
    fixtures.push_back({"nothing selected", {lineShape(1, {{0, 0}, {10, 0}})}, PathSelection{}});
    fixtures.push_back({"two whole shapes",
                        {lineShape(1, {{0, 0}, {10, 0}, {10, 10}}), lineShape(2, {{5, 5}, {15, 5}, {15, 15}})},
                        shapeSel({1, 2})});

    bool agrees = true;
    // **The guard against a fixture whose right and wrong answers coincide.**
    // If every op in every fixture returned the same refusal, an availability
    // sweep that ignored `op` entirely would agree with the verbs perfectly
    // and this section would be inert. So the fixtures must between them
    // produce several DISTINCT refusals, and that is asserted, not assumed.
    std::vector<PathOpRefusal> seenRefusals;
    for (const Fixture& f : fixtures) {
      const PathOpAvailability avail = pathOpAvailability(f.shapes, f.selection);
      for (const PathOp op : kPathPanelOps) {
        const PathOpRefusal want = pathOpCanRun(op, f.shapes, f.selection);
        if (avail.refusalFor(op) != want) agrees = false;
        if (avail.enabled(op) != (want == PathOpRefusal::None)) agrees = false;
        if (std::find(seenRefusals.begin(), seenRefusals.end(), want) == seenRefusals.end())
          seenRefusals.push_back(want);
      }
    }
    check(agrees,
          "paths panel: every button greys exactly on its own verb's pathOpCanRun() answer");
    check(seenRefusals.size() >= 5,
          "premise: the fixtures produce at least five DISTINCT refusals, so a sweep that "
          "ignored the op could not pass the check above");

    // And the property the greying exists to hold up: a LIT button cannot
    // refuse. Run every enabled verb for real and confirm it changed
    // something -- the panel's promise, checked rather than reasoned about.
    bool litRuns = true;
    for (const Fixture& f : fixtures) {
      const PathOpAvailability avail = pathOpAvailability(f.shapes, f.selection);
      for (const PathOp op : kPathPanelOps) {
        if (!avail.enabled(op)) continue;
        std::vector<VectorShape> shapes = f.shapes;
        uint64_t nextId = 100;
        const PathOpResult r = runPathOp(op, &shapes, &nextId, f.selection);
        if (!r.changed || r.refusal != PathOpRefusal::None) litRuns = false;
      }
    }
    check(litRuns, "paths panel: a lit button cannot refuse -- every enabled verb ran");
  }

  // =======================================================================
  // 3. pathEditPruneSelection() -- the dangle after an erasure
  // =======================================================================
  //
  // `PathOpResult::erasedShapes` is the panel's obligation, not its
  // information: a selection or an open placement session naming a shape that
  // a JOIN or a DELETE consumed is a dangle, and `pathEditBeginPen()` already
  // carries a defensive arm for exactly it.
  {
    const std::vector<VectorShape> before = {lineShape(1, {{0, 0}, {10, 0}, {20, 0}}),
                                             lineShape(2, {{0, 5}, {10, 5}})};
    const std::vector<VectorShape> after = {before[0]};  // shape 2 erased

    {
      PathEditState st;
      st.selection = shapeSel({1, 2});
      pathEditPruneSelection(&st, after);
      // BOTH halves: the erased id goes AND the survivor stays. A prune that
      // simply cleared the selection would pass the first half alone.
      check(st.selection.shapes.size() == 1 && st.selection.shapes[0] == 1,
            "paths panel: an erased shape leaves the shape selection, and the survivor stays");
    }

    {
      PathEditState st;
      st.selection = componentSel({ref(1, 0, 0), ref(2, 0, 0), ref(2, 0, 1)});
      pathEditPruneSelection(&st, after);
      check(st.selection.components.size() == 1 && st.selection.components[0].shapeId == 1,
            "paths panel: component refs onto an erased shape go, and the survivor's stay");
    }

    {
      // The open placement session -- the case app/PathOps.hpp names by
      // number. Placement open ON the shape that was erased.
      PathEditState st;
      st.selection = shapeSel({1});
      st.openPathActive = true;
      st.openPathShapeId = 2;
      st.openPathSubPath = 0;
      pathEditPruneSelection(&st, after);
      check(!pathEditHasOpenPath(st) && st.openPathShapeId == 0,
            "paths panel: an open placement session on an erased shape ENDS, ids cleared");
    }

    {
      // Placement open on a SURVIVING shape whose subpath count shrank. No id
      // disappeared, so a prune written against `erasedShapes` alone would
      // leave `openPathSubPath` indexing past the end -- which is what
      // `ui/`'s overlay dereferences to draw the rubber band.
      std::vector<VectorShape> two = {lineShape(1, {{0, 0}, {10, 0}})};
      two[0].path.subpaths.push_back(two[0].path.subpaths[0]);
      const std::vector<VectorShape> one = {lineShape(1, {{0, 0}, {10, 0}})};

      PathEditState st;
      st.selection = componentSel({ref(1, 0, 0), ref(1, 1, 0)});
      st.openPathActive = true;
      st.openPathShapeId = 1;
      st.openPathSubPath = 1;
      pathEditPruneSelection(&st, one);
      check(!pathEditHasOpenPath(st) && st.selection.components.size() == 1 &&
                st.selection.components[0].subPath == 0,
            "paths panel: a stale subpath INDEX dangles too, with no id having gone");
    }

    {
      // The no-op. Without this, "prune everything, always" passes every
      // assertion above -- the coinciding-answers trap, in the one place it
      // would actually have bitten.
      PathEditState st;
      st.selection = shapeSel({1, 2});
      st.openPathActive = true;
      st.openPathShapeId = 2;
      st.openPathSubPath = 0;
      pathEditPruneSelection(&st, before);
      check(st.selection.shapes.size() == 2 && pathEditHasOpenPath(st) &&
                st.openPathShapeId == 2,
            "paths panel: a prune against geometry that still resolves changes nothing");
    }
  }

  // `pathEditSelectShapes()` -- the shape list's row click and RELEASE's
  // re-selection, and the reason neither writes `st.pathEdit` from `ui/`.
  {
    const std::vector<VectorShape> shapes = {lineShape(1, {{0, 0}, {10, 0}}),
                                             lineShape(2, {{0, 5}, {10, 5}})};
    PathEditState st;
    st.selection = componentSel({ref(2, 0, 0)});
    pathEditSelectShapes(&st, {1}, SelectionCombine::Replace, shapes);
    check(st.selection.mode == PathSelectMode::Shape && st.selection.shapes.size() == 1 &&
              st.selection.shapes[0] == 1 && st.selection.components.empty(),
          "paths panel: a shape-list click leaves Component mode and takes the row it hit");

    pathEditSelectShapes(&st, {2}, SelectionCombine::Add, shapes);
    check(st.selection.shapes.size() == 2,
          "paths panel: shift-clicking a second row extends rather than replaces");

    // A writer that could install a dangle would make the prune above a thing
    // callers must remember instead of an invariant.
    pathEditSelectShapes(&st, {99}, SelectionCombine::Replace, shapes);
    check(st.selection.shapes.empty(),
          "paths panel: an id no shape carries is dropped, never selected");
  }

  // =======================================================================
  // 4. MAKE FILL / MAKE STROKE's target
  // =======================================================================
  //
  // The one rule in this panel with no precedent elsewhere in the build: every
  // other paint command targets the ACTIVE layer, and here the active layer is
  // the Vector one holding the source geometry.
  {
    // Bottom to top (core/Document.hpp): an RGB layer, a Pigment layer, the
    // Vector layer, and an RGB layer ABOVE it.
    //
    // **Deliberately not an RGB layer directly below.** With one there, FILL
    // and STROKE would return the same index and a target rule that ignored
    // `kind` entirely would pass -- the coinciding-answers trap. With Pigment
    // in between, the two consumers must give different answers.
    std::vector<Layer> layers;
    layers.push_back(rgbLayer("under"));
    layers.push_back(pigmentLayer("pig"));
    layers.push_back(vectorLayer("vec"));
    layers.push_back(rgbLayer("over"));

    const std::optional<size_t> fill = pathPaintTargetBelow(layers, 2, PathPaintKind::Fill);
    const std::optional<size_t> stroke = pathPaintTargetBelow(layers, 2, PathPaintKind::Stroke);
    check(fill.has_value() && *fill == 0,
          "paths panel: FILL skips the Pigment layer it cannot write and takes the RGB one");
    check(stroke.has_value() && *stroke == 1,
          "paths panel: STROKE takes the NEAREST layer below it can write -- the Pigment one");

    check(!pathPaintTargetBelow(layers, 0, PathPaintKind::Fill).has_value() &&
              !pathPaintTargetBelow(layers, 0, PathPaintKind::Stroke).has_value(),
          "paths panel: the bottom layer has nothing below it, so both buttons grey");
    check(!pathPaintTargetBelow(layers, 99, PathPaintKind::Fill).has_value(),
          "paths panel: an index off the end of the stack targets nothing rather than "
          "reading past it");

    // A Group holds no pixels of any kind, and a kind test alone would hand
    // one to a consumer that then refuses it with a sentence naming a
    // malformation the user did not cause.
    std::vector<Layer> withGroup;
    withGroup.push_back(rgbLayer("under"));
    Layer group;
    group.kind = LayerKind::Group;
    group.name = "grp";
    withGroup.push_back(std::move(group));
    withGroup.push_back(vectorLayer("vec"));
    const std::optional<size_t> past = pathPaintTargetBelow(withGroup, 2, PathPaintKind::Fill);
    check(past.has_value() && *past == 0,
          "paths panel: a Group between the path and the paint is passed over, not targeted");
  }

  return ok;
}

}  // namespace np
