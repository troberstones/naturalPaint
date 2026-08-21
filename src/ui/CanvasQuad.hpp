#pragma once
#include "gfx/Context.hpp"

struct ImDrawList;
struct ImVec2;

namespace np {

// The document, drawn with the correct transfer function.
//
// --- Why this module exists ------------------------------------------------
//
// Everything the canvas shows -- ui/DocumentTexture's RGBA16Float composite,
// and sim/PaintSim's RGBA8Unorm canvas, graded and grayscale views -- holds
// **linear light**. Drawing those through `ImDrawList::AddImageQuad()` handed
// them to Dear ImGui's pipeline, which applies `pow(rgb, gamma)` with `gamma`
// chosen from the swapchain format. On the sRGB surface this application used
// to take, that was a *second decode* of a value that was already linear:
// linear 0.25 reached the screen as byte 61 where it should have been 137.
// Zero error at both endpoints, which is why no black-and-white test image
// ever caught it. src/app/selftest/PresentTransfer.cpp measured it end to end.
//
// The chrome wanted the opposite treatment -- its colours are sRGB bytes and
// need decoding -- so one shared pipeline and one shared gamma uniform could
// not serve both. gfx/Context now picks a **non-sRGB** surface, which makes
// ImGui's gamma 1.0 and its chrome bytes exactly right, and the document gets
// this pipeline, which does the encode the chrome no longer needs.
//
// --- What it does ----------------------------------------------------------
//
// One `AddCallback()` per quad, which is Dear ImGui's supported route for
// drawing custom content inline in its render pass (imgui.h's "custom draw
// callbacks"; the WGPU backend publishes its pass encoder through
// `GetPlatformIO().Renderer_RenderState`). The quad is drawn by our own
// pipeline, whose fragment shader samples the linear texture and applies the
// exact piecewise IEC 61966-2-1 encode -- the same curve color/Space's
// `srgbEncode()` implements, asserted against it.
//
// If the adapter offers only sRGB surfaces, the attachment performs that
// encode itself and the shader is compiled without it, so the document is
// correct on either surface. Both variants are exercised: PresentTransfer's
// section builds this module's pipeline at both formats and asserts each
// reaches the same byte.
//
// --- Ordering --------------------------------------------------------------
//
// Corners are baked to clip space when the quad is queued, not in a shader, so
// the callback carries no transform and needs no uniform. Vertices for the
// whole frame go up in one write, which is why `flushCanvasQuads()` must be
// called after the UI is built and before `ImGui_ImplWGPU_RenderDrawData()`,
// and `endCanvasQuadFrame()` after the submit that consumed them.

// Creates the pipeline for `gpu.surfaceFormat`. Safe to call once at startup;
// a second call with the same format is a no-op. Headless builds that never
// draw a quad need not call it at all.
void initCanvasQuad(GpuContext& gpu);
void shutdownCanvasQuad();

// Queue the document texture as an arbitrary quad (rotated/mirrored canvas) or
// as an axis-aligned rect (navigator thumbnail, companion pane). `view` may be
// null, in which case nothing is queued.
void addCanvasQuad(ImDrawList* dl, WGPUTextureView view, const ImVec2& q00, const ImVec2& q10,
                   const ImVec2& q11, const ImVec2& q01);
void addCanvasImage(ImDrawList* dl, WGPUTextureView view, const ImVec2& min, const ImVec2& max);

// Uploads this frame's vertices and builds the per-quad bind groups. Call
// between building the UI and rendering it.
void flushCanvasQuads(GpuContext& gpu);
// Releases the bind groups `flushCanvasQuads()` made. Call after the submit.
void endCanvasQuadFrame();

// How many document quads this session drew, and how many it had to drop for
// want of a slot. Both are reported at shutdown: a cap that silently dropped
// a quad would otherwise look exactly like a document that had nothing to
// show, which is the one failure this module could hide.
size_t canvasQuadsDrawn();
size_t canvasQuadsDropped();

// The fragment shader's encode, on the CPU, for the assertions: the byte this
// module puts on screen for a linear value, given whether the attachment
// encodes in hardware. Exposed so PresentTransfer can compare against
// color/Space's `srgbEncode()` rather than against a copy of this file's
// arithmetic.
// Draws `view` filling `target` with THIS MODULE'S pipeline, built for
// `targetFormat`. The seam --selftest uses to measure the transfer function at
// both target formats, so neither shader variant is dead code: on an sRGB
// target the encode is compiled out and the attachment performs it, on a
// non-sRGB target the shader performs it, and the section asserts the two put
// the same byte on screen. Only ImGui's draw-list plumbing and the quad
// transform are bypassed; the shader and the pipeline are the real ones.
bool renderCanvasQuadForTest(GpuContext& gpu, WGPUTextureView view, WGPUTexture target,
                             WGPUTextureFormat targetFormat, uint32_t w, uint32_t h);

int canvasPresentedByte(float linear, bool attachmentIsSrgb);

}  // namespace np
