#pragma once

#include "app/DocumentLifecycle.hpp"
#include "app/FilterOps.hpp"
#include "ops/PointOps.hpp"

// app/AdjustmentOps -- the wiring bridge for the **Image > Adjustments** menu,
// in the shape `app/FilterOps` already set for the Filter menu: the engine
// (`ops/PointOps`, `ops/PointOpTiles`) stays pure and knows nothing of an
// `AppState` or a menu; this file is the only place that decides which layer
// an adjustment reaches, whether the selection bounds it, and how it becomes
// one `core::History` entry. `ui/MacPaintUI.cpp`'s dialogs call the functions
// below and draw the result; `--selftest` (app/selftest/AdjustmentMenu.cpp)
// calls the same functions, so the two cannot disagree about what a menu item
// does.
//
// ==========================================================================
// Destructive, on purpose -- and the non-destructive path already exists
// ==========================================================================
//
// This codebase already has a complete NON-destructive grading path:
// `core::OpStack` on every layer, an `Adjustment` layer kind that composites
// (core/Composite.cpp), `io/OpSerial` persistence, a GPU LUT bake
// (color/LutBake) and the GRADE panel's stack editor. Six of the functions
// below wrap the exact same `ops/PointOps` maths that path evaluates. So the
// obvious question is why a second, destructive route exists at all.
//
// **Because `core::OpStack` cannot honour a selection, and never will.** It is
// evaluated per texel as a colour transform over a whole layer -- that is
// precisely what makes it collapsible into one 3-D LUT (ADR-0004) and
// therefore what makes a stack of twelve adjustments cost what one costs.
// Teaching it "except inside this marquee" would mean carrying a
// `core::Selection` into the LUT bake, which is the one thing a LUT cannot
// express: a LUT maps colour to colour, and a selection maps *position* to
// coverage. `docs/operations.md` §1.3 makes the identical argument about why
// a vignette is not class A -- "a function of position, so it cannot be a
// per-pixel LUT".
//
// Photoshop's own Image > Adjustments menu is destructive and selection-bounded
// for the same structural reason, and its Adjustment Layers are the
// non-destructive whole-layer alternative. This file is the first half; the
// `Adjustment` layer kind is the second. Neither replaces the other, and a
// user who wants an adjustment they can re-edit later reaches for the layer.
//
// ==========================================================================
// Everything else is app/FilterOps' argument, unchanged
// ==========================================================================
//
// The refusal vocabulary, the whole-canvas rectangle, the composite-through-
// the-selection blend, the copy-on-write tile discipline, the one-history-
// entry-and-only-when-something-changed rule, and the preview/commit sharing
// are all `app/FilterOps.hpp`'s -- read that header, because none of it is
// re-argued here. What makes the sharing literal rather than aspirational is
// `app/PixelOpBridge.hpp`: every function below calls the SAME
// `applyPixelFilter()`/`computePixelFilter()` templates the seven Filter-menu
// items call, with `ops/PointOpTiles`' `pointOpTiles` as the engine.
//
// Two consequences of that sharing worth stating explicitly, because they are
// the questions a reader will have:
//
//  - **An adjustment refuses on a Pigment layer**, with `PixelOpRefusal::
//    NoRgbStore`, exactly as a filter does. A Pigment layer holds `Latent`s,
//    not Working-space RGBA, and `ops/FloodFill`'s argument against filling
//    latents with a straight colour applies unchanged to a point op, which is
//    such a fill in every texel it touches. This is a real limitation, not
//    merely a convenient one; it closes when a `PigmentTileStore` route
//    exists, not before, and a menu item cannot honestly offer what does not
//    exist.
//
//  - **An adjustment never grows the layer.** `ops/PointOpTiles.hpp`'s header
//    explains why: a texel with no coverage un-premultiplies to
//    `{0,0,0,0}` and comes back unchanged, so empty canvas stays empty even
//    under an Invert or a Levels with a raised black output. That differs
//    from the Filter menu, where a blur legitimately spreads paint outward,
//    and it is the behaviour Photoshop's own adjustments have.
namespace np {

// ------------------------------------------------------------- the commands
//
// One function per menu item. Each builds its own one-element `PointOpRun` and
// passes its own `editLabel`, rather than this file exposing a single
// "apply an arbitrary run" entry point, for `app/FilterOps.hpp`'s own stated
// reason: the callers each name their own op and their own params type, so
// there is exactly one place two commands could silently start sharing a
// parameter, and it is not this one. The label is what the History panel
// shows, so "levels" and "exposure" have to be distinguishable there.

// Image > Adjustments > Levels... (Cmd+L). `channels[0..2]` are R, G, B's own
// independent `LevelsParams`; a *composite* Levels adjustment is the caller
// passing the same struct three times, which is `ops/PointOps.hpp`'s stated
// convention and not something this layer re-invents.
FilterOpResult applyLevelsAdjustment(OpenDocument& doc,
                                     const std::array<LevelsParams, 3>& channels);

// Image > Adjustments > Curves... (Cmd+M). Control points are in the
// **shaper** domain (ADR-0004) -- a format-level commitment, not a detail of
// this call. A channel with fewer than 2 points is the identity for that
// channel and skips the shaper round-trip entirely.
FilterOpResult applyCurvesAdjustment(OpenDocument& doc, const std::array<Curve, 3>& channels);

// Image > Adjustments > Exposure... Stops; a pure multiply in linear light,
// deliberately NOT the shaper domain.
FilterOpResult applyExposureAdjustment(OpenDocument& doc, const ExposureParams& params);

// Image > Adjustments > Channel Mixer... The 3x4 matrix including offsets.
FilterOpResult applyChannelMixerAdjustment(OpenDocument& doc, const ChannelMixerParams& params);

// Image > Adjustments > Desaturate (Shift+Cmd+U). **The one command here with
// no dialog and therefore no preview twin**, matching Photoshop: it takes no
// parameters a user could set, so there is nothing to preview and nothing to
// cancel. It runs `applyGrayscale()` at its default Rec.709 weights -- the
// same weights `shaders/grayscale_blit.wgsl` hardcodes for the GPU preview
// pass, so a desaturated layer and a grayscale *view* of it agree
// numerically.
FilterOpResult applyDesaturate(OpenDocument& doc);

// ------------------------------------------------------------- live preview
//
// Each computes EXACTLY what its `apply` twin would write -- same engine, same
// selection blend, same params construction -- and returns it in
// `*previewOut` instead of writing it anywhere. See `app/FilterOps.hpp`'s own
// `previewX()` section: sharing `computePixelFilter()` is what makes "the
// preview and the commit compute the same answer" a compile error away rather
// than a discipline away.
//
// `previewOut` is left untouched on any refusal or on an identity request
// (`texelsChanged == 0`), so "nothing to preview" and "preview computed" are
// never confused.
//
// **Cost note, measured rather than assumed** -- see
// `app/selftest/AdjustmentMenu.cpp`'s timing section. A point op is far
// cheaper than the spatial filters `app/FilterOps` previews (T15 measures
// Gaussian blur at ~275 ms at 1024x1024), because there is no apron, no
// gather, and no tile the source did not already have. The dialogs still
// recompute on `IsItemDeactivatedAfterEdit()` rather than every frame of a
// drag, for consistency with the Filter dialogs beside them.
FilterOpResult previewLevelsAdjustment(const OpenDocument& doc,
                                       const std::array<LevelsParams, 3>& channels,
                                       TileStore* previewOut);
FilterOpResult previewCurvesAdjustment(const OpenDocument& doc,
                                       const std::array<Curve, 3>& channels,
                                       TileStore* previewOut);
FilterOpResult previewExposureAdjustment(const OpenDocument& doc, const ExposureParams& params,
                                         TileStore* previewOut);
FilterOpResult previewChannelMixerAdjustment(const OpenDocument& doc,
                                             const ChannelMixerParams& params,
                                             TileStore* previewOut);

}  // namespace np
