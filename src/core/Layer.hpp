#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/Mask.hpp"
#include "core/OpStack.hpp"
#include "core/Pigment.hpp"
#include "core/TileStore.hpp"

// core/Layer (PLAN.md "Phase 2 -- See a file", step 4; CONTEXT.md "Layer
// kinds"). "Design for N, ship 1": all seven kinds CONTEXT.md names are
// constructible today -- the enum below has a value for each -- but only
// `RGB` and, as of PLAN.md Phase 5 step 3, `Pigment` actually own pixel
// storage. The other five (Media, Strokes, Adjustment, Text, Flats) are inert
// placeholders nothing exercises yet: Media needs the fluid solver's own
// per-medium state on top of the pigment tiles this step added, and
// Adjustment/Text/Strokes/Flats "hold no pixels of their own" per CONTEXT.md
// and structurally never will -- they'll eventually gain their own
// parameter-only members (a string+font, a Dab list, ...), not tile storage.
//
// **`Adjustment` stopped being inert at PLAN.md Phase 5 step 5.** It owns no
// tiles and never will; what it owns is the `ops` member below, which was
// already there, plus the mask every kind already had. So the kind needed no
// new storage at all -- it needed core/Composite to stop skipping it (an
// Adjustment layer transforms the composite accumulated *beneath* it rather
// than contributing to it, PRD C5) and io/NpaintFile to gain a carrier for an
// op stack, without which the layer's whole content would vanish on save.
// Media, Strokes, Text and Flats are still inert placeholders: Media needs the
// fluid solver's own per-medium state on top of the pigment tiles step 3
// added, and the other three still have no parameter member to hold.
namespace np {

// LayerKind lives here, not in app/Keymap.hpp where it was first sketched --
// a layer's kind is a core domain concept ("what kind of Layer this is"),
// and app/ depends on core/, never the reverse. app/Keymap.hpp now includes
// this header and reuses np::LayerKind directly for its binding-scope field
// rather than keeping a second, competing definition.
enum class LayerKind {
  Pigment,
  RGB,
  Media,
  Strokes,
  Adjustment,
  Text,
  Flats,
};

// Small enough to be `inline` in-header, matching TileStore.hpp/Tile.hpp's
// precedent of staying header-only when nothing here is non-trivial.
inline const char* layerKindName(LayerKind kind) {
  switch (kind) {
    case LayerKind::Pigment: return "Pigment";
    case LayerKind::RGB: return "RGB";
    case LayerKind::Media: return "Media";
    case LayerKind::Strokes: return "Strokes";
    case LayerKind::Adjustment: return "Adjustment";
    case LayerKind::Text: return "Text";
    case LayerKind::Flats: return "Flats";
  }
  return "?";
}

// The colour labels PRD C15 asks for, as *names* rather than an enum, for
// exactly `kDefaultBlendName`'s reason one screen down: the member that carries
// one is a `std::string`, so a label a newer build invented (`teal`) survives a
// load/save through this build untouched (PRD I10 at the value level), and
// app/LayerPanel maps a known name to a swatch while showing an unknown one as
// itself. This list is the set this build can draw; it is not a closed set the
// format enforces.
//
// Photoshop's seven, because a user's existing muscle memory for "the red ones
// are the ones I have not finished" is the only evidence available about which
// seven, and inventing a different palette would make the labels a translation
// exercise. Lower case, because that is what goes in the file and comparisons
// are exact -- app/LayerPanel upper-cases for display the way it already does
// for `np:blend`.
inline const char* const kLayerColorLabelNames[] = {"red",  "orange", "yellow", "green",
                                                    "blue", "violet", "grey"};

// "No label", which is the empty string and is what every layer this build has
// ever created carries. Named rather than spelled `""` at each site, and it is
// the reason io/NpaintFile writes no `np:label` attribute at all for an
// unlabelled layer: a document with no labels produces exactly the bytes it
// produced before this member existed.
inline constexpr const char* kNoLayerColorLabel = "";

inline std::optional<LayerKind> layerKindFromName(std::string_view name) {
  if (name == "Pigment") return LayerKind::Pigment;
  if (name == "RGB") return LayerKind::RGB;
  if (name == "Media") return LayerKind::Media;
  if (name == "Strokes") return LayerKind::Strokes;
  if (name == "Adjustment") return LayerKind::Adjustment;
  if (name == "Text") return LayerKind::Text;
  if (name == "Flats") return LayerKind::Flats;
  return std::nullopt;
}

// The blend identity a Layer carries, as a *name* rather than an enum.
//
// This is deliberate and it is the one decision in this header worth
// arguing, so the reasoning is here rather than in a commit message.
//
// PLAN.md Phase 5 step 2 is `core/Blend` -- "the linear-safe set (over,
// plus, multiply, screen, min, max) and `Mix`, the KM latent lerp.
// Display-referred modes labelled as such (PRD B7)". That step owns the
// enumeration, and it owns two decisions this step cannot make honestly:
// which display-referred modes exist at all, and how they are *labelled* as
// display-referred. Writing an enum here would be guessing at both, and a
// guess in a file format is expensive to withdraw -- docs/document-format.md
// stores this value as an EXR `np:blend` string, so a wrong enum today
// becomes a wrong string on disk tomorrow.
//
// A string also makes PRD I10 ("attributes the reader does not understand
// are preserved verbatim") true for free at the *value* level, not just the
// attribute level: a newer build's `np:blend = "linear-burn"` survives a
// load/save through this build exactly, because nothing here ever parses it
// into a closed set and back. With an enum, an unrecognised name would have
// to be caught, stashed in a side channel and re-emitted -- machinery whose
// only job is to undo the enum.
//
// So: this member carries the identity, and nothing in this build acts on
// it. There is no blending anywhere in this codebase yet. When Phase 5's
// `core/Blend` lands, the natural change is for this member to become that
// enum plus one name<->enum mapping (exactly the shape `layerKindName()` /
// `layerKindFromName()` above already have for `kind`), and io/NpaintFile is
// the single place that would need to route through it.
inline constexpr const char* kDefaultBlendName = "normal";

// One entry in a Document's layer list (CONTEXT.md Relationships: "A Layer
// holds Tiles; tiles are allocated only where content exists").
//
// The metadata members below (name/blend/opacity/visible/locked/parent)
// arrived with Phase 4 step 4 as plain data with no behaviour attached
// anywhere -- "nothing composites, nothing honours `visible`, nothing
// enforces `locked`, and no UI shows `name`" -- because they are what the
// native document format persists (docs/document-format.md gives every layer
// part an `np:name`, `np:blend`, `np:opacity`, `np:visible`, `np:locked` and
// `np:parent`) and a format that round-trips a struct with nothing in it
// proves nothing. That step said Phase 5 step 1 was where they would start
// having *effects*.
//
// **Phase 5 step 1 has landed, and this is where each of them now stands:**
//
//   name     shown by the layers panel (ui/MacPaintUI.cpp, via
//            app/LayerPanel's row text) and settable through
//            `core::setLayerName()`.
//   blend    still carried, still never parsed here. `core/Composite`
//            implements exactly one blend (`over`, i.e. the
//            `kDefaultBlendName` below) and reports any other name by name
//            rather than acting on it; the enumeration is still Phase 5
//            step 2's to own, so this member is still a `std::string` for
//            all the reasons argued above it.
//   opacity  a real coverage multiplier in `core/Composite`.
//   visible  a hidden layer contributes exactly nothing to the composite.
//   locked   enforced -- but only by `core/LayerOps`' operations, which is
//            all there is to enforce it on: there is still no pixel-edit
//            path to a layer at all. core/LayerOps.hpp states the exact
//            scope of the lock and is blunt about what it cannot mean yet.
//   parent   still carried, still never acted on -- **and Phase 5 step 9 did
//            not change that, which is worth saying because it was expected
//            to.** This line used to read "honouring a group link means
//            compositing a group's members offscreen first, which is Phase 5
//            step 9's machinery". Step 9 landed clipping masks and needed no
//            offscreen buffer at all: a clipping group is folded per texel,
//            inside the base's own tile walk (core/Composite.hpp §13). So the
//            prediction was wrong about the mechanism as well as the timing,
//            and groups are still unbuilt -- this build creates none.
//   clipped   real as of Phase 5 step 9: the layer is clipped by the alpha of
//            the layer below (PRD C9). See the member below.
struct Layer {
  // CONTEXT.md: Pigment is "the default kind for a new layer", and PRD's
  // principle 3 ("Pigment by default. A new layer mixes as paint"). Kept as
  // the default here since Phase 2, and as of Phase 5 step 3 a default-
  // constructed Layer of this kind can genuinely hold, project, blend and
  // save pigment -- it just cannot be painted on, because no brush reaches a
  // Layer at all. `Document::createBlank()` and `core::makeRgbLayer()` still
  // hand out RGB layers for that reason, and say so.
  LayerKind kind = LayerKind::Pigment;

  // Pixel storage for RGB layers ("pixels are Working space RGBA",
  // CONTEXT.md). Populated only when `kind == RGB`; std::nullopt for every
  // other kind, including Pigment (which has `pigmentTiles` below instead)
  // and Adjustment/Text/Strokes/Flats (which never hold pixels -- an
  // Adjustment layer with either tile store engaged is malformed, and
  // io/NpaintFile refuses to save one by name).
  //
  // Deliberately std::optional<TileStore> rather than a mandatory, always-
  // present TileStore member. Two reasons: (1) most layer kinds hold no
  // pixels at all, so a mandatory member would carry TileStore's (currently
  // empty, but not necessarily forever) bookkeeping for kinds that never
  // populate it; (2) more importantly, DESIGN-imaging.md §2's own memory
  // table gives a Pigment/Media tile a *different* shape -- 7 channels
  // (c0/c1/c2/mass plus a 3-channel residual) against RGB's 4-channel
  // rgba16float, i.e. not a core::TileStore<core::Tile> at all.
  //
  // **Phase 5 step 3 took the extension this comment predicted, verbatim.**
  // It said: "the natural extension at that point is a second, similarly-
  // optional member (or a variant) holding the pigment tile type, populated
  // for Pigment/Media instead of rgbTiles -- not a change to this field's
  // type." That is exactly `pigmentTiles` below, and this field's type is
  // unchanged. The second-member form beat a `std::variant` for the same
  // reason core/OpStack.hpp gives for `Op` not being one: this codebase has
  // no variant anywhere, `Layer` is already "kind tag plus fields only
  // meaningful for that kind", and a variant would have made every existing
  // `layer.rgbTiles.has_value()` site a `std::holds_alternative` rewrite for
  // no reader benefit.
  std::optional<TileStore> rgbTiles;

  // Pixel storage for Pigment layers: latent x mass at f16, seven channels
  // per texel (core/Pigment.hpp). Populated only when `kind == Pigment`;
  // std::nullopt otherwise, and it is an invariant of this struct that at
  // most one of `rgbTiles` and `pigmentTiles` is engaged.
  //
  // **A Pigment layer holds latents, and nothing in this build can paint
  // one.** `sim::PaintSim` owns one dense texture and a stroke reaches no
  // `Layer` at all, so the only ways a Pigment layer gets content today are
  // loading a `.npaint` that has some, or a test writing texels directly.
  // That gap is Phase 10's ("Paint on it") and is stated here rather than
  // implied away: the storage, the projection, the compositing, the blending
  // and the file format are real; the brush is not connected to any of it.
  //
  // The Media kind will reuse this member unchanged -- DESIGN-imaging.md
  // gives Media the same latent-plus-mass tile -- plus per-medium simulation
  // state that has no home on `Layer` yet. Nothing here anticipates that.
  std::optional<PigmentTileStore> pigmentTiles;

  // The per-layer mask (PRD C4's "per-layer mask", PLAN.md Phase 5 step 4):
  // per-texel coverage at f16, one channel, 32 KiB per occupied tile
  // (core/Mask.hpp). `std::nullopt` means the layer has no mask -- which is
  // not the same thing as a mask that reveals everything, and core/Mask.hpp
  // separates the three states (absent, all 1.0, all 0.0) at length.
  //
  // **A third optional store rather than a member of the other two**, because
  // a mask is orthogonal to what the layer holds: the same mask applies to an
  // RGB layer's tiles, a Pigment layer's latents, and -- as of Phase 5 step 5,
  // which brought them -- an Adjustment layer that holds no pixels at all
  // (PRD D13's dodge and burn is exactly that: "a brush painting into an
  // adjustment layer's mask"). Hanging it off `rgbTiles`/`pigmentTiles` would
  // have made it unavailable to precisely the kind that needs it most; step 5
  // is where that prediction was cashed, and it needed no change here. It is therefore
  // the one storage member here that is **not** mutually exclusive with the
  // others; the "at most one of rgbTiles and pigmentTiles" invariant above is
  // unaffected.
  //
  // **What it multiplies, in one line, because it is the whole of PRD C3 for
  // this member: coverage, after the projection and after the op stack, never
  // mass and never a mixing weight.** core/Composite.hpp §5 derives it and
  // `--selftest` prints the values that separate the two. A mask on a Pigment
  // layer that scaled `pig.m` would be an eraser (PRD F10), not a mask -- it
  // would change the pigment mixture rather than let the backdrop through.
  //
  // **Nothing in this build can paint one**, exactly as for `pigmentTiles`:
  // the content of a mask can only come from a `.npaint` or from a test
  // writing texels. `core::addLayerMask()` creates an empty (reveal-all) one
  // and `core::removeLayerMask()` takes it away, which is the whole of the
  // lifecycle a user can reach.
  std::optional<MaskTileStore> mask;

  // The per-layer, non-destructive grading stack (DESIGN-imaging.md §3's own
  // Layer diagram: "ops  OpStack -- per-layer, non-destructive").
  //
  // **Where an OpStack lives was the ownership decision Phase 4 refused to
  // guess at, and PLAN.md Phase 5 step 3 is where it is made.**
  // io/NpaintFile.hpp deferred `np:ops` in exactly those terms: "`core::OpStack`
  // is real, but it lives on `app::AppState`, not on `core::Layer` or
  // `core::Document` -- so there is no per-layer op stack to serialise, and
  // hanging one off Layer here purely to have something to write would be
  // inventing the very ownership decision Phase 5 step 3 has to make." The
  // decision: **the layer owns it.** `app::AppState::opStack` stays where it
  // is and keeps meaning what it meant (the global grade previewed on the GPU
  // through sim::PaintSim); it is not the same stack and is not migrated here.
  //
  // **The ordering this member exists to pin down, which is the load-bearing
  // sentence of PLAN.md's step 3: the stack applies *after* the latent -> RGB
  // projection, so grading never bakes the latents.** core/Composite projects
  // a Pigment layer's (latent, mass) to a premultiplied RGBA texel and *then*
  // runs this stack over it; the stored latents are untouched by any grade,
  // for good and for a reason DESIGN-imaging.md §3 states as the document
  // invariant: "any op that is a linear combination of pixels stays valid in
  // latent space, and any op that is not, is not" -- levels, curves and every
  // LUT are in the second column. Applying them to `c0..c2` would not be a
  // grade of the colour, it would be a different pigment. `--selftest`
  // asserts the tile's raw half words are bit-identical across a grade.
  //
  // It applies to RGB layers too, at the same point in the walk (there the
  // "projection" is just the tile read), because a per-layer op stack is a
  // property of a Layer in the design's own diagram and not of Pigment. An
  // empty stack -- every layer this build creates, and every layer any
  // `.npaint` written to date carries -- is skipped outright rather than run
  // as an identity, which is what keeps step 1's byte-identity regression
  // boundary exact.
  //
  // **Persisted as of PLAN.md Phase 5 step 5, and the reason that step had to
  // do it is this member.** An **Adjustment** layer holds no pixels at all --
  // no `rgbTiles`, no `pigmentTiles` -- so this stack is its entire content,
  // and a format that could not carry one would lose the whole layer on every
  // save (PRD I11, and docs/document-format.md's stated purpose). Step 3 could
  // afford to warn and drop, because a Pigment layer still had latents worth
  // writing; step 5 could not. io/OpSerial serialises a stack into the hex
  // `string` carrier docs/document-format.md itself names as the fix for its
  // dropped-blob problem, io/NpaintFile writes it as `np:ops` for **every**
  // kind that has one, and an entry a newer build wrote that this build cannot
  // interpret survives the round trip as an `OpClass::Unknown` op holding its
  // own bytes (PRD I10).
  OpStack ops;

  // The user-facing name. Deliberately NOT unique and deliberately not used
  // to identify anything: docs/document-format.md is explicit that "layer
  // names are not unique -- two layers may both be 'Layer 1' -- so the part
  // name is a stable synthetic id (`L0001`) and the user-facing name lives
  // in `np:name`". Empty means unnamed, which is what a layer created by
  // `Document::createBlank()` or `placeImageAsLayer()` gets: inventing a
  // default naming scheme ("Layer 1", "Background") is a UI decision that
  // belongs with the layer panel in Phase 5, not with the data member.
  std::string name;

  // The blend identity, as a name. See kDefaultBlendName above for the full
  // argument; the short version is that Phase 5's `core/Blend` owns the
  // enumeration and this member exists only to carry the value across a
  // save/load without touching it.
  std::string blend = kDefaultBlendName;

  // [0,1]. Not clamped here -- there is no mutator to clamp in, this being
  // a plain aggregate -- but io/NpaintFile refuses to save a value outside
  // that range by name rather than writing a number no reader can act on
  // (PRD I11).
  float opacity = 1.0f;

  bool visible = true;

  // **Clipped by the alpha of the layer below** (PRD C9, P0; PLAN.md Phase 5
  // step 9). One bool, because clipping is one bit of state per layer and
  // everything else about it is a property of where the layer sits.
  //
  // A bool rather than an index or a pointer to the base, and that is the one
  // decision this member makes. The base is *derived* -- it is the nearest
  // layer below that is not itself clipped -- so storing it would be storing a
  // second, invalidatable copy of the stack order, and every reorder would
  // have to fix it up. `Layer::parent` already carries a part *name* for
  // exactly the opposite reason (a group link is not derivable from position),
  // and the two are different relationships: a parent is membership, a clip is
  // adjacency. core/Composite's `clipRuns()` is the single place that turns
  // this bit plus the stack order into "which layer clips which".
  //
  // **A run of consecutive clipped layers clips to ONE base**, the nearest
  // non-clipped layer below the run -- they do not progressively erode each
  // other. core/Composite.hpp §12 derives it; it is stated here because a
  // reader of this member will otherwise assume the cumulative reading, which
  // is the single most common clipping-mask bug.
  //
  // **The bottom layer cannot be clipped** -- there is nothing below it.
  // `core::setLayerClipped()` refuses index 0 by name and with the numbers,
  // `core::moveLayer()` refuses a move that would put a clipped layer there,
  // and core/Composite composites a baseless clipped layer *unclipped* and
  // warns by name, because a file may still carry the flag (PRD I10) and a
  // flag must never be the thing that makes a layer's pixels vanish.
  //
  // Persisted as `np:clipped` (int, 0 or 1), written **only when true** so
  // that every `.npaint` this build wrote before this step keeps producing the
  // same bytes -- measured against HEAD, not assumed. See
  // docs/document-format.md.
  bool clipped = false;

  // Locked layers reject edits, to the exact extent core/LayerOps.hpp spells
  // out: its operations refuse to remove, move, rename or re-opacity a locked
  // layer, and deliberately still allow it to be hidden, unlocked and
  // duplicated. There is still no pixel-edit path to any layer -- a stroke
  // reaches sim::PaintSim's dense texture, never a Layer -- so "rejects edits"
  // cannot yet mean "the brush refuses"; that is stated rather than faked.
  bool locked = false;

  // The EXR *part* name (`L0002`) of the group this layer belongs to, or
  // empty for a top-level layer. docs/document-format.md:
  // "Groups have no native concept. A group is a part with no image channels
  // and `np:kind='group'`; members carry `np:parent` naming it."
  //
  // A part name rather than a Layer index, because that is what the format
  // stores and because an index would be invalidated by every reorder. This
  // build creates no groups (there is no `LayerKind::Group`, and CONTEXT.md's
  // seven kinds do not include one), so this is always empty in a document
  // this build authored -- but a document authored by a build that *does*
  // have groups round-trips its parent links through here untouched.
  std::string parent;

  // **A stable identity for this layer within its document** (PLAN.md Phase 5
  // step 12, layer comps; PRD C14). Monotonic within one `Document`, never
  // reused, and **not** an index.
  //
  // It exists for one reason: a layer comp is a set of per-layer states that
  // has to survive the stack changing underneath it. app/HistoryPanel.hpp
  // section (b) states the identical hazard for history rows -- "eviction
  // shifts every index down by one, which would silently repoint a panel row
  // ... at a different state" -- and a comp keyed by index has it three times
  // over, because a delete, an add *and* a reorder each move every index above
  // them. A comp restored through indices onto a stack one layer shorter would
  // apply every state to the wrong layer, silently, and produce a picture that
  // is wrong without looking wrong. So a comp entry stores this number, and
  // `core::restoreLayerComp()` reports or refuses rather than guessing when it
  // cannot find it.
  //
  // **0 means "not yet assigned", and it is what every layer this build
  // creates starts with.** Ids are handed out lazily by
  // `core::normalizeLayerIds()`, which `captureLayerComp()` calls and nothing
  // else in the running application does -- so a document that never uses a
  // comp carries zeros here for its whole life and nothing about it changes.
  // That is deliberate: it keeps the id off every code path that predates
  // comps, and it is what makes "a document with no comps saves byte-identically
  // to before this feature existed" structural rather than careful --
  // io/NpaintFile writes ids only inside `np:comps`, and only when there are
  // comps to write.
  //
  // **Not the EXR part name** (`L0003`), which is the *format's* stable id. The
  // two are different: a part name exists only once a layer has been through a
  // save, and `NpaintCarry::layerPartNames` is where it lives because `core/`
  // knows nothing about EXR. `np:comps` carries the mapping between the two so
  // the identity survives a round trip; core/LayerComp.hpp gives the argument.
  //
  // `core::duplicateLayer()` resets the copy's to 0. Two layers sharing one
  // nonzero id is the single state that would make a restore ambiguous, and
  // `restoreLayerComp()` refuses it by name with the numbers rather than
  // picking one of them.
  uint64_t id = 0;

  // **The colour label** (PRD C15, P2; PLAN.md Phase 5 step 11). A *name*, not
  // an enum, for `blend`'s reason -- see `kLayerColorLabelNames` above.
  // `kNoLayerColorLabel` (empty) means unlabelled, which is what every layer
  // this build has ever created carries and what makes the attribute absent
  // from every `.npaint` written before this step.
  //
  // A label is metadata a user sorts by and nothing composites through: it does
  // not reach core/Composite, it is not in a layer comp (core/LayerComp.hpp's
  // boundary is "a property clicking a comp silently overwrites", and a label
  // is organisation rather than appearance), and it is deliberately settable on
  // a **locked** layer -- labelling is how a user marks a layer they have
  // finished and locked, so a lock that froze the label would fight its own
  // most common use.
  std::string colorLabel = kNoLayerColorLabel;

  // **Link group membership** (PRD C15's "linking"; PLAN.md Phase 5 step 11).
  // 0 means unlinked. Two layers are linked when they carry the same non-zero
  // value.
  //
  // A group *id* rather than a list of partner ids, and the difference is what
  // makes a link symmetric for free: with a list, linking A to B is two writes
  // that can disagree, and every delete has to walk every other layer's list to
  // repair it. With a shared number, membership is a single field and the
  // relation cannot become one-sided.
  //
  // **Links are resolved, never repaired: a group with fewer than two live
  // members is not a link.** That is this member's answer to "what happens to a
  // partner's link when a linked layer is deleted", and it is the reason
  // nothing in core/LayerOps had to grow a cleanup pass. A survivor keeps its
  // number; `core::linkedLayers()` is the single place that turns a number into
  // a set, it counts members before it reports one, and a group of one behaves
  // in every way like no link at all. Undo restores the deleted partner and the
  // link with it, because the number was never destroyed. Eager cleanup would
  // have had to run inside `removeLayer()` -- a mutation of layers the user did
  // not name, on the one path every other delete in the codebase goes through.
  //
  // **Not a `Layer::id`.** Ids are handed out lazily and are 0 for every layer
  // until a comp is captured (see `id` above), so keying links off them would
  // force id assignment onto every document that ever links two layers and
  // destroy step 12's "a document that never uses a comp carries zeros"
  // property. Group numbers come from `core::nextLinkGroupId()`, which is
  // one above the highest value present -- safe here and *not* safe for `id`,
  // because nothing outside `Document::layers` ever names a link group, so a
  // number can never be re-issued to something that still refers to it.
  //
  // Persisted as `np:link` (int), written **only when non-zero**, the rule
  // `np:clipped` already uses.
  uint64_t linkGroup = 0;
};

}  // namespace np
