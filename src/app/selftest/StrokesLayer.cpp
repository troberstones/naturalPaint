#include "app/selftest/Support.hpp"

#include <cstdio>

#include "app/StrokeSession.hpp"
#include "brush/StrokesLayer.hpp"
#include "core/DirtyTiles.hpp"
#include "core/LayerOps.hpp"
#include "core/Merge.hpp"
#include "core/StrokesContent.hpp"
#include "core/VectorRaster.hpp"
#include "io/NpaintFile.hpp"
#include "io/StrokesSerial.hpp"

namespace np {

// ---------------------------------------------------------------------------
// PLAN.md phase 8's substrate: `LayerKind::Strokes` as a real layer kind.
//
// The kind was an "inert placeholder" with no content member at all, so four
// separate claims in this build were about something that did not exist: PRD
// C1 listed it P0, PRD C11 rasterised it, PRD D6 asked clone and heal to stay
// correct under a regrade, and PRD F11 said erasing it deletes dab records
// rather than pixels. This section is what makes those four checkable.
//
// **Every one of the properties below is silent when it breaks**, which is
// why they are here rather than left to a picture:
//
//   * The SPATIAL INDEX cannot be tested by its answers alone. An index that
//     degenerated to one bucket holding every dab returns exactly the right
//     set for every query and is a linear scan wearing a grid's name -- the
//     whole thing phase 8 asked for, absent, with nothing able to see it. So
//     the grid's SHAPE is asserted, not only its results.
//
//   * CHECKPOINTS cannot be tested by pixels at all. A checkpointed replay
//     and a from-scratch replay produce identical tiles BY CONSTRUCTION, so
//     an image comparison passes whether checkpoints exist or not. The
//     mechanism is exposed (`strokesCheckpointCount()`,
//     `strokesLastReplayedDabs()`) and asserted directly, and the pixel
//     equality is asserted BESIDE it -- neither alone says anything.
//
//   * SAMPLES-ONLY-FROM-BELOW fails invisibly in both directions. Sampling
//     the whole document instead makes the layer read its own output, which
//     still produces a picture; sampling nothing makes a heal reproduce
//     transparent black, which looks like a hole. Both are asserted, plus the
//     regrade itself (PRD D6): change a layer BENEATH and the marks change
//     with it.
//
//   * PRD F11 is asserted as "the records went and the pixels did not",
//     because an eraser that quietly fell through to an RGB erase on a layer
//     with no RGB store writes nothing and looks identical to one that
//     deleted no records.
namespace {

// A canvas small enough that a whole-document composite is cheap and large
// enough to span several `StrokesIndex::kCellSize` cells, which the grid
// assertions need.
constexpr int kW = 256;
constexpr int kH = 256;

DabRecord inkDab(float x, float y, float r, std::array<float, 4> rgba) {
  DabRecord d;
  d.x = x;
  d.y = y;
  d.radius = r;
  // A hardness-1 disc -- exactly 1.0 over its flat core, the last `edgePx` of
  // the rim antialiased (`DabRecord::edgePx` defaults to `BrushTip`'s 1.0 and
  // `tipOf()` passes the record's value through) --
  // so the assertions below can talk about "the centre texel" and
  // "a texel outside" without depending on the falloff's exact shape --
  // brush/Deposit §2 owns that shape and asserts it there.
  d.hardness = 1.0f;
  d.rgba = rgba;
  return d;
}

DabRecord belowDab(float x, float y, float r, float dx, float dy) {
  DabRecord d = inkDab(x, y, r, {0.0f, 0.0f, 0.0f, 1.0f});
  d.source = DabColorSource::Below;
  d.sourceDx = dx;
  d.sourceDy = dy;
  return d;
}

void fill(Layer& layer, std::array<float, 4> rgba) {
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      const PixelCoord at{x, y};
      layer.rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(tileLocalOffset(at), rgba);
    }
}

// A document: an RGB layer of one flat colour, with a Strokes layer above it.
Document baseDocument(std::array<float, 4> under) {
  Document doc = Document::createBlank(kW, kH, WorkingSpace{});
  doc.layers.clear();
  Layer rgb = makeRgbLayer("under");
  fill(rgb, under);
  doc.layers.push_back(std::move(rgb));
  doc.layers.push_back(makeStrokesLayer("dabs"));
  doc.layers[0].id = 1;
  doc.layers[1].id = 2;
  doc.nextLayerId = 3;
  return doc;
}

std::array<float, 4> texelOf(const TileStore& tiles, int x, int y) {
  const PixelCoord at{x, y};
  const Tile* t = tiles.find(tileCoordAt(at));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(at));
}

bool sameTiles(const TileStore& a, const TileStore& b) {
  for (int y = 0; y < kH; ++y)
    for (int x = 0; x < kW; ++x) {
      const std::array<float, 4> p = texelOf(a, x, y);
      const std::array<float, 4> q = texelOf(b, x, y);
      for (int c = 0; c < 4; ++c)
        if (p[c] != q[c]) return false;
    }
  return true;
}

bool nearly(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

bool runStrokesLayerTest() {
  bool ok = true;
  auto check = [&ok](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
  };
  strokesForgetAll();

  std::printf("  -- A. the dab record, its bounds and its hash --\n");
  {
    const DabRecord d = inkDab(50.0f, 60.0f, 10.0f, {1.0f, 0.0f, 0.0f, 1.0f});
    const DabBounds b = dabRecordBounds(d);
    check(!b.empty() && b.x0 <= 40 && b.y0 <= 50 && b.x1 >= 61 && b.y1 >= 71,
          "bounds: a radius-10 dab's box contains the whole disc");

    DabRecord zero = d;
    zero.radius = 0.0f;
    DabRecord nan = d;
    nan.x = std::numeric_limits<float>::quiet_NaN();
    DabRecord inf = d;
    inf.radius = std::numeric_limits<float>::infinity();
    check(dabRecordBounds(zero).empty() && dabRecordBounds(nan).empty() &&
              dabRecordBounds(inf).empty(),
          "bounds: a zero, NaN or infinite dab covers NOTHING");

    StrokesContent c;
    c.dabs.push_back(d);
    c.nextDabId = 7;
    const uint64_t h0 = strokesContentHash(c);
    check(strokesContentHash(c) == h0, "hash: the same content hashes the same twice");

    // Every field, one at a time. A hash that missed one would let an edit to
    // that field reach neither the compositor (core/DirtyTiles) nor the
    // evaluation cache -- the exact defect core/DirtyTiles.hpp records for
    // Flats, where "toggle the eye icon to see your own edit" was how it got
    // reported.
    bool everyFieldCounts = true;
    {
      auto differs = [&](const StrokesContent& m) { return strokesContentHash(m) != h0; };
      StrokesContent m;
      m = c; m.dabs[0].id = 99; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].strokeId = 99; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].x += 0.5f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].y += 0.5f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].radius += 1.0f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].hardness = 0.1f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].roundness = 0.5f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].angle = 30.0f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].flow = 0.5f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].rgba[1] = 1.0f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].source = DabColorSource::Below; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].sourceDx = 4.0f; everyFieldCounts &= differs(m);
      m = c; m.dabs[0].sourceDy = 4.0f; everyFieldCounts &= differs(m);
      m = c; m.nextDabId = 8; everyFieldCounts &= differs(m);
      m = c; m.dabs.push_back(d); everyFieldCounts &= differs(m);
    }
    check(everyFieldCounts, "hash: EVERY field of a dab moves it, and so does the allocator");

    check(strokesPrefixHash(c, c.dabs.size()) != strokesPrefixHash(c, 0),
          "hash: the prefix hash separates a prefix from the empty one");
    StrokesContent appended = c;
    appended.dabs.push_back(inkDab(90.0f, 90.0f, 4.0f, {0.0f, 1.0f, 0.0f, 1.0f}));
    check(strokesPrefixHash(appended, 1) == strokesPrefixHash(c, 1),
          "hash: APPENDING leaves every earlier prefix hash untouched");
    StrokesContent edited = c;
    edited.dabs[0].x += 1.0f;
    check(strokesPrefixHash(edited, 1) != strokesPrefixHash(c, 1),
          "hash: editing INSIDE a prefix changes that prefix's hash");
  }

  std::printf("  -- B. the spatial index is a GRID, not a scan wearing its name --\n");
  {
    StrokesContent c;
    // Spread across the canvas at a spacing wider than one cell, so a correct
    // grid MUST allocate more than one bucket.
    for (int i = 0; i < 16; ++i)
      c.dabs.push_back(inkDab(8.0f + i * 16.0f, 8.0f + i * 16.0f, 3.0f, {1, 1, 1, 1}));
    const StrokesIndex index(c);
    check(index.dabCount() == 16 && index.bucketCount() > 1,
          "index: 16 spread dabs land in MORE THAN ONE bucket -- a one-bucket index answers "
          "every query correctly and is the linear scan phase 8 asked to be rid of");
    check(index.entryCount() >= index.dabCount(), "index: every dab is indexed at least once");

    const std::vector<size_t> all = index.query(-1000, -1000, 1000, 1000);
    check(all.size() == 16, "index: a query covering everything returns every dab");
    bool ascending = true;
    for (size_t i = 1; i < all.size(); ++i)
      if (all[i] <= all[i - 1]) ascending = false;
    check(ascending,
          "index: results come back ASCENDING and unique, so a caller composites in paint "
          "order without sorting");

    // The dab at i == 4 sits at (72, 72) with radius 3.
    const std::vector<size_t> one = index.query(70, 70, 75, 75);
    check(one.size() == 1 && one[0] == 4,
          "index: a small query returns the ONE dab whose bounds it overlaps, not sixteen");
    check(index.query(1000, 1000, 1004, 1004).empty(),
          "index: a query over empty canvas returns nothing rather than everything");

    // Negative coordinates: a dab hanging off the top-left is legal (tile
    // coordinates are signed), and a cell index computed with `>>` instead of
    // a floor would be one cell out for every one of them.
    StrokesContent neg;
    neg.dabs.push_back(inkDab(-20.0f, -20.0f, 5.0f, {1, 1, 1, 1}));
    const StrokesIndex negIndex(neg);
    check(negIndex.query(-30, -30, -10, -10).size() == 1,
          "index: a dab at NEGATIVE coordinates is found -- a cell index built with a shift "
          "instead of a floor is off by one for every one of them");
  }

  std::printf("  -- C. rasterising dab records --\n");
  {
    StrokesContent c;
    c.dabs.push_back(inkDab(64.0f, 64.0f, 8.0f, {1.0f, 0.0f, 0.0f, 1.0f}));
    const TileStore tiles = strokesRasterize(c, kW, kH, {});
    const std::array<float, 4> centre = texelOf(tiles, 64, 64);
    check(nearly(centre[0], 1.0f) && nearly(centre[3], 1.0f) && nearly(centre[1], 0.0f),
          "raster: a hard red dab is opaque red at its centre, premultiplied");
    check(texelOf(tiles, 120, 120)[3] == 0.0f,
          "raster: and transparent well outside its radius -- a dab that painted its whole "
          "bounding box would pass every centre assertion");

    // A second dab over the first: paint order is the vector's order.
    StrokesContent two = c;
    two.dabs.push_back(inkDab(64.0f, 64.0f, 8.0f, {0.0f, 0.0f, 1.0f, 1.0f}));
    const std::array<float, 4> top = texelOf(strokesRasterize(two, kW, kH, {}), 64, 64);
    check(nearly(top[2], 1.0f) && nearly(top[0], 0.0f),
          "raster: the LATER dab wins at an overlap -- the vector's order is the paint "
          "order, so a reversed loop would put strokes silently back to front");
  }

  std::printf("  -- D. samples-only-from-below (PRD D6) --\n");
  {
    Document doc = baseDocument({1.0f, 0.0f, 0.0f, 1.0f});
    check(strokesSourceLayers(doc, 1) == std::vector<size_t>{0} &&
              strokesSourceLayers(doc, 0).empty(),
          "below: a Strokes layer's sources are exactly the layers BENEATH it");

    // A `Below` dab with no offset over a red layer must reproduce red.
    doc.layers[1].strokes.dabs.push_back(belowDab(64.0f, 64.0f, 8.0f, 0.0f, 0.0f));
    strokesForgetAll();
    std::shared_ptr<const TileStore> t = strokesLayerTiles(doc, 1);
    check(t != nullptr && nearly(texelOf(*t, 64, 64)[0], 1.0f) &&
              nearly(texelOf(*t, 64, 64)[3], 1.0f),
          "below: a Below dab reproduces the composite beneath it -- red under, red in the "
          "mark");

    // **PRD D6, in the words the requirement uses**: an ADJUSTMENT layer
    // slipped in beneath the Strokes layer -- "layers beneath them are
    // regraded" -- and the mark must come back inverted on the next
    // evaluation, with no dab record edited. A full Invert in the display
    // domain turns the red beneath into cyan.
    const uint64_t contentBefore = strokesContentHash(doc.layers[1].strokes);
    Document regraded = doc;
    Layer adj = makeAdjustmentLayer("invert");
    adj.id = 5;
    Op inv;
    inv.pointKind = PointOpKind::Invert;
    inv.invert.domain = InvertParams::Domain::Display;
    inv.invert.amount = 1.0f;
    adj.ops.add(inv);
    regraded.layers.insert(regraded.layers.begin() + 1, std::move(adj));
    // The Strokes layer moved up one; its id did not, so this also exercises
    // the cache entry noticing that what lies BENEATH it changed.
    t = strokesLayerTiles(regraded, 2);
    check(t != nullptr && nearly(texelOf(*t, 64, 64)[0], 0.0f) &&
              nearly(texelOf(*t, 64, 64)[1], 1.0f) && nearly(texelOf(*t, 64, 64)[2], 1.0f),
          "below: PRD D6 -- an Adjustment layer added BENEATH regrades what the mark "
          "reproduces, on the next evaluation, with no dab record edited");
    check(strokesContentHash(regraded.layers[2].strokes) == contentBefore,
          "below: and the records really were untouched -- had the evaluation baked pixels "
          "into them, D6 would hold once and never again");

    // A layer ABOVE must not be read. Sampling the whole document instead of
    // the prefix still produces a picture, so nothing but this notices.
    Document withAbove = doc;
    Layer green = makeRgbLayer("above");
    fill(green, {0.0f, 1.0f, 0.0f, 1.0f});
    green.id = 9;
    withAbove.layers.push_back(std::move(green));
    strokesForgetAll();
    t = strokesLayerTiles(withAbove, 1);
    check(t != nullptr && nearly(texelOf(*t, 64, 64)[0], 1.0f) && texelOf(*t, 64, 64)[1] < 0.5f,
          "below: a layer ABOVE the Strokes layer is not a source -- sampling the whole "
          "document instead would still draw something, just a different picture");

    // Nothing beneath: transparent, not black.
    Document bottom = Document::createBlank(kW, kH, WorkingSpace{});
    bottom.layers.clear();
    bottom.layers.push_back(makeStrokesLayer("dabs"));
    bottom.layers[0].id = 1;
    bottom.layers[0].strokes.dabs.push_back(belowDab(64.0f, 64.0f, 8.0f, 0.0f, 0.0f));
    strokesForgetAll();
    t = strokesLayerTiles(bottom, 0);
    check(t != nullptr && texelOf(*t, 64, 64)[3] == 0.0f,
          "below: with nothing beneath, a Below dab draws NOTHING rather than opaque black "
          "-- a hole is what a wrong answer here looks like");

    // The offset: a clone reproduces a DIFFERENT place, not the same place.
    Document off = baseDocument({0.0f, 0.0f, 0.0f, 0.0f});
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        const PixelCoord at{x, y};
        off.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
            .writePixel(tileLocalOffset(at), {0.0f, 1.0f, 1.0f, 1.0f});
      }
    off.layers[1].strokes.dabs.push_back(belowDab(100.0f, 100.0f, 6.0f, -90.0f, -90.0f));
    strokesForgetAll();
    t = strokesLayerTiles(off, 1);
    check(t != nullptr && nearly(texelOf(*t, 100, 100)[1], 1.0f) &&
              nearly(texelOf(*t, 100, 100)[2], 1.0f),
          "below: the source OFFSET is honoured -- the mark at (100,100) reproduces "
          "(10,10), which is what makes the record a clone rather than a smear");
  }

  std::printf("  -- E. checkpoint tiles (PLAN.md phase 8) --\n");
  {
    const size_t interval = strokesCheckpointInterval();
    check(interval > 0, "checkpoint: the interval is a real number this test sizes against");

    Document doc = baseDocument({0.0f, 0.0f, 0.0f, 0.0f});
    // Enough dabs for two checkpoints, laid so each writes somewhere real and
    // the replay is not trivially a no-op.
    const size_t count = interval * 2 + 5;
    for (size_t i = 0; i < count; ++i)
      doc.layers[1].strokes.dabs.push_back(
          inkDab(static_cast<float>(4 + (i * 3) % (kW - 8)),
                 static_cast<float>(4 + (i / 40) % (kH - 8)), 3.0f, {1.0f, 1.0f, 1.0f, 1.0f}));
    strokesForgetAll();
    strokesLayerTiles(doc, 1);
    check(strokesCheckpointCount(doc.layers[1].id) == 2,
          "checkpoint: a replay of 2*interval+5 dabs leaves exactly TWO checkpoints -- no "
          "assertion over pixels can see this, because a checkpointed and a from-scratch "
          "replay are identical by construction");
    check(strokesLastReplayedDabs(doc.layers[1].id) == count,
          "checkpoint: the first evaluation replays every dab, having nothing to resume "
          "from");

    // Appending: the whole point.
    doc.layers[1].strokes.dabs.push_back(inkDab(200.0f, 200.0f, 3.0f, {1, 1, 1, 1}));
    const std::shared_ptr<const TileStore> appended = strokesLayerTiles(doc, 1);
    check(strokesLastReplayedDabs(doc.layers[1].id) == 6,
          "checkpoint: APPENDING one dab replays six, not five hundred -- resumed from the "
          "last checkpoint, which is what PLAN.md phase 8 asks for by name");

    // And the resumed answer must be the same answer.
    const TileStore scratch = strokesRasterize(doc.layers[1].strokes, kW, kH, {});
    check(appended != nullptr && sameTiles(*appended, scratch),
          "checkpoint: the resumed result is TEXEL-IDENTICAL to a from-scratch replay -- "
          "the assertion above says checkpoints happen, this one says they are honest");

    // An edit inside the first prefix must throw them away.
    doc.layers[1].strokes.dabs[0].x += 1.0f;
    strokesLayerTiles(doc, 1);
    check(strokesLastReplayedDabs(doc.layers[1].id) == doc.layers[1].strokes.dabs.size(),
          "checkpoint: editing the FIRST dab invalidates every checkpoint and replays all "
          "of them -- a stale checkpoint would keep drawing a dab the user moved");
  }

  std::printf("  -- F. the `np:dabs` round trip --\n");
  {
    StrokesContent c;
    c.nextDabId = 42;
    DabRecord a = inkDab(11.25f, 23.5f, 7.75f, {0.5f, 0.25f, 0.125f, 0.75f});
    a.id = 3;
    a.strokeId = 9;
    a.roundness = 0.4f;
    a.angle = 33.5f;
    a.flow = 0.625f;
    c.dabs.push_back(a);
    DabRecord b = belowDab(80.0f, 90.0f, 12.0f, -5.5f, 6.25f);
    b.id = 4;
    c.dabs.push_back(b);

    const std::string blob = serializeStrokesContent(c);
    // **Updated for `npdabs2`.** This pinned `npdabs1:` until the record
    // gained `edgePx` (io/StrokesSerial.hpp): both records here carry
    // `DabRecord`'s default edgePx of 1, which only v2 can hold, so the
    // content-decided version rule writes v2.
    check(blob.rfind("npdabs2:", 0) == 0,
          "npdabs: the payload carries its version in its prefix -- npdabs2: for records "
          "whose edgePx a v1 payload cannot hold -- so a future npdabs3: is refused by name "
          "rather than half-decoded");
    StrokesContent back;
    std::string why;
    check(deserializeStrokesContent(blob, &back, &why) &&
              strokesContentHash(back) == strokesContentHash(c),
          "npdabs: the content hash is identical across a round trip, so NO field was "
          "silently dropped");
    check(back.dabs.size() == 2 && back.dabs[1].source == DabColorSource::Below &&
              back.dabs[1].sourceDx == -5.5f && back.dabs[0].flow == 0.625f,
          "npdabs: including the source policy and the floats EXACTLY -- io/StrokesSerial "
          "carries IEEE-754 bit patterns, not decimal renderings");

    StrokesContent untouched;
    untouched.nextDabId = 5;
    // **Updated for `npdabs2`, and re-pinned one version further out.** This
    // used "npdabs2:00" until npdabs2 became a version this build reads, at
    // which point it would have been testing that a KNOWN version with a
    // two-digit payload is refused as truncated -- true, and green with the
    // version gate deleted (io/TextSerial's `nptext3` lesson). The future tag
    // now sits on a payload that is otherwise PERFECTLY VALID, so only the
    // version gate can refuse it.
    const std::string futureTagged = "npdabs3:" + blob.substr(std::strlen("npdabs2:"));
    check(!deserializeStrokesContent(futureTagged, &untouched, &why) &&
              untouched.nextDabId == 5 && contains(why, "npdabs1:") && contains(why, "npdabs2:"),
          "npdabs: a FUTURE version tag on an otherwise valid payload is refused BY NAME -- "
          "the refusal names both versions this build reads -- and leaves the output "
          "untouched (PRD I10 carries it verbatim instead)");
    check(!deserializeStrokesContent("npdabs1:zz", &untouched, &why) && contains(why, "hex"),
          "npdabs: a non-hex character is refused rather than decoded as zero");
    // The allocation bomb: a count field is four bytes and can ask for a
    // terabyte. Eight zero bytes of `nextDabId`, then 0xFFFFFFFF.
    check(!deserializeStrokesContent("npdabs1:0000000000000000ffffffff", &untouched, &why) &&
              contains(why, "not plausible"),
          "npdabs: an implausible dab count is refused BEFORE anything is allocated");
    check(!deserializeStrokesContent(blob + "00", &untouched, &why) &&
              contains(why, "trailing bytes"),
          "npdabs: trailing bytes are refused -- a payload this build only half understands "
          "must not open as though it were understood");

    // And through a real file.
    Document doc = baseDocument({0.25f, 0.5f, 0.75f, 1.0f});
    doc.layers[1].strokes = c;
    doc.layers[1].opacity = 0.375f;
    doc.layers[1].blend = "screen";
    const char* path = "selftest_strokes_roundtrip.npaint";
    const NpaintSaveResult saved = saveNpaint(doc, path, NpaintSaveOptions{});
    check(saved.ok,
          "npaint: a document containing a Strokes layer SAVES -- it used to be refused by "
          "name, since the kind had no on-disk representation");
    if (!saved.ok) std::printf("      save error: %s\n", saved.error.c_str());
    if (saved.ok) {
      const NpaintLoadResult loaded = loadNpaint(path);
      check(loaded.ok && loaded.document.layers.size() == 2 &&
                loaded.document.layers[1].kind == LayerKind::Strokes,
            "npaint: and it loads back as a Strokes layer, not as an RGB one -- the records "
            "are what was stored, not a picture of them");
      if (loaded.ok && loaded.document.layers.size() == 2) {
        const Layer& r = loaded.document.layers[1];
        check(!r.rgbTiles.has_value() && !r.pigmentTiles.has_value(),
              "npaint: with no tile store, which is the kind's definition");
        check(strokesContentHash(r.strokes) == strokesContentHash(c),
              "npaint: every dab record survives the file, field for field");
        check(r.opacity == 0.375f && r.blend == "screen",
              "npaint: and its ordinary layer metadata with them");

        // **Identical PIXELS, which is the claim the brief actually makes** --
        // a hash can agree while an evaluation of the reloaded document
        // disagrees, because the evaluation also reads what lies beneath.
        strokesForgetAll();
        const std::shared_ptr<const TileStore> beforeTiles = strokesLayerTiles(doc, 1);
        const TileStore beforeCopy = beforeTiles ? *beforeTiles : TileStore{};
        strokesForgetAll();
        const std::shared_ptr<const TileStore> afterTiles =
            strokesLayerTiles(loaded.document, 1);
        check(beforeTiles != nullptr && afterTiles != nullptr &&
                  sameTiles(beforeCopy, *afterTiles),
              "npaint: a saved-and-reloaded Strokes layer evaluates to IDENTICAL pixels");

        // The `isLayerAttributeRecognised()` trap io/NpaintFile's Text
        // assertions record: without the entry, the reader files `np:dabs` in
        // the carry as unknown and the writer then emits BOTH its own and the
        // carried one, leaving last-write-wins to choose. On an UNEDITED
        // document the two are identical and every assertion above passes.
        //
        // **The carry has to be HANDED BACK to the save, and that is the whole
        // reason this assertion is worth its lines.** Written without the
        // fourth argument it was green under a sabotage that deleted the
        // `isLayerAttributeRecognised()` entry outright -- `saveNpaint()`
        // defaults `carry` to `nullptr`, so a save that is never given the
        // carry can never emit a carried attribute and the trap this block is
        // named for cannot fire. An assertion that cannot see the defect it
        // names is worse than no assertion, because it is read as coverage.
        Document edited = loaded.document;
        edited.layers[1].strokes.dabs.clear();
        const char* path2 = "selftest_strokes_roundtrip2.npaint";
        if (saveNpaint(edited, path2, NpaintSaveOptions{}, &loaded.carry).ok) {
          const NpaintLoadResult again = loadNpaint(path2);
          check(again.ok && again.document.layers.size() == 2 &&
                    again.document.layers[1].strokes.dabs.empty(),
                "npaint: an EDIT to the dab list wins over the loaded file's own np:dabs -- "
                "the assertion that catches a missing isLayerAttributeRecognised() entry, "
                "which every round trip above provably cannot");
        }
        std::remove(path2);
      }
    }
    std::remove(path);
  }

  // ==========================================================================
  // F2. npdabs2: a record's `edgePx` is stored, and a pre-edgePx document
  //     keeps the hard rim it was painted with (review finding 6)
  // ==========================================================================
  //
  // `npdabs1` had no field for `BrushTip::edgePx`, so `tipOf()` replayed every
  // record at the running build's default -- a hardness-1 r=6 record saved as
  // 112 texels at exactly 1.0 re-rendered as 80 at 1.0 plus 32 fractional,
  // nothing in the file changed. io/StrokesSerial.hpp has the wire layout and
  // the version rule these assertions pin.
  std::printf("  -- F2. npdabs2: edgePx is stored; npdabs1 keeps its hard rim --\n");
  {
    auto bitsOf = [](float v) {
      uint32_t b = 0;
      std::memcpy(&b, &v, sizeof b);
      return b;
    };
    // Coverage census of a rasterised store around one dab: texels at exactly
    // 1.0 alpha, and texels strictly between 0 and 1.
    auto census = [](const TileStore& t, int cx, int cy, int* full, int* frac) {
      *full = 0;
      *frac = 0;
      for (int y = cy - 16; y < cy + 16; ++y)
        for (int x = cx - 16; x < cx + 16; ++x) {
          const float a = texelOf(t, x, y)[3];
          if (a == 1.0f) ++*full;
          else if (a > 0.0f && a < 1.0f) ++*frac;
        }
    };

    // 1. A HAND-WRITTEN npdabs1 payload -- the bytes a pre-bump build wrote,
    //    field by field (io/StrokesSerial.hpp's layout, little-endian):
    //    nextDabId 2, one record: id 1, strokeId 0, x 64, y 64, radius 6,
    //    hardness 1, roundness 1, angle 0, flow 1, rgba (1,0,0,1), source Ink,
    //    sourceDx 0, sourceDy 0. 81 bytes, 69 of them the record.
    const std::string v1Fixture =
        "npdabs1:"
        "0200000000000000" "01000000"          // nextDabId, count
        "0100000000000000" "0000000000000000"  // id, strokeId
        "00008042" "00008042"                  // x 64.0, y 64.0
        "0000c040" "0000803f"                  // radius 6.0, hardness 1.0
        "0000803f" "00000000"                  // roundness 1.0, angle 0.0
        "0000803f"                             // flow 1.0
        "0000803f" "00000000" "00000000" "0000803f"  // rgba 1, 0, 0, 1
        "00"                                   // source Ink
        "00000000" "00000000";                 // sourceDx, sourceDy
    StrokesContent old;
    std::string why;
    const bool v1Read = deserializeStrokesContent(v1Fixture, &old, &why);
    if (!v1Read) std::printf("      refusal: %s\n", why.c_str());
    int v1Full = 0, v1Frac = 0;
    if (v1Read) census(strokesRasterize(old, 128, 128, {}), 64, 64, &v1Full, &v1Frac);
    std::printf("  [measured] a hand-written npdabs1 hard r=6 record rasterises to %d texels at "
                "1.0 and %d fractional (pre-edgePx: 112 and 0)\n", v1Full, v1Frac);
    check(v1Read && old.dabs.size() == 1 && v1Full == 112 && v1Frac == 0,
          "npdabs1: a document saved BEFORE edgePx re-renders exactly as it was painted -- a "
          "hardness-1 r=6 record is 112 texels at exactly 1.0 and none fractional, because "
          "an npdabs1 record reads back with edgePx 0, not the running build's default");
    check(v1Read && old.dabs.size() == 1 && bitsOf(old.dabs[0].edgePx) == 0u &&
              serializeStrokesContent(old) == v1Fixture,
          "npdabs1: and it is read as edgePx +0.0 exactly, and saved again it writes the "
          "IDENTICAL npdabs1 bytes -- an old document re-saved untouched stays readable by the "
          "builds that wrote it (the content-decided version rule)");

    // 2. The npdabs2 round trip, bit-exact, at 0, 1, a non-default 2.5, and
    //    -0.0 (equal to 0 by value, NOT by bits -- the reason the writer's
    //    version test compares bit patterns: v1 would bring it back as +0).
    StrokesContent rt;
    rt.nextDabId = 9;
    const float edges[4] = {0.0f, 1.0f, 2.5f, -0.0f};
    for (int i = 0; i < 4; ++i) {
      DabRecord d = inkDab(20.0f + 30.0f * static_cast<float>(i), 40.0f, 6.0f, {0.0f, 1.0f, 0.0f, 1.0f});
      d.id = static_cast<uint64_t>(i + 1);
      d.edgePx = edges[i];
      rt.dabs.push_back(d);
    }
    const std::string rtBlob = serializeStrokesContent(rt);
    StrokesContent rtBack;
    const bool rtRead = deserializeStrokesContent(rtBlob, &rtBack, &why);
    bool edgesExact = rtRead && rtBack.dabs.size() == 4;
    for (size_t i = 0; edgesExact && i < 4; ++i)
      edgesExact = bitsOf(rtBack.dabs[i].edgePx) == bitsOf(edges[i]);
    check(rtBlob.rfind("npdabs2:", 0) == 0 && edgesExact &&
              strokesContentHash(rtBack) == strokesContentHash(rt),
          "npdabs2: edgePx survives the round trip BIT-EXACTLY at 0, 1, 2.5 and -0.0, and the "
          "content hash with it -- the stored rim is the one the record was made with");
    StrokesContent onlyNegZero;
    onlyNegZero.dabs.push_back(rt.dabs[3]);
    StrokesContent negBack;
    check(serializeStrokesContent(onlyNegZero).rfind("npdabs2:", 0) == 0 &&
              deserializeStrokesContent(serializeStrokesContent(onlyNegZero), &negBack, &why) &&
              negBack.dabs.size() == 1 && bitsOf(negBack.dabs[0].edgePx) == bitsOf(-0.0f),
          "npdabs2: a lone -0.0 edgePx still forces v2 -- v1 is written only when it is "
          "LOSSLESS, compared by bit pattern");

    // And the stored value is what is rasterised, in both directions: the
    // SAME r=6 record at a stored edgePx of 1 is antialiased. Together with
    // assertion 1 (edgePx 0 -> hard) this is what stops `tipOf()` from
    // hard-coding either constant.
    StrokesContent aa = old;
    if (!aa.dabs.empty()) aa.dabs[0].edgePx = 1.0f;
    StrokesContent aaBack;
    int aaFull = 0, aaFrac = 0;
    if (v1Read && deserializeStrokesContent(serializeStrokesContent(aa), &aaBack, &why))
      census(strokesRasterize(aaBack, 128, 128, {}), 64, 64, &aaFull, &aaFrac);
    std::printf("  [measured] the same record stored at edgePx 1: %d at 1.0, %d fractional\n",
                aaFull, aaFrac);
    check(v1Read && aaFrac > 0 && aaFull < 112 && aaFull + aaFrac == 112,
          "npdabs2: the same record stored at edgePx 1 rasterises WITH an antialiased rim over "
          "the same 112-texel footprint -- the record's own value, not a constant");

    // 3. The live route stores the dab tip's OWN edgePx. 2.5 rather than the
    //    default 1, because `DabRecord::edgePx` also defaults to 1: a route
    //    that forgot to copy the field would pass a default-tip check.
    {
      OpenDocument od;
      od.document = baseDocument({0.1f, 0.1f, 0.1f, 1.0f});
      AppState::CloneSourceState clone{};
      setCloneAnchor(clone, Vec2{32.0f, 32.0f});
      latchCloneOffset(clone, Vec2{150.0f, 150.0f});
      StrokeSession s;
      BrushTip tip;
      tip.radius = 8.0f;
      tip.hardness = 1.0f;
      tip.flow = 1.0f;
      tip.edgePx = 2.5f;
      std::string err;
      const bool began =
          s.begin(od, 1, tip, Tool::CloneStamp, &err, nullptr, DynamicInputs{}, &clone);
      if (!began) std::printf("      refusal: %s\n", err.c_str());
      if (began) {
        s.addPoint(150.0f, 150.0f);
        s.addPoint(158.0f, 150.0f);
        s.end();
      }
      const StrokesContent& c = od.document.layers[1].strokes;
      bool allCarry = !c.dabs.empty();
      for (const DabRecord& d : c.dabs) allCarry &= d.edgePx == 2.5f;
      std::printf("  [measured] a live clone stroke on a Strokes layer recorded %zu dabs, "
                  "first edgePx %.2f (tip's 2.50)\n",
                  c.dabs.size(), c.dabs.empty() ? -1.0 : static_cast<double>(c.dabs[0].edgePx));
      check(began && allCarry,
            "record: a live StrokesRecord stroke's records carry the dab TIP's own edgePx "
            "(2.5 here, not the default) -- the rim the stroke was painted with is the rim "
            "it will replay with");
    }

    // 4. A future version through a real FILE: refused by name, opened empty,
    //    and carried verbatim through a save by this build. The file is made
    //    by saving a v2 document and patching its one-byte version digit in
    //    the EXR header (same length, and an EXR header is not compressed) --
    //    the only way to put a payload this build cannot write into a file.
    {
      const char* pathA = "selftest_strokes_future_a.npaint";
      const char* pathB = "selftest_strokes_future_b.npaint";
      const char* pathC = "selftest_strokes_future_c.npaint";
      Document doc = baseDocument({0.25f, 0.5f, 0.75f, 1.0f});
      doc.layers[1].strokes = rt;
      const std::string futureValue = "npdabs3:" + rtBlob.substr(std::strlen("npdabs2:"));
      bool patched = false;
      if (saveNpaint(doc, pathA, NpaintSaveOptions{}).ok) {
        std::ifstream in(pathA, std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const size_t at = bytes.find("npdabs2:");
        if (at != std::string::npos && bytes.find("npdabs2:", at + 1) == std::string::npos) {
          bytes[at + 6] = '3';
          std::ofstream out(pathB, std::ios::binary | std::ios::trunc);
          out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
          patched = out.good();
        }
      }
      check(patched, "future: premise -- a v2 file was written and its one version tag patched "
                     "to npdabs3:");
      const NpaintLoadResult future = patched ? loadNpaint(pathB) : NpaintLoadResult{};
      bool namedBoth = false;
      for (const std::string& w : future.warnings)
        if (contains(w, "npdabs1:") && contains(w, "npdabs2:")) namedBoth = true;
      auto carriedDabs = [](const NpaintLoadResult& r) -> std::string {
        if (r.carry.layerAttributes.size() < 2) return {};
        for (const NpaintAttribute& a : r.carry.layerAttributes[1])
          if (a.name == "np:dabs") return a.stringValue;
        return {};
      };
      check(future.ok && future.document.layers.size() == 2 &&
                future.document.layers[1].kind == LayerKind::Strokes &&
                future.document.layers[1].strokes.dabs.empty() && namedBoth &&
                carriedDabs(future) == futureValue,
            "future: an npdabs3: Strokes layer opens EMPTY with a warning naming the versions "
            "this build reads, and its payload is held in the carry byte for byte");
      std::string afterSave;
      if (future.ok && saveNpaint(future.document, pathC, NpaintSaveOptions{}, &future.carry).ok) {
        const NpaintLoadResult again = loadNpaint(pathC);
        if (again.ok) afterSave = carriedDabs(again);
      }
      check(afterSave == futureValue,
            "future: and SAVED AGAIN by this build the npdabs3: payload is written back "
            "verbatim, not replaced by an empty dab list -- the layer does not render here, "
            "but it is not destroyed (PRD I10)");
      for (const char* p : {pathA, pathB, pathC}) std::remove(p);
    }
  }

  std::printf("  -- G. PRD C11: a Strokes layer rasterises --\n");
  {
    Document doc = baseDocument({0.0f, 0.0f, 0.0f, 0.0f});
    doc.layers[1].strokes.dabs.push_back(inkDab(64.0f, 64.0f, 8.0f, {1.0f, 0.0f, 0.0f, 1.0f}));
    doc.layers[1].opacity = 0.5f;
    doc.layers[1].blend = "multiply";
    strokesForgetAll();
    std::vector<std::string> warnings;
    const LayerOpResult r = rasteriseLayer(doc, 1, &warnings);
    check(r.ok,
          "raster: PRD C11 rasterises a Strokes layer -- core/Merge refused it by name for "
          "as long as the kind had no parameter member");
    if (!r.ok) std::printf("      refusal: %s\n", r.error.c_str());
    check(doc.layers[1].kind == LayerKind::RGB && doc.layers[1].rgbTiles.has_value() &&
              doc.layers[1].strokes.dabs.empty(),
          "raster: it becomes an ordinary RGB layer holding tiles, and the records are gone");
    check(doc.layers[1].opacity == 0.5f && doc.layers[1].blend == "multiply",
          "raster: its participation in the stack is untouched -- rasterising changes what "
          "the content IS, not how the layer composites");
    bool named = false;
    for (const std::string& w : warnings)
      if (contains(w, "dab record") && contains(w, "F11")) named = true;
    check(named,
          "raster: and it says what was lost, naming PRD F11 -- the eraser stops deleting "
          "records on this layer the moment it is pixels");

    // Media is now the ONLY kind C11 refuses, and it must still say so.
    Document media = baseDocument({0, 0, 0, 0});
    media.layers[1].kind = LayerKind::Media;
    const LayerOpResult mr = rasteriseLayer(media, 1);
    check(!mr.ok && contains(mr.error, "Media") && contains(mr.error, "C11"),
          "raster: Media is the one kind C11 still refuses, and the refusal names it and "
          "the requirement rather than saying 'not built'");
  }

  std::printf("  -- H. PRD F11: erasing deletes records, not pixels --\n");
  {
    // The routing table first: the eraser is the ONE tool this kind takes.
    Layer target = makeStrokesLayer("dabs");
    check(strokeRouteFor(Tool::Eraser, &target) == StrokeRoute::StrokesErase,
          "F11: the eraser on a Strokes layer routes to strokes-erase");
    check(strokeRouteFor(Tool::Brush, &target) == StrokeRoute::None &&
              strokeRouteFor(Tool::Pencil, &target) == StrokeRoute::None &&
              strokeRouteFor(Tool::Smudge, &target) == StrokeRoute::None &&
              strokeRouteFor(Tool::Dodge, &target) == StrokeRoute::None,
          "F11: a tool that would have to GUESS what it was recording still refuses the "
          "kind by name rather than painting nothing in silence");
    Layer locked = target;
    locked.locked = true;
    check(strokeRouteFor(Tool::Eraser, &locked) == StrokeRoute::None,
          "F11: a LOCKED Strokes layer refuses for being locked, the ordering every row in "
          "the routing table shares");
    check(strokeRouteWritesLayer(StrokeRoute::StrokesErase) &&
              !grainReachesRoute(StrokeRoute::StrokesErase),
          "F11: the route writes a layer but PAPER GRAIN does NOT reach it -- it computes "
          "no per-texel coverage at all, so delegating would light a whole control group "
          "over a route that ignores every control in it");

    // Now the behaviour.
    OpenDocument od;
    od.document = baseDocument({0.0f, 0.0f, 0.0f, 0.0f});
    for (int i = 0; i < 12; ++i)
      od.document.layers[1].strokes.dabs.push_back(
          inkDab(20.0f + i * 16.0f, 64.0f, 6.0f, {1.0f, 1.0f, 1.0f, 1.0f}));
    const size_t beforeCount = od.document.layers[1].strokes.dabs.size();

    StrokeSession session;
    BrushTip tip;
    tip.radius = 10.0f;
    std::string err;
    const bool began = session.begin(od, 1, tip, Tool::Eraser, &err);
    check(began, "F11: an eraser stroke BEGINS on a Strokes layer");
    if (!began) std::printf("      refusal: %s\n", err.c_str());
    if (began) {
      session.addPoint(20.0f, 64.0f);
      session.addPoint(36.0f, 64.0f);
      session.end();
    }
    const size_t afterCount = od.document.layers[1].strokes.dabs.size();
    check(afterCount < beforeCount,
          "F11: the eraser DELETED dab records -- an eraser that fell through to an RGB "
          "erase on a layer with no RGB store writes nothing and looks the same");
    check(!od.document.layers[1].rgbTiles.has_value() &&
              !od.document.layers[1].pigmentTiles.has_value(),
          "F11: and it wrote no pixels, because the layer has nowhere to put any -- 'not "
          "pixels' is half of what F11 says");

    // The rule the deletion follows, asserted directly rather than through a
    // session: centre-containment, and no partial deletion.
    StrokesContent c;
    // The second dab has to be a CANDIDATE for this to be a test of the centre
    // rule at all: `eraseDabsUnderDisc()` narrows by bounds first, so a dab
    // whose bounds miss the eraser's rectangle is discarded before the centre
    // is ever considered, and the assertion below passes whatever the rule is.
    // At radius 20 its bounds are its centre +/-21, and the eraser's rectangle
    // is [40, 61) x [40, 61) -- so 65 overlaps (45 < 61) while its centre sits
    // 15 texels out, beyond the disc's 10. A dab at 90 overlaps NOTHING, which
    // is what this fixture used to say and why deleting the centre test left
    // it green.
    c.dabs.push_back(inkDab(50.0f, 50.0f, 20.0f, {1, 1, 1, 1}));  // centre inside
    c.dabs.push_back(inkDab(65.0f, 50.0f, 20.0f, {1, 1, 1, 1}));  // overlaps, centre outside
    std::vector<DabRecord> removed;
    const size_t n = eraseDabsUnderDisc(c, 50.0f, 50.0f, 10.0f, &removed);
    check(n == 1 && removed.size() == 1 && c.dabs.size() == 1 && c.dabs[0].x == 65.0f,
          "F11: 'covers' means the disc contains the dab's CENTRE -- deleting everything a "
          "grazing contact touches would remove a whole stroke from one edge tap");
    check(eraseDabsUnderDisc(c, 50.0f, 50.0f, 10.0f, nullptr) == 0,
          "F11: and erasing the same disc twice deletes nothing the second time");
  }

  std::printf("  -- I. the kind reaches the screen, the panel and the menu --\n");
  {
    check(layerRastersToTiles(LayerKind::Strokes),
          "screen: the materialised view rewrites a Strokes layer, without which the "
          "compositor never sees its marks at all");

    // **And the marks actually arrive**, which the predicate above cannot say.
    // It is a direct call on the predicate, so it goes red when the predicate
    // is wrong and stays green for every other way the rewrite could fail --
    // a `continue` in the wrong place, `documentHasVectorLayers()`'s early-out
    // taking a Strokes-only document down the no-copy path, the branch reading
    // `copy` instead of `doc`. Sabotaging `layerRastersToTiles()` reddened
    // that one assertion and nothing else in this file, which is precisely the
    // shape of a suite that pins a predicate and not a behaviour. This walks
    // the whole compositor instead and looks at the texel the user would see.
    Document onScreen = baseDocument({0.0f, 0.0f, 0.0f, 0.0f});
    onScreen.layers[1].strokes.dabs.push_back(
        inkDab(64.0f, 64.0f, 8.0f, {1.0f, 0.0f, 0.0f, 1.0f}));
    strokesForgetAll();
    const std::vector<float> screen = compositeDocumentPremultiplied(onScreen);
    const size_t centreAt = (static_cast<size_t>(64) * kW + 64) * 4;
    check(screen.size() == static_cast<size_t>(kW) * kH * 4 && nearly(screen[centreAt], 1.0f) &&
              nearly(screen[centreAt + 3], 1.0f),
          "screen: and the dab reaches the DOCUMENT COMPOSITE -- a Strokes layer the "
          "materialiser skips composites as an empty parametric layer and leaves the canvas "
          "exactly as it was, with no error raised anywhere");

    Document before = baseDocument({0, 0, 0, 0});
    Document after = before;
    after.layers[1].strokes.dabs.push_back(inkDab(64.0f, 64.0f, 8.0f, {1, 1, 1, 1}));
    const DocumentDirtyTiles d = documentDirtyTiles(before, after);
    check(d.reason == FullRecompositeReason::StrokesContentChanged,
          "screen: a dab-list change dirties the canvas -- core/DirtyTiles' pass 1 is a "
          "WHITELIST, and a kind it does not compare reads as 'nothing changed' for an edit "
          "that changed everything (the Flats defect, one kind later)");
    check(documentDirtyTiles(before, before).reason !=
              FullRecompositeReason::StrokesContentChanged,
          "screen: and an unchanged document does not, so the recomposite is not permanent");

    const std::vector<NewLayerKindEntry>& menu = newLayerKindMenu();
    bool live = false;
    for (const NewLayerKindEntry& e : menu)
      if (e.kind == LayerKind::Strokes)
        live = e.buildable && e.command == LayerCommand::NewStrokesLayer;
    check(live,
          "menu: the NEW popup's Strokes row is live and issues ITS OWN command -- the row "
          "existed and was greyed for as long as the kind had no content member");
    check(layerKindUnbuildableReason(LayerKind::Strokes) == nullptr,
          "menu: and it carries no greyed excuse, because a live row with one is "
          "indistinguishable from a broken button");

    const std::vector<LayerCommand>& all = allLayerCommands();
    check(std::find(all.begin(), all.end(), LayerCommand::NewStrokesLayer) != all.end(),
          "menu: the command is in `allLayerCommands()`, so the menu model can reach it");
    const Layer made = makeStrokesLayer("s");
    check(made.kind == LayerKind::Strokes && made.strokes.dabs.empty() &&
              made.strokes.nextDabId == 1 && !made.rgbTiles.has_value() &&
              !made.pigmentTiles.has_value() && !made.mask.has_value(),
          "menu: `makeStrokesLayer()` gives the same shape of emptiness the other "
          "parametric makers do -- no tiles, no mask, no ops, no dabs");
  }

  std::printf("  -- J. PRD D6 class C: clone and heal RECORD instead of painting --\n");
  {
    // ======================================================================
    // The hole this section closes: `Layer > New Strokes Layer` made a real,
    // saveable layer that NOTHING in the build could put a mark in.
    // `strokeRouteFor()` answered `StrokesErase` for the eraser and `None` for
    // every other tool, and `grep -rn StrokesDab src` found no production code
    // constructing a `DabRecord` at all -- so PRD F11's erase worked correctly
    // on records nothing could make. This is `docs/operations.md`'s class C
    // ("a recorded op on the Strokes layer, re-evaluated on demand") and PRD
    // D6's non-destructive half.
    //
    // Every assertion below is silent when it breaks, in one of two ways:
    //
    //   * A route that recorded NOTHING leaves a Strokes layer with no store
    //     to write and therefore no pixels either -- a stroke that looks
    //     exactly like the refusal it replaced, with no message anywhere.
    //   * A route that BAKED the sampled colour into the record's own `rgba`
    //     produces a picture that is correct on the first evaluation and
    //     never again. That is the failure D6 exists to name, it is invisible
    //     until someone regrades a layer underneath, and it is what the
    //     regrade assertion below is for rather than the reproduction one.
    // ======================================================================

    // --- J1. the routing table -----------------------------------------
    Layer strokes = makeStrokesLayer("dabs");
    check(strokeRouteFor(Tool::CloneStamp, &strokes) == StrokeRoute::StrokesRecord &&
              strokeRouteFor(Tool::Heal, &strokes) == StrokeRoute::StrokesRecord,
          "D6: both repair tools on a Strokes layer route to strokes-record -- ONE route "
          "for the two, because what it appends is one record either way");
    check(strokeRouteFor(Tool::Brush, &strokes) == StrokeRoute::None &&
              strokeRouteFor(Tool::Pencil, &strokes) == StrokeRoute::None,
          "D6: and the ordinary brush still refuses by name -- a recorded PAINT stroke "
          "needs decisions this phase has not made (StrokeSession section 1d)");
    Layer lockedStrokes = strokes;
    lockedStrokes.locked = true;
    check(strokeRouteFor(Tool::CloneStamp, &lockedStrokes) == StrokeRoute::None &&
              strokeRouteFor(Tool::Heal, &lockedStrokes) == StrokeRoute::None,
          "D6: a LOCKED Strokes layer refuses both, the ordering every row in the table "
          "shares -- locked before kind");
    check(strokeRouteWritesLayer(StrokeRoute::StrokesRecord) &&
              !grainReachesRoute(StrokeRoute::StrokesRecord) &&
              !wetnessReachesSolver(StrokeRoute::StrokesRecord) &&
              std::string(strokeRouteName(StrokeRoute::StrokesRecord)) == "strokes-record",
          "D6: it WRITES a layer, PAPER GRAIN does not reach it (there is no per-texel "
          "coverage at record time for tooth to modify), and it is named for what it "
          "writes rather than for either tool that reaches it");

    // **The destructive rows are untouched, and this is the regression
    // surface the whole route was built under.** A `kind == Strokes` test that
    // was accidentally written as a fallthrough would silently redirect every
    // clone and every heal in the application into a dab list.
    Layer rgb = makeRgbLayer("rgb");
    Layer pigment = makePigmentLayer("pig");
    check(strokeRouteFor(Tool::CloneStamp, &rgb) == StrokeRoute::CloneStamp &&
              strokeRouteFor(Tool::Heal, &rgb) == StrokeRoute::Heal,
          "D6: an RGB layer still takes the DESTRUCTIVE clone and heal, unchanged -- the "
          "recording row must not be reachable from the kind File > New makes marks on");
    check(strokeRouteFor(Tool::CloneStamp, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Heal, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::CloneStamp, nullptr) == StrokeRoute::None,
          "D6: and a Pigment layer and a null target still refuse both, for section 1b's "
          "and 1c's own reasons");

    // --- J2. no source anchor is still a refusal, in begin() ------------
    {
      OpenDocument od;
      od.document = baseDocument({0.1f, 0.1f, 0.1f, 1.0f});
      StrokeSession s;
      BrushTip tip;
      tip.radius = 8.0f;
      std::string err;
      AppState::CloneSourceState noAnchor{};
      check(!s.begin(od, 1, tip, Tool::CloneStamp, &err, nullptr, DynamicInputs{}, &noAnchor) &&
                contains(err, "Option-click"),
            "D6: with no source set the recording stroke REFUSES out loud -- at offset "
            "(0,0) every record would reproduce exactly what is already beneath it, which "
            "is invisible in the picture and permanent in the document");
      check(od.document.layers[1].strokes.dabs.empty(),
            "D6: and the refusal recorded nothing, so a refused stroke leaves no entry the "
            "eraser could later find");
    }

    // --- J3. THE HEADLINE ASSERTION ------------------------------------
    //
    // A clone onto a Strokes layer appends a record; that record reproduces
    // the repair when composited; and changing a layer BENEATH it changes what
    // the repair reproduces.
    {
      OpenDocument od;
      od.document = baseDocument({0.1f, 0.1f, 0.1f, 1.0f});
      // A bright red SOURCE patch in the corner of the layer beneath, well
      // away from where the stroke will land.
      for (int y = 16; y < 48; ++y)
        for (int x = 16; x < 48; ++x) {
          const PixelCoord at{x, y};
          od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {1.0f, 0.0f, 0.0f, 1.0f});
        }

      AppState::CloneSourceState clone{};
      setCloneAnchor(clone, Vec2{32.0f, 32.0f});
      check(latchCloneOffset(clone, Vec2{150.0f, 150.0f}) && clone.offset.x == -118.0f,
            "D6: the shared anchor gesture latches the same offset it latches for the "
            "destructive route -- one Option-click means one thing for both");

      StrokeSession s;
      BrushTip tip;
      tip.radius = 8.0f;
      tip.hardness = 1.0f;
      tip.flow = 1.0f;
      std::string err;
      const bool began =
          s.begin(od, 1, tip, Tool::CloneStamp, &err, nullptr, DynamicInputs{}, &clone);
      check(began, "D6: a clone stroke BEGINS on a Strokes layer");
      if (!began) std::printf("      refusal: %s\n", err.c_str());
      if (began) {
        s.addPoint(150.0f, 150.0f);
        s.addPoint(158.0f, 150.0f);
        s.end();
      }

      const StrokesContent& c = od.document.layers[1].strokes;
      check(!c.dabs.empty(),
            "D6: the stroke APPENDED dab records -- this is the whole reachability hole: "
            "before it, no production code in the build constructed a DabRecord at all");
      check(!od.document.layers[1].rgbTiles.has_value() &&
                !od.document.layers[1].pigmentTiles.has_value(),
            "D6: and it wrote no pixels anywhere, which is the 'instead of' half of the "
            "sentence");
      bool wellFormed = !c.dabs.empty();
      bool idsUnique = true;
      const uint64_t strokeId = c.dabs.empty() ? 0 : c.dabs.front().strokeId;
      for (size_t i = 0; i < c.dabs.size(); ++i) {
        const DabRecord& d = c.dabs[i];
        wellFormed &= d.source == DabColorSource::Below && d.sourceDx == -118.0f &&
                      d.sourceDy == -118.0f && d.radius == 8.0f && d.id != 0 &&
                      d.strokeId == strokeId && strokeId != 0;
        for (size_t j = 0; j < i; ++j)
          if (c.dabs[j].id == d.id) idsUnique = false;
      }
      check(wellFormed,
            "D6: every record carries source=Below, the INTEGER offset the gesture set, "
            "the tip's own radius and one shared non-zero stroke id -- a record whose "
            "source policy was wrong reproduces a picture nobody authored");
      check(idsUnique && c.nextDabId > c.dabs.size(),
            "D6: ids are unique and come off the layer's own allocator, so a reopened "
            "layer cannot mint an id a live undo entry still names");
      check(od.history.entries().size() == 1 && od.history.entries()[0].label == "clone stamp" &&
                od.revision > 0,
            "D6: and the stroke left ONE history entry, labelled for the tool that made it "
            "-- a recorded repair is one undo step like any other stroke, not one per dab");

      // The repair REPRODUCES. Red under the source patch, red in the mark.
      strokesForgetAll();
      std::shared_ptr<const TileStore> t = strokesLayerTiles(od.document, 1);
      const uint64_t contentBefore = strokesContentHash(c);
      check(t != nullptr && nearly(texelOf(*t, 150, 150)[0], 1.0f) &&
                texelOf(*t, 150, 150)[1] < 0.1f && nearly(texelOf(*t, 150, 150)[3], 1.0f),
            "D6: the record REPRODUCES the repair when composited -- the mark at (150,150) "
            "carries the red standing at (32,32), which is what makes it a clone");

      // **The sentence PRD D6 actually asks for.** An Adjustment layer slipped
      // in BENEATH -- "layers beneath them are regraded" -- and the mark must
      // come back inverted on the next evaluation with no record edited. This
      // is the assertion that goes red, and the only one that does, if the
      // route baked the sampled colour into the record instead of recording a
      // policy: a baked record reproduces red for ever.
      Document regraded = od.document;
      Layer adj = makeAdjustmentLayer("invert");
      adj.id = 5;
      Op inv;
      inv.pointKind = PointOpKind::Invert;
      inv.invert.domain = InvertParams::Domain::Display;
      inv.invert.amount = 1.0f;
      adj.ops.add(inv);
      regraded.layers.insert(regraded.layers.begin() + 1, std::move(adj));
      t = strokesLayerTiles(regraded, 2);
      check(t != nullptr && texelOf(*t, 150, 150)[0] < 0.1f &&
                nearly(texelOf(*t, 150, 150)[1], 1.0f) && nearly(texelOf(*t, 150, 150)[2], 1.0f),
            "D6: and a layer added BENEATH changes what the repair reproduces -- red became "
            "cyan, which is the exact thing the DESTRUCTIVE clone route cannot do because "
            "its output is texels");
      check(strokesContentHash(regraded.layers[2].strokes) == contentBefore,
            "D6: with no record edited -- had the evaluation or the recording baked pixels "
            "into them, D6 would hold once and never again");

      // --- J4. PRD F11's round trip: the two halves are ONE list ---------
      const size_t recorded = od.document.layers[1].strokes.dabs.size();
      StrokeSession rub;
      BrushTip wide;
      wide.radius = 24.0f;
      std::string err2;
      const bool rubbing = rub.begin(od, 1, wide, Tool::Eraser, &err2);
      check(rubbing, "D6: an eraser stroke begins on the layer the clone just recorded into");
      if (rubbing) {
        rub.addPoint(150.0f, 150.0f);
        rub.addPoint(158.0f, 150.0f);
        rub.end();
      }
      check(recorded > 0 && od.document.layers[1].strokes.dabs.size() < recorded,
            "D6: and PRD F11's eraser DELETES the records this route appended -- the round "
            "trip is the proof the recording half and the erasing half are the same list");
    }

    // --- J5. a recorded HEAL is not a recorded clone wearing its name ---
    //
    // Same record, same offset, one field different. Over a source that is
    // BRIGHTER than the destination beneath, a clone reproduces the source's
    // own value and a heal reproduces the source's TEXTURE under the
    // destination's ILLUMINATION -- and with a flat source over a flat
    // destination the rim is constant, which `ops/Poisson` section 1 solves
    // exactly, so the healed value is the destination's.
    //
    // Without this assertion a `BelowHealed` record that fell through to the
    // clone's arm in `applyDab()` would be invisible: every count, every id,
    // every offset and the whole regrade property would still be right.
    {
      // 0.05 under, 0.9 in the source region. The dark value is chosen so the
      // display-domain Invert below moves it a long way (0.05 linear is 0.24
      // in display, which inverts to 0.76 and comes back as 0.53) -- a
      // regrade assertion whose two values differ by a rounding error asserts
      // nothing.
      Document lit = baseDocument({0.05f, 0.05f, 0.05f, 1.0f});
      for (int y = 0; y < kH; ++y)
        for (int x = 0; x < 100; ++x) {
          const PixelCoord at{x, y};
          lit.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {0.9f, 0.9f, 0.9f, 1.0f});
        }
      DabRecord cloned = belowDab(150.0f, 150.0f, 8.0f, -118.0f, -118.0f);
      DabRecord healedRec = cloned;
      healedRec.source = DabColorSource::BelowHealed;

      Document asClone = lit;
      asClone.layers[1].strokes.dabs.push_back(cloned);
      strokesForgetAll();
      std::shared_ptr<const TileStore> tc = strokesLayerTiles(asClone, 1);

      Document asHeal = lit;
      asHeal.layers[1].strokes.dabs.push_back(healedRec);
      strokesForgetAll();
      std::shared_ptr<const TileStore> th = strokesLayerTiles(asHeal, 1);

      check(tc != nullptr && nearly(texelOf(*tc, 150, 150)[0], 0.9f),
            "heal: the CLONE record reproduces the source's own value, 0.9 over a 0.2 "
            "ground -- a bright disc with a seam all round it");
      check(th != nullptr && nearly(texelOf(*th, 150, 150)[0], 0.05f) &&
                nearly(texelOf(*th, 150, 150)[3], 1.0f),
            "heal: the HEAL record reproduces the same texture under the DESTINATION's "
            "illumination -- 0.05, seamless, and a gradient-domain solve is the only "
            "thing that gives that answer");

      // And it tracks a regrade too, which is the property the whole kind is
      // for: the heal's SOURCE and its BOUNDARY are both reads of what lies
      // beneath, so both move.
      Document regraded = asHeal;
      Layer adj = makeAdjustmentLayer("invert");
      adj.id = 5;
      Op inv;
      inv.pointKind = PointOpKind::Invert;
      inv.invert.domain = InvertParams::Domain::Display;
      inv.invert.amount = 1.0f;
      adj.ops.add(inv);
      regraded.layers.insert(regraded.layers.begin() + 1, std::move(adj));
      std::shared_ptr<const TileStore> tr = strokesLayerTiles(regraded, 2);
      check(tr != nullptr && texelOf(*tr, 150, 150)[0] > 0.4f,
            "heal: and a recorded heal tracks a regrade beneath as a recorded clone does -- "
            "its boundary is a read of what lies below, not a stored number");
    }

    // --- J6. the heal's source policy survives the file -----------------
    {
      StrokesContent c;
      c.dabs.push_back(belowDab(10.0f, 10.0f, 4.0f, 3.0f, -3.0f));
      c.dabs.back().source = DabColorSource::BelowHealed;
      StrokesContent back;
      std::string err;
      check(deserializeStrokesContent(serializeStrokesContent(c), &back, &err) &&
                back.dabs.size() == 1 &&
                back.dabs[0].source == DabColorSource::BelowHealed,
            "heal: `np:dabs` round-trips the healed source policy -- a reader that mapped "
            "it to Ink would reopen every recorded heal as a black painted dab");
    }

    // --- J7. the end-to-end heal stroke records the healed policy -------
    {
      OpenDocument od;
      od.document = baseDocument({0.2f, 0.2f, 0.2f, 1.0f});
      AppState::CloneSourceState clone{};
      setCloneAnchor(clone, Vec2{32.0f, 32.0f});
      latchCloneOffset(clone, Vec2{150.0f, 150.0f});
      StrokeSession s;
      BrushTip tip;
      tip.radius = 6.0f;
      std::string err;
      if (s.begin(od, 1, tip, Tool::Heal, &err, nullptr, DynamicInputs{}, &clone)) {
        s.addPoint(150.0f, 150.0f);
        s.end();
      }
      const StrokesContent& c = od.document.layers[1].strokes;
      bool allHealed = !c.dabs.empty();
      for (const DabRecord& d : c.dabs) allHealed &= d.source == DabColorSource::BelowHealed;
      check(allHealed,
            "heal: a HEAL stroke records source=BelowHealed and a CLONE stroke records "
            "source=Below -- one route, and the tool is latched into the record rather "
            "than re-read, so a tool switched mid-drag cannot split a stroke's meaning");
    }

    // --- J8. PRD E1 still bounds the stroke, at dab granularity ---------
    {
      OpenDocument od;
      od.document = baseDocument({0.2f, 0.2f, 0.2f, 1.0f});
      od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);
      AppState::CloneSourceState clone{};
      setCloneAnchor(clone, Vec2{32.0f, 32.0f});
      latchCloneOffset(clone, Vec2{150.0f, 150.0f});
      StrokeSession s;
      BrushTip tip;
      tip.radius = 6.0f;
      std::string err;
      if (s.begin(od, 1, tip, Tool::CloneStamp, &err, nullptr, DynamicInputs{}, &clone)) {
        s.addPoint(150.0f, 150.0f);
        s.end();
      }
      check(od.document.layers[1].strokes.dabs.empty(),
            "E1: a dab whose centre is outside the selection records NOTHING -- the unit of "
            "effect is a whole record, so the ants bound this route at dab granularity, "
            "which is stated rather than discovered (StrokeSession section 1d)");
    }
  }

  strokesForgetAll();
  // Named the way every neighbouring suite names itself. The sections above
  // print their letters but not the suite they belong to, so a `FAIL` in a
  // nine-thousand-line log had no word in it to grep back to this file.
  std::printf("[selftest] strokes layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
