#include "flats/MembraneGpu.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gfx/Context.hpp"
#include "gfx/ShaderLoader.hpp"

namespace np {

namespace {

constexpr uint32_t kWorkgroup = 8;
uint32_t groupsOf(uint32_t n) { return (n + kWorkgroup - 1) / kWorkgroup; }

// One uniform struct shape shared by every membrane_*.wgsl pass. Which
// fields a given shader actually reads varies (see each .wgsl's own `Params`
// struct); all of them are four 4-byte scalars so the same 16-byte buffer
// and the same write helper serve all of them. `d` is reinterpreted as f32
// only by membrane_correct_apply.wgsl, which is why it is written through
// the float overload below rather than typed here.
struct Uniform4 {
  uint32_t a = 0, b = 0, c = 0, d = 0;
};

WGPUBindGroupEntry bufEntry(uint32_t binding, WGPUBuffer buf, uint64_t size) {
  WGPUBindGroupEntry e = {};
  e.binding = binding;
  e.buffer = buf;
  e.offset = 0;
  e.size = size;
  return e;
}

WGPUBuffer makeBuffer(GpuContext& gpu, uint64_t size, WGPUBufferUsage usage, const char* label) {
  WGPUBufferDescriptor d = {};
  d.label = sv(label);
  d.size = size;
  d.usage = usage;
  return wgpuDeviceCreateBuffer(gpu.device, &d);
}

WGPUBuffer makeFloatBuffer(GpuContext& gpu, size_t n, const float* init, const char* label) {
  WGPUBuffer buf = makeBuffer(gpu, std::max<size_t>(n, 1) * sizeof(float),
                              static_cast<WGPUBufferUsage>(WGPUBufferUsage_Storage |
                                                            WGPUBufferUsage_CopyDst |
                                                            WGPUBufferUsage_CopySrc),
                              label);
  if (init) {
    wgpuQueueWriteBuffer(gpu.queue, buf, 0, init, n * sizeof(float));
  } else {
    std::vector<float> zeros(n, 0.f);
    wgpuQueueWriteBuffer(gpu.queue, buf, 0, zeros.data(), n * sizeof(float));
  }
  return buf;
}

void writeUniform(GpuContext& gpu, WGPUBuffer u, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
  Uniform4 v{a, b, c, d};
  wgpuQueueWriteBuffer(gpu.queue, u, 0, &v, sizeof(v));
}

void writeUniformAlpha(GpuContext& gpu, WGPUBuffer u, uint32_t a, uint32_t b, float alpha) {
  struct { uint32_t a, b, pad; float alpha; } v{a, b, 0, alpha};
  wgpuQueueWriteBuffer(gpu.queue, u, 0, &v, sizeof(v));
}

// One dispatch, one command buffer, one submit -- see MembraneGpu.hpp's
// header comment: correctness over throughput, since consecutive dispatches
// in this solver routinely need different uniform values (the smoother's
// red-black colour, the coarse level's dims, the line-search alpha), and
// wgpuQueueWriteBuffer's effect lands at the point it is CALLED, not where a
// not-yet-submitted command buffer that reads it happens to be recorded.
// Interleaving writes with dispatches batched into one submit would have
// every dispatch in that submit observe only the LAST write.
void dispatch(GpuContext& gpu, WGPUComputePipeline pipeline,
             const std::vector<WGPUBindGroupEntry>& entries, uint32_t gx, uint32_t gy) {
  WGPUBindGroupLayout layout = wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
  WGPUBindGroupDescriptor bgd = {};
  bgd.layout = layout;
  bgd.entryCount = entries.size();
  bgd.entries = entries.data();
  WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
  wgpuComputePassEncoderSetPipeline(pass, pipeline);
  wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
  wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, 1);
  wgpuComputePassEncoderEnd(pass);
  wgpuComputePassEncoderRelease(pass);

  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);
  wgpuBindGroupRelease(bg);
  wgpuBindGroupLayoutRelease(layout);
}

// Copies `count` floats out of `src` (already the queue's current view of
// it -- everything above is submitted before this is called) into a fresh
// host-visible staging buffer and blocks until the map completes. Only ever
// called on the small per-workgroup partials buffers, never on a full level.
std::vector<float> readback(GpuContext& gpu, WGPUBuffer src, size_t count) {
  const size_t bytes = count * sizeof(float);
  WGPUBuffer staging = makeBuffer(gpu, bytes,
                                  static_cast<WGPUBufferUsage>(WGPUBufferUsage_CopyDst |
                                                                WGPUBufferUsage_MapRead),
                                  "membrane readback");
  WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
  wgpuCommandEncoderCopyBufferToBuffer(enc, src, 0, staging, 0, bytes);
  WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
  wgpuQueueSubmit(gpu.queue, 1, &cmd);
  wgpuCommandBufferRelease(cmd);
  wgpuCommandEncoderRelease(enc);

  struct MapState { bool done = false, ok = false; } state;
  WGPUBufferMapCallbackInfo mci = {};
  mci.mode = WGPUCallbackMode_AllowProcessEvents;
  mci.userdata1 = &state;
  mci.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* ud1, void*) {
    auto* st = static_cast<MapState*>(ud1);
    st->ok = (status == WGPUMapAsyncStatus_Success);
    st->done = true;
  };
  wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, mci);
  while (!state.done) wgpuInstanceProcessEvents(gpu.instance);

  std::vector<float> out;
  if (state.ok) {
    const auto* raw = static_cast<const float*>(wgpuBufferGetConstMappedRange(staging, 0, bytes));
    if (raw) out.assign(raw, raw + count);
    wgpuBufferUnmap(staging);
  }
  wgpuBufferRelease(staging);
  return out;
}

struct Pipelines {
  WGPUComputePipeline smooth = nullptr;
  WGPUComputePipeline restrictResidual = nullptr;
  WGPUComputePipeline prolong = nullptr;
  WGPUComputePipeline correctReduce = nullptr;
  WGPUComputePipeline correctApply = nullptr;
  WGPUComputePipeline residualReduce = nullptr;

  bool ok() const {
    return smooth && restrictResidual && prolong && correctReduce && correctApply && residualReduce;
  }
  void release() {
    for (WGPUComputePipeline* p : {&smooth, &restrictResidual, &prolong, &correctReduce,
                                   &correctApply, &residualReduce}) {
      if (*p) wgpuComputePipelineRelease(*p);
      *p = nullptr;
    }
  }
};

WGPUComputePipeline buildPipeline(GpuContext& gpu, const char* path) {
  WGPUShaderModule mod = compileShader(gpu.device, gpu.instance, path);
  if (!mod) return nullptr;
  WGPUComputePipelineDescriptor d = {};
  d.label = sv(path);
  d.compute.module = mod;
  d.compute.entryPoint = sv("main");
  WGPUComputePipeline pipe = wgpuDeviceCreateComputePipeline(gpu.device, &d);
  wgpuShaderModuleRelease(mod);
  return pipe;
}

bool buildPipelines(GpuContext& gpu, Pipelines& p) {
  p.smooth = buildPipeline(gpu, "membrane_smooth.wgsl");
  p.restrictResidual = buildPipeline(gpu, "membrane_restrict.wgsl");
  p.prolong = buildPipeline(gpu, "membrane_prolong.wgsl");
  p.correctReduce = buildPipeline(gpu, "membrane_correct_reduce.wgsl");
  p.correctApply = buildPipeline(gpu, "membrane_correct_apply.wgsl");
  p.residualReduce = buildPipeline(gpu, "membrane_residual_reduce.wgsl");
  return p.ok();
}

// One grid level's GPU state. Mirrors flats/Membrane.cpp's (anonymous
// namespace) `Level`, plus the small per-workgroup reduction scratch buffer
// and its own uniform buffer (each level dispatches with different
// width/height, so buffers are not shared across levels).
struct GpuLevel {
  uint32_t w = 0, h = 0;
  uint32_t gx = 0, gy = 0;  // workgroup counts at (8,8)
  WGPUBuffer u = nullptr, b = nullptr, e = nullptr, freeBuf = nullptr;
  WGPUBuffer uniformBuf = nullptr;
  WGPUBuffer partials = nullptr;  // vec2<f32> for correct, f32 for residual -- sized for the larger
  size_t numWorkgroups = 0;

  void release() {
    for (WGPUBuffer* buf : {&u, &b, &e, &freeBuf, &uniformBuf, &partials}) {
      if (*buf) wgpuBufferRelease(*buf);
      *buf = nullptr;
    }
  }
};

GpuLevel makeLevel(GpuContext& gpu, uint32_t w, uint32_t h, const std::vector<float>& freeF) {
  GpuLevel L;
  L.w = w;
  L.h = h;
  L.gx = groupsOf(w);
  L.gy = groupsOf(h);
  L.numWorkgroups = static_cast<size_t>(L.gx) * L.gy;
  const size_t n = static_cast<size_t>(w) * h;
  L.u = makeFloatBuffer(gpu, n, nullptr, "membrane u");
  L.b = makeFloatBuffer(gpu, n, nullptr, "membrane b");
  L.e = makeFloatBuffer(gpu, n, nullptr, "membrane e");
  L.freeBuf = makeFloatBuffer(gpu, n, freeF.data(), "membrane free");
  L.uniformBuf = makeBuffer(gpu, sizeof(Uniform4),
                            static_cast<WGPUBufferUsage>(WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst),
                            "membrane params");
  // vec2<f32> partial per workgroup is the larger of the two reduction
  // shapes (residual only needs f32); size for that so one buffer serves
  // both membrane_correct_reduce.wgsl and membrane_residual_reduce.wgsl.
  L.partials = makeBuffer(gpu, std::max<size_t>(L.numWorkgroups, 1) * 2 * sizeof(float),
                          static_cast<WGPUBufferUsage>(WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst |
                                                        WGPUBufferUsage_CopySrc),
                          "membrane partials");
  return L;
}

// A coarse cell is free only if ALL of its children are -- identical rule
// and identical reasoning to flats/Membrane.cpp's `coarsenMask()`. Done on
// the CPU: it runs once per level per solve (not once per iteration), so it
// is nowhere near the restriction/prolongation cost the GPU design doc
// singles out.
std::vector<uint8_t> coarsenMaskCpu(const std::vector<uint8_t>& free, int w, int h, int& w2, int& h2) {
  w2 = (w + 1) >> 1;
  h2 = (h + 1) >> 1;
  std::vector<uint8_t> f2(static_cast<size_t>(w2) * h2, 0);
  for (int cy = 0; cy < h2; cy++) {
    for (int cx = 0; cx < w2; cx++) {
      const int x = cx << 1, y = cy << 1;
      uint8_t all = 1;
      for (int dy = 0; dy < 2 && all; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          const int px = x + dx, py = y + dy;
          if (px >= w || py >= h) continue;
          if (!free[static_cast<size_t>(py) * w + px]) { all = 0; break; }
        }
      }
      f2[static_cast<size_t>(cy) * w2 + cx] = all;
    }
  }
  return f2;
}

constexpr int NU = 6;  // matches flats/Membrane.cpp's smoothing sweep count

void gpuSmooth(GpuContext& gpu, Pipelines& p, GpuLevel& L, int sweeps) {
  for (int s = 0; s < sweeps; s++) {
    for (uint32_t color = 0; color < 2; color++) {
      writeUniform(gpu, L.uniformBuf, L.w, L.h, color, 0);
      const std::vector<WGPUBindGroupEntry> entries = {
          bufEntry(0, L.uniformBuf, sizeof(Uniform4)),
          bufEntry(1, L.u, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
          bufEntry(2, L.b, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
          bufEntry(3, L.freeBuf, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      };
      dispatch(gpu, p.smooth, entries, L.gx, L.gy);
    }
  }
}

void gpuRestrict(GpuContext& gpu, Pipelines& p, GpuLevel& fine, GpuLevel& coarse) {
  writeUniform(gpu, coarse.uniformBuf, fine.w, fine.h, coarse.w, coarse.h);
  const std::vector<WGPUBindGroupEntry> entries = {
      bufEntry(0, coarse.uniformBuf, sizeof(Uniform4)),
      bufEntry(1, fine.u, static_cast<uint64_t>(fine.w) * fine.h * sizeof(float)),
      bufEntry(2, fine.b, static_cast<uint64_t>(fine.w) * fine.h * sizeof(float)),
      bufEntry(3, fine.freeBuf, static_cast<uint64_t>(fine.w) * fine.h * sizeof(float)),
      bufEntry(4, coarse.u, static_cast<uint64_t>(coarse.w) * coarse.h * sizeof(float)),
      bufEntry(5, coarse.b, static_cast<uint64_t>(coarse.w) * coarse.h * sizeof(float)),
      bufEntry(6, coarse.freeBuf, static_cast<uint64_t>(coarse.w) * coarse.h * sizeof(float)),
  };
  dispatch(gpu, p.restrictResidual, entries, coarse.gx, coarse.gy);
}

void gpuProlong(GpuContext& gpu, Pipelines& p, GpuLevel& coarse, GpuLevel& fine) {
  writeUniform(gpu, fine.uniformBuf, fine.w, fine.h, coarse.w, coarse.h);
  const std::vector<WGPUBindGroupEntry> entries = {
      bufEntry(0, fine.uniformBuf, sizeof(Uniform4)),
      bufEntry(1, coarse.u, static_cast<uint64_t>(coarse.w) * coarse.h * sizeof(float)),
      bufEntry(2, coarse.freeBuf, static_cast<uint64_t>(coarse.w) * coarse.h * sizeof(float)),
      bufEntry(3, fine.freeBuf, static_cast<uint64_t>(fine.w) * fine.h * sizeof(float)),
      bufEntry(4, fine.e, static_cast<uint64_t>(fine.w) * fine.h * sizeof(float)),
  };
  dispatch(gpu, p.prolong, entries, fine.gx, fine.gy);
}

float gpuCorrect(GpuContext& gpu, Pipelines& p, GpuLevel& L) {
  writeUniform(gpu, L.uniformBuf, L.w, L.h, 0, 0);
  const std::vector<WGPUBindGroupEntry> reduceEntries = {
      bufEntry(0, L.uniformBuf, sizeof(Uniform4)),
      bufEntry(1, L.u, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(2, L.b, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(3, L.e, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(4, L.freeBuf, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(5, L.partials, static_cast<uint64_t>(L.numWorkgroups) * 2 * sizeof(float)),
  };
  dispatch(gpu, p.correctReduce, reduceEntries, L.gx, L.gy);

  const std::vector<float> parts = readback(gpu, L.partials, L.numWorkgroups * 2);
  double num = 0, den = 0;
  for (size_t i = 0; i < L.numWorkgroups; i++) {
    num += static_cast<double>(parts[i * 2 + 0]);
    den += static_cast<double>(parts[i * 2 + 1]);
  }
  if (!(den > 0)) return 0.f;
  const float alpha = static_cast<float>(num / den);

  writeUniformAlpha(gpu, L.uniformBuf, L.w, L.h, alpha);
  const std::vector<WGPUBindGroupEntry> applyEntries = {
      bufEntry(0, L.uniformBuf, sizeof(Uniform4)),
      bufEntry(1, L.u, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(2, L.e, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(3, L.freeBuf, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
  };
  dispatch(gpu, p.correctApply, applyEntries, L.gx, L.gy);
  return alpha;
}

double gpuResidual(GpuContext& gpu, Pipelines& p, GpuLevel& L) {
  writeUniform(gpu, L.uniformBuf, L.w, L.h, 0, 0);
  const std::vector<WGPUBindGroupEntry> entries = {
      bufEntry(0, L.uniformBuf, sizeof(Uniform4)),
      bufEntry(1, L.u, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(2, L.b, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(3, L.freeBuf, static_cast<uint64_t>(L.w) * L.h * sizeof(float)),
      bufEntry(4, L.partials, static_cast<uint64_t>(L.numWorkgroups) * sizeof(float)),
  };
  dispatch(gpu, p.residualReduce, entries, L.gx, L.gy);

  const std::vector<float> parts = readback(gpu, L.partials, L.numWorkgroups);
  double s = 0;
  for (float v : parts) s += static_cast<double>(v);
  return std::sqrt(s);
}

void vcycle(GpuContext& gpu, Pipelines& p, std::vector<GpuLevel>& ls, size_t k) {
  GpuLevel& L = ls[k];
  if (k == ls.size() - 1) { gpuSmooth(gpu, p, L, 40); return; }
  gpuSmooth(gpu, p, L, NU);
  gpuRestrict(gpu, p, L, ls[k + 1]);
  vcycle(gpu, p, ls, k + 1);
  gpuProlong(gpu, p, ls[k + 1], L);
  gpuCorrect(gpu, p, L);
  gpuSmooth(gpu, p, L, NU);
}

}  // namespace

std::vector<float> membraneSagGpu(GpuContext& gpu, const FlatMask& line, int w, int h, int cycles,
                                  float tol) {
  const size_t n = static_cast<size_t>(w) * h;
  std::vector<float> sag(n, 0.f);

  Pipelines pipelines;
  if (!buildPipelines(gpu, pipelines)) {
    pipelines.release();
    return sag;
  }

  std::vector<uint8_t> free(n);
  for (size_t i = 0; i < n; i++) free[i] = line[i] ? 0 : 1;

  // Same level hierarchy flats/Membrane.cpp builds: coarsen until <=8x8.
  struct MaskLevel { int w, h; std::vector<uint8_t> free; };
  std::vector<MaskLevel> maskLevels;
  maskLevels.push_back({w, h, free});
  while (maskLevels.back().w > 8 && maskLevels.back().h > 8) {
    const MaskLevel& prev = maskLevels.back();
    int w2 = 0, h2 = 0;
    std::vector<uint8_t> f2 = coarsenMaskCpu(prev.free, prev.w, prev.h, w2, h2);
    maskLevels.push_back({w2, h2, std::move(f2)});
  }

  std::vector<GpuLevel> ls;
  ls.reserve(maskLevels.size());
  for (const MaskLevel& ml : maskLevels) {
    std::vector<float> freeF(ml.free.size());
    for (size_t i = 0; i < ml.free.size(); i++) freeF[i] = ml.free[i] ? 1.f : 0.f;
    ls.push_back(makeLevel(gpu, static_cast<uint32_t>(ml.w), static_cast<uint32_t>(ml.h), freeF));
  }

  // Unit gravity RHS on the top level, h = 1 -- identical to
  // flatMembraneSag()'s own top.b initialisation.
  {
    std::vector<float> topB(n, 0.f);
    for (size_t i = 0; i < n; i++)
      if (free[i]) topB[i] = 1.f;
    wgpuQueueWriteBuffer(gpu.queue, ls[0].b, 0, topB.data(), n * sizeof(float));
  }

  const double r0 = gpuResidual(gpu, pipelines, ls[0]);
  for (int c = 0; c < cycles; c++) {
    if (gpuResidual(gpu, pipelines, ls[0]) < tol * r0) break;
    vcycle(gpu, pipelines, ls, 0);
  }

  const std::vector<float> topU = readback(gpu, ls[0].u, n);
  if (topU.size() == n) {
    for (size_t i = 0; i < n; i++)
      if (free[i] && topU[i] > 0) sag[i] = std::sqrt(8.f * topU[i]);
  }

  for (GpuLevel& L : ls) L.release();
  pipelines.release();
  return sag;
}

}  // namespace np
