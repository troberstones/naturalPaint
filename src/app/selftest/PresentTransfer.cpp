#include "app/selftest/Support.hpp"

#include "app/Screenshot.hpp"

// Declarations only. paint/Palette.cpp is the one translation unit that defines
// STB_IMAGE_IMPLEMENTATION for the whole binary -- the same arrangement
// io/ImageDecode.cpp already has, and the same one Support.hpp describes for
// stb_image_write.h. `stbi_load()` is used here rather than io/ImageDecode's
// `decodeImageLinear()` on purpose: that function sRGB-*decodes* what it reads,
// and this section needs the PNG's literal bytes, not an interpretation of them.
#include "stb_image.h"

// The presentation transfer function: what happens to a colour between the
// value the application decides on and the byte a screenshot of the window
// contains.
//
// This section exists because that question had no answer. `--selftest` proved
// ui/AtelierTheme's twelve tokens matched docs/ui.md, and the export path
// proved a linear value round-trips through a PNG file, but nothing anywhere
// connected a value *in the document* to a pixel *on the screen*. The gap is
// not academic: the chrome measurably lands darker than its token, and the
// canvas draws its linear-light document through the very same pipeline into
// the very same swapchain, so an error there is an error in every colour
// judgement a painter makes in this application.
//
// --- The path, as measured -------------------------------------------------
//
// 1. gfx/Context takes `caps.formats[0]` verbatim. On this adapter that is
//    `BGRA8UnormSrgb` (24) -- an **sRGB** format, which is now printed at
//    startup rather than inferred.
// 2. Dear ImGui's WGPU backend switches on exactly that format. For every
//    `...UnormSrgb` case it writes `gamma = 2.2f` into its uniform buffer;
//    otherwise `gamma = 1.0f`. Its fragment shader is then:
//
//        let color = in.color * textureSample(t, s, in.uv);
//        let corrected_color = pow(color.rgb, vec3<f32>(uniforms.gamma));
//
// 3. The attachment is sRGB, so the hardware applies the **exact piecewise
//    IEC 61966-2-1 encode** to that result on write.
// 4. app/Screenshot copies the swapchain texture to a buffer and reorders
//    channels. It converts nothing -- the PNG holds the swapchain's bytes.
//
// None of those four steps has an `NP_USE_OIIO` branch: app/Screenshot writes
// through stb_image_write unconditionally, and ui/DocumentTexture does not
// mention OIIO at all. So this section measures the same path in both
// configurations, which is why it makes no claim about the build option --
// running identically in both builds *is* the claim, and the suite is built
// twice.
//
// So every pixel presented by this application is
// `srgbEncode(pow(value, gamma))`, with `gamma` chosen by the surface format.
//
// --- What that does to the chrome ------------------------------------------
//
// ImGui's vertex colours are sRGB-encoded bytes. `pow(c, 2.2)` is the
// *approximate* sRGB decode; the hardware then re-encodes with the *exact*
// piecewise curve. The two curves are not inverses, so the round trip is not
// the identity. Both are pinned at 1.0, which is why bright tokens survive
// exactly and dark ones are pulled down -- §2 measures the whole curve.
//
// --- What that does to the document ----------------------------------------
//
// This is the part that matters. ui/DocumentTexture uploads **linear-light**
// values (§1 of its header: RGBA16Float, never 8-bit, and nothing in that path
// encodes). ui/MacPaintUI draws it with `AddImageQuad` and no tint, so ImGui's
// `in.color` is white and `color.rgb` *is* the linear document value. The
// correct presentation of a linear value into an sRGB attachment is to pass it
// through untouched and let the hardware encode it -- that is what `gamma =
// 1.0` would do. Instead it is raised to the 2.2 first.
//
// §3 authors known linear values into a layer, runs them through the real
// upload, the real surface format, the real hardware encode and the real
// screenshot writer, decodes the PNG's raw bytes, and compares against
// `srgbEncode()`. It renders the same fixture twice -- once at the gamma the
// backend actually selects and once at 1.0 -- and prints both columns, which
// is what isolates the defect to that single uniform.
//
// --- Why this section asserts today's numbers ------------------------------
//
// The suite must exit 0 FAIL, and fixing the transfer function moves every
// pixel of every screenshot in the project. So the checks below assert the
// behaviour that is *actually* present, each one labelled with whether it is
// the correct answer or the defect, and the correct answer is asserted too --
// against the `gamma = 1.0` render, which proves that the upload, the format,
// the hardware encode, the screenshot writer and the PNG are all sound and
// that the entire error is that one value. When the fix lands, §3's two
// columns swap and the "defect" lines are the ones that change.

namespace np {
namespace {

// Dear ImGui's own switch, transcribed. The backend is a fetched dependency
// and does not export this, so it is replicated here -- and §1 asserts the
// replication against the format the adapter actually handed us, so a change
// upstream shows up as a failure rather than as a silently stale copy.
float imguiGammaForFormat(WGPUTextureFormat f) {
  switch (f) {
    case WGPUTextureFormat_BGRA8UnormSrgb:
    case WGPUTextureFormat_RGBA8UnormSrgb:
    case WGPUTextureFormat_BC1RGBAUnormSrgb:
    case WGPUTextureFormat_BC2RGBAUnormSrgb:
    case WGPUTextureFormat_BC3RGBAUnormSrgb:
    case WGPUTextureFormat_BC7RGBAUnormSrgb:
    case WGPUTextureFormat_ETC2RGB8UnormSrgb:
    case WGPUTextureFormat_ETC2RGB8A1UnormSrgb:
    case WGPUTextureFormat_ETC2RGBA8UnormSrgb: return 2.2f;
    default: return 1.0f;
  }
}

const char* formatName(WGPUTextureFormat f) {
  switch (f) {
    case WGPUTextureFormat_BGRA8UnormSrgb: return "BGRA8UnormSrgb";
    case WGPUTextureFormat_RGBA8UnormSrgb: return "RGBA8UnormSrgb";
    case WGPUTextureFormat_BGRA8Unorm: return "BGRA8Unorm";
    case WGPUTextureFormat_RGBA8Unorm: return "RGBA8Unorm";
    default: return "other";
  }
}

// The presented byte for a value already in the shader, modelled on the CPU:
// `pow` then the exact sRGB encode the hardware performs, then unorm rounding.
int presentedByte(float value, float gamma) {
  const float afterPow = std::pow(std::max(value, 0.0f), gamma);
  const float encoded = srgbEncode(afterPow);
  const float clamped = std::min(std::max(encoded, 0.0f), 1.0f);
  return static_cast<int>(std::lround(clamped * 255.0f));
}

// ImGui's fragment shader, reduced to the case the canvas quad actually hits:
// an untinted `AddImageQuad`, so `in.color` is white and the sampled texel is
// the whole of `color.rgb`. `textureLoad` rather than `textureSample` because
// the target is the same size as the source and this test is about the
// transfer function, not about filtering -- a sampler would fold bilinear
// weights into the numbers and blur the very thing being measured.
constexpr const char* kPresentShaderSrc = R"(
struct Params { gamma : f32, _pad0 : f32, _pad1 : f32, _pad2 : f32, };
@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var docTex : texture_2d<f32>;

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
  var out : VSOut;
  out.pos = vec4<f32>(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  out.uv = uv;
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(docTex));
  let clamped = clamp(in.uv, vec2<f32>(0.0), vec2<f32>(0.999999));
  let color = textureLoad(docTex, vec2<i32>(clamped * dims), 0);
  let corrected = pow(color.rgb, vec3<f32>(P.gamma));
  return vec4<f32>(corrected, color.a);
}
)";

struct PresentParams {
  float gamma = 1.0f;
  float pad0 = 0, pad1 = 0, pad2 = 0;
};
static_assert(sizeof(PresentParams) == 16, "must match kPresentShaderSrc's Params layout");

}  // namespace

bool runPresentTransferTest(GpuContext& gpu) {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Tolerance -----------------------------------------------------------
  //
  // ONE code value, and it is derived rather than chosen. Three roundings sit
  // between the CPU model and the decoded byte: the f16 the texture stores
  // (exact for every fixture below -- all dyadic), `pow()` on the GPU against
  // libm's (WGSL specifies `pow` only to within a few ULP), and the
  // hardware's linear-to-sRGB encode, which WebGPU specifies to a tolerance
  // rather than bit-exactly. Each can move the result across a rounding
  // boundary at most once, and only when the exact value sits within an ULP
  // of `x.5`.
  //
  // It is deliberately *not* wide enough to hide anything. The defect §3
  // measures is 60 to 76 code values; one code value cannot absorb it, and
  // widening this number until the defect fitted inside it would be the exact
  // failure this section was written to prevent.
  constexpr int kByteTol = 1;

  // ======================================================================
  // 1. What the surface actually is, and what the backend does with it.
  // ======================================================================
  const WGPUTextureFormat fmt = gpu.surfaceFormat;
  const float gammaSelected = imguiGammaForFormat(fmt);
  const bool srgbSurface = (gammaSelected == 2.2f);
  std::printf("  surface format %d (%s); ImGui selects gamma %.1f\n", static_cast<int>(fmt),
              formatName(fmt), static_cast<double>(gammaSelected));

  // The screenshot writer refuses any format it cannot reorder by name. If
  // that refusal ever fires, every screenshot-based check in this project is
  // measuring nothing, so it is asserted here rather than discovered later.
  const bool writable =
      fmt == WGPUTextureFormat_BGRA8UnormSrgb || fmt == WGPUTextureFormat_RGBA8UnormSrgb ||
      fmt == WGPUTextureFormat_BGRA8Unorm || fmt == WGPUTextureFormat_RGBA8Unorm;
  check(writable, "present: the surface format is one app/Screenshot can write");

  // Both halves of the branch state their consequence, so this section says
  // something true on an adapter that prefers a non-sRGB surface too.
  if (srgbSurface)
    check(gammaSelected == 2.2f,
          "present: sRGB surface, so the backend applies pow(rgb, 2.2)");
  else
    check(gammaSelected == 1.0f,
          "present: non-sRGB surface, so the backend applies pow(rgb, 1.0)");

  // ======================================================================
  // 2. The chrome round trip, as arithmetic.
  // ======================================================================
  //
  // An sRGB-encoded byte goes in and `srgbEncode(pow(c, gamma))` comes out.
  // On a non-sRGB surface gamma is 1.0, no encode happens, and the round trip
  // is the identity; on an sRGB surface the two mismatched curves leave a
  // residue. Nothing here reads ui/AtelierTheme -- this measures the *curve*,
  // which is what the tokens are victims of, and stays correct whatever the
  // tokens are changed to.
  {
    int worstCode = 0, worstDelta = 0;
    for (int c = 0; c <= 255; ++c) {
      const int out = srgbSurface ? presentedByte(static_cast<float>(c) / 255.0f, 2.2f) : c;
      const int delta = out - c;
      if (std::abs(delta) > std::abs(worstDelta)) {
        worstDelta = delta;
        worstCode = c;
      }
    }
    std::printf("  chrome round trip: worst code %d -> %d (delta %+d)\n", worstCode,
                worstCode + worstDelta, worstDelta);

    check(presentedByte(1.0f, gammaSelected) == 255,
          "present: white survives the round trip exactly -- both curves pin at 1");
    check(presentedByte(0.0f, gammaSelected) == 0,
          "present: black survives the round trip exactly -- both curves pin at 0");

    if (srgbSurface) {
      // The measured shape: bright codes exact, dark codes pulled down, worst
      // in the deep shadows. These are the numbers a screenshot of the chrome
      // contains today, and they are the DEFECT, not the intent.
      check(presentedByte(243.0f / 255.0f, 2.2f) == 243,
            "present: code 243 survives exactly (bright end is fixed)");
      check(presentedByte(45.0f / 255.0f, 2.2f) == 41,
            "present: code 45 lands at 41 -- 4 low (DEFECT, matches screenshot)");
      check(presentedByte(32.0f / 255.0f, 2.2f) == 26,
            "present: code 32 lands at 26 -- 6 low (DEFECT, matches screenshot)");
      check(worstDelta < 0 && worstCode < 64,
            "present: the error is a darkening, and it is worst in the shadows");
      check(std::abs(worstDelta) <= 9,
            "present: chrome error is bounded at 9 codes -- cosmetic, not structural");
    } else {
      check(worstDelta == 0, "present: non-sRGB surface, so the chrome round trip is exact");
    }
  }

  // ======================================================================
  // 3. The document, end to end: a linear value in a layer -> a PNG byte.
  // ======================================================================
  //
  // This is the assertion the project did not have. Everything below is the
  // real code path except the one fragment shader line, which is transcribed
  // from the backend and pinned by §1.
  {
    // Dyadic on purpose: every value is exact in f32 and in the f16 the
    // texture stores, so the fixture contributes no error of its own and any
    // difference measured is the transfer function's.
    constexpr int kProbes = 8;
    const float linear[kProbes] = {0.0f,   0.125f, 0.25f, 0.375f,
                                   0.5f,   0.625f, 0.75f, 1.0f};

    // 13 is deliberately not a multiple of 64: the copy stride is 13*4 = 52
    // bytes, which app/Screenshot must pad to 256 and then un-pad on the way
    // out. A width that was already aligned would not exercise that.
    constexpr int32_t kW = 13, kH = 3;
    check((static_cast<uint32_t>(kW) * 4u) % 256u != 0u,
          "present: the fixture's copy stride is deliberately not 256-aligned");

    OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, "present");
    for (int i = 0; i < kProbes; ++i) {
      for (int32_t y = 0; y < kH; ++y) {
        const PixelCoord at{i, y};
        // Opaque, so premultiplied == straight and the compositor's alpha
        // arithmetic contributes nothing. ImGui's blend is likewise a no-op
        // at alpha 1, which is why this section can leave blending out and
        // still be measuring the path the canvas takes.
        od.document.layers[0].rgbTiles->getOrCreate(tileCoordAt(at)).writePixel(
            tileLocalOffset(at),
            std::array<float, 4>{linear[i], linear[i], linear[i], 1.0f});
      }
    }
    od.recordEdit("present", EditKind::Content);

    // The real upload: ui/DocumentTexture, RGBA16Float, straight alpha.
    DocumentTexture dt;
    const WGPUTextureView docView = dt.viewFor(gpu, od);
    check(docView != nullptr, "present: the document uploaded to an RGBA16Float texture");

    WGPUShaderModule shaderMod = nullptr;
    if (docView != nullptr) {
      WGPUShaderSourceWGSL wgsl = {};
      wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
      wgsl.code = sv(kPresentShaderSrc);
      WGPUShaderModuleDescriptor smd = {};
      smd.nextInChain = &wgsl.chain;
      smd.label = sv("present-transfer-selftest");
      shaderMod = wgpuDeviceCreateShaderModule(gpu.device, &smd);
    }
    check(shaderMod != nullptr, "present: the backend's fragment shader compiles");

    WGPURenderPipeline pipeline = nullptr;
    if (shaderMod != nullptr) {
      // The real surface format, so the *hardware* performs the sRGB encode.
      // Nothing in this file encodes; that is the whole point.
      WGPUColorTargetState target = {};
      target.format = fmt;
      target.writeMask = WGPUColorWriteMask_All;

      WGPUFragmentState fs = {};
      fs.module = shaderMod;
      fs.entryPoint = sv("fs");
      fs.targetCount = 1;
      fs.targets = &target;

      WGPURenderPipelineDescriptor rd = {};
      rd.label = sv("present-transfer-selftest");
      rd.vertex.module = shaderMod;
      rd.vertex.entryPoint = sv("vs");
      rd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
      rd.primitive.frontFace = WGPUFrontFace_CCW;
      rd.primitive.cullMode = WGPUCullMode_None;
      rd.multisample.count = 1;
      rd.multisample.mask = 0xFFFFFFFF;
      rd.fragment = &fs;
      pipeline = wgpuDeviceCreateRenderPipeline(gpu.device, &rd);
      wgpuShaderModuleRelease(shaderMod);
    }
    check(pipeline != nullptr, "present: the presentation pipeline builds at the surface format");

    // Render the fixture at one gamma, photograph it with the production
    // screenshot writer, and hand back the PNG's raw bytes. `stbi_load` is
    // used rather than io/ImageDecode's `decodeImageLinear` precisely because
    // the latter sRGB-decodes: this needs the file's actual bytes, not an
    // interpretation of them.
    auto renderAndDecode = [&](float gamma, const char* pngPath,
                               std::vector<uint8_t>& outPx) -> bool {
      WGPUTextureDescriptor rtd = {};
      rtd.label = sv("present-transfer-target");
      rtd.dimension = WGPUTextureDimension_2D;
      rtd.size = {static_cast<uint32_t>(kW), static_cast<uint32_t>(kH), 1};
      rtd.format = fmt;
      rtd.mipLevelCount = 1;
      rtd.sampleCount = 1;
      rtd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
      WGPUTexture targetTex = wgpuDeviceCreateTexture(gpu.device, &rtd);
      if (targetTex == nullptr) return false;
      WGPUTextureView targetView = wgpuTextureCreateView(targetTex, nullptr);

      PresentParams pp;
      pp.gamma = gamma;
      WGPUBufferDescriptor ubd = {};
      ubd.size = sizeof(pp);
      ubd.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
      WGPUBuffer ubuf = wgpuDeviceCreateBuffer(gpu.device, &ubd);
      wgpuQueueWriteBuffer(gpu.queue, ubuf, 0, &pp, sizeof(pp));

      WGPUBindGroupEntry entries[2] = {};
      entries[0].binding = 0;
      entries[0].buffer = ubuf;
      entries[0].offset = 0;
      entries[0].size = sizeof(pp);
      entries[1].binding = 1;
      entries[1].textureView = docView;

      WGPUBindGroupLayout bgl = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
      WGPUBindGroupDescriptor bgd = {};
      bgd.layout = bgl;
      bgd.entryCount = 2;
      bgd.entries = entries;
      WGPUBindGroup bg = wgpuDeviceCreateBindGroup(gpu.device, &bgd);

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
      WGPURenderPassEncoder rp = wgpuCommandEncoderBeginRenderPass(enc, &rpd);
      wgpuRenderPassEncoderSetPipeline(rp, pipeline);
      wgpuRenderPassEncoderSetBindGroup(rp, 0, bg, 0, nullptr);
      wgpuRenderPassEncoderDraw(rp, 6, 1, 0, 0);
      wgpuRenderPassEncoderEnd(rp);
      wgpuRenderPassEncoderRelease(rp);
      WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
      wgpuQueueSubmit(gpu.queue, 1, &cmd);
      wgpuCommandBufferRelease(cmd);
      wgpuCommandEncoderRelease(enc);

      // The production writer, unmodified: it keys its channel order off
      // `gpu.surfaceFormat`, which is exactly this target's format.
      std::string err;
      const bool wrote = captureSurfaceToPng(gpu, targetTex, static_cast<uint32_t>(kW),
                                             static_cast<uint32_t>(kH), pngPath, &err);

      wgpuBindGroupRelease(bg);
      wgpuBindGroupLayoutRelease(bgl);
      wgpuBufferDestroy(ubuf);
      wgpuBufferRelease(ubuf);
      wgpuTextureViewRelease(targetView);
      wgpuTextureDestroy(targetTex);
      wgpuTextureRelease(targetTex);
      if (!wrote) return false;

      int w = 0, h = 0, comp = 0;
      unsigned char* px = stbi_load(pngPath, &w, &h, &comp, 4);
      std::error_code ec;
      std::filesystem::remove(pngPath, ec);
      if (px == nullptr) return false;
      const bool sized = (w == kW && h == kH);
      if (sized) outPx.assign(px, px + static_cast<size_t>(w) * h * 4);
      stbi_image_free(px);
      return sized;
    };

    std::vector<uint8_t> asBuilt, atUnitGamma;
    bool builtOk = false, unitOk = false;
    if (pipeline != nullptr) {
      builtOk = renderAndDecode(gammaSelected, "selftest_present_built.png", asBuilt);
      unitOk = renderAndDecode(1.0f, "selftest_present_unit.png", atUnitGamma);
    }
    check(builtOk, "present: rendered, screenshotted and decoded at the selected gamma");
    check(unitOk, "present: and again at gamma 1.0 -- the rejected alternative, run beside it");

    if (builtOk && unitOk) {
      // Row 1 of 3, so this also proves the row stride survived the padded
      // copy: a bug there would put row 1 at the wrong offset.
      auto byteAt = [&](const std::vector<uint8_t>& px, int x) {
        return static_cast<int>(px[(static_cast<size_t>(1) * kW + x) * 4]);
      };

      std::printf("  %-8s %-9s %-9s %-9s %s\n", "linear", "correct", "as-built", "gamma1.0",
                  "error");
      int worstBuilt = 0, worstUnit = 0;
      for (int i = 0; i < kProbes; ++i) {
        const int correct = static_cast<int>(std::lround(srgbEncode(linear[i]) * 255.0f));
        const int built = byteAt(asBuilt, i);
        const int unit = byteAt(atUnitGamma, i);
        worstBuilt = std::max(worstBuilt, std::abs(built - correct));
        worstUnit = std::max(worstUnit, std::abs(unit - correct));
        std::printf("  %-8.3f %-9d %-9d %-9d %+d\n", static_cast<double>(linear[i]), correct,
                    built, unit, built - correct);
      }

      // The correct answer, asserted. Every element of the path -- the layer
      // write, the composite, the f16 upload, the sRGB attachment, the
      // hardware encode, the screenshot copy, the PNG -- is proven sound at
      // gamma 1.0, which is what localises the defect to the gamma alone.
      check(worstUnit <= kByteTol,
            "present: at gamma 1.0 every probe reaches its correct sRGB byte");

      // Grey stays grey: a channel-order bug in the screenshot writer would
      // not show up in a grey ramp, so R, G and B are compared to each other
      // as well as to the expected value.
      bool neutral = true;
      for (int i = 0; i < kProbes; ++i) {
        const size_t o = (static_cast<size_t>(1) * kW + i) * 4;
        neutral = neutral && atUnitGamma[o] == atUnitGamma[o + 1] &&
                  atUnitGamma[o + 1] == atUnitGamma[o + 2];
      }
      check(neutral, "present: a neutral ramp stays neutral through the whole path");

      // Alpha is forced opaque by the writer, per its own comment.
      check(atUnitGamma[(static_cast<size_t>(1) * kW) * 4 + 3] == 255,
            "present: the screenshot writer forces alpha opaque");

      // The endpoints are correct even as built, because both curves pin
      // there. That is why the defect is invisible in a black-and-white test
      // image and needed a mid-tone ramp to surface.
      check(std::abs(byteAt(asBuilt, 0) - 0) <= kByteTol,
            "present: linear 0.0 is presented correctly even as built");
      check(std::abs(byteAt(asBuilt, kProbes - 1) - 255) <= kByteTol,
            "present: linear 1.0 is presented correctly even as built");

      if (srgbSurface) {
        // The defect, in numbers. `pow(linear, 2.2)` before an encode that
        // already expects linear is a second decode: mid grey is presented at
        // roughly the byte that half *that* brightness should have.
        std::printf("  DEFECT: linear mid-tones are presented %d codes low "
                    "(pow(v,2.2) applied before an sRGB attachment)\n",
                    worstBuilt);
        check(worstBuilt >= 40,
              "present: as built, the document is off by tens of codes (DEFECT)");
        check(std::abs(byteAt(asBuilt, 4) - byteAt(atUnitGamma, 4)) >= 40,
              "present: linear 0.5 differs by tens of codes between the two gammas");
        check(byteAt(asBuilt, 4) < byteAt(atUnitGamma, 4),
              "present: and it is a darkening -- the value is decoded twice");
        // The model in `presentedByte()` predicts the measurement. If this
        // holds, the explanation is complete: there is no unaccounted term.
        bool modelled = true;
        for (int i = 0; i < kProbes; ++i)
          modelled = modelled && std::abs(byteAt(asBuilt, i) -
                                          presentedByte(linear[i], 2.2f)) <= kByteTol;
        check(modelled,
              "present: srgbEncode(pow(v,2.2)) predicts every measured byte -- fully explained");
      } else {
        check(worstBuilt <= kByteTol,
              "present: non-sRGB surface, so the document is presented correctly as built");
      }
    }

    if (pipeline != nullptr) wgpuRenderPipelineRelease(pipeline);
    dt.release();
  }

  std::printf("[selftest] present transfer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
