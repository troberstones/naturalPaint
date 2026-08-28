#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "brush/CoverageBlend.hpp"
#include "brush/Deposit.hpp"
#include "brush/Variance.hpp"

namespace np {

// brush/BrushModel -- a brush shaped like Photoshop's Brush Settings panel,
// which is the shape the files are actually written in.
//
// **Why this replaces the 10 x 12 link matrix.** The matrix
// (brush/Dynamics.hpp) can express an arbitrary source-to-target relationship
// with a curve, which is strictly more general than what Photoshop writes --
// and the generality is not being used. Measured with `--abr-report`, all 51
// presets of art_markers.abr between them occupy **9 of the 120 cells**:
// Pressure, Tilt and Random against Size, Angle, Roundness and Scatter. What
// the matrix cannot express, because it has no place to put them, is Texture,
// Transfer, Scatter Count, the Dual Brush's own cadence and the tool options
// -- five whole panels, switched on by 84, 69, 68, 66 and 101 of the 101
// presets measured. So the matrix is 120 cells expressing less than eight
// named panels do, while dropping most of what the files carry.
//
// The matrix is **shelved, not deleted** -- see brush/Dynamics.hpp -- so the
// curve editor and the two non-Photoshop easings survive and can be layered
// back on top of this if testing wants them.
//
// **Units are Photoshop's, converted exactly once.** `diameterPx` is a
// diameter and `spacingPercent` is a percentage OF that diameter, because that
// is what the file says; the conversion into `BrushTip`'s radius-relative
// units happens at one boundary. Two of this project's shipped defects were
// radius/diameter confusions (`abrSpacingToRadii`, `abrScatterFractionToRadii`
// in io/AbrBrushes.hpp), and both existed because the conversion was scattered
// through the importer rather than done at a named edge.
//
// **The solver route does not read any of this.** shaders/splat.wgsl is a
// round segment-distance smoothstep with no bitmap tip, no roundness, no
// angle, no grain, no scatter and no dual brush, so a brush painted on the
// solver canvas is not the brush this model describes. That divergence
// predates this file and this file widens it considerably; saying so once,
// here, is the alternative to widening it silently.

// A reference into the dab library: the id is what persists, the bitmap is
// what paints.
//
// **The id is why a duplicated brush keeps its tip.** A sampled tip used to
// travel only as a `shared_ptr` hanging off a preset, with no slot for it in
// `user-presets.txt` -- so duplicating a sampled-tip brush and saving it
// reloaded next launch as a round dab (brush/Library.hpp's own note). An id is
// a slot, and it is a short one.
struct DabRef {
  std::string id;  // "abr:<uuid>", "file:<relpath>", "gbr:<relpath>", "gih:<relpath>#<n>"
  std::shared_ptr<const BrushTipBitmap> bitmap;  // resolved; never persisted

  bool empty() const noexcept { return id.empty() && bitmap == nullptr; }
};

// A reference into the pattern library. `id` is the 36-character UUID a
// pattern record carries, which is the same text a brush's `Txtr` descriptor
// puts in `Idnt` -- verified against real packs, so the join needs nothing
// invented in between.
struct PatternRef {
  std::string id;
  std::string name;  // carried so a missing pattern can be named, not just missed

  bool empty() const noexcept { return id.empty(); }
};


// Photoshop's "Brush Tip Shape".
struct PsTipShape {
  DabRef dab;                  // empty for a computed (procedural) tip
  float diameterPx = 40.0f;    // `Dmtr`. #Pxl on all 101 presets measured
  float angleDeg = 0.0f;       // `Angl`. Range measured -46..180, so signed
  float roundness = 1.0f;      // `Rndn` / 100
  float spacingPercent = 25.0f;  // `Spcn`, a percentage OF THE DIAMETER
  float hardness = 1.0f;       // `Hrdn`. Present on 3 of 101 -- computed tips only
  bool spacingEnabled = true;  // `Intr`. True on all 101, so the meaning is INFERRED
  bool flipX = false;          // `flipX` on the tip object
  bool flipY = false;
  bool computed = false;       // classID: computedBrush (3 of 101) vs sampledBrush (98)
};

// "Shape Dynamics" -- `useTipDynamics`, on for 101 of 101.
struct PsShapeDynamics {
  bool enabled = false;
  Variance size;       // `szVr`, with `minimumDiameter` folded into its minimum
  Variance angle;      // `angleDynamics`
  Variance roundness;  // `roundnessDynamics`, with `minimumRoundness` folded in
  bool flipXJitter = false;  // top-level `flipX`, on for 14 of 101
  bool flipYJitter = false;  // top-level `flipY`, on for 12
  bool brushProjection = false;  // off on all 101
  float tiltScale = 0.0f;        // `tiltScale` / 100. Measured 0, 30, 120, 200%
};

// "Scattering" -- `useScatter`, on for 68 of 101. Also the Dual Brush's own.
struct PsScatter {
  bool enabled = false;
  Variance scatter;  // `scatterDynamics`, a jitter as a fraction of the DIAMETER
  bool bothAxes = false;
  // `Cnt `: how many dabs land at each position. Measured 1 x21, 2 x28, 3 x18,
  // 5 x1 -- so 47 of the 68 scattering presets want more than one, and every
  // one of them has been painting a single dab.
  int32_t count = 1;
  Variance countJitter;  // `countDynamics`
};

// "Texture" -- `useTexture`, on for 84 of 101, and imported by nothing until
// now. The pattern it names lives in `patt`, which is 99% of the file.
struct PsTexture {
  bool enabled = false;
  PatternRef pattern;  // `Txtr` = { `Nm  `, `Idnt` }
  bool invert = false;         // `InvT`, on for 40 of the 84
  float scalePercent = 100.0f; // `textureScale`
  float depth = 1.0f;          // `textureDepth` / 100
  float minimumDepth = 0.0f;   // `minimumDepth` / 100
  Variance depthJitter;        // `textureDepthDynamics`
  CoverageBlend blend = CoverageBlend::Height;  // `textureBlendMode`
  float brightness = 0.0f;     // `textureBrightness`, when present
  float contrast = 0.0f;       // `textureContrast`, when present
  bool eachTip = false;        // `TxtC` "Texture Each Tip", on for 34 of the 84
  // `protectTexture`, on for 35 of the 84. It means every brush shares one
  // canvas-anchored texture origin -- which is what `grainCoverageAt()` already
  // does, since it samples at absolute canvas coordinates. So `true` is
  // honoured for free and `false` is the case this build cannot express.
  bool protectTexture = false;
};

// "Dual Brush" -- `useDualBrush`, on for 66 of 101.
struct PsDualBrush {
  bool enabled = false;
  PsTipShape tip;
  CoverageBlend blend = CoverageBlend::Multiply;  // `BlnM`
  PsScatter scatter;  // its own `useScatter`/`Cnt `/`bothAxes`/`*Dynamics`
  bool flip = false;  // `Flip`
};

// "Color Dynamics" -- `useColorDynamics`, on for 1 of 101. Imported for
// completeness; the low count is itself the finding.
struct PsColorDynamics {
  bool enabled = false;
  bool perTip = false;      // `colorDynamicsPerTip`
  Variance foregroundBackground;  // `clVr`
  float hueJitter = 0.0f;         // `H   `
  float saturationJitter = 0.0f;  // `Strt`
  float brightnessJitter = 0.0f;  // `Brgh`
  float purity = 0.0f;            // `purity`
};

// "Transfer" -- `usePaintDynamics`, on for 69 of 101, imported by nothing
// until now.
struct PsTransfer {
  bool enabled = false;
  Variance opacity;  // `opVr`
  Variance flow;     // `prVr`
  // `wtVr` and `mxVr`. Parsed and carried; there is no engine target for
  // either -- a Pigment texel's seven channels do not include a water value,
  // which is brush/Dynamics.hpp's own standing reason for refusing Wetness.
  Variance wetness;
  Variance mix;
};

// The options bar state Photoshop saves WITH the preset, in `toolOptions`.
//
// **Present on all 101 presets and read by nothing.** This is where the
// artist's actual Flow lives -- measured 9..100%, most between 15 and 40 --
// while every imported brush has been painting at this build's default of
// 0.35. It is also where the blend mode lives: Normal 61, Darken 20, Multiply
// 16, Linear Burn 2, Dissolve 1, so 40% of these brushes are not Normal-mode
// brushes at all. `Kyle's AM - 1 Basic` and `Kyle's AM - 1 Basic Darken`
// import to identical rows today for exactly that reason.
struct PsToolOptions {
  std::string blendMode;  // `Md  `, kept as the file's own id, mapped at the edge
  float opacity = 1.0f;   // `Opct` / 100
  float flow = 1.0f;      // `flow` / 100
  bool smoothing = true;  // `smoothing`, true on 94 of 101
  bool pressureOverridesSize = false;
  bool pressureOverridesOpacity = false;
  bool useLegacy = false;
  // The tool preset's OWN overrides, layered on top of the brush's dynamics.
  // Parsed and carried, deliberately not applied: how they compose with the
  // brush's own `szVr`/`opVr` is not determinable from the file, and guessing
  // at a composition rule is the shape of mistake this project has already
  // shipped twice.
  Variance sizeOverride, opacityOverride, flowOverride, colorOverride;
};

struct BrushModel {
  PsTipShape tip;
  PsShapeDynamics shape;
  PsScatter scatter;
  PsTexture texture;
  PsDualBrush dual;
  PsColorDynamics color;
  PsTransfer transfer;
  PsToolOptions options;

  // The checkbox tail of Photoshop's panel. All four are parsed and none is
  // applied; each is refused for its own stated reason in io/AbrBrushes.cpp.
  bool noise = false;     // `Nose`, on for 14 of 101
  bool wetEdges = false;  // `Wtdg`, on for 0 of 101
  // `Rpt `, on for 50 of 101. **Believed to be Build-up (airbrush) by
  // elimination** against Photoshop's checkbox tail -- `Nose` is Noise, `Wtdg`
  // is Wet Edges, `protectTexture` is Protect Texture and `smoothing` lives in
  // `toolOptions`, which leaves Build-up as the one unaccounted for. That is
  // inference, not a reading, and it is exactly the shape of guess that
  // produced the `AbrControl` 6/7 defect. Do not ship a behaviour change on
  // it: open a named brush in Photoshop and read the panel first.
  bool airbrush = false;
  bool brushPose = false;  // `useBrushPose`, on for 0 of 101

  // naturalPaint's own two, which Photoshop has no concept of and which must
  // not be smuggled into a Photoshop-named section above.
  float load = 0.9f;     // pigment concentration
  float wetness = 1.3f;  // water
};

}  // namespace np
