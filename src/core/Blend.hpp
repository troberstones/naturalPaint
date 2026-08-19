#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/Pigment.hpp"

// core/Blend (PLAN.md "Phase 5 -- Stack it", step 2: "the linear-safe set
// (over, plus, multiply, screen, min, max) and `Mix`, the KM latent lerp.
// Display-referred modes labelled as such (PRD B7)"). PRD B7, C3, L5.
//
// --- What this file owns --------------------------------------------------
//
// The blend *vocabulary* (which modes exist, what each is called on disk and
// on screen, and which colour referral each belongs to) and the blend
// *arithmetic* (one two-pixel function per mode). It owns no loop: the
// document walk that applies a mode per layer stays in core/Composite, which
// is what its own header said would happen -- "when `core/Blend` lands,
// `compositeOver()` moves there verbatim and this file keeps only the document
// walk". `compositeOver()` below is that function, moved unchanged.
//
// --- The enum, and why `Layer::blend` is still a `std::string` -------------
//
// This is the decision core/Layer.hpp deferred to this step, and it is
// resolved *differently* from the way that header predicted, so the departure
// is argued here rather than left to be noticed.
//
// core/Layer.hpp expected "this member to become that enum plus one
// name<->enum mapping". Two things argue against converting the member:
//
//  1. **PRD I10 at the value level is free with a string and machinery with an
//     enum.** A newer build's `np:blend = "linear-burn"` survives a load, an
//     edit and a save through this build byte-for-byte, because nothing
//     between io/NpaintFile's reader and its writer ever parses it into a
//     closed set. With an enum the loader would have to detect the unknown
//     name, stash it in a side channel, and have the writer prefer the side
//     channel over the enum -- three moving parts whose only job is to undo
//     the enum, and each of them a place the verbatim guarantee can break.
//     `--selftest` asserts that guarantee today; with a string it is a
//     structural property, with an enum it becomes a test of machinery.
//  2. The one thing an enum on the member buys is type safety at assignment
//     sites, and that is obtainable by typing the **setter** instead:
//     `core::setLayerBlend(doc, i, BlendMode::Multiply)` is exactly as
//     type-safe as an enum member, and it is the only path the UI uses.
//
// So the resolution is: **the enum lives here, the member stays a string, and
// there is exactly one mapping in each direction** -- the same shape
// `layerKindName()` / `layerKindFromName()` already have for `Layer::kind`.
// Everything that acts on a blend does so by calling `blendModeFromName()` at
// its single point of use; nothing round-trips a name through the enum.
//
// The cost of that choice, stated: `Layer::blend` can hold a string no mode
// answers to, and every consumer has to decide what to do with one. There is
// exactly one answer in this codebase and core/Composite.hpp argues it at
// length -- composited as `over`, reported by name, never silently, never
// refused.
//
// --- PRD B7: display-referred modes are labelled as such -------------------
//
// The label is a **field on the mode's metadata** (`BlendModeInfo::space`),
// not a comment and not a list maintained beside the modes. A caller cannot
// obtain a mode's UI text without also obtaining its referral, because
// `blendModeInfo()` returns them together and app/LayerPanel's
// `blendMenuEntryText()` -- the only function that renders a menu entry --
// derives the marker from that field. Adding a mode without classifying it is
// a compile error (aggregate initialisation of a struct with no default for
// `space`), which is the property B7 needs: the UI *cannot* show a
// display-referred mode without its label.
//
// **The criterion, so the classification is derived rather than asserted.** A
// mode is scene-linear (`BlendSpace::LinearLight`) when its formula is
// monotone non-decreasing in each operand over the whole non-negative range a
// linear working space can hold, i.e. when it has no built-in reference white.
// A mode is `BlendSpace::DisplayReferred` when it is only well-behaved on
// [0,1] -- when the value 1.0 is special to it. Applying that:
//
//   over      cs + cb*(1-as).                    No bound. Linear.
//   plus      cs + cb.                           No bound. Linear.
//   multiply  cs*cb (opaque case).               Monotone for all values >= 0;
//                                                1.0 is the *identity*, not a
//                                                ceiling, because the source
//                                                acts as a transmittance
//                                                ratio. Linear.
//   min, max  min/max.                           No bound. Linear.
//   screen    cs + cb - cs*cb.                   **Not** monotone above 1:
//                                                screen(2,2) = 0 and
//                                                screen(2,3) = -1, i.e. adding
//                                                light makes the result
//                                                darker. It is
//                                                1 - (1-cs)(1-cb), which only
//                                                means anything if 1.0 is
//                                                white. Display-referred.
//
// **This departs from PLAN.md's own wording**, which calls all six "the
// linear-safe set". Screen is in the set, is implemented, and is the one
// member of it that fails the test -- so it is the mode that makes B7's
// labelling mechanism carry a real subject instead of being dead code waiting
// for an overlay/soft-light family that this step was never scoped to add.
// `--selftest` asserts the misbehaviour numerically rather than taking the
// classification on trust. Nothing clamps: whether to clamp is a display/
// export policy decision (color/Space.hpp), and io/Export already makes it at
// its own quantization step.
//
// --- Premultiplied, linear light, and what that does to each formula -------
//
// Every function here takes and returns **premultiplied** ("associated")
// linear-light RGBA, because that is what core/Tile stores and what
// core/Composite's walk carries end to end (DESIGN-imaging.md §2, PRD B4).
// Getting the premultiplied form of multiply and screen wrong is the classic
// silent bug in this area -- both are usually written for straight, opaque
// colour -- so each is derived below from the general separable-blend
// formula rather than transcribed.
//
// With straight colours Cs = cs/as and Cb = cb/ab, the standard separable
// blend (PDF 1.7 §11.3.8, CSS Compositing 1 §3, and the same formula
// Photoshop's separable modes follow) is
//
//   co = cb*(1 - as) + cs*(1 - ab) + as*ab*B(Cs, Cb)
//   ao = as + ab*(1 - as)
//
// The three terms are exactly the three Porter-Duff regions: backdrop where
// the source does not cover, source where the backdrop does not, and the
// blend function where both do. Two consequences worth stating because they
// are properties of every mode here rather than of any one:
//
//   * **Alpha is `over` for every mode.** A blend mode changes colour, not
//     coverage; `ao` above does not mention `B` at all. So switching a layer's
//     blend mode never changes the composite's alpha channel, which
//     `--selftest` asserts across all six.
//   * **A fully transparent source is an exact identity on the backdrop, for
//     every mode.** as == 0 makes cs == 0 for well-formed premultiplied data,
//     which kills the second and third terms, and leaves cb * 1.0f -- a
//     multiplication by literal 1.0f, exact for every finite float. Likewise a
//     transparent backdrop passes the source through untouched, which is why a
//     single-layer document composites bit-identically under any mode. Both
//     are asserted at zero tolerance, not reasoned about.
//     (Caveat, stated rather than defended: EXR permits "additive" texels with
//     as == 0 and cs != 0. Nothing in this codebase writes one, and for such a
//     texel `plus` would legitimately still add. The identity claim above is
//     about well-formed data.)
//
// Substituting each B and simplifying so that **no division by alpha appears
// anywhere** -- the derivations are written out at each implementation in
// Blend.cpp:
//
//   over      co = cs + cb*(1-as)                          [exact step-1 form]
//   plus      co = cs + cb
//   multiply  co = cs*cb + cs*(1-ab) + cb*(1-as)
//   screen    co = cs + cb - cs*cb
//   min       co = min(ab*cs, as*cb) + cs*(1-ab) + cb*(1-as)
//   max       co = max(ab*cs, as*cb) + cs*(1-ab) + cb*(1-as)
//
// `over` is deliberately **not** written in the three-term form even though
// B(Cs,Cb) = Cs makes it algebraically identical: `cb*(1-as) + cs*(1-ab) +
// ab*cs` is not bit-identical to `cs + cb*(1-as)` in floating point, and step
// 1's regression boundary -- a single-layer and a non-overlapping multi-layer
// document compositing byte-identically to the plain sum they replaced -- is
// asserted in the suite at zero tolerance. Moving `over` into this file must
// not perturb it by one ulp, so the function moved verbatim.
namespace np {

// Whether a mode's arithmetic is meaningful in the linear working space, or
// presupposes a display encoding where 1.0 is white. PRD B7's label, as data.
enum class BlendSpace {
  LinearLight,
  DisplayReferred,
};

// The closed set of blend modes this build knows the *name* of. Not the set it
// can composite: see `BlendModeInfo::compositesPixels`.
//
// `Normal` is `over`, and its name on disk is core/Layer.hpp's
// `kDefaultBlendName` ("normal") rather than "over" -- that string is already
// in every document this build has ever written, and docs/document-format.md
// shows `np:blend "multiply"` in the same lowercase style.
enum class BlendMode {
  Normal,
  Plus,
  Multiply,
  Screen,
  Min,
  Max,
  Mix,
};

struct BlendModeInfo {
  BlendMode mode;
  // The `np:blend` string. Lowercase, stable, and the only spelling
  // `blendModeFromName()` accepts -- a format value is not the place for
  // case-insensitive matching, because two spellings on disk would both be
  // "the" name and neither would round-trip the other.
  const char* name;
  // The UI label. Title case, and deliberately a separate field: renaming what
  // a menu says must never rename what a file stores.
  const char* label;
  // PRD B7. See this header's criterion.
  BlendSpace space;
  // Whether `blendPixel()` -- the two-RGBA-texel function -- really implements
  // this mode. False for exactly one mode (`Mix`), and that stays false after
  // Phase 5 step 3 wired `Mix` up, because it is an honest statement about
  // `blendPixel()`: an RGBA texel carries no latent, so `Mix` is not a pixel
  // operation and never becomes one. See `compositesLatents` below.
  bool compositesPixels;
  // Whether core/Composite implements this mode at the **layer** level, on
  // pigment latents rather than on RGBA texels. True for exactly one mode
  // (`Mix`), and it became true at PLAN.md Phase 5 step 3, when Pigment layers
  // gained the latent tiles step 2 named as the single unblocking condition.
  //
  // The two flags are deliberately separate rather than one widened flag,
  // because they answer different questions and both are asked: `blendPixel()`
  // needs to know whether it can evaluate a mode from two texels (it cannot,
  // for `Mix`, ever), while core/Composite needs to know whether the *document
  // walk* can honour it (it can, for the layer pair PRD L5 restricts `Mix`
  // to). Collapsing them would have made one of the two answers a lie.
  bool compositesLatents;
  // PRD L5: "`Mix` appears in the blend dropdown only between two Pigment
  // layers". True for exactly one mode, and consumed by
  // `blendModeAvailableForLayer()` so the rule lives in one predicate rather
  // than in the dropdown.
  bool pigmentPairOnly;
};

// Every mode, in the order a dropdown should list them: `Normal` first
// because it is the default, the rest of the linear set, then `Mix` last
// because it is the opt-in one (PRD C3).
std::span<const BlendModeInfo> allBlendModes() noexcept;

const BlendModeInfo& blendModeInfo(BlendMode mode) noexcept;

// The `np:blend` string for a mode. Never empty.
const char* blendModeName(BlendMode mode) noexcept;

// The one parse in the codebase. `std::nullopt` for any name outside the set
// -- including the empty string, and including a newer build's mode this one
// has never heard of. Callers must handle that case; core/Composite.hpp says
// what the single agreed answer is.
std::optional<BlendMode> blendModeFromName(std::string_view name) noexcept;

// Whether this build implements `blend` **anywhere** -- at the texel level or
// at the layer level. True for all seven named modes as of Phase 5 step 3
// (`compositesPixels || compositesLatents`); false only for a name outside the
// set, including the empty string.
//
// Moved here from core/Composite, where step 1 put it when "normal" was the
// only answer. It is the context-free question, and it is the right one for
// app/LayerPanel's row text, which has a `Layer` and no `Document` to ask
// about position. The **contextual** question -- would compositing *this*
// layer, where it actually sits, produce what its blend asks for -- is
// `blendIsImplementedForLayer()` below, and that is the one core/Composite
// warns from.
bool blendIsImplemented(std::string_view blend) noexcept;

// The contextual form: would compositing `doc.layers[layerIndex]`, where it
// sits, produce the pixels its blend asks for?
//
// It differs from `blendIsImplemented()` for exactly one mode, and the
// difference is PRD L5 rather than an implementation gap. `Mix` is a
// Kubelka-Munk lerp between two pigment latents; it is meaningful only for a
// Pigment layer sitting on a Pigment layer, which is precisely what
// `blendModeAvailableForLayer()` already encodes and what the dropdown already
// enforces. A `mix` that arrives from a *file* need not satisfy it -- an
// unrecognised or misplaced `np:blend` is carried verbatim, never coerced
// (PRD I10) -- so the composite has to answer for one, and the answer is
// core/Composite's long-standing one: composited as `over`, warned by name,
// never silently, never refused.
//
// The second case it answers false for is a **chained** `Mix`: three Pigment
// layers where the middle one has already been consumed as the lower half of a
// mixed pair cannot also be the lower half of the next one. core/Composite.hpp
// states that limit and why it is a limit rather than a bug.
//
// An out-of-range `layerIndex` is false, like `blendModeAvailableForLayer()`.
// Not `noexcept`: it computes `mixPairing()`, which allocates.
bool blendIsImplementedForLayer(const Document& doc, size_t layerIndex);

// Which layer, if any, each layer is mixed *with* -- the pairing PRD L5's
// `Mix` produces, computed once for a whole Document so that core/Composite,
// core/Probe and `blendIsImplementedForLayer()` cannot disagree about it.
//
// `mixedWithBelow[i]` is true when layer `i` carries `mix`, L5 holds for it,
// and layer `i-1` has not already been consumed as some other pair's lower
// half. `consumedByAbove[i]` is true when layer `i` is that lower half.
//
// Pairing is greedy from the bottom, which makes it deterministic and makes
// the bottom-most pair in a chain the one that forms: for Pigment layers
// [P0, P1(mix), P2(mix)] it pairs (0,1) and leaves P2 unpaired, warned about,
// and composited as `over`.
struct MixPairing {
  std::vector<bool> mixedWithBelow;
  std::vector<bool> consumedByAbove;
};
MixPairing mixPairing(const Document& doc);

// PRD L5, as one predicate rather than as dropdown code.
//
// True when `mode` may be offered for `doc.layers[layerIndex]`. Every mode
// except `Mix` is always available. `Mix` requires that the layer itself is a
// Pigment layer **and** that there is a layer directly beneath it that is also
// a Pigment layer -- docs/ui.md §3.4's resolution in those words ("shown only
// when both the layer and the one beneath it are Pigment layers -- it is
// meaningless otherwise").
//
// "Beneath" is `layerIndex - 1`, because `Document::layers` is bottom-to-top
// (docs/document-format.md, and core/Composite walks index 0 first). So the
// bottom layer can never take `Mix`: there is nothing under it to mix with.
// An out-of-range `layerIndex` returns false for every mode rather than
// reading past the vector.
bool blendModeAvailableForLayer(const Document& doc, size_t layerIndex,
                                BlendMode mode) noexcept;

// `src` composited **over** `dst`, both premultiplied, linear-light RGBA.
//
//   out.rgb = src.rgb + dst.rgb * (1 - src.a)
//   out.a   = src.a   + dst.a   * (1 - src.a)
//
// `src` is the upper layer, `dst` the accumulated composite of everything
// beneath it. Argument order matches the way the operation is written and
// spoken ("src over dst"), which is worth more than matching a memcpy-style
// destination-first convention nothing else here uses.
//
// **Moved verbatim from core/Composite (PLAN.md Phase 5 step 1), signature and
// body unchanged**, exactly as that header planned. It is kept as its own
// symbol rather than folded into `blendPixel()`'s switch because core/Probe
// and `--selftest` both name it directly, and because the identity below is a
// claim about *this* function.
//
// **The identity that makes step 1's regression check possible**: when
// `src.a == 0` the result is exactly `dst` (`dst * (1 - 0)` is a
// multiplication by literal 1.0f, which is exact for every finite float), and
// when `dst` is all zeros the result is exactly `src`. So a document whose
// layers never overlap -- including every single-layer document -- composites
// to bit-identical output under this function and under the plain sum it
// replaced. That is asserted in `--selftest`, not merely reasoned about here,
// and this step additionally asserts that every *other* mode has the same
// identity.
//
// Not clamped. A working-space value may legitimately exceed 1.0 (color/
// Space.hpp: "whether to clamp is a display/export policy decision"), and
// io/Export already makes that decision at its own quantization step.
std::array<float, 4> compositeOver(const std::array<float, 4>& src,
                                   const std::array<float, 4>& dst) noexcept;

// One texel of `src` blended over one texel of `dst` under `mode`, both
// premultiplied, linear-light RGBA. The single dispatch point; core/Composite
// and core/Probe both go through it so an eyedropper and an export can never
// disagree.
//
// `BlendMode::Mix` returns `compositeOver()` and that is a fallback, not an
// implementation -- `blendModeInfo(Mix).compositesPixels` is false and stays
// false. The reason it cannot be implemented *here* is structural rather than
// a shortfall of effort, and it did not change when Phase 5 step 3 wired `Mix`
// up: `Mix` is a Kubelka-Munk lerp between two **latents**, and an RGBA texel
// has none. The place it is implemented is core/Composite's document walk,
// which has the two layers' pigment tiles in hand; see `mixLatents()`.
std::array<float, 4> blendPixel(BlendMode mode, const std::array<float, 4>& src,
                                const std::array<float, 4>& dst) noexcept;

// `Mix` (PRD C3, **P0**): the Kubelka-Munk latent lerp, as a pure function.
//
// --- Its layer wiring, which arrived at Phase 5 step 3 ---------------------
//
// Step 2 shipped this function with no caller in the document walk and stated
// the unblocking condition as exactly one thing: "a tile that stores a
// latent". Step 3 built that tile (core/Pigment's `PigmentTile`), and the
// prediction held to the letter -- **nothing in this function changed**;
// core/Composite's walk gained a Pigment-pair branch that calls it. The
// alternative step 2 rejected, synthesising latents from a layer's RGB so
// there would be something to blend, was never revisited: `rgbToLatent()`'s
// decomposition is "plausible rather than true" (docs/ui.md §3.3), and a
// `Mix` built on it would be confident, wrong colour that looked like the
// feature working. Pigment layers carry real latents now, so nothing needs
// inventing.
//
// What core/Composite does with it, in one line, because the *weight* is the
// part a reader will want to check: `t` is the **upper layer's mass**, not its
// opacity. Opacity is transparency on every layer kind (PRD C3), and it is
// applied afterwards, in projected RGB, as a fade of the whole mixed result
// back toward the backdrop -- see core/Composite.hpp, which proves that fade
// is the same operation `over` already performs with opacity.
//
// --- The arithmetic --------------------------------------------------------
//
// Mixbox mixes by lerping latents componentwise and projecting back
// (third_party/mixbox/cpp/mixbox.cpp: `latent_mix[i] = (1-t)*latent1[i] +
// t*latent2[i]` over all MIXBOX_LATENT_SIZE components). `np::Latent` stores
// three of the four pigment weights plus the RGB residual, with the fourth
// weight implied as `1 - (c0+c1+c2)`; because that implication is affine,
// lerping c0..c2 lerps c3 by the same t automatically, so lerping six floats
// is *exactly* equivalent to Mixbox's seven and not an approximation of it.
// `--selftest` asserts that equivalence on the implied component rather than
// leaving it as an argument.
//
// `t` is the weight of `b`: t == 0 returns `a` exactly, t == 1 returns `b`
// exactly (`std::lerp`'s own exactness guarantees at the endpoints), and
// values outside [0,1] extrapolate rather than being clamped -- clamping is a
// policy for the caller that decides what t *is*, and a layer has not decided
// yet.
Latent mixLatents(const Latent& a, const Latent& b, float t) noexcept;

}  // namespace np
