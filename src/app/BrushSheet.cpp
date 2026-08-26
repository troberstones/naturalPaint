#include "app/BrushSheet.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/StrokeSession.hpp"
#include "io/AbrBrushes.hpp"
#include "io/Capabilities.hpp"
#include "io/Export.hpp"

// app/BrushSheet -- `--brush-sheet <file.abr> <out.png>`, a contact sheet of
// every imported preset painting the same stroke.
//
// **The stroke is Photoshop's preview stroke, on purpose.** Photoshop draws
// each brush in its picker as one S-curve that tapers at both ends, and the
// whole point of copying that shape is that the two sheets can be laid side by
// side and compared directly. A different stroke would make every difference
// ambiguous: is that blob spacing, or is it just that our path curved harder?
//
// Two properties of that stroke matter, and they are the two the brushes
// actually react to:
//
//   * **Pressure ramps 0 -> 1 -> 0**, which is what produces the tapered ends.
//     Every one of Kyle's Runny Inkers links PRESSURE -> Size, so a preview
//     drawn at constant pressure would show none of what makes them look the
//     way they do.
//   * **Direction sweeps through a full period**, so a dab that orients itself
//     to the stroke turns through every angle. This is the half naturalPaint
//     cannot presently honour -- see the note this tool prints -- and drawing
//     it anyway is what makes the absence visible rather than theoretical.
//
// Pressure is applied the way the real UI applies it, and NOT by hand:
// `brushTipFor()` per sample, then `setTip()`, then `addPoint()`. That is
// exactly `ui/MacPaintUI.cpp`'s own per-frame sequence. Pressure is a HARDWARE
// source, so `StrokeSession` does not evaluate it per dab -- it evaluates the
// stroke-local ones (RANDOM, NOISE, VELOCITY, FADE) itself and takes everything
// else from the tip it was handed. Applying pressure here as well as through
// `brushTipFor()` would apply it twice.

namespace np {
namespace {

// The sheet's geometry, chosen to match the proportions of Photoshop's own
// picker cells so the two images can be compared without rescaling either.
//
// **These are MINIMA, not fixed sizes** -- see `fitCell()` below. A cell that
// is smaller than the brush it holds does not merely crop: the overflow lands
// in the neighbouring cell and is indistinguishable from that brush's own
// paint. A 284 px spatter brush drawn in a 132 px cell buried the preset under
// it completely, and the sheet still looked like a plausible sheet, which is
// the worst way for a diagnostic tool to be wrong.
constexpr int32_t kCellW = 600;
constexpr int32_t kCellH = 132;
constexpr int32_t kCols = 2;
constexpr float kMarginX = 60.0f;   // room for the taper to start and finish
constexpr float kAmplitude = 26.0f;  // peak-to-centre of the S-curve
constexpr int32_t kSamples = 240;    // path points per stroke, not dabs

// The largest distance from a dab's centre that any preset can actually paint,
// INCLUDING what the dynamics can do to it. A link onto Size can only scale
// within [rangeLo, rangeHi] and Size is a Multiply target, so `rangeHi` bounds
// the growth -- taking the max over links is an upper bound, not a guess, and
// an upper bound is exactly what a cell needs to be safe.
//
// **A rotated bitmap tip reaches past its own radius**, which cost this
// function a bug on its first day. `brushTipFor()` maps the LARGER of the
// bitmap's two dimensions onto the diameter, so an un-rotated tip fits inside
// `radius` -- but turn it and the corner swings out to the half-DIAGONAL,
// `radius * hypot(w, h) / max(w, h)`, up to `radius * sqrt(2)` for a square
// sample. Nine of twelve Runny Inkers and every one of the Spatter brushes
// carry a link onto Angle, so this is the common case and not the exotic one.
// It showed up as a spatter brush bleeding 83 stray pixels into the cell below
// it -- small enough to read as "that brush is just noisy" rather than as an
// overflow, which is exactly why it is worth computing rather than padding.
float widestRadius(const BrushPreset& p) {
  float grow = 1.0f;
  for (const BrushLink& l : p.links.links)
    if (l.target == DynamicTarget::Size && l.enabled) grow = std::max(grow, l.rangeHi);
  float reach = p.radius * grow;
  if (p.tipBitmap && p.tipBitmap->width > 0 && p.tipBitmap->height > 0) {
    const float w = static_cast<float>(p.tipBitmap->width);
    const float h = static_cast<float>(p.tipBitmap->height);
    reach *= std::hypot(w, h) / std::max(w, h);
  }
  return reach;
}

// Photoshop's cell background and stroke colour, so a side-by-side comparison
// is not also a comparison of two different colour schemes.
constexpr float kBackdrop = 0.25f;

}  // namespace

// The A/B knob. Iterating on how a dynamics graph should be INTERPRETED needs
// the same brushes drawing the same stroke with one rule changed and nothing
// else, because the alternative -- eyeballing one sheet and reasoning about
// what a different rule would have done -- is how a confound gets mistaken for
// a finding. Spacing and hardness both change apparent stroke width here, so a
// correlation between "has a RANDOM -> Size link" and "paints thin" proves
// nothing on its own.
BrushLinkSet experimentLinks(const BrushLinkSet& in, const char* experiment) {
  if (experiment == nullptr || std::string(experiment) == "as-imported") return in;
  BrushLinkSet out;
  const std::string mode = experiment;
  for (const BrushLink& l : in.links) {
    // Drop the jitter link on Size entirely: isolates how much of the width
    // loss is the second multiplicative size factor and how much is falloff
    // and spacing.
    if (mode == "no-random-size" && l.source == DynamicSource::Random &&
        l.target == DynamicTarget::Size)
      continue;
    // Keep the jitter but stop its floor from multiplying against the control
    // link's floor. Photoshop's Minimum Diameter is ONE floor under the final
    // size, not a floor per contributing row, so two rows each carrying it
    // multiply it to its own square.
    if (mode == "floor-once" && l.source == DynamicSource::Random &&
        l.target == DynamicTarget::Size) {
      BrushLink j = l;
      j.rangeLo = 1.0f - (l.rangeHi - l.rangeLo) * 0.5f;  // half the span, hung off full size
      j.rangeHi = 1.0f;
      out.links.push_back(j);
      continue;
    }
    out.links.push_back(l);
  }
  return out;
}

int runBrushSheet(const char* abrPath, const char* outPath, const char* experiment) {
  std::ifstream in(abrPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "brush-sheet: cannot open %s\n", abrPath);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  const AbrImportResult imported = importAbrBrushes(bytes);
  if (!imported.ok) {
    std::fprintf(stderr, "brush-sheet: import failed: %s\n", imported.error.c_str());
    return 1;
  }
  if (imported.presets.empty()) {
    std::fprintf(stderr, "brush-sheet: no presets in %s\n", abrPath);
    return 1;
  }

  MixboxLut lut;
  if (!lut.load(NP_MIXBOX_LUT)) {
    std::fprintf(stderr, "brush-sheet: could not load the Mixbox LUT at %s\n", NP_MIXBOX_LUT);
    return 1;
  }

  // Grow the cell until the widest brush in the library fits inside one, with
  // its S-curve amplitude and a dab's radius clear on every side. One size for
  // the whole sheet rather than per row, so that two cells side by side are
  // still the same scale and can be compared by eye without measuring.
  float widest = 0.0f;
  for (const BrushPreset& p : imported.presets) widest = std::max(widest, widestRadius(p));
  const int32_t cellH = std::max(
      kCellH, static_cast<int32_t>(std::ceil(2.0f * (widest + kAmplitude) + 24.0f)));
  const float marginX = std::max(kMarginX, widest + 20.0f);
  // **The stroke's TRAVEL has to scale with the brush too, not just the
  // margins.** Sizing the margins alone left a 142 px-radius spatter brush
  // with 240 px of path -- under two dab-diameters, three or four dabs total
  // -- which renders as a clump and says nothing about how the brush behaves
  // along a stroke. That is a preview that is present but useless, which is
  // harder to notice than one that is missing. Six times the widest reach
  // gives roughly a dozen dab-diameters of path at any brush size, so the
  // taper and the direction sweep both have room to show.
  const float travel = std::max(240.0f, widest * 6.0f);
  const int32_t cellW =
      std::max(kCellW, static_cast<int32_t>(std::ceil(2.0f * marginX + travel)));

  const int32_t rows = static_cast<int32_t>((imported.presets.size() + kCols - 1) / kCols);
  const int32_t width = cellW * kCols;
  const int32_t height = cellH * rows;

  OpenDocument od = makeBlankOpenDocument(width, height, WorkingSpace{}, "brush-sheet");
  TileStore& tiles = *od.document.layers[0].rgbTiles;
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const PixelCoord p{x, y};
      tiles.getOrCreate(tileCoordAt(p))
          .writePixel(tileLocalOffset(p), {kBackdrop, kBackdrop, kBackdrop, 1.0f});
    }
  }

  std::printf("brush-sheet: %zu presets, %dx%d, %d col(s), experiment=%s\n",
              imported.presets.size(), width, height, kCols,
              experiment != nullptr ? experiment : "as-imported");

  for (size_t i = 0; i < imported.presets.size(); ++i) {
    const BrushPreset& preset = imported.presets[i];
    const int32_t col = static_cast<int32_t>(i) % kCols;
    const int32_t row = static_cast<int32_t>(i) / kCols;
    const float x0 = static_cast<float>(col * cellW) + marginX;
    const float x1 = static_cast<float>((col + 1) * cellW) - marginX;
    const float cy = static_cast<float>(row * cellH) + static_cast<float>(cellH) * 0.5f;

    // The preset, applied to a brush the same way selecting it in the panel
    // would -- white, in RGB mode, so the mark reads against the backdrop.
    BrushState brush;
    applyPresetToBrush(preset, brush);
    brush.tool = Tool::Brush;
    brush.colorMode = ColorMode::Rgb;
    brush.rgb = {1.0f, 1.0f, 1.0f};

    brush.links = experimentLinks(brush.links, experiment);

    StrokeSession stroke;
    std::string refusal;
    if (!stroke.begin(od, 0, brushTipFor(brush, lut, 0.0f), brush.tool, &refusal, &brush.links)) {
      std::fprintf(stderr, "brush-sheet: %s refused: %s\n", preset.name.c_str(), refusal.c_str());
      continue;
    }
    for (int32_t s = 0; s <= kSamples; ++s) {
      const float t = static_cast<float>(s) / static_cast<float>(kSamples);
      const float x = x0 + (x1 - x0) * t;
      // One full period, so the tangent turns through every direction. The
      // reference sheet's stroke rises, dips and rises again; this is that.
      const float y = cy - kAmplitude * std::sin(t * 2.0f * 3.14159265358979f);
      // 0 at both ends, 1 in the middle: the taper. `sin(pi t)` and not a
      // triangle because a triangle's corner at t=0.5 shows up as a visible
      // kink in the width of any brush with a strong PRESSURE -> Size link.
      const float pressure = std::sin(t * 3.14159265358979f);
      // PRESSURE SMOOTHING: the same per-sample sequence `ui/MacPaintUI.cpp`
      // uses (this file's own top comment), now including the EMA `begin()`
      // above reset for this stroke. Damps this ramp's own instantaneous
      // rate of change rather than the shape of the ramp itself -- the
      // taper still runs 0->1->0, just lagged a few samples at each end.
      stroke.setTip(brushTipFor(brush, lut, stroke.smoothPressure(pressure)));
      stroke.addPoint(x, y);
    }
    stroke.end();
    std::printf("  [%2zu] r%d c%d  %-44.44s radius %5.1f spacing %5.2f links %zu\n", i, row, col,
                preset.name.c_str(), static_cast<double>(preset.radius),
                static_cast<double>(preset.spacing), preset.links.links.size());
  }

  std::string error;
  if (!exportDocumentToFile(od.document, outPath, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                            ExportBitDepth::UInt8, &error)) {
    std::fprintf(stderr, "brush-sheet: export failed: %s\n", error.c_str());
    return 1;
  }
  std::printf("brush-sheet: wrote %s\n", outPath);

  if (imported.sampledTips > 0) {
    std::printf(
        "\nNOTE: %zu of %zu of these brushes have a sampled bitmap tip that the\n"
        "importer does not read, so what this sheet draws for them is a ROUND dab\n"
        "following the right path at the right spacing -- not this library's shape.\n",
        imported.sampledTips, imported.presets.size());
  }
  return 0;
}

}  // namespace np
