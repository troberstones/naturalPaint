#pragma once

namespace np {

class GpuContext;

// --profile-flats-solver [width height iterations] : headless CPU-vs-GPU
// benchmark for the flats rubber-sheet solver (flats/Membrane.cpp's
// flatMembraneSag() vs flats/MembraneGpu.cpp's membraneSagGpu()), run through
// the exact defaults flats/Sag.cpp's production call site uses (cycles=30,
// tol=1e-2). Needs a real GPU adapter, so unlike --profile-vector-warp this
// dispatches after gpu.init() rather than before SDL_Init -- see main.cpp's
// call site. Same physical-iPad rationale as ProfileVectorWarp.hpp: `xcrun
// devicectl` can install, launch and stream stdout for this binary on a real
// iPad, and this harness needs nothing from the touchscreen to be useful
// there, since flatMembraneSag()/membraneSagGpu() are both pure functions of
// a mask.
//
// `width`/`height` default to 2048x2048 -- flats/FlatsLayer.hpp's own header
// comment records "a real-world plate at 2K costs seconds" as the CPU number
// this is meant to be judged against. `iterations` defaults to 5, since a 2K
// CPU solve is not cheap to repeat.
int runProfileFlatsSolver(GpuContext& gpu, int width, int height, int iterations);

}  // namespace np
