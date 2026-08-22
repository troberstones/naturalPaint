#include "app/selftest/Support.hpp"

#include <chrono>

#include "app/StrokeBake.hpp"
#include "core/PigmentBake.hpp"

// The stroke bridge's dirty-and-drying question (PLAN.md roadmap section 11).
//
// `PaintSim::readTileOccupancy()` reduces the solver's pigment and water
// fields to one `(mass, wetness)` pair per 128x128 document tile, so the bake
// knows which tiles hold paint and which have finished drying without reading
// a megabyte to find out.
//
// The assertion that matters is §2: the reduction's answer is checked against
// a **full-field readback of the same fields, reduced on the CPU**. That is
// the exact answer, computed the expensive way, and it is the only way to know
// the cheap way is not quietly under-reporting -- which is the failure mode
// that would silently lose paint, and the one this whole approach was chosen
// to avoid.

namespace np {

bool runStrokeBridgeTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-74s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // 384 = 3 tiles of 128 on a side, so the tile grid is 3x3 and a stroke can
  // sit inside one tile with untouched neighbours on every side -- which is
  // what makes "reports exactly the tiles it touched" a statement with
  // negative cases in it, not just positive ones.
  constexpr uint32_t kW = 384, kH = 384;
  MixboxLut lut;
  const bool lutOk = lut.load(NP_MIXBOX_LUT);
  check(lutOk, "bridge: the Mixbox LUT loaded -- the solver transports real latents");

  PaintSim sim;
  if (!sim.init(gpu, kW, kH, lut)) {
    std::printf("  stroke bridge: PaintSim::init FAILED\n");
    std::printf("[selftest] stroke bridge FAIL\n");
    return false;
  }
  sim.setMode(gpu, PaintMode::Watercolor);
  sim.clearCanvas(gpu);

  // ======================================================================
  // 1. The grid lines up with the document's tiles.
  // ======================================================================
  std::printf("  -- 1. one entry per document tile --\n");
  {
    check(PaintSim::kBridgeTile == kTileSize,
          "bridge: the reduction's tile edge IS core/Tile's -- one entry, one document tile");
    check(sim.tileCountX() == 3 && sim.tileCountY() == 3,
          "bridge: a 384x384 canvas reduces to a 3x3 tile grid");

    std::vector<PaintSim::TileOccupancy> occ;
    check(sim.readTileOccupancy(gpu, occ), "bridge: the occupancy pass ran and read back");
    check(occ.size() == 9, "bridge: nine entries, in row-major tile order");
    std::printf("  [selftest] bridge: %zu tiles, %zu bytes read back per query -- the payload "
                "grows as (W/128)*(H/128), so it stays at the transfer floor\n",
                occ.size(), occ.size() * 2 * sizeof(float));

    bool allEmpty = true;
    for (const auto& t : occ) allEmpty = allEmpty && t.mass == 0.0f && t.wetness == 0.0f;
    check(allEmpty, "bridge: a cleared canvas reports zero mass and zero wetness everywhere");
  }

  // ======================================================================
  // 2. The cheap answer equals the expensive one.
  // ======================================================================
  //
  // A stroke inside the centre tile, then the reduction compared against a
  // full readback of depC/pigC/water/sat reduced per tile on the CPU. Nothing
  // here trusts the shader's arithmetic; it recomputes it.
  std::printf("  -- 2. the reduction vs a full-field readback, reduced on the CPU --\n");
  {
    SimParams p{};
    p.brushRadius = 14.0f;
    p.brushWater = 1.4f;
    p.brushPigment = 0.9f;
    // Diagonally across the middle tile (128..256), staying clear of its edges
    // so advection has room to spread without necessarily crossing into a
    // neighbour -- whether it does is what §3 measures rather than assumes.
    stroke(gpu, sim, p, 160.0f, 160.0f, 224.0f, 224.0f, 8);
    settle(gpu, sim, p, 4);

    std::vector<PaintSim::TileOccupancy> occ;
    check(sim.readTileOccupancy(gpu, occ), "bridge: occupancy read back after a real stroke");

    // The expensive answer.
    std::vector<float> depC, pigC, water, sat;
    const bool fields = sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float,
                                          depC);
    check(fields, "bridge: full-field readback of depC for the comparison");

    if (fields && occ.size() == 9) {
      // Reduce depC.w the same way the shader does -- max over each tile.
      // pigC is folded in by the shader at the suspended weight; this half of
      // the comparison uses depC alone and is therefore a LOWER bound on what
      // the shader must report. Asserting `>=` rather than `==` is deliberate:
      // it catches under-reporting, which is the failure that loses paint,
      // without pretending this CPU pass models the suspended term too.
      std::vector<float> cpuMax(9, 0.0f);
      for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
          const size_t i = (static_cast<size_t>(y) * kW + x) * 4 + 3;
          const size_t tile = (y / PaintSim::kBridgeTile) * 3 + (x / PaintSim::kBridgeTile);
          cpuMax[tile] = std::max(cpuMax[tile], depC[i]);
        }
      }

      bool bounds = true, painted = false, emptyStayedEmpty = true;
      for (size_t t = 0; t < 9; ++t) {
        // The shader reports deposited + 0.75*suspended, so it must be at
        // least the deposited maximum, and never less.
        bounds = bounds && occ[t].mass >= cpuMax[t] - 1e-5f;
        if (cpuMax[t] > 1e-4f) painted = true;
        // And a tile the CPU says holds nothing must not be reported as
        // holding a lot -- the suspended term can only add what is really
        // there, so a large report over an empty tile means the indexing is
        // wrong.
        if (cpuMax[t] == 0.0f) emptyStayedEmpty = emptyStayedEmpty && occ[t].mass < 1.0f;
      }
      check(painted, "bridge: the stroke actually deposited something -- the fixture is real");
      check(bounds,
            "bridge: every tile's reported mass is at least the deposited maximum -- it "
            "cannot under-report, which is the failure that would lose paint");
      check(emptyStayedEmpty,
            "bridge: and a tile the full readback says is empty is not reported as loaded");

      std::printf("  [selftest] bridge: per-tile mass, reduction vs full readback of depC:\n");
      for (uint32_t ty = 0; ty < 3; ++ty) {
        std::printf("    ");
        for (uint32_t tx = 0; tx < 3; ++tx) {
          const size_t t = ty * 3 + tx;
          std::printf("%7.4f/%-7.4f ", static_cast<double>(occ[t].mass),
                      static_cast<double>(cpuMax[t]));
        }
        std::printf("\n");
      }

      // The centre tile is where the stroke was drawn, so it must be the one
      // carrying the most. A transposed tile index would put the maximum in
      // the same place for a symmetric stroke, which is why the stroke is
      // diagonal and the check below also looks at an asymmetric pair.
      const size_t centre = 4;
      bool centreLargest = true;
      for (size_t t = 0; t < 9; ++t)
        if (t != centre) centreLargest = centreLargest && occ[centre].mass >= occ[t].mass;
      check(centreLargest, "bridge: the tile the stroke was drawn in carries the most mass");
    }
  }

  // ======================================================================
  // 3. Wetness falls, and it is what tells the bake when to fire.
  // ======================================================================
  std::printf("  -- 3. the drying half of the same 8 bytes --\n");
  {
    std::vector<PaintSim::TileOccupancy> wet;
    check(sim.readTileOccupancy(gpu, wet), "bridge: occupancy read back while still wet");
    float wettest = 0.0f;
    for (const auto& t : wet) wettest = std::max(wettest, t.wetness);
    check(wettest > 0.0f, "bridge: a freshly painted canvas reports wetness somewhere");

    SimParams p{};
    settle(gpu, sim, p, 240);
    std::vector<PaintSim::TileOccupancy> dry;
    check(sim.readTileOccupancy(gpu, dry), "bridge: and again after a long settle");
    float stillWet = 0.0f, massAfter = 0.0f;
    for (const auto& t : dry) {
      stillWet = std::max(stillWet, t.wetness);
      massAfter = std::max(massAfter, t.mass);
    }
    std::printf("  [selftest] bridge: wettest tile %.5f -> %.5f over 240 settle frames, while "
                "its pigment mass held at %.4f\n",
                static_cast<double>(wettest), static_cast<double>(stillWet),
                static_cast<double>(massAfter));
    check(stillWet < wettest,
          "bridge: wetness falls as the sheet dries -- the signal the bake waits on");
    check(massAfter > 1e-4f,
          "bridge: and the pigment does NOT go with it -- drying settles paint, it does not "
          "remove it");
  }

  // ======================================================================
  // 4. The bridge end to end, on a real stroke.
  // ======================================================================
  //
  // The occupancy pass names a tile; the full readback supplies that tile's
  // solver texels; core/PigmentBake turns them into document texels. This is
  // the whole path, and what it proves is that the two halves fit: the tile
  // the cheap query pointed at really does contain the paint, and that paint
  // really does become a PigmentTexel a layer could hold.
  std::printf("  -- 4. occupancy -> readback -> a PigmentTexel a layer could hold --\n");
  {
    std::vector<PaintSim::TileOccupancy> occ;
    std::vector<float> depC;
    if (sim.readTileOccupancy(gpu, occ) &&
        sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float, depC) &&
        occ.size() == 9) {
      size_t best = 0;
      for (size_t t = 1; t < occ.size(); ++t)
        if (occ[t].mass > occ[best].mass) best = t;

      const uint32_t tx = static_cast<uint32_t>(best % 3), ty = static_cast<uint32_t>(best / 3);
      size_t baked = 0, opaqueEnough = 0;
      float peakCoverage = 0.0f;
      bool inRange = true, finite = true;
      for (uint32_t y = ty * PaintSim::kBridgeTile; y < (ty + 1) * PaintSim::kBridgeTile; ++y) {
        for (uint32_t x = tx * PaintSim::kBridgeTile; x < (tx + 1) * PaintSim::kBridgeTile; ++x) {
          const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
          const std::array<float, 4> dc = {depC[i], depC[i + 1], depC[i + 2], depC[i + 3]};
          const PigmentTexel texel = projectSolverTexel(dc, {0, 0, 0, 0}, kAbsorptionWatercolor);
          inRange = inRange && texel.mass >= 0.0f && texel.mass <= 1.0f;
          finite = finite && std::isfinite(texel.latent.c[0]) && std::isfinite(texel.mass);
          if (texel.mass > 1e-4f) ++baked;
          if (texel.mass > 0.5f) ++opaqueEnough;
          peakCoverage = std::max(peakCoverage, texel.mass);
        }
      }
      std::printf("  [selftest] bridge: tile (%u,%u) -- %zu of %u texels carry paint, %zu past "
                  "half coverage, peak %.4f\n",
                  tx, ty, baked, PaintSim::kBridgeTile * PaintSim::kBridgeTile, opaqueEnough,
                  static_cast<double>(peakCoverage));
      check(baked > 0, "bridge: the named tile really does contain paint");
      check(inRange,
            "bridge: every baked texel's mass is inside [0,1] -- which mixLatents() requires "
            "and a raw solver mass would violate");
      check(finite, "bridge: and none of them is NaN or infinite, including the empty ones");
    } else {
      check(false, "bridge: the end-to-end readback ran");
    }
  }

  // ======================================================================
  // 5. The tile payload, deferred -- and bit-identical to the blocking read.
  // ======================================================================
  //
  // The poll must not block, and what it eventually hands back must be exactly
  // what a full-field readback of the same texels contains. The second half is
  // the one that matters: a per-tile copy gets its origin, its row stride and
  // its buffer offset all from separate arithmetic, and any of the three being
  // wrong produces plausible-looking pigment from the wrong place.
  std::printf("  -- 5. the deferred tile payload --\n");
  {
    std::vector<PaintSim::TileOccupancy> occ;
    sim.readTileOccupancy(gpu, occ);
    size_t best = 0;
    for (size_t t = 1; t < occ.size(); ++t)
      if (occ[t].mass > occ[best].mass) best = t;
    const PaintSim::BridgeTile want{static_cast<uint32_t>(best % 3),
                                    static_cast<uint32_t>(best / 3)};

    check(!sim.beginPigmentReadback(gpu, {}), "bridge: an empty tile list is refused");
    check(!sim.beginPigmentReadback(gpu, {{99, 0}}),
          "bridge: a tile outside the canvas is refused rather than read from nowhere");

    check(sim.beginPigmentReadback(gpu, {want}), "bridge: the readback was issued");
    check(!sim.beginPigmentReadback(gpu, {want}),
          "bridge: a second readback while one is in flight is refused -- one at a time, so a "
          "failure belongs to a known request");

    // Non-blocking, and the two halves of that are measured separately.
    //
    // FIRST: one poll costs essentially nothing, because it pumps the instance
    // once and returns whatever it finds. That is the property the design
    // turns on -- a caller polls once a frame and never stalls.
    //
    // SECOND: readiness arrives when the GPU finishes, which is a WALL-CLOCK
    // wait, not a number of polls. Bounding this loop by an iteration count
    // was my own bug and it is worth naming: 1000 polls complete in
    // microseconds and the copy takes milliseconds, so the budget expired long
    // before the GPU could possibly have finished, and abandoning a pending
    // map left the next submit to fail validation on a still-mapped buffer.
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    PaintSim::PigmentReadback st = sim.pollPigmentReadback(gpu);
    const double firstPollMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    long long polls = 1;
    while (st == PaintSim::PigmentReadback::Submitted &&
           std::chrono::duration<double>(Clock::now() - started).count() < 5.0) {
      ++polls;
      st = sim.pollPigmentReadback(gpu);
    }
    const double waitedMs =
        std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::printf("  [selftest] bridge: [measured] one poll %.4f ms; ready after %lld poll(s) "
                "over %.2f ms of wall clock -- the wait is the GPU's, and no single poll "
                "carries it\n",
                firstPollMs, polls, waitedMs);
    check(st == PaintSim::PigmentReadback::Ready, "bridge: the readback reached Ready");
    check(firstPollMs < 1.0,
          "bridge: a single poll costs well under a millisecond -- it pumps once and returns, "
          "which is what lets a caller poll every frame");

    // The comparison. A full-field readback of depC, indexed at the same
    // texels, must match the tile copy bit for bit -- not to a tolerance.
    std::vector<float> full;
    const bool got =
        sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float, full);
    check(got, "bridge: full-field readback for the comparison");
    const float* tileC = sim.pigmentReadbackDepC(0);
    check(tileC != nullptr, "bridge: the mapped depC pointer is valid while Ready");

    if (got && tileC != nullptr) {
      size_t mismatches = 0, nonZero = 0;
      for (uint32_t y = 0; y < PaintSim::kBridgeTile; ++y) {
        for (uint32_t x = 0; x < PaintSim::kBridgeTile; ++x) {
          const size_t inTile = (static_cast<size_t>(y) * PaintSim::kBridgeTile + x) * 4;
          const size_t inFull =
              ((static_cast<size_t>(want.y * PaintSim::kBridgeTile + y) * kW) +
               (want.x * PaintSim::kBridgeTile + x)) * 4;
          for (int c = 0; c < 4; ++c)
            if (tileC[inTile + c] != full[inFull + c]) ++mismatches;
          if (tileC[inTile + 3] > 0.0f) ++nonZero;
        }
      }
      std::printf("  [selftest] bridge: %zu of %u texels in the tile carry mass; %zu channel "
                  "mismatches against the full-field read\n",
                  nonZero, PaintSim::kBridgeTile * PaintSim::kBridgeTile, mismatches);
      check(mismatches == 0,
            "bridge: the tile copy is BIT-IDENTICAL to the full read -- origin, stride and "
            "offset all agree");
      check(nonZero > 0, "bridge: and the tile it copied is not an empty one");
    }
  }

  // ======================================================================
  // 6. The bake: solver texels become tiles a layer holds.
  // ======================================================================
  std::printf("  -- 6. the bake, into a real Pigment layer --\n");
  {
    Layer layer = makePigmentLayer("baked wash");
    check(layer.pigmentTiles.has_value(), "bridge: the target is a Pigment layer");

    const BakeResult r = bakePigmentTiles(sim, layer, kAbsorptionWatercolor);
    std::printf("  [selftest] bridge: baked %zu tile(s), %zu texel(s), peak coverage %.4f, "
                "%zu tile(s) had nothing above the floor\n",
                r.tilesWritten, r.texelsWritten, static_cast<double>(r.peakCoverage),
                r.tilesEmpty);
    check(r.texelsWritten > 0, "bridge: the bake wrote texels into the layer");
    check(r.peakCoverage > 0.0f && r.peakCoverage <= 1.0f,
          "bridge: and every one of them is a coverage in [0,1]");

    // The layer now holds what core/PigmentBake says the solver texels mean.
    // Recomputed here from the mapped floats rather than trusted, so this
    // compares the stored tile against the mapping, not against itself.
    const float* depC = sim.pigmentReadbackDepC(0);
    const float* depR = sim.pigmentReadbackDepR(0);
    const PaintSim::BridgeTile at = sim.bridgeTileAt(0);
    const PigmentTile* stored =
        layer.pigmentTiles->find(TileCoord{static_cast<int32_t>(at.x), static_cast<int32_t>(at.y)});
    check(stored != nullptr, "bridge: the tile landed at the coordinate it came from");

    if (stored != nullptr && depC != nullptr && depR != nullptr) {
      float worst = 0.0f;
      size_t compared = 0;
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const size_t i = (static_cast<size_t>(y) * kTileSize + x) * 4;
          if (!(depC[i + 3] > 1e-4f)) continue;
          const PigmentTexel expect = projectSolverTexel(
              {depC[i], depC[i + 1], depC[i + 2], depC[i + 3]},
              {depR[i], depR[i + 1], depR[i + 2], depR[i + 3]}, kAbsorptionWatercolor);
          const PigmentTexel got = stored->readTexel(PixelCoord{x, y});
          worst = std::max(worst, std::fabs(got.mass - expect.mass));
          for (size_t c = 0; c < 3; ++c)
            worst = std::max(worst, std::fabs(got.latent.c[c] - expect.latent.c[c]));
          ++compared;
        }
      }
      std::printf("  [selftest] bridge: %zu baked texels compared against core/PigmentBake, "
                  "worst channel error %.3e (f16 storage gives ~4.9e-4 relative)\n",
                  compared, static_cast<double>(worst));
      check(worst < 1e-3f,
            "bridge: every stored texel IS what core/PigmentBake says its solver texel means");
    }

    // A bake ADDS. Texels below the floor are skipped rather than stamped as
    // transparent, so a second bake over a tile that already holds paint
    // cannot erase what the first one put there -- which a whole-tile assign
    // would, for every texel this stroke did not touch.
    if (stored != nullptr) {
      const PixelCoord corner{0, 0};
      PigmentTexel sentinel;
      sentinel.mass = 0.75f;
      sentinel.latent.c = {0.1f, 0.2f, 0.3f};
      layer.pigmentTiles->getOrCreate(
          TileCoord{static_cast<int32_t>(at.x), static_cast<int32_t>(at.y)})
          .writeTexel(corner, sentinel);
      const bool cornerWasEmpty = !(depC[3] > 1e-4f);
      bakePigmentTiles(sim, layer, kAbsorptionWatercolor);
      const PigmentTexel after =
          layer.pigmentTiles
              ->find(TileCoord{static_cast<int32_t>(at.x), static_cast<int32_t>(at.y)})
              ->readTexel(corner);
      check(!cornerWasEmpty || std::fabs(after.mass - 0.75f) < 1e-3f,
            "bridge: re-baking does not erase paint the solver had nothing to say about");
    }

    sim.endPigmentReadback();
    check(sim.pigmentReadbackDepC(0) == nullptr,
          "bridge: after endPigmentReadback the mapped pointer is gone -- no reading a buffer "
          "that has been unmapped");
    check(sim.beginPigmentReadback(gpu, {{1, 1}}),
          "bridge: and the machine is back at Idle, so a new readback can start");
    sim.endPigmentReadback();
  }

  // ======================================================================
  // 7. clearBakedTiles: the sim stops holding what the document now does.
  // ======================================================================
  //
  // §6 baked a tile into `layer` by reading depC_/depR_ and left the sim
  // untouched. If nothing clears it, the sim keeps carrying that same mass
  // -- and, in the forced-bake-while-wet case StrokeBake.hpp documents,
  // whatever pigC_/pigR_ mass the bake silently dropped too -- so a caller
  // that later draws the document over the sim canvas would composite a
  // partial-coverage edge texel twice. Checked directly against depC_/depR_/
  // pigC_/pigR_ (via the *ForDiag() accessors §2/§4/§5 already use for the
  // same reason) rather than through the composited canvas: canvas_ is a
  // cached render target that only updates when frame()'s composite pass
  // runs, so reading it right after a GPU texture write and nothing else
  // would just prove the cache is stale, not that the clear happened.
  std::printf("  -- 7. clearBakedTiles: the sim stops holding what the document now does --\n");
  {
    // Recomputed independently rather than reusing §6's `at`: §6 ends by
    // issuing and then ending a readback of its own, which clears
    // readbackTiles_ -- bridgeTileAt(0) would answer for that call, not for
    // the tile actually baked, if this block trusted it instead.
    std::vector<PaintSim::TileOccupancy> occ;
    check(sim.readTileOccupancy(gpu, occ), "bridge: occupancy read back for §7's target tile");
    size_t best = 0;
    for (size_t t = 1; t < occ.size(); ++t)
      if (occ[t].mass > occ[best].mass) best = t;
    const PaintSim::BridgeTile at{static_cast<uint32_t>(best % 3), static_cast<uint32_t>(best / 3)};
    const uint32_t tx0 = at.x * PaintSim::kBridgeTile, ty0 = at.y * PaintSim::kBridgeTile;

    // Every mass channel (.w, index 3) inside the tile, across all four
    // fields at once -- the largest of the four is what matters: clearing
    // is only proven when NONE of them still carries paint there.
    auto tileMaxMass = [&](GpuContext& g, WGPUTexture depCTex, WGPUTexture depRTex,
                           WGPUTexture pigCTex, WGPUTexture pigRTex) -> float {
      float worst = 0.0f;
      for (WGPUTexture tex : {depCTex, depRTex, pigCTex, pigRTex}) {
        std::vector<float> field;
        if (!sim.readbackField(g, tex, WGPUTextureFormat_RGBA32Float, field)) continue;
        for (uint32_t y = ty0; y < ty0 + PaintSim::kBridgeTile; ++y)
          for (uint32_t x = tx0; x < tx0 + PaintSim::kBridgeTile; ++x)
            worst = std::max(worst, field[(static_cast<size_t>(y) * kW + x) * 4 + 3]);
      }
      return worst;
    };
    // Everything OUTSIDE the tile, for depC_ alone: it is what the bake
    // reads and what section 2's cross-check already trusts, so it stands
    // in for "did this touch anything it should not have" without paying
    // for all four fields' full-canvas readback three more times.
    auto outsideTileIdentical = [&](const std::vector<float>& a, const std::vector<float>& b) {
      for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
          if (y >= ty0 && y < ty0 + PaintSim::kBridgeTile && x >= tx0 &&
              x < tx0 + PaintSim::kBridgeTile)
            continue;
          const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
          for (int c = 0; c < 4; ++c)
            if (a[i + static_cast<size_t>(c)] != b[i + static_cast<size_t>(c)]) return false;
        }
      return true;
    };

    std::vector<float> depCBefore;
    check(sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float, depCBefore),
          "bridge: depC_ readback before clearing");
    const float massBefore =
        tileMaxMass(gpu, sim.depCTexForDiag(), sim.depRTexForDiag(), sim.pigCTexForDiag(),
                    sim.pigRTexForDiag());
    check(massBefore > 1e-4f,
          "bridge: before clearing, the baked tile still carries mass -- there really is "
          "something for this call to clear");

    check(!sim.clearBakedTiles(gpu, {{99, 0}}),
          "bridge: clearBakedTiles refuses a tile outside the canvas, same as beginPigmentReadback");
    check(sim.clearBakedTiles(gpu, {}), "bridge: an empty tile list is a no-op, not a refusal");

    std::vector<float> depCUnaffected;
    check(sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float,
                            depCUnaffected),
          "bridge: depC_ readback after the refused call and the no-op");
    check(depCUnaffected == depCBefore,
          "bridge: neither the refused call nor the no-op changed a single texel");

    check(sim.clearBakedTiles(gpu, {at}), "bridge: clearBakedTiles accepted the real tile");

    const float massAfter =
        tileMaxMass(gpu, sim.depCTexForDiag(), sim.depRTexForDiag(), sim.pigCTexForDiag(),
                    sim.pigRTexForDiag());
    std::printf("  [selftest] bridge: tile (%u,%u) peak mass across depC/depR/pigC/pigR -- "
                "%.4f before clearing, %.2e after\n",
                at.x, at.y, static_cast<double>(massBefore), static_cast<double>(massAfter));
    check(massAfter == 0.0f,
          "bridge: after clearing, every mass channel in the tile is exactly zero -- an upload, "
          "not an approximation");

    std::vector<float> depCAfter;
    check(sim.readbackField(gpu, sim.depCTexForDiag(), WGPUTextureFormat_RGBA32Float, depCAfter),
          "bridge: depC_ readback after clearing, for the outside-tile comparison");
    check(outsideTileIdentical(depCAfter, depCBefore),
          "bridge: and every depC_ texel outside that one tile is untouched, bit for bit");

    // Force a flip. frame() runs `substeps` physics steps and each one flips
    // depC_/depR_/pigC_/pigR_'s parity once, so the default substeps == 2
    // returns to the SAME half it started on -- an even count would let a
    // half that was never actually cleared go untested. One substep moves
    // the parity exactly once: if clearBakedTiles() had only zeroed whichever
    // half was current when it ran, this is where the old paint reappears.
    // *ForDiag() always answers for `.srcTex()` = the current half, so this
    // read is automatically looking at the OTHER texture object than every
    // read above it.
    sim.substeps = 1;
    SimParams p{};
    settle(gpu, sim, p, 1);
    sim.substeps = 2;

    const float massAfterFlip =
        tileMaxMass(gpu, sim.depCTexForDiag(), sim.depRTexForDiag(), sim.pigCTexForDiag(),
                    sim.pigRTexForDiag());
    std::printf("  [selftest] bridge: same tile's peak mass after a forced flip -- %.2e\n",
                static_cast<double>(massAfterFlip));
    check(massAfterFlip < 1e-4f,
          "bridge: the clear survives a flip -- both ping-pong halves were zeroed, not just "
          "the one that was current when clearBakedTiles() ran (a physics step ran to force "
          "the flip, hence a floor rather than an exact zero: nothing painted stands close "
          "enough to diffuse in)");
  }

  // ======================================================================
  // 8. The frame sequence: what decides when, and what it refuses to do.
  // ======================================================================
  std::printf("  -- 8. the frame sequence (app/StrokeBake.hpp section 1) --\n");
  {
    // BakeCycleReport::why is a const char*, and every assertion below checks
    // that a refusal names its cause rather than merely being a refusal.
    auto because = [](const char* why, const char* fragment) {
      return why != nullptr && std::string(why).find(fragment) != std::string::npos;
    };

    // Drives a cycle the way the frame loop does: keep stepping until it
    // reaches a terminal action. Two back-to-back step() calls are NOT two
    // frames -- pollPigmentReadback() pumps the instance once and returns, so
    // the map callback still needs the wall-clock time a real frame's own GPU
    // work would have covered.
    //
    // **Bounded by the clock, not by a step count**, and section 5 above
    // already had to learn this the hard way: 240 steps complete in
    // microseconds while the copy takes milliseconds, so a count-bounded loop
    // gives up before the GPU could possibly have finished and then abandons a
    // pending map -- which is the still-mapped-buffer abort all over again.
    // This is the second time that mistake has been made in this file; the
    // comment in section 5 is why it was recognised in one run rather than
    // debugged from scratch.
    struct Driven {
      BakeCycleReport report;
      int steps = 0;
    };
    auto drive = [&](StrokeBakeCycle& cycle, OpenDocument* doc, PaintMode mode,
                     uint64_t firstFrame) {
      using Clock = std::chrono::steady_clock;
      const auto started = Clock::now();
      Driven d;
      while (std::chrono::duration<double>(Clock::now() - started).count() < 5.0) {
        d.report = cycle.step(gpu, sim, doc, mode, firstFrame + static_cast<uint64_t>(d.steps));
        ++d.steps;
        if (d.report.action == BakeCycleReport::Action::Baked ||
            d.report.action == BakeCycleReport::Action::Refused)
          break;
      }
      return d;
    };

    // --- 8a. The absorption table ---------------------------------------
    check(absorptionFor(PaintMode::Watercolor).has_value() &&
              *absorptionFor(PaintMode::Watercolor) == kAbsorptionWatercolor,
          "cycle: watercolour bakes at its own Beer-Lambert coefficient");
    check(absorptionFor(PaintMode::Ink).has_value() &&
              *absorptionFor(PaintMode::Ink) == kAbsorptionInk,
          "cycle: ink bakes at its own, which is not watercolour's");
    check(!absorptionFor(PaintMode::Oil).has_value(),
          "cycle: oil has NO coefficient rather than a plausible-looking one -- it is a height "
          "field, not a Beer-Lambert medium, so there is no honest number to return");

    // --- 8b. The policy, against hand-built occupancy --------------------
    //
    // Hand-built rather than solver-derived on purpose: this is the whole
    // decision the cycle makes, and a table lets it be checked at exactly the
    // boundaries a real stroke never lands on.
    {
      std::vector<PaintSim::TileOccupancy> occ(4);
      occ[0] = {1.0f, 0.0f};              // loaded and dry     -> take
      occ[1] = {1.0f, 0.5f};              // loaded but wet     -> hold
      occ[2] = {kBakeMassFloor, 0.0f};    // exactly at floor   -> skip
      occ[3] = {0.0f, 0.0f};              // empty              -> skip
      const DryTileScan s = selectDryTiles(occ, 2, 2);
      check(s.ready.size() == 1, "cycle: exactly the loaded, dry tile is taken");
      check(s.ready.size() == 1 && s.ready[0].x == 0 && s.ready[0].y == 0,
            "cycle: and it is reported at its own (x,y), not at its flat index");
      check(s.wetHeld == 1,
            "cycle: the loaded WET tile is counted as held, not taken and not forgotten -- "
            "'nothing to bake' and 'nothing has dried yet' are different states");
      check(occ[2].mass == kBakeMassFloor && s.ready.size() == 1,
            "cycle: the floor is exclusive -- a tile whose best texel sits exactly on it would "
            "bake to nothing, and taking it would cost a readback and a history entry to write "
            "zero texels");

      // Index -> (x,y) has to survive a non-square grid, which is where a
      // transposed row/column bug would finally show up.
      std::vector<PaintSim::TileOccupancy> wide(6);
      wide[4] = {1.0f, 0.0f};  // index 4 in a 3-wide grid is (1, 1)
      const DryTileScan w = selectDryTiles(wide, 3, 2);
      check(w.ready.size() == 1 && w.ready[0].x == 1 && w.ready[0].y == 1,
            "cycle: index 4 of a 3x2 grid is (1,1) -- rows and columns are not transposed");

      const DryTileScan bad = selectDryTiles(occ, 3, 3);
      check(bad.ready.empty() && bad.wetHeld == 0,
            "cycle: an occupancy vector that does not match the tile counts yields nothing "
            "rather than indexing past its end");
    }

    // --- 8c. The cycle against the real solver ---------------------------
    //
    // A fresh stroke, because there is nothing left to bake by now: section 2
    // painted entirely inside the centre tile, and section 7 cleared exactly
    // that tile, so the canvas is empty. The first draft of this section
    // assumed section 2's paint was still there and failed with "nothing to
    // bake" -- worth stating, because the assumption looks safe and is not.
    {
      SimParams p{};
      p.brushRadius = 14.0f;
      p.brushWater = 1.4f;
      p.brushPigment = 0.9f;
      stroke(gpu, sim, p, 160.0f, 160.0f, 224.0f, 224.0f, 8);
      // Long enough for wetness to reach zero: selectDryTiles() takes a tile
      // only when it is finished, and section 3 measured 240 frames doing it.
      settle(gpu, sim, p, 240);
    }

    OpenDocument od = makeBlankOpenDocument(static_cast<int32_t>(sim.width()),
                                            static_cast<int32_t>(sim.height()),
                                            WorkingSpace{}, "bridge");
    recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                                 makePigmentLayer("dried wash")));
    od.activeLayer = od.document.layers.size() - 1;

    StrokeBakeCycle cycle;
    const uint64_t revBefore = od.revision;
    const size_t historyBefore = od.history.entries().size();

    const BakeCycleReport first = cycle.step(gpu, sim, &od, PaintMode::Watercolor, 0);
    std::printf("  [selftest] cycle: frame 0 -> action=%d tiles=%zu wetHeld=%zu (%s)\n",
                static_cast<int>(first.action), first.tiles, first.wetHeld, first.why);
    check(first.action == BakeCycleReport::Action::Submitted && first.tiles > 0,
          "cycle: the first frame finds dry paint and SUBMITS a readback -- it does not bake, "
          "because a blocking tile read measured 3.288 ms of PRD F3's 20 ms budget");
    check(cycle.readbackInFlight(),
          "cycle: and it is holding the tile list itself, so the caller cannot get the submit "
          "and the bake out of step");
    check(od.revision == revBefore,
          "cycle: the submit frame changes NOTHING about the document -- no revision bump, so "
          "no upload, so no frame shows paint that has not arrived yet");

    const Driven drivenBake = drive(cycle, &od, PaintMode::Watercolor, 1);
    const BakeCycleReport second = drivenBake.report;
    std::printf("  [selftest] cycle: baked after %d further step(s) -> action=%d baked %zu "
                "tile(s), %zu texel(s), peak %.4f\n",
                drivenBake.steps + 1, static_cast<int>(second.action), second.bake.tilesWritten,
                second.bake.texelsWritten, static_cast<double>(second.bake.peakCoverage));
    check(second.action == BakeCycleReport::Action::Baked,
          "cycle: a following frame finds the readback ready and bakes it");
    check(second.bake.texelsWritten > 0, "cycle: and it wrote real texels into the layer");
    check(!cycle.readbackInFlight(), "cycle: the cycle is idle again, ready for the next scan");

    // The two assertions this whole section exists for.
    check(od.revision > revBefore,
          "cycle: the bake bumped OpenDocument::revision -- ui/DocumentTexture caches on it, so "
          "without this the freshly baked tiles would sit in the document and never reach the "
          "screen");
    check(od.history.entries().size() > historyBefore,
          "cycle: and appended a history entry, so a dried stroke is undoable (recordEdit does "
          "both; see the one-bake-one-entry limitation in StrokeBake.hpp section 3)");

    // Paint is now in EXACTLY ONE of the two stacked pictures.
    {
      std::vector<PaintSim::TileOccupancy> after;
      check(sim.readTileOccupancy(gpu, after), "cycle: occupancy read back after the bake");
      float bakedTileMass = 0.0f;
      for (const auto& t : after) bakedTileMass = std::max(bakedTileMass, t.mass);
      std::printf("  [selftest] cycle: peak solver mass anywhere after the bake -- %.2e\n",
                  static_cast<double>(bakedTileMass));
      check(bakedTileMass < kBakeMassFloor,
            "cycle: the solver no longer holds ANY tile above the bake floor -- the document "
            "and the canvas are drawn stacked, so paint left in both would render twice, at "
            "double density");
    }

    // --- 8d. Refusals must not destroy paint -----------------------------
    //
    // The dangerous asymmetry: a refusal that had already cleared the solver
    // would lose the stroke with nowhere to have put it. Every refusal path
    // has to leave the paint wet-side, and the only way to prove that is to
    // paint again and drive a refusal for real.
    {
      SimParams p{};
      p.brushRadius = 14.0f;
      p.brushWater = 1.4f;
      p.brushPigment = 0.9f;
      stroke(gpu, sim, p, 160.0f, 160.0f, 224.0f, 224.0f, 8);
      settle(gpu, sim, p, 240);

      auto peakMass = [&]() {
        std::vector<PaintSim::TileOccupancy> occ;
        if (!sim.readTileOccupancy(gpu, occ)) return -1.0f;
        float m = 0.0f;
        for (const auto& t : occ) m = std::max(m, t.mass);
        return m;
      };
      const float massBefore = peakMass();
      check(massBefore > kBakeMassFloor, "cycle: a second stroke has dried, ready to refuse on");

      // An RGB layer is the wrong destination, and it is the likeliest one:
      // Document::createBlank() always makes an RGB layer at index 0.
      OpenDocument rgbDoc = makeBlankOpenDocument(static_cast<int32_t>(sim.width()),
                                                  static_cast<int32_t>(sim.height()),
                                                  WorkingSpace{}, "rgb");
      rgbDoc.activeLayer = 0;
      StrokeBakeCycle rgbCycle;
      const BakeCycleReport sub = rgbCycle.step(gpu, sim, &rgbDoc, PaintMode::Watercolor, 0);
      check(sub.action == BakeCycleReport::Action::Submitted,
            "cycle: the scan half does not look at the destination -- it submits first");
      const BakeCycleReport refused = drive(rgbCycle, &rgbDoc, PaintMode::Watercolor, 1).report;
      std::printf("  [selftest] cycle: RGB destination -> action=%d (%s)\n",
                  static_cast<int>(refused.action), refused.why);
      check(refused.action == BakeCycleReport::Action::Refused &&
                because(refused.why, "not a Pigment"),
            "cycle: baking into an RGB layer is refused BY NAME rather than silently skipped");
      check(peakMass() >= massBefore - 1e-6f,
            "cycle: and the solver still holds every bit of the paint -- a refusal that had "
            "cleared first would have destroyed the stroke with nowhere to have put it");

      // A null document is the state before anything is open, not an error.
      StrokeBakeCycle nullCycle;
      nullCycle.step(gpu, sim, nullptr, PaintMode::Watercolor, 0);
      const BakeCycleReport noDoc = drive(nullCycle, nullptr, PaintMode::Watercolor, 1).report;
      check(noDoc.action == BakeCycleReport::Action::Refused &&
                because(noDoc.why, "no open document"),
            "cycle: no open document refuses by name too");
      check(peakMass() >= massBefore - 1e-6f,
            "cycle: and it also left the paint where it was");

      // Oil reaches the same refusal through absorptionFor(), which is the
      // point of that function returning an optional rather than a number.
      StrokeBakeCycle oilCycle;
      oilCycle.step(gpu, sim, &od, PaintMode::Oil, 0);
      const BakeCycleReport oil = drive(oilCycle, &od, PaintMode::Oil, 1).report;
      check(oil.action == BakeCycleReport::Action::Refused &&
                because(oil.why, "does not bake"),
            "cycle: oil refuses rather than baking a height field as if it were a wash");
      check(peakMass() >= massBefore - 1e-6f, "cycle: oil's refusal kept the paint as well");
    }

    // --- 8e. The scan throttle -------------------------------------------
    //
    // Cheap (0.129 ms) is not free: every frame would spend 0.6% of PRD F3's
    // budget re-answering a question whose answer changes on a human timescale.
    //
    // Against a DELIBERATELY EMPTY canvas. A scan that finds paint submits a
    // readback, and then the next call takes the bake half instead of the scan
    // half -- so on a loaded canvas "Idle" could mean the throttle skipped the
    // scan, or that a readback is still in flight, or that a scan ran and found
    // nothing. Three causes behind one action is not a test. Empty, the only
    // two reachable states are "skipped" and "scanned and found nothing", and
    // `why` separates them exactly.
    {
      sim.clearCanvas(gpu);
      StrokeBakeCycle throttled;

      const BakeCycleReport a = throttled.step(gpu, sim, &od, PaintMode::Watercolor, 100);
      check(a.action == BakeCycleReport::Action::Idle && because(a.why, "nothing to bake"),
            "cycle: the first call always scans, whatever the frame index it is handed");

      const BakeCycleReport soon = throttled.step(gpu, sim, &od, PaintMode::Watercolor, 101);
      check(soon.action == BakeCycleReport::Action::Idle && soon.why[0] == '\0',
            "cycle: a scan one frame later does not run at all -- it reports no reason because "
            "it never looked, which is how a skipped scan differs from an empty one");

      const BakeCycleReport edge = throttled.step(
          gpu, sim, &od, PaintMode::Watercolor, 100 + StrokeBakeCycle::kScanIntervalFrames - 1);
      check(edge.why[0] == '\0',
            "cycle: and the frame one short of the interval is still skipped -- the boundary is "
            "where an off-by-one would hide");

      const BakeCycleReport later = throttled.step(
          gpu, sim, &od, PaintMode::Watercolor, 100 + StrokeBakeCycle::kScanIntervalFrames);
      check(because(later.why, "nothing to bake"),
            "cycle: and it scans again on exactly the frame the interval is up");
      check(!throttled.readbackInFlight(),
            "cycle: the section leaves no readback mapped behind it -- an abandoned mapping "
            "makes the next wgpuQueueSubmit a validation error that aborts the process");
    }
  }

  sim.shutdown();
  std::printf("[selftest] stroke bridge %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
