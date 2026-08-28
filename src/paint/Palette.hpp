#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Pigment.hpp"

namespace np {

// `Latent` moved to core/Pigment.hpp at PLAN.md Phase 5 step 3, together with
// the latent -> RGB projection, and this header now includes it. The reason is
// in that file: `Latent` is a domain value type -- what a Pigment tile stores
// and what `Mix` lerps -- and leaving it here forced `core/Blend.hpp` to
// include `paint/Palette.hpp`, a `core/` -> `paint/` edge that step 2 recorded
// as the structural thing it was least happy about. `paint/` -> `core/` is the
// allowed direction, so this include is the fix rather than a workaround.
//
// What stayed here is the half that genuinely needs the 512x512 LUT: the
// **inverse** map, `rgbToLatent()`. The forward projection never touched the
// LUT's pixels at all (it is a fixed polynomial), which is why it could move
// and why a Pigment layer composites in a build that never loaded the PNG.

// CPU-side mirror of shaders/include/mixbox.wgsl, used for palette swatches and
// for converting the selected colour into the brush uniform.
//
// **Two implementations, chosen by NP_USE_MIXBOX, same as `core::latentToRgb()`
// on the other side of this same round trip.** Under Mixbox this class is
// exactly what its name says: a loaded 512x512 LUT and a bilinear lookup into
// it. Under NP_USE_MIXBOX=OFF there is no LUT -- the two-constant Kubelka-Munk
// fallback (Palette.cpp) is closed-form -- but the class keeps its name and
// its shape (`load()`/`valid()`/`rgbToLatent()`) rather than being renamed or
// `#ifdef`'d out of existence, because every caller (`app/StrokeSession.cpp`
// and the ~20 files it lists as siblings) holds a `const MixboxLut&` and none
// of them need to know or care which basis answered the call. `load()`
// becomes a no-op that always succeeds and `valid()` becomes unconditionally
// true; see their definitions for why that is the correct behaviour rather
// than a stub, including for the several `--selftest` sites that construct an
// intentionally-unloaded one to exercise the ON-build's own "no LUT" fallback.
class MixboxLut {
 public:
  // NP_USE_MIXBOX=OFF only: the reflectance floor `rgbToLatent()` clamps a
  // picked channel to before taking Kubelka's K/S (Palette.cpp). Public so
  // `--selftest` can derive the KM2 round-trip tolerance from the same
  // number the implementation clamps with, rather than a second transcribed
  // literal that could drift from it -- see app/selftest/PigmentLayer.cpp
  // and app/selftest/Blend.cpp for the two sites that do.
  static constexpr float kKm2ReflectanceFloor = 0.03f;

  bool load(const std::string& pngPath);
  bool valid() const;

  // RGB -> latent. The direction that needs the LUT under Mixbox; under the
  // KM2 fallback it needs no LUT at all (closed-form, see Palette.cpp). Its
  // inverse is the free function `np::latentToRgb()` in core/Pigment.hpp -- it
  // was a member here until Phase 5 step 3, and it was a member that never
  // read `data_` under Mixbox, which made it look like a projection required
  // a loaded LUT. It does not, under either basis.
  Latent rgbToLatent(float r, float g, float b) const;

  // Raw 512x512 RGBA8 for upload to the GPU.
  const std::vector<uint8_t>& pixels() const { return data_; }
  static constexpr uint32_t kSize = 512;

 private:
  std::array<float, 3> fetch(float x, float y) const;
  std::vector<uint8_t> data_;
};

// Physical pigment properties, following Curtis et al. 1997 Table 1.
struct Pigment {
  const char* name;
  float rgb[3];
  float density;      // rho   — settles out of suspension faster when high
  float staining;     // omega — resists lifting; a stain will not wash back out
  float granulation;  // gamma — pools into the paper's valleys
};

// Colours are the real pigment measurements published with Mixbox; the physical
// constants are chosen to match how these paints actually behave on paper.
const std::vector<Pigment>& defaultPalette();

}  // namespace np
