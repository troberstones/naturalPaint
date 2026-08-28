// Shared scaffolding for the --selftest sections under src/app/selftest/.
//
// This header is **internal to the suite**. It is not part of app/SelfTest.hpp
// -- that header is the public index of what the suite proves, one declaration
// per section, and it stays that way. Everything below is the plumbing those
// sections happen to share: pigment setup, two stroke conventions, and three
// GPU readbacks. Nothing outside src/app/selftest/ includes this.
//
// These helpers lived in anonymous namespaces inside the single 20 815-line
// app/SelfTest.cpp until that file was split one-translation-unit-per-section.
// Splitting the file is what forced them out of anonymous namespaces: internal
// linkage cannot cross a TU boundary. They stay in flat `namespace np` (there
// is no `np::selftest` -- src/app/selftest/ is a directory grouping only, the
// same rule as color/ core/ ops/ and the rest) and their names are unique
// across the binary.
//
// The include block below is app/SelfTest.cpp's own, carried over unchanged so
// every section TU sees exactly the declarations it saw before the split. It
// costs ~0.6 s per TU to parse, against ~8.9 s to compile the old file's body
// in one piece, which is the whole point of the exercise.

#pragma once

#include "app/SelfTest.hpp"

#include <SDL3/SDL_keyboard.h>

// PLAN.md Phase 4 step 9 measures fsync against F_FULLFSYNC, which is the
// measurement that justifies app/Journal choosing the first. Nothing else in
// this file touches a raw file descriptor.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "app/ControlsLayout.hpp"
#include "app/CurveEdit.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/FixedStep.hpp"
#include "app/HistoryPanel.hpp"
#include "app/Journal.hpp"
#include "app/Keymap.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "app/Memory.hpp"
#include "app/SelectionDrag.hpp"
#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "brush/StrokePath.hpp"
#include "color/LutBake.hpp"
#include "color/Shaper.hpp"
#include "color/Space.hpp"
#include "core/Blend.hpp"
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Half.hpp"
#include "core/Histogram.hpp"
#include "core/History.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"
#include "core/Mask.hpp"
#include "core/Merge.hpp"
#include "core/OpStack.hpp"
#include "core/Pigment.hpp"
#include "core/Premultiply.hpp"
#include "core/Probe.hpp"
#include "core/ResourcePaths.hpp"
#include "core/TileShare.hpp"
#include "core/TileStore.hpp"
#include "io/Export.hpp"
#include "io/ExportAs.hpp"
#include "io/ImageDecode.hpp"
#include "io/ImageIO.hpp"
#include "io/NpaintFile.hpp"
#include "io/OpSerial.hpp"
#include "io/TileResidency.hpp"
#include "ops/PointOps.hpp"
#include "ops/Resample.hpp"
#include "ui/DocumentTexture.hpp"
#include "ui/NaturalPaintUI.hpp"

// NOTE on STB_IMAGE_WRITE_IMPLEMENTATION: io/Export.cpp is the one
// translation unit that defines it -- that macro may only be defined once
// across the whole binary. It used to be defined here, back when --selftest
// was the only thing in the codebase that wrote an image; PRD B6's export
// path is production code now, so the implementation moved to the
// production module and this file includes the header for declarations only
// and links against the bodies io/Export.cpp compiled in. Same arrangement
// io/ImageDecode.cpp already has with paint/Palette.cpp for stb_image.h.
//
// One consequence worth knowing: stbi_zlib_compress() is declared only
// inside stb_image_write.h's implementation section, so it is no longer
// visible here. Nothing in this file needs it -- the 16-bit PNG writer that
// did is now io/Export.hpp's encodePng16(), which this file calls.
#include "stb_image_write.h"
#include "core/DirtyTiles.hpp"
#include "app/CompPanel.hpp"
#include "core/LayerCompOps.hpp"
#include "io/CompSerial.hpp"

namespace np {

// ---- pigment and stroke helpers (app/SelfTest.cpp's first anonymous
// namespace) --------------------------------------------------------------

int pigmentIndex(const char* name);
void loadPigment(SimParams& p, const MixboxLut& lut, int index, bool physical);
void stroke(GpuContext& gpu, PaintSim& sim, SimParams& p, float ax, float ay,
            float bx, float by, int steps);
void settle(GpuContext& gpu, PaintSim& sim, SimParams& p, int frames);
double strokeViaDabs(GpuContext& gpu, PaintSim& sim, const SimParams& p,
                     float spacing, float ax, float ay, float bx, float by,
                     int numSamples);

struct RGB { float r, g, b; };

RGB sampleMean(const std::vector<uint8_t>& px, uint32_t w, uint32_t cx,
               uint32_t cy, int half);

void appendToVector(void* context, void* data, int size);

// Masks OpenEXR's per-part `capDate` header attribute ("YYYY:MM:DD HH:MM:SS",
// read off the wall clock at save time) out of a `.npaint` file's raw bytes,
// by pattern rather than by offset -- the offsets move with the header. Used
// by CowTile.cpp and PigmentBasis.cpp before comparing two saves byte for
// byte: a `.npaint` is not byte-reproducible across saves purely because of
// this stamp, and masking it is how both sections still assert
// byte-identity on everything else. See CowTile.cpp's runCowTileTest() for
// the full account of why the mask exists and why it matches by pattern.
std::string maskCapDates(std::string bytes);

// **The pigment source is live and answering with real data**, for whichever
// pigment basis this build was compiled with (core/Document.hpp names both).
//
// Six sections used to open this claim as `check(lut.load(NP_MIXBOX_LUT))`,
// worded "the real Mixbox LUT loads -- this section asserts against measured
// pigment data, not against a stand-in". That is the right claim, and under
// `NP_USE_MIXBOX=OFF` the literal check stopped meaning it: the KM2 fallback
// is closed-form, so `MixboxLut::load()` is a no-op that always succeeds and
// `valid()` is unconditionally true (paint/Palette.cpp). All six kept passing
// while asserting the existence of a LUT that build does not have -- a green
// line standing where a real precondition used to be, which is the shape
// docs/testing-issues.md keeps warning about.
//
// So this asks the question the sections actually need answered, in terms
// neither basis can satisfy vacuously: `lut` must be usable, AND two
// different palette pigments must convert to two different latents that each
// project back to their own colour. Under Mixbox that fails if the LUT did
// not load (an unloaded `MixboxLut` copies the sRGB triple through, so the
// round trip survives but the two latents stop being LUT-derived -- hence the
// `valid()` term, which is the load result). Under KM2 it fails if
// `rgbToLatent()` were ever stubbed, degenerate, or disconnected.
bool pigmentSourceReady(MixboxLut& lut, const char* lutPath);

// ---- GPU scaffolding (app/SelfTest.cpp's second anonymous namespace, plus
// the padded readback that sat just above runDocumentTextureTest()) --------

WGPUShaderModule compileBlitShader(GpuContext& gpu);
bool readbackRGBA16F(GpuContext& gpu, WGPUTexture tex, uint32_t width, uint32_t height,
                     std::vector<float>& out);
bool readbackRGBA16F3D(GpuContext& gpu, WGPUTexture tex, uint32_t size, std::vector<float>& out);
bool blitPipelineRenderAndReadback(GpuContext& gpu, WGPURenderPipeline pipeline,
                                   WGPUTextureView tileView, TileScreenRect rect,
                                   uint32_t targetSize, std::vector<float>& outPixels);
bool readbackRGBA16FPadded(GpuContext& gpu, WGPUTexture tex, uint32_t width, uint32_t height,
                           std::vector<float>& out);

}  // namespace np
