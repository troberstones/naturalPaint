#pragma once

#include <cstdint>
#include <vector>

#include "core/OpStack.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"

// color/LutBake (PLAN.md "Phase 3 -- Grade it", step 4; ADR-0004
// (docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)).
//
// Bakes a maximal run of adjacent core::OpClass::PointA ops onto a
// kLutSize^3 rgba16float 3-D texture, entirely on the GPU: one seed compute
// dispatch followed by one compute dispatch per op in the run, ping-ponging
// between two same-sized 3-D textures exactly the way sim/PaintSim.cpp's
// Jacobi pressure solve already ping-pongs between two 2-D textures
// (PingPong, PaintSim.hpp) -- same pattern, one dimension higher, genuinely
// new only in that no 3-D texture exists anywhere else in this codebase yet.
//
// *** Everything the LUT stores, at every stage, is a shaper-domain value,
// not linear ***
// The seed pass writes texel (x,y,z) as shaper-domain coordinate
// ((x+0.5)/kLutSize, ...) directly -- by the LUT's own indexing convention,
// the grid coordinate IS the shaper-domain value being looked up (ADR-0004:
// "linear -> shaper (log encode) -> 3-D LUT -> linear", so the LUT's domain
// *and* range are both shaper-domain). Five of the six op kernels
// (Levels/Exposure/Saturation/Grayscale/ChannelMixer -- every ops/PointOps.hpp
// function except Curves) then wrap their own linear-domain math in an
// outer shaperDecode -> op -> shaperEncode, converting the texel to linear
// for the op and back to shaper domain for storage, so the texture stays
// self-consistently shaper-domain across every ping-pong step. Curves is
// the one exception: ADR-0004 authors curve control points *in* the shaper
// domain, so its WGSL kernel (shaders/lut_op_curves.wgsl) runs evalCurve()
// directly on the texel with no encode/decode wrapper at all -- wrapping it
// the way ops/PointOps.cpp's applyCurves() does (shaperEncode -> evalCurve
// -> shaperDecode, for applyCurves()'s own linear-in/linear-out contract)
// would decode this kernel's already-shaper-domain input to linear and then
// immediately re-encode it back, a redundant lossy round trip. See
// shaders/lut_op_curves.wgsl's own header comment for the same point made
// again at the point it actually matters.
//
// Every op kernel clamps its shaper-domain result to [0,1] before writing --
// the one place color/Shaper.hpp's own header comment defers a clamping
// decision to ("whether/how a shaped value gets clamped before becoming a
// 3-D LUT texture coordinate is color/LutBake's... job"); this is where
// that decision lands.
//
// *** Why N sequential small dispatches, not one big "generic interpreter"
// dispatch, despite PLAN.md step 4's literal "via one compute dispatch"
// wording ***
// A single generic shader branching on a per-texel op-kind tag read from a
// heterogeneous storage buffer would be a brand-new pattern (no
// storage-buffer-of-structs exists anywhere else in this codebase), harder
// to verify (one shader trying to do six different things vs. six small
// shaders each directly comparable 1:1 against their ops/PointOps.cpp
// counterpart), and does not actually help where it matters: Phase 3's
// Verify criterion -- "twelve stacked grade ops cost the same as one at
// draw time" -- is about the *Apply pass's* per-frame draw-time cost
// (sampling the already-baked LUT, O(1) regardless of op count), not
// occasional bake-time cost, which only happens on parameter change
// (PLAN.md's own "rebake on parameter change" wording) and can reasonably
// scale with run length. So: one seed pass, then one small pass per op,
// each reading the previous ping-pong texture and writing the other --
// correct for any run length >= 0 (an empty run: seed only, a valid
// identity LUT; a 12-op run: seed, twelve op passes) with no special-casing
// of "the first op."
//
// *** Takes an ordered std::vector<Op> slice, not a core::OpRun ***
// core::OpRun (core/OpStack.hpp) already collapses each run entry's
// PointOpKind and params into an opaque, already-composed ops::PointOp
// std::function closure -- exactly what a CPU-side evaluator
// (ops::applyPointOpsPremultiplied) needs, and exactly what this GPU baker
// cannot use, since there is no way to run a std::function on the GPU. This
// function needs each entry's *kind and params* instead, to pick and
// parameterize the matching WGSL kernel -- and core::OpStack already
// exposes that without any new accessor: OpStack::at(index) returns the
// original core::Op (kind + params) by index, read-only. A caller already
// holding an OpStack and one of its OpRuns builds the ordered
// `std::vector<Op>` this function wants with a trivial loop over
// `[run.startIndex, run.endIndex)` -- `bakeLut()` itself skips any entry
// with `!enabled` or `opClass != OpClass::PointA` (contributing no GPU
// pass, the identity), so that loop may copy `OpStack::at(i)` verbatim for
// every index in the run's raw range, including disabled entries, without
// having to replicate OpRun::ops's own filtering logic first. This keeps
// core::OpStack completely unchanged and keeps this module decoupled from
// OpStack's run-detection bookkeeping -- LutBake only needs "an ordered
// list of point ops," not "a run of some particular OpStack." The future
// caller that does this slicing is the Apply pass (PLAN.md Phase 3 step 6),
// not built here.
//
// *** Stateless / on-demand -- no caching lives here ***
// bakeLut() bakes unconditionally on every call; it never compares against
// a previous bake or skips work. Deciding *when* to rebake (PLAN.md's
// "rebake on parameter change") is a future caller's job, keyed off
// core::OpStack::version() (already built, Phase 3 step 5) -- that caller
// (the Apply pass, step 6) does not exist yet. Do not add a cache here on
// the strength of this comment; a future reader should not assume one.
namespace np {

// Grid resolution of the baked 3-D LUT. ADR-0004: 32^3 is the default here;
// 64^3 ("2 MiB, still cache-resident") is the escape hatch for a future step
// needing finer resolution (hard posterize, near-vertical curve segments),
// not built by this step -- every function in this file is hardcoded to
// this constant, not parameterized over it.
inline constexpr int32_t kLutSize = 32;

// Curves' per-channel control-point cap, for the GPU kernel's fixed-size
// storage buffer (shaders/lut_op_curves.wgsl's CurvesBuffer). Not a
// format-level commitment the way color::Shaper's constants are -- LUT
// bakes are never saved to disk, only ever recomputed live from an
// OpStack's Curve data, so raising this bound later is a pure
// implementation change, not a document-compatibility one. 16 is a
// generous, ordinary bound for an interactively authored curve.
inline constexpr int32_t kMaxCurvePointsPerChannel = 16;

// One baked LUT's GPU resources. Plain struct, no RAII, no destructor,
// matching ui::GpuTile's (src/ui/NaturalPaintUI.hpp) established convention
// for a GPU-texture-owning value in this codebase: raw handles, released
// explicitly via releaseLut3D() below -- the same "release() is a free
// function/method the owner calls, not a destructor" discipline
// releaseGpuTile() and PingPong::release() (sim/PaintSim.hpp) already
// follow.
struct Lut3D {
  WGPUTexture texture = nullptr;
  WGPUTextureView view = nullptr;
  int32_t size = kLutSize;
};

// Releases `lut`'s texture and view (if any) and zeroes the struct. No
// GpuContext parameter, matching PingPong::release()/releaseGpuTile()'s own
// convention: wgpuTextureDestroy/Release and wgpuTextureViewRelease need
// only the handles being released, not the device that created them. Safe
// to call on an already-released or default-constructed Lut3D.
void releaseLut3D(Lut3D& lut);

// Bakes `ops`, in order, onto a fresh kLutSize^3 rgba16float 3-D LUT and
// returns it. See this header's top comment for the full design (the
// shaper-domain-throughout texture contract, the N-small-dispatches
// reasoning, the OpRun-vs-vector<Op> API choice, and the "stateless, no
// caching" contract).
//
// Any entry in `ops` with `!enabled` or `opClass != OpClass::PointA`
// contributes no GPU pass -- treated as identity, matching
// core::OpRun::ops's own "a disabled entry contributes nothing" rule, so
// callers may pass either an already-filtered list or a raw
// `[startIndex, endIndex)` OpStack slice with disabled/non-PointA entries
// still present. An empty (or all-skipped) `ops` still bakes a valid
// identity LUT -- the seed pass alone, no op passes.
//
// On a shader-compile or pipeline-build failure (should not happen with the
// shaders this codebase ships, but see gfx/ShaderLoader.hpp's own
// "a typo mid-session doesn't kill the app" precedent for why this is
// checked rather than assumed), logs to stderr and returns a
// default-constructed Lut3D (all fields null/zero) with nothing left
// resident -- check `.texture != nullptr` before use.
//
// Returns a Lut3D holding whichever ping-pong texture the last dispatch (or
// the seed dispatch, for an empty/all-skipped `ops`) wrote into; the caller
// owns it and must eventually call releaseLut3D(). The other ping-pong
// texture is released internally before this function returns -- bakeLut()
// keeps no scratch texture alive between calls, consistent with it being
// stateless (there is no "next call" to reuse it against).
Lut3D bakeLut(GpuContext& gpu, const std::vector<Op>& ops);

}  // namespace np
