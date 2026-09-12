#include "core/VectorShape.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/PathFlatten.hpp"

namespace np {
namespace {

// FNV-1a, 64-bit. Chosen for being three lines and dependency-free rather
// than for any distribution property: this hash guards a cache, so a
// collision costs a missed re-rasterisation and nothing worse would be
// acceptable at any strength -- which is why the inputs below are exhaustive
// rather than sampled.
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

inline void hashBytes(uint64_t& h, const void* p, size_t n) noexcept {
  const unsigned char* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= kFnvPrime;
  }
}

inline void hashF32(uint64_t& h, float v) noexcept {
  // By bit pattern, so the hash is exact. A denormal or a -0.0 that differs
  // from +0.0 in the bits re-rasterises; that is the safe direction.
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  hashBytes(h, &bits, sizeof(bits));
}

inline void hashU64(uint64_t& h, uint64_t v) noexcept { hashBytes(h, &v, sizeof(v)); }

void hashPoint(uint64_t& h, const PathPoint& p) noexcept {
  hashF32(h, p.x);
  hashF32(h, p.y);
}

void hashPath(uint64_t& h, const Path& path) noexcept {
  hashU64(h, static_cast<uint64_t>(path.rule));
  hashU64(h, path.subpaths.size());
  for (const SubPath& sub : path.subpaths) {
    hashU64(h, sub.anchors.size());
    hashU64(h, sub.closed ? 1u : 0u);
    for (const Anchor& a : sub.anchors) {
      hashPoint(h, a.pt);
      hashPoint(h, a.in);
      hashPoint(h, a.out);
      // `smooth` is an editor hint and changes no pixel, but it is cheap and
      // including it keeps this a hash of the SHAPE rather than of a subset
      // someone has to keep in step with the renderer.
      hashU64(h, a.smooth ? 1u : 0u);
    }
  }
}

// The gradient a paint references, hashed IN PLACE of the index rather than
// beside it. The index is a position in a table that travels with the
// document, so hashing the number would make two documents whose tables differ
// hash the same -- and hashing the number as WELL would make a document
// re-rasterise when an unrelated gradient was inserted before this one, which
// is correct but wasteful. An out-of-range index hashes as the "no gradient"
// marker, which is what it paints as.
void hashGradient(uint64_t& h, const GradientDef& g) noexcept {
  hashU64(h, static_cast<uint64_t>(g.geometry.kind));
  hashU64(h, static_cast<uint64_t>(g.geometry.spread));
  hashF32(h, g.geometry.x0);
  hashF32(h, g.geometry.y0);
  hashF32(h, g.geometry.x1);
  hashF32(h, g.geometry.y1);
  hashU64(h, g.stops.colorStops.size());
  for (const ColorStop& c : g.stops.colorStops) {
    hashF32(h, c.position);
    for (float v : c.color) hashF32(h, v);
    hashF32(h, c.midpoint);
  }
  hashU64(h, g.stops.opacityStops.size());
  for (const OpacityStop& o : g.stops.opacityStops) {
    hashF32(h, o.position);
    hashF32(h, o.opacity);
    hashF32(h, o.midpoint);
  }
  // The name changes no pixel, and is hashed for `Anchor::smooth`'s reason:
  // this is a hash of the CONTENT, not of a renderer-visible subset somebody
  // has to keep in step.
  hashBytes(h, g.name.data(), g.name.size());
}

void hashPaint(uint64_t& h, const Paint& p, const GradientTable& gradients) noexcept {
  hashU64(h, p.on ? 1u : 0u);
  for (float c : p.rgba) hashF32(h, c);
  hashU64(h, static_cast<uint64_t>(p.kind));
  if (p.kind == PaintKind::Gradient && p.gradient < gradients.size()) {
    hashU64(h, 1u);
    hashGradient(h, gradients[p.gradient]);
  } else {
    hashU64(h, 0u);
  }
}

}  // namespace

uint64_t vectorContentHash(const std::vector<VectorShape>& shapes,
                           const GradientTable& gradients) noexcept {
  uint64_t h = kFnvOffset;
  hashU64(h, shapes.size());
  for (const VectorShape& s : shapes) {
    hashPath(h, s.path);
    hashPaint(h, s.fill, gradients);
    hashPaint(h, s.stroke, gradients);
    // Stroke style, exhaustively -- a dash offset change is a visible change.
    hashF32(h, s.strokeStyle.width);
    hashU64(h, static_cast<uint64_t>(s.strokeStyle.cap));
    hashU64(h, static_cast<uint64_t>(s.strokeStyle.join));
    hashF32(h, s.strokeStyle.miterLimit);
    hashU64(h, s.strokeStyle.dashes.size());
    for (float d : s.strokeStyle.dashes) hashF32(h, d);
    hashF32(h, s.strokeStyle.dashOffset);

    hashU64(h, s.clip.has_value() ? 1u : 0u);
    if (s.clip) hashPath(h, *s.clip);

    // The pivot changes no pixel either, but it is document data that the
    // manipulator reads, and a cache keyed on a hash that ignored it would
    // let a pivot move be lost on the next rebuild.
    hashU64(h, s.pivot.has_value() ? 1u : 0u);
    if (s.pivot) hashPoint(h, *s.pivot);
  }
  return h;
}

PathBounds vectorShapesBounds(const std::vector<VectorShape>& shapes) noexcept {
  PathBounds out;
  auto merge = [&out](const PathBounds& b, float outset) {
    if (!b.valid) return;
    const float x0 = b.minX - outset, y0 = b.minY - outset;
    const float x1 = b.maxX + outset, y1 = b.maxY + outset;
    if (!out.valid) {
      out = PathBounds{true, x0, y0, x1, y1};
      return;
    }
    out.minX = std::min(out.minX, x0);
    out.minY = std::min(out.minY, y0);
    out.maxX = std::max(out.maxX, x1);
    out.maxY = std::max(out.maxY, y1);
  };

  for (const VectorShape& s : shapes) {
    const PathBounds b = pathTightBounds(s.path);
    if (!b.valid) continue;

    if (s.fill.on) merge(b, 0.0f);

    if (s.stroke.on && s.strokeStyle.width > 0.0f) {
      // Half the width reaches the stroke's own edge. A MITRE join reaches
      // further -- up to `miterLimit` times the half width by the limit's own
      // definition -- so the outset has to allow for it or a mitred spike is
      // clipped at the layer's edge. Bevel and round never exceed the half
      // width, but using the same conservative figure for all three keeps
      // this from depending on a join style that an edit can change without
      // touching the bounds.
      const float h = s.strokeStyle.width * 0.5f;
      const float limit = std::max(1.0f, s.strokeStyle.miterLimit);
      merge(b, (s.strokeStyle.join == LineJoin::Miter) ? h * limit : h);
    }

    // A shape with neither fill nor stroke still occupies space for
    // selection and for the manipulator's box, so it contributes its bare
    // bounds rather than nothing.
    if (!s.fill.on && !s.stroke.on) merge(b, 0.0f);
  }

  // A clip can only ever REMOVE coverage, so it never enlarges these bounds
  // and is deliberately not merged in.
  return out;
}

}  // namespace np
