#include "app/selftest/Support.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include "io/ClipboardImage.hpp"

namespace np {
namespace {

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

// A tiny, fully-synthetic straight-alpha linear image: four texels, each a
// pure 0 or 1 per channel so io/Export's sRGB transfer function and 8-bit
// quantization are both exact round trips (srgbEncode(0)==0,
// srgbEncode(1)==1, and quantize() of either is exact) -- any residual after
// the PNG round trip below is a real bug in the decode path, not
// quantization noise this test would have to tolerate.
DecodedImage makeFixtureImage() {
  DecodedImage img;
  img.width = 2;
  img.height = 2;
  img.pixels = {
      // (0,0) red, opaque      (1,0) green, opaque
      1, 0, 0, 1, 0, 1, 0, 1,
      // (0,1) blue, opaque     (1,1) white, transparent
      0, 0, 1, 1, 1, 1, 1, 0,
  };
  return img;
}

}  // namespace

// io/ClipboardImage: the system-pasteboard bridge (docs/testing-issues.md
// T9, piece 2). See io/ClipboardImage.hpp §0 for the SDL3-capability finding
// this module rests on, and §2 for why this suite is split the way it is.
bool runClipboardImageTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // 1. decodeClipboardImageBytes(): the pure decode logic, against a REAL
  //    PNG this test encodes with io/Export's own encoder -- not a hand-
  //    written fixture, and not the live pasteboard. This is the part of
  //    the image path --selftest CAN and does prove headlessly (see this
  //    function's header comment on what it cannot).
  // ==========================================================================
  {
    const DecodedImage fixture = makeFixtureImage();
    const ExportResult enc = encodeLinearImage(fixture, WorkingSpace{}, ImageFormat::Png,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt8);
    check(enc.ok && !enc.bytes.empty(),
          "clipboardimage: (setup) the fixture encodes to real PNG bytes");

    const ClipboardImageProbe decoded =
        decodeClipboardImageBytes("image/png", enc.bytes.data(), enc.bytes.size());
    check(decoded.status == ClipboardImageStatus::Image,
          "clipboardimage: real PNG bytes at mime type 'image/png' decode as Image");
    check(decoded.width == 2 && decoded.height == 2,
          "clipboardimage: the decoded probe reports the fixture's exact pixel dimensions");
    check(decoded.mimeType == "image/png",
          "clipboardimage: the probe records which mime type actually decoded");
    if (decoded.status == ClipboardImageStatus::Image && decoded.pixels.size() == 16) {
      float maxResidual = 0.0f;
      for (int i = 0; i < 16; ++i)
        maxResidual = std::max(maxResidual, std::fabs(decoded.pixels[i] - fixture.pixels[i]));
      std::printf("    [measured] clipboard PNG round-trip max residual = %.3e\n",
                  static_cast<double>(maxResidual));
      check(maxResidual < 1e-6f,
            "clipboardimage: every channel of every texel comes back exactly -- 0/1 fixture "
            "values are exact under sRGB encode and 8-bit quantization, so any residual here "
            "is a real decode bug, not rounding");
    }

    // Garbage bytes at a plausible image mime type -- the "an image/* type
    // was on offer but this build could not actually read it" case.
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    const ClipboardImageProbe fromGarbage =
        decodeClipboardImageBytes("image/png", garbage, sizeof(garbage));
    check(fromGarbage.status == ClipboardImageStatus::Unreadable && !fromGarbage.detail.empty(),
          "clipboardimage: bytes that are not really a PNG report Unreadable with a reason, "
          "never a crash");
    check(contains(fromGarbage.detail, "image/png"),
          "clipboardimage: the Unreadable detail names which mime type was tried");

    // Zero-length data -- the shape SDL_GetClipboardData() itself returns
    // for "type is present but there is nothing behind it" before this
    // module ever gets involved (probeClipboardImage() skips a zero-size
    // result rather than handing it to the decoder at all; asserted
    // directly here for the decoder's own robustness too).
    const ClipboardImageProbe fromEmpty = decodeClipboardImageBytes("image/png", nullptr, 0);
    check(fromEmpty.status == ClipboardImageStatus::Unreadable,
          "clipboardimage: zero bytes at an image mime type is Unreadable, not a crash and not "
          "a 0x0 Image");
  }

  // ==========================================================================
  // 2. probeClipboardImage(): the live SDL/Cocoa pasteboard, Empty and
  //    NotAnImage outcomes only (§2 of the header -- no synthetic image is
  //    ever placed on the real pasteboard by this suite). Whatever text was
  //    on the clipboard before this section runs is saved and restored
  //    afterward on a best-effort basis: SDL only gives this process back
  //    TEXT, so if the previous contents were something else (an image, a
  //    file list), this section cannot restore it byte-for-byte -- it can
  //    only avoid leaving stray test text behind.
  // ==========================================================================
  {
    char* prevText = SDL_GetClipboardText();
    const bool hadPrevText = prevText && *prevText != '\0';
    const std::string savedText = hadPrevText ? prevText : std::string();
    if (prevText) SDL_free(prevText);

    check(SDL_ClearClipboardData(), "clipboardimage: (setup) SDL_ClearClipboardData() succeeds");
    const ClipboardImageProbe empty = probeClipboardImage();
    check(empty.status == ClipboardImageStatus::Empty,
          "clipboardimage: probeClipboardImage() on a cleared pasteboard reports Empty");
    check(empty.width == 0 && empty.height == 0 && empty.pixels.empty(),
          "clipboardimage: Empty carries no width, height or pixels -- never a manufactured "
          "0x0 document's worth of data");

    check(SDL_SetClipboardText("naturalPaint --selftest clipboard probe fixture"),
          "clipboardimage: (setup) SDL_SetClipboardText() succeeds");
    const ClipboardImageProbe text = probeClipboardImage();
    check(text.status == ClipboardImageStatus::NotAnImage,
          "clipboardimage: probeClipboardImage() on a pasteboard holding only text reports "
          "NotAnImage, not a crash and not a false Image");
    check(text.width == 0 && text.height == 0 && text.pixels.empty(),
          "clipboardimage: NotAnImage also carries no width, height or pixels");

    // Best-effort restore.
    if (hadPrevText) SDL_SetClipboardText(savedText.c_str());
    else SDL_ClearClipboardData();
  }

  std::printf("[selftest] clipboard image %s\n", ok ? "PASS" : "FAIL");
  std::printf(
      "[selftest] clipboard image: probeClipboardImage()'s Image outcome (a real picture "
      "actually decoded off the live NSPasteboard) is NOT exercised by --selftest -- see "
      "io/ClipboardImage.hpp section 2 and this task's report for how it was verified "
      "manually.\n");
  return ok;
}

}  // namespace np
