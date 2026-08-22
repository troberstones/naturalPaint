#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/Journal.hpp"
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
// tileScreenRect() -- pure tile-to-screen placement geometry with no need
// for mirror/rotate -- reads only these three fields and is untouched by
// this step). mirrorX/mirrorY/rotation are composed
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
  // --controls-all-open (UI detour step 3): opens every collapsing header in
  // the right-hand controls column on the first frame, overriding
  // `app::controlsSections()`' default-open set for that session only.
  //
  // It exists for one job: `--screenshot` has to be able to photograph a
  // section the default state deliberately closes, and the label-clipping fix
  // (app/ControlsLayout.hpp) is only visible on the simulation sliders, which
  // are exactly the sections that now start collapsed. A verification claim
  // that cannot be photographed is a verification claim on trust.
  //
  // `ImGuiCond_Once`, so it is a starting state and not a mode: a header
  // closed by hand after that stays closed.
  bool controlsAllOpen = false;
  // --open-layer-menu (UI detour step 3): holds the `Layer` menu open, so a
  // `--screenshot` can photograph its items. Same justification as
  // `controlsAllOpen` above -- a menu is opened by a click, and the screenshot
  // path has no input.
  bool openLayerMenu = false;
  // --open-export-states (PLAN.md Phase 5 step 13): holds the File > Export
  // Comps / Layers To Files... modal open, so a `--screenshot` can photograph
  // it. `openLayerMenu`'s justification exactly -- a modal is opened by a
  // click and the screenshot path has no input.
  bool openExportStatesDialog = false;
  // --open-export-states <FOLDER>: prefills that dialog's output folder, so a
  // `--screenshot` can photograph the plan table -- the list of exact
  // filenames the export would write -- which is the part of the dialog worth
  // photographing and which stays empty without a folder to resolve against.
  // `controlsScrollTo`'s pattern, one dialog over.
  std::string exportStatesFolder;
  // --open-layer-properties: holds the LAYERS panel's own gear-button modal
  // open, so a `--screenshot` can photograph it -- `openExportStatesDialog`'s
  // justification exactly, one dialog over: it too is opened by a click and
  // the screenshot path has no input.
  bool openLayerProperties = false;
  // --controls-all-open <SECTION>: scrolls that header to the top of the
  // column, every frame, so a `--screenshot` can photograph a section that
  // sits below the fold once every section is open. Empty means "do not
  // scroll". Matched against `ControlsSectionSpec::title` exactly.
  //
  // Pinning rather than scrolling once is deliberate and is why this is a
  // debug flag rather than a feature: the column's layout settles over several
  // frames as headers open, so a single scroll lands somewhere else by the time
  // the shot is taken.
  std::string controlsScrollTo;
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

  // F12, and `--screenshot <path>`. Serviced in main.cpp between the UI's
  // submission and the present, which is the only moment the backbuffer both
  // holds this frame and is still readable. app/Screenshot.hpp says why the app
  // photographs itself rather than being photographed: every macOS route to
  // another process's window pixels is behind a permission that fails
  // *silently*, handing back the desktop with all windows stripped out.
  //
  // A request, not a call, for the same reason every other `request*` above is
  // one -- the key arrives during event handling, and the only legal place to
  // act on it is a specific point in the frame that has not happened yet.
  bool requestScreenshot = false;
  std::string screenshotPath = "naturalpaint-screenshot.png";

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

  // PLAN.md Phase 4 step 8 ("Document lifecycle", PRD I18). **This is where
  // the open document lives**, and it is the answer to the ownership question
  // io/ExportAs.hpp, io/TileResidency.hpp and every prior UI-facing step's
  // Findings row deferred to this step. `guides` and `opStack` above each
  // explain that they are conceptually document content living here only
  // because there was no Document to hang them on; that Document now exists,
  // and moving them onto it is Phase 5's work (guides need a per-document
  // home once tabs exist, and the op stack needs Phase 5 step 3's per-layer
  // ownership decision) rather than a rename this step can do safely.
  //
  // The rule this step sets for what belongs here, so the next dialog does
  // not have to re-decide it: **document and session state lives on AppState;
  // transient widget state does not.** A list of open documents, and the
  // recent-documents list below, are things main.cpp owns and a future
  // document-aware draw loop reads. A text buffer being typed into, a combo's
  // selected index, or which popup is currently open are none of those, and
  // putting them here would grow this struct by one member per dialog -- so
  // they stay function-local in ui/, exactly where ui/MacPaintUI.cpp's Add
  // Guide popup and Export As dialog already keep theirs.
  //
  // **Not wired, and stated plainly rather than implied:** the live painting
  // canvas is still not one of these documents. sim::PaintSim owns a single
  // dense texture with no layer awareness, so a stroke writes that texture
  // and touches no Layer::rgbTiles. See app/DocumentLifecycle.hpp's own
  // section on the gap.
  DocumentSession documents;

  // PRD I18's "open recent", persisted (see app/DocumentLifecycle.hpp for the
  // file and its location). Empty and untouched until the File menu is first
  // opened -- `recentDocumentsLoaded` is what makes that lazy, for the same
  // reason io/ExportAs' presets are: a file nobody asked for costs nothing
  // (PRD A2, ADR-0001), and --selftest's idle-RSS measurement would notice.
  RecentDocuments recentDocuments;
  bool recentDocumentsLoaded = false;

  // PLAN.md Phase 4 step 9 (app/Journal, ADR-0008, PRD O5-O10). The recovery
  // journal for this run: one scratch directory, its lock, and one journal
  // entry per dirty open document. Session state by app/AppState.hpp's own
  // rule above -- it belongs to the process, not to a widget -- and it holds
  // no buffers at rest (ADR-0008's note about the idle-RSS assertion), only a
  // path, a file descriptor and a small map keyed by DocumentId.
  //
  // Begun by main() after `recovery` below has been filled, and ended by
  // main()'s clean shutdown, which removes the directory. Never begun on the
  // --selftest path, which returns before this struct is constructed, so the
  // idle-RSS measurement cannot see it.
  JournalSession journal;

  // Unclean scratch directories found at launch, newest first (PRD O8:
  // "offered for recovery on launch, named and dated"). Filled once, before
  // the journal's own session exists, so this list can never contain it. The
  // UI offers them; nothing here opens or deletes anything on its own.
  std::vector<RecoverySession> recovery;
  // Cleared once the offer has been shown, so declining is not re-asked every
  // frame. File > Recover Documents... sets it again.
  bool recoveryOfferPending = false;

  // Menu-toggleable, no keyboard shortcut (docs/shortcuts.md §3 assigns
  // rulers to Cmd+R, but keymaps/default.json already binds Cmd+R to
  // reload_shaders from earlier Phase-1 work -- see main.cpp's key-down
  // dispatch comment for the full reasoning). Rulers are only meaningful at
  // view.rotation == 0 -- see MacPaintUI.cpp's canvas block for how a
  // rotated view degrades the ruler strips rather than drawing nonsense.
  bool showRulers = false;
  // docs/ui.md section 2's NAVIGATOR, floating over the bottom-right of the
  // canvas. On by default, unlike the rulers: the design draws it, and unlike
  // a ruler strip it costs the paint area nothing -- it floats over the
  // surround, and hides itself when the canvas is too small to spare the
  // corner (ui/AtelierLayout's `atelierNavigatorRect`).
  bool showNavigator = true;
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
