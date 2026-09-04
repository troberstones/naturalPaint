#include "app/LayerPanel.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

#include "core/Blend.hpp"

namespace np {

size_t layerIndexForPanelRow(size_t row, size_t layerCount) noexcept {
  if (layerCount == 0 || row >= layerCount) return 0;
  return layerCount - 1 - row;
}

size_t panelRowForLayerIndex(size_t layerIndex, size_t layerCount) noexcept {
  if (layerCount == 0 || layerIndex >= layerCount) return 0;
  return layerCount - 1 - layerIndex;
}

size_t layerDropTargetIndex(size_t hoveredIndex, bool droppedAboveMidpoint,
                            size_t layerCount) noexcept {
  if (layerCount == 0) return 0;
  const size_t target = hoveredIndex + (droppedAboveMidpoint ? 1 : 0);
  return target < layerCount ? target : layerCount - 1;
}

const char* layerKindGlyph(LayerKind kind) noexcept {
  switch (kind) {
    case LayerKind::Pigment: return "\xE2\x97\x89";     // U+25C9 fisheye
    case LayerKind::RGB: return "\xE2\x96\xA1";         // U+25A1 white square
    case LayerKind::Media: return "\xE2\x97\x88";       // U+25C8 white diamond, black small
    case LayerKind::Strokes: return "\xE2\x9C\x82";     // U+2702 scissors
    case LayerKind::Adjustment: return "\xE2\x96\xA4";  // U+25A4 square, horizontal fill
    case LayerKind::Text: return "T";
    case LayerKind::Flats: return "\xE2\x96\xA9";       // U+25A9 square, orthogonal fill
    // ASCII, `Text`'s own choice: a Group is not one of the design's seven
    // rows (it is created by the `Layer > Group` gesture, not the `NEW`
    // popup -- `newLayerKindMenu()` below still lists exactly seven), so this
    // glyph has no design mock to match and no reason to cost a font-merge
    // codepoint the way the other new-since-1a marks do.
    case LayerKind::Group: return "G";
    // ASCII for `Group`'s stated reason: a Vector layer is not one of design
    // 2a's seven rows, so there is no mock to match and no reason to spend a
    // font-merge codepoint (ui/Fonts.cpp's merge list is exactly those seven).
    case LayerKind::Vector: return "V";
  }
  return "?";
}

uint32_t layerKindRailRgb(LayerKind kind) noexcept {
  // The design's own seven, read straight off option 2a's markup -- both the
  // row rails and the `NEW` popup swatches, which carry the same value per kind
  // there and so do here. Muted, and separated in hue rather than in value, for
  // the reason `layerColorLabelSwatch()` gives about its own chips: at 3 px
  // wide two colours that differ only in lightness read as one colour.
  switch (kind) {
    case LayerKind::Pigment: return 0xc2553d;     // warm red-earth
    case LayerKind::RGB: return 0x4a6f8f;         // slate blue
    case LayerKind::Media: return 0x3f7d6a;       // green
    case LayerKind::Strokes: return 0x7a6aa8;     // violet
    case LayerKind::Adjustment: return 0x8a7a3e;  // ochre
    case LayerKind::Text: return 0x6f6f6f;        // neutral grey
    case LayerKind::Flats: return 0xa05a7a;       // mauve
    // Not one of the design's seven rails (see `layerKindGlyph()`'s own
    // note); distinct from all seven anyway, cheaply, so a future row for it
    // is not stuck picking a colour under time pressure.
    case LayerKind::Group: return 0x5a8a5a;       // moss green
    // Distinct from all eight above in hue, not merely in value -- see this
    // switch's own note on why 3 px of rail cannot carry a lightness step.
    case LayerKind::Vector: return 0x3aa0b8;      // cyan-teal
  }
  // Unreachable while every enumerator is covered, which `-Wswitch` and
  // `--selftest` both check. A grey rather than a colour, so a kind added
  // without a rail reads as "no rail" instead of impersonating one of the seven.
  return 0x444141;
}

const std::vector<NewLayerKindEntry>& newLayerKindMenu() {
  // Design 2a's popup order, which is not the enum's: Pigment first because
  // it is the default kind (PRD principle 3) and the design draws it in the
  // highlighted slot, then RGB, then the five parametric kinds, then Vector
  // (which the design predates). The three with no maker function are listed
  // with `buildable == false` -- see the header for why they are listed at
  // all.
  static const std::vector<NewLayerKindEntry> kMenu = {
      {LayerKind::Pigment, true, LayerCommand::NewPigmentLayer},
      {LayerKind::RGB, true, LayerCommand::NewRgbLayer},
      {LayerKind::Media, false, {}},
      {LayerKind::Adjustment, true, LayerCommand::NewAdjustmentLayer},
      {LayerKind::Strokes, false, {}},
      // Buildable as of PLAN.md phase 14. Flipped IN PLACE rather than
      // appended the way Vector was: Text is one of design 2a's own seven
      // kinds and has been in this list since it existed, so its slot is
      // already the design's and moving it is what would break the ordering a
      // --selftest pins.
      {LayerKind::Text, true, LayerCommand::NewTextLayer},
      // Buildable as of PLAN.md phase 16 (ADR-0009), flipped in place for
      // Text's reason above.
      {LayerKind::Flats, true, LayerCommand::NewFlatsLayer},
      // Buildable as of PLAN.md phase 13: an empty Vector layer is a real,
      // saveable, editable thing, unlike the four above it. Appended rather
      // than slotted among the parametric kinds because design 2a's popup
      // order predates it and reordering that list would move the four rows a
      // --selftest already pins by position.
      {LayerKind::Vector, true, LayerCommand::NewVectorLayer},
  };
  return kMenu;
}

const char* layerKindUnbuildableReason(LayerKind kind) noexcept {
  // core/Layer.hpp's and core/Merge.cpp's own words, one sentence each. Not a
  // generic "not built yet": which *piece* is missing differs per kind, and the
  // difference is the whole information a reader of a greyed row wants.
  switch (kind) {
    case LayerKind::Pigment:
    case LayerKind::RGB:
    case LayerKind::Adjustment:
    case LayerKind::Vector:
    case LayerKind::Text:
    case LayerKind::Flats:
      return nullptr;
    case LayerKind::Media:
      return "Not built yet. A Media layer needs the fluid solver's own per-medium state on "
             "top of the pigment tiles, and nothing on Layer holds it.";
    case LayerKind::Strokes:
      return "Not built yet. A Strokes layer here has no dabs: the kind has no parameter "
             "member to hold them.";
    case LayerKind::Group:
      // Not "unbuildable" in the sense the other four are -- a Group is real
      // and fully built. It simply is not one of `newLayerKindMenu()`'s seven
      // entries, so this arm exists only to keep the switch exhaustive; no
      // caller reaches it through that menu.
      return "Not offered here -- a Group is created from a multi-selection via Layer > "
             "Group, not from the New Layer popup.";
  }
  return nullptr;
}

bool newLayerShortcutsExist() {
  // A constant, deliberately, and the header says why it is a function at all:
  // it is the assertion site for a piece of the design that is absent. This
  // returns true on the day `keymaps/default.json` binds a layer action, and
  // the `--selftest` line that pins it is what will send that revision back
  // here to draw the shortcut column 2a asks for.
  return false;
}

std::string layerLinkBadgeText(const Document& doc, size_t layerIndex) {
  const size_t partners = layerLinkPartnerCount(doc, layerIndex);
  if (partners == 0) return std::string();
  return "LINKED+" + std::to_string(partners);
}

std::string layerKindFilterLabel(const std::optional<LayerKind>& kind) {
  std::string s = "KIND: ";
  s += kind.has_value() ? layerKindName(*kind) : "ALL";
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  return s;
}

std::string layerPanelCountLabel(size_t shown, size_t total) {
  if (shown >= total) return std::to_string(total);
  return std::to_string(shown) + "/" + std::to_string(total);
}

std::string layerRowTitle(const Layer& layer, size_t layerIndex) {
  if (!layer.name.empty()) return layer.name;
  return "Layer " + std::to_string(layerIndex + 1);
}

std::string layerRowSubLine(const Layer& layer) {
  // U+00B7 MIDDLE DOT, docs/ui.md's own separator.
  static constexpr const char* kSep = " \xC2\xB7 ";

  std::string s = layerKindName(layer.kind);
  for (char& c : s)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

  s += kSep;
  std::string blend = layer.blend;
  for (char& c : blend)
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  s += blend.empty() ? "?" : blend;
  if (!blendIsImplemented(layer.blend)) s += " (!)";
  // PRD B7, on the row as well as in the dropdown, and from the same field
  // (`BlendModeInfo::space`) rather than from a second list here. A row is
  // where a user reads what a layer does, so a display-referred mode that was
  // labelled only while the dropdown happened to be open would not be
  // "labelled as such" in any useful sense.
  if (const std::optional<BlendMode> mode = blendModeFromName(layer.blend))
    if (blendModeInfo(*mode).space == BlendSpace::DisplayReferred) s += " (display-referred)";

  // Whole percent, rounded to nearest with ties going up. An opacity outside [0,1]
  // cannot be set through core/LayerOps and cannot be saved, but it can exist
  // in memory (Layer is a plain aggregate), so this prints what is there rather
  // than what ought to be -- a row that showed "100%" for a stored 1.5 would
  // hide the very thing a reader is looking at the panel to find.
  char pct[32];
  std::snprintf(pct, sizeof(pct), "%.0f%%",
                std::floor(static_cast<double>(layer.opacity) * 100.0 + 0.5));
  s += kSep;
  s += pct;

  // docs/ui.md §3.2's own example row is `STROKES · NORMAL · MASK`, so the
  // vocabulary is the document's rather than invented here. Present exactly
  // when `Layer::mask` is engaged, which is the one thing that distinguishes a
  // layer with a reveal-all mask from a layer with none -- they composite
  // byte-identically (core/Mask.hpp), so if the row did not say it, nothing a
  // user can see would. It says nothing about what the mask *contains*: a
  // thumbnail is the panel's job and there is no way to paint one yet.
  // A non-empty op stack (PLAN.md Phase 5 step 5). Present for every kind that
  // can carry one, not only Adjustment -- `Layer::ops` is a property of a
  // Layer in DESIGN-imaging.md's own diagram -- but it is on an Adjustment
  // layer that it is the *only* thing the row could say about the layer's
  // content, since that kind holds no pixels at all. Absent for an empty
  // stack, which is every layer any `.npaint` written before that step
  // carries, so no existing row changes.
  //
  // The count and not the op names: docs/ui.md §3.2's rows are one short
  // monospace line, and a stack of five would not fit. The names belong in the
  // op-stack UI Phase 3 step 8 built for `app::AppState::opStack`, which
  // nothing yet points at a Layer.
  if (layer.ops.size() > 0) {
    s += kSep;
    s += std::to_string(layer.ops.size());
    s += layer.ops.size() == 1 ? " OP" : " OPS";
  }
  if (layer.mask.has_value()) {
    s += kSep;
    s += "MASK";
  }
  // docs/ui.md §3.2's own example row is `ADJUSTMENT · CLIPPED`, so this
  // vocabulary is the document's rather than invented here -- the UI assumed
  // the feature before PLAN.md Phase 5 step 9 built it.
  //
  // It says what the layer *asks for*, not whether the ask can be honoured,
  // and that is deliberate: a `Layer` alone does not know where it sits, and
  // this function takes only a `Layer` (the same reason `blendIsImplemented()`
  // and not `blendIsImplementedForLayer()` is what marks a blend with `(!)`
  // here). A clipped layer with nothing to clip to is reported where the
  // question can actually be answered -- by the compositor, through
  // `core::clippedLayerWithoutBaseWarning()`, at every boundary that writes a
  // file. `core::setLayerClipped()` refuses to create the state in the first
  // place, so the only way to see the marker without a base is a document that
  // arrived carrying one (PRD I10).
  if (layer.clipped) {
    s += kSep;
    s += "CLIPPED";
  }
  if (!layer.visible) {
    s += kSep;
    s += "HIDDEN";
  }
  if (layer.locked) {
    s += kSep;
    s += "LOCKED";
  }
  // Alpha lock, its own word rather than folded into "LOCKED" -- the two are
  // different promises (core/Layer.hpp's own comment on `alphaLocked`: one
  // freezes everything, the other freezes only alpha and leaves painting
  // free), and a row that said "LOCKED" for both would tell a user who can
  // still paint that they cannot.
  if (layer.alphaLocked) {
    s += kSep;
    s += "ALPHA LOCK";
  }
  // The colour label (PLAN.md Phase 5 step 11, PRD C15), last because it is
  // organisation rather than anything about how the layer composites. Absent
  // for an unlabelled layer -- which is every layer this build created before
  // this step -- so no existing row's text changes by a character. Upper-cased
  // **as carried**, exactly as the blend name is, so a label this build has no
  // swatch for still reads as itself.
  if (!layer.colorLabel.empty()) {
    s += kSep;
    for (const char c : layer.colorLabel)
      s += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return s;
}

std::vector<BlendMode> blendMenuForLayer(const Document& doc, size_t layerIndex) {
  std::vector<BlendMode> out;
  if (layerIndex >= doc.layers.size()) return out;
  for (const BlendModeInfo& info : allBlendModes())
    if (blendModeAvailableForLayer(doc, layerIndex, info.mode)) out.push_back(info.mode);
  return out;
}

std::string blendMenuEntryText(BlendMode mode) {
  const BlendModeInfo& info = blendModeInfo(mode);
  std::string s = info.label;
  // PRD B7, read straight off the data. Two spaces rather than one so the
  // marker reads as an annotation on the label and not as part of it.
  if (info.space == BlendSpace::DisplayReferred) s += "  (display-referred)";
  // The "(not composited yet)" marker step 2 put here for `Mix` is gone,
  // because as of PLAN.md Phase 5 step 3 `Mix` is composited -- and it is
  // composited in exactly the situation this menu offers it in, since
  // `blendMenuForLayer()` filters through the same PRD L5 predicate
  // (`blendModeAvailableForLayer()`) that decides whether a mix can be
  // formed. So a mode reachable from this menu is a mode that works; the
  // marker would now be a warning about nothing.
  //
  // The mechanism it belonged to is not gone with it: `layerRowSubLine()`
  // still marks a layer whose carried `np:blend` this build cannot honour
  // with `(!)`, which is the case that genuinely still exists -- a newer
  // build's name, arriving from a file, which the dropdown never offers.
  if (!info.compositesPixels && !info.compositesLatents) s += "  (not composited yet)";
  return s;
}

size_t blendMenuSelection(const Document& doc, size_t layerIndex,
                          const std::vector<BlendMode>& menu) {
  if (layerIndex >= doc.layers.size()) return menu.size();
  const std::string& carried = doc.layers[layerIndex].blend;
  for (size_t i = 0; i < menu.size(); ++i)
    if (carried == blendModeName(menu[i])) return i;
  return menu.size();
}

// --- Colour labels, links and filtering -----------------------------------

std::optional<LayerLabelSwatch> layerColorLabelSwatch(std::string_view name) {
  // Deliberately not derived from ui/Theme.hpp's palette: these are the seven
  // label colours, which have to stay distinguishable from each other as chips
  // a few pixels wide, and the theme's greys would give two of them the same
  // apparent value. Muted rather than saturated so a column of chips does not
  // out-shout the rows they annotate.
  if (name == "red") return LayerLabelSwatch{0.83f, 0.29f, 0.27f};
  if (name == "orange") return LayerLabelSwatch{0.87f, 0.53f, 0.20f};
  if (name == "yellow") return LayerLabelSwatch{0.85f, 0.76f, 0.24f};
  if (name == "green") return LayerLabelSwatch{0.36f, 0.68f, 0.35f};
  if (name == "blue") return LayerLabelSwatch{0.28f, 0.51f, 0.82f};
  if (name == "violet") return LayerLabelSwatch{0.56f, 0.38f, 0.78f};
  if (name == "grey") return LayerLabelSwatch{0.55f, 0.55f, 0.55f};
  return std::nullopt;
}

size_t layerLinkPartnerCount(const Document& doc, size_t layerIndex) {
  const std::vector<size_t> members = linkedLayers(doc, layerIndex);
  return members.size() > 1 ? members.size() - 1 : 0;
}

namespace {

// ASCII case folding only, which is exactly what a substring test over layer
// names can promise here: there is no locale in this build and no Unicode
// case-folding table, so pretending to fold a non-ASCII name would be the kind
// of half-kept promise this codebase writes down rather than ships.
std::string asciiLower(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

}  // namespace

bool layerMatchesFilter(const Document& doc, size_t layerIndex, const LayerFilter& filter) {
  if (layerIndex >= doc.layers.size()) return false;
  const Layer& layer = doc.layers[layerIndex];
  if (filter.kind.has_value() && layer.kind != *filter.kind) return false;
  if (!filter.text.empty()) {
    // Matched against the row's displayed title, not the stored name -- see the
    // header. An unnamed layer's row says "Layer 3", and that is the only text
    // a user has to search by.
    if (asciiLower(layerRowTitle(layer, layerIndex)).find(asciiLower(filter.text)) ==
        std::string::npos)
      return false;
  }
  return true;
}

std::vector<size_t> layersMatchingFilter(const Document& doc, const LayerFilter& filter) {
  std::vector<size_t> out;
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (layerMatchesFilter(doc, i, filter)) out.push_back(i);
  return out;
}

LayerSelection restrictSelectionToFilter(const Document& doc, const LayerSelection& sel,
                                         const LayerFilter& filter) {
  if (!filter.active()) return sel;
  std::vector<size_t> kept;
  for (const size_t index : sel.indices)
    if (layerMatchesFilter(doc, index, filter)) kept.push_back(index);
  return makeLayerSelection(std::move(kept));
}

size_t layersHiddenFromSelection(const Document& doc, const LayerSelection& sel,
                                 const LayerFilter& filter) {
  return sel.size() - restrictSelectionToFilter(doc, sel, filter).size();
}

// --- Group nesting -----------------------------------------------------------

std::vector<std::string> layerGroupAncestry(const Document& doc, size_t layerIndex) noexcept {
  std::vector<std::string> chain;
  if (layerIndex >= doc.layers.size()) return chain;
  std::string tag = doc.layers[layerIndex].parent;
  // Bounded exactly as core::groupAncestry(): never more hops than there are
  // layers, and a tag already on the chain stops the walk -- the identical
  // cycle guard, applied here to a chain of tags instead of to a coverage
  // product.
  for (size_t hops = 0; !tag.empty() && hops < doc.layers.size(); ++hops) {
    bool seen = false;
    for (const std::string& t : chain)
      if (t == tag) seen = true;
    if (seen) break;
    chain.push_back(tag);
    std::string next;
    for (const Layer& l : doc.layers) {
      if (l.kind == LayerKind::Group && l.groupTag == tag) {
        next = l.parent;
        break;
      }
    }
    tag = next;
  }
  return chain;
}

size_t layerGroupDepth(const Document& doc, size_t layerIndex) noexcept {
  return layerGroupAncestry(doc, layerIndex).size();
}

bool layerHiddenByCollapsedGroup(const Document& doc, size_t layerIndex,
                                 const std::set<std::string>& collapsedGroupTags) noexcept {
  if (collapsedGroupTags.empty()) return false;
  for (const std::string& tag : layerGroupAncestry(doc, layerIndex))
    if (collapsedGroupTags.count(tag) != 0) return true;
  return false;
}

}  // namespace np
