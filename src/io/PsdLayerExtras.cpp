#include "io/PsdLayerExtras.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>

#include "core/Mask.hpp"
#include "core/Tile.hpp"
#include "io/PackBits.hpp"

namespace np {
namespace {

// The Group layer named by `tag`, or nullopt for an empty tag, a dangling one,
// or one that names a non-Group layer.
//
// Deliberately the same shape (and the same linear scan) as
// core/Composite.cpp's own `resolveGroupByTag()`, which is file-local there.
// Duplicating five lines is the lesser evil against promoting a compositor
// internal to a public header purely so an exporter can share it -- but the
// two must agree about what "resolves" means, because a tag this says is
// dangling and that one says is live would export a layer into a group the
// composite does not put it in.
std::optional<size_t> groupIndexForTag(const Document& doc, const std::string& tag) {
  if (tag.empty()) return std::nullopt;
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    if (doc.layers[i].kind == LayerKind::Group && doc.layers[i].groupTag == tag) return i;
  }
  return std::nullopt;
}

// The enclosing groups of `doc.layers[index]`, **outermost first** -- the
// order they have to be opened in.
//
// `cyclic` is set when the walk revisits a group, exactly the condition
// `core::groupAncestry()` guards with its own `visited` set. A dangling tag
// simply ends the chain, which is also what that function does.
struct GroupChain {
  std::vector<size_t> groups;  // indices into doc.layers, outermost first
  bool cyclic = false;
};

GroupChain ancestorGroups(const Document& doc, size_t index) {
  GroupChain chain;
  if (index >= doc.layers.size()) return chain;
  std::unordered_set<size_t> visited;
  std::string tag = doc.layers[index].parent;
  // `visited` alone already terminates this walk -- every iteration either
  // ends the chain or inserts a previously-unseen index from a finite set --
  // and the hop cap is the same belt-and-braces `core::groupAncestry()`
  // carries beside its own set.
  //
  // **The cap is `size() + 1`, not `size()`, and the difference is the whole
  // cycle detection.** After `size()` successful hops every group index has
  // been visited, so the hop that PROVES a cycle is the one after that; a cap
  // of `size()` exits the loop first and reports a cyclic document as an
  // ordinary one. A two-layer fixture of two groups each parented to the
  // other is the smallest case, and `--selftest` carries exactly that.
  for (size_t hops = 0; hops <= doc.layers.size() && !tag.empty(); ++hops) {
    const std::optional<size_t> parent = groupIndexForTag(doc, tag);
    if (!parent.has_value()) break;  // dangling: no more ancestors
    if (!visited.insert(*parent).second) {
      chain.cyclic = true;
      return chain;
    }
    chain.groups.push_back(*parent);
    tag = doc.layers[*parent].parent;
  }
  std::reverse(chain.groups.begin(), chain.groups.end());
  return chain;
}

std::string layerLabel(const Document& doc, size_t index) {
  return "layer " + std::to_string(index) + " (\"" + doc.layers[index].name + "\")";
}

}  // namespace

// ==========================================================================
// Masks
// ==========================================================================

bool psdMaskRect(const Layer& layer, PsdMaskRect& out) {
  if (!layer.mask.has_value()) return false;

  bool any = false;
  int32_t top = std::numeric_limits<int32_t>::max();
  int32_t left = std::numeric_limits<int32_t>::max();
  int32_t bottom = std::numeric_limits<int32_t>::min();
  int32_t right = std::numeric_limits<int32_t>::min();

  for (const auto& [coord, tile] : *layer.mask) {
    // The inverted empty-tile skip. All-`kRevealWord` implies all-1.0, so
    // this can only ever skip a tile the texel loop below would have found
    // nothing in -- never one it would have kept.
    if (tile.isFullyRevealed()) continue;
    const PixelCoord origin = tileOrigin(coord);
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        if (tile.readCoverage(PixelCoord{x, y}) == 1.0f) continue;
        const int32_t docX = origin.x + x;
        const int32_t docY = origin.y + y;
        any = true;
        top = std::min(top, docY);
        left = std::min(left, docX);
        // `bottom`/`right` are exclusive, the convention io/PsdImport.cpp
        // reads back (`width = right - left`), so a single hidden texel at
        // (x, y) gives a 1x1 rect and not a 0x0 one.
        bottom = std::max(bottom, docY + 1);
        right = std::max(right, docX + 1);
      }
    }
  }

  if (!any) return false;
  out.top = top;
  out.left = left;
  out.bottom = bottom;
  out.right = right;
  return true;
}

void writePsdMaskBlock(PsdWriter& w, const PsdMaskRect* rect) {
  if (rect == nullptr) {
    w.u32(0);
    return;
  }
  w.u32(kPsdMaskBlockSize);
  w.i32(rect->top);
  w.i32(rect->left);
  w.i32(rect->bottom);
  w.i32(rect->right);
  w.u8(kPsdMaskDefaultReveal);
  // Flags 0: the rect above is absolute, and this build has no "mask
  // disabled" state to express in bit 1 (core/Mask.hpp: a mask is one
  // coverage store and nothing else).
  w.u8(0);
  // Two bytes of padding, and they are what make the record exactly 20 --
  // io/PsdImport.cpp reads them back as `mc.skip(2)` and refuses any other
  // size by name.
  w.zeros(2);
}

std::vector<uint8_t> encodePsdMaskChannel(const Layer& layer, const PsdMaskRect& rect) {
  std::vector<uint8_t> out;
  // `1` = RLE. Raw would also be legal, and the reader takes both, but every
  // other channel this exporter writes is RLE and a mixed-compression file
  // would be a needless second shape to have to reason about.
  out.push_back(0x00);
  out.push_back(0x01);

  const int32_t width = rect.width();
  const int32_t height = rect.height();
  if (width <= 0 || height <= 0) return out;

  const MaskTileStore* store = layer.mask.has_value() ? &*layer.mask : nullptr;

  std::vector<std::vector<uint8_t>> packed;
  packed.reserve(static_cast<size_t>(height));
  std::vector<uint8_t> row(static_cast<size_t>(width));

  for (int32_t y = 0; y < height; ++y) {
    const int32_t docY = rect.top + y;
    for (int32_t x = 0; x < width; ++x) {
      const int32_t docX = rect.left + x;
      // Through `core::maskCoverage()` -- the one leaf core/Composite's walk
      // and `core::layerMaskCoverageAt()`'s probe both read a masked texel
      // through, so a flattener, an eyedropper and this exporter cannot
      // disagree about a masked pixel. A null tile answers 1.0.
      const PixelCoord doc{docX, docY};
      const MaskTile* tile =
          store == nullptr ? nullptr : store->find(tileCoordAt(doc));
      const float coverage = maskCoverage(tile, tileLocalOffset(doc));
      // Round rather than truncate: the natural inverse a reader applies is
      // `byte / 255.0f`, and truncation would make 1.0 encode as 254 for
      // every value a hair under it.
      row[static_cast<size_t>(x)] =
          static_cast<uint8_t>(std::lround(coverage * 255.0f));
    }
    packed.push_back(encodePackBits(row));
  }

  // PackBits framing: the whole row-count table first, then every row's
  // bytes -- what `decodePackBits()` reads, stated from the writing side.
  for (const std::vector<uint8_t>& p : packed) {
    out.push_back(static_cast<uint8_t>((p.size() >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>(p.size() & 0xFFu));
  }
  for (const std::vector<uint8_t>& p : packed) out.insert(out.end(), p.begin(), p.end());
  return out;
}

// ==========================================================================
// Groups
// ==========================================================================

bool planPsdRecords(const Document& doc, std::vector<PsdRecordPlan>& out, std::string& error) {
  out.clear();
  error.clear();
  out.reserve(doc.layers.size());

  // The groups currently open, outermost first, as indices into doc.layers.
  std::vector<size_t> open;

  for (size_t i = 0; i < doc.layers.size(); ++i) {
    const Layer& layer = doc.layers[i];
    const GroupChain chain = ancestorGroups(doc, i);
    if (chain.cyclic) {
      error = "PSD export: " + layerLabel(doc, i) +
              " is inside a cycle of groups (np:parent chases its own group back to itself), "
              "which has no PSD record order -- refused rather than written as some "
              "arbitrary un-cycling of it";
      return false;
    }

    // Where this layer wants to sit: its ancestors, plus -- for a Group
    // layer -- its own tag, because a Group's record CLOSES its own group and
    // therefore has to be written with that group still open.
    std::vector<size_t> want = chain.groups;
    const bool isGroup = layer.kind == LayerKind::Group && !layer.groupTag.empty();
    if (isGroup) want.push_back(i);

    // Whatever is already open must be a prefix of what this layer wants. A
    // group only ever closes at its own Group layer's record, so an open
    // group that this layer is not inside means the document's members are
    // not contiguous with their group -- and PSD, which encloses members
    // between two records, has no way to say that.
    if (open.size() > want.size() ||
        !std::equal(open.begin(), open.end(), want.begin())) {
      const size_t offending = open.empty() ? i : open.back();
      error = "PSD export: " + layerLabel(doc, i) + " interrupts group \"" +
              doc.layers[offending].name + "\" (np:groupId \"" +
              doc.layers[offending].groupTag +
              "\"), whose members are therefore not a contiguous run -- PSD encloses a "
              "group's members between two records and cannot express a member split off "
              "from the rest, so this is refused rather than silently regrouped";
      return false;
    }

    // Open every group this layer is inside that is not open yet, outermost
    // first. **The divider comes first and opens the group from below** --
    // docs/psd-import-gaps.md section 3, verified on two real files. Reversed,
    // every group's membership inverts and nothing anywhere reports an error.
    for (size_t k = open.size(); k < want.size(); ++k) {
      out.push_back(PsdRecordPlan{PsdRecordRole::kGroupDivider, want[k]});
      open.push_back(want[k]);
    }

    if (isGroup) {
      // ...and the header comes last, closing and naming the group.
      out.push_back(PsdRecordPlan{PsdRecordRole::kGroupHeader, i});
      open.pop_back();
    } else if (layer.kind == LayerKind::Group) {
      // A Group layer with no `groupTag` -- which core/Layer.hpp says cannot
      // happen ("non-empty if and only if kind == LayerKind::Group") but which
      // a hand-built or foreign document can still present. It can hold no
      // members, because nothing can name it, so it plans as an empty group
      // rather than as an ordinary layer: the alternative is exporting a
      // channel-less folder as a blank raster layer.
      out.push_back(PsdRecordPlan{PsdRecordRole::kGroupDivider, i});
      out.push_back(PsdRecordPlan{PsdRecordRole::kGroupHeader, i});
    } else {
      out.push_back(PsdRecordPlan{PsdRecordRole::kLayer, i});
    }
  }

  if (!open.empty()) {
    const size_t offending = open.back();
    error = "PSD export: group \"" + doc.layers[offending].name + "\" (np:groupId \"" +
            doc.layers[offending].groupTag + "\", row " + std::to_string(offending) +
            ") has members above its own row, so its section divider would never be "
            "closed -- refused rather than written as a group that swallows the rest of "
            "the stack";
    return false;
  }
  return true;
}

void writePsdLsctBlock(PsdWriter& w, PsdRecordRole role, bool openFolder) {
  if (role == PsdRecordRole::kLayer) return;  // an ordinary record has none

  w.fourcc("8BIM");
  w.fourcc("lsct");
  const size_t marker = w.beginLengthU32();
  if (role == PsdRecordRole::kGroupDivider) {
    w.u32(3);  // bounding section divider
  } else {
    w.u32(openFolder ? 1u : 2u);  // open / closed folder
    w.fourcc("8BIM");
    // `pass`, not `norm`. core/Composite.hpp makes a group a pass-through
    // fold, and all six groups in the two real files docs/psd-import-gaps.md
    // section 3 dumps are `pass` too.
    w.fourcc("pass");
    // Sub-type 0 (normal, not a scene group). Carried so this block is byte
    // for byte the 16-byte header that document dumped from a real file;
    // io/PsdImport.cpp never reads it.
    w.u32(0);
  }
  w.endLengthU32(marker);
}

}  // namespace np
