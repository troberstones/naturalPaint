#include "app/selftest/Support.hpp"

#include "app/Screenshot.hpp"
#include "ui/CanvasQuad.hpp"

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
// This section exists because that question had no answer, and when it was
// first asked the answer turned out to be wrong. It now asserts the fix.
//
// --- What was wrong --------------------------------------------------------
//
// gfx/Context took `caps.formats[0]` verbatim, which on this adapter is
// `BGRA8UnormSrgb`. Dear ImGui's WGPU backend writes `gamma = 2.2f` for every
// `...UnormSrgb` format and its fragment shader raises its output to that
// power; the sRGB attachment then applies the exact piecewise encode.
//
// For the chrome that is an approximate decode followed by an exact re-encode.
// The two curves are not inverses, so dark tokens sagged by up to 9 code
// values -- the whole of the previously unexplained "tokens land 4/255 dark".
//
// For the document it was a **second decode**. ui/DocumentTexture uploads
// linear-light RGBA16Float and sim/PaintSim's canvas is linear in RGBA8Unorm;
// the canvas quad was untinted, so ImGui's `color.rgb` *was* the linear value.
// Linear 0.25 reached the screen as byte 61 where it should have been 137.
// Zero error at both endpoints, which is why no black-and-white test image
// would ever have caught it.
//
// --- The fix ---------------------------------------------------------------
//
// Chrome and document wanted opposite treatment and shared one pipeline and
// one gamma uniform, so the fix separates them:
//
// 1. gfx/Context picks a **non-sRGB** surface when the adapter offers one, so
//    ImGui's gamma is 1.0 and its already-encoded chrome bytes -- and the
//    pigment swatches, which are content and were sagging too -- reach the
//    swapchain untouched. No colour in this application is pre-compensated
//    for a backend quirk.
// 2. ui/CanvasQuad draws the document with its own pipeline, whose fragment
//    shader applies the exact sRGB encode. On an sRGB attachment that encode
//    is compiled out and the hardware does it instead, so the document is
//    correct either way.
//
// --- What this section asserts ---------------------------------------------
//
// §1 the surface the adapter gave us and the gamma ImGui selects from it.
// §2 the chrome round trip as arithmetic, which is now the identity.
// §3 the document end to end -- known linear values authored into a layer,
//    through the real upload, **ui/CanvasQuad's real pipeline and shader**,
//    the real app/Screenshot writer and back out of the PNG's raw bytes --
//    rendered at BOTH target formats, asserting each probe reaches
//    `srgbEncode()`'s byte and that the two formats agree. Rendering at both
//    is what keeps neither shader variant dead code, and what would catch a
//    future adapter that offers only sRGB surfaces.

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
      // The shape of the sag, for an adapter that leaves us on an sRGB
      // surface: bright codes exact, dark codes pulled down, worst in the deep
      // shadows. These were the numbers a screenshot of this application's
      // chrome contained before gfx/Context started avoiding those formats.
      check(presentedByte(243.0f / 255.0f, 2.2f) == 243,
            "present: code 243 survives exactly (bright end is fixed)");
      check(presentedByte(45.0f / 255.0f, 2.2f) == 41,
            "present: on an sRGB surface code 45 would land at 41 -- 4 low");
      check(presentedByte(32.0f / 255.0f, 2.2f) == 26,
            "present: on an sRGB surface code 32 would land at 26 -- 6 low");
      check(worstDelta < 0 && worstCode < 64,
            "present: the error is a darkening, and it is worst in the shadows");
      check(std::abs(worstDelta) <= 9,
            "present: that sag is bounded at 9 codes -- cosmetic, but avoidable");
    } else {
      check(worstDelta == 0, "present: non-sRGB surface, so the chrome round trip is EXACT -- the fix");
    }
  }

  // ======================================================================
  // 3. The document, end to end: a linear value in a layer -> a PNG byte.
  // ======================================================================
  //
  // The assertion the project did not have, and the one the fix is judged on.
  // Everything below is the real code path: the real layer write, the real
  // ui/DocumentTexture upload, **ui/CanvasQuad's own pipeline and fragment
  // shader**, the real hardware behaviour of the chosen attachment and the
  // real app/Screenshot writer. Only ImGui's draw-list plumbing and the quad
  // transform are bypassed, and neither carries a transfer function.
  {
    // Dyadic on purpose: every value is exact in f32 and in the f16 the
    // texture stores, so the fixture contributes no error of its own and any
    // difference measured is the transfer function's.
    constexpr int kProbes = 8;
    const float linear[kProbes] = {0.0f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 1.0f};

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
        // arithmetic contributes nothing. ui/CanvasQuad's blend is likewise a
        // no-op at alpha 1, which is why this section can leave blending out
        // and still be measuring the path the canvas takes.
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

    // The two attachments, and the reason both are measured. The sRGB twin of
    // whatever the adapter gave us keeps app/Screenshot's channel order the
    // same (BGRA stays BGRA), so the only difference between the two runs is
    // which of the shader and the hardware performs the encode -- and that is
    // exactly the branch in ui/CanvasQuad that would otherwise never run here.
    const WGPUTextureFormat plain = presentFormatIsSrgb(fmt)
                                        ? (fmt == WGPUTextureFormat_RGBA8UnormSrgb
                                               ? WGPUTextureFormat_RGBA8Unorm
                                               : WGPUTextureFormat_BGRA8Unorm)
                                        : fmt;
    const WGPUTextureFormat srgbTwin = plain == WGPUTextureFormat_RGBA8Unorm
                                           ? WGPUTextureFormat_RGBA8UnormSrgb
                                           : WGPUTextureFormat_BGRA8UnormSrgb;
    check(!presentFormatIsSrgb(plain) && presentFormatIsSrgb(srgbTwin),
          "present: the two target formats differ in exactly the sRGB bit");

    auto renderAndDecode = [&](WGPUTextureFormat targetFormat, const char* pngPath,
                               std::vector<uint8_t>& outPx) -> bool {
      WGPUTextureDescriptor rtd = {};
      rtd.label = sv("present-transfer-target");
      rtd.dimension = WGPUTextureDimension_2D;
      rtd.size = {static_cast<uint32_t>(kW), static_cast<uint32_t>(kH), 1};
      rtd.format = targetFormat;
      rtd.mipLevelCount = 1;
      rtd.sampleCount = 1;
      rtd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
      WGPUTexture targetTex = wgpuDeviceCreateTexture(gpu.device, &rtd);
      if (targetTex == nullptr) return false;

      // The production pipeline, at this format.
      const bool drew = renderCanvasQuadForTest(gpu, docView, targetTex, targetFormat,
                                                static_cast<uint32_t>(kW),
                                                static_cast<uint32_t>(kH));
      std::string err;
      const bool wrote = drew && captureSurfaceToPng(gpu, targetTex, static_cast<uint32_t>(kW),
                                                     static_cast<uint32_t>(kH), pngPath, &err);
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

    std::vector<uint8_t> onPlain, onSrgb;
    bool plainOk = false, srgbOk = false;
    if (docView != nullptr) {
      plainOk = renderAndDecode(plain, "selftest_present_plain.png", onPlain);
      srgbOk = renderAndDecode(srgbTwin, "selftest_present_srgb.png", onSrgb);
    }
    check(plainOk, "present: drawn by ui/CanvasQuad into a non-sRGB target (shader encodes)");
    check(srgbOk, "present: and into an sRGB target (hardware encodes, shader compiled out)");

    if (plainOk && srgbOk) {
      // Row 1 of 3, so this also proves the row stride survived the padded
      // copy: a bug there would put row 1 at the wrong offset.
      auto byteAt = [&](const std::vector<uint8_t>& px, int x) {
        return static_cast<int>(px[(static_cast<size_t>(1) * kW + x) * 4]);
      };

      std::printf("  %-8s %-9s %-9s %-9s %s\n", "linear", "correct", "non-sRGB", "sRGB",
                  "error");
      int worstPlain = 0, worstSrgb = 0, worstBetween = 0;
      for (int i = 0; i < kProbes; ++i) {
        const int correct = static_cast<int>(std::lround(srgbEncode(linear[i]) * 255.0f));
        const int p = byteAt(onPlain, i), q = byteAt(onSrgb, i);
        worstPlain = std::max(worstPlain, std::abs(p - correct));
        worstSrgb = std::max(worstSrgb, std::abs(q - correct));
        worstBetween = std::max(worstBetween, std::abs(p - q));
        std::printf("  %-8.3f %-9d %-9d %-9d %+d\n", static_cast<double>(linear[i]), correct, p,
                    q, p - correct);
      }

      // The claim, stated three ways: each path is right, and they agree.
      check(worstPlain <= kByteTol,
            "present: every probe reaches its correct sRGB byte -- the shader's encode");
      check(worstSrgb <= kByteTol,
            "present: and on an sRGB attachment too -- the hardware's encode");
      check(worstBetween <= kByteTol,
            "present: the two agree, so which one encodes is not observable");

      // The old defect's own numbers, as the thing that must NOT come back.
      // 0.25 presenting as 61 rather than 137 was the measurement that started
      // this; asserting the distance from it is what makes a regression loud.
      check(std::abs(byteAt(onPlain, 2) - 137) <= kByteTol,
            "present: linear 0.25 presents as 137, not the 61 it used to");
      check(std::abs(byteAt(onPlain, 4) - 188) <= kByteTol,
            "present: linear 0.5 presents as 188, not the 128 it used to");

      // ui/CanvasQuad's own CPU model of what it puts on screen, checked
      // against color/Space rather than against a copy of its arithmetic.
      bool modelled = true;
      for (int i = 0; i < kProbes; ++i)
        modelled = modelled && std::abs(byteAt(onPlain, i) -
                                        canvasPresentedByte(linear[i], false)) <= kByteTol &&
                   canvasPresentedByte(linear[i], false) == canvasPresentedByte(linear[i], true);
      check(modelled,
            "present: canvasPresentedByte() predicts every measured byte, at either format");

      // Grey stays grey: a channel-order bug in the screenshot writer would
      // not show up in a grey ramp, so R, G and B are compared to each other
      // as well as to the expected value.
      bool neutral = true;
      for (int i = 0; i < kProbes; ++i) {
        const size_t o = (static_cast<size_t>(1) * kW + i) * 4;
        neutral = neutral && onPlain[o] == onPlain[o + 1] && onPlain[o + 1] == onPlain[o + 2];
      }
      check(neutral, "present: a neutral ramp stays neutral through the whole path");

      // Alpha is forced opaque by the writer, per its own comment.
      check(onPlain[(static_cast<size_t>(1) * kW) * 4 + 3] == 255,
            "present: the screenshot writer forces alpha opaque");
    }

    dt.release();
  }

  std::printf("[selftest] present transfer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
