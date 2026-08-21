#pragma once

#include <array>
#include <type_traits>

// core/Premultiply -- the un-premultiply guard, in the one place that owns it.
//
// ==========================================================================
// Why this file exists, and why it did not exist until now
// ==========================================================================
//
// Storage in this codebase is **premultiplied**: `rgb *= a` is applied on the
// write side in io/ImageIO.cpp (DESIGN-imaging.md §2's storage policy), and
// every read boundary that hands a colour to something which expects straight
// alpha has to undo it. That is the same write/read split io/ImageDecode.hpp's
// header comment documents for the opposite (decode) direction. The undo is
// three lines, and three lines is exactly the size at which a function gets
// retyped rather than shared. It was retyped four times:
//
//   * core/Probe.cpp        -- the eyedropper's read boundary
//   * io/Export.cpp         -- the flattener's read boundary
//   * ops/PointOps.cpp      -- grade in straight space, re-premultiply after
//   * ops/Resample.cpp      -- filter in premultiplied space, undo at the end
//
// io/Export.cpp's copy carried the note *"It is duplicated rather than hoisted
// into a shared header purely because promoting it would mean editing
// core/Probe, which is outside this step's scope; if a third caller appears,
// that promotion is the right move."* ops/Resample.cpp's copy then recorded
// that it **was** the third caller and deferred anyway. ui/DocumentTexture is
// the fifth. The promotion is this file, and all four notes are deleted rather
// than left to keep predicting an event that has happened.
//
// --- What the guard actually is -------------------------------------------
//
//     a <= 0  ->  {0, 0, 0, 0}
//
// Fully transparent. RGB is arbitrary under premultiplied alpha -- nothing
// stored it and nothing can recover it -- so the result is *defined* rather
// than left to a division by zero, and the value chosen is the one
// core/Tile's own value-initialization already gives an untouched texel. That
// makes "never written" and "written transparent" the same colour at every
// boundary, which is what stops an eyedropper and an export of the same pixel
// from reporting different answers.
//
// `<= 0` and not `== 0`: a negative alpha is not reachable from any writer in
// this codebase, but a `.npaint` is a file and a file can say anything. A
// negative divisor would produce negated colour rather than an obvious
// failure, and that is the kind of thing that survives review.
//
// --- Why it is a template, and not two functions --------------------------
//
// ops/Resample.cpp accumulates its filter weights in **double** and only
// narrows to float when it stores (see that file's own note on why the weights
// must be double: normalised float weights sum to 1 only to within n * 6e-8,
// and an 8x reduction leaves a fully opaque image at alpha 0.999996, which
// io/Export then refuses to write as a JPEG). Its divide therefore has to
// happen in double. Every other caller has floats and must not pay a widening
// round trip. A `float`-only shared home would force one of the two to
// convert, which is precisely the reason this promotion was worth deferring
// until there was a shape that serves both -- so `T` is deduced from the
// caller's own accumulator type and the arithmetic happens there.
//
// `std::is_floating_point_v` rather than accepting anything divisible:
// instantiating this on an integral type would make `a <= 0` mean something
// different (truncation, not transparency) and the static_assert says so at
// the call site instead of silently compiling.
namespace np {

template <typename T>
[[nodiscard]] constexpr std::array<T, 4> unpremultiply(
    const std::array<T, 4>& premultiplied) noexcept {
  static_assert(std::is_floating_point_v<T>,
                "core/Premultiply: alpha division is floating-point; an integral T would make "
                "`a <= 0` mean truncation rather than transparency");
  const T a = premultiplied[3];
  if (a <= T(0)) return {T(0), T(0), T(0), T(0)};
  return {premultiplied[0] / a, premultiplied[1] / a, premultiplied[2] / a, a};
}

}  // namespace np
