#include "app/selftest/Support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/PsdImport.hpp"
#include "core/Composite.hpp"
#include "io/PsdLayerSection.hpp"
#include "io/PsdLayerExtras.hpp"
#include "io/PsdWrite.hpp"

namespace np {

// io/PsdLayerSection -- PSD's Layer and Mask Information section, written
// (docs/psd-export.md tier 2).
//
// **The whole of this section is one round trip through `importPsd()`**, and
// that is a deliberate choice over hand-authored expected bytes. The reader
// it goes through was checked layer-for-layer against three real Photoshop
// files with psd-tools as an oracle (io/PsdImport.hpp records the
// comparison), so agreeing with it is agreeing with something that
// demonstrably agrees with Photoshop -- whereas an expected-bytes fixture
// only ever proves the writer agrees with its own author's reading of the
// spec.
//
// **Two things this shape provably cannot see, stated here rather than
// discovered later.** A round trip is symmetric in exactly the places a
// writer is most likely to be wrong:
//
//   * **The channel table's ORDER.** io/PsdImport.cpp dispatches on each
//     channel's id, never on its position, so writing `0,1,2,-1` instead of
//     the `-1,0,1,2` real Photoshop files carry round-trips perfectly. A
//     deliberate sabotage confirmed exactly that: every assertion below
//     stayed green with the channel order reversed.
//   * **Anything both sides agree about wrongly.** Nothing here is a check
//     on the format; it is a check that the writer inverts THIS reader.
//
// The visible-flag inversion (io/PsdLayerSection.hpp's own header) is the
// classic member of that family and is worth saying is NOT one here: the
// writer's `kFlagHidden` and the reader's are independent constants in
// independent modules, so flipping either polarity alone reddens
// "visibility survives" below. That was measured by sabotage, not assumed --
// docs/psd-export.md predicted this particular round trip would stay green
// and it does not.
//
// What still needs an external oracle is what neither side reads: the
// channel order above, and whether a name/opacity/visibility a psd-tools
// reading of a written file reports matches what was intended. That check
// was run against a file this section can dump (`NP_PSD_LAYER_DUMP`) and
// its numbers are in this landing's report.

namespace {

// Straight linear RGBA over the alpha > 0 texels, plus the occupied-tile
// bounding box -- app/PsdReport.cpp's `measure()` with the tile bounds
// added, and for the identical reason that file gives: geometry and alpha
// counts alone cannot catch a channel swap, an off-by-one stride or a
// missing sRGB encode, and a mean can.
struct Measured {
  bool any = false;
  int32_t left = 0, top = 0, right = 0, bottom = 0;  // half-open, tile-aligned
  size_t covered = 0;
  double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
};

Measured measureStore(const TileStore& tiles) {
  Measured m;
  for (const auto& [coord, tile] : tiles) {
    bool tileHasContent = false;
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const std::array<float, 4> px = tile.readPixel(PixelCoord{x, y});
        if (px[3] <= 0.0f) continue;
        tileHasContent = true;
        ++m.covered;
        const double inv = 1.0 / static_cast<double>(px[3]);
        m.r += static_cast<double>(px[0]) * inv;
        m.g += static_cast<double>(px[1]) * inv;
        m.b += static_cast<double>(px[2]) * inv;
        m.a += static_cast<double>(px[3]);
      }
    }
    if (!tileHasContent) continue;
    const int32_t l = coord.x * kTileSize;
    const int32_t t = coord.y * kTileSize;
    if (!m.any) {
      m.any = true;
      m.left = l;
      m.top = t;
      m.right = l + kTileSize;
      m.bottom = t + kTileSize;
    } else {
      m.left = std::min(m.left, l);
      m.top = std::min(m.top, t);
      m.right = std::max(m.right, l + kTileSize);
      m.bottom = std::max(m.bottom, t + kTileSize);
    }
  }
  if (m.covered > 0) {
    const double n = static_cast<double>(m.covered);
    m.r /= n;
    m.g /= n;
    m.b /= n;
    m.a /= n;
  }
  return m;
}

Measured measureLayer(const Layer& layer) {
  if (!layer.rgbTiles.has_value()) return Measured{};
  return measureStore(*layer.rgbTiles);
}

// Fills a half-open document rectangle with a straight linear colour, stored
// premultiplied -- which is what a `TileStore` holds (core/TileStore.hpp).
void fillRect(Layer& layer, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
              float r, float g, float b, float a) {
  TileStore& tiles = *layer.rgbTiles;
  for (int32_t y = y0; y < y1; ++y) {
    for (int32_t x = x0; x < x1; ++x) {
      const PixelCoord doc{x, y};
      tiles.getOrCreate(tileCoordAt(doc))
          .writePixel(tileLocalOffset(doc), std::array<float, 4>{r * a, g * a, b * a, a});
    }
  }
}

Layer makeRasterLayer(const std::string& name) {
  Layer layer;
  layer.kind = LayerKind::RGB;
  layer.rgbTiles.emplace();
  layer.name = name;
  return layer;
}

// A test-only PSD container: the 26-byte file header, an empty Colour Mode
// Data section, an empty Image Resources section, then the section under
// test.
//
// **The Image Data Section is deliberately absent**, and io/PsdImport.cpp
// never reaches it -- it stops once the layer info section's own declared
// length is consumed. A real export must write one (a PSD with no composite
// opens blank in every application that does not parse layers, which is
// most of them and the whole reason "Maximize Compatibility" exists); that
// is the container's job, not this section's, and building it here would be
// a second, competing copy of it.
std::vector<uint8_t> wrapAsPsd(const Document& doc, PsdLayerSectionResult& sectionOut) {
  PsdWriter w;
  w.fourcc("8BPS");
  w.u16(1);   // version 1 -- never 2 (PSB is refused on read and never written)
  w.zeros(6); // reserved
  w.u16(4);   // channel count: RGB + composite alpha
  w.u32(static_cast<uint32_t>(doc.height));
  w.u32(static_cast<uint32_t>(doc.width));
  w.u16(8);   // 8-bit only -- see io/PsdLayerSection.hpp on `Lr16`
  w.u16(3);   // RGB
  w.u32(0);   // Colour Mode Data
  w.u32(0);   // Image Resources

  sectionOut = writePsdLayerAndMaskInfo(w, doc);
  if (!sectionOut.ok || !w.ok()) return {};
  return w.take();
}

// Round-trips a document and hands back what came out. `ok` false means the
// write or the read refused, and every assertion that depends on it says so
// by failing rather than by reading a default-constructed Document.
struct RoundTrip {
  bool ok = false;
  std::string why;
  std::vector<std::string> warnings;
  std::vector<uint8_t> bytes;
  Document document;
};

RoundTrip roundTrip(const Document& doc) {
  RoundTrip rt;
  PsdLayerSectionResult section;
  rt.bytes = wrapAsPsd(doc, section);
  rt.warnings = section.warnings;
  if (!section.ok) {
    rt.why = "write refused: " + section.error;
    return rt;
  }
  const PsdImportResult imported = importPsd(std::span<const uint8_t>(rt.bytes));
  if (!imported.ok) {
    rt.why = "read refused: " + imported.error;
    return rt;
  }
  rt.ok = true;
  rt.document = imported.document;
  return rt;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// 8-bit quantisation in LINEAR light, not `rgba16float`'s 2.3e-4. Half a
// code at the top of the sRGB curve is ~2.4x wider in linear than in
// encoded, so the bound that holds across the whole range is ~5e-3; 6e-3
// leaves room for the mean of a few thousand samples without leaving room
// for a real error (a channel swap or a missing encode moves a mean by
// tenths, not thousandths).
constexpr double kLinearTol = 6e-3;
constexpr double kAlphaTol = 3e-3;

bool sameColour(const Measured& want, const Measured& got) {
  return near(want.r, got.r, kLinearTol) && near(want.g, got.g, kLinearTol) &&
         near(want.b, got.b, kLinearTol) && near(want.a, got.a, kAlphaTol);
}

bool sameRect(const Measured& want, const Measured& got) {
  return want.any == got.any && want.left == got.left && want.top == got.top &&
         want.right == got.right && want.bottom == got.bottom;
}

// Everything a layer must carry across, in one predicate, so a failure names
// the layer rather than the field. Split from the colour comparison because
// a metadata-only layer (an empty one) has no colour to compare.
bool sameMetadata(const Layer& want, const Layer& got) {
  return want.name == got.name && want.visible == got.visible &&
         want.clipped == got.clipped && want.blend == got.blend &&
         std::fabs(want.opacity - got.opacity) <= 1.0f / 255.0f + 1e-6f;
}

}  // namespace

bool runPsdLayerSectionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- A. One layer ------------------------------------------------------

  {
    Document doc;
    doc.width = 64;
    doc.height = 64;
    Layer only = makeRasterLayer("Background");
    fillRect(only, 0, 0, 64, 64, 0.25f, 0.5f, 0.75f, 1.0f);
    const Measured want = measureLayer(only);
    doc.layers.push_back(std::move(only));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok, "layers: a one-layer document round-trips through importPsd()");
    if (!rt.ok) std::printf("    %s\n", rt.why.c_str());
    check(rt.ok && rt.document.layers.size() == 1,
          "layers: one naturalPaint layer becomes exactly one PSD layer");
    check(rt.ok && rt.document.width == 64 && rt.document.height == 64,
          "layers: the canvas size survives the round trip");
    check(rt.ok && rt.document.layers.size() == 1 &&
              sameMetadata(doc.layers[0], rt.document.layers[0]),
          "layers: name, opacity, visibility, clipping and blend survive");
    check(rt.ok && rt.document.layers.size() == 1 &&
              sameColour(want, measureLayer(rt.document.layers[0])),
          "layers: mean straight linear RGBA survives at 8-bit tolerance");
  }

  // --- B. Several layers: order, names, opacity, visibility, clipping ----
  //
  // **The order assertion is the point of this block.** `Document::layers`
  // index 0 is the bottom of the stack and so is the first PSD layer
  // record; a reversal is the natural first guess in both directions and is
  // wrong in both (io/PsdImport.hpp settles it against psd-tools'
  // compositor and three real files). Distinct names make a reversal a
  // failure rather than a coincidence, which four identically-named layers
  // would not.

  {
    Document doc;
    doc.width = 128;
    doc.height = 128;

    Layer bottom = makeRasterLayer("Bottom");
    fillRect(bottom, 0, 0, 128, 128, 0.8f, 0.1f, 0.1f, 1.0f);

    Layer middle = makeRasterLayer("Middle at 50%");
    middle.opacity = 0.5f;
    middle.blend = "multiply";
    fillRect(middle, 8, 8, 100, 100, 0.1f, 0.8f, 0.2f, 1.0f);

    Layer hidden = makeRasterLayer("Hidden one");
    hidden.visible = false;
    fillRect(hidden, 0, 0, 40, 40, 0.9f, 0.9f, 0.1f, 0.5f);

    Layer clipped = makeRasterLayer("Clipped to the one below");
    clipped.clipped = true;
    fillRect(clipped, 20, 20, 60, 60, 0.2f, 0.2f, 0.9f, 1.0f);

    Layer top = makeRasterLayer("Top");
    top.opacity = 0.125f;
    top.blend = "screen";
    fillRect(top, 64, 64, 128, 128, 0.05f, 0.05f, 0.05f, 0.75f);

    std::vector<Measured> want;
    for (Layer* l : {&bottom, &middle, &hidden, &clipped, &top}) want.push_back(measureLayer(*l));
    doc.layers.push_back(std::move(bottom));
    doc.layers.push_back(std::move(middle));
    doc.layers.push_back(std::move(hidden));
    doc.layers.push_back(std::move(clipped));
    doc.layers.push_back(std::move(top));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok, "layers: a five-layer document round-trips");
    if (!rt.ok) std::printf("    %s\n", rt.why.c_str());

    const bool countOk = rt.ok && rt.document.layers.size() == doc.layers.size();
    check(countOk, "layers: five layers in, five layer records out");

    bool orderOk = countOk;
    bool metaOk = countOk;
    bool colourOk = countOk;
    if (countOk) {
      for (size_t i = 0; i < doc.layers.size(); ++i) {
        if (doc.layers[i].name != rt.document.layers[i].name) orderOk = false;
        if (!sameMetadata(doc.layers[i], rt.document.layers[i])) metaOk = false;
        if (!sameColour(want[i], measureLayer(rt.document.layers[i]))) colourOk = false;
      }
    }
    check(orderOk, "layers: stacking order is preserved, bottom first, not reversed");
    check(metaOk, "layers: every layer's metadata survives, layer for layer");
    check(colourOk, "layers: every layer's mean linear RGBA survives, layer for layer");

    check(countOk && rt.document.layers[2].visible == false &&
              rt.document.layers[0].visible && rt.document.layers[1].visible &&
              rt.document.layers[3].visible && rt.document.layers[4].visible,
          "layers: exactly the hidden layer comes back hidden");
    check(countOk && rt.document.layers[3].clipped && !rt.document.layers[0].clipped &&
              !rt.document.layers[1].clipped && !rt.document.layers[2].clipped &&
              !rt.document.layers[4].clipped,
          "layers: exactly the clipped layer comes back clipped");
    check(countOk && rt.document.layers[1].blend == "multiply" &&
              rt.document.layers[4].blend == "screen" &&
              rt.document.layers[0].blend == kDefaultBlendName,
          "layers: 'mul ' and 'scrn' map back to multiply and screen");
    check(countOk &&
              std::fabs(rt.document.layers[1].opacity - 0.5f) <= 1.0f / 255.0f + 1e-6f &&
              std::fabs(rt.document.layers[4].opacity - 0.125f) <= 1.0f / 255.0f + 1e-6f,
          "layers: a non-255 opacity survives to within one 8-bit code");
  }

  // --- C. A non-ASCII name needs the `luni` block ------------------------
  //
  // The Pascal name carries UTF-8 bytes truncated to 255 and is what a
  // pre-5.0 reader sees; `luni` is what Photoshop itself has preferred
  // since 5.0 and what io/PsdImport.cpp prefers. Writing only the Pascal
  // one would still round-trip a pure-ASCII name, which is why this
  // assertion is not ASCII.

  {
    Document doc;
    doc.width = 32;
    doc.height = 32;
    Layer l = makeRasterLayer("Ωmega — 影 — naïve");
    fillRect(l, 0, 0, 32, 32, 0.4f, 0.4f, 0.4f, 1.0f);
    doc.layers.push_back(std::move(l));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && rt.document.layers.size() == 1 &&
              rt.document.layers[0].name == "Ωmega — 影 — naïve",
          "layers: a non-ASCII name survives through the 'luni' block");
  }

  // --- D. A small patch in a large canvas --------------------------------
  //
  // The rect written is the tight bounding box of occupied TILES, clipped to
  // the canvas -- not the full canvas. A writer that emitted the canvas rect
  // for every layer would still round-trip every assertion in blocks A-C;
  // this is the one that sees it, because the imported layer's occupied
  // tiles are exactly the tiles the writer put samples in.

  {
    Document doc;
    doc.width = 512;
    doc.height = 512;
    Layer patch = makeRasterLayer("A hand-sized patch");
    fillRect(patch, 300, 260, 310, 268, 0.6f, 0.3f, 0.1f, 1.0f);
    const Measured want = measureLayer(patch);
    doc.layers.push_back(std::move(patch));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && want.any && want.left == 256 && want.top == 256 && want.right == 384 &&
              want.bottom == 384,
          "layers: a 10x8 patch occupies exactly one 128px tile");
    check(rt.ok && rt.document.layers.size() == 1 &&
              sameRect(want, measureLayer(rt.document.layers[0])),
          "layers: a small patch does not come back as a canvas-sized layer");
    check(rt.ok && rt.document.layers.size() == 1 &&
              measureLayer(rt.document.layers[0]).covered == want.covered,
          "layers: the covered-pixel count survives exactly");
    check(rt.ok && rt.document.layers.size() == 1 &&
              sameColour(want, measureLayer(rt.document.layers[0])),
          "layers: an offset layer's colour lands at the right coordinates");
    // The whole file for a 512x512 document with one 80-pixel layer must be
    // nearer a kilobyte than a megabyte. A canvas-rect writer would spend
    // 512*512*4 bytes here even after PackBits (a constant row compresses,
    // but 512 rows x 4 channels of row-table alone is 16 KB).
    check(rt.ok && rt.bytes.size() < 4096,
          "layers: a tight rect keeps the whole file under 4 KB");
  }

  // --- E. A fully transparent layer, and a kind with no raster -----------

  {
    Document doc;
    doc.width = 64;
    doc.height = 64;
    Layer solid = makeRasterLayer("Has content");
    fillRect(solid, 0, 0, 64, 64, 0.5f, 0.5f, 0.5f, 1.0f);
    Layer empty = makeRasterLayer("Entirely transparent");
    Layer adjustment;
    adjustment.kind = LayerKind::Adjustment;
    adjustment.name = "An adjustment";
    doc.layers.push_back(std::move(solid));
    doc.layers.push_back(std::move(empty));
    doc.layers.push_back(std::move(adjustment));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok, "layers: a document containing an empty layer round-trips");
    if (!rt.ok) std::printf("    %s\n", rt.why.c_str());
    check(rt.ok && rt.document.layers.size() == 3,
          "layers: an empty layer is still a layer record, not a gap");
    check(rt.ok && rt.document.layers.size() == 3 &&
              rt.document.layers[1].name == "Entirely transparent" &&
              rt.document.layers[2].name == "An adjustment",
          "layers: an empty layer keeps its name and its place in the stack");
    check(rt.ok && rt.document.layers.size() == 3 &&
              !measureLayer(rt.document.layers[1]).any &&
              !measureLayer(rt.document.layers[2]).any,
          "layers: a fully transparent layer allocates no tiles on the way back");

    bool adjustmentWarned = false;
    for (const std::string& warning : rt.warnings)
      if (warning.find("An adjustment") != std::string::npos &&
          warning.find("op stack") != std::string::npos)
        adjustmentWarned = true;
    check(adjustmentWarned,
          "layers: an Adjustment layer warns by name that its op stack was lost");
  }

  // --- F. A Group survives the whole round trip, as a Group --------------
  //
  // **Rewritten at gather, twice, and the second rewrite is the interesting
  // one.** This section originally asserted that a Group wrote NO record and
  // warned that its structure was dropped -- correct while group expansion
  // was a neighbouring track's unmerged work, and stale the moment
  // io/PsdLayerExtras' `planPsdRecords()` was wired in.
  //
  // The obvious replacement was to assert three FLAT records coming back
  // (divider, member, header), on the assumption that our importer does not
  // reconstruct groups. **That assumption was wrong**: io/PsdImport.cpp
  // consumes a divider into a group frame and turns the header into a real
  // `LayerKind::Group` (docs/psd-import-gaps.md section 3 is closed on the
  // read side). So the round trip is a stronger check than a flat record
  // count -- it round-trips group STRUCTURE, and that is what is asserted
  // here instead.
  //
  // The ordering trap still bites, just one level down: PSD records run
  // bottom-first, so the divider opens the group from below and the header
  // closes and names it from above. Emit them the other way round and the
  // importer refuses outright with "a group header appeared with no matching
  // bounding-section divider", which is a much louder failure than the
  // silent membership inversion the same mistake causes in a reader.

  {
    Document doc;
    doc.width = 32;
    doc.height = 32;
    Layer member = makeRasterLayer("Inside the group");
    fillRect(member, 0, 0, 32, 32, 0.3f, 0.3f, 0.3f, 1.0f);
    member.parent = "G1";
    Layer group;
    group.kind = LayerKind::Group;
    group.name = "A folder";
    group.groupTag = "G1";
    group.opacity = 0.5f;
    doc.layers.push_back(std::move(member));
    doc.layers.push_back(std::move(group));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && rt.document.layers.size() == 2,
          "groups: three records in, a member and a Group back out");
    check(rt.ok && rt.document.layers.size() == 2 &&
              rt.document.layers[0].name == "Inside the group" &&
              rt.document.layers[1].name == "A folder" &&
              rt.document.layers[1].kind == LayerKind::Group,
          "groups: the header record comes back as a real Group, not an empty layer");
    check(rt.ok && rt.document.layers.size() == 2 &&
              !rt.document.layers[1].groupTag.empty() &&
              rt.document.layers[0].parent == rt.document.layers[1].groupTag,
          "groups: membership survives -- the member still points at its group");
    check(rt.ok && rt.document.layers.size() == 2 &&
              std::lround(rt.document.layers[1].opacity * 255.0f) == 128,
          "groups: the header record carries the group's own opacity");

    // The old warning must be GONE. A warning saying group structure was
    // dropped, emitted by a build that carries it, is worse than no warning
    // at all -- it sends someone looking for a bug that is fixed.
    bool groupWarned = false;
    for (const std::string& warning : rt.warnings)
      if (warning.find("A folder") != std::string::npos &&
          warning.find("group") != std::string::npos)
        groupWarned = true;
    check(!groupWarned, "groups: a carried group no longer warns that it was dropped");
  }

  // --- G. A mask survives, and the rect it is sized by ---------------------
  //
  // **Added at gather**, because the mask wiring had no assertion at this
  // level at all: io/PsdLayerExtras' own tests cover `psdMaskRect()` and
  // `encodePsdMaskChannel()` in isolation, and this module's tests were
  // written while masks were explicitly out of scope. Neither could see
  // whether a record actually carries the block AND the `-2` channel
  // together -- and a record that writes one without the other declares a
  // channel table that does not match its own extra data, which our reader
  // walks straight off the end of.
  //
  // The rect matters as much as the coverage. A mask channel is sized by the
  // MASK's own rect, not the layer's -- verified on ten real Photoshop masks,
  // every one a different size from its layer (docs/psd-import-gaps.md
  // section 1). The fixture below makes those two deliberately different: a
  // full-canvas layer with a mask that hides four texels in one corner.

  {
    Document doc;
    doc.width = 32;
    doc.height = 32;
    Layer l = makeRasterLayer("Masked");
    fillRect(l, 0, 0, 32, 32, 0.6f, 0.6f, 0.6f, 1.0f);
    l.mask.emplace();
    for (int32_t y = 4; y < 6; ++y)
      for (int32_t x = 4; x < 6; ++x)
        l.mask->getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writeCoverage(tileLocalOffset(PixelCoord{x, y}), 0.0f);
    doc.layers.push_back(std::move(l));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && rt.document.layers.size() == 1 && rt.document.layers[0].mask.has_value(),
          "mask: a masked layer comes back masked, not bare");
    if (rt.ok && !rt.document.layers.empty() && rt.document.layers[0].mask) {
      const Layer& back = rt.document.layers[0];
      check(layerMaskCoverageAt(back, PixelCoord{4, 4}) < 0.01f &&
                layerMaskCoverageAt(back, PixelCoord{5, 5}) < 0.01f,
            "mask: the hidden texels come back hidden");
      check(layerMaskCoverageAt(back, PixelCoord{0, 0}) > 0.99f &&
                layerMaskCoverageAt(back, PixelCoord{20, 20}) > 0.99f,
            "mask: everything outside the mask rect still reveals");
      // The inverted default is the trap this fixture exists for. `MaskTile`
      // defaults to 1.0, so the empty-tile skip inverts relative to a colour
      // tile, and the block's default-colour byte of 255 is what makes the
      // area outside the rect agree with it. Get either backwards and the
      // layer comes back a black rectangle, or entirely gone.
      check(layerMaskCoverageAt(back, PixelCoord{6, 4}) > 0.99f,
            "mask: a texel one past the hidden run is not hidden too");
    }
  }

  // --- G. The two claims that are about bytes, not about a round trip ----

  {
    // Out-of-range linear values clip at 8 bits, and PRD I11 (and
    // io/ExportAs.hpp's precedent) says the warning carries the number.
    Document doc;
    doc.width = 32;
    doc.height = 32;
    Layer bright = makeRasterLayer("Above white");
    fillRect(bright, 0, 0, 4, 4, 3.0f, 1.0f, 1.0f, 1.0f);
    doc.layers.push_back(std::move(bright));

    const RoundTrip rt = roundTrip(doc);
    bool clipWarned = false;
    for (const std::string& warning : rt.warnings)
      if (warning.find("Above white") != std::string::npos &&
          warning.find("16 colour sample") != std::string::npos)
        clipWarned = true;
    check(clipWarned, "layers: clipped highlights are warned with an exact count");
  }

  {
    // `Layer::blend` can hold a name core/Blend does not know (a newer
    // build's mode read from a `.npaint` -- core/Blend.hpp is explicit).
    // That must be a named fallback, never a `fourcc()` of whatever string
    // happened to be there.
    Document doc;
    doc.width = 16;
    doc.height = 16;
    Layer odd = makeRasterLayer("From the future");
    odd.blend = "vivid light";
    fillRect(odd, 0, 0, 16, 16, 0.5f, 0.5f, 0.5f, 1.0f);
    doc.layers.push_back(std::move(odd));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && rt.document.layers.size() == 1 &&
              rt.document.layers[0].blend == kDefaultBlendName,
          "layers: an unmappable blend name falls back to Normal, not to garbage");
    bool blendWarned = false;
    for (const std::string& warning : rt.warnings)
      if (warning.find("From the future") != std::string::npos &&
          warning.find("vivid light") != std::string::npos)
        blendWarned = true;
    check(blendWarned, "layers: an unmappable blend mode is warned by name");
  }

  {
    // **Added at gather, and it exists because nothing else here could
    // see the wiring.** This module shipped with a deliberate three-row
    // blend stub (`norm`/`mul `/`scrn`) while io/PsdBlendKeys was being
    // promoted out of io/PsdImport.cpp's anonymous namespace in a parallel
    // track. Repointing the stub at that table is a one-line change --
    // and every assertion in this file stayed green across it, because the
    // only unmapped-mode test above uses a blend NAME core/Blend does not
    // know, which never reaches the key lookup at all.
    //
    // So the wiring could have been a silent no-op. Overlay is the proof:
    // under the stub it fell back to `norm` with a warning, and under the
    // real table it is `over` and round-trips as Overlay with no warning at
    // all. Losing the wiring reddens this and nothing else.
    Document doc;
    doc.width = 16;
    doc.height = 16;
    Layer lit = makeRasterLayer("Stage two");
    lit.blend = "overlay";
    fillRect(lit, 0, 0, 16, 16, 0.5f, 0.5f, 0.5f, 1.0f);
    doc.layers.push_back(std::move(lit));

    const RoundTrip rt = roundTrip(doc);
    check(rt.ok && rt.document.layers.size() == 1 &&
              rt.document.layers[0].blend == "overlay",
          "layers: Overlay survives as Overlay -- the shared key table is wired in");
    bool warnedAnyway = false;
    for (const std::string& warning : rt.warnings)
      if (warning.find("Stage two") != std::string::npos &&
          warning.find("overlay") != std::string::npos)
        warnedAnyway = true;
    check(!warnedAnyway, "layers: a mode the table DOES carry is not warned about");
  }

  // --- H. The external-oracle dump ---------------------------------------
  //
  // Env-gated and off by default. The round trip above cannot see the
  // channel table's order and could not have seen the flags polarity if the
  // two modules shared a constant; a psd-tools reading of a real file can
  // see both. This writes the file that reading is taken from, so the check
  // is reproducible rather than a number in a commit message.
  if (const char* dumpPath = std::getenv("NP_PSD_LAYER_DUMP")) {
    Document doc;
    doc.width = 96;
    doc.height = 64;

    Layer bottom = makeRasterLayer("Background");
    fillRect(bottom, 0, 0, 96, 64, 0.2f, 0.4f, 0.6f, 1.0f);

    Layer hidden = makeRasterLayer("Hidden middle");
    hidden.visible = false;
    hidden.opacity = 0.5f;
    hidden.blend = "multiply";
    fillRect(hidden, 10, 10, 50, 50, 0.9f, 0.2f, 0.2f, 1.0f);

    Layer top = makeRasterLayer("Top — Ω");
    top.clipped = true;
    fillRect(top, 20, 20, 60, 40, 0.1f, 0.9f, 0.3f, 1.0f);

    doc.layers.push_back(std::move(bottom));
    doc.layers.push_back(std::move(hidden));
    doc.layers.push_back(std::move(top));

    PsdLayerSectionResult section;
    const std::vector<uint8_t> bytes = wrapAsPsd(doc, section);
    std::ofstream out(dumpPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    std::printf("  wrote %zu bytes to %s for the psd-tools oracle\n", bytes.size(), dumpPath);
  }

  return ok;
}

}  // namespace np
