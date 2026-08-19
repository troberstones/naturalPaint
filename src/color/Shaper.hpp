#pragma once

namespace np {

// ACEScct: the Academy Color Encoding System's published "common log
// encoding" transfer function (spec S-2016-005). This is the **Shaper**
// CONTEXT.md's glossary entry names -- "A 1-D log encoding applied before a
// 3-D LUT, so that linear values above 1.0 fit the LUT's [0,1] domain and
// grading control points land where a user expects them" -- and the module
// ADR-0004 (docs/adr/0004-colour-ops-collapse-to-a-shaper-plus-3d-lut.md)
// specifies as the first stage of the point-op collapse
// (DESIGN-imaging.md §4, "Class A -- point ops -> collapse into one LUT"):
//
//   linear -> shaper (log encode, 1D x3) -> 3D LUT (32^3 or 64^3) -> linear
//
// Why ACEScct specifically, and not a bespoke log2 curve invented for this
// codebase: it is a real, published industry standard used for exactly this
// job -- grading in a log domain ahead of a 3-D LUT is the workflow ACEScct
// was designed for (ACES itself, Resolve, Baselight and OCIO's own ACEScct
// config all shape this way) -- so shaperEncode/shaperDecode's output is
// independently checkable against reference material, not resting on a
// formula only this codebase understands. It is piecewise: a linear segment
// at and below a breakpoint in linear light (2^-7), avoiding the
// log2(0) / log2(negative) singularity a pure log curve would hit at and
// below zero, and a log2 segment above the breakpoint -- with constants
// (fixed by the published spec, not chosen here) such that the two segments
// and their first derivatives are both continuous at the breakpoint. Both
// properties are verified by direct hand computation in Shaper.cpp's header
// comment and pinned permanently by runShaperTest() in app/SelfTest.cpp.
//
// *** This is a format-level commitment, not an implementation detail ***
// -- ADR-0004's own words, and the single hardest-to-reverse decision in the
// colour pipeline. Once a curve is authored and saved, its control points
// are coordinates in *this* domain -- shaperEncode's output range, not
// linear light. Changing these constants later silently reshapes every
// curve in every saved document; there is no "just recompute" fix, because
// the saved numbers themselves *are* the shaper's log-domain coordinates,
// and nothing in a saved document records which shaper produced them. Do
// not change the constants in Shaper.cpp without reading ADR-0004 in full.
//
// One mechanism, three problems solved at once (ADR-0004, DESIGN-imaging.md
// §4):
//  - The 3-D LUT is indexed on [0,1], but scene-linear working-space values
//    routinely exceed 1.0 (HDR-ish highlights -- the working space is
//    unclamped linear light). The log shaper compresses a wide linear range
//    into a bounded shaped range: shaperEncode(16.0f) is still comfortably
//    inside [0,1].
//  - Authoring curves against raw linear light crowds everything
//    perceptually interesting into the bottom few percent of the graph --
//    the log domain is what makes the curve UI usable at all.
//  - Curve control points in the shaped domain land where a
//    Photoshop-trained eye expects them.
//
// Deliberately narrow and pure, matching color/Space.hpp's srgbEncode /
// srgbDecode precedent (that file's *display* transfer functions; this one
// is the *grading* transfer function -- different stage of the pipeline,
// same separation-of-concerns discipline): scalar, per-channel, no
// clamping, no LUT/texture/GPU awareness whatsoever. Callers apply these
// per R/G/B channel -- never to alpha, which is opacity, not light, and is
// never run through any transfer function (io/ImageDecode.hpp and
// core/Probe.hpp document the identical policy for the display transfer
// function; it applies here unchanged). Output is *not* clamped to [0,1]
// here: whether/how a shaped value gets clamped before becoming a 3-D LUT
// texture coordinate is color/LutBake's and the apply pass's job (both
// later, unbuilt steps) -- keeping the clamp decision out of this module is
// what keeps shaperEncode/shaperDecode exactly, independently verifiable
// against the published ACEScct spec, with no policy mixed in.

// Scene-linear -> shaper-domain (ACEScct log encode).
float shaperEncode(float linear) noexcept;
// Shaper-domain -> scene-linear (ACEScct log decode, the exact inverse).
float shaperDecode(float shaped) noexcept;

}  // namespace np
