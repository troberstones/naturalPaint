#include "app/selftest/Support.hpp"

#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "ui/MacPaintUI.hpp"

namespace np {

// The intent rules behind PRD E3's five selection tools -- what a gesture
// MEANS, as opposed to what core/SelectionOps computes.
//
// This section exists because the rules are invisible to every other kind of
// check. Each one decides between two plausible behaviours, neither of which
// produces a wrong pixel: deselect or do nothing; commit or refuse. Get one
// backwards and the arithmetic stays perfect while the tool throws away work
// the user was in the middle of. A golden image cannot photograph an
// intention, and the mouse handler they used to live inside was unreachable
// from here at all -- which is why `commitDrawnSelection()` is declared in
// ui/MacPaintUI.hpp.
bool runSelectionToolsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto freshDoc = []() {
    OpenDocument od;
    od.document.width = 64;
    od.document.height = 64;
    return od;
  };
  const Selection shape = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);
  const PixelCoord inside{12, 12};

  // --- 1. An empty gesture, unmodified, deselects ------------------------
  {
    AppState st;
    OpenDocument od = freshDoc();
    od.selection = selectAll(64, 64);
    st.marqueeCombine = SelectionCombine::Replace;
    commitDrawnSelection(st, od, std::nullopt);
    check(!od.selection.has_value(),
          "tools: a click with no drag and no modifier DESELECTS -- what clicking off a "
          "marquee means in every editor");
  }

  // --- 2. An empty gesture WITH a modifier does nothing ------------------
  //
  // The rule that protects work in progress, and the one most likely to be
  // written the other way by reflex.
  {
    for (const SelectionCombine op : {SelectionCombine::Add, SelectionCombine::Subtract,
                                      SelectionCombine::Intersect}) {
      AppState st;
      OpenDocument od = freshDoc();
      od.selection = shape;
      st.marqueeCombine = op;
      commitDrawnSelection(st, od, std::nullopt);
      if (!od.selection.has_value() ||
          selectionCoverageAt(&*od.selection, inside) != 1.0f) {
        ok = false;
      }
    }
    check(ok,
          "tools: a MISSED gesture while holding a modifier changes nothing -- a stray click "
          "mid-Shift-add must not discard a selection built up over several drags");
  }

  // --- 3. Refining nothing is refused, not evaluated ---------------------
  {
    AppState st;
    OpenDocument od = freshDoc();
    st.marqueeCombine = SelectionCombine::Subtract;
    commitDrawnSelection(st, od, shape);
    const bool subtractRefused = !od.selection.has_value();

    OpenDocument od2 = freshDoc();
    st.marqueeCombine = SelectionCombine::Intersect;
    commitDrawnSelection(st, od2, shape);
    const bool intersectRefused = !od2.selection.has_value();

    check(subtractRefused && intersectRefused,
          "tools: Subtract and Intersect against NO selection are refused, not evaluated -- "
          "either would install an engaged selection covering nothing, from a gesture meant "
          "as a refinement");

    // Add against nothing is the opposite call, and it must commit: adding to
    // nothing is the first drag of every multi-drag selection.
    OpenDocument od3 = freshDoc();
    st.marqueeCombine = SelectionCombine::Add;
    commitDrawnSelection(st, od3, shape);
    check(od3.selection.has_value() &&
              selectionCoverageAt(&*od3.selection, inside) == 1.0f,
          "tools: but ADD against no selection COMMITS -- it is the first drag of a "
          "multi-drag selection, not a refinement of something absent");
  }

  // --- 4. A real gesture combines rather than replaces --------------------
  {
    AppState st;
    OpenDocument od = freshDoc();
    od.selection = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);
    st.marqueeCombine = SelectionCombine::Add;
    commitDrawnSelection(st, od, selectRectangle(30.0f, 30.0f, 40.0f, 40.0f));
    check(od.selection.has_value() &&
              selectionCoverageAt(&*od.selection, PixelCoord{12, 12}) == 1.0f &&
              selectionCoverageAt(&*od.selection, PixelCoord{34, 34}) == 1.0f,
          "tools: a modified gesture with a real shape COMBINES -- both the old region and "
          "the new one survive, which is the whole point of the modifier");

    // And the revision moved, or the marching ants and the GPU coverage upload
    // would both still be showing the previous selection.
    OpenDocument od2 = freshDoc();
    const uint64_t before = od2.selectionRevision;
    st.marqueeCombine = SelectionCombine::Replace;
    commitDrawnSelection(st, od2, shape);
    check(od2.selectionRevision != before,
          "tools: installing bumps selectionRevision -- the cached bounds and the GPU "
          "coverage are keyed on it, so a selection that changed without it would draw and "
          "gate as the previous one");
  }

  std::printf("[selftest] selection tools %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
