#pragma once

#include <cstdint>
#include <string>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerGeometry.hpp"
#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/SelectionMask.hpp"
#include "core/TileStore.hpp"
#include "ops/Transform.hpp"

// ops/DocumentTransform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14,
// D15, D16, D17, and E10 for the selection).
//
// ops/Transform.hpp is the resampler. It is complete, and it deliberately stops
// at a flat `TransformImage` and a `TileStore`, with its own section 5 saying
// so: it stayed out of `core/Document` and `core/Layer` to stay out of another
// track's files. **This file is that entry point.** Nothing here reads a source
// texel through a kernel; every resample in this file goes through
// `transformImage()`, exactly once per stored value, and the two extra tile
// shapes (`MaskTile`, `PigmentTile`) reach it by being *packed into* that same
// RGBA resampler rather than by growing a second one.
//
// ==========================================================================
// (1) A LAYER HAS NO OFFSET. THAT IS THE WHOLE OF WHY CROP IS DANGEROUS.
// ==========================================================================
//
// core/LayerGeometry.hpp §1 states the fact this file is built on and
// core/LayerComp.hpp found first: **`core::Layer` has no offset, origin or
// transform field of any kind**, and every tile store -- `rgbTiles`,
// `pigmentTiles` and `mask` -- is keyed in **absolute document coordinates**
// (core/TileStore.hpp).
//
// So "move the layer offsets with the crop" is not a field update. There is no
// field. The layer's offset *is* its tile keying, and the only way to move it
// is to move the keys -- which is exactly `core::translatedTileStore()`, the
// whole-tile re-key or the sub-tile gather, at zero loss.
//
// That makes the classic crop bug structural rather than careless, and worth
// spelling out because it is invisible in a screenshot of a single layer:
//
//   `doc.width = w; doc.height = h;` and nothing else **is a crop that moved
//   the canvas and left every pixel behind.** Layer 0's content is now at
//   document coordinate (x, y) when the user asked for it at (0, 0); every
//   layer is wrong by the same amount, so a *composite* still looks internally
//   consistent -- it is just the wrong part of the picture, shifted. Nothing
//   about the image is misaligned against anything else, which is why this
//   passes a casual look.
//
//   The version that is worse still, and the one this file's tests exist to
//   catch: cropping the **pixels** by re-keying `rgbTiles` and forgetting
//   `mask`. Now the layer's content has moved and its coverage has not, so a
//   masked layer's mask slides off the content it was painted for. That one
//   *is* visible -- and it is visible as "the mask is wrong", a long way from
//   the crop that caused it. core/LayerGeometry.hpp's `translateLayer()`
//   already moves the mask for exactly this reason ("a move that left the mask
//   behind would slide content out from under its own coverage"); every
//   document-level op here inherits that rule rather than restating it, and
//   `--selftest` asserts the mask landed with the pixels rather than assuming
//   the inheritance held.
//
// **The selection is the third store and it does not live on `Document`.** A
// `core::Selection` sits on `app::OpenDocument`, outside this file's reach and
// outside `core/History`'s snapshot. Rather than crop a document and silently
// leave a marquee pointing at the wrong pixels, every document-level entry
// point below takes a `Selection*` **by parameter**, with `nullptr` meaning
// "this caller has none". A caller cannot forget a store it has to pass. That
// is DESIGN-imaging.md §3's own instruction for selections -- "reserve the seam
// ... or it becomes precisely the pervasive retrofit ADR-0001 warns about" --
// applied to geometry instead of to deposition.
//
// ==========================================================================
// (2) PIGMENT LAYERS: TRANSFORMED, MASS-WEIGHTED, AND ONLY THROUGH A KERNEL
//     WITH NO NEGATIVE LOBES
// ==========================================================================
//
// core/LayerGeometry.hpp deferred this and ops/Transform.hpp §5 refused to
// decide it by defaulting. It is decided here, and it is decided **for**
// transforming rather than against, in three parts. All three are needed; any
// one of them alone is a wrong answer that looks right.
//
// **(a) Resampling latents is legitimate, and DESIGN-imaging.md says so by
// name.** §3's table has two columns, "valid on latents" and "must bake to RGB
// first", and the word `resample` is in the first one, beside `blur`, `offset`,
// `advection` and `diffusion`. The rule generating that table is stated one
// line above it: *"Mixbox guarantees that linear combinations of latents are KM
// mixes. So any op that is a linear combination of pixels stays valid in latent
// space, and any op that is not, is not."* A resample is a linear combination
// of pixels. So refusing pigment layers outright would refuse an operation the
// design document explicitly permits -- and would refuse it on the **default
// layer kind** (`Layer::kind` defaults to `Pigment`), which would make "Image
// Size" fail on a document made of nothing but default layers. That is not
// conservatism, it is a broken editor.
//
// **(b) But only a POSITIVE-WEIGHT kernel, and the type system enforces it, not
// a runtime check.** The guarantee in (a) is about *linear combinations*, and
// what makes a linear combination of latents a KM mix rather than nonsense is
// that it stays inside the **convex hull** of the latents going in. Catmull-Rom,
// Mitchell and Lanczos3 have negative lobes -- that is the whole reason they are
// sharper -- so their output is an *affine* combination with some weights below
// zero, which extrapolates **outside** that hull. On RGB that overshoot is a
// ringing artefact you can see and argue about. On a latent triple it is a
// different thing entirely: `c3 = 1 - (c0+c1+c2)` is implied, never stored
// (core/Pigment.hpp), so an overshoot in `c0..c2` drives the *fourth,
// unrepresented* pigment weight to an arbitrary value, and `latentToRgb()` --
// a 20-term cubic, then clamped to [0,1] as a reflectance -- turns that into a
// colour with no relationship to any pigment that was in the neighbourhood.
// Ringing on RGB overshoots a colour; ringing on a latent invents a pigment.
//
// **Measured, because "it rings" is not a number.** A hard edge between two
// pigment mixtures whose weights sum differently (0.97 against 0.30, so the
// implied `c3` steps from 0.03 to 0.70) upscaled 2.7x, asking of the output not
// "does it overshoot" but "where do the pigment weights end up":
//
//     kernel        min kernel weight   worst c_i below 0   worst c3 below 0
//     nearest            +0.000000           +0.0000            +0.0000
//     bilinear           +0.000000           +0.0000            +0.0000
//     Catmull-Rom        -0.074074           -0.0438            -0.0133
//     Mitchell           -0.036282           -0.0041            +0.0000
//     Lanczos3           -0.147267           -0.0753            -0.0372
//
// A `c_i` below zero is a **negative concentration of a pigment**, which is not
// a mixture that was slightly overshot, it is not a mixture. The two lobe-free
// kernels measure exactly `+0.0000` on both columns -- not "small", zero -- and
// that is the property the enum encodes.
//
// (Note the trap in that table: two mixtures with *equal* weight sums would have
// shown `c3` unharmed by every kernel, because a linear filter preserves a
// constant sum exactly however hard `c0` and `c2` ring against each other. The
// measurement had to be built to avoid flattering the answer.)
//
// So `LatentKernel` below is a **separate, closed enum with two values**,
// `Nearest` and `Bilinear`, and there is no way to spell `Lanczos3` at a pigment
// entry point. Rejected: a runtime refusal (`if (kernelHasNegativeLobes)
// return false`), which is a strictly weaker version of the same rule and
// leaves the wrong call *expressible*; and rejected harder, silently
// substituting bilinear when the caller asked for Catmull-Rom, which is
// precisely the deciding-by-defaulting ops/Transform.hpp declined to do. The
// conversion `latentKernelFor()` exists for the one caller that legitimately
// has a `ResampleKernel` in hand -- a document-level Image Size with a mixed
// stack -- and it **refuses by name** rather than rounding down.
//
// **(c) And the resample must be MASS-WEIGHTED, or (a) and (b) are both
// wasted.** This is the part with no precedent in the design document and it is
// the one that would have been got wrong by default. core/Pigment.hpp stores a
// texel as a **straight** latent plus a `mass` that is the alpha analogue --
// they are *not* premultiplied, unlike core::Tile's RGBA. A texel with mass 0
// therefore holds an arbitrary latent: nothing wrote it, nothing can recover it,
// and it is the exact pigment-space twin of the transparent-black texel whose
// RGB ops/Transform.hpp §2 refuses to average. Resampling `c0..c2` straight
// weights that meaningless triple as heavily as a texel full of paint, and the
// result is a coloured fringe around every stroke, in a hue that was never on
// the palette.
//
// So this file premultiplies by mass, resamples, and divides back out:
//
//     out.latent = sum(w_i * m_i * z_i) / sum(w_i * m_i)     out.mass = sum(w_i * m_i)
//
// With `w_i >= 0` (from (b)) and `m_i >= 0`, that quotient is a **convex**
// combination of exactly the latents that had paint in them -- which is the
// literal statement of Mixbox's guarantee, and it is also the physically right
// answer: mixing paint mixes in proportion to how much paint there is. Where
// `sum(w_i * m_i)` is zero there was no paint in the footprint, and the output
// is the default texel (mass 0, zero latent) rather than a divide by ~0.
//
// **How it reaches the resampler, and why that is not a second resampler.** A
// `PigmentTexel` is 7 channels; `transformImage()` takes 4. It is packed into
// **two** RGBA images that share an alpha channel:
//
//     A = (c0*m, c1*m, c2*m, m)          B = (res0*m, res1*m, res2*m, m)
//
// Each goes through `transformImage()` once. The premultiplied-alpha discipline
// that file is built on then does the work of (c) for free and unchanged --
// including the un-premultiply/area-average/re-premultiply bracket around the
// downscale prefilter, which integrates the *premultiplied* values and so is
// mass-weighted too. Nothing about the kernels, the prefilter, the exact paths
// or the edge policy is reimplemented here.
//
// The cost, named rather than absorbed: **8 resampled channels for 7 stored
// ones.** `m` is carried in both images and resampled twice. That is one wasted
// channel, 14% over the minimum, and it buys a pigment path with zero lines of
// its own kernel code. In bytes, for a whole 4096x2160 canvas: an RGB layer's
// single image is 141.6 MB in flight, a pigment layer's pair 283.1 MB. PRD A5's
// objection to invisible allocations applies and ops/Transform.hpp's advice
// stands unchanged -- pass the content bounds, not the canvas, which is what
// `transformLayer()` below does. The two copies of `m` are computed by the same code from
// the same input and are bit-identical -- `--selftest` asserts that with
// `memcmp` rather than assuming it, because if they ever diverge the divide in
// (c) is being done by two different denominators and the residual will drift
// away from the pigment weights.
//
// **Does that count as two resamples under PRD D16?** No, and the distinction
// is the same one D16 itself rests on. D16 forbids convolving the *same value*
// twice. A and B hold **disjoint** channels; every stored value is convolved
// exactly once. `LayerTransformResult::reconstructionPasses` is therefore a
// **max over stores**, not a sum, and it reads 1 for a pigment layer.
//
// ==========================================================================
// (3) MASKS AND SELECTIONS TRANSFORM IN THE SPACE WHERE "OUTSIDE" IS ZERO
// ==========================================================================
//
// Both are one channel of coverage, so both pack into the RGBA resampler the
// obvious way -- coverage in all four channels, read any one back. What is not
// obvious, and what would silently destroy a document, is **which value means
// "there is nothing here"**, because the two stores answer it oppositely and
// `transformImage()` has exactly one answer of its own.
//
// `transformImage()`'s edge policy is fixed and correct for pixels: a
// destination texel whose source position falls outside the source image comes
// back **transparent black**, i.e. zero. A rotation's corners are zero.
//
//   **A selection agrees.** core/SelectionMask.hpp: an absent tile is coverage
//   **0.0**, "outside the selection". A rotated selection's corners are
//   unselected, which is what zero already means. So a selection is packed
//   verbatim -- `(c, c, c, c)` -- and the resampler's own default is the right
//   one with nothing added.
//
//   **A layer mask is the exact inverse.** core/Mask.hpp: an absent mask tile
//   is **1.0**, reveal, "the identity of the multiply a mask feeds", and that
//   header designs the alternative out by name -- had the default been 0, "a
//   mask painted on one tile of a four-tile layer would blank the other three
//   ... precisely the 'discovered by the user as a black layer' failure".
//   Packing a mask verbatim would reintroduce that failure through the back
//   door: every corner a rotation opens up would come back 0, and rotating a
//   masked layer by one degree would hide everything outside its old bounding
//   box.
//
// So a mask is transformed in **hide space**, `h = 1 - coverage`, and unpacked
// as `coverage = 1 - h`. Zero-outside then *means* reveal-outside, and the
// resampler needs no flag, no fill colour and no second edge policy. The
// substitution is affine, so it commutes with every kernel here and preserves
// exactly the linearity §2(b) is about; and it is **bit-exact on the exact
// path**, because `1 - (1 - c)` is computed in float32 where a binary16 `c` and
// its complement are both exactly representable. Measured over **all 65 536
// half words**: zero of them fail to round-trip bit-identically, worst decoded
// coverage error 0.000e+00. `--selftest` asserts that at zero tolerance rather
// than at a threshold.
//
// **The size of the failure the change of variable avoids, measured.** A
// 40x40 fully-revealed mask region rotated 30 degrees, packed the naive way
// (coverage verbatim): **1 649 of the 3 249 destination texels come back with
// coverage below 0.5, the worst at 0.0000.** Half the destination region of a
// one-step rotation, hidden. Packed in hide space, every one of them is 1.0.
//
// core/LayerGeometry.hpp §2 reached the same conclusion for the *translate*
// with a different mechanism -- it default-constructs the destination tile, so
// a `MaskTile` arriving from outside the occupied set is 1.0 rather than 0.0,
// "zeroing it would have turned every mask translate into a partial erase".
// Same rule, same reason, one abstraction level up.
//
// ==========================================================================
// (4) WHAT IS EXACT AND WHAT RESAMPLES, AT DOCUMENT LEVEL
// ==========================================================================
//
// PRD D17's three operations do not cost the same thing and the difference is
// the point of separating them:
//
//   **Crop** -- zero resamples, bit-exact on the stored half words. It is a
//   change of extent plus an integer translate of every store, and the
//   translate is `core::translatedTileStore()`, which moves raw `uint16_t` and
//   never decodes one (core/LayerGeometry.hpp §2). A crop cannot lose a bit,
//   and this file routes it so that it structurally cannot rather than
//   measuring that it did not.
//
//   **Canvas size** -- the same operation with the origin an anchor implies.
//   One index-copy path, for `ops/Transform.hpp`'s own reason: "routing them
//   through the matrix path would have been tidy and would have made a crop
//   lossy for no reason at all".
//
//   **Image size** -- resamples, once, through the matrix path, so a downscale
//   prefilters by the same machinery a rotate does.
//
//   **Rotate/flip canvas** -- resamples for an arbitrary angle and is **exact**
//   for a flip or a quarter turn (PRD D15), because the matrix reaches
//   `exactRemapKind()` unchanged and takes the no-arithmetic path there.
//
// ==========================================================================
// (5) LOCKED LAYERS: A PER-LAYER TRANSFORM REFUSES ONE, A DOCUMENT-LEVEL ONE
//     DOES NOT
// ==========================================================================
//
// `transformLayer()` refuses a locked layer, with the numbers, exactly as
// `core::translateLayer()` does: moving a layer's pixels is the most content-y
// edit there is, and `locked`'s stated scope (core/LayerOps.hpp: "a locked
// layer's content and its own place in the stack are frozen") covers it.
//
// Every document-level entry point below moves and resamples **locked layers
// too**, and that is a decision rather than an oversight. A crop is not an edit
// *of a layer*, it is a change to the pixel grid every layer is expressed in.
// The two alternatives are both worse and one of them is much worse:
//
//   * **Refuse the crop if any layer is locked.** A document with one locked
//     background becomes uncroppable, unresizable and unrotatable. Users lock
//     backgrounds; that is what the feature is for.
//   * **Crop the unlocked layers and leave the locked ones.** Now the locked
//     layer is the only one that did *not* move, so it is misregistered against
//     every other layer by exactly the crop offset -- and a lock, which exists
//     to protect content, has silently destroyed the document's alignment. This
//     is the failure mode of §1's bug, deliberately introduced by a safety
//     feature.
//
// The count is reported rather than hidden: `DocumentTransformResult::
// lockedLayersMoved` says how many, so a UI can tell the user what the grid
// change did to layers they had frozen.
//
// ==========================================================================
// (6) WHAT IS NOT HERE
// ==========================================================================
//
//   - **Undo.** `core::History` snapshots a whole `Document` by value, so these
//     are undoable by the machinery that already exists; nothing here pushes an
//     entry, for `core/LayerOps`' reason -- the op reports an `editLabel` and
//     the caller records it.
//   - **The interactive transform tool** (PRD D14's handles). ui/ is another
//     track's and nothing here touches it.
//   - **Trim to content / auto-crop.** `layerContentBounds()` and
//     `unionLayerBounds()` already give the rectangle; the op is three lines on
//     top of `cropDocument()` and is a menu item, not machinery.
//   - **`Layer::ops` and `Layer::parent` under a transform.** An op stack is a
//     grade, not geometry, and survives untouched; a parent link names a part,
//     not a position. Neither is adjusted and neither needs to be.
//   - **Boundary coverage antialiasing.** Inherited verbatim from
//     ops/Transform.hpp §5: a full-bleed layer rotated 30 degrees gets a hard
//     rectangle edge. Not approximated here either.
//   - **Media/Strokes/Text/Flats layers.** They hold no pixels, so a transform
//     is a no-op on them, and this file says so rather than refusing -- a
//     document-level crop that failed because the stack contained a Text
//     placeholder would be refusing on a technicality.
namespace np {

// --------------------------------------------------------------------------
// A rectangle of document pixels as an origin plus an extent, half-open:
// `[x, x + width) x [y, y + height)`.
//
// **Half-open, where `core::LayerBounds` is inclusive**, and that is not a
// gratuitous second rectangle type. `LayerBounds` is inclusive because every
// producer of one is a scan that finds a last occupied pixel
// (core/LayerGeometry.hpp). Every consumer *here* is `cropImage()` or
// `transformImage()`, whose interface is an origin and an unsigned extent. The
// conversion happens exactly once, in `regionFromBounds()`, which is precisely
// core/LayerGeometry.hpp's own argument for keeping the two conventions apart:
// "the two conversions between the conventions are precisely where an
// off-by-one in an alignment hides".
// --------------------------------------------------------------------------
struct DocumentRegion {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool empty() const noexcept { return width == 0u || height == 0u; }
  friend bool operator==(const DocumentRegion&, const DocumentRegion&) = default;
};

// The half-open region an inclusive `LayerBounds` names. An empty box gives an
// empty region rather than a 1x1 one at the origin -- the same distinction
// `LayerBounds::empty` exists to keep.
DocumentRegion regionFromBounds(const LayerBounds& bounds) noexcept;

// The document canvas as a region: `[0, width) x [0, height)`.
DocumentRegion documentCanvasRegion(const Document& doc) noexcept;

// Where `src` lands under `dstFromSrc`, as the smallest integer region
// containing the transformed footprint, **outset by one pixel on every side**.
//
// The outset is a rounding margin and nothing more. `transformedBounds()` gives
// the exact footprint of the source rectangle's outer corners in float; a
// destination texel can only be written when its *centre* maps back inside the
// source, so the true written set is already inside that box, and the one-pixel
// ring covers the case where a corner lands a rounding error outside a floor or
// a ceil. It is deliberately **not** a kernel-radius outset: a tap that reaches
// outside the source contributes a clamped edge texel, not a new destination
// texel, so widening by the kernel's support would allocate a border of tiles
// that can only ever be transparent.
//
// Returns an empty region for an empty source, and for a transform that puts a
// corner on the horizon of a perspective matrix (`transformedBounds()` reports
// non-finite there, and an extent cannot be made from it).
DocumentRegion transformedRegion(const Mat3& dstFromSrc, const DocumentRegion& src) noexcept;

// --------------------------------------------------------------------------
// What counts as "there is something here", per store. **Four functions and not
// one template**, because the four rules genuinely differ and a shared one would
// make the coincidences look like a rule -- core/LayerGeometry.cpp makes the
// identical argument about alpha and mass both landing on channel 3.
//
//   RGB        alpha != 0            a premultiplied texel with zero alpha
//                                    contributes nothing through `over`.
//   Pigment    mass != 0             mass 0 projects to fully transparent.
//   Mask       word != reveal(1.0)   a mask's "nothing here" is 1.0, not 0, and
//                                    exactly-1.0 is io/NpaintFile's own drop
//                                    rule for a mask tile.
//   Selection  coverage != 0         0 is unselected, and a zero tile is
//                                    `selectsNothing()`.
//
// All four test the **stored word**, not a decoded float, so a denormal, an
// infinity or a NaN a file may carry counts as content instead of being
// silently dropped by a float comparison. Same rule and same reason as
// core/LayerGeometry.cpp's `halfWordIsNonZero()`.
// --------------------------------------------------------------------------
DocumentRegion rgbContentRegion(const TileStore& tiles);
DocumentRegion pigmentContentRegion(const PigmentTileStore& tiles);
DocumentRegion maskContentRegion(const MaskTileStore& tiles);
DocumentRegion selectionContentRegion(const Selection& selection);

// --------------------------------------------------------------------------
// The kernel a latent triple may pass through. Section 2(b) is the argument;
// this enum is the argument made unsayable-to-violate.
//
// Two values, both with **strictly non-negative weights over their whole
// support**, so every output latent is a convex combination of the input
// latents and therefore a Kubelka-Munk mixture of them:
//
//   Nearest    radius 0.5, one tap of weight 1. The output latent is *one of
//              the input latents*, unchanged -- the only kernel here that
//              cannot produce a pigment that was not already in the picture.
//              The right choice for a latent field that must not be blended at
//              all, and the wrong one for anything a user will look at.
//   Bilinear   radius 1, weights `1-|t|` on [-1, 1]: non-negative everywhere,
//              summing to exactly 1. The default, and the sharpest kernel that
//              is legitimate here at all.
//
// There is deliberately no `Mitchell`. Mitchell-Netravali at (1/3, 1/3) is the
// gentlest of the three cubics -- **measured minimum weight -0.036282**, against
// Catmull-Rom's -0.074074 and Lanczos3's -0.147267 -- and on the hard-edge test
// in §2(b) it drove a pigment weight only to -0.0041. But "only" is a tolerance
// argument about a quantity (the implied fourth pigment weight) whose scale
// nothing here bounds, and the two kernels above measure `+0.000000`, not
// "small". The line is drawn at zero because zero is a property that survives
// every input, and 0.036 is a judgement call that would have to be re-made
// against every new kernel and every new document.
// --------------------------------------------------------------------------
enum class LatentKernel { Nearest, Bilinear };

const char* latentKernelName(LatentKernel kernel) noexcept;

// The `ResampleKernel` that *is* this latent kernel. Total, by construction.
ResampleKernel resampleKernelFor(LatentKernel kernel) noexcept;

// True for the three kernels with negative lobes -- CatmullRom, Mitchell,
// Lanczos3. Exposed because "which kernels ring" is a fact about
// ops/Transform's kernels that a UI wants to say out loud beside a menu, and
// deriving it by trying values would be worse than naming it.
bool resampleKernelHasNegativeLobes(ResampleKernel kernel) noexcept;

// The narrowing conversion, for the one caller that legitimately holds a
// `ResampleKernel` chosen for pixels and needs to know whether it is admissible
// on latents: a document-level Image Size over a mixed stack.
//
// **Refuses rather than rounds down.** Returns false and writes a message
// naming the kernel and the reason for the three with negative lobes; a caller
// that wants pigment layers transformed then either picks a lobe-free kernel or
// says so to the user. Silently substituting bilinear here would put the
// decision back where ops/Transform.hpp §5 refused to leave it.
bool latentKernelFor(ResampleKernel kernel, LatentKernel* out, std::string* errorOut);

// --------------------------------------------------------------------------
// Parameters. Two kernels, because two questions.
//
// `pixels.kernel` governs RGB tiles, layer masks and selections -- all
// premultiplied-or-coverage data where a negative lobe is a visible artefact
// and a defensible trade. `latent` governs pigment, where it is not a trade at
// all (§2b). Defaults are Catmull-Rom and Bilinear respectively, which is the
// sharpest legitimate choice on each side rather than one conservative choice
// imposed on both.
// --------------------------------------------------------------------------
struct DocumentTransformParams {
  TransformParams pixels;
  LatentKernel latent = LatentKernel::Bilinear;
};

// --------------------------------------------------------------------------
// The store-level bridges. Each materialises `srcRegion` of its store, runs
// **one** `transformImage()` per packed RGBA image, and builds a fresh store
// covering `dstRegion`.
//
// **`dstFromSrcDoc` is in DOCUMENT coordinates**, not image-local ones, and it
// is the same matrix for every store of a layer -- which is what makes the mask
// land on the pixels. Each function folds its own two origins in:
//
//     imageMatrix = translate(-dstRegion.origin) * dstFromSrcDoc * translate(srcRegion.origin)
//
// That fold is PRD D16 applied to the coordinate change itself: composing the
// origins into the matrix keeps it at one reconstruction pass, where cropping
// the source, resampling, and then translating the result would be two
// generations and a half-pixel argument at each seam.
//
// **Out-of-place, always.** `out` is cleared first and must not alias `in`.
// A fresh store is the whole point rather than an implementation detail: a
// transform *moves* content, so the destination is not a patch over the source
// and every tile the content used to occupy must stop existing. Building into a
// new store makes that structural; clearing the old one in place would be an
// extra pass that has to know a region it cannot derive.
//
// Tiles that come out indistinguishable from absent are not allocated --
// all-transparent for RGB, mass-0 for pigment, all-reveal for a mask, all-zero
// for a selection. PRD C2, and it is what keeps a 45-degree rotate from paying
// 128 KiB for each of its four empty corners.
// --------------------------------------------------------------------------

bool transformRgbTiles(const TileStore& in, const DocumentRegion& srcRegion,
                       const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                       const TransformParams& params, TileStore* out, TransformReport* report,
                       std::string* errorOut);

// Section 3: transformed in **hide space**, `1 - coverage`, so that the
// resampler's transparent-black outside means *reveal* outside.
bool transformMaskTiles(const MaskTileStore& in, const DocumentRegion& srcRegion,
                        const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                        const TransformParams& params, MaskTileStore* out, TransformReport* report,
                        std::string* errorOut);

// Section 2: mass-weighted, through two packed RGBA images, with a kernel that
// has no negative lobes because the parameter's type has no other kind.
//
// `report` describes the `A` image (latent weights and mass); the `B` image
// (residual and mass) is resampled by the same matrix with the same parameters
// and reports identically, which `--selftest` checks rather than assumes.
bool transformPigmentTiles(const PigmentTileStore& in, const DocumentRegion& srcRegion,
                           const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                           LatentKernel kernel, bool prefilterDownscale, PigmentTileStore* out,
                           TransformReport* report, std::string* errorOut);

// **PRD E10: "selections can be transformed, moving coverage without touching
// pixels".** It is here and it is four lines of policy over the same bridge,
// for the reason §3 gives: a selection's absent-means-0 default already agrees
// with the resampler's own edge policy, so unlike a mask it needs no change of
// variable, and unlike a pigment layer it needs no premultiply -- coverage *is*
// the weight.
//
// The uint8 store costs nothing extra on an exact path: coverage round-trips
// `uint8 -> float -> uint8` losslessly for all 256 values (`writeCoverage()`
// rounds to nearest), so a flip or an integer translate of a selection is
// bit-exact. `--selftest` measures that over the whole byte range rather than
// asserting it.
bool transformSelectionCoverage(const Selection& in, const DocumentRegion& srcRegion,
                                const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                                const TransformParams& params, Selection* out,
                                TransformReport* report, std::string* errorOut);

// --------------------------------------------------------------------------
// The layer entry point (PRD D14, D16).
// --------------------------------------------------------------------------

struct LayerTransformResult {
  bool ok = false;
  std::string error;
  // What a caller records as the edit's label when `ok`. Empty otherwise.
  std::string editLabel;

  bool movedRgb = false;
  bool movedPigment = false;
  bool movedMask = false;

  // **The PRD D16 witness, and it is a MAX over the layer's stores, not a
  // sum.** D16's claim is that no stored value is convolved by a reconstruction
  // kernel more than once; a layer's RGB tiles and its mask are disjoint data,
  // so resampling both is still one pass per value. A pigment layer's two
  // packed images are disjoint channels of one texel, likewise (§2).
  //
  // **Zero** for a flip, a quarter turn or an integer translate -- PRD D15's
  // exact path, reached through `exactRemapKind()` unchanged. **One** for
  // everything else. If it is ever greater than one for a single call, some
  // store grew a second pass and the guarantee is gone.
  int reconstructionPasses = 0;

  // How much of the composed matrix was recognised as exact, reported for the
  // RGB or pigment store (the one the user thinks of as "the layer").
  ExactRemap exact = ExactRemap::None;
};

// Applies `dstFromSrc` to layer `index`: its pixels **and its mask**, by the
// same matrix, in one resample each.
//
// **Refusals**, each with the numbers, in core/LayerOps' style: an out-of-range
// index; a **locked** layer (§5); a non-invertible matrix, which is refused by
// `transformImage()` itself and reported through. A layer with no pixel storage
// at all -- Adjustment, Text, Strokes, Flats, Media -- **succeeds and moves
// nothing**, which is where this deliberately parts company with
// `core::translateLayer()`: that one refuses, because an align that reported
// moving five layers when it moved four would be lying about a set the user
// picked. A transform applies to a stack the user did not enumerate, and
// refusing the whole document because it contains a Text placeholder would be
// refusing on a technicality (§6).
//
// A layer whose storage is engaged but empty succeeds and moves nothing, and an
// identity matrix succeeds and changes nothing -- a no-op the user asked for is
// not an error.
LayerTransformResult transformLayer(Document& doc, size_t index, const Mat3& dstFromSrc,
                                    const DocumentTransformParams& params);

// **PRD D16 made into the easy call.** Composes the stack into one matrix and
// resamples once, so `reconstructionPasses` is 1 for a stack of any depth.
//
// This overload exists because the wrong version *looks right*: applying the
// stack entry by entry lands the picture in exactly the same place, and only
// the accumulated filter damage differs (ops/Transform.hpp §1). Making the
// composed call the shorter one is worth more than a comment asking for it.
// An empty stack composes to the identity and is a no-op.
LayerTransformResult transformLayer(Document& doc, size_t index, const TransformStack& stack,
                                    const DocumentTransformParams& params);

// --------------------------------------------------------------------------
// The document entry points (PRD D17).
// --------------------------------------------------------------------------

struct DocumentTransformResult {
  bool ok = false;
  std::string error;
  std::string editLabel;

  // Max over every store of every layer, and over the selection. §2 and
  // `LayerTransformResult::reconstructionPasses` for why a max. **Zero for a
  // crop and for a canvas resize**, structurally: neither goes near the matrix
  // path. Zero for a canvas flip or quarter turn, because those take PRD D15's
  // exact path.
  int reconstructionPasses = 0;

  // The document's extent before and after, so a caller can report the change
  // without holding the old document.
  int32_t previousWidth = 0;
  int32_t previousHeight = 0;

  size_t layersTouched = 0;

  // How many of those were **locked** and were moved anyway. §5 argues why they
  // are; this is the number a UI needs to say so out loud rather than have the
  // user discover it.
  size_t lockedLayersMoved = 0;

  // True when a `Selection*` was passed and was carried along with the pixels.
  bool selectionMoved = false;
};

// **Crop** (PRD D17). Extracts `[x, x + width) x [y, y + height)` of document
// space as the new document: sets the extent, and translates **every** store of
// every layer -- rgb, pigment and mask -- plus `selection` if one is passed, by
// `(-x, -y)`.
//
// **Bit-exact, structurally.** It is `core::translatedTileStore()` on the raw
// half words, never the matrix path (§4). Content that falls outside the new
// canvas is **kept**, not clipped, matching core/LayerGeometry.hpp §4's rule
// that "tiles live in absolute document coordinates and content outside
// `[0,width) x [0,height)` genuinely exists" -- a crop the user undoes must give
// back what it hid, and clipping here would make undo a lie. The transient cost
// is that a heavily cropped document keeps paying for tiles it cannot show;
// trimming them is a separate, explicit "delete cropped pixels" op and is not
// this function pretending to be one.
//
// A negative origin, or an extent that runs past the old canvas, is fine and is
// how "extend the canvas" is spelled -- the same signed-origin rule
// `ops/Transform.hpp`'s `cropImage()` states. Refuses only a zero extent.
//
// `selection` may be null.
DocumentTransformResult cropDocument(Document& doc, int32_t x, int32_t y, uint32_t width,
                                     uint32_t height, Selection* selection);

// **Canvas size** (PRD D17): change the extent, do not touch the pixels.
// `cropDocument()` with the origin the anchor implies, so there is one
// index-copy path. The centred offsets are **floored**, for
// `ops/Transform.hpp::resizeCanvas()`'s reason: growing by an odd number of
// pixels has to put the extra pixel somewhere, and flooring puts it on the
// right/bottom for every parity instead of jittering as the user drags a size
// field.
DocumentTransformResult resizeDocumentCanvas(Document& doc, uint32_t width, uint32_t height,
                                             CanvasAnchor anchor, Selection* selection);

// **Image size** (PRD D17): resample every layer to a new extent.
//
// One matrix, `transformScale(width / doc.width, height / doc.height)`, applied
// to every store of every layer through the same path a rotate takes -- so a
// downscale prefilters (D17's own clause) by the same area-average, and there is
// no second resizer to keep in step.
//
// A 1:1 request is a no-op and is **not** run as an identity resample, matching
// `resizeImage()`: a resize that changes nothing must not perturb a value.
//
// Refuses a zero extent, a document with a zero extent (there is no scale factor
// from nothing), and -- naming it -- a request that would need a lobe-free
// kernel it was not given. That last one is the only place §2(b)'s refusal
// surfaces at document level: if the stack contains a Pigment layer, the
// `params.latent` kernel is what it uses, and since that type cannot hold a
// ringing kernel there is nothing to refuse. The refusal a caller can trip is
// `latentKernelFor()`, used *before* getting here.
DocumentTransformResult resizeDocumentImage(Document& doc, uint32_t width, uint32_t height,
                                            const DocumentTransformParams& params,
                                            Selection* selection);

// **Rotate / flip the canvas** (PRD D14, D15 at document level).
//
// `dstFromSrc` maps old document coordinates to new ones, and `newWidth` x
// `newHeight` is the new extent -- for a quarter turn that is the transposed
// one, which `transformRotate90()` already builds a matrix for. Every layer and
// the selection move by the same matrix.
//
// **Exact for a flip and for a quarter turn**, because the matrix reaches
// `exactRemapKind()` unchanged and takes the path that performs no arithmetic
// on a texel value (PRD D15). `reconstructionPasses` reads 0 there, which is the
// assertion that separates it from an implementation that merely looks right.
DocumentTransformResult transformDocument(Document& doc, const Mat3& dstFromSrc,
                                          uint32_t newWidth, uint32_t newHeight,
                                          const DocumentTransformParams& params,
                                          Selection* selection);

}  // namespace np
