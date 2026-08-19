#pragma once

// Phase-2 seam reservation (PLAN.md "Phase 2 — See a file", step 7; PRD E1;
// DESIGN-imaging.md "Selections"). PRD E1 requires that every deposit and
// every op respect the active selection, and DESIGN-imaging.md is explicit
// that this must be a parameter threaded through interfaces that already
// exist, reserved now, rather than a retrofit once selection tools ship --
// "the pervasive retrofit ADR-0001 warns about."
//
// This type is a placeholder ONLY. It intentionally holds no data and
// implements no coverage sampling, gating, or storage. The real selection
// mask is a sparse, tile-backed coverage mask at r8unorm (16 KiB per touched
// 128x128 tile), built on core/TileStore -- neither of which exist yet. That
// implementation lands with the "Select and paste" phase, once TileStore
// exists to build it on.
//
// Until then, this class exists solely so call sites that must eventually
// honour a selection already have a parameter slot for it: filling in the
// real implementation later changes only this file (and whatever new code
// populates and samples a mask), not every deposit/op signature.
//
// Do not add members, sampling methods, or gating logic here. Do not have
// any call site branch on a SelectionMask pointer yet -- nothing produces
// one, so any such branch would be dead code exercising nothing.
namespace np {

class SelectionMask {};

}  // namespace np
