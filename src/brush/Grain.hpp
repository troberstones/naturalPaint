#pragma once

#include <cstdint>

// brush/Grain -- **paper tooth: a tiled height field the tip pushes paint
// into, and skips over.**
//
// ==========================================================================
// 0. Source, and what it does and does not license
// ==========================================================================
//
// docs/brush-model-references.md's "Grain interaction" paragraph, which is
// itself a compressed reading of US 5,347,620 (Zimmer, filed 1991, EXPIRED --
// public domain, see that file's licensing table). The patent's own mechanism,
// as recorded there:
//
//   * Grain is a **tiled wraparound lookup**, `grain_pattern[Y % NR][X % NC]`,
//     indexed by ABSOLUTE document coordinates -- not by a coordinate local to
//     the dab, which is what makes it paper and not wallpaper (§3 below).
//   * The overlay fraction is `F = clamp(P*S*O1 - G, 0, 1)`, where `P` is tip
//     pressure/height at that texel, `G` is the grain surface height there,
//     and `S`/`O1` are scaling terms the source material names but does not
//     define further.
//   * Deep valleys fill; peaks get skipped. That is what makes colored pencil
//     and charcoal read the way they do, and this module's whole job is to
//     make that true of a `dabCoverage()` result.
//
// **What is NOT from the patent, and is this file's own derivation, said
// plainly rather than dressed as a citation:**
//
//   * `grain_pattern[NR][NC]` in the patent is a stored lookup table -- almost
//     certainly a digitised scan of a real paper or canvas, the way a `.abr`'s
//     `samp` block is a digitised scan of a real brush tip
//     (brush/Deposit.hpp §2c). This codebase ships no binary fixtures
//     (CONTEXT.md, and this task's own hard rule 5) and has no such scan, so
//     `grainHeightAt()` below GENERATES the table's entries procedurally, from
//     a deterministic hash, rather than reading one. That table is filled
//     lazily, one coordinate at a time, and never materialised as an array --
//     which is an implementation choice free to make because a hash keyed on
//     lattice position already IS "the same value every time this cell is
//     looked up", the one property a stored table would give for free.
//   * `S` and `O1` are two multiplicative terms in the source paragraph with
//     no stated distinct meaning between them -- the material available here
//     could not resolve what a caller would set one to that it would not
//     equally set the other to. `grainOverlayFraction()` below keeps BOTH
//     terms, spelled with the patent's own letters, so the formula's shape is
//     exactly what the source states; `grainCoverageAt()`, the function
//     everything else in this codebase actually calls, folds `O1` to `1.0f`
//     and exposes only `S` (as `GrainParams::strength`) to a caller. That is
//     this file's own simplification, not a second citation.
//   * The whole of `GrainParams` -- what a "paper" is parameterised by, and
//     the numbers it defaults to -- is this file's own design. The patent
//     names a lookup TABLE, not a period/depth/strength triple; translating
//     "a scanned sheet of paper" into "a period and a contrast" for a
//     procedurally generated field is this module's call, argued for in §1.
//
// ==========================================================================
// 1. Why a hash lookup, not interpolated noise
// ==========================================================================
//
// `brush/Dynamics.hpp`'s NOISE source (`dynamicNoiseAt()`) is smoothstep-
// interpolated between hashed lattice points, because what it drives -- a
// slider value drifting along a stroke -- reads as a kink if its derivative
// jumps. Paper tooth is a different kind of surface: real paper fibre is
// high-frequency and textural, not a smooth undulation, and the patent's own
// mechanism is a raw per-TEXEL table lookup with no interpolation term
// anywhere in it. So `grainHeightAt()` hashes the texel's own wrapped integer
// coordinate directly and returns that -- no lattice, no smoothstep between
// neighbours. This is simpler than `dynamicNoiseAt()`'s construction, and it
// is simpler because it is closer to what the patent actually describes: one
// table entry per texel, not a field sampled between entries.
//
// The hash is `np::splitmix64()` (brush/Dynamics.hpp), reused rather than
// re-derived: it is already this codebase's answer to "a fast, well-mixed,
// pure function of an integer, stable across runs and processes", the exact
// property a tiled grain field needs, and a second hash with the same
// contract would be a second thing to keep in sync with the first for no
// reason. See that header's own comment for why splitmix64 (Vigna 2015,
// public domain) was chosen over `std::hash` or a PRNG object.
//
// ==========================================================================
// 2. Where this plugs in, and what it costs `dabCoverage()`'s own contract
// ==========================================================================
//
// `brush/Deposit.hpp` says `dabCoverage()` is "the one function that decides
// what a dab looks like" and has "exactly two callers by design" precisely so
// a preview cannot drift from the deposit. Grain is deliberately kept OUT of
// that function rather than folded into it: `dabCoverage(tip, dx, dy)` takes
// only a dab-RELATIVE offset, and grain's whole point is that it is NOT
// relative to the dab -- §3 below. Threading an absolute document position
// into `dabCoverage()` would turn a two-argument geometric query into a
// function that needs to know where on the page it is being asked about,
// which every existing caller (bitmap sampling, the ellipse mapping, the dual
// tip) has no use for and would have to carry regardless.
//
// So grain is applied at the two call sites that already have (or can be
// given) an absolute integer texel coordinate, on the RESULT of
// `dabCoverage()`, the same way the selection is applied outside
// `dabCoverage()` in `depositDab()`'s own loop rather than inside it
// (brush/Deposit.hpp §4): `brush/Deposit.cpp`'s `depositDab()` loop, which
// already iterates absolute canvas texels `(x, y)` to fetch `cover` and
// `sel`, and `app/DabPreview.cpp`'s `dabPreviewTexel()`, which iterates
// absolute PREVIEW texels `(px, py)` for the identical reason. Both now call
// `grainCoverageAt()` on the coverage `dabCoverage()` already returned, before
// that coverage becomes a mass. **This is the resolution to the honest
// problem this task poses**: `dabCoverage()`'s own two-caller contract is
// preserved unchanged (it still knows nothing about position), and grain
// joins the selection as the second thing that is layered on afterward, at
// the one or two places absolute position already exists. The cost is that
// two call sites, not one, now have to remember to apply it -- exactly the
// cost the selection gate already pays, and for the identical reason: a
// preview with no document has no selection to gate with either, and it does
// not get one by being folded into `dabCoverage()`.
//
// **What that costs the preview, honestly stated.** `app/DabPreview` draws
// one dab "on empty paper" (its own header §1) with no `Document` and
// therefore no real absolute position. The convention this file adopts is
// that a preview cell's own integer texel coordinate `(px, py)` -- already
// the coordinate `dabPreviewTexel()` iterates in -- stands in for the
// document position grain would see if this dab were painted with its
// cell's own top-left corner at document `(0, 0)`. That is a stated
// convention, not a fact about where the brush will actually be used: paint
// the identical brush at a different scroll position and the real stroke's
// grain will differ from what the preview showed, exactly as it would for
// any two different positions on a real page (§3). What the convention DOES
// guarantee, and what `--selftest` checks rather than trusts, is that the
// preview and a real `depositDab()` given the SAME integer coordinates agree
// bit for bit -- `radius <= kDabPreviewFitRadius` keeps the preview's scale
// at exactly 1.0 (app/DabPreview.hpp §3), and a canvas sized and centred to
// match `dabPreviewOffset()`'s own cell geometry (`--selftest`'s dab-preview
// section already builds one this way) makes the deposit's loop variable
// `(x, y)` and the preview's `(px, py)` the identical integers.
//
// ==========================================================================
// 3. Wallpaper vs paper: the property the whole feature stands on
// ==========================================================================
//
// A grain field keyed by a coordinate relative to the DAB would look
// identical on every stroke, in every position -- a texture printed on the
// BRUSH rather than on the paper, which is wallpaper: it moves when the brush
// moves. Keyed by ABSOLUTE document position, the identical brush stroke
// picks up different peaks and valleys depending on where it lands on the
// page, exactly as a real brush dragged across a real sheet does. This is the
// one property that makes the feature real rather than decorative, it is
// exactly what "the field is indexed by page position, not by dab-local
// offset" means, and it is `--selftest`'s named, load-bearing sabotage: make
// `grainHeightAt()` ignore its `(x, y)` argument (fold it to a constant, or
// derive it from the dab centre instead of the texel) and every assertion
// that two different absolute positions get different grain must go red, or
// the field is decorative and the sabotage has found nothing.
//
// ==========================================================================
// 4. What is deliberately not here
// ==========================================================================
//
// **No `BrushTip`, no `Layer`, no ImGui, no GPU.** This module is
// `brush/Dynamics`'s own purity, restated: pure functions of integers and a
// small parameter struct, so `--selftest` can exercise the field and the
// formula directly with no document, no store and no tile in sight.
//
// **No interpolation, no multi-octave sum, no anisotropic rotation.** A
// canvas-aligned tiled hash is the whole of what a coordinate-indexed lookup
// table needs to be to make deep valleys fill and peaks get skipped; none of
// those three would change that claim, and each is a knob this file leaves
// for later rather than a gap that blocks what §3 argues for.
//
// **No per-pigment or per-brush paper.** One `GrainParams` describes one
// paper for one brush tip (`BrushTip::grain`, brush/Deposit.hpp) -- the same
// granularity `BrushState`'s other tip fields already have. A document-wide
// "this canvas is this paper regardless of which brush touches it" concept is
// a real, different feature (every brush would need to agree on one field)
// and is not this one.
namespace np {

// What a "paper" is, for this module's purposes -- generated rather than
// scanned (§0), and parameterised by the two things a procedural field can
// vary that a scanned bitmap's own pixels would otherwise fix: how often it
// repeats, and how deep its texture cuts.
struct GrainParams {
  // OFF by default, and that default is load-bearing: every existing brush,
  // every golden reference and every `--selftest` assertion written before
  // this feature existed must see the identical arithmetic it always did.
  // `grainCoverageAt()` below returns its `coverage` argument utterly
  // unchanged -- no extra floating-point operation at all, not merely one
  // that happens to be a no-op -- whenever this is false, which is what
  // makes that guarantee checkable at exactly zero tolerance rather than at
  // "close enough".
  bool enabled = false;

  // The tile period along X and Y, in document texels -- the patent's `NC`
  // and `NR`. Two fields rather than one square period because the source
  // paragraph names them separately (a real scanned sheet need not tile the
  // same distance both ways); this build's own UI control (ui/MacPaintUI.cpp)
  // happens to drive both from one slider, which is that control's choice and
  // not a constraint this struct imposes.
  //
  // 24 is this file's own pick, stated as one: no source gives a period for a
  // GENERATED field (the patent's NR/NC are a scanned table's own
  // dimensions). It sits close to the default brush radius
  // (`BrushState::radius`, 20 px) rather than well below it, deliberately: a
  // period much shorter than a typical dab would tile many times under one
  // stamp, and it would be the tiling seam a user saw rather than the paper.
  // At 24 an ordinary default-sized dab spans roughly one tile of the field.
  int32_t periodX = 24;
  int32_t periodY = 24;

  // Amplitude of the grain surface height `G` (`grainHeightAt()` below
  // returns a value in `[0, depth]`), in the same units `P` -- tip
  // coverage -- is expressed in, [0,1]. 0 is a perfectly flat sheet: every
  // `G` is 0 and `grainOverlayFraction()` degenerates to `clamp(P*S*O1, 0,
  // 1)`, texture with no bite. Higher sinks deeper valleys and taller peaks
  // relative to the tip's own coverage, which is the paper's CONTRAST.
  //
  // 0.35 is this file's own pick: high enough that a mid-coverage stroke
  // (P around 0.5-0.7, the smoothstep skirt of an ordinary soft tip) visibly
  // loses coverage at a peak without every peak reaching pure zero, low
  // enough that a fully saturated core (P == 1) still fills every valley.
  float depth = 0.35f;
  // The patent's `S`: a scale on `P` before the grain surface is subtracted
  // (§0's note on why `O1` is not a separate field here). 1.0 is the
  // identity -- `P` enters the formula exactly as `dabCoverage()` returned
  // it -- and is the natural default for the same reason `BrushTip::flow`'s
  // own comment gives for not clamping flow to [0,1]: a paper that makes a
  // fully loaded tip bite HARDER than its own coverage is a legitimate
  // "rough paper" setting, not a value this struct should refuse.
  float strength = 1.0f;
};

// G: the grain surface height at absolute document texel `(x, y)`, in
// `[0, params.depth]`. **Tiles exactly** -- `grainHeightAt(p, x, y) ==
// grainHeightAt(p, x + p.periodX, y)` for every `x`/`y`, because the two
// calls hash the identical wrapped coordinate (§1) -- and **is a pure
// function of its three arguments**: the same call returns the same float on
// every invocation, in this run and in every later one, for
// `dynamicNoiseAt()`'s own reason (brush/Dynamics.hpp): a document's paint
// must render identically wherever it is opened.
//
// A period of 0 or less is floored to 1 (a degenerate "paper" with no tiling
// at all, one texel repeated everywhere) rather than dividing by zero --
// `GrainParams` is a plain struct on state a slider can drag to its own
// floor, so an untrusted-by-construction value here gets a defined answer
// rather than undefined behaviour, the same discipline
// `brush/Deposit.cpp`'s `bitmapTipScale()` already applies to a
// zero-dimension bitmap.
float grainHeightAt(const GrainParams& params, int32_t x, int32_t y) noexcept;

// F = clamp(P*S*O1 - G, 0, 1) -- US 5,347,620's own overlay-fraction
// combination (§0), spelled with its own four letters and nothing folded in,
// so the formula stated in this file's header is the formula the code
// computes with no algebraic rearrangement between the two.
float grainOverlayFraction(float P, float S, float O1, float G) noexcept;

// The one function every caller outside this file actually calls: grain-
// modulated coverage for tip-shape coverage `coverage` (`dabCoverage()`'s own
// return value, playing `P`) at absolute document texel `(x, y)`.
//
// Returns `coverage` UNCHANGED, bit for bit, whenever `!params.enabled` --
// checked first, before any floating-point operation this function would
// otherwise perform, which is what makes "grain off is a no-op" a claim about
// the code path taken and not merely about the numbers it happens to produce
// (§ GrainParams::enabled's own comment).
//
// `O1` (§0) is fixed at 1.0 here; `params.strength` plays `S`.
float grainCoverageAt(const GrainParams& params, float coverage, int32_t x, int32_t y) noexcept;

// Whether two `GrainParams` describe the same paper. Bit equality throughout,
// not a tolerance -- `brush/Library.hpp`'s `presetMatches()` convention
// (every field here arrives from a slider or from a preset load, so two that
// should be equal are bit-equal already), and this is what that function's
// own EDITED-badge comparison calls for `BrushPreset::grain`.
bool grainParamsEqual(const GrainParams& a, const GrainParams& b) noexcept;

}  // namespace np
