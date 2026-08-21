#pragma once

#include <cstdint>
#include <string>

#include "gfx/Wgpu.hpp"

// app/Screenshot -- the app photographs its own window.
//
// Built 2026-08-21, during the UI detour, because the obvious route does not
// work here and the reason is worth writing down before someone tries it again.
//
// --- Why not screen capture ----------------------------------------------
//
// macOS gates every route to another process's window pixels behind the
// Screen Recording TCC permission, and the failure mode is *silent*: a capture
// taken without it returns the desktop wallpaper with every window stripped
// out, which reads as "the app isn't running" rather than "you lack a
// permission". Three routes were tried and all three failed:
//
//   * `screencapture -l<windowid>` -- "could not create image from window";
//     CGWindowListCreateImage is gated on macOS 14+.
//   * `screencapture -R<x,y,w,h>` -- captures the CURRENT Space, so it returns
//     the desktop whenever the window sits on another one.
//   * ScreenCaptureKit -- the only one that names the problem:
//     `SCStreamErrorDomain Code=-3801, "The user declined TCCs"`.
//
// Even once granted, the permission is cached per-process and needs the
// grantee restarted, and it is revocable at any time by something outside this
// project's control.
//
// So this module exists instead, and it is strictly better for the job it is
// actually for: verifying a UI change. It needs no permission, does not care
// about window focus or Spaces or occlusion, captures exactly the pixels this
// process rendered rather than whatever the compositor put on a display, and
// is deterministic enough that two frames can be *diffed* -- which is what
// makes a UI change checkable the way `--selftest` output is checkable, rather
// than eyeballed.
//
// --- The one thing that made it possible ---------------------------------
//
// `gfx/Context`'s surface is configured `RenderAttachment | CopySrc`. The
// second flag is this module's entire GPU-side requirement, and it costs
// nothing on a surface that was already being rendered into.
//
// --- The two traps ---------------------------------------------------------
//
//  1. **`bytesPerRow` must be a multiple of 256** (WebGPU's copy alignment).
//     A 1280-wide window at 4 bytes/texel is 5120 = 20 x 256 and happens to be
//     legal, which is exactly why this must be padded rather than asserted:
//     the first odd window size would otherwise produce a silently skewed
//     image rather than an error. The staging buffer is allocated at the
//     padded stride and the padding is dropped row by row on the way out.
//  2. **The surface format is whatever the adapter preferred**, which on this
//     machine is BGRA8Unorm, not RGBA. Writing those bytes to a PNG without
//     swapping produces an image whose reds and blues are exchanged -- a
//     subtle enough error that it would survive review of a screenshot of a
//     mostly-grey UI. The channel order is read from the format at run time,
//     and an unrecognised format is refused by name rather than guessed at.
namespace np {

struct GpuContext;

// Copies `surfaceTexture` into `path` as an 8-bit RGBA PNG.
//
// **Call it after this frame's UI submission and before
// `wgpuSurfacePresent()`.** Both halves of that matter: the UI has to be in
// the texture already or the capture is of an empty backbuffer, and a
// presented texture is no longer readable. It records its own encoder rather
// than borrowing the caller's, because it has to submit and then *wait* -- and
// a caller that had already submitted could not have the wait folded into it.
//
// Blocking: it submits, spins `wgpuInstanceProcessEvents` until the map
// resolves, and copies out. That stalls the frame it runs on, which is right
// for a screenshot and would be wrong anywhere near the paint path -- so this
// is never called per frame.
//
// Returns false and fills `errorOut` on an unsupported surface format, a
// failed map, or a write error. The file is written only once the whole path
// has succeeded, so a failed capture leaves no truncated PNG behind to be
// mistaken for a real one.
bool captureSurfaceToPng(GpuContext& gpu, WGPUTexture surfaceTexture, uint32_t width,
                         uint32_t height, const std::string& path, std::string* errorOut);

}  // namespace np
