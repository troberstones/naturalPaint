#include "core/Blend.hpp"

#include <cmath>

namespace np {
namespace {

// --- Stage 3: PDF 1.7 / CSS Compositing 1's non-separable blend helpers ----
//
// Hue/Saturation/Color/Luminosity need the whole straight-colour RGB
// triple's luminance and saturation, not per-channel arithmetic -- so they
// cannot be derived into a division-free premultiplied form the way
// multiply/screen/min/max were. `blendPixel()`'s switch cases for these six
// modes un-premultiply explicitly, behind an as==0/ab==0 guard (see there).
//
// **Deliberately Photoshop's own luma weights (0.3, 0.59, 0.11), NOT this
// codebase's Rec.709 kRec709LumaWeights (ops/ColorOps.hpp).** Those are two
// different constants for two different purposes -- Rec.709 for grayscale/
// saturation ops elsewhere, these for matching Photoshop's actual Hue/
// Saturation/Color/Luminosity output -- and reusing the wrong one would
// silently diverge while looking like reuse.
constexpr float kLumaR = 0.3f;
constexpr float kLumaG = 0.59f;
constexpr float kLumaB = 0.11f;

// Straight (un-premultiplied) [0,1]-nominal RGB triple.
struct Straight {
  float r, g, b;
};

float lumHSL(const Straight& c) { return kLumaR * c.r + kLumaG * c.g + kLumaB * c.b; }

float satHSL(const Straight& c) {
  return std::fmax(c.r, std::fmax(c.g, c.b)) - std::fmin(c.r, std::fmin(c.g, c.b));
}

// Pulls C back into a legal [0,1] range around its own luminance (PDF 1.7's
// ClipColor()). The division here is by (L-n) or (x-L) -- never by alpha --
// and is guarded against an exact-zero denominator: when n == L (or x == L)
// the clip term would not have moved anything anyway, since n < L < x holds
// strictly except at that degenerate point, so leaving C unclipped there is
// exact, not an approximation.
Straight clipColorHSL(Straight c) {
  const float l = lumHSL(c);
  const float n = std::fmin(c.r, std::fmin(c.g, c.b));
  const float x = std::fmax(c.r, std::fmax(c.g, c.b));
  if (n < 0.0f && l != n) {
    const float k = l / (l - n);
    c.r = l + (c.r - l) * k;
    c.g = l + (c.g - l) * k;
    c.b = l + (c.b - l) * k;
  }
  if (x > 1.0f && x != l) {
    const float k = (1.0f - l) / (x - l);
    c.r = l + (c.r - l) * k;
    c.g = l + (c.g - l) * k;
    c.b = l + (c.b - l) * k;
  }
  return c;
}

Straight setLumHSL(Straight c, float l) {
  const float d = l - lumHSL(c);
  c.r += d;
  c.g += d;
  c.b += d;
  return clipColorHSL(c);
}

// Rescales C so Sat(C) == s while preserving which channel is min/mid/max
// and where the mid channel sits between them (PDF 1.7's SetSat()). Sorts
// the three channels by value with plain index bookkeeping rather than
// pointer/reference aliasing into the struct, then writes the rescaled
// triple back to the original slots.
Straight setSatHSL(Straight c, float s) {
  float v[3] = {c.r, c.g, c.b};
  int order[3] = {0, 1, 2};  // order[0] = index of min, order[2] = index of max
  if (v[order[0]] > v[order[1]]) std::swap(order[0], order[1]);
  if (v[order[1]] > v[order[2]]) std::swap(order[1], order[2]);
  if (v[order[0]] > v[order[1]]) std::swap(order[0], order[1]);
  const int iMin = order[0], iMid = order[1], iMax = order[2];
  if (v[iMax] > v[iMin]) {
    v[iMid] = ((v[iMid] - v[iMin]) * s) / (v[iMax] - v[iMin]);
    v[iMax] = s;
  } else {
    v[iMid] = 0.0f;
    v[iMax] = 0.0f;
  }
  v[iMin] = 0.0f;
  return Straight{v[0], v[1], v[2]};
}

// The four Photoshop non-separable formulas, each a small composition of the
// helpers above. Cb = backdrop straight triple, Cs = source straight triple.
Straight hueHSL(const Straight& cb, const Straight& cs) {
  return setLumHSL(setSatHSL(cs, satHSL(cb)), lumHSL(cb));
}
Straight saturationHSL(const Straight& cb, const Straight& cs) {
  return setLumHSL(setSatHSL(cb, satHSL(cs)), lumHSL(cb));
}
Straight colorHSL(const Straight& cb, const Straight& cs) { return setLumHSL(cs, lumHSL(cb)); }
Straight luminosityHSL(const Straight& cb, const Straight& cs) {
  return setLumHSL(cb, lumHSL(cs));
}

// Darker/Lighter Color: no ClipColor/SetLum machinery, just a whole-triple
// compare-and-select on luminance.
Straight darkerColorHSL(const Straight& cb, const Straight& cs) {
  return lumHSL(cb) <= lumHSL(cs) ? cb : cs;
}
Straight lighterColorHSL(const Straight& cb, const Straight& cs) {
  return lumHSL(cb) >= lumHSL(cs) ? cb : cs;
}

// Shared shape for all six non-separable modes: guard the two
// division-by-alpha cases explicitly (an unguarded `src[i]/as` at as==0 is
// 0/0 = NaN, and 0*NaN is NaN, not 0 -- so the "transparent source is a
// bit-exact identity" invariant every other mode holds would silently break
// for these six without this), then un-premultiply into straight triples,
// run the PDF-1.7 formula, and recombine via the same three-term Porter-Duff
// split every other mode in blendPixel() uses.
//
// `B` is a non-type template parameter (the formula function itself, not a
// runtime function pointer) so each of the six call sites below gets its OWN
// instantiation with the callee baked in as a compile-time constant, rather
// than all six sharing one instantiation that takes `B` as a runtime value.
// That distinction is not stylistic: with `B` as a runtime function-pointer
// parameter (this used to be a local lambda inside blendPixel() taking
// `Straight (*b)(const Straight&, const Straight&)`), the optimiser folded
// all six switch cases' identical-shaped bodies into ONE shared block reached
// through a genuine indirect call (`blr` on arm64) -- confirmed in the
// linked binary's disassembly, one `blr` shared by the Hue/Saturation/Color/
// Luminosity/DarkerColor/LighterColor cases, each just loading its own
// formula's address into a register before jumping into the shared code.
// That merge is legal (it doesn't change any formula or branch) but it
// means every non-separable-mode pixel pays a real indirect call rather than
// having its specific formula inlined -- measured below.
template <Straight (*B)(const Straight&, const Straight&)>
void nonSeparable(float as, float ab, float sOnly, float bOnly, const std::array<float, 4>& src,
                  const std::array<float, 4>& dst, std::array<float, 4>& out) {
  if (as == 0.0f) {
    out[0] = dst[0];
    out[1] = dst[1];
    out[2] = dst[2];
    return;
  }
  if (ab == 0.0f) {
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
    return;
  }
  const Straight cs{src[0] / as, src[1] / as, src[2] / as};
  const Straight cb{dst[0] / ab, dst[1] / ab, dst[2] / ab};
  const Straight blended = B(cb, cs);
  out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * blended.r;
  out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * blended.g;
  out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * blended.b;
}

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
    // Stage 1 (docs/blend-mode-gaps.md): 7 Photoshop-compatible modes, all
    // display-referred -- each formula below has a 0 or 1 baked in that only
    // means something in [0,1] display-referred space, none are monotone
    // non-decreasing over the whole non-negative range core/Blend.hpp's B7
    // criterion requires for LinearLight.
    {BlendMode::Difference, "difference", "Difference", BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Exclusion,  "exclusion",  "Exclusion",  BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Subtract,   "subtract",   "Subtract",   BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::LinearBurn, "linear-burn","Linear Burn",BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::ColorDodge, "color-dodge","Color Dodge",BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::ColorBurn,  "color-burn", "Color Burn", BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Divide,     "divide",     "Divide",     BlendSpace::DisplayReferred, true, false, false},
    // Stage 2: the "light family" plus Overlay and Hard Mix. Every one of
    // them treats 1.0 as a reference white or black in its own formula (see
    // core/Blend.hpp's comment on the enum), so all seven are
    // DisplayReferred -- there is no scene-linear reading of any of them.
    {BlendMode::HardLight,  "hard-light",   "Hard Light",   BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Overlay,    "overlay",      "Overlay",      BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::VividLight, "vivid-light",  "Vivid Light",  BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::LinearLight,"linear-light", "Linear Light", BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::PinLight,   "pin-light",    "Pin Light",    BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::SoftLight,  "soft-light",   "Soft Light",   BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::HardMix,    "hard-mix",     "Hard Mix",     BlendSpace::DisplayReferred, true, false, false},
    // Stage 3: Photoshop's non-separable modes. All six are DisplayReferred:
    // Lum()/Sat()/ClipColor() all bake in [0,1] semantics (a luminance shift
    // clips against 0 and 1, a saturation rescale pins its max to a [0,1]
    // target), so none of the six is monotone over the unbounded range PRD
    // B7's LinearLight criterion (core/Blend.hpp) requires.
    {BlendMode::Hue,          "hue",          "Hue",          BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Saturation,   "saturation",   "Saturation",   BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Color,        "color",        "Color",        BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::Luminosity,   "luminosity",   "Luminosity",   BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::DarkerColor,  "darker-color", "Darker Color", BlendSpace::DisplayReferred, true, false, false},
    {BlendMode::LighterColor, "lighter-color","Lighter Color",BlendSpace::DisplayReferred, true, false, false},
};

constexpr size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);

static_assert(kModeCount == 27,
              "every BlendMode enumerator needs a row in kModes -- blendModeInfo() indexes "
              "this table by enumerator value (7 base modes + Stage 1's 7 separable modes + "
              "Stage 2's 7 light-family modes + Stage 3's 6 non-separable modes)");

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
  // PLAN.md Phase 5 step 9: **and neither of them may be clipped.** `Mix`
  // composites two layers as one unit; a clip makes the layer below the thing
  // that decides where the layer above *shows*. Both are relationships with
  // the same neighbour and there is no reading in which both hold -- see
  // core/Composite.hpp §15, which derives the rule and says what the composite
  // does with a document that carries the combination anyway (PRD I10).
  if (doc.layers[layerIndex].clipped || doc.layers[layerIndex - 1].clipped) return false;
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

    // --- Stage 1 (docs/blend-mode-gaps.md): 7 Photoshop-compatible modes,
    // all BlendSpace::DisplayReferred. Unlike the modes above, these are NOT
    // separable-and-division-free -- Photoshop's own formulas are defined on
    // STRAIGHT (un-premultiplied) [0,1] colour, so each case here
    // un-premultiplies, applies B(Cb,Cs), and re-premultiplies by hand,
    // guarded so the division by alpha can never see a zero denominator (a
    // fully transparent source or backdrop is still a bit-exact identity, as
    // required by core/Blend.hpp's alpha-is-over / transparent-identity
    // invariants).
    case BlendMode::Difference: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = |Cb - Cs|.
      const float Br = std::fabs(Cbr - Csr);
      const float Bg = std::fabs(Cbg - Csg);
      const float Bb = std::fabs(Cbb - Csb);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    // --- Stage 2: the "light family" plus Overlay and Hard Mix -------------
    //
    // None of these seven has a division-free premultiplied closed form the
    // way Plus/Multiply/Screen/Min/Max do above -- Photoshop defines every
    // one of them on STRAIGHT [0,1] colour. So each case un-premultiplies
    // explicitly (Cs = cs/as, Cb = cb/ab), applies the mode's own textbook
    // formula, and re-composites with the same three-term separable form
    // every mode here uses: co = cs*(1-ab) + cb*(1-as) + as*ab*B(Cs,Cb).
    //
    // The two early-outs guard the division itself. as == 0 (a fully
    // transparent source) makes cs == 0 for well-formed premultiplied data,
    // so cs/as would be 0.0f/0.0f = NaN in IEEE754 -- and that NaN survives
    // the later `* as` (NaN * 0 is NaN, not 0) and poisons an otherwise exact
    // identity. Returning `dst`'s colour directly, the same answer the
    // general formula would give if the division were finite, sidesteps the
    // NaN rather than computing and discarding it. ab == 0 is the mirror
    // case on the backdrop.

    case BlendMode::HardLight: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // HardLight(Cb,Cs) = Cs<=0.5 ? Multiply(Cb,2Cs) : Screen(Cb,2Cs-1)
      //                  = Cs<=0.5 ? Cb*(2Cs) : Cb+(2Cs-1)-Cb*(2Cs-1)
      const float Br = Csr <= 0.5f ? Cbr * (2.0f * Csr)
                                   : Cbr + (2.0f * Csr - 1.0f) - Cbr * (2.0f * Csr - 1.0f);
      const float Bg = Csg <= 0.5f ? Cbg * (2.0f * Csg)
                                   : Cbg + (2.0f * Csg - 1.0f) - Cbg * (2.0f * Csg - 1.0f);
      const float Bb = Csb <= 0.5f ? Cbb * (2.0f * Csb)
                                   : Cbb + (2.0f * Csb - 1.0f) - Cbb * (2.0f * Csb - 1.0f);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::Exclusion: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cb + Cs - 2*Cb*Cs.
      const float Br = Cbr + Csr - 2.0f * Cbr * Csr;
      const float Bg = Cbg + Csg - 2.0f * Cbg * Csg;
      const float Bb = Cbb + Csb - 2.0f * Cbb * Csb;
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::Overlay: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // Overlay(Cb,Cs) = HardLight(Cs,Cb): HardLight's OWN Cb-argument here
      // is this mode's Cs, and HardLight's OWN Cs-argument -- the one the
      // <=0.5 test reads -- is this mode's Cb. Written out by hand against
      // Cb rather than reused from the HardLight case above, so the argument
      // swap cannot silently revert to testing Cs like Hard Light does.
      const float Br = Cbr <= 0.5f ? Csr * (2.0f * Cbr)
                                   : Csr + (2.0f * Cbr - 1.0f) - Csr * (2.0f * Cbr - 1.0f);
      const float Bg = Cbg <= 0.5f ? Csg * (2.0f * Cbg)
                                   : Csg + (2.0f * Cbg - 1.0f) - Csg * (2.0f * Cbg - 1.0f);
      const float Bb = Cbb <= 0.5f ? Csb * (2.0f * Cbb)
                                   : Csb + (2.0f * Cbb - 1.0f) - Csb * (2.0f * Cbb - 1.0f);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::Subtract: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cb - Cs.
      const float Br = Cbr - Csr;
      const float Bg = Cbg - Csg;
      const float Bb = Cbb - Csb;
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::VividLight: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // ColorDodge(Cb,x) = Cb==0 ? 0 : (x>=1 ? 1 : min(1, Cb/(1-x)))
      // ColorBurn(Cb,x)  = Cb>=1 ? 1 : (x==0 ? 0 : 1 - min(1, (1-Cb)/x))
      // VividLight(Cb,Cs) = Cs<=0.5 ? ColorBurn(Cb,2Cs) : ColorDodge(Cb,2Cs-1)
      // Neither sub-formula divides unguarded: ColorDodge's division only
      // runs in the branch where x < 1 (so 1-x != 0), and ColorBurn's only
      // runs where x != 0 -- both expanded by hand below, per channel.
      const float Br =
          Csr <= 0.5f
              ? (Cbr >= 1.0f ? 1.0f
                             : (2.0f * Csr == 0.0f
                                    ? 0.0f
                                    : 1.0f - std::fmin(1.0f, (1.0f - Cbr) / (2.0f * Csr))))
              : (Cbr == 0.0f ? 0.0f
                             : (2.0f * Csr - 1.0f >= 1.0f
                                    ? 1.0f
                                    : std::fmin(1.0f, Cbr / (1.0f - (2.0f * Csr - 1.0f)))));
      const float Bg =
          Csg <= 0.5f
              ? (Cbg >= 1.0f ? 1.0f
                             : (2.0f * Csg == 0.0f
                                    ? 0.0f
                                    : 1.0f - std::fmin(1.0f, (1.0f - Cbg) / (2.0f * Csg))))
              : (Cbg == 0.0f ? 0.0f
                             : (2.0f * Csg - 1.0f >= 1.0f
                                    ? 1.0f
                                    : std::fmin(1.0f, Cbg / (1.0f - (2.0f * Csg - 1.0f)))));
      const float Bb =
          Csb <= 0.5f
              ? (Cbb >= 1.0f ? 1.0f
                             : (2.0f * Csb == 0.0f
                                    ? 0.0f
                                    : 1.0f - std::fmin(1.0f, (1.0f - Cbb) / (2.0f * Csb))))
              : (Cbb == 0.0f ? 0.0f
                             : (2.0f * Csb - 1.0f >= 1.0f
                                    ? 1.0f
                                    : std::fmin(1.0f, Cbb / (1.0f - (2.0f * Csb - 1.0f)))));
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::LinearBurn: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cb + Cs - 1.
      const float Br = Cbr + Csr - 1.0f;
      const float Bg = Cbg + Csg - 1.0f;
      const float Bb = Cbb + Csb - 1.0f;
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::LinearLight: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // LinearLight(Cb,Cs) = Cb + 2Cs - 1. Both textbook branches --
      // LinearBurn(Cb,2Cs) = Cb+2Cs-1 and LinearDodge(Cb,2Cs-1) = Cb+2Cs-1 --
      // reduce to this identical expression, so there is no branch left to
      // write; the Cs<=0.5 split every other mode in this family has simply
      // does not survive simplification here.
      const float Br = Cbr + 2.0f * Csr - 1.0f;
      const float Bg = Cbg + 2.0f * Csg - 1.0f;
      const float Bb = Cbb + 2.0f * Csb - 1.0f;
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::ColorDodge: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cb==0 ? 0 : (Cs>=1 ? 1 : min(1, Cb/(1-Cs))).
      const float Br = Cbr == 0.0f ? 0.0f : (Csr >= 1.0f ? 1.0f : std::fmin(1.0f, Cbr / (1.0f - Csr)));
      const float Bg = Cbg == 0.0f ? 0.0f : (Csg >= 1.0f ? 1.0f : std::fmin(1.0f, Cbg / (1.0f - Csg)));
      const float Bb = Cbb == 0.0f ? 0.0f : (Csb >= 1.0f ? 1.0f : std::fmin(1.0f, Cbb / (1.0f - Csb)));
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::PinLight: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // PinLight(Cb,Cs) = Cs<0.5 ? min(Cb,2Cs) : max(Cb,2Cs-1)
      const float Br = Csr < 0.5f ? std::fmin(Cbr, 2.0f * Csr) : std::fmax(Cbr, 2.0f * Csr - 1.0f);
      const float Bg = Csg < 0.5f ? std::fmin(Cbg, 2.0f * Csg) : std::fmax(Cbg, 2.0f * Csg - 1.0f);
      const float Bb = Csb < 0.5f ? std::fmin(Cbb, 2.0f * Csb) : std::fmax(Cbb, 2.0f * Csb - 1.0f);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::ColorBurn: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cb>=1 ? 1 : (Cs==0 ? 0 : 1 - min(1, (1-Cb)/Cs)).
      const float Br = Cbr >= 1.0f ? 1.0f : (Csr == 0.0f ? 0.0f : 1.0f - std::fmin(1.0f, (1.0f - Cbr) / Csr));
      const float Bg = Cbg >= 1.0f ? 1.0f : (Csg == 0.0f ? 0.0f : 1.0f - std::fmin(1.0f, (1.0f - Cbg) / Csg));
      const float Bb = Cbb >= 1.0f ? 1.0f : (Csb == 0.0f ? 0.0f : 1.0f - std::fmin(1.0f, (1.0f - Cbb) / Csb));
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::SoftLight: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // D(x) = x<=0.25 ? ((16x-12)x+4)x : sqrt(x)
      // SoftLight(Cb,Cs) = Cs<=0.5 ? Cb-(1-2Cs)*Cb*(1-Cb) : Cb+(2Cs-1)*(D(Cb)-Cb)
      // D(Cb) expanded by hand per channel below; computed unconditionally
      // (it is cheap and has no side effects) rather than nested inside the
      // Cs>0.5 branch, purely for readability -- it is unused, not wrong,
      // when Cs<=0.5.
      const float Dr = Cbr <= 0.25f ? ((16.0f * Cbr - 12.0f) * Cbr + 4.0f) * Cbr : std::sqrt(Cbr);
      const float Dg = Cbg <= 0.25f ? ((16.0f * Cbg - 12.0f) * Cbg + 4.0f) * Cbg : std::sqrt(Cbg);
      const float Db = Cbb <= 0.25f ? ((16.0f * Cbb - 12.0f) * Cbb + 4.0f) * Cbb : std::sqrt(Cbb);
      const float Br = Csr <= 0.5f ? Cbr - (1.0f - 2.0f * Csr) * Cbr * (1.0f - Cbr)
                                   : Cbr + (2.0f * Csr - 1.0f) * (Dr - Cbr);
      const float Bg = Csg <= 0.5f ? Cbg - (1.0f - 2.0f * Csg) * Cbg * (1.0f - Cbg)
                                   : Cbg + (2.0f * Csg - 1.0f) * (Dg - Cbg);
      const float Bb = Csb <= 0.5f ? Cbb - (1.0f - 2.0f * Csb) * Cbb * (1.0f - Cbb)
                                   : Cbb + (2.0f * Csb - 1.0f) * (Db - Cbb);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::Divide: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // B(Cb,Cs) = Cs==0 ? 1 : min(1, Cb/Cs).
      const float Br = Csr == 0.0f ? 1.0f : std::fmin(1.0f, Cbr / Csr);
      const float Bg = Csg == 0.0f ? 1.0f : std::fmin(1.0f, Cbg / Csg);
      const float Bb = Csb == 0.0f ? 1.0f : std::fmin(1.0f, Cbb / Csb);
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::HardMix: {
      if (as == 0.0f) { out[0] = dst[0]; out[1] = dst[1]; out[2] = dst[2]; break; }
      if (ab == 0.0f) { out[0] = src[0]; out[1] = src[1]; out[2] = src[2]; break; }
      const float Csr = src[0] / as, Csg = src[1] / as, Csb = src[2] / as;
      const float Cbr = dst[0] / ab, Cbg = dst[1] / ab, Cbb = dst[2] / ab;
      // HardMix(Cb,Cs) = (Cb+Cs)>=1 ? 1 : 0. A second published formula
      // exists (threshold Vivid Light's own result at 0.5 instead) -- this
      // is a real, acknowledged ambiguity in how Hard Mix is defined across
      // sources, not an oversight. This build deliberately picks the
      // simpler Cb+Cs>=1 form rather than blending the two.
      const float Br = (Cbr + Csr) >= 1.0f ? 1.0f : 0.0f;
      const float Bg = (Cbg + Csg) >= 1.0f ? 1.0f : 0.0f;
      const float Bb = (Cbb + Csb) >= 1.0f ? 1.0f : 0.0f;
      out[0] = src[0] * sOnly + dst[0] * bOnly + as * ab * Br;
      out[1] = src[1] * sOnly + dst[1] * bOnly + as * ab * Bg;
      out[2] = src[2] * sOnly + dst[2] * bOnly + as * ab * Bb;
      break;
    }

    case BlendMode::Hue:
      // Hue(Cb,Cs) = SetLum(SetSat(Cs, Sat(Cb)), Lum(Cb)) -- take the
      // source's hue and saturation, but the backdrop's luminance.
      nonSeparable<hueHSL>(as, ab, sOnly, bOnly, src, dst, out);
      break;

    case BlendMode::Saturation:
      // Saturation(Cb,Cs) = SetLum(SetSat(Cb, Sat(Cs)), Lum(Cb)) -- the
      // backdrop's hue and luminance, the source's saturation.
      nonSeparable<saturationHSL>(as, ab, sOnly, bOnly, src, dst, out);
      break;

    case BlendMode::Color:
      // Color(Cb,Cs) = SetLum(Cs, Lum(Cb)) -- the source's hue and
      // saturation, the backdrop's luminance. One SetLum() call: no
      // SetSat() needed at all.
      nonSeparable<colorHSL>(as, ab, sOnly, bOnly, src, dst, out);
      break;

    case BlendMode::Luminosity:
      // Luminosity(Cb,Cs) = SetLum(Cb, Lum(Cs)) -- the mirror of Color:
      // the backdrop's hue and saturation, the source's luminance.
      nonSeparable<luminosityHSL>(as, ab, sOnly, bOnly, src, dst, out);
      break;

    case BlendMode::DarkerColor:
      // No ClipColor/SetLum machinery: a whole-triple compare-and-select
      // on luminance, not a per-channel min (which is `Min` above).
      nonSeparable<darkerColorHSL>(as, ab, sOnly, bOnly, src, dst, out);
      break;

    case BlendMode::LighterColor:
      // The mirror of DarkerColor: whole-triple select, not a per-channel
      // max (which is `Max` above).
      nonSeparable<lighterColorHSL>(as, ab, sOnly, bOnly, src, dst, out);
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
