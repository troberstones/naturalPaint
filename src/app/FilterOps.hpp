#pragma once

#include <cstdint>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "app/StrokeSession.hpp"
#include "ops/Blur.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Filters.hpp"

// app/FilterOps -- the wiring bridge for the Filter and Image menus
// (docs/reachability-audit.md C1: "~93 entry points... no UI path to any of
// them"), in the shape app/StrokeSession already set for a layer-writing
// tool: the engine (ops/Blur, ops/Filters, ops/DocumentTransform) stays pure
// and knows nothing of `AppState` or a menu; this file is the only place
// that decides which layer a Filter-menu op reaches, whether the selection
// bounds it, and how it becomes one `core::History` entry. `ui/MacPaintUI`'s
// dialogs call the functions below and draw the result; `--selftest`
// (app/selftest/FilterMenu.cpp) calls the same functions, so the two cannot
// disagree about what a menu item does -- which is the exact drift
// app/StrokeSession's own header warns a second copy of "can this tool paint
// this layer" would produce.
//
// ==========================================================================
// Two different questions, two different vocabularies
// ==========================================================================
//
// The Filter menu's four items (Gaussian Blur, Sharpen, Unsharp Mask, Add
// Noise) are **pixel ops on the active layer**, exactly the shape the paint
// bucket and the gradient already are: `app/StrokeSession.hpp` section 6's
// `PixelOpRefusal` already answers "which layer, and why not" for them, and
// this file reuses it rather than inventing a second refusal vocabulary --
// which is what the task brief asks for by name. `applyGaussianBlur()` and
// its three siblings below refuse for the identical three reasons the bucket
// does (`NoLayer`, `Locked`, `NoRgbStore`) and for the identical structural
// reason: `ops/Blur.hpp` and `ops/Filters.hpp` both work on `core::TileStore`
// -- an **RGB layer**'s Working-space RGBA, not a **Pigment layer**'s
// **Latent**s -- and ops/FloodFill's argument against filling a Pigment
// layer's latents with a straight colour applies here unchanged, because a
// filter is exactly such a fill in every texel it touches.
//
// **This is a real limitation, not only a convenient one, and CONTEXT.md
// says so from the domain side.** Its own worked example -- "a blur on a
// pigment layer... works directly on the Latents? Blur is a linear
// combination, so yes" -- means Gaussian Blur specifically is NOT
// structurally barred from a Pigment layer the way Sharpen, Unsharp Mask and
// Add Noise are (all three read a straight, shaper-domain difference, which
// CONTEXT.md's Relationships section requires "a bake to Working space
// first" for). What stops Gaussian Blur from reaching a Pigment layer today
// is narrower: `ops/Blur.hpp` has no `PigmentTileStore` overload, only a
// `TileStore` one. `ops/DocumentTransform.hpp` section 2 shows the shape such
// an overload would take -- pack `Latent * Mass` and `Mass` into two RGBA
// planes, run the existing kernel on each, divide back out -- but building
// that engine entry point is not this task's wiring job, and a menu item
// cannot honestly offer it before it exists. Left as a named follow-up
// rather than built speculatively or silently dropped.
//
// The Image menu's two items (Image Size, Canvas Size) are **document-level
// geometry** through `ops/DocumentTransform`, which moves *every* layer --
// including locked ones, including layers with no pixel storage at all --
// and that header's own section 5 argues at length for why a document-level
// op must not refuse on a lock the way a layer-level one does. So these two
// use `DocumentOpOutcome`, not `PixelOpRefusal`: the failure modes are "zero
// extent" and "the document itself is empty", not "wrong kind of layer".
//
// ==========================================================================
// Why the selection is honoured by COMPOSITING, not by a Selection* the
// engine takes
// ==========================================================================
//
// ops/Blur.hpp and ops/Filters.hpp take no `Selection` at all -- deliberately;
// they are the same "class B" spatial machinery a live preview and an export
// path would also want, and baking a selection into the kernel would make
// every one of those callers carry a selection they do not have. So the
// selection is applied **here**, once, by the same shape every one of the
// four ops uses:
//
//   1. Run the engine over the WHOLE CANVAS rectangle, not the selection's
//      bounding box and not the layer's content bounds. This is not a
//      simplification, it is the correct edge policy: ops/Blur.hpp's whole
//      "apron" section exists because clipping the gather early reintroduces
//      the tile-seam bug in a different shape, and a filter that stopped at
//      the layer's content box would clip its own apron at exactly the edge
//      a blur most needs to reach past. The one cost, named rather than
//      hidden: `blurTiles()` and friends allocate every tile across the
//      requested rectangle (their header makes no promise, unlike
//      ops/DocumentTransform's bridges, to drop tiles that come out
//      transparent), so a filter on a small painting inside a very large
//      canvas briefly resident-allocates tiles it will then discard most of
//      the content of. Acceptable for a first correct version; a
//      content-bounds-union-selection-bounds crop is the natural follow-up
//      and does not change this file's shape, only the rectangle it passes.
//   2. Blend the engine's output back into the layer's live tiles texel by
//      texel, weighted by `selectionCoverageAt()` -- 1.0 where there is no
//      selection at all (core/SelectionMask.hpp's own default), the coverage
//      fraction under a soft-edged marquee, and exactly 0.0 outside a hard
//      one. `compositeFilterResult()` is that loop, shared by all four ops
//      for the reason ops/Filters.hpp gives its own combine step: "two
//      implementations of the same integral is exactly how the two of them
//      drift".
//
// **Texels the selection excludes are never written, not written-then-
// reverted.** A texel with coverage 0.0 is `continue`d before either store is
// touched, so "outside the selection is bit-identical to before" is not an
// approximation the loop happens to satisfy -- it is what not calling
// `Tile::writePixel()` on that address means. `--selftest` asserts it as
// exact equality over the whole excluded region rather than a tolerance, for
// the same reason `runBucketRefusalTest()`'s section E does.
//
// **A tile is only unshared (copy-on-write) when it actually has something
// to write.** `compositeFilterResult()` scans a tile before calling
// `TileStoreOf::getOrCreate()` on it, so a tile the selection excludes
// entirely, or one the filter left bit-identical everywhere (an identity
// request -- sigma 0, amount 0, noise amount 0), costs neither an allocation
// nor a COW copy. That is also what makes "one history entry, and only when
// something changed" correct rather than merely asserted: `texelsChanged`
// counts real writes, and `end()`-shaped callers gate `recordEdit()` on it
// exactly as `fillThroughSelection()`'s caller does.
//
// ==========================================================================
// Why the original is copied before the engine runs
// ==========================================================================
//
// Each `applyX()` below takes `const TileStore original = *target->rgbTiles;`
// **before** calling the engine, and passes `original` (not
// `*target->rgbTiles` a second time) into `compositeFilterResult()` as the
// "what was there before" side of the blend. `TileStoreOf`'s copy
// constructor shares every tile via `shared_ptr` (an O(tiles) refcount
// bump, not a byte copy), so this is cheap -- and it is not optional. Passing
// the SAME store object as both the read side and the write side would still
// be correct per texel (each texel's old value is read before that texel is
// written, and nothing else touches it in between), but it makes the safety
// argument depend on that ordering discipline surviving every future edit to
// the loop. A genuine second `TileStoreOf` object, sharing tiles until
// `getOrCreate()`'s COW barrier forks one, makes the two sides independent by
// construction -- the same guarantee `core::History` leans on when it holds
// a `Document` snapshot next to the live one.
namespace np {

// Blends `filtered` into `target`, texel by texel, over `rect`, weighted by
// `selectionCoverageAt(selection, ...)` -- see this header's own section
// above. `original` is `target`'s content **before** the engine ran (a
// genuine separate `TileStoreOf`, not a second reference to `target`), and is
// what a partially-covered texel blends toward instead of, and what a
// coverage-0 or already-identical texel is compared against to decide
// whether writing it at all would be a no-op.
//
// Returns the number of texels actually written -- coverage > 0 AND the
// blended value differs from what `target` already held -- which is exactly
// the count `applyX()` gates `OpenDocument::recordEdit()` on, for
// `fillThroughSelection()`'s own reason: an edit that changed nothing must
// not create an undo step nobody can tell apart from the one before it.
size_t compositeFilterResult(const TileStore& original, const TileStore& filtered,
                             const PixelRect& rect, const Selection* selection,
                             TileStore& target);

// What one Filter-menu pixel op did. `refusal` is `PixelOpRefusal::None` on
// success; every other value means nothing was read from the engine at all
// and `texelsChanged` is 0. A refusal that reaches an `applyX()` call is the
// re-check `ui/MacPaintUI.cpp`'s gradient drag already performs on itself --
// the dialog's OK button is a second frame after the menu click that opened
// it, and nothing in this build can lock or retype a layer in between today,
// but the guard belongs where it is relied on rather than where it happened
// to be established (app/StrokeSession.hpp section 5 makes the identical
// argument about its own latched target).
struct FilterOpResult {
  PixelOpRefusal refusal = PixelOpRefusal::None;
  size_t texelsChanged = 0;
};

// PRD D4 (docs/reachability-audit.md C1): Gaussian blur, ops/Blur's own
// anchor filter. `sigma` is the dialog's own field, in document texels;
// non-finite or negative is refused by `blurParamsValid()` before the engine
// is asked to do anything, exactly as `ops/Blur.hpp` documents for every
// other caller.
FilterOpResult applyGaussianBlur(OpenDocument& doc, float sigma);

// PRD D5: the one-click filter -- `ops/Filters.hpp`'s `sharpenTiles()`, fixed
// at `kSharpenSigma`. `strength` is the unsharp amount; 0 is the identity.
FilterOpResult applySharpen(OpenDocument& doc, float strength);

// PRD D5: the full three-control filter -- amount, radius (via `params.blur`)
// and threshold, `ops/Filters.hpp`'s `unsharpMaskTiles()`. Deliberately takes
// the whole `UnsharpParams` rather than three scalars, so the dialog's own
// struct and the one the engine reads are the same object and cannot drift.
FilterOpResult applyUnsharpMask(OpenDocument& doc, const UnsharpParams& params);

// PRD D5: `ops/Filters.hpp`'s counter-based add-noise. Takes the whole
// `NoiseParams`, for the identical reason `applyUnsharpMask()` does.
FilterOpResult applyAddNoise(OpenDocument& doc, const NoiseParams& params);

// ==========================================================================
// Live preview (docs/testing-issues.md T15)
// ==========================================================================
//
// `previewX()` below computes EXACTLY what `applyX()` above would write into
// the active layer -- same engine call, same `compositeFilterResult()`
// selection blend -- but returns it in `*previewOut` instead of writing it
// anywhere. `doc` is `const&` and nothing it reaches is mutated: no
// `recordEdit()`, no write to `target->rgbTiles`, no history entry. That is
// the whole mechanism T15 asks for -- "compute into a scratch buffer... draw
// that instead of the layer... discard it on Cancel" -- and the reason each
// `previewX()` shares its engine call and its composite step with the
// matching `applyX()` (both route through this file's internal
// `computePixelFilter()`) is that a SECOND hand-written copy of "run the
// engine, blend through the selection" is exactly how a preview and a commit
// end up computing two different answers, which is this task's own sabotage
// (b): "the preview and the committed result use different parameters."
// Sharing the function makes that divergence a compile error away rather
// than a discipline away.
//
// `ui/MacPaintUI.cpp`'s dialogs are the only callers: on the frame a slider
// settles (released, not every intermediate frame of the drag -- see that
// file's `IsItemDeactivatedAfterEdit()` comment for the measured cost that
// throttle exists to bound) or the dialog first opens, the dialog calls the
// matching `previewX()` and hands the result to the preview-overlay
// machinery near `addCanvasQuad()`'s document draw, which composites a
// whole-document picture with the active layer's tiles swapped for
// `*previewOut` and shows THAT instead of the real document texture until
// the dialog closes. See that file's `g_filterPreview` and
// `FilterPreviewTexture` for the overlay and PRD F3's budget note on why the
// composite is a synthetic document rather than a straight tile blit.
//
// `previewOut` is left untouched on any refusal or on an identity request
// (`texelsChanged == 0`) -- callers read `FilterOpResult` first, exactly as
// `applyX()`'s own callers already do, so "nothing to preview" and "preview
// computed" are never confused.
FilterOpResult previewGaussianBlur(const OpenDocument& doc, float sigma, TileStore* previewOut);
FilterOpResult previewSharpen(const OpenDocument& doc, float strength, TileStore* previewOut);
FilterOpResult previewUnsharpMask(const OpenDocument& doc, const UnsharpParams& params,
                                  TileStore* previewOut);
FilterOpResult previewAddNoise(const OpenDocument& doc, const NoiseParams& params,
                               TileStore* previewOut);

// What one Image-menu document op did. `error` is `ops/DocumentTransform`'s
// own message (naming the extent or the layer count that refused it) and is
// empty exactly when `ok` is true.
struct DocumentOpOutcome {
  bool ok = false;
  std::string error;
};

// PRD D17: Image Size -- resample every layer to a new extent, through
// `ops/DocumentTransform::resizeDocumentImage()`. `kernel` governs RGB tiles,
// layer masks and selections; the latent kernel a Pigment layer's resample
// takes is left at `DocumentTransformParams`'s own default (Bilinear) and is
// not exposed here, because ops/DocumentTransform.hpp section 2(b) makes it a
// closed, lobe-free enum for a reason no dialog control should be able to
// reopen -- see that header before adding one.
//
// A request that leaves the extent unchanged records no history entry, the
// same "no-op the user asked for is not an edit" rule `resizeImage()` itself
// states; detected by comparing `DocumentTransformResult::previousWidth`/
// `previousHeight` against the request rather than trusting `ok`, because
// `ok` is true for a no-op too.
DocumentOpOutcome applyImageSize(OpenDocument& doc, uint32_t width, uint32_t height,
                                 ResampleKernel kernel);

// PRD D17: Canvas Size -- change the extent, do not touch the pixels, through
// `ops/DocumentTransform::resizeDocumentCanvas()`. Same no-op rule as
// `applyImageSize()`.
DocumentOpOutcome applyCanvasSize(OpenDocument& doc, uint32_t width, uint32_t height,
                                  CanvasAnchor anchor);

}  // namespace np
