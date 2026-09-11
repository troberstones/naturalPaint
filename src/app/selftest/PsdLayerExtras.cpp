#include "app/selftest/Support.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/Mask.hpp"
#include "core/Tile.hpp"
#include "io/PackBits.hpp"
#include "io/PsdLayerExtras.hpp"
#include "io/PsdWrite.hpp"

namespace np {

// io/PsdLayerExtras: the mask block and channel `-2`, and the `lsct` group
// records -- the two parts of a PSD layer section that are not the layer
// (PLAN.md phase 15, docs/psd-export.md).
//
// **What is proved here against what is only exercised.** Three claims in
// this section are the ones that "open a file without error and are
// confidently wrong", so each is asserted in a form that a plausible-looking
// mistake cannot satisfy:
//
//  1. **Record order.** The divider opens a group from BELOW and the header
//     closes and names it, so the divider is written FIRST. Asserted by
//     explicit index, not by "the plan contains a divider and a header" --
//     a reversed writer satisfies the second sentence perfectly.
//  2. **The default colour byte is 255.** Asserted as a hand-computed byte
//     at a hand-computed offset of a hand-computed 24-byte block, not by
//     comparing this module's output with itself. A 0 there hides everything
//     outside the mask rect; our own reader does not read masks yet, so
//     nothing in a round trip could ever see it.
//  3. **The channel is sized by the MASK rect, not the layer rect.** The
//     fixtures below give the mask a rect that is a different size from any
//     plausible layer rect, so a channel sized from the wrong one is the
//     wrong *length* and not merely the wrong pixels.
//
// **Nesting depth is a hand-written fixture and nothing more.** Every group
// in every real Photoshop file this project has examined is depth 0
// (docs/psd-import-gaps.md's own table says so for both files that have
// groups). The depth-2 assertion below is therefore evidence that the
// planner's recursion is self-consistent, and is **not** evidence that
// Photoshop agrees -- its assertion text says so, so a later reader cannot
// mistake it for real-file coverage.

namespace {

Layer makeLayer(const char* name) {
  Layer l;
  l.name = name;
  l.kind = LayerKind::RGB;
  return l;
}

Layer makeGroup(const char* name, const char* tag) {
  Layer l;
  l.name = name;
  l.kind = LayerKind::Group;
  l.groupTag = tag;
  return l;
}

// A layer with an engaged but entirely revealing mask -- core/Mask.hpp's
// "all 1.0" state, whose canonical form is zero tiles.
Layer makeMaskedLayer(const char* name) {
  Layer l = makeLayer(name);
  l.mask.emplace();
  return l;
}

// Hides one document texel by writing `coverage` there.
void setMaskTexel(Layer& l, int32_t x, int32_t y, float coverage) {
  const PixelCoord doc{x, y};
  l.mask->getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), coverage);
}

std::string roleName(PsdRecordRole r) {
  switch (r) {
    case PsdRecordRole::kLayer: return "layer";
    case PsdRecordRole::kGroupDivider: return "divider";
    case PsdRecordRole::kGroupHeader: return "header";
  }
  return "?";
}

// "divider:2 layer:0 layer:1 header:2" -- the whole plan in one comparable
// string, so an order assertion names what it wanted instead of failing on
// an index nobody can read back.
std::string planString(const std::vector<PsdRecordPlan>& plan) {
  std::string s;
  for (size_t i = 0; i < plan.size(); ++i) {
    if (i != 0) s += " ";
    s += roleName(plan[i].role) + ":" + std::to_string(plan[i].layerIndex);
  }
  return s;
}

std::string planOf(const Document& doc, bool& okOut) {
  std::vector<PsdRecordPlan> plan;
  std::string error;
  okOut = planPsdRecords(doc, plan, error);
  return okOut ? planString(plan) : ("REFUSED: " + error);
}

}  // namespace

bool runPsdLayerExtrasTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- A. planPsdRecords(): the order that inverts ----------------------

  {
    // No groups at all: one record per layer, in Document::layers order, and
    // the writer reverses nothing.
    Document doc;
    doc.layers = {makeLayer("bottom"), makeLayer("middle"), makeLayer("top")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "layer:0 layer:1 layer:2",
          "psd groups: an ungrouped document plans one record per layer");
  }

  {
    // One group with two members. core/LayerSetOps' GroupLayers puts the
    // Group layer immediately ABOVE its members, so this is the shape a real
    // grouped document has.
    Document doc;
    Layer m0 = makeLayer("m0");
    Layer m1 = makeLayer("m1");
    m0.parent = "G1";
    m1.parent = "G1";
    doc.layers = {m0, m1, makeGroup("Folder", "G1")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "divider:2 layer:0 layer:1 header:2",
          "psd groups: the DIVIDER is first and the HEADER last");
    // The same claim stated as the thing that goes wrong when it is not
    // true, by index rather than by presence: an inverted writer produces
    // "header:2 layer:0 layer:1 divider:2", which contains exactly one
    // divider and one header and passes any assertion phrased that way.
    std::vector<PsdRecordPlan> plan;
    std::string error;
    check(planPsdRecords(doc, plan, error) && plan.size() == 4 &&
              plan[0].role == PsdRecordRole::kGroupDivider &&
              plan[3].role == PsdRecordRole::kGroupHeader && plan[3].layerIndex == 2,
          "psd groups: index 0 is the divider, index 3 the naming header");
  }

  {
    // Two sibling groups: each pair encloses only its own members, and the
    // lower group closes before the upper one opens.
    Document doc;
    Layer a = makeLayer("a");
    a.parent = "G1";
    Layer b = makeLayer("b");
    b.parent = "G2";
    doc.layers = {a, makeGroup("lower", "G1"), b, makeGroup("upper", "G2")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "divider:1 layer:0 header:1 divider:3 layer:2 header:3",
          "psd groups: two sibling groups nest neither inside the other");
  }

  {
    // A group with no members at all: divider immediately followed by header.
    Document doc;
    doc.layers = {makeLayer("under"), makeGroup("empty", "G1")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "layer:0 divider:1 header:1",
          "psd groups: an empty group is still a divider/header pair");
  }

  {
    // A member that is itself the last layer -- i.e. a top-level layer above
    // the group, so the group's header is NOT the final record. This is the
    // shape a planner that closes every group at end-of-document would get
    // right by accident and this one has to get right on purpose.
    Document doc;
    Layer m = makeLayer("inside");
    m.parent = "G1";
    doc.layers = {m, makeGroup("Folder", "G1"), makeLayer("above")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "divider:1 layer:0 header:1 layer:2",
          "psd groups: a layer above a group is outside it");
  }

  {
    // **A hand-built fixture, and it is the only kind there is for this.**
    // docs/psd-import-gaps.md's table records group nesting depth as 0 in
    // both real files that have groups at all, so nothing below is evidence
    // about Photoshop -- only that the planner's own recursion is
    // self-consistent.
    Document doc;
    Layer inner = makeLayer("inner-member");
    inner.parent = "G2";
    Layer innerGroup = makeGroup("inner", "G2");
    innerGroup.parent = "G1";
    Layer outerMember = makeLayer("outer-member");
    outerMember.parent = "G1";
    doc.layers = {inner, innerGroup, outerMember, makeGroup("outer", "G1")};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned &&
              s == "divider:3 divider:1 layer:0 header:1 layer:2 header:3",
          "psd groups: depth-2 nesting (FIXTURE ONLY -- no real file has it)");
  }

  {
    // Non-contiguous members have no PSD record order: refuse, by name.
    Document doc;
    Layer m0 = makeLayer("m0");
    m0.parent = "G1";
    Layer m1 = makeLayer("m1");
    m1.parent = "G1";
    doc.layers = {m0, makeLayer("interloper"), m1, makeGroup("Folder", "G1")};
    std::vector<PsdRecordPlan> plan;
    std::string error;
    check(!planPsdRecords(doc, plan, error) &&
              error.find("interloper") != std::string::npos,
          "psd groups: a split membership is refused, naming the layer");
  }

  {
    // A dangling parent is NOT a refusal: core::groupAncestry() already
    // treats one as "no more ancestors", and Layer::parent is carried
    // verbatim through this codebase by anything that does not resolve it.
    Document doc;
    Layer m = makeLayer("orphan");
    m.parent = "GONE";
    doc.layers = {m};
    bool planned = false;
    const std::string s = planOf(doc, planned);
    check(planned && s == "layer:0",
          "psd groups: a dangling np:parent plans as a top-level layer");
  }

  {
    // A cycle: two groups each claiming the other as parent.
    Document doc;
    Layer g1 = makeGroup("g1", "G1");
    g1.parent = "G2";
    Layer g2 = makeGroup("g2", "G2");
    g2.parent = "G1";
    doc.layers = {g1, g2};
    std::vector<PsdRecordPlan> plan;
    std::string error;
    check(!planPsdRecords(doc, plan, error) &&
              error.find("cycle") != std::string::npos,
          "psd groups: a cycle of groups is refused by name");
  }

  // --- B. the lsct blocks, byte for byte --------------------------------

  {
    // docs/psd-import-gaps.md section 3's own hexdump of a real divider:
    //   '8BIM' 'lsct' 00 00 00 04 00 00 00 03
    PsdWriter w;
    writePsdLsctBlock(w, PsdRecordRole::kGroupDivider, true);
    const std::vector<uint8_t> b = w.bytes();
    const std::vector<uint8_t> expect = {'8', 'B', 'I', 'M', 'l', 's', 'c', 't',
                                         0,   0,   0,   4,   0,   0,   0,   3};
    check(w.ok() && b == expect, "psd lsct: a divider is the verified 16 bytes, type 3");
  }

  {
    // And a real header:
    //   '8BIM' 'lsct' 00 00 00 10 00 00 00 01 '8BIM' 'pass' 00 00 00 00
    PsdWriter w;
    writePsdLsctBlock(w, PsdRecordRole::kGroupHeader, true);
    const std::vector<uint8_t> b = w.bytes();
    const std::vector<uint8_t> expect = {'8', 'B', 'I', 'M', 'l', 's', 'c', 't',
                                         0,   0,   0,   16,  0,   0,   0,   1,
                                         '8', 'B', 'I', 'M', 'p', 'a', 's', 's',
                                         0,   0,   0,   0};
    check(w.ok() && b == expect, "psd lsct: an open header is type 1 with the 'pass' key");
  }

  {
    PsdWriter w;
    writePsdLsctBlock(w, PsdRecordRole::kGroupHeader, false);
    const std::vector<uint8_t> b = w.bytes();
    check(w.ok() && b.size() == 28 && b[15] == 2 && b[20] == 'p' && b[23] == 's',
          "psd lsct: a closed header is type 2 and still 'pass'");
  }

  {
    // The defined no-op, so a caller can walk a plan without branching.
    PsdWriter w;
    writePsdLsctBlock(w, PsdRecordRole::kLayer, true);
    check(w.ok() && w.size() == 0, "psd lsct: an ordinary layer record gets no block");
  }

  // --- C. mask bounds ---------------------------------------------------

  {
    Layer l = makeLayer("no mask");
    PsdMaskRect r;
    check(!psdMaskRect(l, r), "psd mask: a layer with no mask has no mask block");
  }

  {
    // An engaged, entirely revealing mask -- zero tiles, core/Mask.hpp's
    // canonical "all 1.0". Nothing to tell another application about.
    Layer l = makeMaskedLayer("reveal all");
    PsdMaskRect r;
    check(!psdMaskRect(l, r), "psd mask: an all-1.0 mask writes no mask block");
  }

  {
    // A mask store with an allocated tile that is still entirely revealing:
    // the same answer, via the inverted empty-tile skip rather than via an
    // empty map. A skip written the RGB way round (all-zero) keeps this tile
    // and produces a 128x128 rect of nothing.
    Layer l = makeMaskedLayer("allocated but revealing");
    l.mask->getOrCreate(TileCoord{0, 0});
    PsdMaskRect r;
    check(l.mask->occupiedTileCount() == 1 && !psdMaskRect(l, r),
          "psd mask: an allocated all-reveal tile still writes no block");
  }

  {
    // A small hidden patch: rows 40..41, columns 30..32 hidden. The rect is
    // the tight bounds of coverage that is NOT 1.0, with bottom/right
    // exclusive.
    Layer l = makeMaskedLayer("patch");
    for (int32_t y = 40; y <= 41; ++y)
      for (int32_t x = 30; x <= 32; ++x) setMaskTexel(l, x, y, 0.0f);
    PsdMaskRect r;
    check(psdMaskRect(l, r) && r.top == 40 && r.left == 30 && r.bottom == 42 &&
              r.right == 33 && r.width() == 3 && r.height() == 2,
          "psd mask: the rect is the tight bounds of non-1.0 coverage");
  }

  {
    // A partial coverage value, not a hidden one -- 0.5 is not 1.0 and must
    // be inside the rect. A bounds test written as "coverage == 0" would
    // miss the whole soft edge of every real mask.
    Layer l = makeMaskedLayer("soft");
    setMaskTexel(l, 7, 9, 0.5f);
    PsdMaskRect r;
    check(psdMaskRect(l, r) && r.top == 9 && r.left == 7 && r.bottom == 10 && r.right == 8,
          "psd mask: partial coverage counts, not only fully hidden");
  }

  {
    // The non-1.0 region touching the canvas origin: the rect starts at 0 and
    // is not nudged inward by an off-by-one in the bounds accumulation.
    Layer l = makeMaskedLayer("edge");
    setMaskTexel(l, 0, 0, 0.0f);
    setMaskTexel(l, 5, 5, 0.0f);
    PsdMaskRect r;
    check(psdMaskRect(l, r) && r.top == 0 && r.left == 0 && r.bottom == 6 && r.right == 6,
          "psd mask: a region touching the canvas edge starts at 0");
  }

  {
    // Across a tile boundary, so the bounds come from two tiles and not one:
    // 120..135 spans the 128-texel tile edge in both axes.
    Layer l = makeMaskedLayer("straddle");
    setMaskTexel(l, 120, 120, 0.0f);
    setMaskTexel(l, 135, 135, 0.0f);
    PsdMaskRect r;
    check(psdMaskRect(l, r) && l.mask->occupiedTileCount() == 2 && r.top == 120 &&
              r.left == 120 && r.bottom == 136 && r.right == 136,
          "psd mask: bounds span tiles, not one tile's own extent");
  }

  // --- D. the 20-byte mask record, hand-computed ------------------------

  {
    // Hand-computed rather than captured. The block is a u32 length then
    // twenty bytes:
    //   00 00 00 14   length = 20
    //   00 00 00 28   top    =   40
    //   00 00 00 1E   left   =   30
    //   00 00 00 2A   bottom =   42
    //   00 00 00 21   right  =   33
    //   FF            default colour = 255 -- REVEAL outside the rect
    //   00            flags = 0 (absolute coordinates, mask not disabled)
    //   00 00         padding, which is what makes the record 20 and not 18
    Layer l = makeMaskedLayer("patch");
    for (int32_t y = 40; y <= 41; ++y)
      for (int32_t x = 30; x <= 32; ++x) setMaskTexel(l, x, y, 0.0f);
    PsdMaskRect r;
    const bool has = psdMaskRect(l, r);
    PsdWriter w;
    writePsdMaskBlock(w, has ? &r : nullptr);
    const std::vector<uint8_t> b = w.bytes();
    const std::vector<uint8_t> expect = {0, 0, 0, 0x14, 0, 0, 0, 0x28, 0,    0,    0, 0x1E,
                                         0, 0, 0, 0x2A, 0, 0, 0, 0x21, 0xFF, 0x00, 0, 0};
    check(w.ok() && has && b == expect,
          "psd mask: the 24-byte block is exactly the hand-computed bytes");
    // Stated a second time as the single byte it is, because that is the one
    // this project has been bitten by the inverse of before
    // (naturalpaint-selection-semantics) and because a whole-block compare
    // fails for a dozen reasons while this fails for one.
    check(b.size() == 24 && b[20] == 255,
          "psd mask: the default colour byte is 255 (reveal), never 0");
    check(b.size() == 24 && b[21] == 0 && b[22] == 0 && b[23] == 0,
          "psd mask: flags are 0 (absolute) and the record is 20 bytes");
  }

  {
    PsdWriter w;
    writePsdMaskBlock(w, nullptr);
    const std::vector<uint8_t> b = w.bytes();
    check(w.ok() && b.size() == 4 && b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0,
          "psd mask: no mask still writes the mandatory u32 0");
  }

  // --- E. channel -2, round-tripped through decodePackBits() ------------

  {
    // The property the layer rect cannot satisfy. This layer's mask rect is
    // 3x2; a channel sized from any plausible layer rect (the canvas, the
    // occupied tile, 128x128) decodes to a different number of samples and
    // this fails on LENGTH before it ever fails on content.
    Layer l = makeMaskedLayer("patch");
    for (int32_t y = 40; y <= 41; ++y)
      for (int32_t x = 30; x <= 32; ++x) setMaskTexel(l, x, y, 0.0f);
    // One partial sample inside the rect, so the decoded content is not a
    // constant that a wrong-sized-but-lucky channel could also produce.
    setMaskTexel(l, 31, 40, 0.5f);

    PsdMaskRect r;
    const bool has = psdMaskRect(l, r);
    const std::vector<uint8_t> block = encodePsdMaskChannel(l, r);
    // The block begins with its own u16 compression word, which the layer
    // record's declared channel length includes.
    const bool rle = block.size() >= 2 && block[0] == 0 && block[1] == 1;

    std::vector<uint8_t> decoded;
    const size_t expected = static_cast<size_t>(r.width()) * static_cast<size_t>(r.height());
    const bool decodedOk =
        rle && decodePackBits(block, 2, block.size(), static_cast<uint32_t>(r.height()),
                              expected, decoded);
    check(has && rle && decodedOk && decoded.size() == 6,
          "psd mask channel: RLE, sized by the MASK rect (3x2 = 6 samples)");

    // Row 0 is document y = 40, columns 30..32: hidden, half, hidden.
    // Row 1 is document y = 41: hidden, hidden, hidden.
    const std::vector<uint8_t> expectSamples = {0, 128, 0, 0, 0, 0};
    check(decodedOk && decoded == expectSamples,
          "psd mask channel: samples are round(coverage * 255)");
  }

  {
    // A rect wide enough that PackBits must emit both packet kinds and more
    // than one row-table entry, decoded back through the function two real
    // importers already depend on.
    Layer l = makeMaskedLayer("stripes");
    for (int32_t x = 0; x < 300; ++x) setMaskTexel(l, x, 0, 0.0f);
    for (int32_t x = 0; x < 300; ++x)
      setMaskTexel(l, x, 1, static_cast<float>(x % 251) / 255.0f);
    PsdMaskRect r;
    const bool has = psdMaskRect(l, r);
    const std::vector<uint8_t> block = encodePsdMaskChannel(l, r);
    std::vector<uint8_t> decoded;
    const size_t expected = static_cast<size_t>(r.width()) * static_cast<size_t>(r.height());
    const bool decodedOk =
        block.size() >= 2 &&
        decodePackBits(block, 2, block.size(), static_cast<uint32_t>(r.height()), expected,
                       decoded);
    bool contentOk = decodedOk && decoded.size() == 600;
    if (contentOk) {
      for (int32_t x = 0; x < 300 && contentOk; ++x) contentOk = decoded[x] == 0;
      // Row 1's values pass through binary16 storage, which is exact for
      // n/255 only approximately -- allow the one-count quantisation the
      // half round trip can introduce rather than asserting a bit-exact
      // byte that has nothing to do with what is being tested here.
      for (int32_t x = 0; x < 300 && contentOk; ++x) {
        const int want = x % 251;
        const int got = decoded[300 + static_cast<size_t>(x)];
        contentOk = got >= want - 1 && got <= want + 1;
      }
    }
    check(has && r.width() == 300 && r.height() == 2 && contentOk,
          "psd mask channel: a 300x2 mask round-trips through decodePackBits");
  }

  {
    // A rect whose interior includes texels from tiles that were never
    // allocated: those read 1.0 through core::maskCoverage() and encode as
    // 255, matching the default colour byte for everything outside the rect.
    Layer l = makeMaskedLayer("far corners");
    setMaskTexel(l, 0, 0, 0.0f);
    setMaskTexel(l, 200, 200, 0.0f);
    PsdMaskRect r;
    const bool has = psdMaskRect(l, r);
    const std::vector<uint8_t> block = encodePsdMaskChannel(l, r);
    std::vector<uint8_t> decoded;
    const size_t expected = static_cast<size_t>(r.width()) * static_cast<size_t>(r.height());
    const bool decodedOk =
        block.size() >= 2 &&
        decodePackBits(block, 2, block.size(), static_cast<uint32_t>(r.height()), expected,
                       decoded);
    // (100, 100) falls in a tile the store never allocated.
    const size_t probe = static_cast<size_t>(100 - r.top) * static_cast<size_t>(r.width()) +
                         static_cast<size_t>(100 - r.left);
    check(has && r.width() == 201 && r.height() == 201 && decodedOk &&
              decoded.size() == expected && decoded[probe] == 255 && decoded[0] == 0 &&
              decoded.back() == 0,
          "psd mask channel: an unallocated tile inside the rect reads 255");
  }

  return ok;
}

}  // namespace np
