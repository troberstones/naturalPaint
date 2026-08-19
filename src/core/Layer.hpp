#pragma once

#include <optional>
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

// One entry in a Document's layer list (CONTEXT.md Relationships: "A Layer
// holds Tiles; tiles are allocated only where content exists").
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
};

}  // namespace np
