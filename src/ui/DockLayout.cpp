#include "ui/DockLayout.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

// The dock's extent along the axis its slots divide, and along the one they
// all span in full. Written once so the four sides share one body -- see the
// header's axis note.
struct Axes {
  float major = 0.0f;
  float minor = 0.0f;
};

Axes axesOf(const AtelierRect& dock, DockSide side) {
  if (dockStacksVertically(side)) return Axes{dock.h, dock.w};
  return Axes{dock.w, dock.h};
}

// Place a slot of `extent` at `offset` along the major axis.
AtelierRect rectAt(const AtelierRect& dock, DockSide side, float offset, float extent) {
  if (dockStacksVertically(side)) return AtelierRect{dock.x, dock.y + offset, dock.w, extent};
  return AtelierRect{dock.x + offset, dock.y, extent, dock.h};
}

}  // namespace

DockTiling dockTile(const AtelierRect& dock, DockSide side,
                    const std::vector<DockSlotSpec>& specs) {
  DockTiling out;
  if (specs.empty() || dock.empty()) return out;

  const Axes ax = axesOf(dock, side);
  const size_t n = specs.size();
  const float splitterTotal = static_cast<float>(n - 1) * kDockSplitterThickness;

  // What the collapsed panels take off the top before anything is shared. A
  // collapsed panel is a fixed cost, not a participant.
  float collapsedTotal = 0.0f;
  float weightTotal = 0.0f;
  float minTotal = 0.0f;
  size_t expandedCount = 0;
  for (const DockSlotSpec& s : specs) {
    if (s.collapsed) {
      collapsedTotal += s.headerExtent;
      continue;
    }
    ++expandedCount;
    // A non-positive weight would make this slot's share zero and, worse,
    // could make `weightTotal` zero for the whole dock and divide by it
    // below. Treated as the default rather than refused: a weight is
    // persisted user data (app/PanelLayout), and a file that has been
    // hand-edited to a `0` should land on a sane layout by the same
    // round-trip-repair principle that module applies to every other field.
    weightTotal += (s.weight > 0.0f) ? s.weight : 1.0f;
    minTotal += s.minExtent;
  }

  // The space the expanded panels share.
  const float shareable = ax.major - splitterTotal - collapsedTotal;

  // The honest limit: no distribution of weight can fit the minima. Every
  // expanded slot gets exactly its floor and the tiling reports that it runs
  // past the dock -- see the header. Note this is also the branch a dock of
  // entirely collapsed panels can reach, when even the headers do not fit.
  out.overflowed = shareable < minTotal;

  // Hand out the whole `shareable` amount among the expanded slots.
  //
  // **Not one weighted pass.** A slot whose weighted share falls below its
  // floor has to be raised to that floor, and the extra it takes has to come
  // out of its siblings' shares -- so a single pass that computes every share
  // from the same denominator and then clamps each one upwards produces
  // extents that sum to MORE than `shareable`, and the slots quietly overrun
  // the dock. Worse, it does so in the case `overflowed` reports as fine
  // (`shareable >= minTotal` can hold while one greedy weight still starves a
  // sibling below its floor), so the overrun would be undisclosed as well as
  // wrong.
  //
  // The fix is the standard water-filling loop: repeatedly find the slots
  // whose share is under their floor, PIN those at the floor, remove them from
  // both the space and the weight being divided, and redistribute what is left
  // among the rest. Each round pins at least one slot, so it terminates in at
  // most `expandedCount` rounds. What comes out satisfies both constraints at
  // once -- every slot at or above its floor, and the total exactly
  // `shareable` -- which is the pair the single pass cannot satisfy together.
  std::vector<float> extents(n, 0.0f);
  std::vector<bool> pinned(n, false);
  {
    float freeSpace = shareable;
    float freeWeight = weightTotal;
    size_t freeCount = expandedCount;

    for (size_t i = 0; i < n; ++i)
      if (specs[i].collapsed) extents[i] = specs[i].headerExtent;

    if (out.overflowed) {
      // Nothing to divide: every expanded slot takes its floor and the tiling
      // discloses that the result runs past the dock.
      for (size_t i = 0; i < n; ++i)
        if (!specs[i].collapsed) extents[i] = specs[i].minExtent;
    } else {
      bool pinnedThisRound = true;
      while (pinnedThisRound && freeCount > 0) {
        pinnedThisRound = false;
        for (size_t i = 0; i < n; ++i) {
          if (specs[i].collapsed || pinned[i]) continue;
          const float w = (specs[i].weight > 0.0f) ? specs[i].weight : 1.0f;
          const float share = (freeWeight > 0.0f) ? freeSpace * (w / freeWeight) : 0.0f;
          if (share < specs[i].minExtent) {
            pinned[i] = true;
            pinnedThisRound = true;
            extents[i] = specs[i].minExtent;
            freeSpace -= specs[i].minExtent;
            freeWeight -= w;
            --freeCount;
          }
        }
      }
      // Whatever is still unpinned is at or above its floor by construction.
      // The last of them takes the remainder rather than its own computed
      // share, which is what makes the sum exact rather than exact-to-a-
      // rounding-error -- ui/AtelierLayout.cpp's own trick, applied to a dock.
      size_t seen = 0;
      float assigned = 0.0f;
      for (size_t i = 0; i < n; ++i) {
        if (specs[i].collapsed || pinned[i]) continue;
        ++seen;
        const float w = (specs[i].weight > 0.0f) ? specs[i].weight : 1.0f;
        if (seen == freeCount) {
          extents[i] = freeSpace - assigned;
        } else {
          extents[i] = std::floor(freeSpace * (w / freeWeight));
        }
        assigned += extents[i];
      }
    }
  }

  // Place them, alternating slot and splitter.
  out.slots.reserve(n);
  if (n > 1) out.splitters.reserve(n - 1);
  float offset = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    DockSlot slot;
    slot.rect = rectAt(dock, side, offset, extents[i]);
    slot.collapsed = specs[i].collapsed;
    slot.atMinimum = !specs[i].collapsed && extents[i] <= specs[i].minExtent;
    // `pinned` and the `<=` above answer the same question by two roads --
    // the loop's own record, and the resulting number. They agree except in
    // the overflowed branch (where nothing is pinned but every slot is at its
    // floor), so the number is what is reported and the vector is only the
    // means of producing it.
    out.slots.push_back(slot);
    offset += extents[i];
    if (i + 1 < n) {
      out.splitters.push_back(rectAt(dock, side, offset, kDockSplitterThickness));
      offset += kDockSplitterThickness;
    }
  }
  out.usedExtent = offset;
  return out;
}

DockDragResult dockApplyDrag(float extentA, float extentB, float weightA, float weightB,
                             float minExtent, float deltaPx) {
  DockDragResult r;
  // A drag moves the boundary, so the pair's combined extent is conserved --
  // that conservation is the reason only two weights need to change.
  const float pairExtent = extentA + extentB;
  const float pairWeight = ((weightA > 0.0f) ? weightA : 1.0f) +
                           ((weightB > 0.0f) ? weightB : 1.0f);
  if (pairExtent <= 0.0f || pairWeight <= 0.0f) {
    r.weightA = (weightA > 0.0f) ? weightA : 1.0f;
    r.weightB = (weightB > 0.0f) ? weightB : 1.0f;
    return r;
  }

  // Clamped so neither side crosses its floor. When the pair is too small to
  // hold two minima at all -- possible in the overflowed case -- the clamp
  // collapses to a single point and the drag becomes a no-op, which is the
  // correct behaviour: there is nothing to redistribute.
  const float lo = std::min(minExtent, pairExtent * 0.5f);
  const float hi = std::max(pairExtent - minExtent, pairExtent * 0.5f);
  const float newA = std::clamp(extentA + deltaPx, lo, hi);
  const float newB = pairExtent - newA;

  // Back out the weights from the extents. The pair's total weight is
  // conserved for the same reason its total extent is, so the other slots'
  // weights keep meaning exactly what they meant before the drag.
  r.weightA = pairWeight * (newA / pairExtent);
  r.weightB = pairWeight * (newB / pairExtent);
  // Never zero: a slot with no weight has no boundary left to drag back.
  constexpr float kMinWeight = 0.01f;
  r.weightA = std::max(kMinWeight, r.weightA);
  r.weightB = std::max(kMinWeight, r.weightB);
  return r;
}

DockDropTarget dockDropTargetAt(const AtelierRect& region, float x, float y) {
  DockDropTarget out;
  if (region.empty()) return out;

  // Normalised position inside the region, clamped so a drop that strayed
  // outside still resolves to the nearest edge rather than to nothing -- a
  // pointer a few pixels past the window edge is a person aiming AT that edge.
  const float u = std::clamp((x - region.x) / region.w, 0.0f, 1.0f);
  const float v = std::clamp((y - region.y) / region.h, 0.0f, 1.0f);

  // Distance to each edge, in fractions of the region. Comparing these rather
  // than pixel distances is what makes the corners behave the same shape on a
  // wide window as on a tall one -- see the header.
  const float dLeft = u, dRight = 1.0f - u, dTop = v, dBottom = 1.0f - v;
  const float nearest = std::min(std::min(dLeft, dRight), std::min(dTop, dBottom));
  if (nearest > kDockDropEdgeFraction) return out;  // the middle: flyout

  out.isDock = true;
  if (nearest == dLeft)        out.side = DockSide::Left;
  else if (nearest == dRight)  out.side = DockSide::Right;
  else if (nearest == dTop)    out.side = DockSide::Top;
  else                         out.side = DockSide::Bottom;
  return out;
}

}  // namespace np
