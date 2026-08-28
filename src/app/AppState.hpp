#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "brush/Dynamics.hpp"
#include "brush/Library.hpp"
#include "app/BrushLibraryFile.hpp"
#include "app/CloseDecision.hpp"
#include "app/PanelLayout.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/DocumentPresets.hpp"
#include "app/Journal.hpp"
#include "app/QuitSequence.hpp"
#include "app/SelectionDrag.hpp"
#include "app/StrokeBake.hpp"
#include "app/TransformSession.hpp"
#include "app/UserBrushLibrary.hpp"
#include "core/Clipboard.hpp"
#include "core/SelectionBoundary.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionShapes.hpp"
#include "brush/StrokePath.hpp"
#include "core/OpStack.hpp"
#include "core/Probe.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// docs/ui.md section 2's tool palette has ~26 cells (27 counting Water and
// DryBrush, which the wireframe never drew -- see below), and this enum has
// one value per cell so the palette, app/StrokeSession's routing table and
// the --selftest tripwires all agree on a single canonical list rather than
// three that can quietly drift apart.
//
// **Only the first seven -- Brush through Zoom -- do anything.** They are
// what this build's canvas, StrokeSession and strokeRouteFor() have real
// behaviour for. The other twenty exist purely so the palette can show a
// name, an icon and a keyboard-shortcut slot for a tool that is not built
// yet: ui/MacPaintUI.cpp's palette draws every one of them visibly disabled
// (dimmed, unclickable, tooltip says so), and app/StrokeSession.cpp's
// strokeRouteFor() routes them to StrokeRoute::None. **`Eraser` has left that
// list** -- PRD F9/F10 are P0, and it now routes to StrokeRoute::RgbErase on a
// writable RGB layer, to StrokeRoute::PigmentErase on a writable Pigment one,
// and refuses by name on every other kind (ADR-0007, and
// app/StrokeSession.hpp §1's Eraser rows). Each earns real
// behaviour on its own PRD id and phase, per docs/ui.md section 4's table --
// which is also where MEASURE and SLICE's earlier "Dropped" disposition is
// reversed: the palette keeps them for now, and per the user's own words,
// "we'll prune the unneeded tools in the future as the capabilities settle
// in." That table's own callout says a disposition changed without a
// reason recorded is how GRAD, FILL and MEASURE went missing from the PRD
// for a whole revision before -- this comment is the reason, recorded.
//
// Water and DryBrush predate this palette (this build's two watercolour
// brush variants) and are not in the wireframe's ~26 at all; they sit in
// group 3 beside Brush in the palette's display order because that is where
// a user reaching for "the wet one" or "the dry one" would look for them --
// beside the brush they are variants of, not filed among tools with no
// relationship to painting at all.
enum class Tool {
  // --- the seven with real behaviour --------------------------------------
  Brush,       // water + pigment
  Water,       // pre-wet the paper, no pigment
  DryBrush,    // little water, hard edge, pigment sits on the tooth
  Eyedropper,
  Marquee,     // PRD E3's rectangle
  // PRD E3's ellipse. A separate Tool value rather than a mode on Marquee,
  // because docs/shortcuts.md section 1 reserves `M` for "Marquee -- rectangle
  // | ellipse": the two are flyout siblings, and a flyout member IS a Tool
  // value in ui/AtelierChrome's kToolGroups. A mode flag would need its own
  // parallel routing everywhere Tool is switched on, for no gain.
  EllipseMarquee,
  Hand,
  Zoom,
  // --- docs/ui.md section 2's remaining palette cells: name/icon/slot only,
  // see this enum's own comment above for what "only" means here -----------
  Move,
  Lasso,
  PolygonLasso,
  MagicWand,
  Crop,
  Measure,
  Frame,
  CloneStamp,
  Eraser,
  PaintBucket,
  Gradient,
  Pencil,
  Smudge,
  Dodge,
  Burn,
  Pen,
  Curve,
  Text,
  Shape,
  Slice,
  Count
};

// PRD **L4** (P0), docs/ui.md §3.3: "The colour panel has RGB and PIGMENT
// modes; PIGMENT selects physical constants, not just a colour."
//
// Which of the two the foreground colour is currently *being said in*. It is
// not a panel preference: it decides what `foregroundSrgb()` returns and
// therefore what every brush, bucket and gradient in the build actually lays
// down, so it belongs beside the colour it selects between rather than as a
// file-static in the panel that draws it -- which is exactly where it used to
// live (`ui/MacPaintUI.cpp`'s `g_colorPigmentMode`, beside a
// `float g_colorRgb[3]` that **no file in `src/` ever read**; the panel said so
// out loud: "Not yet connected: no tool reads this colour").
enum class ColorMode {
  // The foreground is `BrushState::pigment`, a row of `defaultPalette()`:
  // a colour **and** three physical constants (density, staining,
  // granulation). The default, and what a Pigment or Media layer wants.
  Pigment,
  // The foreground is `BrushState::rgb`, an arbitrary triple with no physical
  // constants behind it. What the RGB picker sets, and what the eyedropper
  // must set -- three floats sampled off a canvas cannot say how a paint
  // settles or lifts, and inventing values for that would be a lie the user
  // could not detect.
  Rgb,
};
// **The one range per `BrushState` field, read by every widget that edits
// it.** Reachability audit B3: the options bar (`ui/AtelierChrome.cpp`'s
// SIZE/HARD/LOAD/WET row) and the BRUSH panel (`ui/MacPaintUI.cpp`'s
// Radius/Hardness/Load/Water sliders) edit the same four `BrushState`
// fields, and `AtelierChrome.cpp`'s own comment where LOAD is drawn states
// the rule: "one field behind two widgets with two ranges is two clamps,
// and the narrower one silently truncates what the other set." SIZE broke
// it -- 2..90 in the bar, 1..200 in the panel, so a value the panel accepted
// (say, 150) silently clamped to 90 the instant the bar's own widget next
// touched the field -- because its two literals were never unified into one
// symbol for that rule to keep in sync. LOAD, WET and HARD happened to
// carry matching literals at both sites already, which is a coincidence a
// second author changing one file at a time is not obliged to preserve.
// These eight constants are the fix, generalised: **one named range per
// field, and both files read it** -- not because the other three were
// broken, but because "happened to match" is exactly the state SIZE was
// once in too, and the fix that only protects the field that already broke
// protects nothing going forward.
//
// **Radius: 1..200, not a guess.** `brush/Deposit.hpp`'s roundness-floor
// derivation (:225-230) names 200 px explicitly as "the widest radius the
// UI offers" and reasons about the resulting minor-axis pixel width from
// it -- the deposit engine already assumes this ceiling is real, so
// narrowing the options bar's widget below it was the bug, not the panel's
// wider range being wrong. The floor: `Deposit.hpp:410`'s comment on
// `BrushTip::radius` says "a radius of 0 or less deposits nothing at all"
// -- 0 is provably useless, so 1 is the smallest value strictly above it
// that still reads as a whole pixel on the `"%.0f px"` format both widgets
// share. (The options bar's old floor of 2 had no comment anywhere in this
// file's history explaining it -- git blame finds no derivation, the same
// "guessed, not measured" failure this project's other magic numbers are
// held to.)
//
// **Hardness, Load, Wetness: unchanged from what both files already
// agreed** -- 0..1, 0..2.5 and 0..3 respectively. Not re-derived here
// because neither site disagreed and nothing in `brush/Deposit.hpp`,
// `sim::PaintSim` or the PRD names a different ceiling for any of the
// three; unifying them is closing the same class of bug B3 was, before a
// second author's independent edit reopens it on a field that has not
// broken yet.
constexpr float kBrushRadiusMin = 1.0f;
constexpr float kBrushRadiusMax = 200.0f;
constexpr float kBrushHardnessMin = 0.0f;
constexpr float kBrushHardnessMax = 1.0f;
constexpr float kBrushLoadMin = 0.0f;
constexpr float kBrushLoadMax = 2.5f;
constexpr float kBrushWetnessMin = 0.0f;
constexpr float kBrushWetnessMax = 3.0f;

struct BrushState {
  Tool tool = Tool::Brush;
  int pigment = 6;  // Ultramarine Blue

  // Which of the two things above the foreground colour currently is.
  ColorMode colorMode = ColorMode::Pigment;

  // The arbitrary foreground colour, in **display-referred sRGB**, the same
  // encoding `paint::Pigment::rgb` is in -- deliberately, so that
  // `foregroundSrgb()` can return either one and every consumer downstream
  // does exactly one thing with the result.
  //
  // **The encoding is the part that is easy to get wrong and impossible to
  // see.** This build's working space is linear-light (CONTEXT.md's "Working
  // space", DESIGN-imaging.md §2) and a document's texels are linear; a
  // swatch, an ImGui colour picker and `MixboxLut::rgbToLatent()`'s API are
  // all sRGB. Storing linear here would make the swatch and the picker both
  // wrong; storing sRGB and forgetting the decode makes every stroke land
  // about twice as dark as the colour that was chosen -- and both failures
  // read as "colour management is broken somewhere else" rather than as a
  // missing one-line conversion. sRGB is the choice because it matches the
  // palette, so there is one rule for both halves of the union rather than
  // two: *the foreground is sRGB, and whoever hands it to the document
  // decodes.* `ui/MacPaintUI`'s `foregroundLinearRgba()` and
  // `brushTipFor()`'s `tip.linearRgb` are the two places that decode, and
  // `--selftest` asserts they agree.
  //
  // The initial value is `ui/MacPaintUI.cpp`'s retired `g_colorRgb` default,
  // carried over unchanged so the RGB picker opens where it always did.
  std::array<float, 3> rgb = {0.10f, 0.12f, 0.45f};

  float radius = 20.0f;
  float load = 0.9f;      // pigment concentration
  float wetness = 1.3f;   // water deposited
  float hardness = 0.35f;
  // Minor/major axis ratio of an elliptical tip; 1.0 is round. The design's
  // TIP section (turn 4a) shows it beside radius, hardness and spacing, and
  // DynamicTarget::Roundness drives it.
  float roundness = 1.0f;
  // Tip rotation in degrees, the axis `roundness` is measured against -- a
  // round tip does not care, an elliptical one does. DynamicTarget::Angle
  // drives it, and Photoshop's `Angl` imports straight into it.
  float angle = 0.0f;

  // A `.abr` sampled bitmap tip (brush/Deposit.hpp §2c), or null for the
  // procedural round/elliptical tip every other field above already
  // describes. Moves only in lockstep with a whole preset -- set by
  // `applyPresetToBrush()`, read by `presetFromBrush()` and by
  // `brushTipFor()` -- because there is no slider that swaps a bitmap on its
  // own, the same reason `brush/Library.hpp`'s `presetMatches()` gives for
  // not comparing it separately.
  std::shared_ptr<const BrushTipBitmap> tipBitmap;

  // A Dual Brush's second tip (brush/Deposit.hpp §2d), or null for every tip
  // that has none. Moves only in lockstep with a whole preset -- set by
  // `applyPresetToBrush()`, read by `presetFromBrush()` and by
  // `brushTipFor()` -- for the identical reason `tipBitmap` above gives.
  std::shared_ptr<const BrushTip> dualTip;
  // Only meaningful when `dualTip` is set.
  DualBrushBlend dualBlend = DualBrushBlend::Multiply;

  // Every input-drives-parameter relationship this brush has (brush/Dynamics).
  //
  // **This replaced `bool pressureSize` / `bool pressureFlow`, which were
  // already two links with their ranges written in as literals** -- both
  // routes applied exactly `0.25 + 0.75p` and `0.15 + 0.85p`, which is
  // `linkContribution()` at [0.25,1] and [0.15,1]. `defaultBrushLinks()`
  // reproduces them identically, so nothing about how a pen feels changed
  // when the booleans went away; what changed is that the relationship is now
  // sayable for the other seven sources and ten targets too.
  BrushLinkSet links = defaultBrushLinks();

  // Mirrors `brush/Library.hpp`'s `BrushPreset::scatterBothAxes` -- see that
  // field's own comment. No slider writes this yet; it moves only with
  // `applyPresetToBrush()`/`presetFromBrush()`, alongside `tipBitmap`.
  bool scatterBothAxes = false;

  // Which cell of the DYNAMICS matrix the LINK editor below it is showing.
  //
  // UI state on BrushState rather than on AppState proper because it is
  // scoped to the brush being edited: switching brushes should not leave the
  // editor pointed at a cell the new brush has nothing in. It names a CELL,
  // not a link -- the editor opens on an empty cell too, which is how a link
  // gets created (the design has no separate "new link" dialog; +LINK just
  // opens the editor on the first empty cell).
  DynamicSource editSource = DynamicSource::Pressure;
  DynamicTarget editTarget = DynamicTarget::Size;

  // The brush library this brush was picked from (brush/Library.hpp).
  //
  // It lives on BrushState rather than beside it because `active` only means
  // anything relative to the live brush -- the EDITED badge is exactly "the
  // fields above no longer match `brushLibrary.presets[active]`", and the two
  // halves of that comparison drifting into different structs is how a badge
  // starts lying.
  BrushLibrary brushLibrary = defaultBrushLibrary();
  // Arc-length dab spacing (CONTEXT.md "Dab", ADR-0003), in units of the
  // current brush radius: a dab emits every `spacing * radius` px of travel.
  // 0.25 is the conventional middle ground among painting apps (most sit in
  // roughly a 0.1-0.3 range) between visibly discrete stamps (spacing too
  // large) and dab-count/overdraw cost (spacing too small). No UI control
  // yet -- exposing it is a later phase's job; this is just the constant
  // default.
  float spacing = 0.25f;

  // **The ceiling one stroke can reach on an RGB layer**, in [0,1] --
  // deliberately NOT the same quantity as `load` above, which is how much a
  // single dab lays down. brush/RgbDeposit.hpp §2 carries the whole argument;
  // the short form is that at the default 0.25-radius spacing every dab
  // overlaps its neighbours about four deep, so a brush that applied its
  // opacity per dab would have no setting at all that produces a flat 50 %
  // pass.
  //
  // 1.0 rather than a lower default because a brush that does not reach the
  // colour it is loaded with is the surprising one, and because it makes this
  // field invisible to every existing behaviour until someone moves it. **No UI
  // control yet** -- exposing it is a later step's job, exactly as `spacing`
  // above still says of itself; this is the constant default and the place the
  // DYNAMICS matrix would multiply if a `DynamicTarget::Opacity` is ever added
  // (there is none today, which is why `brushTipFor()` copies this straight
  // through rather than scaling it).
  //
  // Read only by the RGB deposit route. The pigment route caps at `kMaxMass`,
  // a property of the paper rather than of the stroke.
  float opacity = 1.0f;

  // Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp) -- OFF by default,
  // `GrainParams`'s own default. The BRUSH EDITOR's PAPER GRAIN section
  // (`ui/MacPaintUI.cpp`'s `drawBrushSection()`) is the one control surface
  // that writes this; `brushTipFor()` copies it straight into the tip it
  // builds, unscaled by any DYNAMICS target -- there is no
  // `DynamicTarget::Grain` for the identical reason there is no
  // `DynamicTarget::Opacity` (`opacity`'s own comment above).
  GrainParams grain;
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

// Which **Image > Adjustments** command has been asked for and not yet
// serviced (`AppState::requestAdjustment` below carries the reason this is one
// field rather than one bool per command).
//
// Every enumerator is a menu item in `ui/MenuModel`'s Adjustments submenu and
// a function in `app/AdjustmentOps.hpp`. The split between "opens a modal" and
// "acts immediately" is NOT encoded here: it is `ui/MacPaintUI.cpp`'s
// adjustment block that knows which is which, for the same reason
// `MenuEffect` lives on the menu model rather than on the action -- the
// question "does this need to ask the user something" is about the interface,
// not about the operation.
enum class AdjustmentRequest {
  None,
  Levels,
  Curves,
  Exposure,
  ChannelMixer,
  Desaturate,
  BrightnessContrast,
  HueSaturation,
  Vibrance,
  ColorBalance,
  BlackAndWhite,
  PhotoFilter,
  Invert,
  Posterize,
  Threshold,
  GradientMap,
  AutoTone,
  AutoContrast,
  AutoColor,
  Equalize,
};

struct AppState {
  PaintMode mode = PaintMode::Watercolor;
  // The stroke bridge's per-frame cycle. It lives here rather than as a local
  // in main()'s frame loop because two callers need the same one: main.cpp
  // steps it every frame (above drawUI() -- see app/StrokeBake.hpp section 1),
  // and ui/MacPaintUI forces it to settle before moving the history cursor
  // (section 4). Two cycles would each keep their own drying episode and
  // fight over one readback slot.
  StrokeBakeCycle bakeCycle;
  // Seconds a wash keeps moving before it sets. Drives evaporation and
  // absorption together via setWorkingTime(); 15 matches the shipped defaults.
  float workingTime = 15.0f;
  BrushState brush;
  CanvasView view;
  SimParams sim;

  // PRD **Q10** (P0): "Eyedropper picks into the foreground colour, with
  // sample size, sample-all-layers, and Option-click while painting."
  //
  // The two parameters of a pick, and nothing else -- `core::ProbeParams`'s
  // third field, `activeLayerIndex`, is not a user setting: it is whichever
  // layer is active at the moment of the click, read off the OpenDocument.
  //
  // **These live on `AppState` and are surfaced in exactly one place**, the
  // options bar (`ui/AtelierChrome.cpp`). `ui/AtelierChrome.cpp`'s LOAD slider
  // states the house rule they are obeying: "one field behind two widgets with
  // two ranges is two clamps, and the narrower one silently truncates what the
  // other set." SIZE already breaks it (options bar 2-90 against the BRUSH
  // panel's 1-200) and this is deliberately not a second breakage -- there is
  // one widget per field, so there is nothing for a second range to disagree
  // with. If either ever appears in a second panel, both widgets must offer
  // `core::kProbeSampleSizes` and `ProbeSource` entire, not a subset.
  //
  // Session state, surviving a tool switch, the way `brush.tool` itself does.
  // **Not persisted to disk**, and that is a decision rather than an omission:
  // `app/BrushLibraryFile.cpp` writes a preferences file and this could have
  // joined it, but that file's format freezes positional keys forever, and
  // freezing a key for a two-field tool that has existed for one phase buys a
  // convenience (an 11x11 sample surviving a restart) at the price of a format
  // commitment. Photoshop does persist these; when a preferences file exists
  // that is not the brush-library file, they should go in it.
  struct EyedropperState {
    // Edge length -- `core::kProbeSampleSizes`' set, Photoshop's own.
    int32_t sampleSize = 1;
    // Which layer(s) a pick reads (`core::ProbeSource`).
    //
    // `CurrentLayer` is the default, matching `ProbeParams`'s own default and
    // Photoshop's. Not `AllLayers`: a painter's first pick is nearly always
    // "the colour I just put down", and a default that silently composited the
    // adjustment layers above it would hand back a colour the layer does not
    // contain and cannot reproduce.
    ProbeSource source = ProbeSource::CurrentLayer;
  };
  EyedropperState eyedropper;

  // What the last eyedropper click did, in one sentence, or empty when there
  // has not been one. Shown in the options bar.
  //
  // It exists because of the one case a swatch alone cannot explain: picking
  // while the COLOR panel is in PIGMENT mode **switches the panel to RGB
  // mode**, and a mode that changed under the user without a word is the same
  // class of defect as a tool that does nothing. `app/StrokeSession`'s
  // refusal strings are the precedent -- a gesture whose effect is not the
  // obvious one says so in the band, in the same voice.
  std::string lastPickReport;

  // Photoshop-style tool-group memory (docs/ui.md section 2, "nest similar
  // tools into a flyout"): which member of each palette group slot the
  // user most recently picked, so a flyout cell shows the tool a painter
  // actually reached for last time rather than resetting to the group's
  // fixed default the moment `brush.tool` moves to an unrelated tool.
  // Indexed by a group's position in ui/AtelierChrome.hpp's `kToolGroups`,
  // not by `Tool` -- there is one slot of memory per *group*, which is the
  // whole point of a flyout.
  //
  // A `Tool` per group, not a bool or an index into `brush.tool` itself,
  // because most of a group's members are not `toolImplemented()`
  // (this file's own `Tool` comment): picking one from a flyout usually
  // does NOT move `brush.tool` at all (the same "no-op if unimplemented"
  // rule every disabled palette cell already follows), yet the cell still
  // has to keep showing what was picked -- `brush.tool` alone cannot
  // represent that.
  //
  // Empty until the palette's first frame, not sized here: this header
  // cannot know ui/AtelierChrome.hpp's group count or table without a
  // circular include (that header already includes this one for `Tool`),
  // so ui/MacPaintUI.cpp's `toolGroupButton()` resizes and fills it, once,
  // the first time it draws -- the same lazy-init shape
  // `recentDocumentsLoaded` below uses for a different reason. This is
  // session state by this struct's own rule further down (survives
  // switching tools and reopening a flyout, the way `brush.tool` itself
  // does), not the transient widget state that rule keeps off this struct
  // -- "which popup is open" would be the latter; "which tool a group's
  // flyout last landed on" is closer to `brush.tool` itself.
  std::vector<Tool> toolGroupCurrent;

  // --flyout-demo: holds the Brush group's flyout open (ui/MacPaintUI.cpp's
  // `toolGroupButton()`, matched by `toolGroupIndex(Tool::Brush)` rather
  // than a hardcoded slot index, so a reordering of `kToolGroups` cannot
  // silently point this at the wrong group) so `--screenshot` can
  // photograph it -- `openLayerMenu`'s justification exactly, one popup
  // over: a flyout opens on right-click or a ~350ms press-and-hold, and
  // the screenshot path has neither.
  bool openToolFlyoutDemo = false;

  // --panel-stack-demo: forms two tab stacks in the right dock the first frame
  // the panel layout is read -- HISTOGRAM and GRADE as tabs beside COLOR, and
  // COMPS as a tab beside the collapsed HISTORY -- so `--screenshot` can
  // photograph an open tab strip and a collapsed one.
  //
  // Same justification as `openToolFlyoutDemo` one field up: a stack is formed
  // by dragging one panel onto another, and the screenshot path has no drag.
  // A feature that can only be verified by a human is one that silently rots,
  // and the tab strip is the newest chrome in this build with the least
  // coverage -- the previous revision shipped a clipped glyph on the flyout
  // rail for exactly that reason.
  //
  // **Applied to the in-memory layout only, and never saved.** It runs where
  // the lazy `loadFromFile()` runs (ui/MacPaintUI.cpp's `drawUI`), after it, so
  // a demo run cannot rewrite the user's real `panel-layout.txt`.
  bool panelStackDemo = false;

  // --- Selection and clipboard commands, consumed in ui/MacPaintUI ---------
  //
  // Request flags rather than direct action, for the reason the zoom commands
  // already are: the key event arrives in main.cpp's SDL loop, and acting on a
  // selection needs the active OpenDocument and the sim, which are drawUI()'s
  // to reach. One place decides what a command means.
  bool requestSelectAll = false;
  bool requestDeselect = false;
  bool requestReselect = false;
  bool requestInvertSelection = false;
  bool requestCopy = false;
  bool requestCopyMerged = false;
  bool requestCut = false;
  bool requestPaste = false;
  bool requestDeleteSelection = false;

  // Undo / redo (PRD O1, R2). Same request-flag reasoning as the block
  // above -- ⌘Z/⇧⌘Z resolve in main.cpp's keymap dispatch, which has no
  // OpenDocument, no sim and no GpuContext to settle wet paint against
  // before moving the history cursor (ui/MacPaintUI.cpp's
  // `moveHistoryCursor()`, which is also what the title-bar buttons and the
  // Edit menu's Undo/Redo call -- one function, four callers, never two
  // undo paths). `--selftest`'s reachability suite
  // (app/selftest/MenuBasics.cpp) asserts the menu action and the keymap
  // action both end up flipping the SAME flag.
  bool requestUndo = false;
  bool requestRedo = false;

  // The application's one clipboard (PRD M5). Holds copy-on-write tile
  // references, so an idle clipboard after a full-document copy costs
  // refcounts rather than the 68 MB a flattened 4K buffer would -- which is
  // why it can simply live here rather than needing a purge command.
  Clipboard clipboard;

  // --- Marquee drag, in document texel space ------------------------------
  //
  // Live only between mouse-down and mouse-up on the canvas with Tool::Marquee
  // selected. The selection itself lives on the OpenDocument (it is per
  // document, not per app); this is just the rubber band.
  bool marqueeDragging = false;
  float marqueeX0 = 0.0f, marqueeY0 = 0.0f;
  float marqueeX1 = 0.0f, marqueeY1 = 0.0f;

  // How this drag will combine with the selection already installed (PRD E7).
  //
  // **Latched at mouse-down and not re-read during the drag**, which is the
  // convention every editor uses and is worth stating because the obvious
  // implementation -- reading the modifiers at mouse-up, where the selection
  // is actually built -- is wrong in a way that is easy to miss. Shift is also
  // the constrain-to-square modifier, so a user who presses it mid-drag is
  // asking for a square, not asking to convert a Replace into an Add. Sampling
  // at mouse-up would conflate the two gestures; sampling at mouse-down keeps
  // "which boolean" and "what shape" as independent questions.
  SelectionCombine marqueeCombine = SelectionCombine::Replace;

  // T10 (docs/testing-issues.md): Space-move state for the in-progress
  // rectangle/ellipse marquee drag. Reset to a fresh `SelectionMoveState{}`
  // whenever a new marquee drag starts -- see app/SelectionDrag.hpp's own
  // doc comment for why an offset from a previous drag has no meaning here.
  SelectionMoveState marqueeMove;

  // T10's other two gestures (Shift-constrain, Option-from-centre) need no
  // stored state of their own -- unlike Space-move they are pure functions
  // of *this frame's* live modifier read and are therefore reapplied fresh
  // every frame by app/SelectionDrag.hpp's computeSelectionDragBox(), which
  // is exactly what lets Option be toggled mid-drag in both directions
  // without the anchor (`marqueeX0/Y0` above) ever being rewritten.
  //
  // The live box (unclamped, same as `marqueeX0..Y1` above always were) for
  // the current frame of a rectangle/ellipse marquee drag, computed once per
  // frame in ui/MacPaintUI.cpp and consulted by both the live rubber-band
  // draw and (at mouse-up, where it is clamped to the canvas same as
  // before) the actual commit, so the two can never disagree about what
  // shape is being drawn.
  float marqueeBoxX0 = 0.0f, marqueeBoxY0 = 0.0f;
  float marqueeBoxX1 = 0.0f, marqueeBoxY1 = 0.0f;

  // The lasso's path, in document texel space, shared by both lasso tools
  // (PRD E3) because core/SelectionShapes rasterises them with one function --
  // a freehand lasso is a polygon with a vertex per pointer sample and a
  // polygon lasso is one with a vertex per click, and nothing downstream needs
  // to know which produced it.
  //
  // Freehand points are only appended when the pointer has actually MOVED at
  // least a texel. A stationary pointer at 120 Hz would otherwise pile up
  // thousands of coincident vertices, and every one of them becomes a
  // zero-length edge the rasteriser walks.
  std::vector<SelectionPoint> lassoPoints;

  // Polygon lasso only: the gesture spans many clicks, so unlike every other
  // selection tool it is not bounded by one mouse-down/up pair and needs a
  // flag of its own to say "a path is open".
  bool polygonLassoActive = false;

  // The active document's selection BOUNDARY, for PRD E6's marching ants.
  // Recomputed only when the selection changes -- extraction walks every texel
  // of every occupied tile (~6 ms for a full-canvas selection on a 2048x2048
  // document, measured in `--selftest`), which is not something to do 120 times
  // a second inside PRD F3's 20 ms frame.
  //
  // **This replaced a cached `SelectionBounds`.** The bounding box was exact
  // while `selectRectangle()` was the only constructor, and became a lie the
  // moment PRD E3's lasso, polygon lasso and wand landed: every selection drew
  // as a rectangle. core/SelectionBoundary.hpp carries that argument.
  //
  // Keyed **inside the object**, on `(DocumentId, selectionRevision)`, which is
  // why there is no companion pair of cached-key members beside it: the caller
  // hands it the key and it decides whether to re-extract.
  SelectionBoundaryCache selectionBoundary;

  // The key the GPU coverage upload is gated on: `PaintSim::setSelection()`
  // uploads a canvas-sized texture and must not do it per frame.
  //
  // Keyed on the document id as well as the revision, and that is not
  // belt-and-braces: revisions start at 0 per document, so two open tabs sit
  // at the same revision most of the time. Keying on the revision alone would
  // leave one tab's paint gated by the other tab's selection whenever the two
  // numbers happened to agree, which is the common case rather than the rare
  // one. `SelectionBoundaryCache` keys itself the same way, for the same
  // reason.
  DocumentId cachedSelectionDoc = 0;
  uint64_t cachedSelectionRevision = 0;

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

  // The other three hardware axes, already converted to the normalised [0,1]
  // form brush/Dynamics.hpp's sources take (app/PenAxes.hpp does the polar
  // conversion; SDL reports tilt as two independent angles, not as a lean and
  // a direction).
  //
  // **Their defaults are the values a mouse produces, and the DYNAMICS matrix
  // reads them literally.** An upright pen has no azimuth, so 0 there is the
  // honest rendering of "no information" rather than a stale sample -- which
  // is also what the panel's live gutter shows on a machine with no tablet.
  // Barrel rests at 0.5 because its range is signed about the rest position.
  float penTilt = 0.0f;
  float penAzimuth = 0.0f;
  float penBarrel = 0.5f;

  // Raw SDL tilt in degrees, kept only because the two axes arrive in
  // separate SDL_EVENT_PEN_AXIS events and the polar conversion needs both at
  // once -- an x-tilt event alone cannot compute an azimuth.
  float penTiltXDeg = 0.0f;
  float penTiltYDeg = 0.0f;

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

  // **Free Transform** (Cmd+T / Edit > Free Transform), and the session it
  // starts. A request rather than a direct call for the same reason the view
  // commands above are: it needs to decide between transforming the whole
  // active layer and transforming just the pixels under a selection, which
  // needs the live document -- and both of its two entry points (the menu
  // action, and main.cpp's keymap dispatch) run outside any place that has
  // one. `ui/MacPaintUI.cpp`'s canvas block services it.
  //
  // A drop that imports exactly one picture ALSO starts a session, straight
  // from main.cpp's drop handler, so a dropped image is immediately movable
  // without reaching for the menu -- see app/OpenAnyFile.hpp's
  // `DropOutcome::transformableLayer`.
  bool requestFreeTransform = false;

  // **Image > Adjustments** (app/AdjustmentOps.hpp): which adjustment the next
  // frame should service, and `None` the rest of the time. Serviced and
  // cleared by `ui/MacPaintUI.cpp`'s adjustment block, which either opens that
  // command's modal or -- for the ones Photoshop gives no dialog -- performs
  // it there and then.
  //
  // **One enum in `AppState`, rather than the file-static `g_xRequested` bools
  // the Filter menu's seven items use.** Not a stylistic preference: five of
  // these commands carry a keyboard chord (Cmd+L Levels, Cmd+M Curves, and so
  // on), and a chord is resolved in `main.cpp`, which cannot see a file-static
  // in another translation unit. The Filter items got away with the older
  // shape only because not one of them has a chord. Putting the request here
  // means the menu item and the chord set the SAME field rather than two
  // fields that could drift about what "Levels" means -- the identical
  // argument `requestFreeTransform` above makes for Cmd+T, and the reason
  // that flag is not a `g_freeTransformRequested` either.
  //
  // A single field rather than one bool per command, because these are
  // mutually exclusive by construction (a modal is open or it is not) and
  // because this list grows to every item in `docs/operations.md` §1.2 --
  // nineteen bools that may only ever have one set is a state space with
  // nothing enforcing its own invariant.
  AdjustmentRequest requestAdjustment = AdjustmentRequest::None;

  // The one in-progress transform. Empty (`!active()`) whenever no gesture is
  // running; `commit()` and `cancel()` both return it to that state, and
  // nothing is written to the document until `commit()`, so an abandoned
  // session costs nothing and leaves nothing to unwind.
  TransformSession transform;

  // **The frame loop stops when this is true, and nothing else.** It is not a
  // request and never has been: `--screenshot` sets it directly as its
  // capture-and-exit mechanism, and tools/golden/run_golden.sh depends on that
  // exit happening on the frame it was asked for, with nothing in the way.
  bool quit = false;

  // **A user asking to leave**, which is a different thing (app/QuitSequence).
  // Set by `SDL_EVENT_QUIT`, by the window's close button, by the keymap's
  // "quit" action and by File > Quit; serviced once per frame in main.cpp,
  // where it either exits at once (no dirty documents) or starts asking about
  // each dirty one in turn.
  //
  // Separate from `quit` above deliberately and structurally. A dirty-document
  // check bolted onto `quit` would put a modal in front of `--screenshot`'s own
  // exit and hang the golden harness forever; keeping the flags apart means
  // there is no expression through which the screenshot path can reach the
  // guard. app/QuitSequence.hpp carries the full argument.
  bool requestQuit = false;

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

  // The Save / Don't Save / Cancel question raised by a close, and not yet
  // answered (app/CloseDecision.hpp). Zero-initialised -- "nothing pending" --
  // for all but a handful of frames in a session.
  //
  // Session state by this header's own rule above, and it has to be: it names
  // a *document* by id rather than describing a widget, it outlives the frame
  // the click happened on, and it is raised from ui/AtelierChrome.cpp's tab
  // strip while being answered by ui/MacPaintUI.cpp's dialog. A function-local
  // static in either file would be invisible to the other half of the feature,
  // which is how the tab strip and the File menu would end up with two
  // different answers to the same question.
  PendingClose pendingClose;

  // The documents a quit has still to ask about (app/QuitSequence.hpp). Empty
  // and `running == false` for every frame of a normal session -- a quit is in
  // flight only between the keystroke and the last answer.
  //
  // Session state for the same reason `pendingClose` is, and it has to be: the
  // sequence is started from main.cpp's event loop and advanced from
  // ui/MacPaintUI.cpp's Save / Don't Save / Cancel dialog, so a function-local
  // static in either would leave the other half of the feature unable to see
  // it.
  QuitSequence quitSequence;

  // PRD I18's "open recent", persisted (see app/DocumentLifecycle.hpp for the
  // file and its location). Empty and untouched until the File menu is first
  // opened -- `recentDocumentsLoaded` is what makes that lazy, for the same
  // reason io/ExportAs' presets are: a file nobody asked for costs nothing
  // (PRD A2, ADR-0001), and --selftest's idle-RSS measurement would notice.
  RecentDocuments recentDocuments;
  bool recentDocumentsLoaded = false;

  // T12: where every panel is docked, how big it is and whether it is collapsed,
  // flown out or put away, as the user arranged it (app/PanelLayout.hpp for the
  // model, the file, and the repair rules a file from another build goes
  // through -- including a version 1 file written by the single-column build
  // this replaced).
  //
  // Lazy exactly as `recentDocuments` above is, and for the same reason -- but
  // note this one is read on the first frame that draws the chrome, which in
  // a windowed session is frame 1. The laziness is therefore worth almost
  // nothing at runtime and everything to `--selftest`, whose headless path
  // never draws a panel and so must never touch a preferences file.
  PanelLayout panels;
  bool panelsLoaded = false;

  // Which flyout panel is open, if any -- session state, deliberately NOT
  // persisted. A flyout is a transient view over the canvas, and restoring one
  // open on relaunch would present a covered canvas as the application's
  // resting state. `Count`-style sentinel is not available on
  // `ControlsSection`, so the pair is a bool and a value rather than an
  // optional; `flyoutOpen` is the one that decides.
  bool flyoutOpen = false;
  ControlsSection flyoutSection = ControlsSection::Color;

  // A splitter drag in progress: which dock, and the index of the splitter
  // within it. Session state for the same reason -- a drag cannot survive a
  // relaunch because the mouse button cannot.
  bool splitterDragging = false;
  PanelPlacement splitterDock = PanelPlacement::Right;
  size_t splitterIndex = 0;
  // A dock-edge drag in progress (resizing the whole dock rather than one
  // slot inside it). Kept apart from the splitter above because the two have
  // different bounds and different write-backs, and one bool doing both jobs
  // is how a drag ends up resizing the wrong thing.
  bool dockEdgeDragging = false;
  PanelPlacement dockEdgeDock = PanelPlacement::Right;

  // A panel being TORN OFF: dragged by its grip towards another dock, the
  // flyout rail, or back where it came from. Session state like the two drags
  // above, and for the same reason -- a drag cannot survive a relaunch because
  // the mouse button cannot.
  //
  // The user's report is what this exists for: *"I don't see handles to tear
  // off any of the panels like tool settings or the tool bar on the left."*
  // Moving a panel used to be a menu action only, which is not what tearing
  // off means to anyone who has used a docking UI.
  //
  // `panelDragActive` also suppresses the grip's click-to-collapse for the
  // press that became a drag -- Dear ImGui's button reports a press as a click
  // on release however far the pointer travelled in between, so without this a
  // completed tear-off would ALSO toggle the panel shut on the way out.
  bool panelDragActive = false;
  ControlsSection panelDragSection = ControlsSection::Color;

  // T9: the user's saved File > New sizes (app/DocumentPresets.hpp). Lazy
  // exactly as `recentDocuments`/`panels` above, and for the same
  // reason -- read the first time the New Document dialog opens, not at
  // startup, so `--selftest`'s headless run (which never opens that dialog)
  // never touches `document-presets.txt`.
  DocumentPresetStore documentPresets;
  bool documentPresetsLoaded = false;

  // PRD G6/G7's imported `.abr` brush libraries, remembered across launches
  // (app/BrushLibraryFile.hpp for the file, the row cache and the lazy read).
  //
  // Here rather than on `BrushState` beside `brushLibrary`, even though the
  // two are operated on together: this is persistence and session state -- a
  // file path, a handful of cached rows and two counters -- and it belongs
  // with `recentDocuments` above, which it is the same kind of thing as. The
  // coupling to `brush.brushLibrary` is carried in every signature that needs
  // it (`importFile`, `useLibrary`, `unload` all take a `BrushLibrary&`)
  // rather than by nesting, which is also what lets --selftest exercise the
  // whole feature with no `AppState` anywhere near it.
  //
  // Empty and untouched until the BRUSH LIBRARY pane is first drawn --
  // `brushLibrariesLoaded` is what makes that lazy, exactly as
  // `recentDocumentsLoaded` does above. **Reading the preferences file is not
  // reading a `.abr`**: it is a few hundred bytes of text plus one `stat()`
  // per remembered library, and no brush pack is opened until a brush from one
  // is picked.
  BrushLibraryStore brushLibraries;
  bool brushLibrariesLoaded = false;

  // PRD G6's OTHER half: the presets the user made, saved so a tuned brush
  // is still there after a quit (app/UserBrushLibrary.hpp; reachability-
  // audit.md's A7). A sibling of `brushLibraries` above, not a member of it
  // -- app/UserBrushLibrary.hpp's own header, §0, is the argument for why
  // this is a second file rather than the `.abr` registry extended: the two
  // change at different rates and lose different things when a write is
  // interrupted, and coupling them means a bug in one blocks the other.
  //
  // Empty and untouched until first needed, exactly like `brushLibraries`
  // above -- `userBrushLibraryLoaded` is the same lazy gate, guarding the
  // same idle-RSS promise (PRD A2, ADR-0001).
  UserBrushLibraryStore userBrushLibrary;
  bool userBrushLibraryLoaded = false;

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

  // docs/reachability-audit.md F2: true for the whole life of a `--screenshot
  // <path> [frames]` run, set once in main() right after this struct is
  // constructed. MacPaintUI's title bar reads it to freeze the live fps
  // readout at a fixed string instead of the real, run-varying number --
  // the same reasoning as main.cpp's `(-FLT_MAX, -FLT_MAX)` mouse
  // suppression on screenshot frames: a live number is a nondeterminism
  // source a golden view could never hold a threshold against, since it is
  // a different string run to run rather than stable glyph-edge noise.
  bool screenshotCliActive = false;
};

}  // namespace np
