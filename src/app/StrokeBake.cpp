#include "app/StrokeBake.hpp"

#include <algorithm>
#include <cmath>

#include "core/PigmentBake.hpp"

namespace np {

size_t bakePigmentTileFrom(const float* depC, const float* depR, float absorption,
                           PigmentTile& out) {
  if (depC == nullptr || depR == nullptr) return 0;
  size_t written = 0;
  for (int32_t y = 0; y < kTileSize; ++y) {
    for (int32_t x = 0; x < kTileSize; ++x) {
      const size_t i = (static_cast<size_t>(y) * kTileSize + x) * 4;
      if (!(depC[i + 3] > kBakeMassFloor)) continue;
      const std::array<float, 4> c = {depC[i], depC[i + 1], depC[i + 2], depC[i + 3]};
      const std::array<float, 4> r = {depR[i], depR[i + 1], depR[i + 2], depR[i + 3]};
      out.writeTexel(PixelCoord{x, y}, projectSolverTexel(c, r, absorption));
      ++written;
    }
  }
  return written;
}

BakeResult bakePigmentTiles(const PaintSim& sim, Layer& layer, float absorption) {
  BakeResult result;
  if (!layer.pigmentTiles.has_value()) return result;

  const size_t n = sim.pigmentReadbackTileCount();
  for (size_t i = 0; i < n; ++i) {
    const float* depC = sim.pigmentReadbackDepC(i);
    const float* depR = sim.pigmentReadbackDepR(i);
    if (depC == nullptr || depR == nullptr) return result;  // readback not Ready

    // getOrCreate, not a fresh tile: a bake adds to what the layer already
    // holds. Writing into a copy and assigning would drop every texel this
    // bake did not touch, which is most of them.
    const TileCoord at{static_cast<int32_t>(sim.bridgeTileAt(i).x),
                       static_cast<int32_t>(sim.bridgeTileAt(i).y)};
    PigmentTile& tile = layer.pigmentTiles->getOrCreate(at);
    const size_t written = bakePigmentTileFrom(depC, depR, absorption, tile);
    result.texelsWritten += written;
    if (written == 0) {
      ++result.tilesEmpty;
    } else {
      ++result.tilesWritten;
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          result.peakCoverage = std::max(result.peakCoverage, tile.readTexel(PixelCoord{x, y}).mass);
    }
  }
  return result;
}

std::optional<float> absorptionFor(PaintMode mode) {
  switch (mode) {
    case PaintMode::Watercolor: return kAbsorptionWatercolor;
    case PaintMode::Ink: return kAbsorptionInk;
    // Oil has no Beer-Lambert coefficient because it is not a Beer-Lambert
    // medium. Returning one of the other two would bake a height field as if
    // it were a wash, and returning 0 would make `bakedMassFromSim()` yield 0
    // for every texel -- a bake that silently wrote nothing. Neither is better
    // than saying there is no answer.
    case PaintMode::Oil: return std::nullopt;
    case PaintMode::Count: break;
  }
  return std::nullopt;
}

DryTileScan selectDryTiles(const std::vector<PaintSim::TileOccupancy>& occupancy,
                           uint32_t tileCountX, uint32_t tileCountY, float massFloor,
                           float wetnessFloor) {
  DryTileScan scan;
  // The occupancy vector is sized by the sim; a caller passing mismatched
  // counts would otherwise index past the end or silently skip the tail.
  const size_t expected = static_cast<size_t>(tileCountX) * tileCountY;
  if (occupancy.size() != expected) return scan;

  for (uint32_t y = 0; y < tileCountY; ++y) {
    for (uint32_t x = 0; x < tileCountX; ++x) {
      const PaintSim::TileOccupancy& t = occupancy[static_cast<size_t>(y) * tileCountX + x];
      if (!(t.mass > massFloor)) continue;
      // Loaded but not finished. Counted rather than taken -- baking it would
      // drop whatever is still suspended, which is what a *forced* bake is
      // for and this is not one.
      if (t.wetness > wetnessFloor) {
        ++scan.wetHeld;
        continue;
      }
      scan.ready.push_back(PaintSim::BridgeTile{x, y});
    }
  }
  return scan;
}

BakeCycleReport StrokeBakeCycle::step(GpuContext& gpu, PaintSim& sim, OpenDocument* doc,
                                      PaintMode mode, uint64_t frameIndex) {
  BakeCycleReport report;

  // --- The bake half: a readback issued last frame has come back ----------
  //
  // First, deliberately. A cycle already in flight has to finish before a new
  // scan can start (`beginPigmentReadback()` refuses a second one anyway), and
  // draining first keeps the solver's ping-pong halves from holding baked
  // paint any longer than the one frame the deferral costs.
  if (!inFlight_.empty()) {
    const PaintSim::PigmentReadback state = sim.pollPigmentReadback(gpu);
    if (state == PaintSim::PigmentReadback::Submitted) {
      report.action = BakeCycleReport::Action::Idle;
      report.why = "readback still in flight";
      return report;
    }
    if (state != PaintSim::PigmentReadback::Ready) {
      // Failed, or somehow Idle. Drop the cycle and let the next scan offer
      // the same tiles again -- nothing was cleared, so nothing was lost.
      sim.endPigmentReadback();
      inFlight_.clear();
      report.action = BakeCycleReport::Action::Refused;
      report.why = "readback did not complete";
      return report;
    }

    // Everything below can refuse, and every refusal path must leave the
    // solver untouched: the paint is only safe to clear once it is in a layer.
    Layer* layer = (doc != nullptr) ? activeLayerOf(*doc) : nullptr;
    const std::optional<float> absorption = absorptionFor(mode);
    const char* refusal = nullptr;
    if (doc == nullptr) {
      refusal = "no open document";
    } else if (layer == nullptr) {
      refusal = "no active layer";
    } else if (layer->kind != LayerKind::Pigment || !layer->pigmentTiles.has_value()) {
      refusal = "active layer is not a Pigment layer";
    } else if (layer->locked) {
      // Matches app/StrokeSession's routing rule: a locked layer refuses
      // rather than falling through to somewhere the user did not aim.
      refusal = "active layer is locked";
    } else if (!absorption.has_value()) {
      refusal = "this medium does not bake";
    }

    if (refusal != nullptr) {
      sim.endPigmentReadback();
      inFlight_.clear();
      report.action = BakeCycleReport::Action::Refused;
      report.why = refusal;
      return report;
    }

    report.bake = bakePigmentTiles(sim, *layer, *absorption);

    // The clear goes with the bake, in this order and this frame. See
    // StrokeBake.hpp section 1: the two pictures are stacked, so a frame
    // presented between these two lines shows the paint twice.
    sim.clearBakedTiles(gpu, inFlight_);
    sim.endPigmentReadback();

    // recordEdit() is what moves `OpenDocument::revision`, and DocumentTexture
    // caches on it -- without this the freshly baked tiles would sit in the
    // document and never reach the screen. Content rather than Structural:
    // this is paint, so app/Journal writes on its timer rather than within the
    // frame (ADR-0008). It also appends a history entry, which is where the
    // one-bake-one-entry limitation in the header comes from.
    doc->recordEdit("dried paint", EditKind::Content);

    report.action = BakeCycleReport::Action::Baked;
    report.tiles = inFlight_.size();
    inFlight_.clear();
    return report;
  }

  // --- The scan half: is anything dry enough to take? ---------------------
  if (scanned_ && frameIndex - lastScanFrame_ < kScanIntervalFrames) return report;
  lastScanFrame_ = frameIndex;
  scanned_ = true;

  // Scanning before checking the destination is deliberate: `wetHeld` is worth
  // reporting even when nothing could be baked, because "paint is drying" and
  // "there is nowhere to put it" are different things for a UI to say.
  std::vector<PaintSim::TileOccupancy> occupancy;
  if (!sim.readTileOccupancy(gpu, occupancy)) {
    report.why = "no solver fields yet";
    return report;
  }

  const DryTileScan scan = selectDryTiles(occupancy, sim.tileCountX(), sim.tileCountY());
  report.wetHeld = scan.wetHeld;
  if (scan.ready.empty()) {
    report.why = (scan.wetHeld > 0) ? "paint is still wet" : "nothing to bake";
    return report;
  }

  if (!sim.beginPigmentReadback(gpu, scan.ready)) {
    report.action = BakeCycleReport::Action::Refused;
    report.why = "readback refused";
    return report;
  }
  inFlight_ = scan.ready;
  report.action = BakeCycleReport::Action::Submitted;
  report.tiles = inFlight_.size();
  return report;
}

}  // namespace np
