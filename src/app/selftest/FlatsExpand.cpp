#include "app/selftest/Support.hpp"

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "core/LayerOps.hpp"
#include "core/Merge.hpp"
#include "flats/FlatsLayer.hpp"

namespace np {

// ---------------------------------------------------------------------------
// core/Merge's `expandFlatsLayer()` -- PRD N9, "a Flats layer expands to real
// layers on demand: per colour, per fill, or merged".
//
// **Why this is worth a headless section.** The panels this shipped with
// cannot be asserted here at all: `--selftest` has no window and no ImGui
// frame, so nothing in this suite can prove that a slider passes the right
// bounds or that a button greys out. The expansion is the opposite -- it is a
// pure function from (document, mode) to (document), it is destructive, and
// three of the ways it can be wrong are silent:
//
//   * the colour domain. `FlatRgb` is 8-bit DISPLAY sRGB and
//     `fillThroughSelection()` wants STRAIGHT LINEAR. Skipping the decode
//     produces a layer that is roughly twice as dark as the flat it came
//     from, which looks like a colour-management bug rather than a missing
//     conversion, and no other assertion in this suite would notice.
//   * the group structure. core/LayerSetOps builds a group as a contiguous
//     run of members with the Group directly ABOVE them and each member's
//     `parent` carrying the group's tag. Building it any other way still
//     composites, so it looks right on screen while `layerGroupDepth()` and
//     `groupMemberSpan()` disagree with it.
//   * the undo step. `recordMerge()` is what makes this one entry; bypassing
//     it leaves an expansion that cannot be taken back and, worse, a
//     composite cache that never notices the document changed.
//
// The fixture is deliberately tiny and its answer is known by construction:
// two closed boxes on a white ground, so the evaluation must find exactly two
// non-background fills.
namespace {

void drawBox(Document& doc, size_t layer, int x0, int y0, int x1, int y1) {
  const std::array<float, 4> black{0.0f, 0.0f, 0.0f, 1.0f};
  auto put = [&](int x, int y) {
    const PixelCoord at{x, y};
    doc.layers[layer].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at), black);
  };
  for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
  for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
}

// A white ground with two closed boxes on it, and a Flats layer above.
// Returns the document; the Flats layer is index 1.
Document twoBoxDocument() {
  Document doc = Document::createBlank(48, 32, WorkingSpace{});
  const std::array<float, 4> white{1.0f, 1.0f, 1.0f, 1.0f};
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 48; ++x) {
      const PixelCoord at{x, y};
      doc.layers[0].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at), white);
    }
  drawBox(doc, 0, 4, 4, 18, 27);
  drawBox(doc, 0, 26, 4, 42, 27);
  addLayer(doc, 1, makeFlatsLayer("Flats"));
  return doc;
}

}  // namespace

bool runFlatsExpandTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ---- the fixture really does have two fills ----------------------------
  //
  // Asserted rather than assumed, because every count below is relative to it
  // and a fixture that quietly segmented into one region would make the
  // per-fill and per-colour assertions agree for the wrong reason.
  size_t fillCount = 0;
  {
    Document doc = twoBoxDocument();
    const std::shared_ptr<const FlatEvaluation> eval = flatsEvaluateLayer(doc, 1);
    if (eval)
      for (const int r : eval->roots()) {
        const FlatFill& f = eval->fills[static_cast<size_t>(r)];
        if (!f.isBg && !f.deleted && f.visible) ++fillCount;
      }
    std::printf("  the two-box fixture segments into %zu non-background fill(s)\n", fillCount);
    check(fillCount == 2, "flats expand: the fixture has exactly two fills to expand");
  }

  // ---- per fill ----------------------------------------------------------
  {
    OpenDocument od;
    od.document = twoBoxDocument();
    od.activeLayer = 1;
    const size_t before = od.document.layers.size();
    const LayerEditResult r = applyFlatsExpand(od, FlatsExpandMode::PerFill, 1);
    check(r.ok, "flats expand: per fill succeeds on a Flats layer");
    // Two members plus the Group replace the one Flats layer: +2 net.
    check(od.document.layers.size() == before + 2,
          "flats expand: per fill leaves two RGB members and a Group where the Flats layer was");

    // The group's shape, which is the part that looks right on screen even
    // when it is wrong.
    size_t groupRow = od.document.layers.size();
    for (size_t i = 0; i < od.document.layers.size(); ++i)
      if (od.document.layers[i].kind == LayerKind::Group) groupRow = i;
    check(groupRow != od.document.layers.size(), "flats expand: it makes a Group layer");
    if (groupRow != od.document.layers.size() && groupRow >= 2) {
      const std::string tag = od.document.layers[groupRow].groupTag;
      check(!tag.empty() && od.document.layers[groupRow - 1].parent == tag &&
                od.document.layers[groupRow - 2].parent == tag,
            "flats expand: **the members are the contiguous run directly below the Group**, "
            "each carrying its tag -- core/LayerSetOps' own shape");
      check(od.document.layers[groupRow - 1].kind == LayerKind::RGB &&
                od.document.layers[groupRow - 2].kind == LayerKind::RGB,
            "flats expand: and the members are RGB layers, which hold real pixels");
    }
    // No Flats layer survives: the source is consumed, not left hidden.
    bool anyFlats = false;
    for (const Layer& l : od.document.layers)
      if (l.kind == LayerKind::Flats) anyFlats = true;
    check(!anyFlats,
          "flats expand: the source Flats layer is consumed, not left behind hidden -- a "
          "leftover would composite the same picture twice or accumulate silently");

    // **The end of the gesture the user actually performs**: expand, then drag
    // the group below the line art. Reported from the app -- the group row
    // moved and its children stayed where they were, because `moveLayer()`
    // rotated one element. The two halves are tested apart (this file for the
    // shape, selftest/LayerGroup for the block move) and this is the seam
    // between them, which is where the defect actually lived: each half was
    // correct on its own.
    if (groupRow != od.document.layers.size() && groupRow >= 2) {
      const std::string tag = od.document.layers[groupRow].groupTag;
      const LayerOpResult moved = moveLayer(od.document, groupRow, 0);
      const size_t landed = moved.index;
      check(moved.ok && landed == 2,
            "flats expand: the expanded group drags to the bottom -- its row lands at 2, "
            "because its two members have to fit below it");
      check(landed < od.document.layers.size() &&
                od.document.layers[landed].kind == LayerKind::Group &&
                od.document.layers[landed].groupTag == tag &&
                od.document.layers[1].parent == tag && od.document.layers[0].parent == tag,
            "flats expand: **and its members came with it** -- the whole point of the "
            "gesture, and what an expanded group below the line art has to mean");
      check(od.document.layers[3].kind != LayerKind::Group &&
                od.document.layers[3].parent != tag,
            "flats expand: ...with the line art now ABOVE the group, which is what the drag "
            "asked for");
    }
  }

  // ---- the colour domain, which is the silent one ------------------------
  {
    OpenDocument od;
    od.document = twoBoxDocument();
    od.activeLayer = 1;
    // The fill colour the evaluation chose, before the expansion consumes it.
    FlatRgb want{};
    {
      const std::shared_ptr<const FlatEvaluation> eval = flatsEvaluateLayer(od.document, 1);
      for (const int rr : eval->roots()) {
        const FlatFill& f = eval->fills[static_cast<size_t>(rr)];
        if (!f.isBg && !f.deleted && f.visible) { want = f.color; break; }
      }
    }
    applyFlatsExpand(od, FlatsExpandMode::PerFill, 1);

    // Find any written texel in any expanded member and compare it against
    // the DECODED colour. Premultiplied by an alpha of 1, so the stored value
    // is the straight linear value.
    // **Only the expanded members.** The fixture's own line-art layer is an
    // RGB layer covered in opaque white, so a probe that merely takes the
    // first RGB layer with pixels reads the ground and compares it against a
    // fill colour -- a test that fails for a reason having nothing to do with
    // the decode. Members are identified by the group tag the expansion set.
    std::string tag;
    for (const Layer& l : od.document.layers)
      if (l.kind == LayerKind::Group) tag = l.groupTag;
    bool found = false, matched = false;
    for (const Layer& l : od.document.layers) {
      if (l.kind != LayerKind::RGB || !l.rgbTiles.has_value()) continue;
      if (tag.empty() || l.parent != tag) continue;
      for (int y = 0; y < 32 && !found; ++y)
        for (int x = 0; x < 48 && !found; ++x) {
          const PixelCoord at{x, y};
          const Tile* t = l.rgbTiles->find(tileCoordAt(at));
          if (t == nullptr) continue;
          const std::array<float, 4> px = t->readPixel(tileLocalOffset(at));
          if (px[3] <= 0.0f) continue;
          found = true;
          const float wantR = srgbDecode(static_cast<float>(want[0]) / 255.0f);
          matched = std::fabs(px[0] - wantR) < 0.01f;
        }
      if (found) break;
    }
    check(found, "flats expand: an expanded member actually has pixels in it");
    check(matched,
          "flats expand: **and they are the fill's colour DECODED to linear** -- writing the "
          "8-bit display value straight through fills about twice as dark and nothing else "
          "in this suite would see it");
  }

  // ---- per colour and merged collapse; per group is one here -------------
  {
    auto expandedLayerCount = [&](FlatsExpandMode mode) {
      OpenDocument od;
      od.document = twoBoxDocument();
      od.activeLayer = 1;
      applyFlatsExpand(od, mode, 1);
      size_t rgb = 0;
      for (const Layer& l : od.document.layers)
        if (l.kind == LayerKind::RGB) ++rgb;
      return rgb - 1;  // less the line-art layer the fixture started with
    };
    const size_t merged = expandedLayerCount(FlatsExpandMode::Merged);
    const size_t group = expandedLayerCount(FlatsExpandMode::PerGroup);
    std::printf("  merged -> %zu layer(s), per group (none defined) -> %zu\n", merged, group);
    check(merged == 1, "flats expand: merged produces exactly one layer");
    check(group == 1,
          "flats expand: with no groups recorded, per group produces the single \"Ungrouped\" "
          "layer rather than nothing");
  }

  // ---- the refusals, and that a refusal changes nothing -------------------
  {
    OpenDocument od;
    od.document = twoBoxDocument();
    od.activeLayer = 0;
    const size_t before = od.document.layers.size();
    const LayerEditResult r = applyFlatsExpand(od, FlatsExpandMode::PerFill, 0);
    check(!r.ok && od.document.layers.size() == before,
          "flats expand: expanding a layer that is not a Flats layer refuses and changes "
          "nothing");

    OpenDocument locked;
    locked.document = twoBoxDocument();
    locked.document.layers[1].locked = true;
    const size_t lbefore = locked.document.layers.size();
    const LayerEditResult lr = applyFlatsExpand(locked, FlatsExpandMode::PerFill, 1);
    check(!lr.ok && locked.document.layers.size() == lbefore,
          "flats expand: a locked Flats layer refuses and changes nothing");
  }

  // ---- one undo step, and undo really restores the Flats layer -----------
  {
    OpenDocument od;
    od.document = twoBoxDocument();
    od.activeLayer = 1;
    // The baseline entry an opened document gets. Without it `entries()` is
    // empty, the first `recordEdit()` is the only entry, and `canUndo()` is
    // false -- there would be nothing to undo TO. core/History.hpp says so:
    // "until a second entry exists there is nothing to undo".
    od.history.begin("open", od.document);
    const uint64_t hashBefore = flatsContentHash(od.document.layers[1].flats);
    const size_t entriesBefore = od.history.entries().size();
    applyFlatsExpand(od, FlatsExpandMode::PerFill, 1);
    check(od.history.entries().size() == entriesBefore + 1,
          "flats expand: **it is ONE history entry**, however many layers it produced");
    // The usual call site's shape: `undo()` returns the entry's document and
    // the caller installs it (core/History.hpp).
    if (const Document* back = od.history.undo()) od.document = *back;
    bool restored = false;
    for (const Layer& l : od.document.layers)
      if (l.kind == LayerKind::Flats && flatsContentHash(l.flats) == hashBefore) restored = true;
    check(restored,
          "flats expand: and undo puts the Flats layer back with its parameters and repairs "
          "intact");
  }

  std::printf("[selftest] flats expand %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
