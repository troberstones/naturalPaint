#pragma once

#include <cstdint>
#include <string>

// core/CanvasLimits -- the largest canvas this build can put on screen, and
// the one refusal every path that sets a document's extent shares.
//
// ==========================================================================
// Why this exists: an abort, not a wrong picture
// ==========================================================================
//
// `ui/DocumentTexture` composites into **one** WGPU texture the full size of
// the canvas (`WGPUTextureFormat_RGBA16Float`, created in `viewFor()`), and
// nothing used to compare a document's extent against what the adapter can
// actually create. Opening a 17000x1200 PNG on an M4 Max therefore did this,
// measured 2026-09-03:
//
//   [open] Opened 'wide.png' (PNG, 17000x1200) as a new document.
//   [gpu] error (2): Validation Error
//     In wgpuDeviceCreateTexture, label = 'document composite'
//       Dimension X value 17000 exceeds the limit of 16384
//     In wgpuTextureCreateView / wgpuQueueWriteTexture   (once per frame after)
//   thread '<unnamed>' panicked at src/lib.rs:598:5:
//   Error in wgpuQueueSubmit: Validation Error
//   fatal runtime error: failed to initiate panic, error 5, aborting
//
// The file opened fine. The renderer then failed every frame until wgpu-native
// panicked inside `wgpuQueueSubmit`, and a Rust panic across the C ABI is an
// **abort**: every other open document went with it, unsaved. So this is not a
// cosmetic limit -- it is the difference between a message and data loss, and
// that is why the refusal lives here rather than being left to whichever call
// site happens to remember.
//
// **The two bounds that never met.** `app/DocumentPresets.hpp`'s
// `kMaxDocumentPresetDimension` is 32768 -- deliberately generous, because its
// job is catching "a pasted extra digit" in a hand-edited presets file, and its
// own comment says no real hand-chosen size comes close. That is a *corrupt
// input* bound and it is correct as one. It is also exactly twice the limit the
// GPU enforces, so the New Document dialog invited a size the renderer could
// not survive. Both bounds stay: this one is about what can be *drawn*, that
// one about what can be *parsed*, and a document rejected by either is rejected
// for a reason the other cannot state. `--selftest` pins the relationship
// (this ceiling is never above that one) rather than leaving the two constants
// to drift apart again.
//
// ==========================================================================
// Why the ceiling is a variable with one writer
// ==========================================================================
//
// `maxTextureDimension2D` is a property of the adapter, not of this source
// tree: WebGPU *guarantees* 8192 and this machine reports 16384. Hard-coding
// either would be wrong in a different direction -- 8192 refuses documents
// this GPU draws perfectly, 16384 aborts on hardware that only promises the
// minimum. So `main()` sets it once from `GpuContext::maxTextureDimension`
// immediately after `gpu.init()`, and it is `kFallbackMaxCanvasDimension`
// (the spec's guaranteed floor) until it does.
//
// The default matters more than it looks. Three callers reach the refusal
// before or without any GPU at all -- `--selftest`, the golden harness, and
// any headless path that opens a file -- and a ceiling of 0 would refuse every
// document while a ceiling of INT_MAX would let the headless paths disagree
// with the windowed one about the same file. The spec floor is the only value
// that is honest in all three cases: it is what *some* conforming adapter
// would enforce, so a document that passes headlessly is a document that draws
// somewhere.
//
// **`canvasDimensionRefusal()` takes the ceiling as a parameter** and is pure,
// so the suite can drive both sides of the boundary on every path without a
// GPU and without depending on what this particular machine reports -- the
// mistake that would make the test green here and silent on an 8192 adapter.
namespace np {

// WebGPU's guaranteed minimum for `maxTextureDimension2D` (WebGPU spec,
// "Limits" table). Every conforming adapter supports at least this, so it is
// the only defensible answer before one has been asked.
inline constexpr int32_t kFallbackMaxCanvasDimension = 8192;

// The largest width or height, on either axis independently, that this build
// can currently render. `kFallbackMaxCanvasDimension` until `main()` reports
// the adapter's real figure.
int32_t maxCanvasDimension() noexcept;

// Called **once**, by `main()`, with `GpuContext::maxTextureDimension`. A
// zero or negative `px` is ignored rather than stored: an adapter that failed
// to report leaves the conservative default in place instead of setting a
// ceiling that refuses everything.
void setMaxCanvasDimension(int32_t px) noexcept;

// The empty string when a `width` x `height` canvas can be rendered at a
// ceiling of `maxDim`; otherwise the exact sentence to show the user, naming
// the requested size, the offending axis and the limit.
//
// A non-positive extent is **not** this function's refusal -- every caller
// already refuses that in its own vocabulary (`resizeDocumentImage()` has "no
// scale factor from nothing", `openAnyFileAsDocument()` has the decoder's own
// message), and inventing a second spelling for it here would mean two
// different messages for one condition depending on which check ran first.
std::string canvasDimensionRefusal(int32_t width, int32_t height, int32_t maxDim);

// `canvasDimensionRefusal(width, height, maxCanvasDimension())`. The form
// every call site outside the suite wants.
std::string canvasDimensionRefusal(int32_t width, int32_t height);

}  // namespace np
