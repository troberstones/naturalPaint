#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "brush/StrokePath.hpp"
#include "core/OpStack.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Only tools that actually do something are listed. Lasso, marquee and text
// belong to the MacPaint chrome but have no meaning yet for a fluid canvas, so
// they are deliberately absent rather than present and dead.
enum class Tool {
  Brush,       // water + pigment
  Water,       // pre-wet the paper, no pigment
  DryBrush,    // little water, hard edge, pigment sits on the tooth
  Eyedropper,
  Hand,
  Zoom,
  Count
};

struct BrushState {
  Tool tool = Tool::Brush;
  int pigment = 6;  // Ultramarine Blue
  float radius = 20.0f;
  float load = 0.9f;      // pigment concentration
  float wetness = 1.3f;   // water deposited
  float hardness = 0.35f;
  bool pressureSize = true;
  bool pressureFlow = true;
  // Arc-length dab spacing (CONTEXT.md "Dab", ADR-0003), in units of the
  // current brush radius: a dab emits every `spacing * radius` px of travel.
  // 0.25 is the conventional middle ground among painting apps (most sit in
  // roughly a 0.1-0.3 range) between visibly discrete stamps (spacing too
  // large) and dab-count/overdraw cost (spacing too small). No UI control
  // yet -- exposing it is a later phase's job; this is just the constant
  // default.
  float spacing = 0.25f;
};

// PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4): zoom/pan plus
// independent axis mirrors, a rotation angle and a grayscale-preview toggle.
// All four of the new fields below are *view* state -- nothing in
// core/Document, core/Layer or sim/PaintSim ever reads a CanvasView, so
// flipping any of them can't touch the document by construction, not just by
// convention (--selftest's runViewTransformTest() asserts this for the one
// field, grayscale, that does reach into GPU state at all).
//
// zoom/panX/panY keep their original meaning unchanged (NaturalPaintUI.cpp's
// tileScreenRect()/zoomOnWheel()/applyPan() -- a separate, read-only tiled
// viewer that has no need for mirror/rotate -- read only these three fields
// and are untouched by this step). mirrorX/mirrorY/rotation are composed
// together with zoom/pan into one affine transform by app/ViewTransform.hpp;
// see that header and ui/MacPaintUI.cpp's canvas block for where pen input
// maps back through that transform's actual analytic inverse, per
// docs/shortcuts.md section 3's own mandate ("mirror as a special case in
// the draw path is what makes painting-under-mirror land in the wrong
// spot" -- PLAN.md).
struct CanvasView {
  float zoom = 1.0f;
  float panX = 0.0f;
  float panY = 0.0f;
  bool mirrorX = false;   // PRD Q2: left/right, independent of mirrorY
  bool mirrorY = false;   // PRD Q2: up/down, independent of mirrorX
  float rotation = 0.0f;  // PRD Q4: radians, arbitrary angle, about canvas centre
  // PRD Q3: a per-pixel luminance pass (sim/PaintSim's updateGrayscalePreview
  // + shaders/grayscale_blit.wgsl), not a geometric part of the transform --
  // kept here anyway because it is still pure view state, never document
  // state.
  bool grayscale = false;
  // PLAN.md Phase 3 step 6 ("Apply pass") / step 8 ("Op-stack UI"): the
  // grading preview toggle, the same "pure view state, never document
  // state" shape as grayscale immediately above -- but unlike grayscale (a
  // self-contained GPU blit with no other inputs), this one reaches into
  // AppState::opStack and sim::PaintSim's bake/blit pipeline
  // (PaintSim::updateGradePreview() + shaders/grade_blit.wgsl) rather than
  // shaders/grayscale_blit.wgsl alone. User-visible and explicit, exactly
  // like grayscale: grading does NOT silently switch on just because
  // st.opStack becomes non-empty -- a user builds a stack via the GRADE
  // section's real op-authoring UI (ui/MacPaintUI.cpp, step 8) and flips
  // this on separately to preview it, via the "Preview Graded Output"
  // checkbox at that section's top (the one and only way to toggle it --
  // step 6's earlier "Test Grade (debug)" View-menu item, which hardcoded
  // two fixed op indices main.cpp no longer seeds, is gone).
  bool grade = false;
};

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// A guide is a single line pinned at one document-space coordinate along the
// axis *perpendicular* to its own orientation -- a Horizontal guide is a
// horizontal line at a fixed Y, a Vertical guide a vertical line at a fixed
// X, matching Photoshop's own naming. Drawn (app/Snapping.hpp's
// resolveSnap(), ui/MacPaintUI.cpp) through the same ViewTransform every
// other document-space thing on the canvas goes through -- never a second,
// screen-space-only position.
enum class GuideOrientation { Horizontal, Vertical };

struct Guide {
  GuideOrientation orientation = GuideOrientation::Horizontal;
  float position = 0.0f;
};

struct AppState {
  PaintMode mode = PaintMode::Watercolor;
  // Seconds a wash keeps moving before it sets. Drives evaporation and
  // absorption together via setWorkingTime(); 15 matches the shipped defaults.
  float workingTime = 15.0f;
  BrushState brush;
  CanvasView view;
  SimParams sim;

  // Stroke, in canvas texel space.
  float lastX = 0.0f, lastY = 0.0f;
  bool strokeActive = false;
  bool strokeStarting = false;

  // Arc-length dab emission (1.3 / ADR-0003). MacPaintUI feeds strokePath
  // one raw pointer sample per render frame; it comes back with 0..N dab
  // positions in pendingDabs, which main.cpp drains into
  // PaintSim::depositDab() calls before running physics this frame.
  // strokePath.reset() on every fresh stroke; strokePath.flush() on
  // pen-up -- see MacPaintUI.cpp.
  StrokePath strokePath;
  std::vector<Vec2> pendingDabs;
  // The most recent dab position, carried across render frames. Oil's
  // contact/velocity/transfer pipeline still runs inside PaintSim::frame()
  // (see PaintSim.hpp's depositDab() comment) and needs a genuine segment --
  // not a collapsed point -- for its tangential brush-velocity term
  // (shaders/oil_velocity.wgsl's `vb`), so MacPaintUI feeds it
  // (lastDabX/Y -> the newest dab this frame) rather than brushA == brushB.
  float lastDabX = 0.0f, lastDabY = 0.0f;
  // True on any render frame where the user was actively painting (down,
  // hovered, inside the canvas, not panning) -- regardless of whether that
  // frame happened to cross a dab's spacing threshold. Distinct from
  // st.sim.brushActive, which (post-1.3) means "oil has a fresh dab-sourced
  // segment to act on this frame" and is usually false on most frames of a
  // normal stroke. Latency::recordFrame's "did this frame draw a point"
  // sample wants this one, not that one -- see main.cpp.
  bool paintingThisFrame = false;

  // SDL3 pen state. penSeen stays false on a mouse-only machine, in which case
  // pressure is pinned to 1.
  bool penSeen = false;
  bool penDown = false;
  float penPressure = 1.0f;

  // Freshest pointer-input timestamp (SDL_GetTicksNS) seen during this
  // frame's poll loop; 0 if no pen/mouse sample arrived this frame. Feeds
  // Latency::recordFrame() once the frame that used it has been presented.
  uint64_t lastInputEventNs = 0;

  bool showDemo = false;
  bool paused = false;
  bool requestClear = false;
  bool requestMode = false;
  bool requestReload = false;
  // View commands that need the canvas window's actual on-screen size
  // (ImGui's GetContentRegionAvail()), which only exists inside MacPaintUI's
  // canvas Begin()/End() block -- same request-then-consume shape as
  // requestClear/requestMode/requestReload above, just consumed one block
  // deeper (PLAN.md Phase 2 step 11, PRD Q1: fit to window, 100%, and
  // keyboard-triggered zoom in/out all need to know how big the canvas
  // currently is on screen).
  bool requestFitWindow = false;
  bool requestZoom100 = false;
  bool requestZoomIn = false;
  bool requestZoomOut = false;
  bool quit = false;

  // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD
  // Q5-Q7). Session-local view state, the same treatment every other
  // AppState field above gets -- not document state. Photoshop-style guides
  // are conceptually document content (they'd be saved with the file), but
  // this codebase has no document-save path at all yet, and the interactive
  // canvas (this struct, MacPaintUI.cpp) has no core::Document/layer-stack
  // awareness -- it only knows sim::PaintSim's single dense texture. That
  // bridge is a real, separately-tracked, deliberately deferred gap; true
  // guide persistence waits for both a save path and a live-canvas-to-
  // Document bridge to exist. Until then, guides live here and vanish with
  // the session, like zoom/pan/mirror/rotation already do.
  std::vector<Guide> guides;

  // PLAN.md Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch ->
  // un-shape") / step 5 (`core/OpStack`). The ordered grading op stack
  // itself -- conceptually document content (a real save path would
  // persist it alongside the pixels, and ADR-0004's whole design is about
  // *document* grading), the same way `guides` just above is conceptually
  // document content in Photoshop's own convention -- but this codebase
  // has no save path and no live-canvas-to-Document bridge: sim::PaintSim
  // exposes a single dense texture with no core::Document/core::Layer
  // awareness at all, the identical gap `guides`' own comment describes.
  // Until that bridge exists, the op stack lives here and vanishes with
  // the session, exactly like guides/zoom/pan/mirror/rotation already do.
  // Read by sim::PaintSim::updateGradePreview() (via CanvasView::grade,
  // see ui/MacPaintUI.cpp's canvas block) -- core::OpStack itself stays
  // completely unaware AppState exists.
  OpStack opStack;

  // Menu-toggleable, no keyboard shortcut (docs/shortcuts.md §3 assigns
  // rulers to Cmd+R, but keymaps/default.json already binds Cmd+R to
  // reload_shaders from earlier Phase-1 work -- see main.cpp's key-down
  // dispatch comment for the full reasoning). Rulers are only meaningful at
  // view.rotation == 0 -- see MacPaintUI.cpp's canvas block for how a
  // rotated view degrades the ruler strips rather than drawing nonsense.
  bool showRulers = false;
  bool showGuides = true;
  bool showGrid = false;
  // PRD Q6: global toggle. When true, dragging a new guide off a ruler
  // snaps to existing guides, grid lines and canvas edges (app/Snapping.hpp
  // resolveSnap()) -- never freehand brush painting, which has no code path
  // into resolveSnap() at all.
  bool snappingEnabled = true;
  // PRD Q7: grid spacing and subdivision count, document-space px. Edited
  // via a couple of sliders in MacPaintUI's controls panel.
  float gridSpacing = 64.0f;
  int gridSubdivisions = 4;
  // A guide currently being dragged off a ruler (PRD Q5 drag-to-create),
  // following the cursor until mouse-release commits it into `guides`.
  // nullopt when no such drag is in progress.
  std::optional<Guide> pendingGuide;

  float frameMs = 0.0f;
};

}  // namespace np
