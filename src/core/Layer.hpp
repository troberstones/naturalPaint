#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/TileStore.hpp"

// core/Layer (PLAN.md "Phase 2 -- See a file", step 4; CONTEXT.md "Layer
// kinds"). "Design for N, ship 1": all seven kinds CONTEXT.md names are
// constructible today -- the enum below has a value for each -- but only
// `RGB` actually owns pixel storage. The other six (Pigment, Media, Strokes,
// Adjustment, Text, Flats) are inert placeholders nothing exercises yet:
// Pigment/Media need the Kubelka-Munk solver's own tile shape (Phase 5+, see
// Layer::rgbTiles below for why that isn't just "reuse core::Tile"), and
// Adjustment/Text/Strokes/Flats "hold no pixels of their own" per CONTEXT.md
// and structurally never will -- they'll eventually gain their own
// parameter-only members (an op stack, a string+font, a Dab list, ...), not
// tile storage.
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
//   parent   still carried, still never acted on. Honouring a group link
//            means compositing a group's members offscreen first, which is
//            Phase 5 step 9's machinery, and this build creates no groups.
struct Layer {
  // CONTEXT.md: Pigment is "the default kind for a new layer" -- the
  // eventual domain default once Pigment layers are real, kept as the
  // default here even though nothing makes a Pigment layer functional yet,
  // so this field's meaning doesn't have to change when Phase 5 lands it.
  LayerKind kind = LayerKind::Pigment;

  // Pixel storage for the one kind that's real today (RGB: "pixels are
  // Working space RGBA", CONTEXT.md). Populated only when `kind == RGB`;
  // std::nullopt for every other kind, including Pigment/Media (not real
  // yet) and Adjustment/Text/Strokes/Flats (never will hold pixels).
  //
  // Deliberately std::optional<TileStore> rather than a mandatory, always-
  // present TileStore member. Two reasons: (1) most layer kinds hold no
  // pixels at all, so a mandatory member would carry TileStore's (currently
  // empty, but not necessarily forever) bookkeeping for kinds that never
  // populate it; (2) more importantly, DESIGN-imaging.md §2's own memory
  // table gives a future Pigment/Media tile a *different* shape -- 7
  // channels (c0/c1/c2/mass plus a 3-channel residual) against RGB's 4-
  // channel rgba16float, i.e. not a core::TileStore<core::Tile> at all.
  // Hard-wiring this field to today's core::Tile would make it awkward for
  // whoever adds Pigment/Media layers for real (Phase 5) to give them their
  // own, differently-shaped tile storage. The natural extension at that
  // point is a second, similarly-optional member (or a variant) holding the
  // pigment tile type, populated for Pigment/Media instead of rgbTiles --
  // not a change to this field's type.
  std::optional<TileStore> rgbTiles;

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
};

}  // namespace np
