#include "color/LutBake.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <vector>

#include "gfx/ShaderLoader.hpp"

namespace np {
namespace {

// ---------------------------------------------------------------- helpers
//
// Small bind-group-entry builders, mirroring sim/PaintSim.cpp's anonymous-
// namespace bufEntry()/texEntry() exactly in shape -- PaintSim's own copies
// are private to that translation unit, so this file gets its own rather
// than exposing them.

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

// ------------------------------------------------------------- 3-D texture
//
// No 3-D texture exists anywhere else in this codebase (sim/PaintSim.cpp's
// makeField()/makePingPong() are hard-coded to WGPUTextureDimension_2D and
// are load-bearing for the whole running simulation, so they are not
// touched here) -- this is a small, file-local analog of the exact same
// shape sim/PaintSim.hpp's PingPong already establishes for 2-D fields:
// two same-sized textures, a `cur` parity, and src()/dst()/flip().

WGPUTexture make3DLutTexture(WGPUDevice device, int32_t size, const char* label) {
  WGPUTextureDescriptor d = {};
  d.label = sv(label);
  d.dimension = WGPUTextureDimension_3D;
  d.size = {static_cast<uint32_t>(size), static_cast<uint32_t>(size), static_cast<uint32_t>(size)};
  d.format = WGPUTextureFormat_RGBA16Float;
  d.mipLevelCount = 1;
  d.sampleCount = 1;
  // No RenderAttachment (unlike makeField()'s 2-D fields) -- this texture is
  // seeded by a compute pass, not a clear-render-pass. CopySrc is for
  // --selftest's GPU readback (app/SelfTest.cpp's readbackRGBA16F3D()).
  d.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding |
            WGPUTextureUsage_CopySrc;
  return wgpuDeviceCreateTexture(device, &d);
}

struct PingPong3D {
  WGPUTexture tex[2] = {nullptr, nullptr};
  WGPUTextureView view[2] = {nullptr, nullptr};
  int cur = 0;

  WGPUTextureView src() const { return view[cur]; }
  WGPUTextureView dst() const { return view[1 - cur]; }
  WGPUTexture srcTex() const { return tex[cur]; }
  void flip() { cur ^= 1; }
};

void make3DPingPong(WGPUDevice device, PingPong3D& pp, int32_t size, const char* label) {
  for (int i = 0; i < 2; ++i) {
    pp.tex[i] = make3DLutTexture(device, size, label);
    pp.view[i] = wgpuTextureCreateView(pp.tex[i], nullptr);
  }
  pp.cur = 0;
}

void release3DPingPong(PingPong3D& pp) {
  for (int i = 0; i < 2; ++i) {
    if (pp.view[i]) {
      wgpuTextureViewRelease(pp.view[i]);
      pp.view[i] = nullptr;
    }
    if (pp.tex[i]) {
      wgpuTextureDestroy(pp.tex[i]);
      wgpuTextureRelease(pp.tex[i]);
      pp.tex[i] = nullptr;
    }
  }
}

// --------------------------------------------------------------- uniforms
//
// One small struct per op kernel's uniform buffer, laid out to match its
// WGSL counterpart (shaders/lut_op_*.wgsl) byte for byte -- the same
// "matching struct layout by construction, every member naturally aligned"
// discipline sim/PaintSim.hpp's SimParams documents for itself. Every
// struct here is built from vec4-sized (16-byte) chunks specifically to
// avoid WGSL's uniform-address-space scalar-packing subtleties entirely,
// per this step's own design brief.

struct LevelsUniform {
  float chR[4];       // blackIn, whiteIn, gamma, blackOut
  float chG[4];
  float chB[4];
  float whiteOut[4];  // whiteOutR, whiteOutG, whiteOutB, unused
};
static_assert(sizeof(LevelsUniform) == 64, "must match lut_op_levels.wgsl's LevelsUniform");

struct ExposureUniform {
  float stops;
  float pad[3];
};
static_assert(sizeof(ExposureUniform) == 16, "must match lut_op_exposure.wgsl's vec4<f32> P");

struct SaturationUniform {
  float scale;
  float weights[3];
};
static_assert(sizeof(SaturationUniform) == 16, "must match lut_op_saturation.wgsl's vec4<f32> P");

struct GrayscaleUniform {
  float weights[3];
  float pad;
};
static_assert(sizeof(GrayscaleUniform) == 16, "must match lut_op_grayscale.wgsl's vec4<f32> P");

struct ChannelMixerUniform {
  float row0[4];
  float row1[4];
  float row2[4];
};
static_assert(sizeof(ChannelMixerUniform) == 48,
              "must match lut_op_channel_mixer.wgsl's ChannelMixerUniform");

// Curves' one exception: a read-only storage buffer, not a uniform buffer
// (see color/LutBake.hpp / shaders/lut_op_curves.wgsl for why -- variable-
// length per-channel point lists don't fit a uniform buffer's array-stride-
// multiple-of-16 rule without padding every vec2<f32> out to 16 bytes;
// storage buffers have no such rule). Flattened R-then-G-then-B, each
// channel padded up to kMaxCurvePointsPerChannel slots (unused trailing
// slots are never read -- evalCurveGpu() only indexes [0, count)).
struct CurvesGpuBuffer {
  uint32_t countR = 0, countG = 0, countB = 0, pad = 0;
  float points[kMaxCurvePointsPerChannel * 3][2] = {};
};
static_assert(sizeof(CurvesGpuBuffer) == 16 + kMaxCurvePointsPerChannel * 3 * 8,
              "must match lut_op_curves.wgsl's CurvesBuffer");
static_assert(offsetof(CurvesGpuBuffer, points) == 16,
              "the points array must start immediately after the four u32 counts, matching "
              "WGSL's own offset computation for array<vec2<f32>> (8-byte align, and 16 is "
              "already a multiple of 8)");

WGPUBuffer makeUniformBuffer(GpuContext& gpu, const void* data, uint64_t size) {
  WGPUBufferDescriptor bd = {};
  bd.size = size;
  bd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(gpu.device, &bd);
  wgpuQueueWriteBuffer(gpu.queue, buf, 0, data, size);
  return buf;
}

WGPUBuffer makeStorageBuffer(GpuContext& gpu, const void* data, uint64_t size) {
  WGPUBufferDescriptor bd = {};
  bd.size = size;
  bd.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
  WGPUBuffer buf = wgpuDeviceCreateBuffer(gpu.device, &bd);
  wgpuQueueWriteBuffer(gpu.queue, buf, 0, data, size);
  return buf;
}

// Path per PointOpKind, in enum declaration order (core/OpStack.hpp).
const char* kOpKernelPath[] = {
    "lut_op_levels.wgsl",  "lut_op_curves.wgsl",       "lut_op_exposure.wgsl",
    "lut_op_saturation.wgsl", "lut_op_grayscale.wgsl", "lut_op_channel_mixer.wgsl",
};

}  // namespace

void releaseLut3D(Lut3D& lut) {
  if (lut.view) {
    wgpuTextureViewRelease(lut.view);
    lut.view = nullptr;
  }
  if (lut.texture) {
    wgpuTextureDestroy(lut.texture);
    wgpuTextureRelease(lut.texture);
    lut.texture = nullptr;
  }
}

Lut3D bakeLut(GpuContext& gpu, const std::vector<Op>& ops) {
  PingPong3D pp;
  make3DPingPong(gpu.device, pp, kLutSize, "lutbake-pingpong");

  WGPUShaderModule seedMod = compileShader(gpu.device, gpu.instance, "lut_seed.wgsl");
  if (!seedMod) {
    std::fprintf(stderr, "[lutbake] lut_seed.wgsl failed to compile\n");
    release3DPingPong(pp);
    return {};
  }
  WGPUComputePipelineDescriptor seedDesc = {};
  seedDesc.label = sv("lut_seed");
  seedDesc.compute.module = seedMod;
  seedDesc.compute.entryPoint = sv("main");
  WGPUComputePipeline seedPipe = wgpuDeviceCreateComputePipeline(gpu.device, &seedDesc);
  wgpuShaderModuleRelease(seedMod);
  if (!seedPipe) {
    std::fprintf(stderr, "[lutbake] lut_seed pipeline failed to build\n");
    release3DPingPong(pp);
    return {};
  }

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPUComputePassEncoder cpass = wgpuCommandEncoderBeginComputePass(enc, nullptr);

  // Everything built/allocated while encoding is released only after
  // wgpuQueueSubmit() below -- releasing a CPU-side handle right after the
  // command that references it is encoded (not after the GPU has actually
  // finished executing it) is the same discipline sim/PaintSim.cpp's run()
  // lambda and app/SelfTest.cpp's blitPipelineRenderAndReadback() already
  // rely on: wgpu-native ref-counts the underlying GPU objects for any
  // still-in-flight work, so this is safe without an explicit wait.
  std::vector<WGPUBuffer> scratchBuffers;
  std::vector<WGPUComputePipeline> scratchPipelines;
  scratchPipelines.push_back(seedPipe);
  std::unordered_map<int, WGPUComputePipeline> pipelineCache;
  bool ok = true;

  auto dispatchPass = [&](WGPUComputePipeline pipeline, std::vector<WGPUBindGroupEntry> entries) {
    WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    WGPUBindGroupDescriptor bgd = {};
    bgd.layout = layout;
    bgd.entryCount = entries.size();
    bgd.entries = entries.data();
    WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);
    wgpuComputePassEncoderSetPipeline(cpass, pipeline);
    wgpuComputePassEncoderSetBindGroup(cpass, 0, bg, 0, nullptr);
    // 32 / 4 == 8 exactly (kLutSize / this file's @workgroup_size(4,4,4)) --
    // every invocation lands in-bounds, no remainder to handle.
    wgpuComputePassEncoderDispatchWorkgroups(cpass, 8, 8, 8);
    wgpuBindGroupLayoutRelease(layout);
    wgpuBindGroupRelease(bg);
  };

  // Seed: writes pp.dst() directly, no source texture to read.
  dispatchPass(seedPipe, {texEntry(0, pp.dst())});
  pp.flip();

  auto getOpPipeline = [&](PointOpKind kind) -> WGPUComputePipeline {
    const int key = static_cast<int>(kind);
    auto it = pipelineCache.find(key);
    if (it != pipelineCache.end()) return it->second;
    const char* path = kOpKernelPath[key];
    WGPUShaderModule mod = compileShader(gpu.device, gpu.instance, path);
    if (!mod) {
      std::fprintf(stderr, "[lutbake] %s failed to compile\n", path);
      ok = false;
      return nullptr;
    }
    WGPUComputePipelineDescriptor d = {};
    d.label = sv(path);
    d.compute.module = mod;
    d.compute.entryPoint = sv("main");
    WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(gpu.device, &d);
    wgpuShaderModuleRelease(mod);
    if (!pipe) {
      std::fprintf(stderr, "[lutbake] %s pipeline failed to build\n", path);
      ok = false;
      return nullptr;
    }
    pipelineCache.emplace(key, pipe);
    scratchPipelines.push_back(pipe);
    return pipe;
  };

  for (const Op& op : ops) {
    // Matches core::OpRun::ops's own rule: a disabled entry, or (should it
    // ever occur here) a non-PointA entry, contributes no pass -- identity.
    if (!op.enabled || op.opClass != OpClass::PointA) continue;

    switch (op.pointKind) {
      case PointOpKind::Levels: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::Levels);
        if (!pipe) break;
        LevelsUniform u{};
        const std::array<LevelsParams, 3>& lv = op.levels;
        u.chR[0] = lv[0].blackIn; u.chR[1] = lv[0].whiteIn; u.chR[2] = lv[0].gamma; u.chR[3] = lv[0].blackOut;
        u.chG[0] = lv[1].blackIn; u.chG[1] = lv[1].whiteIn; u.chG[2] = lv[1].gamma; u.chG[3] = lv[1].blackOut;
        u.chB[0] = lv[2].blackIn; u.chB[1] = lv[2].whiteIn; u.chB[2] = lv[2].gamma; u.chB[3] = lv[2].blackOut;
        u.whiteOut[0] = lv[0].whiteOut; u.whiteOut[1] = lv[1].whiteOut; u.whiteOut[2] = lv[2].whiteOut;
        u.whiteOut[3] = 0.0f;
        WGPUBuffer buf = makeUniformBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
      case PointOpKind::Curves: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::Curves);
        if (!pipe) break;
        CurvesGpuBuffer u{};
        uint32_t* counts[3] = {&u.countR, &u.countG, &u.countB};
        for (int c = 0; c < 3; ++c) {
          const Curve& curve = op.curves[static_cast<size_t>(c)];
          uint32_t n = static_cast<uint32_t>(curve.size());
          if (n > static_cast<uint32_t>(kMaxCurvePointsPerChannel)) {
            std::fprintf(stderr,
                         "[lutbake] curves: channel %d has %u control points, truncating to "
                         "kMaxCurvePointsPerChannel=%d\n",
                         c, n, kMaxCurvePointsPerChannel);
            n = static_cast<uint32_t>(kMaxCurvePointsPerChannel);
          }
          *counts[c] = n;
          const uint32_t base = static_cast<uint32_t>(c) * static_cast<uint32_t>(kMaxCurvePointsPerChannel);
          for (uint32_t i = 0; i < n; ++i) {
            u.points[base + i][0] = curve[i].x;
            u.points[base + i][1] = curve[i].y;
          }
        }
        WGPUBuffer buf = makeStorageBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
      case PointOpKind::Exposure: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::Exposure);
        if (!pipe) break;
        ExposureUniform u{};
        u.stops = op.exposure.stops;
        WGPUBuffer buf = makeUniformBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
      case PointOpKind::Saturation: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::Saturation);
        if (!pipe) break;
        SaturationUniform u{};
        u.scale = op.saturation.scale;
        u.weights[0] = op.saturation.lumaWeights[0];
        u.weights[1] = op.saturation.lumaWeights[1];
        u.weights[2] = op.saturation.lumaWeights[2];
        WGPUBuffer buf = makeUniformBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
      case PointOpKind::Grayscale: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::Grayscale);
        if (!pipe) break;
        GrayscaleUniform u{};
        u.weights[0] = op.grayscale.lumaWeights[0];
        u.weights[1] = op.grayscale.lumaWeights[1];
        u.weights[2] = op.grayscale.lumaWeights[2];
        WGPUBuffer buf = makeUniformBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
      case PointOpKind::ChannelMixer: {
        WGPUComputePipeline pipe = getOpPipeline(PointOpKind::ChannelMixer);
        if (!pipe) break;
        ChannelMixerUniform u{};
        const auto& m = op.channelMixer.matrix;
        for (int i = 0; i < 4; ++i) {
          u.row0[i] = m[0][static_cast<size_t>(i)];
          u.row1[i] = m[1][static_cast<size_t>(i)];
          u.row2[i] = m[2][static_cast<size_t>(i)];
        }
        WGPUBuffer buf = makeUniformBuffer(gpu, &u, sizeof(u));
        scratchBuffers.push_back(buf);
        dispatchPass(pipe, {bufEntry(0, buf, sizeof(u)), texEntry(1, pp.src()), texEntry(2, pp.dst())});
        pp.flip();
        break;
      }
    }
  }

  wgpuComputePassEncoderEnd(cpass);
  wgpuComputePassEncoderRelease(cpass);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  for (WGPUBuffer b : scratchBuffers) {
    wgpuBufferDestroy(b);
    wgpuBufferRelease(b);
  }
  for (WGPUComputePipeline p : scratchPipelines) wgpuComputePipelineRelease(p);

  if (!ok) {
    release3DPingPong(pp);
    return {};
  }

  Lut3D result;
  result.texture = pp.srcTex();
  result.view = pp.src();
  result.size = kLutSize;

  // Detach the kept half from the ping-pong before releasing it, so
  // release3DPingPong() below only frees the unused scratch half.
  pp.tex[pp.cur] = nullptr;
  pp.view[pp.cur] = nullptr;
  release3DPingPong(pp);

  return result;
}

}  // namespace np
