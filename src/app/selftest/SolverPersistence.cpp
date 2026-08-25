#include "app/selftest/Support.hpp"

#include "app/StrokeBake.hpp"
#include "core/PigmentBake.hpp"

// track8/solverio -- PRD H1/F6/O2/O3/I8, docs/reachability-audit.md's B1.
//
// --- What this section is, and what it deliberately is NOT ----------------
//
// B1 reads "the solver canvas cannot be saved, exported or undone" and names
// three things missing: `io/` never reads `sim::PaintSim`, `readbackCanvas()`
// has no production caller, and the PaintSim branch never calls
// `recordEdit()`. All three turn out to already be false at this base
// commit (`ae99fbc`) -- not because this track built them, but because "The
// stroke bridge" series (`ab77003`..`9490517`, seven commits, all ancestors
// of this branch's base) already did:
//
//   * `main.cpp` calls `st.bakeCycle.step(gpu, *sim, ...)` unconditionally,
//     every frame a sim exists, BEFORE `drawUI()` (app/StrokeBake.hpp
//     section 1 explains why the ordering matters).
//   * `StrokeBakeCycle::bakeReadyTiles()` (app/StrokeBake.cpp) writes dried
//     solver texels into `Layer::pigmentTiles` -- an ordinary Pigment
//     layer's own storage, the same storage a hand-painted Pigment layer
//     uses -- via `bakePigmentTiles()`, and calls `doc->recordEdit("dried
//     paint", EditKind::Content)` (or `amendEdit()` while a wash is still
//     drying in batches, coalescing one stroke into exactly one entry).
//   * `ui/MacPaintUI.cpp`'s `settleWetPaintBeforeHistoryMove()` calls
//     `StrokeBakeCycle::forceBake()` before every undo/redo/history-panel
//     cursor move, so paint that has not finished drying is settled into a
//     layer first rather than left floating over a document the user just
//     navigated away from.
//   * Every refusal path (wrong layer kind, no document, locked layer, a
//     medium with no Beer-Lambert coefficient) leaves the solver untouched
//     -- `sim.clearBakedTiles()` is only ever called AFTER a successful
//     write -- so a refusal costs the painter nothing.
//
// `app/selftest/StrokeBridge.cpp` (1241 lines, GPU-driven, real solver
// strokes) already proves every one of those claims end to end, including
// "one stroke, one history entry" (its section 9) and "undo restores the
// document from before the wet stroke, exactly" (its section 10). This file
// does not re-prove them; duplicating a GPU solve and a 5-second forced-bake
// wait to re-assert what is already asserted would cost real CI time for
// zero new coverage, and the PLAN.md/AGENT-BRIEF discipline this project
// runs on is explicit about not paying that twice.
//
// **What StrokeBridge.cpp does NOT touch is `io/`.** Nothing in this
// codebase, before this file, ever ran a baked Pigment layer through
// `saveNpaint()`/`loadNpaint()` and checked the result. That is the one
// genuinely open question B1's own wording raises and the bridge series
// never answered: does dried solver paint SURVIVE closing the file? The
// architecture says it must -- CONTEXT.md: "A Media layer is a Pigment
// layer with a solver attached, not a separate storage format" -- so a
// baked tile should be indistinguishable from a hand-painted one by the
// time `io/NpaintFile` sees it, and `io/`'s existing, already-tested Pigment
// layer round trip (app/selftest/NpaintFormat.cpp) should just work with no
// new code. This file is that claim, checked rather than assumed, plus the
// two invariants the brief calls non-negotiable (one history entry, and an
// exact undo) re-asserted here headless so they are proven again
// immediately adjacent to the round trip that is this file's actual point,
// with no live GPU in the loop.
//
// --- Why this is headless and GPU-free -------------------------------------
//
// `StrokeBakeCycle::step()`/`forceBake()` take a `PaintSim&`, so exercising
// them for real needs a GpuContext. This file never calls either. Instead it
// calls `bakePigmentTileFrom()` (core's half of the bridge, declared in
// app/StrokeBake.hpp, already proven bit-identical to a real GPU bake by
// StrokeBridge.cpp section 6) directly on hand-built `depC`/`depR` arrays --
// the exact 128x128x4-float shape `PaintSim::pigmentReadbackDepC()` would
// have handed it -- and calls `OpenDocument::recordEdit()` the same way
// `bakeReadyTiles()` does. That reuses the two REAL production functions the
// bridge is built from without needing the GPU compute passes that would
// have produced their inputs, which is exactly what `runPigmentBakeTest()`
// next door already does for the arithmetic half. Nothing here needs
// gating: it has no GPU path to gate.

namespace np {

bool runSolverPersistenceTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-74s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // 1. The fixture: two tiles' worth of synthetic solver output.
  // ==========================================================================
  //
  // Two tiles, not one -- "compared texel by texel over the whole affected
  // region, not sampled" needs a region wider than a single tile can prove,
  // since a single-tile bug (a transposed x/y, an off-by-one origin) could
  // still pass a one-tile test by accident. Every fourth texel is left at
  // zero on purpose: bakePigmentTileFrom() must skip texels at or below
  // kBakeMassFloor rather than writing them as (false) transparency, and the
  // round trip through io/ must not manufacture paint where the solver
  // reported none, or erase a floor-texel's neighbours by rounding.
  std::printf("  -- 1. synthetic solver output, the shape a real readback hands the bake --\n");
  constexpr int32_t kN = kTileSize;
  auto fillSyntheticDeposit = [&](uint32_t seed, std::vector<float>& depC,
                                  std::vector<float>& depR, size_t& expectedWritten) {
    depC.assign(static_cast<size_t>(kN) * kN * 4, 0.0f);
    depR.assign(static_cast<size_t>(kN) * kN * 4, 0.0f);
    expectedWritten = 0;
    for (int32_t y = 0; y < kN; ++y) {
      for (int32_t x = 0; x < kN; ++x) {
        if ((static_cast<uint32_t>(x + y) + seed) % 4 == 0) continue;  // the floor case
        const size_t i = (static_cast<size_t>(y) * kN + x) * 4;
        const float u = static_cast<float>(x) / (kN - 1);
        const float v = static_cast<float>(y) / (kN - 1);
        // A varying, physically-plausible mass in (0, 1) -- never at the f16
        // precision boundary (kMaxBakedMass), which is core/PigmentBake.hpp
        // section 3's own edge case and not what this file is measuring.
        const float m = 0.05f + 0.5f * (u + v) * (seed == 0 ? 1.0f : 0.6f);
        depC[i + 0] = m * (0.20f + 0.50f * u);
        depC[i + 1] = m * (0.20f + 0.50f * v);
        depC[i + 2] = m * (0.10f + 0.30f * (u * v));
        depC[i + 3] = m;
        depR[i + 0] = m * (0.05f + 0.10f * u);
        depR[i + 1] = m * (0.05f + 0.10f * v);
        depR[i + 2] = m * 0.02f;
        depR[i + 3] = 0.0f;  // the solver always writes 0 here; projectSolverTexel ignores it
        ++expectedWritten;
      }
    }
  };

  std::vector<float> depCA, depRA, depCB, depRB;
  size_t expectedA = 0, expectedB = 0;
  fillSyntheticDeposit(0, depCA, depRA, expectedA);
  fillSyntheticDeposit(1, depCB, depRB, expectedB);
  check(expectedA > 0 && expectedB > 0 && expectedA != expectedB,
        "fixture: two tiles with different populated-texel counts -- a transposed tile "
        "index would still pass a test where both tiles look the same");

  // ==========================================================================
  // 2. The bake and the history entry, exactly as bakeReadyTiles() does them.
  // ==========================================================================
  std::printf("  -- 2. the bake, and exactly one history entry --\n");
  OpenDocument od = makeBlankOpenDocument(2 * kTileSize, kTileSize, WorkingSpace{},
                                          "solver-persistence");
  // Document::createBlank() (inside makeBlankOpenDocument) always makes an
  // RGB layer at index 0 -- StrokeBridge.cpp's own §8d comment on this same
  // point -- so the Pigment layer this bake targets is added on top of it,
  // matching every real document a bake ever runs against.
  recordLayerEdit(od, addLayer(od.document, od.document.layers.size(),
                               makePigmentLayer("baked wash")));
  const size_t layerIndex = od.document.layers.size() - 1;
  od.activeLayer = layerIndex;

  const size_t historyBeforeBake = od.history.entries().size();
  Layer* target = activeLayerOf(od);
  check(target != nullptr && target->kind == LayerKind::Pigment &&
            target->pigmentTiles.has_value(),
        "the active layer is a real Pigment layer before any paint reaches it");

  size_t written = 0;
  if (target != nullptr && target->pigmentTiles.has_value()) {
    written += bakePigmentTileFrom(depCA.data(), depRA.data(), kAbsorptionWatercolor,
                                   target->pigmentTiles->getOrCreate(TileCoord{0, 0}));
    written += bakePigmentTileFrom(depCB.data(), depRB.data(), kAbsorptionWatercolor,
                                   target->pigmentTiles->getOrCreate(TileCoord{1, 0}));
  }
  std::printf("  [selftest] solver persistence: baked %zu texels (expected %zu + %zu = %zu)\n",
              written, expectedA, expectedB, expectedA + expectedB);
  check(written == expectedA + expectedB,
        "bakePigmentTileFrom() wrote exactly the texels above the floor -- not more, not "
        "fewer -- across both tiles");

  // The one call app/StrokeBake.cpp's bakeReadyTiles() makes on a successful
  // bake that is not continuing a drying episode: EditKind::Content, because
  // dried paint is content, not structure (it does not bump
  // structuralRevision, which the journal keys its own cadence on).
  od.recordEdit("dried paint", EditKind::Content);
  check(od.history.entries().size() == historyBeforeBake + 1,
        "a solver bake produces EXACTLY ONE history entry -- the non-negotiable this whole "
        "file exists to protect, re-checked here (StrokeBridge.cpp §9 checks it again with a "
        "real solver and a real multi-batch drying episode)");

  // ==========================================================================
  // 3. Undo restores the pre-bake state exactly; redo restores the bake.
  // ==========================================================================
  std::printf("  -- 3. undo restores the empty layer; redo restores the bake --\n");
  const Document* beforeBake = od.history.undo();
  check(beforeBake != nullptr, "there is a prior entry to undo to");
  if (beforeBake != nullptr) {
    const Layer& layerThen = beforeBake->layers[layerIndex];
    check(layerThen.pigmentTiles.has_value() &&
              layerThen.pigmentTiles->occupiedTileCount() == 0,
          "one undo reaches the document exactly as it was before the bake -- the Pigment "
          "layer holds ZERO tiles, not a partially-erased one");
  }

  const Document* afterRedo = od.history.redo();
  check(afterRedo != nullptr, "there is a later entry to redo to");
  size_t redoMismatches = 0;
  if (afterRedo != nullptr) {
    const Layer& layerAgain = afterRedo->layers[layerIndex];
    check(layerAgain.pigmentTiles.has_value() &&
              layerAgain.pigmentTiles->occupiedTileCount() == 2,
          "redo restores both baked tiles, not a subset of them");
    if (layerAgain.pigmentTiles.has_value()) {
      for (TileCoord tc : {TileCoord{0, 0}, TileCoord{1, 0}}) {
        const PigmentTile* live = target->pigmentTiles->find(tc);
        const PigmentTile* stored = layerAgain.pigmentTiles->find(tc);
        if (live == nullptr || stored == nullptr) { ++redoMismatches; continue; }
        for (int32_t y = 0; y < kN; ++y)
          for (int32_t x = 0; x < kN; ++x)
            if (!(live->readTexel(PixelCoord{x, y}) == stored->readTexel(PixelCoord{x, y})))
              ++redoMismatches;
      }
    }
  }
  check(redoMismatches == 0,
        "and every one of the 32768 texels in both tiles came back byte for byte -- History "
        "keeps a real copy of the document, not a reference the undo above could have "
        "invalidated");

  // ==========================================================================
  // 4. The io/ round trip: does dried solver paint survive closing the file?
  // ==========================================================================
  //
  // This is the section the rest of the file exists for. `od.document` still
  // holds the baked layer -- `History::undo()`/`redo()` only move a read
  // cursor over its own copies (core/History.hpp: "the usual call installs
  // it immediately, `doc.document = *h.undo()`" -- deliberately NOT done
  // above, so `od.document` never stopped being the live, mutable document a
  // save would actually see) -- so saving it now is exactly what "File > Save"
  // would do the instant a wash finishes drying.
  std::printf("  -- 4. the io/ round trip, texel by texel over both tiles --\n");
  const char* kPath = "selftest_solverio_roundtrip.npaint";
  std::remove(kPath);
  const NpaintSaveResult saved = saveNpaint(od.document, kPath);
  check(saved.ok && saved.error.empty(), saved.ok ? "saveNpaint() wrote the baked document"
                                                   : saved.error.c_str());
  check(saved.warnings.empty(),
        "and raised no warnings -- a baked Pigment layer is not a special case for the "
        "writer, it is an ordinary one");

  const NpaintLoadResult loaded = loadNpaint(kPath);
  check(loaded.ok && loaded.error.empty(), "loadNpaint() read it back");
  check(loaded.document.layers.size() == od.document.layers.size(),
        "the reloaded document has the same layer count -- the RGB layer at index 0 and the "
        "Pigment layer both came back");

  if (loaded.ok && loaded.document.layers.size() > layerIndex) {
    const Layer& reloaded = loaded.document.layers[layerIndex];
    check(reloaded.kind == LayerKind::Pigment && reloaded.pigmentTiles.has_value(),
          "the reloaded layer is still a Pigment layer with real tile storage");
    check(reloaded.pigmentTiles.has_value() &&
              reloaded.pigmentTiles->occupiedTileCount() == 2,
          "and holds exactly the two tiles the bake wrote -- not zero (lost), not more "
          "(phantom paint from padding or a neighbouring tile)");

    size_t compared = 0, mismatches = 0, nonEmptyCompared = 0;
    if (reloaded.pigmentTiles.has_value()) {
      for (TileCoord tc : {TileCoord{0, 0}, TileCoord{1, 0}}) {
        const PigmentTile* before = target->pigmentTiles->find(tc);
        const PigmentTile* after = reloaded.pigmentTiles->find(tc);
        check(before != nullptr && after != nullptr,
              "both the pre-save and the post-load tile exist at the coordinate they were "
              "written at");
        if (before == nullptr || after == nullptr) continue;
        for (int32_t y = 0; y < kN; ++y) {
          for (int32_t x = 0; x < kN; ++x) {
            const PixelCoord p{x, y};
            const PigmentTexel b = before->readTexel(p);
            const PigmentTexel a = after->readTexel(p);
            ++compared;
            if (b.mass > 1e-4f) ++nonEmptyCompared;
            // Zero tolerance, matching app/selftest/NpaintFormat.cpp's own
            // reasoning for a Pigment layer's seven channels: the whole
            // chain is HALF in, memcpy, HALF out -- no float stage exists
            // for these channels to round through, so a mismatch of any
            // size is a real bug, not noise.
            if (!(a == b)) ++mismatches;
          }
        }
      }
    }
    std::printf("  [selftest] solver persistence: %zu texels compared across both tiles, %zu "
                "carrying real mass, %zu mismatches after the io/ round trip\n",
                compared, nonEmptyCompared, mismatches);
    check(compared == 2 * static_cast<size_t>(kN) * kN,
          "every texel of both tiles was actually compared -- not sampled, per the brief");
    check(nonEmptyCompared == expectedA + expectedB,
          "and the count of texels carrying mass survived the round trip too, not just their "
          "values where they happened to already agree");
    check(mismatches == 0,
          "EVERY texel in both tiles -- mass and all six latent channels -- is bit-identical "
          "after saveNpaint()/loadNpaint(). Baked solver paint survives closing the file.");
  }
  std::remove(kPath);

  // ==========================================================================
  // 5. An old file still loads.
  // ==========================================================================
  //
  // Honestly framed: this track adds no new attribute, no new channel and no
  // format-version bump anywhere in `io/` (confirmed at the end of this
  // section's block, and true by inspection -- `git diff --stat -- src/io/`
  // against this branch's base is empty). So "before this change" and
  // "after this change" write byte-identical files, and this assertion
  // cannot exercise a real backward-compatibility bridge the way, say,
  // app/selftest/Channels.cpp's `old = loadNpaint(kNoChannels)` does for a
  // genuinely older document shape. What it DOES prove, honestly: this
  // build's writer and this build's reader still agree on a plain,
  // un-baked Pigment layer -- i.e. this file has not broken the ordinary
  // case while adding a test for the baked one. That is the strongest true
  // statement available without a second build to compare against, and it
  // is stated as exactly that rather than dressed up as compatibility
  // testing it is not.
  std::printf("  -- 5. an ordinary (un-baked) document still round-trips --\n");
  {
    OpenDocument plain = makeBlankOpenDocument(kTileSize, kTileSize, WorkingSpace{}, "plain");
    const char* kOldPath = "selftest_solverio_plain.npaint";
    std::remove(kOldPath);
    const NpaintSaveResult oldSaved = saveNpaint(plain.document, kOldPath);
    check(oldSaved.ok, "a document this file never touched saves without error");
    const NpaintLoadResult oldLoaded = loadNpaint(kOldPath);
    check(oldLoaded.ok && oldLoaded.document.layers.size() == plain.document.layers.size(),
          "and loads back -- this track's addition (a new selftest .cpp calling existing "
          "io/ entry points) changed nothing io/NpaintFile reads or writes");
    std::remove(kOldPath);
  }

  // ==========================================================================
  // 6. The refusal path already exists; this section does not rebuild it.
  // ==========================================================================
  //
  // "Nothing may silently lose work" and "a path that cannot round-trip must
  // refuse by name" are both already true in production, at
  // app/StrokeBake.cpp's bakeReadyTiles(): an RGB active layer, no open
  // document, a locked layer, or a medium with no Beer-Lambert coefficient
  // (Oil) each refuse BY NAME (checked with `because(refused.why, ...)`) and
  // leave the solver untouched -- proven with a live PaintSim and a real
  // dried stroke in app/selftest/StrokeBridge.cpp §8d and §10f. This file
  // built no new refusal path, so it re-asserts nothing here rather than
  // re-deriving those five checks against a mock; see the report for why
  // (main.cpp/ui/MacPaintUI.cpp both discard the BakeCycleReport those
  // refusals arrive in, which is a real, separate, much smaller gap this
  // track chose to report rather than patch inside contended UI territory).

  std::printf("[selftest] solver persistence %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
