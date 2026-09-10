#include "io/PsdBlendKeys.hpp"

#include <cstring>

// io/PsdBlendKeys implementation. io/PsdBlendKeys.hpp argues why this table
// is shared, what "exact" means for the two rows that are, and why a missing
// key is nullptr-and-warn; io/PsdImport.hpp argues each individual mapping.
// The table and `mapBlendKey()` below were moved here VERBATIM out of
// io/PsdImport.cpp's anonymous namespace -- same rows, same order, same
// comments -- because that importer is verified layer-for-layer against
// three real Photoshop files and a relocation must not perturb it.
namespace np {
namespace {

// A 4-byte compare, the same one io/PsdImport.cpp's `fourccEquals()` does.
// Not shared with it: that one is private to the reader's cursor code and
// reaching across for a one-line `memcmp` would couple two files for nothing.
bool keyEquals(const std::array<char, 4>& fourcc, const char* lit) noexcept {
  return std::memcmp(fourcc.data(), lit, 4) == 0;
}

// --- Blend mode mapping ------------------------------------------------------
//
// io/PsdImport.hpp argues each mapping (and the deliberate absence of the
// rest) at length; this table is just the wire keys next to the
// core::BlendMode each one maps to. Every key not listed here is reported by
// name and left as Normal -- see `mapBlendKey()`. **Not every row here is an
// EXACT match** -- `dark`/`lite` are; `lddg` is not; see each row's own
// comment and io/PsdImport.hpp's "Blend mode mapping" section for the full
// argument.
constexpr PsdBlendKeyEntry kBlendKeyMap[] = {
    {"norm", BlendMode::Normal},
    {"mul ", BlendMode::Multiply},
    {"scrn", BlendMode::Screen},
    // Darken is an exact per-channel minimum and Lighten an exact per-channel
    // maximum of source and backdrop -- io/PsdImport.hpp's own header
    // derives why these two, alone among Photoshop's non-`norm` keys with no
    // literal core::BlendMode counterpart, are still an EXACT match rather
    // than an approximation.
    {"dark", BlendMode::Min},
    {"lite", BlendMode::Max},
    // Linear Dodge (Add) is `Cs + Cb`, exactly core::Blend.cpp's
    // `BlendMode::Plus` (additive light). **Not exact, unlike the two rows
    // above**: min/max are order-preserving, so they commute with any
    // monotone transfer function and agree with Photoshop whether the
    // addition happens before or after gamma encoding. Addition does not
    // commute with a transfer function -- Photoshop adds in gamma space by
    // default ("Blend RGB Colors Using Gamma 1.0" off) and this codebase
    // adds in linear light. That is the identical compromise `mul `/`scrn`
    // already ship with above, not a new one.
    {"lddg", BlendMode::Plus},
    // Stage 1 (docs/blend-mode-gaps.md): these 7 are NOT exact matches, unlike
    // `dark`/`lite` above. This codebase composites them in premultiplied,
    // LINEAR-LIGHT RGBA (core/Blend.hpp), while Photoshop's default is to
    // blend in whichever (usually gamma-encoded) space the document works in
    // -- the same reason a `mul`/`scrn`/`lddg`-style key would only be an
    // approximation rather than an exact match. Listed anyway, and not
    // reported as a mismatch, because "approximate but visually close" is a
    // materially better outcome than falling back to Normal.
    {"diff", BlendMode::Difference},  // Difference
    {"smud", BlendMode::Exclusion},   // Exclusion
    {"fsub", BlendMode::Subtract},    // Subtract
    {"lbrn", BlendMode::LinearBurn},  // Linear Burn
    {"div ", BlendMode::ColorDodge},  // Color Dodge
    {"idiv", BlendMode::ColorBurn},   // Color Burn
    {"fdiv", BlendMode::Divide},      // Divide
    // Stage 2's seven "light family" modes (docs/blend-mode-gaps.md). Unlike
    // `dark`/`lite` above, these are NOT exact matches in substance, even
    // though `mapBlendKey()` reports them as one (there is no third bucket
    // between "exact" and "no equivalent, warn and fall back to Normal", and
    // a mode this build genuinely composites belongs on this side of that
    // line, not the other). The approximation: this codebase composites in
    // LINEAR light and Photoshop's default compositing is GAMMA-space, and
    // none of Hard Light/Overlay/Vivid Light/Linear Light/Pin Light/Soft
    // Light/Hard Mix is invariant to that choice of space (unlike Darken/
    // Lighten's per-channel min/max above, which are). A PSD written by
    // Photoshop with one of these blend keys will therefore round-trip
    // through this importer with the right blend *mode* but not bit-exact
    // pixels -- the same caveat this build already carries for any
    // gamma-space blend, stated here rather than left implicit.
    {"hLit", BlendMode::HardLight},
    {"over", BlendMode::Overlay},
    {"vLit", BlendMode::VividLight},
    {"lLit", BlendMode::LinearLight},
    {"pLit", BlendMode::PinLight},
    {"sLit", BlendMode::SoftLight},
    {"hMix", BlendMode::HardMix},
    // Stage 3's six non-separable modes. These, like `lddg` (Linear Dodge),
    // are approximations rather than exact matches for the same reason: this
    // codebase blends in premultiplied LINEAR light, while Photoshop's own
    // Hue/Saturation/Color/Luminosity/Darker-Color/Lighter-Color operate in
    // gamma (display-encoded) space, so the same PSD file can composite
    // visibly differently between the two. `colr` and `lum ` are the two
    // that matter most in practice -- they are the modes actually present in
    // the user's own real PSD file (three `colr` layers) -- so getting those
    // two exactly right (mode selection, not the linear-vs-gamma gap, which
    // is a known, stated approximation) is worth more than the rest of this
    // table.
    //
    // Three of the six keys carry a trailing space, matching Photoshop's own
    // 4-byte padding of a 3-character key -- get it exactly right or
    // fourccEquals() silently fails to match real files.
    {"hue ", BlendMode::Hue},
    {"sat ", BlendMode::Saturation},
    {"colr", BlendMode::Color},
    {"lum ", BlendMode::Luminosity},
    {"dkCl", BlendMode::DarkerColor},
    {"lgCl", BlendMode::LighterColor},
};

}  // namespace

// `std::nullopt`-free by design: every key maps to a BlendMode, and the
// bool return says whether that mapping was an EXACT one (found in the
// table above) or the "no equivalent, reported and left as Normal" fallback
// io/PsdImport.hpp promises. The caller uses the bool to decide whether to
// warn; it never changes the returned mode, since Normal is the answer
// either way (the fallback's `BlendMode::Normal` and `norm`'s own entry
// happen to produce the same value, which is what makes "exact or
// Normal-and-warn" a total function rather than a partial one).
BlendMode mapBlendKey(const std::array<char, 4>& key, bool& exactMatch) noexcept {
  for (const PsdBlendKeyEntry& entry : kBlendKeyMap) {
    if (keyEquals(key, entry.psdKey)) {
      exactMatch = true;
      return entry.mode;
    }
  }
  exactMatch = false;
  return BlendMode::Normal;
}

// --- The reverse direction: BlendMode -> PSD key ---------------------------
//
// A linear scan of the same rows, so the two directions can never disagree
// about which key names which mode -- there is no second table to keep in
// step. `nullptr` for a mode with no Photoshop key (see the header: exactly
// `BlendMode::Mix` today, and adding a mode without triaging it is what
// app/selftest/PsdBlendKeys.cpp's tripwire refuses to let happen quietly).
// Deliberately NOT a substitution: a near-enough key writes a file that
// opens and is wrong, which is worse than a file that opens with a stated
// omission.
const char* psdBlendKeyFor(BlendMode mode) noexcept {
  for (const PsdBlendKeyEntry& entry : kBlendKeyMap) {
    if (entry.mode == mode) return entry.psdKey;
  }
  return nullptr;
}

std::span<const PsdBlendKeyEntry> psdBlendKeys() noexcept {
  return std::span<const PsdBlendKeyEntry>(kBlendKeyMap,
                                           sizeof(kBlendKeyMap) / sizeof(kBlendKeyMap[0]));
}

}  // namespace np
