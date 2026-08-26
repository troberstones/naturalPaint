#include "ui/CanvasQuad.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "color/Space.hpp"
#include "imgui.h"
#include "backends/imgui_impl_wgpu.h"

namespace np {
namespace {

// Four draws is the whole of today's demand -- the sim canvas, the document
// over it, the navigator thumbnail and the companion pane -- and the split
// caps visible documents at two (ADR-0001), so nothing here can grow with the
// tab count. Eight leaves room for one more overlay without a resize; past
// that `canvasQuadsDropped()` counts rather than the frame silently losing a
// quad, which is the failure this counter exists to make visible.
constexpr size_t kMaxQuads = 8;
constexpr size_t kVertsPerQuad = 6;

struct Vertex {
  float x, y;  // clip space, baked on the CPU -- see the header on ordering
  float u, v;
};

struct Quad {
  WGPUTextureView view = nullptr;
  WGPUBindGroup bindGroup = nullptr;
  Vertex verts[kVertsPerQuad] = {};
};

struct State {
  WGPUDevice device = nullptr;
  WGPURenderPipeline pipeline = nullptr;
  WGPUBindGroupLayout bindGroupLayout = nullptr;
  WGPUSampler sampler = nullptr;
  WGPUBuffer vertexBuffer = nullptr;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  std::vector<Quad> quads;
  size_t drawn = 0;
  size_t dropped = 0;
  bool warnedOverflow = false;
};

State& state() {
  static State s;
  return s;
}

// The fragment shader. `kEncodeSrc` is the exact piecewise IEC 61966-2-1
// encode -- the same curve as color/Space's `srgbEncode()`, and asserted
// against it rather than trusted to match. It is compiled in only when the
// attachment does *not* do it in hardware; on an sRGB attachment the shader
// passes linear through and the hardware performs the identical curve, so the
// two variants put the same byte on screen by construction.
//
// Alpha is never encoded: it is a coverage fraction, not a light level.
constexpr const char* kEncodeSrc = R"(
fn transfer(c : vec3<f32>) -> vec3<f32> {
  let lo = c * 12.92;
  let hi = 1.055 * pow(max(c, vec3<f32>(0.0)), vec3<f32>(1.0 / 2.4)) - 0.055;
  return select(hi, lo, c <= vec3<f32>(0.0031308));
}
)";

constexpr const char* kPassThroughSrc = R"(
fn transfer(c : vec3<f32>) -> vec3<f32> { return c; }
)";

constexpr const char* kBodySrc = R"(
@group(0) @binding(0) var samp : sampler;
@group(0) @binding(1) var tex  : texture_2d<f32>;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@location(0) pos : vec2<f32>, @location(1) uv : vec2<f32>) -> VSOut {
  var out : VSOut;
  out.pos = vec4<f32>(pos, 0.0, 1.0);
  out.uv = uv;
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let c = textureSample(tex, samp, in.uv);
  return vec4<f32>(transfer(c.rgb), c.a);
}
)";

// Screen space (ImGui's logical pixels) to clip space, using the same
// DisplayPos/DisplaySize the backend projects its own vertices with.
void toClip(const ImVec2& p, const ImVec2& origin, const ImVec2& size, float* outX, float* outY) {
  *outX = size.x > 0.0f ? ((p.x - origin.x) / size.x) * 2.0f - 1.0f : -1.0f;
  *outY = size.y > 0.0f ? 1.0f - ((p.y - origin.y) / size.y) * 2.0f : 1.0f;
}

void canvasQuadCallback(const ImDrawList*, const ImDrawCmd* cmd) {
  State& s = state();
  if (cmd == nullptr || cmd->UserCallbackData == nullptr || s.pipeline == nullptr) return;
  size_t index = 0;
  std::memcpy(&index, cmd->UserCallbackData, sizeof(index));
  if (index >= s.quads.size()) return;
  const Quad& q = s.quads[index];
  if (q.bindGroup == nullptr) return;

  const auto* rs =
      static_cast<ImGui_ImplWGPU_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
  if (rs == nullptr || rs->RenderPassEncoder == nullptr) return;
  WGPURenderPassEncoder pass = rs->RenderPassEncoder;

  // The backend sets the scissor only for its own draw commands, so a callback
  // inherits whatever the previous command left behind. Setting it from this
  // command's own clip rect is what keeps the canvas inside its band when the
  // window is small enough for the quad to reach under the controls column.
  const ImDrawData* dd = ImGui::GetDrawData();
  if (dd != nullptr) {
    const ImVec2 off = dd->DisplayPos, scale = dd->FramebufferScale;
    const float fbW = dd->DisplaySize.x * scale.x, fbH = dd->DisplaySize.y * scale.y;
    float x0 = (cmd->ClipRect.x - off.x) * scale.x, y0 = (cmd->ClipRect.y - off.y) * scale.y;
    float x1 = (cmd->ClipRect.z - off.x) * scale.x, y1 = (cmd->ClipRect.w - off.y) * scale.y;
    x0 = std::fmax(x0, 0.0f);
    y0 = std::fmax(y0, 0.0f);
    x1 = std::fmin(x1, fbW);
    y1 = std::fmin(y1, fbH);
    if (x1 <= x0 || y1 <= y0) return;
    wgpuRenderPassEncoderSetScissorRect(pass, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                        static_cast<uint32_t>(x1 - x0),
                                        static_cast<uint32_t>(y1 - y0));
  }

  wgpuRenderPassEncoderSetPipeline(pass, s.pipeline);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, s.vertexBuffer,
                                       index * kVertsPerQuad * sizeof(Vertex),
                                       kVertsPerQuad * sizeof(Vertex));
  wgpuRenderPassEncoderSetBindGroup(pass, 0, q.bindGroup, 0, nullptr);
  wgpuRenderPassEncoderDraw(pass, kVertsPerQuad, 1, 0, 0);
}

void queue(ImDrawList* dl, WGPUTextureView view, const ImVec2& p00, const ImVec2& p10,
           const ImVec2& p11, const ImVec2& p01) {
  State& s = state();
  if (dl == nullptr || view == nullptr || s.pipeline == nullptr) return;
  if (s.quads.size() >= kMaxQuads) {
    ++s.dropped;
    if (!s.warnedOverflow) {
      s.warnedOverflow = true;
      std::printf("[canvas-quad] WARNING: more than %zu document quads in one frame; the extra "
                  "ones are not drawn. Raise kMaxQuads in ui/CanvasQuad.cpp.\n",
                  kMaxQuads);
    }
    return;
  }

  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 origin = ImGui::GetMainViewport()->Pos;
  const ImVec2 size = io.DisplaySize;

  Quad q;
  q.view = view;
  const ImVec2 pts[kVertsPerQuad] = {p00, p10, p01, p01, p10, p11};
  const ImVec2 uvs[kVertsPerQuad] = {{0, 0}, {1, 0}, {0, 1}, {0, 1}, {1, 0}, {1, 1}};
  for (size_t i = 0; i < kVertsPerQuad; ++i) {
    toClip(pts[i], origin, size, &q.verts[i].x, &q.verts[i].y);
    q.verts[i].u = uvs[i].x;
    q.verts[i].v = uvs[i].y;
  }

  size_t index = s.quads.size();
  s.quads.push_back(q);
  ++s.drawn;
  // The three-argument AddCallback copies `index` into the draw list's own
  // buffer, so this does not depend on the lifetime of a local.
  dl->AddCallback(canvasQuadCallback, &index, sizeof(index));
  // Hands ImGui's own pipeline, vertex buffer and bind groups back, so the
  // draw commands after this one are unaffected by what the callback bound.
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

}  // namespace

// Pipeline, layout and sampler for one target format. Shared by
// `initCanvasQuad()` and by `renderCanvasQuadForTest()`, so the shader and the
// pipeline a test measures are the ones the application draws with, not a
// transcription of them.
namespace {

struct Resources {
  WGPURenderPipeline pipeline = nullptr;
  WGPUBindGroupLayout bindGroupLayout = nullptr;
  WGPUSampler sampler = nullptr;
};

Resources makeResources(WGPUDevice device, WGPUTextureFormat format) {
  Resources r;
  if (device == nullptr) return r;

  const std::string src =
      std::string(presentFormatIsSrgb(format) ? kPassThroughSrc : kEncodeSrc) + kBodySrc;
  WGPUShaderSourceWGSL wgsl = {};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code = sv(src.c_str());
  WGPUShaderModuleDescriptor smd = {};
  smd.nextInChain = &wgsl.chain;
  smd.label = sv("canvas-quad");
  WGPUShaderModule mod = wgpuDeviceCreateShaderModule(device, &smd);
  if (mod == nullptr) return r;

  WGPUSamplerDescriptor sd = {};
  sd.label = sv("canvas-quad");
  sd.addressModeU = WGPUAddressMode_ClampToEdge;
  sd.addressModeV = WGPUAddressMode_ClampToEdge;
  sd.addressModeW = WGPUAddressMode_ClampToEdge;
  // **NEAREST when magnified, LINEAR when minified, and the split is the whole
  // point.**
  //
  // `magFilter` is what runs when the canvas is zoomed IN -- one document texel
  // covering many screen pixels -- and it is the only filter a painter ever
  // sees as "blur". Linear there interpolates between texel centres, so a hard
  // one-texel edge becomes a soft ramp several screen pixels wide and a
  // zoomed-in canvas stops showing what is actually stored. Nearest shows the
  // texel, which is what a paint application is for: at 800% the user is
  // looking at individual texels ON PURPOSE, usually to fix one.
  //
  // `minFilter` stays Linear, and that is not an oversight. It runs when the
  // canvas is zoomed OUT, where one screen pixel covers many document texels
  // and nearest picks exactly one of them arbitrarily -- which shimmers as the
  // view pans and drops thin strokes entirely between sample points. That is
  // aliasing, not crispness, and it makes a zoomed-out canvas lie about what is
  // on it in the opposite direction. Photoshop, Krita and Aseprite all make
  // this same split for this same reason.
  //
  // `mipmapFilter` is Nearest to match: the document texture is created with
  // `mipLevelCount = 1` (ui/DocumentTexture), so there is no chain to filter
  // between and this value never selects anything -- Nearest simply says so
  // rather than implying a chain exists.
  sd.magFilter = WGPUFilterMode_Nearest;
  sd.minFilter = WGPUFilterMode_Linear;
  sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
  sd.maxAnisotropy = 1;
  r.sampler = wgpuDeviceCreateSampler(device, &sd);

  WGPUBindGroupLayoutEntry bgle[2] = {};
  bgle[0].binding = 0;
  bgle[0].visibility = WGPUShaderStage_Fragment;
  bgle[0].sampler.type = WGPUSamplerBindingType_Filtering;
  bgle[1].binding = 1;
  bgle[1].visibility = WGPUShaderStage_Fragment;
  bgle[1].texture.sampleType = WGPUTextureSampleType_Float;
  bgle[1].texture.viewDimension = WGPUTextureViewDimension_2D;
  WGPUBindGroupLayoutDescriptor bgld = {};
  bgld.entryCount = 2;
  bgld.entries = bgle;
  r.bindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &bgld);

  WGPUPipelineLayoutDescriptor pld = {};
  pld.bindGroupLayoutCount = 1;
  pld.bindGroupLayouts = &r.bindGroupLayout;
  WGPUPipelineLayout pl = wgpuDeviceCreatePipelineLayout(device, &pld);

  WGPUVertexAttribute attrs[2] = {};
  attrs[0].format = WGPUVertexFormat_Float32x2;
  attrs[0].offset = 0;
  attrs[0].shaderLocation = 0;
  attrs[1].format = WGPUVertexFormat_Float32x2;
  attrs[1].offset = 2 * sizeof(float);
  attrs[1].shaderLocation = 1;
  WGPUVertexBufferLayout vbl = {};
  vbl.arrayStride = sizeof(Vertex);
  vbl.stepMode = WGPUVertexStepMode_Vertex;
  vbl.attributeCount = 2;
  vbl.attributes = attrs;

  // Dear ImGui's own blend state, so a quad drawn by this pipeline composites
  // with the chrome around it exactly as the `AddImageQuad` it replaced did.
  // The textures carry straight (un-premultiplied) alpha, which is what
  // SrcAlpha/OneMinusSrcAlpha expects.
  WGPUBlendState blend = {};
  blend.color.operation = WGPUBlendOperation_Add;
  blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
  blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
  blend.alpha.operation = WGPUBlendOperation_Add;
  blend.alpha.srcFactor = WGPUBlendFactor_One;
  blend.alpha.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

  WGPUColorTargetState target = {};
  target.format = format;
  target.blend = &blend;
  target.writeMask = WGPUColorWriteMask_All;

  WGPUFragmentState fs = {};
  fs.module = mod;
  fs.entryPoint = sv("fs");
  fs.targetCount = 1;
  fs.targets = &target;

  WGPURenderPipelineDescriptor rd = {};
  rd.label = sv("canvas-quad");
  rd.layout = pl;
  rd.vertex.module = mod;
  rd.vertex.entryPoint = sv("vs");
  rd.vertex.bufferCount = 1;
  rd.vertex.buffers = &vbl;
  rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
  rd.primitive.frontFace = WGPUFrontFace_CCW;
  rd.primitive.cullMode = WGPUCullMode_None;
  rd.multisample.count = 1;
  rd.multisample.mask = 0xFFFFFFFF;
  rd.fragment = &fs;
  r.pipeline = wgpuDeviceCreateRenderPipeline(device, &rd);

  wgpuPipelineLayoutRelease(pl);
  wgpuShaderModuleRelease(mod);
  return r;
}

void releaseResources(Resources& r) {
  if (r.pipeline != nullptr) wgpuRenderPipelineRelease(r.pipeline);
  if (r.bindGroupLayout != nullptr) wgpuBindGroupLayoutRelease(r.bindGroupLayout);
  if (r.sampler != nullptr) wgpuSamplerRelease(r.sampler);
  r = Resources{};
}

}  // namespace

void initCanvasQuad(GpuContext& gpu) {
  State& s = state();
  if (s.pipeline != nullptr && s.format == gpu.surfaceFormat) return;
  shutdownCanvasQuad();
  s.device = gpu.device;
  s.format = gpu.surfaceFormat;
  if (gpu.device == nullptr) return;

  const Resources r = makeResources(gpu.device, gpu.surfaceFormat);
  s.pipeline = r.pipeline;
  s.bindGroupLayout = r.bindGroupLayout;
  s.sampler = r.sampler;
  if (s.pipeline == nullptr) return;

  WGPUBufferDescriptor bd = {};
  bd.label = sv("canvas-quad-vertices");
  bd.size = kMaxQuads * kVertsPerQuad * sizeof(Vertex);
  bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  s.vertexBuffer = wgpuDeviceCreateBuffer(gpu.device, &bd);
}

void shutdownCanvasQuad() {
  State& s = state();
  endCanvasQuadFrame();
  s.quads.clear();
  if (s.vertexBuffer != nullptr) {
    wgpuBufferDestroy(s.vertexBuffer);
    wgpuBufferRelease(s.vertexBuffer);
    s.vertexBuffer = nullptr;
  }
  if (s.pipeline != nullptr) {
    wgpuRenderPipelineRelease(s.pipeline);
    s.pipeline = nullptr;
  }
  if (s.bindGroupLayout != nullptr) {
    wgpuBindGroupLayoutRelease(s.bindGroupLayout);
    s.bindGroupLayout = nullptr;
  }
  if (s.sampler != nullptr) {
    wgpuSamplerRelease(s.sampler);
    s.sampler = nullptr;
  }
  s.format = WGPUTextureFormat_Undefined;
  s.device = nullptr;
}

void addCanvasQuad(ImDrawList* dl, WGPUTextureView view, const ImVec2& q00, const ImVec2& q10,
                   const ImVec2& q11, const ImVec2& q01) {
  queue(dl, view, q00, q10, q11, q01);
}

void addCanvasImage(ImDrawList* dl, WGPUTextureView view, const ImVec2& min, const ImVec2& max) {
  queue(dl, view, min, ImVec2(max.x, min.y), max, ImVec2(min.x, max.y));
}

void flushCanvasQuads(GpuContext& gpu) {
  State& s = state();
  if (s.pipeline == nullptr || s.quads.empty()) return;

  std::vector<Vertex> verts;
  verts.reserve(s.quads.size() * kVertsPerQuad);
  for (const Quad& q : s.quads)
    verts.insert(verts.end(), q.verts, q.verts + kVertsPerQuad);
  wgpuQueueWriteBuffer(gpu.queue, s.vertexBuffer, 0, verts.data(), verts.size() * sizeof(Vertex));

  // Built fresh each frame and released after the submit, rather than cached
  // by view pointer the way the ImGui backend caches its own. That cache is
  // exactly what makes a retired texture view unfreeable (see
  // ui/DocumentTexture.hpp decision 5); this module declines to add a second
  // one. Three or four bind groups a frame is not a cost worth a leak.
  for (Quad& q : s.quads) {
    WGPUBindGroupEntry entries[2] = {};
    entries[0].binding = 0;
    entries[0].sampler = s.sampler;
    entries[1].binding = 1;
    entries[1].textureView = q.view;
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = s.bindGroupLayout;
    bgd.entryCount = 2;
    bgd.entries = entries;
    q.bindGroup = wgpuDeviceCreateBindGroup(gpu.device, &bgd);
  }
}

void endCanvasQuadFrame() {
  State& s = state();
  for (Quad& q : s.quads) {
    if (q.bindGroup != nullptr) {
      wgpuBindGroupRelease(q.bindGroup);
      q.bindGroup = nullptr;
    }
  }
  s.quads.clear();
}

size_t canvasQuadsDrawn() { return state().drawn; }
size_t canvasQuadsDropped() { return state().dropped; }

bool renderCanvasQuadForTest(GpuContext& gpu, WGPUTextureView view, WGPUTexture target,
                             WGPUTextureFormat targetFormat, uint32_t w, uint32_t h) {
  if (gpu.device == nullptr || view == nullptr || target == nullptr) return false;
  Resources r = makeResources(gpu.device, targetFormat);
  if (r.pipeline == nullptr) {
    releaseResources(r);
    return false;
  }

  // A quad filling the target, in the same clip space and the same vertex
  // layout `queue()` bakes -- the transform is the only thing this bypasses,
  // and the transform is not what a transfer function test is about.
  const Vertex verts[kVertsPerQuad] = {
      {-1.0f, 1.0f, 0.0f, 0.0f},  {1.0f, 1.0f, 1.0f, 0.0f},  {-1.0f, -1.0f, 0.0f, 1.0f},
      {-1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 0.0f},  {1.0f, -1.0f, 1.0f, 1.0f}};
  WGPUBufferDescriptor bd = {};
  bd.size = sizeof(verts);
  bd.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
  WGPUBuffer vb = wgpuDeviceCreateBuffer(gpu.device, &bd);
  wgpuQueueWriteBuffer(gpu.queue, vb, 0, verts, sizeof(verts));

  WGPUBindGroupEntry entries[2] = {};
  entries[0].binding = 0;
  entries[0].sampler = r.sampler;
  entries[1].binding = 1;
  entries[1].textureView = view;
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = r.bindGroupLayout;
  bgd.entryCount = 2;
  bgd.entries = entries;
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPUTextureView targetView = wgpuTextureCreateView(target, nullptr);
  WGPURenderPassColorAttachment att = {};
  att.view = targetView;
  att.loadOp = WGPULoadOp_Clear;
  att.storeOp = WGPUStoreOp_Store;
  att.clearValue = {0.0, 0.0, 0.0, 1.0};
  att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
  WGPURenderPassDescriptor rpd = {};
  rpd.colorAttachmentCount = 1;
  rpd.colorAttachments = &att;

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpd);
  wgpuRenderPassEncoderSetPipeline(pass, r.pipeline);
  wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vb, 0, sizeof(verts));
  wgpuRenderPassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, w, h);
  wgpuRenderPassEncoderDraw(pass, kVertsPerQuad, 1, 0, 0);
  wgpuRenderPassEncoderEnd(pass);
  wgpuRenderPassEncoderRelease(pass);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  wgpuBindGroupRelease(bg);
  wgpuTextureViewRelease(targetView);
  wgpuBufferDestroy(vb);
  wgpuBufferRelease(vb);
  releaseResources(r);
  return true;
}

int canvasPresentedByte(float linear, bool attachmentIsSrgb) {
  // Either the shader encodes and the attachment writes the result raw, or the
  // shader passes through and the attachment encodes. Both are `srgbEncode()`
  // applied exactly once, which is the whole claim this module makes.
  (void)attachmentIsSrgb;
  const float encoded = srgbEncode(std::fmax(linear, 0.0f));
  const float clamped = std::fmin(std::fmax(encoded, 0.0f), 1.0f);
  return static_cast<int>(std::lround(clamped * 255.0f));
}

}  // namespace np
