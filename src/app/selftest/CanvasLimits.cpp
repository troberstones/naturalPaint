#include "app/selftest/Support.hpp"

#include "app/DocumentPresets.hpp"
#include "core/CanvasLimits.hpp"
#include "ops/DocumentTransform.hpp"

namespace np {

// core/CanvasLimits -- the guard between a document's extent and what the
// adapter can actually create a texture for.
//
// **What this section is really pinning.** Before it existed, opening a
// 17000x1200 PNG aborted the process: `wgpuDeviceCreateTexture` refused the
// canvas-sized "document composite" texture, every subsequent
// `wgpuQueueWriteTexture` failed against the invalid texture, and
// wgpu-native's `wgpuQueueSubmit` panicked -- a Rust panic across the C ABI,
// so `abort()`, taking every other open document with it unsaved. The
// assertions below are therefore not about a message being worded nicely;
// they are about there being a refusal at all, on every path that can set a
// document's extent.
//
// **Everything here drives `canvasDimensionRefusal(w, h, maxDim)`'s
// three-argument form** rather than the ambient one. That is the whole reason
// the three-argument form exists: this machine reports 16384, WebGPU
// guarantees only 8192, and a test written against `maxCanvasDimension()`
// would pass here while saying nothing about the adapter a user actually has.
// Driving an explicit ceiling makes every case below hermetic and makes the
// boundary itself -- exactly at, one past -- testable without a GPU.
bool runCanvasLimitsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- 1. the pure predicate, at and past the boundary --\n");

  constexpr int32_t kCeil = 4096;  // a stand-in, unrelated to this machine's

  check(canvasDimensionRefusal(1, 1, kCeil).empty(), "1x1 is accepted");
  check(canvasDimensionRefusal(kCeil, kCeil, kCeil).empty(),
        "exactly the ceiling on both axes is accepted (<=, not <)");
  check(!canvasDimensionRefusal(kCeil + 1, kCeil, kCeil).empty(),
        "one past the ceiling on width is refused");
  check(!canvasDimensionRefusal(kCeil, kCeil + 1, kCeil).empty(),
        "one past the ceiling on height is refused");
  check(!canvasDimensionRefusal(kCeil + 1, kCeil + 1, kCeil).empty(),
        "one past on both axes is refused");

  // The message has to name the axis, because "too big" sends the user to
  // resize the wrong side of a panorama. Three distinct sentences, checked as
  // distinct rather than by matching their text: a future rewording that
  // collapsed two of them into one would still be caught.
  {
    const std::string w = canvasDimensionRefusal(kCeil + 1, 8, kCeil);
    const std::string h = canvasDimensionRefusal(8, kCeil + 1, kCeil);
    const std::string b = canvasDimensionRefusal(kCeil + 1, kCeil + 1, kCeil);
    check(w != h && h != b && w != b, "width / height / both give three different refusals");
    check(w.find("4096") != std::string::npos, "the refusal names the limit it enforced");
    check(w.find("4097") != std::string::npos, "the refusal names the size that was refused");
  }

  // A non-positive extent is deliberately NOT this function's refusal -- every
  // caller already has its own sentence for it, and a second spelling would
  // make the message depend on which check ran first. Pinned so that "it
  // returns empty for 0x0" reads as the documented contract rather than an
  // oversight someone later "fixes" into a duplicate message.
  check(canvasDimensionRefusal(0, 0, kCeil).empty(),
        "a zero extent is left to the caller's own vocabulary, not refused here");
  check(canvasDimensionRefusal(99999, 99999, 0).empty(),
        "a zero ceiling refuses nothing (an adapter that never reported)");

  std::printf("  -- 2. the ambient ceiling --\n");

  // Never zero and never negative: a zero would refuse every document, which
  // is the one failure mode worse than the abort this guards.
  check(maxCanvasDimension() > 0, "maxCanvasDimension() is positive before any GPU reports");
  check(maxCanvasDimension() >= kFallbackMaxCanvasDimension,
        "the default is at least WebGPU's guaranteed 8192");

  // `setMaxCanvasDimension()` ignores a non-positive report rather than
  // storing it -- an adapter whose `wgpuAdapterGetLimits()` failed must leave
  // the conservative default standing, not set a ceiling of zero.
  {
    const int32_t before = maxCanvasDimension();
    setMaxCanvasDimension(0);
    check(maxCanvasDimension() == before, "setMaxCanvasDimension(0) is ignored");
    setMaxCanvasDimension(-4);
    check(maxCanvasDimension() == before, "setMaxCanvasDimension(-4) is ignored");
  }

  // **The two bounds that never met.** app/DocumentPresets' 32768 is a
  // corrupt-input bound; this is a render bound. The defect was that nothing
  // related them, so the New Document dialog offered sizes the renderer
  // aborted on. This asserts the relationship rather than either constant:
  // whatever the adapter reports, the parse bound must not be the *only*
  // thing standing between a typed number and the GPU.
  check(kFallbackMaxCanvasDimension <= kMaxDocumentPresetDimension,
        "the render ceiling is never above the preset parse bound");

  std::printf("  -- 3. the document paths that can set an extent --\n");

  // Image Size and Canvas Size are the two commands that can take a document
  // that opened perfectly and grow it past the ceiling -- the open-path guard
  // would never see it. Both refuse, and both leave the document untouched,
  // which is what their own "Nothing was changed." claims.
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    const int32_t oversize = maxCanvasDimension() + 1;

    DocumentTransformResult canvasR =
        resizeDocumentCanvas(doc, static_cast<uint32_t>(oversize), 64u, CanvasAnchor::Center,
                             nullptr);
    check(!canvasR.ok, "canvas size past the ceiling is refused");
    check(doc.width == 64 && doc.height == 64,
          "a refused canvas size left the document at its old extent");

    DocumentTransformParams params;
    DocumentTransformResult imageR =
        resizeDocumentImage(doc, static_cast<uint32_t>(oversize), 64u, params, nullptr);
    check(!imageR.ok, "image size past the ceiling is refused");
    check(doc.width == 64 && doc.height == 64,
          "a refused image size left the document at its old extent");

    // The complement, so the guard cannot pass by refusing everything -- the
    // failure mode a bounds check most easily degrades into.
    DocumentTransformResult okR =
        resizeDocumentCanvas(doc, 128u, 128u, CanvasAnchor::Center, nullptr);
    check(okR.ok && doc.width == 128 && doc.height == 128,
          "a canvas size inside the ceiling still succeeds");
  }

  return ok;
}

}  // namespace np
