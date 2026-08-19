#include "sim/PaintSim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "core/Half.hpp"
#include "gfx/ShaderLoader.hpp"

namespace np {
namespace {

// Water, saturation and the solver scratch are fine at half precision. The
// pigment fields are not: transfer_pigment exchanges between suspended and
// deposited 240x/second, and rounding each side to f16 independently bled ~45%
// of the pigment mass over 20 seconds. At f32 the same exchange conserves
// exactly. Costs 2x memory on four of the seven fields.
constexpr WGPUTextureFormat kWaterFormat = WGPUTextureFormat_RGBA16Float;
constexpr WGPUTextureFormat kPigmentFormat = WGPUTextureFormat_RGBA32Float;
constexpr uint32_t kWorkgroup = 8;

uint32_t texelBytes(WGPUTextureFormat f) {
  return f == WGPUTextureFormat_RGBA32Float ? 16u : 8u;
}

uint32_t groups(uint32_t n) { return (n + kWorkgroup - 1) / kWorkgroup; }

WGPUTexture makeField(WGPUDevice device, uint32_t w, uint32_t h, const char* label,
                      WGPUTextureFormat format) {
  WGPUTextureDescriptor d = {};
  d.label = sv(label);
  d.dimension = WGPUTextureDimension_2D;
  d.size = {w, h, 1};
  d.format = format;
  d.mipLevelCount = 1;
  d.sampleCount = 1;
  // RenderAttachment is only there so the field can be zeroed with a cheap
  // clear-only render pass instead of an 8 MB upload per texture.
  d.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding |
            WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
  return wgpuDeviceCreateTexture(device, &d);
}

void makePingPong(WGPUDevice device, PingPong& f, uint32_t w, uint32_t h,
                  const char* label, WGPUTextureFormat format) {
  f.format = format;
  for (int i = 0; i < 2; ++i) {
    f.tex[i] = makeField(device, w, h, label, format);
    f.view[i] = wgpuTextureCreateView(f.tex[i], nullptr);
  }
  f.cur = 0;
}

void clearView(WGPUCommandEncoder enc, WGPUTextureView view) {
  WGPURenderPassColorAttachment att = {};
  att.view = view;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 0.0};

  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;

  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
}

WGPUBindGroupEntry bufEntry(uint32_t binding, WGPUBuffer b, uint64_t size) {
  WGPUBindGroupEntry e = {};
  e.binding = binding;
  e.buffer = b;
  e.offset = 0;
  e.size = size;
  return e;
}

WGPUBindGroupEntry texEntry(uint32_t binding, WGPUTextureView v) {
  WGPUBindGroupEntry e = {};
  e.binding = binding;
  e.textureView = v;
  return e;
}

WGPUBindGroupEntry samplerEntry(uint32_t binding, WGPUSampler s) {
  WGPUBindGroupEntry e = {};
  e.binding = binding;
  e.sampler = s;
  return e;
}

}  // namespace

static const char* kPassLabels[] = {
    "paper", "splat", "update_velocities", "divergence", "jacobi", "project",
    "flow_outward", "advect_water", "advect_pigment", "transfer_pigment",
    "capillary_flow",
    "oil_splat", "oil_velocity", "oil_advect", "oil_transfer", "oil_brush",
    "ink_splat", "ink_stream", "ink_collide", "ink_pigment", "composite"};

const char* passLabel(int id) {
  const int n = static_cast<int>(sizeof(kPassLabels) / sizeof(kPassLabels[0]));
  return (id >= 0 && id < n) ? kPassLabels[id] : "?";
}

const char* paintModeName(PaintMode m) {
  switch (m) {
    case PaintMode::Watercolor: return "Watercolour";
    case PaintMode::Oil:        return "Oil";
    case PaintMode::Ink:        return "Ink";
    default:                    return "?";
  }
}

void PingPong::release() {
  for (int i = 0; i < 2; ++i) {
    if (view[i]) { wgpuTextureViewRelease(view[i]); view[i] = nullptr; }
    if (tex[i]) { wgpuTextureDestroy(tex[i]); wgpuTextureRelease(tex[i]); tex[i] = nullptr; }
  }
  cur = 0;
}

// ---------------------------------------------------------------- setup

bool PaintSim::init(GpuContext& gpu, uint32_t width, uint32_t height,
                         const MixboxLut& lut) {
  WGPUSamplerDescriptor sd = {};
  sd.addressModeU = WGPUAddressMode_ClampToEdge;
  sd.addressModeV = WGPUAddressMode_ClampToEdge;
  sd.addressModeW = WGPUAddressMode_ClampToEdge;
  sd.magFilter = WGPUFilterMode_Linear;
  sd.minFilter = WGPUFilterMode_Linear;
  sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sd.lodMaxClamp = 1.0f;
  sd.maxAnisotropy = 1;
  linear_ = wgpuDeviceCreateSampler(gpu.device, &sd);

  WGPUBufferDescriptor bd = {};
  bd.label = sv("sim params");
  bd.size = sizeof(SimParams);
  bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  uniform_ = wgpuDeviceCreateBuffer(gpu.device, &bd);

  // The Mixbox LUT stays on the CPU. It is only needed for RGB -> latent, which
  // happens once when a colour is picked; the GPU only ever goes the other way,
  // and that direction is a pure polynomial.
  if (!lut.valid()) {
    std::fprintf(stderr, "[sim] Mixbox LUT missing; pigment mixing would be wrong\n");
    return false;
  }

  if (!buildPipelines(gpu)) return false;
  allocFields(gpu, width, height);
  return true;
}

void PaintSim::releaseFields() {
  for (PingPong* f : {&water_, &pigC_, &pigR_, &depC_, &depR_, &sat_, &aux_})
    f->release();
  for (PingPong* f : {&lbmA_, &lbmB_, &lbmC_, &brushVol_, &brushC_, &brushR_})
    f->release();
  inkAllocated_ = false;
  oilAllocated_ = false;
  if (paperView_) { wgpuTextureViewRelease(paperView_); paperView_ = nullptr; }
  if (paper_) { wgpuTextureDestroy(paper_); wgpuTextureRelease(paper_); paper_ = nullptr; }
  if (canvasView_) { wgpuTextureViewRelease(canvasView_); canvasView_ = nullptr; }
  if (canvas_) { wgpuTextureDestroy(canvas_); wgpuTextureRelease(canvas_); canvas_ = nullptr; }
  if (grayscaleView_) { wgpuTextureViewRelease(grayscaleView_); grayscaleView_ = nullptr; }
  if (grayscale_) { wgpuTextureDestroy(grayscale_); wgpuTextureRelease(grayscale_); grayscale_ = nullptr; }
  if (gradedView_) { wgpuTextureViewRelease(gradedView_); gradedView_ = nullptr; }
  if (graded_) { wgpuTextureDestroy(graded_); wgpuTextureRelease(graded_); graded_ = nullptr; }
  // The baked LUT is not sized off width_/height_ (it's always kLutSize^3),
  // but this is the general "tear down this sim's owned GPU state" path
  // (called by both resize and shutdown), so it is released here too,
  // alongside graded_/gradedView_, rather than growing a second release
  // path just for it.
  if (hasBakedLut_) { releaseLut3D(lut_); hasBakedLut_ = false; }
  for (auto& kv : bindCache_) wgpuBindGroupRelease(kv.second);
  bindCache_.clear();
}

void PaintSim::allocFields(GpuContext& gpu, uint32_t w, uint32_t h) {
  releaseFields();
  width_ = w;
  height_ = h;

  makePingPong(gpu.device, water_, w, h, "water(u,v,p,M)", kWaterFormat);
  makePingPong(gpu.device, pigC_, w, h, "suspended latent*mass", kPigmentFormat);
  makePingPong(gpu.device, pigR_, w, h, "suspended residual*mass", kPigmentFormat);
  makePingPong(gpu.device, depC_, w, h, "deposited latent*mass", kPigmentFormat);
  makePingPong(gpu.device, depR_, w, h, "deposited residual*mass", kPigmentFormat);
  makePingPong(gpu.device, sat_, w, h, "capillary saturation", kWaterFormat);
  makePingPong(gpu.device, aux_, w, h, "divergence + pressure correction", kWaterFormat);

  paper_ = makeField(gpu.device, w, h, "paper(h,c)", kWaterFormat);
  paperView_ = wgpuTextureCreateView(paper_, nullptr);

  WGPUTextureDescriptor cd = {};
  cd.label = sv("canvas");
  cd.dimension = WGPUTextureDimension_2D;
  cd.size = {w, h, 1};
  cd.format = WGPUTextureFormat_RGBA8Unorm;
  cd.mipLevelCount = 1;
  cd.sampleCount = 1;
  cd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
             WGPUTextureUsage_CopySrc;  // CopySrc so --selftest can read it back
  canvas_ = wgpuDeviceCreateTexture(gpu.device, &cd);
  canvasView_ = wgpuTextureCreateView(canvas_, nullptr);

  // Grayscale preview target (PRD Q3): same descriptor as canvas_ (down to
  // CopySrc, for the same "a test could read this back" reason) but its own
  // texture -- updateGrayscalePreview() writes here, never into canvas_.
  WGPUTextureDescriptor gd = cd;
  gd.label = sv("canvas-grayscale-preview");
  grayscale_ = wgpuDeviceCreateTexture(gpu.device, &gd);
  grayscaleView_ = wgpuTextureCreateView(grayscale_, nullptr);

  // Grade preview target (PLAN.md Phase 3 step 6) -- same descriptor
  // pattern as grayscale_ immediately above (down to CopySrc, for the
  // same "a test could read this back" reason via readbackGraded()), its
  // own texture, unconditionally created here so it exists whether or not
  // the grade toggle is ever used this session -- updateGradePreview()
  // writes here, never into canvas_.
  WGPUTextureDescriptor grd = cd;
  grd.label = sv("canvas-grade-preview");
  graded_ = wgpuDeviceCreateTexture(gpu.device, &grd);
  gradedView_ = wgpuTextureCreateView(graded_, nullptr);

  clearCanvas(gpu);
  generatePaper(gpu);
}

void PaintSim::allocInkFields(GpuContext& gpu) {
  if (inkAllocated_) return;
  // f32 throughout: the LBE conserves density only if the streaming and
  // collision arithmetic does, and f16 bled mass here the same way it did in
  // transfer_pigment.
  makePingPong(gpu.device, lbmA_, width_, height_, "lbm f0..f3", kPigmentFormat);
  makePingPong(gpu.device, lbmB_, width_, height_, "lbm f4..f7", kPigmentFormat);
  makePingPong(gpu.device, lbmC_, width_, height_, "lbm f8,s,h", kPigmentFormat);
  inkAllocated_ = true;
}

void PaintSim::allocOilFields(GpuContext& gpu) {
  if (oilAllocated_) return;
  // Tiny: 64x64 each. The brush carries its own paint so it can run dry and
  // pick colour up, per IMPaSTo.
  makePingPong(gpu.device, brushVol_, kBrushGrid, kBrushGrid, "brush volume", kPigmentFormat);
  makePingPong(gpu.device, brushC_, kBrushGrid, kBrushGrid, "brush latent", kPigmentFormat);
  makePingPong(gpu.device, brushR_, kBrushGrid, kBrushGrid, "brush residual", kPigmentFormat);
  oilAllocated_ = true;
}

void PaintSim::setMode(GpuContext& gpu, PaintMode m) {
  if (m == mode_) return;

  // 1.4 / ADR-0001 bullet 3: free the *outgoing* medium's optional field set,
  // not just allocate the incoming one -- otherwise every medium a session
  // ever visits stays resident for the rest of it, which contradicts
  // ADR-0001's whole per-mode residency table (watercolour ~193 MB / ink
  // ~210 MB / oil ~160 MB: numbers that only hold if the ink lattice and the
  // oil brush grid are mutually exclusive, not cumulative).
  //
  // ⚠️ This is exactly the case ADR-0001's bindCache_ warning is about.
  // bindGroup()'s cache key (below) is (passId, ping-pong parity) only, not
  // texture identity -- PingPong::release() resets `cur` to 0, so a field
  // freed here and later reallocated (revisiting a medium) comes back with
  // the same parity it had before, and can collide with a stale cache entry
  // that still points at the now-destroyed textures. releaseFields() already
  // clears the whole cache for the same reason on init/resize/shutdown; this
  // path needs the identical discipline, because it is the one place fields
  // actually get freed-then-reallocated *within* a running session.
  bool freedAnything = false;
  if (mode_ == PaintMode::Ink && inkAllocated_) {
    for (PingPong* f : {&lbmA_, &lbmB_, &lbmC_}) f->release();
    inkAllocated_ = false;
    freedAnything = true;
  }
  if (mode_ == PaintMode::Oil && oilAllocated_) {
    for (PingPong* f : {&brushVol_, &brushC_, &brushR_}) f->release();
    oilAllocated_ = false;
    freedAnything = true;
  }
  if (freedAnything) {
    for (auto& kv : bindCache_) wgpuBindGroupRelease(kv.second);
    bindCache_.clear();
  }

  mode_ = m;
  if (m == PaintMode::Ink) allocInkFields(gpu);
  if (m == PaintMode::Oil) allocOilFields(gpu);
  // Different models read the shared fields differently, so carrying a canvas
  // across a switch would reinterpret a wash as a paint slab. Start clean.
  clearCanvas(gpu);
}

void PaintSim::resize(GpuContext& gpu, uint32_t w, uint32_t h) {
  if (w == width_ && h == height_) return;
  allocFields(gpu, w, h);
}

void PaintSim::clearCanvas(GpuContext& gpu) {
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  for (PingPong* f : {&water_, &pigC_, &pigR_, &depC_, &depR_, &sat_, &aux_}) {
    clearView(enc, f->view[0]);
    clearView(enc, f->view[1]);
    f->cur = 0;
  }
  if (inkAllocated_) {
    for (PingPong* f : {&lbmA_, &lbmB_, &lbmC_}) {
      clearView(enc, f->view[0]); clearView(enc, f->view[1]); f->cur = 0;
    }
  }
  if (oilAllocated_) {
    for (PingPong* f : {&brushVol_, &brushC_, &brushR_}) {
      clearView(enc, f->view[0]); clearView(enc, f->view[1]); f->cur = 0;
    }
  }
  clearView(enc, canvasView_);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
}

void PaintSim::generatePaper(GpuContext& gpu) {
  SimParams p{};
  p.resolutionX = width_;
  p.resolutionY = height_;
  wgpuQueueWriteBuffer(gpu.queue, uniform_, 0, &p, sizeof(p));

  std::vector<WGPUBindGroupEntry> entries = {
      bufEntry(0, uniform_, sizeof(SimParams)),
      texEntry(1, paperView_),
  };
  WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipelines_[kPaper], 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = layout;
  bgd.entryCount = entries.size();
  bgd.entries = entries.data();
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipelines_[kPaper]);
  wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass, groups(width_), groups(height_), 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(layout);
}

// ---------------------------------------------------------------- pipelines

bool PaintSim::buildPipelines(GpuContext& gpu) {
  static const char* kPaths[kPassCount] = {
      "paper.wgsl",        "splat.wgsl",           "update_velocities.wgsl",
      "divergence.wgsl",   "jacobi.wgsl",          "project.wgsl",
      "flow_outward.wgsl", "advect_water.wgsl",    "advect_pigment.wgsl",
      "transfer_pigment.wgsl", "capillary_flow.wgsl",
      "oil_splat.wgsl",    "oil_velocity.wgsl",    "oil_advect.wgsl",
      "oil_transfer.wgsl", "oil_brush.wgsl",
      "ink_splat.wgsl",    "ink_stream.wgsl",      "ink_collide.wgsl",
      "ink_pigment.wgsl",
  };

  // Build into locals first so a failed reload leaves the old pipelines intact.
  WGPUComputePipeline built[kPassCount] = {};
  bool ok = true;
  for (int i = 0; i < kPassCount && ok; ++i) {
    WGPUShaderModule mod = compileShader(gpu.device, gpu.instance, kPaths[i]);
    if (!mod) { ok = false; break; }

    WGPUComputePipelineDescriptor d = {};
    d.label = sv(kPaths[i]);
    d.compute.module = mod;
    d.compute.entryPoint = sv("main");
    built[i] = wgpuDeviceCreateComputePipeline(gpu.device, &d);
    wgpuShaderModuleRelease(mod);
    if (!built[i]) ok = false;
  }

  WGPURenderPipeline compPipe = nullptr;
  if (ok) {
    WGPUShaderModule compMod = compileShader(gpu.device, gpu.instance, "composite.wgsl");
    if (!compMod) {
      ok = false;
    } else {
      WGPUColorTargetState target = {};
      target.format = WGPUTextureFormat_RGBA8Unorm;
      target.writeMask = WGPUColorWriteMask_All;

      WGPUFragmentState fs = {};
      fs.module = compMod;
      fs.entryPoint = sv("fs");
      fs.targetCount = 1;
      fs.targets = &target;

      WGPURenderPipelineDescriptor rd = {};
      rd.label = sv("composite");
      rd.vertex.module = compMod;
      rd.vertex.entryPoint = sv("vs");
      rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      rd.primitive.frontFace = WGPUFrontFace_CCW;
      rd.primitive.cullMode = WGPUCullMode_None;
      rd.multisample.count = 1;
      rd.multisample.mask = 0xFFFFFFFF;
      rd.fragment = &fs;
      compPipe = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
      wgpuShaderModuleRelease(compMod);
      if (!compPipe) ok = false;
    }
  }

  // Grayscale preview blit (PRD Q3) -- built and hot-reloaded alongside
  // every other pass here, per shaders/'s "everything reloads on Cmd+R"
  // convention. A single-texture-input blit, so it needs no uniform buffer
  // at all (its WGSL reads the source texture's own size via
  // textureDimensions()), unlike composite.wgsl above.
  WGPURenderPipeline grayPipe = nullptr;
  if (ok) {
    WGPUShaderModule grayMod = compileShader(gpu.device, gpu.instance, "grayscale_blit.wgsl");
    if (!grayMod) {
      ok = false;
    } else {
      WGPUColorTargetState target = {};
      target.format = WGPUTextureFormat_RGBA8Unorm;
      target.writeMask = WGPUColorWriteMask_All;

      WGPUFragmentState fs = {};
      fs.module = grayMod;
      fs.entryPoint = sv("fs");
      fs.targetCount = 1;
      fs.targets = &target;

      WGPURenderPipelineDescriptor rd = {};
      rd.label = sv("grayscale_blit");
      rd.vertex.module = grayMod;
      rd.vertex.entryPoint = sv("vs");
      rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      rd.primitive.frontFace = WGPUFrontFace_CCW;
      rd.primitive.cullMode = WGPUCullMode_None;
      rd.multisample.count = 1;
      rd.multisample.mask = 0xFFFFFFFF;
      rd.fragment = &fs;
      grayPipe = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
      wgpuShaderModuleRelease(grayMod);
      if (!grayPipe) ok = false;
    }
  }

  // Apply-pass grading blit (PLAN.md Phase 3 step 6) -- built and hot-
  // reloaded alongside every other pass here, same convention as the
  // grayscale preview blit immediately above. Its bind group layout has
  // three entries (srcTex, lutTex, lutSamp) instead of grayscale's one,
  // but is otherwise the identical render-pipeline shape.
  WGPURenderPipeline gradePipe = nullptr;
  if (ok) {
    WGPUShaderModule gradeMod = compileShader(gpu.device, gpu.instance, "grade_blit.wgsl");
    if (!gradeMod) {
      ok = false;
    } else {
      WGPUColorTargetState target = {};
      target.format = WGPUTextureFormat_RGBA8Unorm;
      target.writeMask = WGPUColorWriteMask_All;

      WGPUFragmentState fs = {};
      fs.module = gradeMod;
      fs.entryPoint = sv("fs");
      fs.targetCount = 1;
      fs.targets = &target;

      WGPURenderPipelineDescriptor rd = {};
      rd.label = sv("grade_blit");
      rd.vertex.module = gradeMod;
      rd.vertex.entryPoint = sv("vs");
      rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      rd.primitive.frontFace = WGPUFrontFace_CCW;
      rd.primitive.cullMode = WGPUCullMode_None;
      rd.multisample.count = 1;
      rd.multisample.mask = 0xFFFFFFFF;
      rd.fragment = &fs;
      gradePipe = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
      wgpuShaderModuleRelease(gradeMod);
      if (!gradePipe) ok = false;
    }
  }

  if (!ok) {
    for (int i = 0; i < kPassCount; ++i)
      if (built[i]) wgpuComputePipelineRelease(built[i]);
    if (compPipe) wgpuRenderPipelineRelease(compPipe);
    if (grayPipe) wgpuRenderPipelineRelease(grayPipe);
    if (gradePipe) wgpuRenderPipelineRelease(gradePipe);
    std::fprintf(stderr, "[sim] pipeline build failed; keeping previous shaders\n");
    return false;
  }

  for (int i = 0; i < kPassCount; ++i) {
    if (pipelines_[i]) wgpuComputePipelineRelease(pipelines_[i]);
    pipelines_[i] = built[i];
  }
  if (composite_) wgpuRenderPipelineRelease(composite_);
  composite_ = compPipe;
  if (grayscalePipeline_) wgpuRenderPipelineRelease(grayscalePipeline_);
  grayscalePipeline_ = grayPipe;
  if (gradePipeline_) wgpuRenderPipelineRelease(gradePipeline_);
  gradePipeline_ = gradePipe;

  for (auto& kv : bindCache_) wgpuBindGroupRelease(kv.second);
  bindCache_.clear();
  return true;
}

bool PaintSim::reloadShaders(GpuContext& gpu) { return buildPipelines(gpu); }

// ---------------------------------------------------------------- dispatch

WGPUBindGroup PaintSim::bindGroup(GpuContext& gpu, int passId,
                                       WGPUBindGroupLayout layout,
                                       const std::vector<WGPUBindGroupEntry>& entries) {
  // Parities fully determine which views the entries point at, so this key is
  // sufficient and the cache stays tiny.
  // EVERY ping-pong field must appear here. The key is what tells a cached bind
  // group apart from a stale one; a field missing from it will silently hand
  // back a bind group pointing at the wrong half of a pair.
  const uint64_t parity =
      (uint64_t)water_.cur | ((uint64_t)pigC_.cur << 1) | ((uint64_t)pigR_.cur << 2) |
      ((uint64_t)depC_.cur << 3) | ((uint64_t)depR_.cur << 4) |
      ((uint64_t)sat_.cur << 5) | ((uint64_t)aux_.cur << 6) |
      ((uint64_t)lbmA_.cur << 7) | ((uint64_t)lbmB_.cur << 8) |
      ((uint64_t)lbmC_.cur << 9) | ((uint64_t)brushVol_.cur << 10) |
      ((uint64_t)brushC_.cur << 11) | ((uint64_t)brushR_.cur << 12);
  const uint64_t key = ((uint64_t)passId << 16) | parity;

  auto it = bindCache_.find(key);
  if (it != bindCache_.end()) return it->second;

  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = layout;
  // Labelled so a validation failure names the pass instead of reporting ''.
  bgd.label = sv(passLabel(passId));
  bgd.entryCount = entries.size();
  bgd.entries = entries.data();
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);
  bindCache_.emplace(key, bg);
  return bg;
}

void PaintSim::frame(GpuContext& gpu, const SimParams& paramsIn,
                     const SelectionMask* /*selectionMask*/) {
  // `selectionMask` is the phase-2 seam reservation (see PaintSim.hpp) --
  // deliberately unread. Nothing populates a SelectionMask before the
  // "Select and paste" phase, and gating the physics substeps or Oil's
  // in-frame deposit on it belongs there, not here.
  SimParams params = paramsIn;
  params.resolutionX = width_;
  params.resolutionY = height_;
  const int activeSubsteps =
      (mode_ == PaintMode::Ink) ? std::max(inkSubsteps, 1) : std::max(substeps, 1);
  params.dt = paramsIn.dt / static_cast<float>(activeSubsteps);
  params.mode = static_cast<uint32_t>(mode_);
  wgpuQueueWriteBuffer(gpu.queue, uniform_, 0, &params, sizeof(params));

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, nullptr);

  const uint32_t gx = groups(width_);
  const uint32_t gy = groups(height_);
  const WGPUBindGroupEntry ub = bufEntry(0, uniform_, sizeof(SimParams));

  auto run = [&](Pass id, std::vector<WGPUBindGroupEntry> entries) {
    WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipelines_[id], 0);
    wgpuComputePassEncoderSetPipeline(cpass, pipelines_[id]);
    wgpuComputePassEncoderSetBindGroup(cpass, 0, bindGroup(gpu, id, layout, entries), 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cpass, gx, gy, 1);
    wgpuBindGroupLayoutRelease(layout);
  };

  auto runGrid = [&](Pass id, uint32_t n, std::vector<WGPUBindGroupEntry> entries) {
    WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipelines_[id], 0);
    wgpuComputePassEncoderSetPipeline(cpass, pipelines_[id]);
    wgpuComputePassEncoderSetBindGroup(cpass, 0, bindGroup(gpu, id, layout, entries), 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cpass, groups(n), groups(n), 1);
    wgpuBindGroupLayoutRelease(layout);
  };

  const int steps = std::max(substeps, 1);
  const int inkSteps = std::max(inkSubsteps, 1);

  // =============================================================== WATERCOLOUR
  if (mode_ == PaintMode::Watercolor) {
    // Deposition (kSplat) moved to depositDab() -- called once per emitted
    // arc-length dab, entirely outside this function (ADR-0003, 1.3). This
    // loop only ever advances the physics substeps now; nothing below reads
    // brushActive/brushA/B.
    for (int s2 = 0; s2 < steps; ++s2) {
      // ---- MoveWater (Curtis §4.1) ----
      run(kUpdateVel, {ub, samplerEntry(1, linear_), texEntry(2, water_.src()),
                       texEntry(3, paperView_), texEntry(4, water_.dst())});
      water_.flip();

      run(kDivergence, {ub, texEntry(1, water_.src()), texEntry(2, aux_.dst())});
      aux_.flip();

      for (int i = 0; i < jacobiIterations; ++i) {
        run(kJacobi, {ub, texEntry(1, water_.src()), texEntry(2, aux_.src()),
                      texEntry(3, aux_.dst())});
        aux_.flip();
      }

      run(kProject, {ub, texEntry(1, water_.src()), texEntry(2, aux_.src()),
                     texEntry(3, water_.dst())});
      water_.flip();

      run(kFlowOutward, {ub, samplerEntry(1, linear_), texEntry(2, water_.src()),
                         texEntry(3, water_.dst())});
      water_.flip();

      // The water layer itself moves, so a tilted board produces a run rather
      // than just sloshing pigment inside a stationary wet patch.
      run(kAdvectWater, {ub, texEntry(1, water_.src()), texEntry(2, water_.dst())});
      water_.flip();

      // ---- MovePigment / TransferPigment (Curtis §4.2) ----
      run(kAdvectPig, {ub, texEntry(1, water_.src()),
                       texEntry(2, pigC_.src()), texEntry(3, pigR_.src()),
                       texEntry(4, pigC_.dst()), texEntry(5, pigR_.dst())});
      pigC_.flip(); pigR_.flip();

      run(kTransferPig,
          {ub, texEntry(1, water_.src()), texEntry(2, paperView_),
           texEntry(3, pigC_.src()), texEntry(4, pigR_.src()),
           texEntry(5, depC_.src()), texEntry(6, depR_.src()),
           texEntry(7, pigC_.dst()), texEntry(8, pigR_.dst()),
           texEntry(9, depC_.dst()), texEntry(10, depR_.dst())});
      pigC_.flip(); pigR_.flip(); depC_.flip(); depR_.flip();

      // ---- SimulateCapillaryFlow (Curtis §4.3) ----
      run(kCapillary, {ub, texEntry(1, water_.src()), texEntry(2, paperView_),
                       texEntry(3, sat_.src()), texEntry(4, water_.dst()),
                       texEntry(5, sat_.dst())});
      water_.flip(); sat_.flip();
    }
  }

  // ======================================================================= OIL
  // IMPaSTo Fig. 5: contact -> velocity -> conservative advection -> transfer.
  // No pressure solve at all; oil does not flow on its own, it goes where the
  // brush pushes it.
  else if (mode_ == PaintMode::Oil) {
    run(kOilSplat, {ub, texEntry(1, water_.src()), texEntry(2, water_.dst())});
    water_.flip();

    for (int s2 = 0; s2 < steps; ++s2) {
      run(kOilVelocity, {ub, texEntry(1, water_.src()), texEntry(2, water_.dst())});
      water_.flip();

      run(kOilAdvect, {ub, texEntry(1, water_.src()), texEntry(2, pigC_.src()),
                       texEntry(3, pigR_.src()), texEntry(4, water_.dst()),
                       texEntry(5, pigC_.dst()), texEntry(6, pigR_.dst())});
      water_.flip(); pigC_.flip(); pigR_.flip();

      // Canvas side and brush side read the same "before" state, so the order of
      // these two does not matter; they are split only to avoid many canvas
      // texels racing on one brush cell.
      run(kOilTransfer,
          {ub, texEntry(1, water_.src()), texEntry(2, pigC_.src()),
           texEntry(3, pigR_.src()), texEntry(4, brushVol_.src()),
           texEntry(5, brushC_.src()), texEntry(6, brushR_.src()),
           texEntry(7, water_.dst()), texEntry(8, pigC_.dst()),
           texEntry(9, pigR_.dst())});

      runGrid(kOilBrush, kBrushGrid,
              {ub, texEntry(1, water_.src()), texEntry(2, pigC_.src()),
               texEntry(3, pigR_.src()), texEntry(4, brushVol_.src()),
               texEntry(5, brushC_.src()), texEntry(6, brushR_.src()),
               texEntry(7, brushVol_.dst()), texEntry(8, brushC_.dst()),
               texEntry(9, brushR_.dst())});

      water_.flip(); pigC_.flip(); pigR_.flip();
      brushVol_.flip(); brushC_.flip(); brushR_.flip();
    }
  }

  // ======================================================================= INK
  // MoXi: deposit -> stream (with bounce-back) -> collide -> move constituents.
  else {
    // Same move as watercolour above: ink deposition (kInkSplat) now happens
    // in depositDab(), not here.
    for (int s2 = 0; s2 < inkSteps; ++s2) {
      run(kInkStream, {ub, texEntry(1, lbmA_.src()), texEntry(2, lbmB_.src()),
                       texEntry(3, lbmC_.src()), texEntry(4, paperView_),
                       texEntry(5, lbmA_.dst()), texEntry(6, lbmB_.dst()),
                       texEntry(7, lbmC_.dst())});
      lbmA_.flip(); lbmB_.flip(); lbmC_.flip();

      run(kInkCollide, {ub, texEntry(1, lbmA_.src()), texEntry(2, lbmB_.src()),
                        texEntry(3, lbmC_.src()), texEntry(4, lbmA_.dst()),
                        texEntry(5, lbmB_.dst()), texEntry(6, lbmC_.dst()),
                        texEntry(7, water_.dst())});
      lbmA_.flip(); lbmB_.flip(); lbmC_.flip(); water_.flip();

      run(kInkPigment, {ub, texEntry(1, water_.src()), texEntry(2, pigC_.src()),
                        texEntry(3, pigR_.src()), texEntry(4, depC_.src()),
                        texEntry(5, depR_.src()), texEntry(6, lbmC_.src()),
                        texEntry(7, pigC_.dst()), texEntry(8, pigR_.dst()),
                        texEntry(9, depC_.dst()), texEntry(10, depR_.dst()),
                        texEntry(11, lbmC_.dst())});
      pigC_.flip(); pigR_.flip(); depC_.flip(); depR_.flip(); lbmC_.flip();
    }
  }

  wgpuComputePassEncoderEnd(cpass);
  wgpuComputePassEncoderRelease(cpass);

  // ---- resolve to the canvas texture for ImGui ----
  {
    WGPURenderPassColorAttachment att = {};
    att.view = canvasView_;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue = {0.0, 0.0, 0.0, 1.0};

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;

    // No sampler and no LUT: the composite only goes latent -> RGB, which is a
    // pure polynomial. Every read is an exact texel fetch.
    std::vector<WGPUBindGroupEntry> entries = {
        ub,
        texEntry(1, depC_.src()), texEntry(2, depR_.src()),
        texEntry(3, pigC_.src()), texEntry(4, pigR_.src()),
        texEntry(5, water_.src()), texEntry(6, sat_.src()),
        texEntry(7, paperView_),
    };
    WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(composite_, 0);
    WGPUBindGroup bg = bindGroup(gpu, kPassCount, layout, entries);

    WGPURenderPassEncoder rpass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    wgpuRenderPassEncoderSetPipeline(rpass, composite_);
    wgpuRenderPassEncoderSetBindGroup(rpass, 0, bg, 0, nullptr);
    wgpuRenderPassEncoderDraw(rpass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(rpass);
    wgpuRenderPassEncoderRelease(rpass);
    wgpuBindGroupLayoutRelease(layout);
  }

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
}

void PaintSim::depositDab(GpuContext& gpu, const SimParams& paramsIn, float x, float y,
                          const SelectionMask* /*selectionMask*/) {
  // Oil deposits through frame()'s own kOilSplat/kOilTransfer/kOilBrush
  // instead (see PaintSim.hpp's comment on this method) -- nothing to do.
  if (mode_ != PaintMode::Watercolor && mode_ != PaintMode::Ink) return;

  // `selectionMask` is the phase-2 seam reservation (see PaintSim.hpp) --
  // deliberately unread. Nothing populates a SelectionMask before the
  // "Select and paste" phase; gating the splat itself belongs there.

  SimParams params = paramsIn;
  params.resolutionX = width_;
  params.resolutionY = height_;
  params.mode = static_cast<uint32_t>(mode_);
  // A dab is a point, not a segment: distToSegment (shaders/include/
  // common.wgsl) degenerates correctly when brushA == brushB.
  params.brushAx = x; params.brushAy = y;
  params.brushBx = x; params.brushBy = y;
  params.brushActive = 1;
  // dt plays no role here -- ADR-0003 strips the `* P.dt` scaling from both
  // kSplat and kInkSplat, so a dab deposits the same fixed quantity whatever
  // this value is. Carried through unchanged only because it is part of the
  // shared uniform layout, not because anything downstream reads it.
  wgpuQueueWriteBuffer(gpu.queue, uniform_, 0, &params, sizeof(params));

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, nullptr);

  const uint32_t gx = groups(width_);
  const uint32_t gy = groups(height_);
  const WGPUBindGroupEntry ub = bufEntry(0, uniform_, sizeof(SimParams));

  auto run = [&](Pass id, std::vector<WGPUBindGroupEntry> entries) {
    WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipelines_[id], 0);
    wgpuComputePassEncoderSetPipeline(cpass, pipelines_[id]);
    wgpuComputePassEncoderSetBindGroup(cpass, 0, bindGroup(gpu, id, layout, entries), 0, nullptr);
    wgpuComputePassEncoderDispatchWorkgroups(cpass, gx, gy, 1);
    wgpuBindGroupLayoutRelease(layout);
  };

  if (mode_ == PaintMode::Watercolor) {
    run(kSplat, {ub, texEntry(1, water_.src()), texEntry(2, pigC_.src()),
                 texEntry(3, pigR_.src()), texEntry(4, water_.dst()),
                 texEntry(5, pigC_.dst()), texEntry(6, pigR_.dst())});
    water_.flip(); pigC_.flip(); pigR_.flip();
  } else {  // Ink
    run(kInkSplat, {ub, texEntry(1, water_.src()), texEntry(2, lbmC_.src()),
                    texEntry(3, pigC_.src()), texEntry(4, pigR_.src()),
                    texEntry(5, lbmC_.dst()), texEntry(6, pigC_.dst()),
                    texEntry(7, pigR_.dst())});
    lbmC_.flip(); pigC_.flip(); pigR_.flip();
  }

  wgpuComputePassEncoderEnd(cpass);
  wgpuComputePassEncoderRelease(cpass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
}

// PRD Q3 / PLAN.md Phase 2 step 11: desaturate canvasView_ into
// grayscaleView_, a small dedicated blit -- deliberately its own render pass
// rather than a flag on the composite pass above, so canvas_ itself is
// structurally unreachable from here (this function never binds canvas_ as
// anything but a *read*-only texture_2d, and never opens a render pass whose
// attachment is canvasView_). Called once per frame, only while the
// grayscale toggle is on, from main.cpp *after* this frame's frame()/
// depositDab() calls have already submitted their own work -- so the GPU
// executes this blit after the composite it reads from, not before (see
// main.cpp's call site for why that ordering, not merely "eventually",
// matters).
void PaintSim::updateGrayscalePreview(GpuContext& gpu) {
  if (!grayscalePipeline_ || !grayscaleView_ || !canvasView_) return;

  WGPUBindGroupEntry entry = texEntry(0, canvasView_);
  WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(grayscalePipeline_, 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = layout;
  bgd.label = sv("grayscale-preview");
  bgd.entryCount = 1;
  bgd.entries = &entry;
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPURenderPassColorAttachment att = {};
  att.view = grayscaleView_;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 1.0};

  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, grayscalePipeline_);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(layout);
}

// PLAN.md Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch ->
// un-shape"). See this method's declaration in PaintSim.hpp for the full
// design (deliberate live-canvas target, the domain contract, the
// single-run narrow scope); this is the implementation of that contract.
void PaintSim::updateGradePreview(GpuContext& gpu, const OpStack& opStack) {
  if (!gradePipeline_ || !gradedView_ || !canvasView_) return;

  // ---- rebake gate: only when the OpStack actually changed ----
  if (!hasBakedLut_ || opStack.version() != lastBakedOpStackVersion_) {
    if (hasBakedLut_) releaseLut3D(lut_);

    // Narrow scope (see this method's header comment): only the first run
    // is baked. `runs.empty()` -- no PointA content at all -- is treated
    // as an empty ops list, which bakeLut() itself already handles as a
    // valid seed-only identity LUT.
    const auto runs = opStack.detectRuns();
    std::vector<Op> ops;
    if (!runs.empty()) {
      const OpRun& run = runs.front();
      // color/LutBake.hpp's own documented calling convention: an
      // ordered std::vector<Op> slice built by looping the run's raw
      // [startIndex, endIndex) range and calling OpStack::at(i) for each
      // index -- NOT run.ops, which is already-composed std::function
      // closures bakeLut() has no way to run on the GPU. bakeLut() itself
      // skips any entry with !enabled or opClass != PointA, so copying
      // the raw range verbatim (including any disabled entries within
      // it) is correct and saves re-deriving OpRun::ops's own filtering
      // logic here.
      for (size_t i = run.startIndex; i < run.endIndex; ++i) ops.push_back(opStack.at(i));
    }

    lut_ = bakeLut(gpu, ops);
    if (!lut_.texture) {
      // bakeLut()'s own documented failure contract: a shader-compile or
      // pipeline-build failure logs to stderr and returns a default
      // (null) Lut3D. Leave hasBakedLut_ false so the next call retries
      // the bake rather than believing a stale/nonexistent LUT is ready,
      // and draw nothing this call rather than binding a null texture.
      hasBakedLut_ = false;
      return;
    }
    hasBakedLut_ = true;
    lastBakedOpStackVersion_ = opStack.version();
  }

  // ---- blit: canvasView_ -> (shaper -> LUT sample -> un-shaper) -> gradedView_ ----
  // linear_ (built in init(), ClampToEdge on all three axes including W)
  // is reused as-is rather than creating a second sampler -- WGPUSampler
  // objects are dimension-agnostic in WebGPU, and this one was already
  // built with 3-D use anticipated.
  WGPUBindGroupEntry entries[3] = {
      texEntry(0, canvasView_),
      texEntry(1, lut_.view),
      samplerEntry(2, linear_),
  };
  WGPUBindGroupLayout layout = wgpuRenderPipelineGetBindGroupLayout(gradePipeline_, 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = layout;
  bgd.label = sv("grade-preview");
  bgd.entryCount = 3;
  bgd.entries = entries;
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPURenderPassColorAttachment att = {};
  att.view = gradedView_;
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 1.0};

  WGPURenderPassDescriptor rp = {};
  rp.colorAttachmentCount = 1;
  rp.colorAttachments = &att;

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
  wgpuRenderPassEncoderSetPipeline(pass, gradePipeline_);
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(layout);
}

bool PaintSim::readbackCanvas(GpuContext& gpu, std::vector<uint8_t>& out) {
  const uint32_t bytesPerRow = width_ * 4;  // 256-aligned for any width we use
  if (bytesPerRow % 256 != 0) {
    std::fprintf(stderr, "[sim] readback needs a width multiple of 64\n");
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height_;

  WGPUBufferDescriptor bd = {};
  bd.label = sv("canvas readback");
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = canvas_;
  srcTex.aspect = WGPUTextureAspect_All;

  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = height_;

  WGPUExtent3D extent = {width_, height_, 1};

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
    const void* src = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (src) {
      out.resize(total);
      std::memcpy(out.data(), src, total);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

// Mirrors readbackCanvas() exactly (same RGBA8 format, same blocking-map
// technique), reading graded_ instead of canvas_.
bool PaintSim::readbackGraded(GpuContext& gpu, std::vector<uint8_t>& out) {
  const uint32_t bytesPerRow = width_ * 4;  // 256-aligned for any width we use
  if (bytesPerRow % 256 != 0) {
    std::fprintf(stderr, "[sim] readback needs a width multiple of 64\n");
    return false;
  }
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height_;

  WGPUBufferDescriptor bd = {};
  bd.label = sv("graded readback");
  bd.size = total;
  bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
  WGPUBuffer staging = wgpuDeviceCreateBuffer(gpu.device, &bd);

  WGPUTexelCopyTextureInfo srcTex = {};
  srcTex.texture = graded_;
  srcTex.aspect = WGPUTextureAspect_All;

  WGPUTexelCopyBufferInfo dstBuf = {};
  dstBuf.buffer = staging;
  dstBuf.layout.bytesPerRow = bytesPerRow;
  dstBuf.layout.rowsPerImage = height_;

  WGPUExtent3D extent = {width_, height_, 1};

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
    const void* src = wgpuBufferGetConstMappedRange(staging, 0, total);
    if (src) {
      out.resize(total);
      std::memcpy(out.data(), src, total);
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

bool PaintSim::readbackField(GpuContext& gpu, WGPUTexture tex,
                                  WGPUTextureFormat format,
                                  std::vector<float>& out) {
  const uint32_t bytesPerRow = width_ * texelBytes(format);
  if (bytesPerRow % 256 != 0) return false;
  const uint64_t total = static_cast<uint64_t>(bytesPerRow) * height_;

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
  dstBuf.layout.rowsPerImage = height_;
  WGPUExtent3D extent = {width_, height_, 1};

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
      const size_t n = static_cast<size_t>(width_) * height_ * 4;
      out.resize(n);
      if (format == WGPUTextureFormat_RGBA32Float) {
        std::memcpy(out.data(), raw, n * sizeof(float));
      } else {
        const auto* h = static_cast<const uint16_t*>(raw);
        for (size_t i = 0; i < n; ++i) out[i] = halfToFloat(h[i]);
      }
      ok = true;
    }
    wgpuBufferUnmap(staging);
  }
  wgpuBufferDestroy(staging);
  wgpuBufferRelease(staging);
  return ok;
}

PaintSim::Stats PaintSim::computeStats(GpuContext& gpu) {
  Stats s;
  std::vector<float> pig, dep, water;
  if (!readbackField(gpu, pigC_.srcTex(), pigC_.format, pig)) return s;
  if (!readbackField(gpu, depC_.srcTex(), depC_.format, dep)) return s;
  if (!readbackField(gpu, water_.srcTex(), water_.format, water)) return s;

  const size_t cells = static_cast<size_t>(width_) * height_;
  double speedSum = 0;
  for (size_t i = 0; i < cells; ++i) {
    const float pm = pig[i * 4 + 3];
    const float dm = dep[i * 4 + 3];
    s.suspended += pm;
    s.deposited += dm;
    if (pm + dm > 1e-4) s.pigmentCells += 1;

    const float u = water[i * 4 + 0], v = water[i * 4 + 1];
    const float p = water[i * 4 + 2], m = water[i * 4 + 3];
    s.totalWater += p;
    if (m > 0.01f) {
      s.wetCells += 1;
      const double sp = std::sqrt(static_cast<double>(u) * u + static_cast<double>(v) * v);
      speedSum += sp;
      if (sp > s.maxSpeed) s.maxSpeed = sp;
    }
  }
  s.meanSpeed = s.wetCells > 0 ? speedSum / s.wetCells : 0.0;
  return s;
}

void PaintSim::shutdown() {
  releaseFields();
  if (linear_) { wgpuSamplerRelease(linear_); linear_ = nullptr; }
  if (uniform_) { wgpuBufferDestroy(uniform_); wgpuBufferRelease(uniform_); uniform_ = nullptr; }
  for (int i = 0; i < kPassCount; ++i) {
    if (pipelines_[i]) { wgpuComputePipelineRelease(pipelines_[i]); pipelines_[i] = nullptr; }
  }
  if (composite_) { wgpuRenderPipelineRelease(composite_); composite_ = nullptr; }
  if (grayscalePipeline_) { wgpuRenderPipelineRelease(grayscalePipeline_); grayscalePipeline_ = nullptr; }
  if (gradePipeline_) { wgpuRenderPipelineRelease(gradePipeline_); gradePipeline_ = nullptr; }
}

PaintSim* ensurePaintSim(std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
                         uint32_t width, uint32_t height, const MixboxLut& lut) {
  if (sim) return sim.get();
  auto candidate = std::make_unique<PaintSim>();
  if (!candidate->init(gpu, width, height, lut)) {
    std::fprintf(stderr, "[sim] Simulation failed to initialise.\n");
    return nullptr;
  }
  sim = std::move(candidate);
  return sim.get();
}

}  // namespace np
