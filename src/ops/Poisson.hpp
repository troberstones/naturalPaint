#pragma once

#include <array>
#include <cstdint>
#include <vector>

// ops/Poisson -- **the gradient-domain solve behind the Heal tool.**
//
// ==========================================================================
// 0. What "heal" means arithmetically, and why it is a Poisson problem
// ==========================================================================
//
// A clone stamp copies the source outright: `healed = src`. Every time the
// source and the destination stand under different light -- and on a photograph
// or a painting they almost always do -- that copy arrives as a rectangle of
// visibly wrong brightness with a hard seam around it, and the tool's whole
// output is the seam. A heal copies the source's TEXTURE and keeps the
// destination's ILLUMINATION, which is Perez et al.'s seamless clone:
//
//     minimise  integral over W of |grad(f) - grad(src)|^2
//     subject to  f = dst  on the boundary of W
//
// whose Euler-Lagrange equation is the Poisson equation
// `laplace(f) = laplace(src)` with Dirichlet data `dst` on the rim.
//
// **The substitution is what makes this small.** Write `f = src + h`. Then
// `laplace(f) = laplace(src)` becomes `laplace(h) = 0`, and `f = dst` on the
// rim becomes `h = dst - src` on the rim. So the whole solve is: **take the
// error the copy would have left around its own edge, and spread it smoothly
// across the inside.** No divergence field, no per-texel right-hand side, no
// second image to differentiate -- one harmonic interpolation of one ring of
// numbers, which is exactly `harmonicFill()` below and nothing more.
//
// That is also why this file is in `ops/` and named for the maths rather than
// for the tool: it knows nothing of a tile, a dab, a `Layer` or a stroke.
// `brush/Heal` is what turns it into a tool.
//
// ==========================================================================
// 1. A constant rim is solved EXACTLY, and that is a statement about the
//    problem rather than a shortcut in the solver
// ==========================================================================
//
// A constant field is harmonic -- its five-point Laplacian is identically zero
// -- so when every rim value is the same number `c`, the exact solution of
// `laplace(h) = 0, h = c on the rim` is `h == c` everywhere, with no iteration
// at all. `harmonicFill()` recognises that case and returns it bit-exactly.
//
// Two facts a user can see follow from it, and both are the reason to bother:
//
//   * **Healing from a region identical to the destination changes nothing, bit
//     for bit.** The rim of `dst - src` is all zeros, so `h == 0` and
//     `healed == src == dst` exactly. A tool that perturbed the picture by a
//     rounding error per dab while claiming to have found nothing to fix would
//     dirty tiles, move the revision and re-upload textures on every frame of a
//     drag that changed nothing.
//   * **A pure difference in illumination is removed EXACTLY.** If the source
//     is the destination plus a constant `c` -- the textbook case, two patches
//     of the same material under different light -- the rim of `dst - src` is
//     `-c` everywhere, so `h == -c` and `healed == dst` exactly. This is the
//     single property that separates a heal from a clone, and it is asserted at
//     zero tolerance rather than within a convergence bound.
//
// **This is not a fast path bolted onto an approximate solver.** An iterative
// solver started from zero approaches `c` from below and never reaches it, so
// without this the two claims above would both have to be made "within a
// tolerance that depends on how many cycles we ran", which is a promise about a
// setting rather than about the tool.
//
// ==========================================================================
// 2. Everything else: a geometric multigrid V-cycle, mask-free
// ==========================================================================
//
// The general rim needs a real solve, and the solver is `flats/Membrane`'s --
// red-black Gauss-Seidel smoothing, full-weighting restriction of the residual,
// bilinear prolongation of the coarse correction -- with one part deliberately
// removed and one part deliberately kept.
//
// **Removed: the line search on the coarse correction.** That header explains
// why it needs one: coarsening a drawing turns every thin channel into ink, so
// the coarse grid is solving a *different problem* and its correction can point
// the wrong way. There is no mask here. The domain is a rectangle, every
// interior cell is free at every level, and the coarse problem is the same
// problem on a coarser grid -- the textbook case a V-cycle was derived for. A
// line search would still be correct; it would just be an unexplained cost and
// an unexplained line of code.
//
// **Kept: the residual-relative stopping rule.** `cycles` is a ceiling, not a
// setting.
//
// **The interior sums are computed PAIRWISE** -- `(left + right) + (up + down)`
// rather than accumulated one at a time -- so that a constant field is a
// bit-exact fixed point of the smoother: `(c+c)+(c+c)` is exactly `4c` in
// binary floating point where `((c+c)+c)+c` is not. §1's exact answers survive
// any number of sweeps that follow them instead of drifting.
//
// **The cost is quadratic in the brush radius, and that is stated rather than
// hidden.** One heal dab solves over its own bounding box, so a 10 px tip
// solves ~400 cells and a 200 px tip solves ~160000, four times over (§3). The
// tool is a retouching tool used in short strokes at moderate size; a full-width
// heal is slow, and it is slow for a reason the picture shows.
//
// ==========================================================================
// 3. Four channels, solved independently, including alpha
// ==========================================================================
//
// `healPatch()` runs the scalar solve four times over a premultiplied RGBA
// patch. Independently, because the Laplacian is separable across channels and
// there is nothing to couple them with -- and **including alpha**, which is the
// part worth arguing.
//
// A heal whose rim carried a step in coverage and did not solve for it would
// blend colour smoothly across an edge while leaving the alpha discontinuity it
// came from, which is the one combination `core::Tile`'s premultiplied storage
// cannot represent honestly: colour scaled by one coverage sitting in a texel
// labelled with another. Solving alpha with the rest keeps `healed` a
// well-formed premultiplied texel by construction, and in the overwhelmingly
// common case -- an opaque source over an opaque destination -- the alpha rim is
// all zeros and §1 answers it for free.
//
// The clamping that follows the solve is `brush/Heal`'s, not this file's: what
// a texel may legally hold is a property of the storage, and `ops/` does not
// know what storage this patch came out of.
namespace np {

// Solve `laplace(u) = 0` on the interior of a `w` x `h` grid, holding the
// one-cell border ring at whatever `u` already contains there.
//
// **In-place, and the ring is the input.** `u` must be `w * h` values with the
// border ring carrying the Dirichlet data; the interior is overwritten and its
// incoming contents are ignored. That shape -- rather than a separate ring
// argument -- is what lets the multigrid levels below reuse the same buffer
// layout for the correction, whose own ring is zero.
//
// A grid with no interior at all (`w < 3` or `h < 3`) is left exactly as it
// arrived: there is nothing to solve, and a "boundary only" patch is a
// legitimate input at the very edge of a canvas rather than an error.
//
// `cycles` is a ceiling: the solve stops as soon as the residual has fallen by
// `tol` relative to where it started, or immediately when §1's constant rim is
// recognised.
void harmonicFill(std::vector<float>& u, int w, int h, int cycles = 8, float tol = 1e-4f);

// One healed patch: `src`'s texture carried under `dst`'s illumination.
//
// `src` and `dst` are `w * h` premultiplied RGBA texels in row-major order, and
// the result is the same size. Every texel of the **border ring** comes back
// bit-identical to `dst` -- it is the boundary condition, not a computed value
// -- so a caller that wants the whole of a region healed must hand in a patch
// one texel larger on every side than the region it cares about.
//
// The maths is §0's: `healed = src + h`, with `h` the harmonic interpolation of
// `dst - src` over the ring. Total for every finite input; a `w` or `h` below 3
// returns `dst` unchanged, for `harmonicFill()`'s reason.
std::vector<std::array<float, 4>> healPatch(const std::vector<std::array<float, 4>>& src,
                                            const std::vector<std::array<float, 4>>& dst, int w,
                                            int h, int cycles = 8);

}  // namespace np
