#pragma once

#include <vector>

#include "app/ViewTransform.hpp"
#include "ops/RadialBlur.hpp"

// app/RadialBlurHandles -- the on-canvas handles Filter > Radial Blur shows
// while its dialog is open: a centre disc, and one amount handle on a guide a
// fixed number of screen pixels from the centre. Geometry and drag maths only,
// through the canvas's own ViewTransform, so the selftest drives exactly what
// the canvas draws and the dialog records.
//
// The guide shows the sweep the engine really takes at that radius: Spin's
// arc runs +/- amount/2, Zoom's segment runs 1 -/+ amount times the radius.
// The engine is symmetric in the sign of `amount`, so the handles set its
// magnitude and keep whatever sign it already had.
namespace np {

enum class RadialBlurHandle { None, Center, Amount };

inline constexpr float kRadialBlurGuidePx = 72.0f;
inline constexpr float kRadialBlurPickPx = 9.0f;
// The dialog's slider limits; a handle drag clamps to the same.
inline constexpr float kRadialBlurMaxSpinDegrees = 90.0f;
inline constexpr float kRadialBlurMaxZoom = 1.0f;

// All screen space.
struct RadialBlurHandleShape {
  Vec2 center;
  Vec2 amountHandle;
  Vec2 rest;                // where the amount handle sits at amount 0
  std::vector<Vec2> guide;  // Spin: the swept arc; Zoom: the scale segment
};

RadialBlurHandleShape radialBlurHandleShape(const RadialBlurParams& params,
                                            const ViewTransform& view);

// The centre wins where both are in reach.
RadialBlurHandle radialBlurHandleAt(const RadialBlurHandleShape& shape, Vec2 pointer);

struct RadialBlurHandleDrag {
  RadialBlurHandle handle = RadialBlurHandle::None;
  Vec2 grabOffset;  // canvas: centre minus the pointer at press, so the centre does not jump
  float amountSign = 1.0f;
};

RadialBlurHandleDrag beginRadialBlurHandleDrag(RadialBlurHandle handle,
                                               const RadialBlurParams& params,
                                               const ViewTransform& view, Vec2 pointer);

// The centre lands on whole texels inside the document; Spin on whole
// degrees and Zoom on hundredths, the precision the dialog shows.
void updateRadialBlurHandleDrag(const RadialBlurHandleDrag& drag, const ViewTransform& view,
                                Vec2 pointer, float docW, float docH, RadialBlurParams* params);

}  // namespace np
