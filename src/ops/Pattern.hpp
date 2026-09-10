#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/TileStore.hpp"
#include "io/PsPatterns.hpp"
#include "ops/Roi.hpp"

// ops/Pattern (PRD D27, PLAN.md Phase 19 step 5 / docs/automation-plan.md §6
// step 8) -- Define Pattern and Fill With Pattern.
//
// ==========================================================================
// (1) A DEFINED PATTERN IS SESSION STATE, NOT DOCUMENT DATA
// ==========================================================================
//
// The question is the one `app/DocumentLifecycle.hpp:232` already answers for
// the active selection, and it is worth answering the same way rather than by
// habit, because the two look alike and the reasoning is not the obvious one.
//
// That comment's test is **not** "did the user explicitly ask for it". It is:
// `core::HistoryEntry` holds a whole `core::Document` (core/History.hpp), so
// anything inside `Document` is restored by Undo. A selection is therefore out
// of `Document`, because otherwise "every undo would restore a marquee along
// with the pixels, and drawing a marquee would be an undoable act. Neither is
// what an editor does."
//
// A defined pattern fails that test in exactly the same way. Put it in
// `Document` and Define Pattern becomes an undoable step that changed no
// pixel, and one Undo of an actual paint stroke silently un-defines a pattern
// the user made afterwards. That is the identical surprise.
//
// **Where the analogy does NOT hold is the interesting half**, and it is why
// the answer is not simply "so store it like a saved selection channel".
// `core::saveSelectionAsChannel()` moves coverage across the session/document
// line on an explicit command precisely because a saved selection *is* picture
// data -- a named `core::AlphaChannel` in `Document::channels`, which is in
// history and in the file, and undoing its creation is a real content undo. A
// pattern is not picture data for the document it came from. Its whole purpose
// is to be defined in one document and filled into **another**, which document
// data cannot do by construction, and which a saved channel deliberately
// cannot do either. Same rule, opposite answer, because the two things are
// meant to cross different lines.
//
// So: a `PatternStore`, held for the session, reached through
// `sessionPatterns()`.
//
// **Two consequences, named rather than left to be found.**
//
//   1. A defined pattern does not survive a restart and is not written into
//      any file. That is a real gap -- Photoshop's patterns live in a preset
//      library on disk -- and the place to close it is a JSON library beside
//      `export-presets.json` (io/ExportAs.cpp:879's established pattern),
//      **not** `Document`. It is absent, not half-built.
//   2. `sessionPatterns()` is a process-level accessor rather than a member of
//      `AppState`, and that is forced rather than chosen:
//      docs/automation-plan.md §7 requires that `applyCommand()` never reach
//      for session state through an `AppState&`, because a command that does
//      cannot run under `--batch` and its batch behaviour would silently
//      differ from its interactive one. `fill_with_pattern` has to resolve a
//      pattern by name from inside `applyCommand(OpenDocument&, ...)`, so the
//      store has to be reachable without one.
//
// ==========================================================================
// (2) WHY `Pattern` IS NOT `PsPattern`, AND WHY THE TWO ARE STILL ONE FAMILY
// ==========================================================================
//
// `io/PsPatterns.hpp` already decodes the `patt` block of a Photoshop `.abr`
// into a `PsPattern`, and reusing that struct here was the obvious move. It is
// the wrong one, and the reason is written in that header in as many words:
// `PsPattern::height8` is "a scalar height field -- what paper tooth actually
// is -- so an RGB pattern collapses to luminance on the way in rather than
// carrying two channels nothing samples." That is the right decision for the
// thing it was built for -- `brush/Grain`'s paper texture, which samples a
// height and nothing else -- and it is a silently lossy one for a **fill**,
// where the user's whole expectation is that the colours come back.
//
// So `Pattern` carries the working space instead: linear-light, premultiplied
// RGBA float, the same space `ops/Transform`'s `TransformImage` uses and the
// same space `core::TileStore` holds in half. What keeps this from being a
// second parallel universe is `patternFromPsPattern()`: the `.abr` reader
// stays the only decoder of Photoshop pattern bytes in this build, and its
// output feeds in here through one named, lossless-in-the-direction-it-goes
// conversion. There is no second reader, which is the thing that would
// actually drift.
//
// The conversion up from `PsPattern` is greyscale-to-RGB and opaque, because
// that is genuinely all the information `height8` holds -- the loss happened
// in the `.abr` reader, on purpose, and this function does not pretend to undo
// it.
//
// ==========================================================================
// (3) THE TILING RULE, WHICH IS WHERE THE OFF-BY-ONE LIVES
// ==========================================================================
//
// A fill reads the pattern texel
//
//     (euclideanMod(x - originX, width), euclideanMod(y - originY, height))
//
// and the word that matters is **euclidean**. C's `%` is truncating: for
// negative operands `-1 % 8` is `-1`, not `7`. A document texel at x = -1 is
// entirely ordinary -- `ops/Offset` hit the same thing and says so at
// `offsetSourceTexel()` -- and a truncating modulus there indexes backwards
// off the front of the pattern buffer. Even inside a canvas that starts at 0,
// a non-zero `origin` puts negative operands into the expression immediately.
//
// The seam is the assertion that catches this and it is not the one people
// write: filling a `2W x 2H` region with a `W x H` pattern must reproduce the
// pattern **four times exactly**, which means the texel at x = W - 1 and the
// texel at x = W come from opposite ends of the pattern with nothing dropped
// and nothing repeated. An off-by-one shows as a one-texel stutter at every
// tile boundary and nowhere else, which no eyeball catches on real photographic
// paper and no average-based metric notices either.
namespace np {

// One defined pattern, in the working space.
struct Pattern {
  // A stable identity, not a display name. Generated when a pattern is defined
  // from a selection and carried across verbatim from `PsPattern::id` (the
  // 36-character UUID that joins a brush's `Txtr` descriptor to its paper)
  // when it came from an `.abr`. Nothing keys off the ordinal position in the
  // store, for docs/automation-plan.md §5's reason: an id in a file must not
  // be a position in a vector.
  std::string id;
  // What the user typed, or the pattern's own name out of the `.abr`. This is
  // what `fill_with_pattern` resolves against, and it is what a user sees, so
  // it is UI-facing text and may be reworded by whoever owns the pattern.
  std::string name;
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4, row-major, top to bottom. Linear-light, PREMULTIPLIED
  // RGBA -- `core/TileStore.hpp`'s space, not `io/ImageDecode.hpp`'s straight
  // one. Defining a pattern from a layer is a copy, with no premultiply
  // conversion anywhere in it, which is what makes a define/fill round trip on
  // an untouched region exact rather than exact-to-a-tolerance.
  std::vector<float> px;

  size_t sampleCount() const noexcept {
    return static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  }
  bool valid() const noexcept {
    return width > 0 && height > 0 && px.size() == sampleCount();
  }
};

// The largest pattern edge a define will accept, matching
// `kMaxPatternDimension` for one reason to remember rather than two. A user
// can select the whole of a large canvas, and a pattern is held resident for
// the session; refusing by name beats holding 400 MB nobody asked to hold.
inline constexpr uint32_t kMaxDefinedPatternDimension = 4096;

// Reads `bounds` out of `src` and stores it as a pattern.
//
// `bounds` is the caller's rectangle -- the selection's `selectionBounds()`
// when there is a selection, the canvas otherwise; this function does not
// decide that, because "what does the user mean by the current selection" is a
// question `app/` answers and `ops/` must not answer differently.
//
// **The selection's coverage is deliberately NOT applied.** A pattern is a
// rectangle by definition -- it has to tile -- so a non-rectangular marquee
// contributes its bounding box and nothing else. Multiplying the coverage in
// would produce a pattern with soft transparent corners that tile into a grid
// of ghosts, which is a plausible-looking wrong answer rather than a feature.
// Photoshop refuses a feathered selection outright; this takes the box and
// says so through `*warningOut`, because refusing a rectangle the user drew
// with a 1px feather would be worse.
//
// Returns false and sets `*errorOut` for an empty or over-large rectangle.
bool definePattern(const TileStore& src, const PixelRect& bounds, std::string name,
                   Pattern* out, std::string* errorOut);

// The `.abr` bridge of section 2. Greyscale in, opaque RGB out.
Pattern patternFromPsPattern(const PsPattern& source);

// The single pattern texel one document texel reads. Exposed for the same
// reason `offsetSourceTexel()` is: it is the whole semantic content of the op,
// the euclidean modulus included, and a test that retyped it would be checking
// its own copy.
PixelCoord patternSourceTexel(const Pattern& pattern, int32_t originX, int32_t originY,
                              PixelCoord doc) noexcept;

struct PatternFillParams {
  // Not owned. Null is refused rather than treated as "fill with nothing".
  const Pattern* pattern = nullptr;
  // Where the pattern's own (0, 0) lands in document space. The tiling phase.
  int32_t originX = 0;
  int32_t originY = 0;
};

bool patternFillParamsValid(const PatternFillParams& p) noexcept;

// The engine, in `app/PixelOpBridge.hpp`'s dispatch shape. Every texel of
// `outRect` is REPLACED by its pattern texel; the selection does the bounding,
// upstream in `compositeFilterResult()`, exactly as it does for every other
// menu pixel op. A pattern texel with alpha 0 therefore erases through the
// selection rather than leaving what was underneath, which is what "fill"
// means and is the same answer `ops/Gradient` gives.
bool patternFillTiles(const TileStore& src, const PixelRect& outRect,
                      const PatternFillParams& p, TileStore* dst);

// --- the session store ---------------------------------------------------
//
// A vector, not a map: the count is tens, every lookup is by name or id, and a
// map would be a second thing to keep in step -- `findCommand()`'s own
// argument, applied to the same size of problem.
class PatternStore {
 public:
  // Replaces any existing pattern with the same NAME, so defining twice under
  // one name updates rather than shadowing. Ids stay unique; the replaced
  // pattern's id goes with it.
  void define(Pattern pattern);
  const Pattern* findByName(std::string_view name) const noexcept;
  const Pattern* findById(std::string_view id) const noexcept;
  size_t size() const noexcept { return patterns_.size(); }
  const Pattern& at(size_t i) const { return patterns_[i]; }
  void clear() noexcept { patterns_.clear(); }

 private:
  std::vector<Pattern> patterns_;
};

// The session's patterns. Section 1 for why this is reachable without an
// `AppState&` and why it is not in `Document`.
PatternStore& sessionPatterns();

}  // namespace np
