#include <cmath>
#include <optional>
#include <string>

#include "app/CommandSupport.hpp"
#include "app/PixelOpBridge.hpp"
#include "core/SelectionMask.hpp"
#include "ops/Lens.hpp"
#include "ops/Pattern.hpp"

// app/CommandsPatterns -- the command rows for PLAN.md Phase 19 step 5's two
// parked P2 image ops: lens correction (PRD D22) and pattern define/fill
// (PRD D27).
//
// Its own file rather than a section of app/CommandsImage.cpp because those
// two ops are severable from everything else in phase 19 and are built
// separately; a shared file would be a shared merge conflict for no benefit.
//
// Every row here is an adapter and nothing more, which is app/CommandsImage's
// rule and applies unchanged: read the parameters, refuse what is missing or
// out of range **by name**, call the engine through the bridge that already
// exists, and translate the result through app/CommandSupport.hpp. Nothing
// below reimplements an op, and nothing below invents a default or a clamp
// that `LensParams` / `PatternFillParams` do not already carry.
//
// **Both pixel rows go through `applyPixelFilter()`** (app/PixelOpBridge.hpp),
// not because it is convenient but because that header exists so that a
// twenty-seventh op cannot answer "which layer, does it refuse, what
// rectangle, how does the selection bound it, when is the history entry
// recorded" differently from the twenty-six that came before. `define_pattern`
// does not, and cannot: it changes no pixel and records no history, so it has
// no answer to give to the last two of those questions. It still takes the
// same `pixelOpUnavailable()` precondition, so "which layer, and does it
// refuse" is answered once.
namespace np {
namespace {

// --------------------------------------------------------------------------
// lens_correct (PRD D22)
// --------------------------------------------------------------------------

CommandResult doLensCorrect(OpenDocument& doc, const JsonValue& params) {
  LensParams p;
  p.k1 = static_cast<float>(params.numberOr("k1", p.k1));
  p.k2 = static_cast<float>(params.numberOr("k2", p.k2));
  p.caRed = static_cast<float>(params.numberOr("ca_red", p.caRed));
  p.caBlue = static_cast<float>(params.numberOr("ca_blue", p.caBlue));

  const std::string kernelName = params.stringOr("kernel", "");
  if (!kernelName.empty()) {
    const std::optional<ResampleKernel> k = resampleKernelFromName(kernelName);
    if (!k) return commandRefused("refused: no resample kernel is named \"" + kernelName + "\".");
    p.kernel = *k;
  }

  // The frame is the document canvas, always, and is not a parameter.
  // ops/Lens.hpp section 3: the optical centre and the normalising radius are
  // properties of the picture, and letting a caller pass a different rectangle
  // would let an action recorded on a crop centre the lens somewhere the
  // photograph's own centre is not.
  p.frame = PixelRect{0, 0, doc.document.width, doc.document.height};

  // Refuse by name rather than letting `lensCorrectTiles()` return false,
  // which `computePixelFilter()` would read as a silent no-op -- exactly the
  // outcome docs/automation-plan.md §7 says a batch must never produce. The
  // engine keeps its own check; this one exists so the user gets a sentence.
  if (!std::isfinite(p.k1) || !std::isfinite(p.k2) || !std::isfinite(p.caRed) ||
      !std::isfinite(p.caBlue))
    return commandRefused("refused: lens_correct needs finite k1, k2, ca_red and ca_blue.");
  if (!lensParamsValid(p))
    return commandRefused(
        "refused: these lens coefficients fold the picture through itself -- the radial map is "
        "not increasing out to the corner, so two rings of the result would read the same ring of "
        "the source. Reduce k1/k2, or check their sign: positive corrects barrel.");

  return fromFilterResult(applyPixelFilter(doc, lensCorrectTiles, p, "Lens Correction"), doc,
                          "lens correction");
}

// --------------------------------------------------------------------------
// define_pattern (PRD D27, first half)
// --------------------------------------------------------------------------

CommandResult doDefinePattern(OpenDocument& doc, const JsonValue& params) {
  const std::string name = params.stringOr("name", "");
  if (name.empty())
    return commandRefused(
        "refused: define_pattern needs a name. A fill resolves its pattern by name, so an unnamed "
        "pattern is one nothing could ever ask for.");

  const Layer* target = activeLayerOf(doc);
  if (target == nullptr || !target->rgbTiles.has_value())
    return commandRefused("refused: define_pattern has no RGB layer to read.");

  // The selection's BOUNDS, not its coverage -- ops/Pattern.hpp says why at
  // length: a pattern is a rectangle by definition, so a lasso contributes its
  // box. Absent selection means the whole canvas, which is
  // `selectionCoverageAt()`'s "null means 1.0" rule read at rectangle scale
  // and is the same answer every other op here gives.
  PixelRect bounds{0, 0, doc.document.width, doc.document.height};
  bool boxed = false;
  if (doc.selection.has_value()) {
    const std::optional<SelectionBounds> sb = selectionBounds(*doc.selection);
    if (!sb)
      return commandRefused(
          "refused: the selection selects nothing, so there is no rectangle to define a pattern "
          "from.");
    bounds = PixelRect{sb->x0, sb->y0, sb->x1, sb->y1};
    boxed = true;
  }

  Pattern pattern;
  std::string error;
  if (!definePattern(*target->rgbTiles, bounds, name, &pattern, &error))
    return commandRefused(error);

  CommandResult r;
  r.ok = true;
  // **`changesPixels` is false and that is the point.** The replayer turns a
  // zero `texelsChanged` into a "this step did nothing" warning
  // (app/Command.hpp), and it would be wrong to raise it here: defining a
  // pattern is a real act that touches no texel, exactly as `select_layer` is.
  r.changesPixels = false;
  r.status = "define pattern: \"" + pattern.name + "\", " + std::to_string(pattern.width) + "x" +
             std::to_string(pattern.height) + " texels";
  if (boxed) {
    r.warnings.push_back(
        "the pattern is the selection's bounding rectangle; any part of the marquee that was not "
        "rectangular contributed its box, because a pattern has to tile");
  }
  sessionPatterns().define(std::move(pattern));
  return r;
}

// --------------------------------------------------------------------------
// fill_with_pattern (PRD D27, second half)
// --------------------------------------------------------------------------

CommandResult doFillWithPattern(OpenDocument& doc, const JsonValue& params) {
  const std::string name = params.stringOr("pattern", "");
  if (name.empty())
    return commandRefused("refused: fill_with_pattern needs the name of a defined pattern.");

  // **By name, never by index**, for docs/automation-plan.md §5's reason
  // applied one vocabulary over: an action replayed in a session whose
  // patterns were defined in a different order must not fill with a different
  // one. `resolveTarget()` makes the identical argument about layers.
  const Pattern* pattern = sessionPatterns().findByName(name);
  if (pattern == nullptr)
    return commandRefused("refused: no pattern named \"" + name +
                          "\" has been defined in this session. Define it first; patterns are "
                          "session state and are not saved with a document.");
  if (!pattern->valid())
    return commandRefused("refused: the pattern named \"" + name + "\" holds no texels.");

  PatternFillParams p;
  p.pattern = pattern;
  const double ox = params.numberOr("origin_x", 0.0);
  const double oy = params.numberOr("origin_y", 0.0);
  if (!std::isfinite(ox) || !std::isfinite(oy) || ox != std::floor(ox) || oy != std::floor(oy))
    return commandRefused("refused: fill_with_pattern's origin_x and origin_y are whole texels.");
  p.originX = static_cast<int32_t>(ox);
  p.originY = static_cast<int32_t>(oy);

  return fromFilterResult(applyPixelFilter(doc, patternFillTiles, p, "Fill With Pattern"), doc,
                          "pattern fill");
}

}  // namespace

void registerPatternCommands(std::vector<CommandSpec>* out) {
  out->push_back({"lens_correct",
                  "Lens Correction",
                  {"k1", "k2", "ca_red", "ca_blue", "kernel"},
                  pixelOpUnavailable,
                  doLensCorrect, /*selectionBounded=*/true});
  // **`selectionBounded` and `changesPixels` disagree here, and this is the
  // only row in the table where they do.** Defining a pattern touches no
  // texel, so the result reports `changesPixels == false` -- the adapter above
  // argues why. It is nonetheless bounded in the sense app/Command.hpp
  // defines: the source rectangle IS the selection's bounds, and an absent
  // selection silently means the whole canvas. A `define_pattern` recorded
  // under a live marquee and replayed with nothing selected therefore defines
  // a pattern the size of the document and reports success -- exactly the
  // failure app/Recorder §4 exists to refuse, and exactly what the old
  // `changesPixels` proxy let straight through.
  out->push_back(
      {"define_pattern", "Define Pattern", {"name"}, pixelOpUnavailable, doDefinePattern,
       /*selectionBounded=*/true});
  out->push_back({"fill_with_pattern",
                  "Fill With Pattern",
                  {"pattern", "origin_x", "origin_y"},
                  pixelOpUnavailable,
                  doFillWithPattern, /*selectionBounded=*/true});
}

}  // namespace np
