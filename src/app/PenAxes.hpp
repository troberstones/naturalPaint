#pragma once

namespace np {

// SDL3's raw pen axes -> the normalised [0,1] sources brush/Dynamics.hpp
// consumes.
//
// Pure math, extracted for the same reason app/ControlsLayout.hpp and
// app/Snapping.hpp are: the conversion is the part with an answer that can be
// wrong, and inside main.cpp's event switch nothing could reach it.
//
// **SDL reports tilt as two independent angles, x and y, each in degrees over
// [-90,90] -- not as an altitude and an azimuth.** Those are the two numbers a
// tablet actually measures, and the pair the DYNAMICS matrix does NOT want:
// its TILT row means "how far the pen is leaning" and its AZIMUTH row means
// "which way", which are the polar form. Converting is this file's whole job,
// and it is not a rotation of axes -- see penTiltNormalised() for why the
// tangents are involved.

// How far the pen leans away from the page normal, as [0,1] over 0-90 degrees:
// 0 is upright, 1 is flat against the surface.
//
// The projection onto each axis is a TANGENT, not the angle itself. A pen
// leaning 45 degrees in x and 45 in y is not leaning 63.6 degrees
// (sqrt(45^2+45^2)) -- it is leaning 54.7, because the tilts compose as
// direction cosines. Taking the naive hypotenuse of the two angles overstates
// every diagonal lean, worst at exactly 45/45, which is the most common wrist
// position for a right-handed painter shading. So: tan each, take the
// hypotenuse of those, and go back through atan.
//
// Inputs are clamped to +/-89.9 degrees before the tangent, since tan(90) is
// infinite and SDL's range is inclusive.
float penTiltNormalised(float xTiltDeg, float yTiltDeg) noexcept;

// Which way the pen is leaning, as [0,1] over a full turn measured
// anticlockwise from +x. Undefined when the pen is upright, where it returns
// 0 rather than a stale or NaN angle -- an upright pen has no azimuth, and
// the matrix's gutter showing a steady 0 is the honest rendering of that.
float penAzimuthNormalised(float xTiltDeg, float yTiltDeg) noexcept;

// Barrel rotation (SDL_PEN_AXIS_ROTATION, degrees over [-180,180]) as [0,1],
// so that the pen's REST orientation lands at 0.5 rather than at an end.
// brush/Dynamics.hpp's gutter maps it back through `v*360 - 180`, so the pair
// round-trips; a rest position at 0 would make half the range unreachable
// from a curve whose domain starts there.
float penBarrelNormalised(float rotationDeg) noexcept;

}  // namespace np
