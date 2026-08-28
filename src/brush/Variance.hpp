#pragma once

#include <cstdint>

#include "brush/Dynamics.hpp"

namespace np {

// brush/Variance -- Photoshop's `brVr` object, which is the same four fields
// twelve times over, resolved by one formula.
//
// **Why this type exists.** A Photoshop brush preset carries a "Control /
// Jitter / Minimum / Fade" group at twelve places: Size, Angle and Roundness
// in Shape Dynamics; Scatter and Count in Scattering; Opacity, Flow, Wetness
// and Mix in Transfer; Depth in Texture; Foreground/Background in Color
// Dynamics; and the Dual Brush's own two. Measured across the 101 presets in
// the four packs on this machine, all 800 such objects carry exactly the same
// four keys -- `bVTy`, `jitter`, `Mnm ` and `fStp` -- so there is one shape
// here, not twelve similar ones.
//
// The importer used to expand each of these into up to TWO `BrushLink`s on a
// generic source-to-target matrix, each carrying its own copy of the minimum.
// That is where audit **B6** came from: two links onto one Multiply target
// multiply, so a 30% floor expressed twice became a 9% floor. Here the minimum
// is applied ONCE, outside the product, by construction -- so B6 and its
// sibling B7 (`multiplyFloor[Roundness]` imported and never applied) are not
// fixed so much as made unrepresentable. That is the whole argument for the
// type.

// Photoshop's Control dropdown -- the `bVTy` integer, whose ordinals are the
// FILE's and not this build's choice.
//
// **Ordinals 0, 2, 3, 5 and 6 are observed** across all 101 presets measured
// (`--abr-keys`). **1, 4 and 7 are not observed anywhere** and are inference.
//
// `6 = Direction` is the one ordinal pinned to a named brush read off
// Photoshop's own panel: Kyle Webster's "Blot Bot Perfecto" carries
// `angleDynamics bVTy = 6`, and Photoshop's Shape Dynamics panel shows that
// brush's Angle Control as **Direction**. This pair was committed backwards
// once and shipped through a full green suite, because it was read off an enum
// that looked orderly rather than off the application that writes the files.
// `7 = InitialDirection` is placed by elimination and has never been seen.
enum class VarianceControl : uint8_t {
  Off = 0,
  Fade = 1,
  PenPressure = 2,
  PenTilt = 3,
  StylusWheel = 4,
  Rotation = 5,
  Direction = 6,
  InitialDirection = 7,
};

const char* varianceControlName(VarianceControl control) noexcept;

// The source a control reads, or false when this build has none.
//
// `StylusWheel` is the only permanent false: it is an airbrush wheel, and SDL
// reports no such axis. `Rotation` takes Barrel and `StylusWheel` does not,
// even though both are "the pen twisted", because barrel rotation IS a real
// SDL axis (`SDL_PEN_AXIS_ROTATION`) and mapping both onto it would put two
// different physical inputs in one place and silently make one of them a lie.
bool varianceControlSource(VarianceControl control, DynamicSource& out) noexcept;

struct Variance {
  VarianceControl control = VarianceControl::Off;
  // `jitter`, as a fraction: the file's 0-100% divided by 100.
  float jitter = 0.0f;
  // `Mnm `, likewise -- the floor the control fades toward, applied ONCE. This
  // field was read and then silently discarded by the previous importer.
  float minimum = 0.0f;
  // `fStp`, the number of DABS a Fade takes to reach `minimum`. Meaningful
  // only when `control == Fade`. Measured range across the four packs is
  // 1..25, not the constant 25 this was first believed to be.
  int32_t fadeSteps = 25;
  // Whether the key was present at all, as opposed to defaulted. Carried so
  // the import report can tell "the file said Off" from "the file said
  // nothing", which are different facts about a brush.
  bool present = false;
};

// Which of the twelve sites a resolution is for.
//
// **This is a salt, and it is load-bearing.** Every site draws from the same
// per-dab random stream, so without a per-site salt one dab's Size, Angle and
// Scatter jitters would all draw the SAME number and move in lockstep -- which
// reads as one coherent wobble rather than as three independent variations,
// and is the difference between a brush that looks hand-made and one that
// looks like it is vibrating. Named here rather than passed as loose integers
// so two sites cannot silently collide.
enum class VarianceSite : uint32_t {
  Size = 1,
  Angle = 2,
  Roundness = 3,
  Scatter = 4,
  Count = 5,
  Opacity = 6,
  Flow = 7,
  Wetness = 8,
  Mix = 9,
  TextureDepth = 10,
  ForegroundBackground = 11,
  DualScatter = 12,
  DualCount = 13,
};

// A MULTIPLICATIVE resolution, in `[minimum, 1]` -- for Size, Roundness, Flow,
// Opacity, Texture Depth and Count.
//
// The formula, stated once because it is the whole module:
//
//     m  = clamp(minimum, 0, 1)
//     rj = 1 - jitter * (1 - random)      // random in [0,1], per dab per site
//     c  = control's axis, or 1 when Off or unavailable
//     result = m + (1 - m) * rj * c
//
// **`m` is outside the product.** That is what makes a 30% minimum a 30% floor
// no matter how many things are varying, and it is the structural half of
// audit B6.
//
// **An unavailable device contributes the target's IDENTITY, not its floor** --
// `c = 1`, exactly as if the control were Off. This is audit **B7**, whose
// symptom was `Kyle's Spatter Brushes - Supreme Spatter & Texture` painting
// exactly zero pixels with a mouse because its Tilt-driven size resolved to
// its floor of 0. `DynamicInputs`' own `hasTilt`/`hasBarrel`/`hasPressure`
// flags are what this reads, so the fix is inherited rather than re-derived.
//
// **The formula is a synthesis, not a citation.** Photoshop publishes no
// definition of how Control, Jitter and Minimum compose. What IS grounded is
// each part in isolation: Minimum is a floor (the panel labels it one), Jitter
// is a per-dab random attenuation, and Control scales by a device axis.
// Combining them multiplicatively with the floor applied last is the reading
// that makes each panel control do what its own label says without any of them
// being able to cancel another. Compare against Photoshop before trusting the
// interaction of two at once.
float varianceScale(const Variance& v, const DynamicInputs& in, uint64_t seed, uint32_t dabIndex,
                    VarianceSite site) noexcept;

// An ADDITIVE resolution, in `[-span, +span]` -- for Angle and Scatter, whose
// panels express a jitter as a fraction of a full range rather than as a
// multiplier. `span` is that full range in the target's own units: 180 degrees
// for Angle, the tip's radius count for Scatter.
//
// The control contributes a DIRECTION here rather than a magnitude: a
// Direction-controlled Angle wants the stroke's own heading, not the heading
// scaled by something. So `control` sets the base offset and `jitter` adds a
// symmetric random spread around it.
float varianceOffset(const Variance& v, const DynamicInputs& in, float span, uint64_t seed,
                     uint32_t dabIndex, VarianceSite site) noexcept;

// Whether this variance does anything at all -- absent, Off, and with no
// jitter or floor. Used by the import report to tell a brush that carries the
// key from one that means something by it.
bool varianceIsInert(const Variance& v) noexcept;

}  // namespace np
