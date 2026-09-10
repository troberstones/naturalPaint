#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "app/PathOps.hpp"
#include "core/Layer.hpp"

// app/PathsPanel -- the two questions the PATHS panel asks every frame, kept
// where a test can ask them too (docs/path-editing-plan.md section 4).
//
// ==========================================================================
// 1. WHY THIS IS NOT SIMPLY WRITTEN INSIDE THE PANEL BODY
// ==========================================================================
//
// `ui/MacPaintUI.cpp`'s `drawPathsSection()` is a thin caller, the same way
// the flats panel is a thin caller of `flats/Model`. Both questions below are
// pure functions of the document, they decide what the user is ALLOWED to
// press, and `--selftest` opens no window -- so a copy of either living in an
// ImGui function would be a rule with no test, which is the shape of defect
// [[naturalpaint-test-that-tests-a-copy]] names.
//
// There is deliberately nothing else here. Layout, labels, tooltips and the
// status line are the panel's, and a header that started collecting them
// would end up being the panel.
namespace np {

// Every `PathOp`, in the order the panel lays them out, so the sweep below
// and the buttons cannot disagree about which verbs exist.
//
// **Written out rather than derived from the enum**, for `app/selftest/
// ControlsLayout.cpp`'s stated reason: a list derived from the thing it
// checks agrees with a bug instead of catching it.
inline constexpr std::array<PathOp, 11> kPathPanelOps = {
    PathOp::Close,        PathOp::Open,         PathOp::Join,
    PathOp::Reverse,      PathOp::MakeCompound, PathOp::ReleaseCompound,
    PathOp::Smooth,       PathOp::Corner,       PathOp::Break,
    PathOp::InsertAnchor, PathOp::DeleteAnchor,
};

// One frame's answer to "what does this selection permit?", one entry per
// `kPathPanelOps` slot.
//
// **The panel greys on this and nothing else.** `app/PathOps.hpp` section 2
// is explicit that a button re-deriving its own precondition is a second
// implementation of every rule in that file, in the file least able to test
// it -- so the sweep asks `pathOpCanRun()` and the button asks the sweep.
struct PathOpAvailability {
  std::array<PathOpRefusal, kPathPanelOps.size()> refusal{};

  PathOpRefusal refusalFor(PathOp op) const noexcept;
  // The greying rule itself, in one place: lit exactly when the verb would
  // run. A lit button therefore cannot refuse, which is the property
  // app/PathOps.hpp section 2 asks the panel to hold up.
  bool enabled(PathOp op) const noexcept;
};

PathOpAvailability pathOpAvailability(const std::vector<VectorShape>& shapes,
                                      const PathSelection& selection);

// --- MAKE FILL / MAKE STROKE's target -------------------------------------

// Which of the two consumers is asking. They take different layer kinds, so
// they get different answers rather than one union that lies to one of them:
// `fillPathIntoLayer()` writes premultiplied rgba16float and refuses a
// Pigment layer by name (app/PathConsumers.hpp), while
// `strokePathWithBrush()` has a route for each.
enum class PathPaintKind { Fill, Stroke };

// The layer MAKE FILL / MAKE STROKE would paint into: the nearest layer
// BELOW `vectorIndex` that holds texels of a kind this consumer can write.
//
// **Below, because a Vector layer is the source and cannot be the target.**
// The active layer is the Vector one -- that is what makes this panel live at
// all -- so unlike every other paint command in the build these two need a
// second layer, and "the one under the path" is both Photoshop's answer and
// the only one a user can predict without a picker. `layers` is bottom-to-top
// (core/Document.hpp), so this walks DOWN in index.
//
// **Filtered on KIND alone, deliberately.** A locked or alpha-locked
// candidate is returned, so the consumer's own refusal sentence reaches the
// status line and says which layer and why. Skipping it here would grey the
// button with no explanation and, worse, silently paint into a different
// layer further down than the one the user was looking at.
std::optional<size_t> pathPaintTargetBelow(const std::vector<Layer>& layers, size_t vectorIndex,
                                           PathPaintKind kind);

}  // namespace np
