#pragma once

#include <array>
#include <cstdint>

#include "core/Half.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// core/Pigment (PLAN.md "Phase 5 -- Stack it", step 3: "Pigment layers --
// latent x mass tile storage at f16"). PRD C1, C3, C8, F10; DESIGN-imaging.md
// §3 "Pigment layers -- Kubelka-Munk in a layered document";
// docs/document-format.md's `pig.c0 pig.c1 pig.c2 pig.m` + `res.R res.G res.B`.
//
// --- Why `Latent` lives in `core/` and not beside the Mixbox LUT -----------
//
// It used to live in paint/Palette.hpp, which made `core/Blend.hpp` include
// `paint/Palette.hpp` for it -- a `core/` -> `paint/` edge, and the one thing
// PLAN.md's Phase 5 step 2 Findings row named as what got worse, with the fix
// stated in advance: "the natural fix is for `Latent` to move to `core/`
// beside the tile that will store it, at step 3". This is that move. `Latent`
// is a domain value type -- the thing a Pigment tile stores and the thing
// `mixLatents()` lerps -- that merely happened to be declared next to the LUT
// that produces one. `paint/Palette.hpp` now includes *this* header, which is
// the allowed direction.
//
// The move is only possible because of a fact worth stating plainly, since it
// is what makes a Pigment layer compositable at all in a build with no LUT
// file loaded: **the latent -> RGB projection needs no LUT.** `latentToRgb()`
// below is a fixed 20-term polynomial in the three stored pigment weights plus
// the additive residual; the 512x512 Mixbox texture is needed only by the
// *inverse* map, `MixboxLut::rgbToLatent()`, which stays in paint/. So
// core/Composite can project a Pigment layer to RGB with no file, no image
// decode and no `paint/` dependency, and `--selftest` asserts the projection
// against the real LUT's own output rather than against a stand-in.
//
// --- Six floats, seven channels, and the implied eighth --------------------
//
// Three quantities get confused here, so they are separated once:
//
//   * **`Latent` holds six floats**: `c[0..2]`, three of Mixbox's four pigment
//     weights, and `res[0..2]`, an additive linear-RGB residual.
//   * **Mixbox's latent has seven components.** The fourth pigment weight is
//     `c3 = 1 - (c0+c1+c2)`, *derived, never stored*. Because that implication
//     is affine, lerping c0..c2 lerps c3 by the same `t` automatically -- so
//     six floats is *exactly* Mixbox's seven and not an approximation of it.
//     core/Blend.hpp argues it and `--selftest` asserts it on the implied
//     component. Nothing here invents a seventh stored float.
//   * **docs/document-format.md names seven channels** -- `pig.c0 pig.c1
//     pig.c2 pig.m` and `res.R res.G res.B`. Those are the six `Latent` floats
//     **plus mass**, which is not part of the latent at all: it is the
//     Pigment-layer analogue of alpha, the quantity PRD F10 says an eraser
//     reduces "leaving the Latent untouched". `c3` is *not* one of the seven;
//     it is derived on every use, in `latentToRgb()` below and in
//     third_party/mixbox and shaders/include/mixbox.wgsl alike.
//
// So: 6 stored latent floats + 1 stored mass = the format's 7 named channels,
// and the 8th quantity (c3) is derived. `PigmentTexel` and `PigmentTile` below
// are laid out in exactly the format's channel order, so the on-disk mapping
// is positional and needs no table.
namespace np {

// A Mixbox latent: three pigment weights (the fourth is implied as
// 1-c0-c1-c2) plus an additive RGB residual. Linear combinations of these are
// Kubelka-Munk mixes, which is why the solver transports them instead of RGB,
// and why `Mix` (PRD C3) is a lerp of these rather than of colour.
//
// Moved verbatim from paint/Palette.hpp -- same members, same order, same
// default-initialisation -- so every existing producer and consumer of one is
// unaffected by the move.
struct Latent {
  std::array<float, 3> c{};
  std::array<float, 3> res{};

  friend bool operator==(const Latent&, const Latent&) = default;
};

// Mixbox's 20-term cubic in the four pigment weights, evaluated in linear RGB.
//
// **Moved verbatim from paint/Palette.cpp's anonymous namespace**, coefficient
// for coefficient and term for term, and that word is load-bearing for the
// same reason it was when `compositeOver()` moved in step 2: `--selftest`
// asserts `latentToRgb(rgbToLatent(c)) == c` at a 5.0e-7 bound and measures
// **0.000e+00**, which is exact only because `rgbToLatent()` defines the
// residual as `rgb - <this function>(c)` and `latentToRgb()` adds it back to
// the output of *the same* function. Two transcriptions of this polynomial
// that differed in one ulp would silently turn that exactness into noise.
//
// Identical to `mixboxEvalPolynomial()` in shaders/include/mixbox.wgsl; keep
// the two in step if either is ever touched.
std::array<float, 3> pigmentPolynomialRgb(const std::array<float, 3>& c) noexcept;

// The latent -> RGB projection: **straight** (non-premultiplied) linear RGB,
// clamped to [0,1].
//
// The clamp is Mixbox's own, moved with the function rather than introduced
// here. It is the one place in this codebase's compositing chain that clamps,
// and it is defensible for a reason that does not generalise: the polynomial's
// output is a *reflectance*, and a reflectance above 1 or below 0 is not a
// bright highlight, it is a pigment mixture outside the model's domain. This
// is not io/Export's quantization clamp and does not license one elsewhere.
//
// No LUT, no state, no `paint/` -- see this header's opening. It is a free
// function rather than a `MixboxLut` member (which is what it used to be)
// precisely because it never touched the LUT's pixels: a member that ignores
// its object is a misleading signature, and having it on the class implied the
// projection was unavailable without a loaded 512x512 PNG. It is not.
std::array<float, 3> latentToRgb(const Latent& z) noexcept;

// One Pigment texel, in the format's own channel order. `mass` is the Pigment
// analogue of alpha (PRD F10: erase "reduces ... Mass on Pigment layers
// leaving the Latent untouched"), and core/Composite projects the pair to a
// premultiplied RGBA texel as `(latentToRgb(latent) * mass, mass)`.
struct PigmentTexel {
  Latent latent;
  float mass = 0.0f;

  friend bool operator==(const PigmentTexel&, const PigmentTexel&) = default;
};

// One 128x128 Pigment tile: seven half-float channels per texel, in
// docs/document-format.md's own order --
//
//   0 pig.c0   1 pig.c1   2 pig.c2   3 pig.m   4 res.R   5 res.G   6 res.B
//
// **Seven channels, not eight, and that is a decision a reviewer may push
// on.** DESIGN-imaging.md §3's memory table has two rows for a pigment tile,
// "pigment, 8ch f16 | 256 KiB" and "pigment, 8ch f32 | 512 KiB", while its
// prose one line above says "a pigment tile carries 7 channels (c0 c1 c2 m,
// res rgb)". Seven is what the *content* is and what the *file format* names;
// eight is what a GPU upload would want, because the natural GPU
// representation is two `rgba16float` textures (which is exactly the shape
// sim/PaintSim's own `pigC_`/`pigR_` field pair already has). This tile stores
// seven, for three reasons:
//
//  1. **The file format has seven named channels.** A tile whose channel count
//     equals the part's latent channel count makes io/NpaintFile's pack and
//     unpack positional, with no "skip the padding word" step -- and a padding
//     word that a writer forgot to skip is a class of bug that writes garbage
//     into a document and is invisible until someone else's tool opens it.
//  2. **PRD C2 is about content.** The eighth channel would hold nothing, at
//     32 KiB per occupied tile -- 14% of the tile, paid on every tile of every
//     Pigment layer, for a GPU path that does not exist: no Pigment layer
//     reaches the GPU in this build, because `sim::PaintSim` owns one dense
//     texture and no stroke reaches a `Layer` at all.
//  3. The design table's 256 KiB row still bounds this: 224 KiB is under it.
//     When a GPU upload does arrive it repacks into two RGBA planes, and an
//     upload is a copy either way.
//
// Callers work in ordinary `float`; the half<->float conversion happens at the
// read/write boundary via core/Half, exactly as core::Tile does it, so the
// resident cost is the stored one without every caller hand-rolling the
// encoding.
class PigmentTile {
 public:
  // Value-initializes every texel to half-precision zero: mass 0, which is
  // "no pigment here" and projects to a fully transparent texel -- the
  // correct implicit content for a tile no stroke has touched, and the exact
  // analogue of core::Tile's transparent-black default.
  PigmentTile() = default;

  static constexpr int32_t kChannels = 7;
  static constexpr size_t kTexelCount =
      static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize) * kChannels;

  // `local` must satisfy `0 <= local.x, local.y < kTileSize` -- exactly what
  // Tile.hpp's tileLocalOffset() produces from a document coordinate.
  PigmentTexel readTexel(PixelCoord local) const noexcept {
    const size_t base = texelIndex(local);
    PigmentTexel t;
    t.latent.c[0] = halfToFloat(texels_[base + 0]);
    t.latent.c[1] = halfToFloat(texels_[base + 1]);
    t.latent.c[2] = halfToFloat(texels_[base + 2]);
    t.mass = halfToFloat(texels_[base + 3]);
    t.latent.res[0] = halfToFloat(texels_[base + 4]);
    t.latent.res[1] = halfToFloat(texels_[base + 5]);
    t.latent.res[2] = halfToFloat(texels_[base + 6]);
    return t;
  }

  void writeTexel(PixelCoord local, const PigmentTexel& t) noexcept {
    const size_t base = texelIndex(local);
    texels_[base + 0] = floatToHalf(t.latent.c[0]);
    texels_[base + 1] = floatToHalf(t.latent.c[1]);
    texels_[base + 2] = floatToHalf(t.latent.c[2]);
    texels_[base + 3] = floatToHalf(t.mass);
    texels_[base + 4] = floatToHalf(t.latent.res[0]);
    texels_[base + 5] = floatToHalf(t.latent.res[1]);
    texels_[base + 6] = floatToHalf(t.latent.res[2]);
  }

  // Raw half-float storage, for the bulk paths (io/NpaintFile's pack/unpack)
  // that move a whole tile row at once. This is what makes the "HALF in, HALF
  // out, no conversion" claim true for the latent channels of a Pigment part
  // exactly as it already is for an RGB one.
  const uint16_t* data() const noexcept { return texels_.data(); }
  uint16_t* data() noexcept { return texels_.data(); }

 private:
  static size_t texelIndex(PixelCoord local) noexcept {
    return (static_cast<size_t>(local.y) * static_cast<size_t>(kTileSize) +
            static_cast<size_t>(local.x)) *
           static_cast<size_t>(kChannels);
  }

  std::array<uint16_t, kTexelCount> texels_{};
};

// Same discipline as core::Tile's own size assertion: a PigmentTile is nothing
// but its texel buffer, so if this ever fails something grew the type by
// accident. 128*128*7*2 = 229376 = 224 KiB, against core::Tile's 128 KiB and
// under DESIGN-imaging.md §3's 256 KiB budget row for a pigment tile.
static_assert(sizeof(PigmentTile) == 224 * 1024,
              "one 128x128 7-channel f16 pigment tile must be exactly 224 KiB "
              "(DESIGN-imaging.md §3's pigment-tile row bounds it at 256 KiB)");

// The sparse store a Pigment layer holds, from the same template core::Tile's
// `TileStore` is an alias of. See core/TileStore.hpp on why that is one
// template rather than two classes.
using PigmentTileStore = TileStoreOf<PigmentTile>;

}  // namespace np
