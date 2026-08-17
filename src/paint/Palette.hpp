#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace np {

// A Mixbox latent: three pigment weights (the fourth is implied as 1-c0-c1-c2)
// plus an additive RGB residual. Linear combinations of these are Kubelka-Munk
// mixes, which is why the solver transports them instead of RGB.
struct Latent {
  std::array<float, 3> c{};
  std::array<float, 3> res{};
};

// CPU-side mirror of shaders/include/mixbox.wgsl, used for palette swatches and
// for converting the selected colour into the brush uniform.
class MixboxLut {
 public:
  bool load(const std::string& pngPath);
  bool valid() const { return !data_.empty(); }

  Latent rgbToLatent(float r, float g, float b) const;
  std::array<float, 3> latentToRgb(const Latent& z) const;

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
