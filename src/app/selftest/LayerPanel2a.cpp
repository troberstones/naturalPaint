#include "app/selftest/Support.hpp"

#include <set>

#include "ui/AtelierTheme.hpp"

namespace np {

// The LAYERS panel as design "naturalPaint Panels" turn 2, option 2a
// specifies it -- the pure half, which is nearly all of it.
//
// **This panel is mostly string assembly and table lookup, and a metadata line
// built inline in an ImGui call is a line nothing can read.** So the rail
// colours, the `NEW` popup's seven entries, the link badge, the filter chip and
// the count label all live in app/LayerPanel, and this section is what proves
// they say what 2a says they say.
//
// The other half of what it exists for is the *omissions*. Three pieces of the
// design are deliberately not drawn because nothing in the model can supply
// them -- a Media layer's drying countdown, a Flats layer's fill count, and the
// `NEW` popup's keyboard shortcuts. An omission is exactly the kind of decision
// that gets quietly reversed by a later revision reaching for a plausible
// number, so each one is pinned here by an assertion that fails the moment the
// number appears, with the reason next to it.
bool runLayerPanel2aTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
  };
  // U+00B7 MIDDLE DOT with its spaces, docs/ui.md's own separator, written once
  // here so the expected strings below read as the design's rows do.
  static constexpr const char* kSep = " \xC2\xB7 ";
  const std::vector<LayerKind> kAllKinds = {LayerKind::Pigment,    LayerKind::RGB,
                                            LayerKind::Media,      LayerKind::Strokes,
                                            LayerKind::Adjustment, LayerKind::Text,
                                            LayerKind::Flats,
                                            // PLAN.md phase 13. Group is
                                            // still absent on purpose -- it is
                                            // created by the Layer > Group
                                            // gesture, never by this popup.
                                            LayerKind::Vector};

  std::printf("  -- A. the kind rail: complete, and no two kinds the same --\n");

  // 2a "carries 1a's kind rail forward", and 1a's whole argument for spending
  // colour on kind is that the rail is what makes a kind readable at 322 px.
  // The failure that would destroy that is two kinds sharing a colour -- and it
  // is invisible in any screenshot of a stack that does not happen to contain
  // both of them, which is most stacks. Nothing but an exhaustive check finds
  // it.
  {
    std::set<uint32_t> seen;
    bool distinct = true;
    bool namedFallback = false;
    for (const LayerKind kind : kAllKinds) {
      const uint32_t rail = layerKindRailRgb(kind);
      if (!seen.insert(rail).second) distinct = false;
      // `layerKindRailRgb()`'s unreachable arm returns the divider grey, so a
      // kind added to core/Layer.hpp without a rail lands there rather than
      // impersonating one of the seven. Reaching it from a real kind means the
      // switch has a hole.
      if (rail == kDivider) namedFallback = true;
    }
    check(seen.size() == kAllKinds.size() && distinct,
          "rail: all seven kinds get a DISTINCT colour -- two kinds drawn the same is the "
          "one failure that makes the rail worse than no rail");
    check(!namedFallback,
          "rail: no real kind falls through to the divider grey -- that arm exists for a "
          "kind added without a rail, and must stay unreached");
  }

  // A rail is 3 px wide and sits on the row's own fill, so a rail equal to
  // either fill is a rail nobody can see. Both fills are checked, because a row
  // is drawn on the chrome base when it is not selected and on the selected
  // wash when it is, and a rail that vanished only on the selected row would be
  // a bug that appears when you click.
  {
    bool visibleOnBoth = true;
    for (const LayerKind kind : kAllKinds) {
      const uint32_t rail = layerKindRailRgb(kind);
      if (rail == kChromeBase || rail == kRowSelected) visibleOnBoth = false;
    }
    check(visibleOnBoth,
          "rail: no rail equals the chrome base or the selected-row wash -- a rail the "
          "colour of the row it marks is not drawn at all");
  }

  std::printf("  -- B. the NEW popup: eight kinds, five of them buildable --\n");

  // 2a's second change: "the three `New` buttons collapse into one `NEW` with a
  // kind popup carrying all seven kinds and their rails". All seven, so the
  // popup is where a reader learns the product has seven layer kinds -- and
  // exactly the three `core/LayerOps` has a maker function for are live.
  {
    const std::vector<NewLayerKindEntry>& menu = newLayerKindMenu();
    std::set<int> kinds;
    for (const NewLayerKindEntry& e : menu) kinds.insert(static_cast<int>(e.kind));
    check(menu.size() == kAllKinds.size() && kinds.size() == kAllKinds.size(),
          "new: the popup lists every popup kind exactly once -- a kind listed twice would "
          "offer two buttons for one gesture, one omitted would hide a kind");

    size_t buildable = 0;
    bool correctSet = true;
    bool reasonsMatch = true;
    for (const NewLayerKindEntry& e : menu) {
      const bool expected = e.kind == LayerKind::Pigment || e.kind == LayerKind::RGB ||
                            e.kind == LayerKind::Adjustment || e.kind == LayerKind::Vector ||
                            e.kind == LayerKind::Text;
      if (e.buildable != expected) correctSet = false;
      if (e.buildable) ++buildable;
      // A disabled entry must SAY why, and a live one must not carry an excuse
      // that would then be shown for a kind that works.
      const char* reason = layerKindUnbuildableReason(e.kind);
      if (e.buildable != (reason == nullptr)) reasonsMatch = false;
    }
    check(buildable == 5 && correctSet,
          "new: exactly Pigment, RGB, Adjustment, Vector and Text are buildable -- core/LayerOps "
          "has five maker functions and Media/Strokes/Flats hold no content at all");
    check(reasonsMatch,
          "new: every disabled kind carries a reason and every live one carries none -- a "
          "greyed row that cannot say why is indistinguishable from a broken button");

    // The transposition nothing on screen would show: a popup row labelled
    // "Pigment" wired to `NewRgbLayer` makes an RGB layer with a Pigment
    // heading, and the picture that results looks like a compositing bug rather
    // than a menu bug.
    bool wiredRight = true;
    for (const NewLayerKindEntry& e : menu) {
      if (!e.buildable) continue;
      if (e.kind == LayerKind::Pigment && e.command != LayerCommand::NewPigmentLayer)
        wiredRight = false;
      if (e.kind == LayerKind::RGB && e.command != LayerCommand::NewRgbLayer) wiredRight = false;
      if (e.kind == LayerKind::Adjustment && e.command != LayerCommand::NewAdjustmentLayer)
        wiredRight = false;
      if (e.kind == LayerKind::Vector && e.command != LayerCommand::NewVectorLayer)
        wiredRight = false;
      if (e.kind == LayerKind::Text && e.command != LayerCommand::NewTextLayer) wiredRight = false;
    }
    check(wiredRight,
          "new: each buildable entry issues ITS OWN kind's command -- a transposed pair "
          "makes the wrong kind of layer and looks like a compositing fault");

    check(!menu.empty() && menu.front().kind == LayerKind::Pigment,
          "new: Pigment leads the popup -- PRD principle 3 (\"Pigment by default\") and the "
          "design's own highlighted slot");

    // The popup draws each kind's rail AND its glyph, so it is the one place
    // both tables are exercised for every kind at once -- including the four a
    // row can only show for a document that arrived carrying them, which is
    // exactly the case a hand-built fixture forgets to cover.
    std::set<uint32_t> popupRails;
    bool everyEntryDrawable = true;
    for (const NewLayerKindEntry& e : menu) {
      popupRails.insert(layerKindRailRgb(e.kind));
      if (std::string(layerKindGlyph(e.kind)) == "?") everyEntryDrawable = false;
    }
    check(everyEntryDrawable && popupRails.size() == menu.size(),
          "new: every popup entry has a real glyph and its own rail -- the popup is the one "
          "place all seven of each are drawn together, so a gap shows up here first");
  }

  std::printf("  -- C. the shortcut column the design draws and this build does not --\n");

  // The design's popup carries `SHIFT-CMD-N` beside Pigment and `SHIFT-CMD-R`
  // beside RGB. Neither key does anything here, and `CMD-N` is bound to
  // `clear_canvas` -- so drawing them would name a key that wipes the canvas
  // as the way to make a Pigment layer.
  //
  // This is checked against the shipped keymap and not only against the
  // constant, so it is the *binding* that is being asserted absent. The day
  // someone adds one, this line fails and points at the piece of 2a that should
  // then be drawn.
  {
    Keymap km;
    const bool loaded = km.loadFromFile("default.json");
    bool anyLayerAction = false;
    for (const KeyBinding& b : km.bindings()) {
      std::string action = b.action;
      for (char& c : action)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (action.find("layer") != std::string::npos) anyLayerAction = true;
    }
    check(loaded && !anyLayerAction && !newLayerShortcutsExist(),
          "shortcuts: keymaps/default.json binds NO layer action, so the NEW popup draws no "
          "shortcut column -- wiring one must bring the column with it");
  }

  std::printf("  -- D. the metadata line is the design's, for the rows the model has --\n");

  // Six of the design's eight example rows, character for character. Every one
  // of these comes out of `layerRowSubLine()` unchanged -- this section adds no
  // second way to build the line, which is the whole reason it can assert the
  // design's strings at all.
  {
    Layer pigment;
    pigment.kind = LayerKind::Pigment;
    pigment.blend = "multiply";
    pigment.opacity = 0.72f;
    check(layerRowSubLine(pigment) == std::string("PIGMENT") + kSep + "MULTIPLY" + kSep + "72%",
          "row: `PIGMENT - MULTIPLY - 72%` -- the design's own first row, exactly");

    Layer rgb;
    rgb.kind = LayerKind::RGB;
    check(layerRowSubLine(rgb) == std::string("RGB") + kSep + "NORMAL" + kSep + "100%",
          "row: `RGB - NORMAL - 100%`");

    Layer strokes;
    strokes.kind = LayerKind::Strokes;
    strokes.mask = MaskTileStore{};
    check(layerRowSubLine(strokes) ==
              std::string("STROKES") + kSep + "NORMAL" + kSep + "100%" + kSep + "MASK",
          "row: `STROKES - NORMAL - 100% - MASK` -- the marker survives the line being "
          "clipped at 322 px because the row also draws a mask chip");

    Layer adjust;
    adjust.kind = LayerKind::Adjustment;
    adjust.clipped = true;
    adjust.ops.add(makeNewOp(PointOpKind::Curves));
    adjust.ops.add(makeNewOp(PointOpKind::Exposure));
    check(layerRowSubLine(adjust) == std::string("ADJUSTMENT") + kSep + "NORMAL" + kSep +
                                         "100%" + kSep + "2 OPS" + kSep + "CLIPPED",
          "row: `ADJUSTMENT - NORMAL - 100% - 2 OPS - CLIPPED` -- the design's worst case, "
          "which is what the row's clip indent and text clipping are sized for");

    Layer text;
    text.kind = LayerKind::Text;
    check(layerRowSubLine(text) == std::string("TEXT") + kSep + "NORMAL" + kSep + "100%",
          "row: `TEXT - NORMAL - 100%`");
  }

  std::printf("  -- E. `(!)` marks the modes that are not composited, and only those --\n");

  // The design's eighth row is `RGB - DISSOLVE (!) - 100%`, where `(!)` says
  // this build cannot composite the mode the file carries (PRD I10 keeps the
  // name rather than dropping the layer). Both halves matter: an unmarked
  // unimplemented mode is a silent wrong picture, and a marked implemented one
  // is a warning about nothing that teaches a user to ignore the marker.
  // Dissolve stands in for "linear-burn" (docs/blend-mode-gaps.md's stages
  // made that a real mode) -- permanently unimplemented, so this fixture
  // stays valid no matter how many more Photoshop modes later land.
  {
    Layer carried;
    carried.kind = LayerKind::RGB;
    carried.blend = "dissolve";
    check(layerRowSubLine(carried) ==
              std::string("RGB") + kSep + "DISSOLVE (!)" + kSep + "100%",
          "blend: `RGB - DISSOLVE (!) - 100%` -- the design's own eighth row, and the "
          "name is upper-cased as carried rather than mapped through a table");

    bool anyImplementedMarked = false;
    for (const BlendModeInfo& info : allBlendModes()) {
      Layer row;
      row.kind = LayerKind::RGB;
      row.blend = info.name;
      if (contains(layerRowSubLine(row), "(!)")) anyImplementedMarked = true;
    }
    check(!anyImplementedMarked,
          "blend: NO mode in core/Blend's own table is marked (!) -- normal, plus, multiply, "
          "screen, min, max and mix are all composited, so a marker on one would be noise");
  }

  std::printf("  -- F. what the design draws that the model cannot supply --\n");

  // **The drying countdown.** docs/ui.md §3.2 and the design both give a Media
  // row `MEDIA:WATERCOLOUR - WET - 4.2s`. `core::Layer` has no medium name and
  // no wet state; core/Layer.hpp says a Media layer "needs the fluid solver's
  // own per-medium state" and has none, and the wetness that does exist is
  // `sim::PaintSim`'s one canvas-wide field with no layer awareness. There is
  // no seconds-until-dry anywhere in the build. So the row says what a row can
  // say, and this assertion is what a revision reaching for a plausible number
  // will trip.
  {
    Layer media;
    media.kind = LayerKind::Media;
    const std::string line = layerRowSubLine(media);
    check(line == std::string("MEDIA") + kSep + "NORMAL" + kSep + "100%" &&
              !contains(line, "WATERCOLOUR") && !contains(line, "WET"),
          "media: the row carries NO medium name and NO drying countdown -- nothing on Layer "
          "knows either, and PaintSim's wetness is canvas-wide with no layer awareness");
  }

  // **The fill count.** The design's `FLATS - 153 FILLS - NORMAL`, and
  // docs/ui.md §3.2's "suffixed with the fill count". A Flats layer has no
  // fills: core/Merge.cpp says "a Flats layer no regions", there is no fill
  // list, no count, and no Fills panel.
  {
    Layer flats;
    flats.kind = LayerKind::Flats;
    const std::string line = layerRowSubLine(flats);
    check(line == std::string("FLATS") + kSep + "NORMAL" + kSep + "100%" &&
              !contains(line, "FILL"),
          "flats: the row carries NO fill count -- a Flats layer holds no regions in this "
          "build, so any number here would be invented");
  }

  std::printf("  -- G. the link badge takes the trailing slot, and only that --\n");

  // §6.1's rule for the maximal row: "LINKED+n takes the trailing slot, so
  // nothing in the worst case is truncated at 322 px". Two things follow, and
  // both are checked: the badge counts *partners* (so a pair reads `LINKED+1`,
  // not `LINKED+2`), and the metadata line must NOT also say it -- a row that
  // said `LINKED` twice would spend the width the rule exists to save.
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    while (doc.layers.size() < 3) addLayer(doc, doc.layers.size(), makeRgbLayer("l"));
    check(layerLinkBadgeText(doc, 0).empty(),
          "link: an unlinked layer draws NO badge -- an empty trailing slot is what makes "
          "an occupied one readable");

    const uint64_t group = nextLinkGroupId(doc);
    setLayerLinkGroup(doc, 0, group);
    setLayerLinkGroup(doc, 1, group);
    check(layerLinkBadgeText(doc, 0) == "LINKED+1" && layerLinkBadgeText(doc, 1) == "LINKED+1",
          "link: a linked PAIR reads LINKED+1 on both rows -- the badge counts partners, not "
          "members, so it answers \"what else moves with this\"");
    setLayerLinkGroup(doc, 2, group);
    check(layerLinkBadgeText(doc, 0) == "LINKED+2",
          "link: a group of three reads LINKED+2 -- the design's own badge text");

    check(!contains(layerRowSubLine(doc.layers[0]), "LINKED"),
          "link: the METADATA line never says LINKED -- the badge owns the trailing slot, "
          "and saying it twice would spend the width §6.1's rule exists to save");
  }

  std::printf("  -- H. the filter chip and the header count --\n");

  // The design draws `KIND: ALL`. The label is derived from `layerKindName()`
  // rather than typed out, so a kind added to core/Layer.hpp shows up in the
  // chip -- which matters because the four kinds this build cannot *create* are
  // exactly the ones a user needs to *find* in a document that arrived carrying
  // them (PRD I10).
  {
    check(layerKindFilterLabel(std::nullopt) == "KIND: ALL",
          "filter: an inactive kind filter reads `KIND: ALL`, the design's own chip");
    check(layerKindFilterLabel(LayerKind::Pigment) == "KIND: PIGMENT" &&
              layerKindFilterLabel(LayerKind::Adjustment) == "KIND: ADJUSTMENT",
          "filter: a kind filter names the kind, upper-cased -- docs/ui.md section 1 puts "
          "caps labels in the monospace face and this is one");
    bool everyKindLabelled = true;
    for (const LayerKind kind : kAllKinds)
      if (layerKindFilterLabel(kind) == "KIND: ?") everyKindLabelled = false;
    check(everyKindLabelled,
          "filter: every kind produces a real chip label -- including the four that cannot "
          "be created, which are the ones a loaded document most needs filtering for");
  }

  // The design's header shows a bare `8`. A bare count over a filtered list is
  // a different wrong answer depending on which number it is, so a filtered
  // panel shows both.
  {
    check(layerPanelCountLabel(8, 8) == "8" && layerPanelCountLabel(0, 0) == "0",
          "count: an unfiltered panel shows the bare stack size, the design's own `8`");
    check(layerPanelCountLabel(3, 8) == "3/8",
          "count: a filter hiding five rows shows `3/8` -- a bare 8 over three rows and a "
          "bare 3 over a stack of eight are each a different lie about the document");
  }

  std::printf("  -- I. the filter still governs what a command may touch --\n");

  // 2a moves the filter into a band of its own, which changes where it is drawn
  // and nothing about what it means. app/LayerPanel.hpp's rule -- a hidden row
  // stays selected, and a command acts only on rows the user can see -- is what
  // makes the refusal sentence the design quotes possible at all, so the two
  // are checked together here rather than being assumed to have survived the
  // re-layout.
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    doc.layers[0].name = "photo plate";
    addLayer(doc, 1, makeRgbLayer("Line pass"));
    addLayer(doc, 2, makeRgbLayer("Wash"));

    LayerFilter filter;
    filter.text = "wash";
    const std::vector<size_t> shown = layersMatchingFilter(doc, filter);
    check(shown.size() == 1 && doc.layers[shown[0]].name == "Wash",
          "filter: matching is case-insensitive over the row's DISPLAYED title, so what a "
          "user types matches what is in front of them");

    const LayerSelection all = makeLayerSelection({0, 1, 2});
    check(restrictSelectionToFilter(doc, all, filter).size() == 1 &&
              layersHiddenFromSelection(doc, all, filter) == 2,
          "filter: a command sees only the visible members, and the count of the rest is "
          "what the refusal sentence the design quotes is built from");
    check(all.size() == 3,
          "filter: the selection itself is UNCHANGED -- a hidden row stays selected, so "
          "clearing the box brings it back and typing is never a destructive edit");
  }

  std::printf("[selftest] layers panel 2a %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
