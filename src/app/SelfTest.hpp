#pragma once
#include "gfx/Context.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Headless check: paints a Hansa Yellow stroke, lets it settle, then drags
// Phthalo Blue across it and samples the overlap.
//
// This is the one assertion that matters for the whole design. Pigment is
// transported in Mixbox latent space, where linear operations are Kubelka-Munk
// mixes, so wet blue over wet yellow must read GREEN. An RGB solver averaging
// #0D1B44 with #FCD300 gives a muddy grey — if this test sees grey, the latent
// pipeline is broken somewhere between the splat and the composite.
//
// Writes the canvas to `outPng` for eyeballing. Returns true if the overlap is
// green by a clear margin.
bool runSelfTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPng);

// Lays one wet pigmented blob, then runs `seconds` of simulation at 60 Hz,
// reporting pigment mass, wet area, and flow speed as it goes. Written to answer
// a specific question: why does the spreading water lose its colour?
// Renders the same pair of strokes in every medium, so the three models can be
// compared side by side and each is exercised at least once.
void runModeTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPrefix);

void runDiagnostic(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                   float seconds, const char* outPngPrefix);

}  // namespace np
