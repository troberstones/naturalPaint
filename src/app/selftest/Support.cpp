// Definitions for app/selftest/Support.hpp. See that header for why these
// helpers are shared rather than section-local, and why they are no longer in
// anonymous namespaces.

#include "app/selftest/Support.hpp"

namespace np {

namespace {

// Embedded WGSL for a tiny textured-quad blit, used only by
// runTiledViewportTest() below to place one uploaded tile at a known screen
// rect and read the result back -- NOT part of ui/NaturalPaintUI's production
// API, which draws exclusively via ImDrawList::AddImage
// (TiledDocumentView::draw()). AddImage's own rendering goes through ImGui's
// WebGPU backend, which needs a live ImGui context/frame to drive; this
// module's actual position math and GPU upload don't need any of that, so
// this shader lets --selftest exercise them directly against a real
// offscreen WebGPU render target instead -- the same rigor every other piece
// of this project's pipeline is held to (PaintSim's own readbackCanvas/
// readbackField).
//
// `vs` maps the unit square [0,1]^2 (via vertex_index, two triangles) onto
// [P.rectMin, P.rectMax] in pixel space, then to NDC against P.targetSize --
// exactly the rect tileScreenRect() computes, handed in via the uniform
// below. `fs` does a plain nearest-texel fetch (textureLoad, no sampler) at
// the tile-local texel the interpolated uv lands on, so a fixture pixel maps
// to a screen block with an exact, hand-computable boundary -- no bilinear
// blending to reason about when picking sample points below.
//
// `fs` derives the texel grid from textureDimensions(tileTex) rather than a
// hardcoded 128, so this same shader works whether `tileTex` is bound to a
// tile's all-levels view (level 0's own dimensions, 128x128, same as
// before -- runTiledViewportTest() below reads that view's level 0
// explicitly) or one of GpuTile::levelViews' single-level views (that
// level's own, smaller dimensions -- runMipPyramidTest()'s mip-selection
// proof below). A texture_2d view scoped to exactly one mip level reports
// that level's size as textureDimensions' result, which is exactly what
// this needs and why NaturalPaintUI's draw() binds a single-level view per
// PLAN.md step 9 rather than relying on automatic LOD selection.
constexpr const char* kBlitShaderSrc = R"(
struct BlitParams {
  rectMin : vec2<f32>,
  rectMax : vec2<f32>,
  targetSize : vec2<f32>,
  _pad : vec2<f32>,
};

@group(0) @binding(0) var<uniform> P : BlitParams;
@group(0) @binding(1) var tileTex : texture_2d<f32>;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var uvs = array<vec2<f32>, 6>(
      vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
      vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
  let uv = uvs[vi];
  let px = mix(P.rectMin, P.rectMax, uv);
  var out : VSOut;
  out.pos = vec4<f32>(px.x / P.targetSize.x * 2.0 - 1.0,
                      1.0 - px.y / P.targetSize.y * 2.0, 0.0, 1.0);
  out.uv = uv;
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let clampedUv = clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.999999));
  let dims = vec2<f32>(textureDimensions(tileTex));
  let texel = vec2<i32>(clampedUv * dims);
  return textureLoad(tileTex, texel, 0);
}
)";

struct BlitParams {
  float rectMinX = 0, rectMinY = 0;
  float rectMaxX = 0, rectMaxY = 0;
  float targetW = 0, targetH = 0;
  float pad0 = 0, pad1 = 0;
};
static_assert(sizeof(BlitParams) == 32, "must match kBlitShaderSrc's BlitParams layout");

}  // namespace

int pigmentIndex(const char* name) {
  const auto& pal = defaultPalette();
  for (size_t i = 0; i < pal.size(); ++i)
    if (std::string(pal[i].name) == name) return static_cast<int>(i);
  return 0;
}

// `physical` is false when the caller is overriding density/staining/granulation
// itself, e.g. the NP_WET calibration path.
void loadPigment(SimParams& p, const MixboxLut& lut, int index, bool physical) {
  const auto& pg = defaultPalette()[index];
  const Latent z = lut.rgbToLatent(pg.rgb[0], pg.rgb[1], pg.rgb[2]);
  for (int i = 0; i < 3; ++i) {
    p.brushLatentC[i] = z.c[i];
    p.brushLatentR[i] = z.res[i];
  }
  if (physical) {
    p.density = pg.density;
    p.staining = pg.staining;
    p.granulation = pg.granulation;
  }
}

// Drag the brush from a to b over `steps` frames, stepping the solver each
// time. Pre-1.3 this deposited paint implicitly, once per frame() call, via
// the kSplat/kInkSplat gated inside frame() itself; that path is gone for
// watercolour and ink (ADR-0003 -- deposition is a standalone per-dab
// dispatch now, see PaintSim::depositDab()), so this helper deposits one dab
// per step explicitly for those two media. Oil is unaffected: it still
// deposits through frame()'s own kOilSplat/kOilTransfer/kOilBrush, driven by
// the same brushA/B/brushActive this helper has always set. This is
// deliberately *not* routed through the arc-length emitter (StrokePath) --
// these callers (runSelfTest, runModeTest, runDiagnostic) are exercising
// pigment mixing and transport, not spacing, so "one dab per step" is the
// direct, simplest equivalent of what used to happen automatically.
void stroke(GpuContext& gpu, PaintSim& sim, SimParams& p, float ax, float ay,
            float bx, float by, int steps) {
  const bool depositsViaDab =
      sim.mode() == PaintMode::Watercolor || sim.mode() == PaintMode::Ink;
  float px = ax, py = ay;
  for (int i = 1; i <= steps; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(steps);
    const float x = ax + (bx - ax) * t;
    const float y = ay + (by - ay) * t;
    p.brushAx = px; p.brushAy = py;
    p.brushBx = x;  p.brushBy = y;
    p.brushActive = 1;
    if (depositsViaDab) sim.depositDab(gpu, p, x, y);
    sim.frame(gpu, p);
    px = x; py = y;
  }
  p.brushActive = 0;
}

void settle(GpuContext& gpu, PaintSim& sim, SimParams& p, int frames) {
  p.brushActive = 0;
  for (int i = 0; i < frames; ++i) sim.frame(gpu, p);
}

// Drags the brush from a to b, but deposits the way 1.3 actually deposits:
// through the arc-length emitter and PaintSim::depositDab(), not stroke()'s
// per-frame swept-segment convention (which splat.wgsl no longer implements
// -- deposition is per-dab now, ADR-0003). `numSamples` stands in for "how
// many times the solver would have sampled the pointer during this stroke",
// i.e. a proxy for stroke speed at a fixed sampling rate: few samples over
// this distance is a fast stroke, many samples is a slow one. Returns the
// total pigment mass (suspended + deposited) after the stroke. No physics
// substeps run in between dabs -- computeStats() is read immediately after
// the last one, so this isolates deposition itself from any transport that
// would otherwise happen at a different total tick count between the two
// speeds (a confound, not the thing ADR-0003 makes a claim about).
double strokeViaDabs(GpuContext& gpu, PaintSim& sim, const SimParams& p,
                     float spacing, float ax, float ay, float bx, float by,
                     int numSamples) {
  StrokePath path;
  path.reset();
  std::vector<Vec2> dabs;
  const float spacingPx = std::max(spacing * p.brushRadius, 0.1f);

  for (int i = 0; i <= numSamples; ++i) {
    const float t = static_cast<float>(i) / static_cast<float>(numSamples);
    const float x = ax + (bx - ax) * t;
    const float y = ay + (by - ay) * t;
    dabs.clear();
    path.addPoint(x, y, spacingPx, dabs);
    for (const auto& d : dabs) sim.depositDab(gpu, p, d.x, d.y);
  }
  dabs.clear();
  path.flush(spacingPx, dabs);  // pen-up: emit the tail segment too
  for (const auto& d : dabs) sim.depositDab(gpu, p, d.x, d.y);

  const auto s = sim.computeStats(gpu);
  return s.suspended + s.deposited;
}


RGB sampleMean(const std::vector<uint8_t>& px, uint32_t w, uint32_t cx,
               uint32_t cy, int half) {
  double r = 0, g = 0, b = 0;
  int n = 0;
  for (int y = -half; y <= half; ++y) {
    for (int x = -half; x <= half; ++x) {
      const size_t i = ((static_cast<size_t>(cy + y)) * w + (cx + x)) * 4;
      r += px[i]; g += px[i + 1]; b += px[i + 2];
      ++n;
    }
  }
  return {static_cast<float>(r / n), static_cast<float>(g / n),
          static_cast<float>(b / n)};
}

// stb_image_write's write_to_func callback: append `size` bytes to the
// std::vector<uint8_t> passed as `context`. Used by runImageDecodeTest() to
// generate fixtures entirely in memory (no scratch files).
void appendToVector(void* context, void* data, int size) {
  auto* v = static_cast<std::vector<uint8_t>*>(context);
  const auto* b = static_cast<const uint8_t*>(data);
  v->insert(v->end(), b, b + size);
}

// Same technique as gfx/ShaderLoader.cpp's compileShader(), just from an
// in-memory string instead of a file on disk -- this shader is test-only
// scaffolding, not one of the solver's reloadable-from-disk passes, so it has
// no reason to live under shaders/ or go through NP_SHADER_DIR.
WGPUShaderModule compileBlitShader(GpuContext& gpu) {
  wgpuDevicePushErrorScope(gpu.device, WGPUErrorFilter_Validation);

  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(kBlitShaderSrc);

  WGPUShaderModuleDescriptor desc = {};
  desc.nextInChain = &wgsl.chain;
  desc.label = sv("tiled-viewport-selftest-blit");
  WGPUShaderModule mod = wgpuDeviceCreateShaderModule(gpu.device, &desc);

  struct ScopeResult { bool done = false; bool failed = false; } res;
  WGPUPopErrorScopeCallbackInfo pci = {};
  pci.mode = WGPUCallbackMode_AllowProcessEvents;
  pci.userdata1 = &res;
  pci.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message,
                    void* ud1, void*) {
    auto* r = static_cast<ScopeResult*>(ud1);
    if (type != WGPUErrorType_NoError) {
      r->failed = true;
      std::fprintf(stderr, "[selftest] tiled-viewport blit shader:\n%.*s\n", svLen(message),
                   message.data ? message.data : "");
    }
    r->done = true;
  };
  wgpuDevicePopErrorScope(gpu.device, pci);
  while (!res.done) wgpuInstanceProcessEvents(gpu.instance);

  if (res.failed || !mod) {
    if (mod) wgpuShaderModuleRelease(mod);
    return nullptr;
  }
  return mod;
}

// Same copy-to-buffer / map / decode technique as PaintSim::readbackField()
// (sim/PaintSim.cpp), generalized over an explicit width/height/texture
// instead of a PaintSim instance's own fields -- reusing readbackField()
// itself isn't practical here: it reads through `this->width_`/`height_`,
// a whole PaintSim's canvas dimensions (1024x1024 in this codebase's
// --selftest setup), not this test's small offscreen target, and
// constructing an unrelated full PaintSim (~193 MB, ADR-0001) just to borrow
// one method would make this test far heavier than the thing it's testing.
bool readbackRGBA16F(GpuContext& gpu, WGPUTexture tex, uint32_t width, uint32_t height,
                     std::vector<float>& out) {
  const uint32_t bytesPerRow = width * 8;  // RGBA16Float = 8 bytes/texel
  if (bytesPerRow % 256 != 0) return false;
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = tex;
  srcTex.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = height;
  WGPUExtent3D extent = {width, height, 1};

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcTex, &dstBuf, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  struct MapState { bool done = false; bool ok = false; } state;
  WGPUBufferMapCallbackInfo mci = {};
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.userdata1 = &state;
  mci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
    auto* s = static_cast<MapState*>(ud1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, total, mci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);

  bool ok = false;
  if (state.ok) {
    const void* raw = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (raw) {
      const size_t n = static_cast<size_t>(width) * height * 4;
      out.resize(n);
      const auto* h = static_cast<const uint16_t*>(raw);
      for (size_t i = 0; i < n; ++i) out[i] = halfToFloat(h[i]);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

// 3-D analog of readbackRGBA16F() above, for color/LutBake's kLutSize^3
// rgba16float LUT textures (PLAN.md Phase 3 step 4). Same copy-to-buffer /
// map / decode technique, generalized to a WGPUExtent3D copy region instead
// of a 2-D one.
//
// Copies the whole volume in a single wgpuCommandEncoderCopyTextureToBuffer
// call rather than size separate 2-D per-layer copies: a 3-D texture copy's
// `copySize.depthOrArrayLayers` is the z-extent and `layout.rowsPerImage` is
// how many rows (here, texels in Y) separate consecutive z-slices in the
// destination buffer, so setting rowsPerImage to the texture's own height
// (not padded) is sufficient for one call to cover every slice contiguously
// -- there is no per-layer wgpu-native API to reach for here, only the one
// already used for 2-D. bytesPerRow's 256-byte alignment requirement is
// satisfied with zero padding for this LUT's own size: size (32) texels x 8
// bytes/texel (rgba16float) = 256 bytes/row exactly, the same convenient
// coincidence readbackRGBA16F()'s own 2-D readback callers already lean on
// at typical field widths.
bool readbackRGBA16F3D(GpuContext& gpu, WGPUTexture tex, uint32_t size, std::vector<float>& out) {
  const uint32_t bytesPerRow = size * 8;  // RGBA16Float = 8 bytes/texel
  if (bytesPerRow % 256 != 0) return false;
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * size * size;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = tex;
  srcTex.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = size;
  WGPUExtent3D extent = {size, size, size};

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcTex, &dstBuf, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  struct MapState { bool done = false; bool ok = false; } state;
  WGPUBufferMapCallbackInfo mci = {};
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.userdata1 = &state;
  mci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
    auto* s = static_cast<MapState*>(ud1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, total, mci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);

  bool ok = false;
  if (state.ok) {
    const void* raw = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (raw) {
      const size_t n = static_cast<size_t>(size) * size * size * 4;
      out.resize(n);
      const auto* h = static_cast<const uint16_t*>(raw);
      for (size_t i = 0; i < n; ++i) out[i] = halfToFloat(h[i]);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

// Shared by runTiledViewportTest() and runMipPyramidTest() (PLAN.md step 9):
// given an already-built blit `pipeline` (caller owns/releases it --
// building it is the one step each call site still does itself, so each
// keeps its own "shader compiles"/"pipeline builds" check() lines), draws
// `tileView` at `rect` into a fresh `targetSize` x `targetSize` RGBA16Float
// offscreen target -- the exact placement TiledDocumentView::draw() would
// use for that tile -- and reads the result back to `outPixels` via
// readbackRGBA16F(). Everything this function itself creates (the uniform
// buffer, bind group, and render target) is released before returning;
// `pipeline` and `tileView` are the caller's.
bool blitPipelineRenderAndReadback(GpuContext& gpu, WGPURenderPipeline pipeline,
                                   WGPUTextureView tileView, TileScreenRect rect,
                                   uint32_t targetSize, std::vector<float>& outPixels) {
  WGPUTextureDescriptor rtd = {};
  rtd.label = sv("blit-selftest-target");
  rtd.dimension = WGPUTextureDimension_2D;
  rtd.size = {targetSize, targetSize, 1};
  rtd.format = WGPUTextureFormat_RGBA16Float;
  rtd.mipLevelCount = 1;
  rtd.sampleCount = 1;
  rtd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  WGPUTexture targetTex = wgpuDeviceCreateTexture(gpu.device, &rtd);
  WGPUTextureView targetView = wgpuTextureCreateView(targetTex, nullptr);

  BlitParams bp;
  bp.rectMinX = rect.min.x;
  bp.rectMinY = rect.min.y;
  bp.rectMaxX = rect.max.x;
  bp.rectMaxY = rect.max.y;
  bp.targetW = static_cast<float>(targetSize);
  bp.targetH = static_cast<float>(targetSize);

  WGPUBufferDescriptor ubd = {};
  ubd.size = sizeof(bp);
  ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  WGPUBuffer ubuf = wgpuDeviceCreateBuffer(gpu.device, &ubd);
  wgpuQueueWriteBuffer(gpu.queue, ubuf, 0, &bp, sizeof(bp));

  WGPUBindGroupEntry entries[2] = {};
  entries[0].binding = 0;
  entries[0].buffer = ubuf;
  entries[0].offset = 0;
  entries[0].size = sizeof(bp);
  entries[1].binding = 1;
  entries[1].textureView = tileView;

  WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = bgl;
  bgd.entryCount = 2;
  bgd.entries = entries;
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPURenderPassColorAttachment att = {};
  att.view = targetView;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 0.0};

  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, pipeline);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 6, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  const bool readOk = readbackRGBA16F(gpu, targetTex, targetSize, targetSize, outPixels);

  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(bgl);
  wgpuBufferDestroy(ubuf);
  wgpuBufferRelease(ubuf);
  wgpuTextureViewRelease(targetView);
  wgpuTextureDestroy(targetTex);
  wgpuTextureRelease(targetTex);

  return readOk;
}


// Reads an RGBA16Float texture back with a **padded** row stride.
//
// The anonymous-namespace `readbackRGBA16F()` further up this file refuses a
// width whose tight stride is not a multiple of 256 -- which is correct for
// what it does, but it is exactly the case this section has to exercise. A
// document is whatever size a file says, and `wgpuQueueWriteTexture` accepts
// any stride while `wgpuCommandEncoderCopyTextureToBuffer` does not; the
// asymmetry is the whole point, so the readback pads and drops the padding on
// the way out (app/Screenshot.cpp does the same thing for the surface, and its
// header explains why the alignment rule exists at all).
bool readbackRGBA16FPadded(GpuContext& gpu, WGPUTexture tex, uint32_t width, uint32_t height,
                           std::vector<float>& out) {
  const uint32_t tightBytesPerRow = width * 8;  // RGBA16Float = 8 bytes/texel
  const uint32_t paddedBytesPerRow = ((tightBytesPerRow + 255u) / 256u) * 256u;
  const uint64_t total = static_cast<uint64_t>(paddedBytesPerRow) * height;

  WGPUBufferDescriptor bd = {};
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = tex;
  srcTex.aspect = WGPUTextureAspect_All;
  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = paddedBytesPerRow;
  dstBuf.layout.rowsPerImage = height;
  const WGPUExtent3D extent = {width, height, 1};

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyTextureToBuffer(enc, &srcTex, &dstBuf, &extent);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  struct MapState { bool done = false; bool ok = false; } state;
  WGPUBufferMapCallbackInfo mci = {};
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.userdata1 = &state;
  mci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
    auto* s = static_cast<MapState*>(ud1);
    s->ok = (status == WGPUMapAsyncStatus_Success);
    s->done = true;
  };
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, static_cast<size_t>(total), mci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);

  bool ok = false;
  if (state.ok) {
    const auto* base = static_cast<const uint8_t*>(
        wgpuBufferGetConstMappedRange(staging, 0, static_cast<size_t>(total)));
    if (base != nullptr) {
      out.resize(static_cast<size_t>(width) * height * 4u);
      for (uint32_t y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) *
                                                                       paddedBytesPerRow);
        for (uint32_t i = 0; i < width * 4u; ++i)
          out[static_cast<size_t>(y) * width * 4u + i] = halfToFloat(row[i]);
      }
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferRelease(staging);
  return ok;
}


}  // namespace np
