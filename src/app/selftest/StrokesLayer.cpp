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
  // A hard disc, so the assertions below can talk about "the centre texel" and
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
    check(blob.rfind("npdabs1:", 0) == 0,
          "npdabs: the payload carries its version in its prefix, so a future npdabs2: is "
          "refused by name rather than half-decoded");
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
    check(!deserializeStrokesContent("npdabs2:00", &untouched, &why) &&
              untouched.nextDabId == 5 && contains(why, "npdabs1:"),
          "npdabs: an unrecognised version tag is refused BY NAME and leaves the output "
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
              strokeRouteFor(Tool::CloneStamp, &target) == StrokeRoute::None,
          "F11: every other tool refuses the kind by name rather than painting nothing in "
          "silence -- there is no recording tool yet to say what a dab would BE");
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

  strokesForgetAll();
  // Named the way every neighbouring suite names itself. The sections above
  // print their letters but not the suite they belong to, so a `FAIL` in a
  // nine-thousand-line log had no word in it to grep back to this file.
  std::printf("[selftest] strokes layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
