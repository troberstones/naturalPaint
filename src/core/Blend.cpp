#include "core/Blend.hpp"

#include <cmath>

namespace np {
namespace {

// The table. Order is the dropdown's order (see allBlendModes()).
//
// Aggregate-initialised with every field named by position, so adding a mode
// without giving it a `space` (PRD B7) or a `compositesPixels` answer does not
// compile. That is deliberate and it is the mechanism B7 asks for: the
// classification is not a comment beside the mode, it is a field the mode
// cannot exist without.
constexpr BlendModeInfo kModes[] = {
    // mode                 name         label       space                         texels  latents  pigmentPair
    {BlendMode::Normal,     "normal",    "Normal",   BlendSpace::LinearLight,      true,   false,   false},
    {BlendMode::Plus,       "plus",      "Plus",     BlendSpace::LinearLight,      true,   false,   false},
    {BlendMode::Multiply,   "multiply",  "Multiply", BlendSpace::LinearLight,      true,   false,   false},
    // The one display-referred member of the set. core/Blend.hpp derives why:
    // `cs + cb - cs*cb` stops being monotone above 1.0, so it only means
    // anything in an encoding where 1.0 is white. --selftest proves the
    // misbehaviour numerically.
    {BlendMode::Screen,     "screen",    "Screen",   BlendSpace::DisplayReferred,  true,   false,   false},
    {BlendMode::Min,        "min",       "Min",      BlendSpace::LinearLight,      true,   false,   false},
    {BlendMode::Max,        "max",       "Max",      BlendSpace::LinearLight,      true,   false,   false},
    // Composited at the **layer** level and never at the texel level: `Mix` is
    // a lerp of pigment latents, and an RGBA texel has none, so
    // `compositesPixels` is false permanently rather than pending. The layer
    // half landed at PLAN.md Phase 5 step 3, when Pigment layers gained latent
    // tiles -- core/Composite's Pigment-pair branch calls `mixLatents()`.
    // Linear light: a Kubelka-Munk mix has no reference white in it at all.
    {BlendMode::Mix,        "mix",       "Mix",      BlendSpace::LinearLight,      false,  true,    true},
};

constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

static_assert(kModeCount == 7,
              "every BlendMode enumerator needs a row in kModes -- blendModeInfo() indexes "
              "this table by enumerator value");

// blendModeInfo() indexes by enumerator, so the table's row order is load-
// bearing: a row inserted in the wrong place would hand out another mode's
// name and another mode's PRD B7 classification. Checked at compile time
// rather than trusted to the comment above the table.
constexpr bool tableIsIndexedByEnumerator() {
  for (size_t i = 0; i < kModeCount; ++i)
    if (static_cast<size_t>(kModes[i].mode) != i) return false;
  return true;
}
static_assert(tableIsIndexedByEnumerator(),
              "kModes must be in BlendMode enumerator order -- blendModeInfo() indexes it");

}  // namespace

std::span<const BlendModeInfo> allBlendModes() noexcept {
  return std::span<const BlendModeInfo>(kModes, kModeCount);
}

const BlendModeInfo& blendModeInfo(BlendMode mode) noexcept {
  // Indexed rather than searched, and the table's row order is asserted to
  // match the enumerator order below, so a row added in the wrong place is a
  // startup-free, test-visible failure rather than a silently wrong label.
  const size_t i = static_cast<size_t>(mode);
  return kModes[i < kModeCount ? i : 0];
}

const char* blendModeName(BlendMode mode) noexcept { return blendModeInfo(mode).name; }

std::optional<BlendMode> blendModeFromName(std::string_view name) noexcept {
  for (const BlendModeInfo& info : kModes)
    if (name == info.name) return info.mode;
  return std::nullopt;
}

bool blendIsImplemented(std::string_view blend) noexcept {
  const std::optional<BlendMode> mode = blendModeFromName(blend);
  if (!mode.has_value()) return false;
  const BlendModeInfo& info = blendModeInfo(*mode);
  return info.compositesPixels || info.compositesLatents;
}

MixPairing mixPairing(const Document& doc) {
  MixPairing p;
  p.mixedWithBelow.assign(doc.layers.size(), false);
  p.consumedByAbove.assign(doc.layers.size(), false);
  // Greedy from the bottom. `blendModeAvailableForLayer()` is the single
  // implementation of PRD L5 and is asked here rather than re-derived, so the
  // set of layers that may mix and the set the dropdown offers `Mix` to are
  // the same set by construction.
  for (size_t i = 1; i < doc.layers.size(); ++i) {
    if (doc.layers[i].blend != blendModeName(BlendMode::Mix)) continue;
    if (!blendModeAvailableForLayer(doc, i, BlendMode::Mix)) continue;
    // A layer can belong to at most **one** pair, in one role. Both
    // exclusions are needed and they are different situations: the layer
    // beneath is already the *lower* half of a pair below it (so it has been
    // consumed), or it is already the *upper* half of one (so mixing into it
    // would be mixing into a mixed result, which is the chain this build does
    // not associate). See core/Composite.hpp on why that is a stated limit
    // rather than something to fix by re-associating.
    if (p.consumedByAbove[i - 1] || p.mixedWithBelow[i - 1]) continue;
    p.mixedWithBelow[i] = true;
    p.consumedByAbove[i - 1] = true;
  }
  return p;
}

bool blendIsImplementedForLayer(const Document& doc, size_t layerIndex) {
  if (layerIndex >= doc.layers.size()) return false;
  const Layer& layer = doc.layers[layerIndex];
  const std::optional<BlendMode> mode = blendModeFromName(layer.blend);
  if (!mode.has_value()) return false;
  const BlendModeInfo& info = blendModeInfo(*mode);
  if (info.compositesPixels) return true;
  if (!info.compositesLatents) return false;
  // The only latent-level mode. Whether it can be honoured is a property of
  // where the layer sits, not of the name.
  return mixPairing(doc).mixedWithBelow[layerIndex];
}

bool blendModeAvailableForLayer(const Document& doc, size_t layerIndex,
                                BlendMode mode) noexcept {
  if (layerIndex >= doc.layers.size()) return false;
  if (!blendModeInfo(mode).pigmentPairOnly) return true;
  // PRD L5 / docs/ui.md §3.4: both this layer and the one beneath it must be
  // Pigment layers. `layers` is bottom-to-top, so "beneath" is index - 1 and
  // the bottom layer has nothing beneath it.
  if (layerIndex == 0) return false;
  return doc.layers[layerIndex].kind == LayerKind::Pigment &&
         doc.layers[layerIndex - 1].kind == LayerKind::Pigment;
}

std::array<float, 4> compositeOver(const std::array<float, 4>& src,
                                   const std::array<float, 4>& dst) noexcept {
  const float inv = 1.0f - src[3];
  return {src[0] + dst[0] * inv, src[1] + dst[1] * inv, src[2] + dst[2] * inv,
          src[3] + dst[3] * inv};
}

std::array<float, 4> blendPixel(BlendMode mode, const std::array<float, 4>& src,
                                const std::array<float, 4>& dst) noexcept {
  // `over` first and by itself: it must stay bit-for-bit the function step 1
  // shipped, because a non-overlapping document's byte-identity to the plain
  // sum is asserted at zero tolerance. Routing it through the general
  // three-term form below would be algebraically identical and numerically
  // different.
  if (mode == BlendMode::Normal) return compositeOver(src, dst);
  // Not implemented here, and deliberately not faked: there is no latent at an
  // RGB texel to lerp (see mixLatents()). Every boundary that writes a file
  // reports it -- core/Composite's unimplementedBlendWarning().
  if (mode == BlendMode::Mix) return compositeOver(src, dst);

  const float as = src[3];
  const float ab = dst[3];
  // The two Porter-Duff "only one of them covers" weights, shared by every
  // remaining mode. `1 - as` is exact whenever as is in [0,1] (Sterbenz), and
  // an out-of-range alpha cannot be produced by this module -- core/Composite
  // clamps the one alpha multiplier it introduces.
  const float sOnly = 1.0f - ab;  // weight on the source where the backdrop is absent
  const float bOnly = 1.0f - as;  // weight on the backdrop where the source is absent

  // Alpha is `over` for every mode -- the separable-blend formula's `ao` does
  // not mention the blend function at all. A blend mode changes colour, not
  // coverage; --selftest asserts this across all six.
  const float ao = as + ab * bOnly;

  std::array<float, 4> out{0.0f, 0.0f, 0.0f, ao};

  switch (mode) {
    case BlendMode::Plus:
      // B(Cs,Cb) = Cs + Cb.
      //   as*ab*(cs/as + cb/ab) = ab*cs + as*cb
      //   co = cb*(1-as) + cs*(1-ab) + ab*cs + as*cb
      //      = cb - as*cb + cs - ab*cs + ab*cs + as*cb
      //      = cs + cb
      // i.e. additive light, and the alpha is still the union alpha rather
      // than Porter-Duff PLUS's `as + ab` -- coverage does not add just
      // because light does, and an alpha above 1 has no meaning here.
      for (int i = 0; i < 3; ++i) out[i] = src[i] + dst[i];
      break;

    case BlendMode::Multiply:
      // B(Cs,Cb) = Cs*Cb.
      //   as*ab*(cs/as)*(cb/ab) = cs*cb        <- the alphas cancel exactly
      //   co = cs*cb + cs*(1-ab) + cb*(1-as)
      // No division anywhere, which is the whole reason to derive it rather
      // than un-premultiply, blend and re-premultiply.
      for (int i = 0; i < 3; ++i) out[i] = src[i] * dst[i] + src[i] * sOnly + dst[i] * bOnly;
      break;

    case BlendMode::Screen:
      // B(Cs,Cb) = Cs + Cb - Cs*Cb.
      //   as*ab*B = ab*cs + as*cb - cs*cb
      //   co = cb*(1-as) + cs*(1-ab) + ab*cs + as*cb - cs*cb
      //      = cs + cb - cs*cb
      // The premultiplied form is the same expression as the straight one --
      // a coincidence worth writing down, because it is exactly the sort of
      // thing that gets assumed for multiply too, where it is false.
      for (int i = 0; i < 3; ++i) out[i] = src[i] + dst[i] - src[i] * dst[i];
      break;

    case BlendMode::Min:
      // B(Cs,Cb) = min(Cs,Cb).
      //   as*ab*min(cs/as, cb/ab) = min(ab*cs, as*cb)
      // because as*ab >= 0 and multiplying both arguments of min by a
      // non-negative scalar preserves the order.
      //   co = min(ab*cs, as*cb) + cs*(1-ab) + cb*(1-as)
      for (int i = 0; i < 3; ++i)
        out[i] = std::fmin(ab * src[i], as * dst[i]) + src[i] * sOnly + dst[i] * bOnly;
      break;

    case BlendMode::Max:
      // The same derivation with max.
      for (int i = 0; i < 3; ++i)
        out[i] = std::fmax(ab * src[i], as * dst[i]) + src[i] * sOnly + dst[i] * bOnly;
      break;

    case BlendMode::Normal:
    case BlendMode::Mix:
      break;  // handled above; listed so the switch is exhaustive
  }
  return out;
}

Latent mixLatents(const Latent& a, const Latent& b, float t) noexcept {
  Latent out{};
  // std::lerp rather than `a + t*(b-a)`: it is exact at both endpoints and
  // monotonic, which is what makes "t == 0 returns `a` exactly" an assertion
  // rather than a hope.
  for (int i = 0; i < 3; ++i) {
    out.c[i] = std::lerp(a.c[i], b.c[i], t);
    out.res[i] = std::lerp(a.res[i], b.res[i], t);
  }
  return out;
}

}  // namespace np
