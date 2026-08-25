#pragma once

#include <array>
#include <cstdint>

#include "core/Document.hpp"
#include "core/Tile.hpp"

// core/Probe (PLAN.md "Phase 2 -- See a file", step 10: "Pixel probe
// reporting both linear and display values, and the eyedropper, which is
// the same sampling code writing to the foreground colour instead of a
// readout (PRD Q10). Sample size and sample-all-layers are parameters of
// the sample, not separate tools.").
//
// probePixel() below is that one sampling code: a pure, read-only Document
// query -- coordinate + parameters in, colour out -- with no UI attached.
//
// **It is still not the eyedropper tool, and now it has one.** This header
// used to say the tool could not be built because "PRD Q10's 'eyedropper
// picks into the foreground colour' needs a real foreground-colour concept
// to write to, and today's interactive brush state (app/AppState.hpp's
// BrushState) only holds a palette index, not an arbitrary RGB value".
// That blocker is gone: `BrushState` now carries a `ColorMode` and an
// arbitrary display-referred sRGB triple beside the palette index, and
// `ui/MacPaintUI.cpp`'s canvas block is the trivial wrapper this comment
// promised -- call probePixel(), copy the result into the foreground. The
// split the paragraph was really defending still holds and is why this file
// did not grow a tool: sampling is a Document query with no UI, and every
// consumer (the eyedropper, a future pixel-probe readout) is a wrapper, not
// a second sampling path.
namespace np {

// Which layer, or which part of the stack, one sample reads.
//
// **This was a `bool sampleAllLayers` and two states were one too few.**
// Photoshop offers three, and the third -- "Current & Below" -- is the one a
// painter actually reaches for: sampling the colour a new stroke will land
// *on top of*, without the layers above the one being worked on contaminating
// the answer. A bool cannot say it, and the rename (rather than a third
// bool, or an `int`) is what forced every existing caller to be looked at
// again; see this file's own note on the default below.
enum class ProbeSource {
  // Read `ProbeParams::activeLayerIndex`'s own stored colour, **ignoring its
  // `visible`, its `opacity` and its mask** -- it answers "what is on this
  // layer". Photoshop's "Current Layer". See Probe.cpp for why a hidden
  // layer must stay probeable in this mode.
  CurrentLayer,

  // Composite the whole stack with core/Composite's `over`, bottom to top,
  // **honouring `visible` and `opacity`** -- it answers "what colour does the
  // document show here", the same question io/Export's flattener answers,
  // through the same `blendPixel()`. Photoshop's "All Layers".
  AllLayers,

  // Composite the stack bottom-up and **stop after `activeLayerIndex`**, so
  // everything above the active layer is ignored. Photoshop's "Current &
  // Below".
  //
  // **It honours `visible` and `opacity`, exactly as `AllLayers` does, and
  // that is a deliberate choice against the other half of the asymmetry
  // above.** The rule that decides it is not "does the active layer appear in
  // this mode" but *what question the mode asks*: `CurrentLayer` asks what a
  // layer holds (storage), and `AllLayers` and `ActiveAndBelow` both ask what
  // a stack shows (display). A truncated stack is still a stack, so a hidden
  // layer inside it contributes nothing here for the same reason it
  // contributes nothing to the document -- including when the hidden layer is
  // the active one, which reads as "nothing is showing there", the truth.
  // The workflow the asymmetry exists to protect (a layer hidden while being
  // worked on) is not lost: `CurrentLayer` still reads it, which is precisely
  // why the two modes are kept apart rather than harmonised.
  ActiveAndBelow,
};

// Photoshop's own sample-size set, and its own labels for them.
//
// **Edge lengths, not areas.** The user asked for these as "1 px, 9px, etc.",
// which is the *area* reading of the same set -- 9 is 3x3 -- and every label
// below spells the box out ("3 by 3 Average") precisely so that no reader has
// to decide which of the two a bare "9" meant. `ProbeParams::sampleSize` is
// the edge, matching Photoshop's control and matching the arithmetic
// (`sampleSize * sampleSize` texels).
inline constexpr int32_t kProbeSampleSizes[] = {1, 3, 5, 11, 31, 51, 101};
inline constexpr const char* kProbeSampleSizeLabels[] = {
    "Point Sample",      "3 by 3 Average",   "5 by 5 Average",  "11 by 11 Average",
    "31 by 31 Average",  "51 by 51 Average", "101 by 101 Average",
};
inline constexpr int kProbeSampleSizeCount =
    static_cast<int>(sizeof(kProbeSampleSizes) / sizeof(kProbeSampleSizes[0]));
static_assert(sizeof(kProbeSampleSizeLabels) / sizeof(kProbeSampleSizeLabels[0]) ==
                  static_cast<size_t>(kProbeSampleSizeCount),
              "one label per sample size -- a size without a label is a menu row that "
              "reads as blank, and a label without a size is a row that picks the wrong box");

// The label for an arbitrary edge length: the table's own string when the
// size is one of Photoshop's seven, else nullptr. Callers that must show
// *something* for an off-table size format it themselves; returning nullptr
// rather than a fabricated "N by N Average" keeps the table the single source
// of the wording the UI shows.
const char* probeSampleSizeLabel(int32_t sampleSize) noexcept;

// How much of the document one sample averages, and which layer(s) it
// reads. A real, first-class parameter of the sample (per step 10's "not
// separate tools") rather than a hardcoded 1x1 read.
struct ProbeParams {
  // Edge length, in document pixels, of the square box averaged around the
  // probed coordinate. 1 = a single texel ("point sample"); 3, 5, ...
  // average an NxN box, Photoshop's "3 by 3 Average" / "5 by 5 Average"
  // shape -- `kProbeSampleSizes` above is that whole set. Must be >= 1 --
  // probePixel() clamps anything <= 0 up to 1 rather than misbehaving on a
  // bad caller value. Any positive size works; the table is what the UI
  // offers, not what this function accepts.
  //
  // Odd sizes centre exactly on the probed coordinate. Even sizes are
  // still handled (never a crash or UB) but the box is biased half a
  // texel toward +x/+y, because there is no ambiguity-free way to centre
  // an even-width box on a single integer coordinate; this reuses the same
  // floor-biased direction core/Tile.hpp's tileCoordAt()/tileLocalOffset()
  // already commit to for negative coordinates, rather than inventing a
  // second rounding convention.
  //
  // **The box is clipped to the document, and only the texels inside it are
  // averaged** -- see probePixel()'s own comment for the bug that was, why it
  // was invisible, and why "outside the canvas" and "inside but unpainted"
  // must not be the same thing.
  int32_t sampleSize = 1;

  // Which layer, or which part of the stack, to read (see `ProbeSource`).
  //
  // **This field was `bool sampleAllLayers` and the rename is load-bearing.**
  // The precedent in this repo runs the other way -- the pigment-selection
  // track added a `const Selection*` to `depositDabs()` with *no* default
  // specifically so all nine call sites had to be edited, on the grounds that
  // "nothing had to change" is indistinguishable from "nothing was checked".
  // A struct member cannot express that (an aggregate member without a
  // default is a member every brace-init has to fill, which would touch forty
  // sites that have no opinion about layer selection at all), so the same
  // force is applied by *renaming*: every one of the twelve sites that ever
  // wrote `sampleAllLayers = ...` stopped compiling and had to state which of
  // three modes it meant, while the sites that only ever wrote `ProbeParams{}`
  // keep exactly the meaning they had. That is the honest split -- a forced
  // look for everyone who had an opinion, and no churn for anyone who did not.
  //
  // Only `over` exists; a layer whose blend names something else is
  // composited as `over` here, silently, because a probe returns a colour and
  // has nowhere to put a warning. The boundaries that produce a *file* --
  // io/Export's `exportDocument()` and io/NpaintFile's `saveNpaint()` -- do
  // carry core/Composite's warning by name. **That claim is not widened by
  // the third mode**: `ActiveAndBelow` composites through the identical
  // `blendPixel()` walk `AllLayers` does and is silent in exactly the same
  // way, no more and no less.
  ProbeSource source = ProbeSource::CurrentLayer;

  // Which layer `CurrentLayer` reads, and which layer `ActiveAndBelow` stops
  // at (inclusive). core::Document has no "active layer" concept -- that
  // belongs to app-level selection state, out of scope for this module -- so
  // callers that track one pass it here; callers that don't (e.g. --selftest,
  // against a single-layer Document) can rely on the default, index 0.
  // Out-of-range or non-RGB-kind indices are handled the same as "nothing
  // there": a fully transparent sample, never a crash.
  int32_t activeLayerIndex = 0;
};

// One probed colour, in both of PLAN.md step 10's required forms.
//
// Both are straight (un-premultiplied) alpha. core::Tile stores
// premultiplied colour (DESIGN-imaging.md §2 "Alpha: premultiplied
// (associated)"; io/ImageIO.cpp's writeDecodedImageIntoLayer premultiplies
// on write, rgb *= a). A probe reports "the colour at this point" the way
// a user would recognise it -- straight, not dimmed toward black at low
// alpha -- so probePixel() un-premultiplies when reading, mirroring at
// this read boundary the same split io/ImageDecode.hpp's header comment
// documents at the opposite (decode) boundary.
struct ProbeSample {
  // Linear-light, straight-alpha RGBA -- the colour as the working space
  // actually stores it, no transfer function applied. {0,0,0,0} (fully
  // transparent black) both when nothing has ever been painted at the
  // probed location and, per the un-premultiply guard above, as the
  // defined RGB for any fully-transparent result.
  std::array<float, 4> linear{0.0f, 0.0f, 0.0f, 0.0f};

  // linear's RGB channels run through color::srgbEncode() for display.
  // Alpha is carried through unchanged -- alpha is opacity, not light, and
  // is never gamma-encoded, the same policy io/ImageDecode.hpp's decode
  // side already documents for the inverse (encoded-file -> linear)
  // direction.
  std::array<float, 4> display{0.0f, 0.0f, 0.0f, 0.0f};
};

// Samples `doc` at document-pixel coordinate `at`, per `params`. Read-only:
// goes through TileStore::find(), never getOrCreate(), so probing an area
// nobody has painted allocates nothing. Never crashes -- an out-of-bounds
// coordinate, an unoccupied tile, an empty/no-RGB-layer Document, or an
// out-of-range activeLayerIndex all read back ProbeSample{}, fully
// transparent black in both linear and display form, the same implicit
// value core::Tile itself gives an unwritten texel.
ProbeSample probePixel(const Document& doc, PixelCoord at, const ProbeParams& params = {});

// The half-open document-space box `probePixel()` will actually average for
// these parameters: the `sampleSize x sampleSize` box centred on `at`,
// intersected with `[0,doc.width) x [0,doc.height)`. `x1 <= x0 || y1 <= y0`
// means the box misses the document entirely and the sample is transparent
// black.
//
// Exposed rather than left inside probePixel() because the clipping is the
// part of this module a test has to be able to state independently -- an
// assertion that reads the box back from probePixel()'s own averaging would
// be checking the arithmetic against itself. It is also what a UI needs to
// draw the sampled region on the canvas.
struct ProbeBox {
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // half-open: [x0,x1) x [y0,y1)
  int32_t texels() const noexcept {
    return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
  }
};
ProbeBox probeSampleBox(const Document& doc, PixelCoord at, int32_t sampleSize) noexcept;

}  // namespace np
