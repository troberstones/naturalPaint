#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "app/AdjustmentOps.hpp"
#include "app/CommandsImage.hpp"
#include "app/CommandSupport.hpp"
#include "app/CropTool.hpp"
#include "app/FilterOps.hpp"
#include "app/TransformSession.hpp"
#include "core/LayerGeometry.hpp"
#include "core/SelectionMask.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/CommandsImage -- the command rows for everything that changes pixels or
// the document's own geometry: the Filter menu's seven, Image > Adjustments'
// nineteen (four of which are auto solvers), and the four document commands.
//
// Every one of them is an adapter and nothing more: read the parameters,
// refuse what is missing or out of range by name, call the applier that
// already exists, and translate its result through app/CommandSupport.hpp.
// **No adapter here may reimplement an op**; if a value needs clamping or a
// default, the applier's own params struct is where that already lives.
//
// ==========================================================================
// (1) The three id prefixes, and why the prefix is part of the format
// ==========================================================================
//
// `filter_`, `adjust_`, and the four document commands with no prefix at all
// (`image_size`, `canvas_size`, `crop_to_selection`, `trim_to_content`).
//
// This is a one-time choice and it is load-bearing, because an id is written
// into a `.npaction` file and is **never reused for a different meaning**
// (app/Command.hpp §3). Renaming `filter_sharpen` later to drop the prefix
// would break every file that already contains it. So the prefixes are chosen
// against the vocabulary each command comes from -- `app/FilterOps.hpp`'s
// appliers and `app/AdjustmentOps.hpp`'s -- and not against the menu each
// happens to sit in today. Menu text is UI copy and gets reworded; the two
// header files are the actual sources of these commands.
//
// The document four carry no prefix for the same reason: their vocabulary is
// "the document itself", not a menu called Image, and `image_size` was already
// registered under that name before this file was filled in.
//
// ==========================================================================
// (2) Out of range is a REFUSAL, and it names the parameter
// ==========================================================================
//
// `filter_gaussian_blur` refusing a sigma of zero is the worked pattern, and
// it refuses rather than clamping for a reason specific to batch: in the UI a
// clamped parameter is a slider that will not go further, which the user can
// see. In a batch it is thirty files written with a filter that did nothing
// and reported success -- docs/automation-plan.md §7's "a silent no-op is the
// failure mode this feature is built to have".
//
// So the identity request of a magnitude parameter is refused, by name,
// wherever the applier's own header identifies one ("0 is the identity"). The
// same applies to a parameter present but of the wrong JSON type: `numberOr()`
// answers its fallback for a string, so `"sigma": "four"` would have run a
// default-parameter filter and said it worked. Every reader below therefore
// checks the type before the value, and every refusal quotes the key.
//
// **What is deliberately NOT refused here**: anything the applier's own params
// struct already documents as clamped or defaulted. `AutoLevelsParams::
// clipFraction` documents a clamp to [0, 0.49] and argues for the bound; a
// second copy of 0.49 in this file would be the drift app/PixelOpBridge.hpp
// exists to prevent. A negative clip fraction IS refused, because a negative
// fraction of pixels is not a quantity the clamp could be said to be
// interpreting -- it is a caller bug the clamp would hide.
//
// ==========================================================================
// (3) The selection is not this file's business
// ==========================================================================
//
// Every applier below is already selection-bounded through
// `app/PixelOpBridge.hpp`'s shared `applyPixelFilter()`, and the crops read
// `OpenDocument::selection` themselves. Nothing here reads or writes a
// selection, and nothing here should: recording and replaying the selection is
// a separate command family (docs/automation-plan.md §7 -- a selection is
// session state and is never in a document file). An adapter that reached for
// it would be the second answer to "what does this op cover", which is exactly
// the split app/PixelOpBridge.hpp's header refuses to allow.
namespace np {
namespace {

// ==========================================================================
// The parameter readers
// ==========================================================================
//
// One shape, used by every adapter: **return the empty string on success, and
// the refusal sentence otherwise**, writing through a pointer that is
// pre-loaded with the applier's own default. That ordering is the whole point
// -- the caller writes
//
//     EmbossParams p;                       // the applier's defaults
//     std::string why = readNumber(params, id, "depth", kOptional, &p.depth);
//
// so a value this file does not receive is the value the params struct itself
// declares, and there is no second table of defaults here to drift from it
// (the house rule: "if a value needs a default or a clamp, the applier's own
// params struct already has it").
//
// A key that is absent is the default. A key that is present but of the wrong
// type is a REFUSAL, not a default: `JsonValue::numberOr()` would answer the
// fallback for `"sigma": "four"`, and a filter that ran with its default
// parameter and reported success is precisely the silent-wrong-answer this
// whole feature is built to avoid.
constexpr bool kRequired = true;
constexpr bool kOptional = false;

std::string refuseMissing(const char* id, const char* key) {
  return std::string("refused: ") + id + " needs a \"" + key + "\".";
}

std::string refuseValue(const char* id, const char* key, const std::string& mustBe) {
  return std::string("refused: ") + id + "'s \"" + key + "\" must be " + mustBe + ".";
}

std::string readNumber(const JsonValue& params, const char* id, const char* key, bool required,
                       float* io) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || v->isNull()) return required ? refuseMissing(id, key) : std::string();
  if (!v->isNumber()) return refuseValue(id, key, "a number");
  const double d = v->asNumber();
  // Non-finite is caught here rather than being left to the engine's own
  // `xParamsValid()`, so the sentence can name the key. The engine's predicate
  // knows the request is invalid but not which of its fields carried the NaN.
  if (!std::isfinite(d)) return refuseValue(id, key, "a finite number");
  *io = static_cast<float>(d);
  return {};
}

// A count of texels, a level count, an offset. **Whole numbers, refused rather
// than truncated**: 1.5 texels is not an emboss offset, and silently flooring
// it is how a hand-edited action file comes to mean something other than what
// it visibly says.
std::string readWhole(const JsonValue& params, const char* id, const char* key, bool required,
                      int32_t* io) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || v->isNull()) return required ? refuseMissing(id, key) : std::string();
  if (!v->isNumber()) return refuseValue(id, key, "a number");
  const double d = v->asNumber();
  if (!std::isfinite(d) || d != std::floor(d)) return refuseValue(id, key, "a whole number");
  if (d < -2147483648.0 || d > 2147483647.0) return refuseValue(id, key, "within int32 range");
  *io = static_cast<int32_t>(d);
  return {};
}

std::string readBool(const JsonValue& params, const char* id, const char* key, bool* io) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || v->isNull()) return {};
  if (!v->isBool()) return refuseValue(id, key, "true or false");
  *io = v->asBool();
  return {};
}

// An RGB triple, a per-band lift, a matrix row: JSON arrays of a fixed length.
// The length is checked and named, because a two-element "colour" would
// otherwise leave the third channel at whatever the params struct defaulted to
// and produce a picture nobody asked for.
template <size_t N>
std::string readFixedArray(const JsonValue& params, const char* id, const char* key, bool required,
                           std::array<float, N>* io) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || v->isNull()) return required ? refuseMissing(id, key) : std::string();
  if (!v->isArray() || v->size() != N)
    return refuseValue(id, key, "an array of " + std::to_string(N) + " numbers");
  for (size_t i = 0; i < N; ++i) {
    const JsonValue& e = v->at(i);
    if (!e.isNumber() || !std::isfinite(e.asNumber()))
      return refuseValue(id, key, "an array of " + std::to_string(N) + " finite numbers");
    (*io)[i] = static_cast<float>(e.asNumber());
  }
  return {};
}

// An enum-valued parameter, always by NAME through the pair that lives beside
// the enum itself (ops/Blur.hpp's `blurKindFromName()` and its three
// siblings). Never by ordinal -- docs/automation-plan.md §5, and app/Command.hpp
// §3: the enum is appended to, and a file keyed by position is a file that
// changes meaning when someone adds a value.
template <class Enum, class FromName>
std::string readEnumByName(const JsonValue& params, const char* id, const char* key,
                           FromName fromName, Enum* io) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || v->isNull()) return {};
  if (!v->isString()) return refuseValue(id, key, "a name, written as a string");
  const std::optional<Enum> parsed = fromName(v->asString());
  if (!parsed)
    return std::string("refused: ") + id + " does not know a \"" + key + "\" named \"" +
           v->asString() + "\".";
  *io = *parsed;
  return {};
}

// For the adjustments whose params struct is entirely optional fields with
// identity defaults -- Brightness/Contrast, Hue/Saturation, Color Balance,
// Black & White. A step naming none of them is a request to do nothing, which
// in a batch is a file written unmodified and reported as a success. Refused,
// naming every key that would have made it mean something.
//
// **Not the same as giving one of them a required flag.** Any ONE of gain,
// offset or gamma is a real Brightness/Contrast; demanding a specific one
// would force an action to spell out an identity value for a control the user
// never touched.
std::string requireAnyOf(const JsonValue& params, const char* id,
                         std::initializer_list<const char*> keys) {
  std::string list;
  for (const char* key : keys) {
    const JsonValue* v = params.find(key);
    if (v != nullptr && !v->isNull()) return {};
    if (!list.empty()) list += ", ";
    list += std::string("\"") + key + "\"";
  }
  return std::string("refused: ") + id + " needs at least one of " + list +
         "; a step that sets none of them would change nothing and report success.";
}

// The magnitude gate. `app/FilterOps.hpp`'s and `ops/Filters.hpp`'s headers
// each identify their own identity value ("0 is the identity"); this turns
// that into the refusal §2 above argues for. Separate from `readNumber()`
// because it is a claim about the OP, not about JSON: `adjust_invert` has no
// such parameter (a default-constructed Invert inverts, by ops/ToneOps.hpp's
// explicit decision) and must not be given one.
std::string requireAbove(const char* id, const char* key, float value, float floorValue) {
  if (value > floorValue) return {};
  return std::string("refused: ") + id + "'s \"" + key + "\" must be above " +
         std::to_string(static_cast<int>(floorValue)) +
         "; at or below it the filter is the identity, which in a batch is a file written "
         "unmodified and reported as a success.";
}

// ==========================================================================
// Filters -- app/FilterOps.hpp's seven
// ==========================================================================

CommandResult doGaussianBlur(OpenDocument& doc, const JsonValue& params) {
  float sigma = 0.0f;
  std::string why = readNumber(params, "filter_gaussian_blur", "sigma", kRequired, &sigma);
  if (why.empty()) why = requireAbove("filter_gaussian_blur", "sigma", sigma, 0.0f);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyGaussianBlur(doc, sigma), doc, "gaussian blur");
}

CommandResult doSharpen(OpenDocument& doc, const JsonValue& params) {
  // The one-click filter: `applySharpen()` takes a bare float and supplies
  // `kSharpenSigma` itself, so there is no radius key here and there must not
  // be one -- a radius the caller could set is `filter_unsharp_mask`, which is
  // the same engine with the radius exposed.
  float strength = 0.0f;
  std::string why = readNumber(params, "filter_sharpen", "strength", kRequired, &strength);
  if (why.empty()) why = requireAbove("filter_sharpen", "strength", strength, 0.0f);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applySharpen(doc, strength), doc, "sharpen");
}

CommandResult doUnsharpMask(OpenDocument& doc, const JsonValue& params) {
  UnsharpParams p;
  const char* kId = "filter_unsharp_mask";

  // `radius` is one key whose meaning depends on `blur_kind`, because
  // `BlurParams` is one struct with two mutually exclusive fields (`sigma` for
  // Gaussian, `boxRadius` for Box). Two keys -- "sigma" and "box_radius" --
  // was the alternative and is worse: a file setting both would have no
  // defined meaning, and a file setting the wrong one for its kind would be
  // silently an identity blur.
  std::string why = readEnumByName(params, kId, "blur_kind", blurKindFromName, &p.blur.kind);
  if (!why.empty()) return commandRefused(why);

  if (p.blur.kind == BlurKind::Box) {
    if (why = readWhole(params, kId, "radius", kRequired, &p.blur.boxRadius); !why.empty())
      return commandRefused(why);
    if (p.blur.boxRadius < 1)
      return commandRefused(refuseValue(kId, "radius", "at least 1 texel for a box blur"));
  } else {
    if (why = readNumber(params, kId, "radius", kRequired, &p.blur.sigma); !why.empty())
      return commandRefused(why);
    if (why = requireAbove(kId, "radius", p.blur.sigma, 0.0f); !why.empty())
      return commandRefused(why);
  }

  if (why = readNumber(params, kId, "amount", kRequired, &p.amount); !why.empty())
    return commandRefused(why);
  if (why = requireAbove(kId, "amount", p.amount, 0.0f); !why.empty()) return commandRefused(why);
  if (why = readNumber(params, kId, "threshold", kOptional, &p.threshold); !why.empty())
    return commandRefused(why);

  // The engine's own predicate has the last word on the whole request, rather
  // than this adapter re-deriving what a valid unsharp is. It cannot name the
  // offending field, which is why every field was checked individually first;
  // reaching this line means the request is invalid for a reason the fields
  // above do not cover, and saying so plainly beats guessing.
  if (!unsharpParamsValid(p))
    return commandRefused(std::string("refused: ") + kId +
                          " was given a combination ops/Filters cannot build a kernel from.");
  return fromFilterResult(applyUnsharpMask(doc, p), doc, "unsharp mask");
}

CommandResult doAddNoise(OpenDocument& doc, const JsonValue& params) {
  NoiseParams p;
  const char* kId = "filter_add_noise";
  std::string why = readNumber(params, kId, "amount", kRequired, &p.amount);
  if (why.empty()) why = requireAbove(kId, "amount", p.amount, 0.0f);
  if (why.empty())
    why = readEnumByName(params, kId, "distribution", noiseDistributionFromName, &p.distribution);
  if (why.empty()) why = readBool(params, kId, "monochrome", &p.monochrome);
  if (!why.empty()) return commandRefused(why);

  // **The seed is the one parameter here JSON cannot carry losslessly**, and
  // it is the one that decides whether the whole filter is reproducible
  // (ops/Filters.hpp calls the seed "the whole reproducibility contract").
  // `NoiseParams::seed` is a uint64; a JSON number is a double, which is exact
  // only up to 2^53. A seed above that would round on the way in, so a file
  // saying 18446744073709551615 would produce grain the file cannot name.
  // Refused rather than rounded: a silently different seed is a different
  // picture that still reports success.
  const JsonValue* seed = params.find("seed");
  if (seed != nullptr && !seed->isNull()) {
    if (!seed->isNumber()) return commandRefused(refuseValue(kId, "seed", "a number"));
    const double d = seed->asNumber();
    if (!std::isfinite(d) || d != std::floor(d) || d < 0.0 || d > 9007199254740992.0)
      return commandRefused(refuseValue(
          kId, "seed",
          "a whole number from 0 to 2^53; a JSON number is a double, so a larger seed would "
          "round on the way in and the grain would not be the grain the file names"));
    p.seed = static_cast<uint64_t>(d);
  }

  if (!noiseParamsValid(p))
    return commandRefused(std::string("refused: ") + kId +
                          " was given a request ops/Filters cannot draw noise for.");
  return fromFilterResult(applyAddNoise(doc, p), doc, "add noise");
}

CommandResult doEmboss(OpenDocument& doc, const JsonValue& params) {
  EmbossParams p;
  const char* kId = "filter_emboss";
  // `dx`/`dy` are texel offsets and so are read as WHOLE numbers -- see
  // `readWhole()`. Every integer pair including (0, 0) is legal here
  // (ops/Filters.hpp says so explicitly: a zero offset is the flat mid-grey
  // relief card, a defined answer and not a no-op), so there is no range check
  // on them and adding one would contradict the engine.
  std::string why = readWhole(params, kId, "dx", kOptional, &p.dx);
  if (why.empty()) why = readWhole(params, kId, "dy", kOptional, &p.dy);
  if (why.empty()) why = readNumber(params, kId, "depth", kOptional, &p.depth);
  if (why.empty()) why = readNumber(params, kId, "amount", kRequired, &p.amount);
  if (why.empty()) why = requireAbove(kId, "amount", p.amount, 0.0f);
  if (!why.empty()) return commandRefused(why);
  if (!embossParamsValid(p))
    return commandRefused(std::string("refused: ") + kId +
                          " was given a request ops/Filters cannot build a relief from.");
  return fromFilterResult(applyEmboss(doc, p), doc, "emboss");
}

CommandResult doMedian(OpenDocument& doc, const JsonValue& params) {
  MedianParams p;
  const char* kId = "filter_median";
  std::string why = readWhole(params, kId, "radius", kRequired, &p.radius);
  if (!why.empty()) return commandRefused(why);
  // radius 0 is documented as the exact identity, so it is refused for §2's
  // reason. `medianParamsValid()` still gets the last word on a negative one.
  if (p.radius < 1)
    return commandRefused(refuseValue(
        kId, "radius",
        "at least 1 texel; radius 0 is a 1x1 window, which ops/Filters short-circuits to a "
        "bit-exact copy"));
  if (!medianParamsValid(p))
    return commandRefused(std::string("refused: ") + kId +
                          " was given a window ops/Filters cannot build.");
  return fromFilterResult(applyMedian(doc, p), doc, "median");
}

CommandResult doMotionBlur(OpenDocument& doc, const JsonValue& params) {
  MotionBlurParams p;
  const char* kId = "filter_motion_blur";
  // **The key carries its unit** (docs/automation-plan.md §5's fourth rule).
  // `MotionBlurParams::angleRadians` is radians, and a key called "angle" in a
  // hand-edited file would be typed in degrees by half the people who touch
  // it. No conversion happens here -- naming the unit is the fix; converting
  // would be this adapter inventing arithmetic the engine does not have.
  std::string why = readNumber(params, kId, "angle_radians", kOptional, &p.angleRadians);
  if (why.empty()) why = readWhole(params, kId, "radius", kRequired, &p.radius);
  if (!why.empty()) return commandRefused(why);
  if (p.radius < 1)
    return commandRefused(
        refuseValue(kId, "radius", "at least 1 texel; radius 0 is the documented identity"));
  if (!motionBlurParamsValid(p))
    return commandRefused(std::string("refused: ") + kId +
                          " was given a smear ops/Filters cannot build.");
  return fromFilterResult(applyMotionBlur(doc, p), doc, "motion blur");
}

// ==========================================================================
// PLAN.md phases 8 and 9's three -- Inpaint, Remove Lighting Gradient, Offset
// ==========================================================================
//
// The three CommandCoverage.cpp used to carry as `NotYetRegistered`. Same
// shape as the seven filters above -- an adapter and nothing more -- with two
// decisions the seven did not have to make, both argued at length where they
// are used: Inpaint's selection is the hole it fills rather than a bound
// (app/FilterOps.hpp's own section on the point), and Offset's `dx`/`dy` are
// recorded as a FRACTION of the canvas rather than as texels, because its
// canonical use (`offsetByHalf()`) is defined AS a fraction and only a
// fraction still means "by half" after the document is resized.

CommandResult doInpaint(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "filter_inpaint";
  int32_t radius = 0;
  const std::string why = readWhole(params, kId, "radius", kRequired, &radius);
  if (!why.empty()) return commandRefused(why);
  // `inpaintParamsValid()` (app/FilterOps.hpp) is the engine's own bound, but
  // it takes an already-built `InpaintParams` this adapter never holds --
  // `applyInpaint()` builds `hole` from `doc.selection` itself, and passing a
  // radius-only stand-in here would be a second, narrower copy of the same
  // range check. `kInpaintMaxRadius` is public precisely so this file can
  // quote it without one.
  if (radius < 1 || radius > kInpaintMaxRadius)
    return commandRefused(refuseValue(
        kId, "radius", "from 1 to " + std::to_string(kInpaintMaxRadius) + " texels"));
  return fromFilterResult(applyInpaint(doc, radius), doc, "inpaint");
}

// Shared by the precondition and the applier -- `applyInpaint()` would refuse
// a locked layer or a missing selection itself (app/PixelOpBridge's own
// belt-and-suspenders argument, restated for `inpaintRefusal()` on section E
// of app/selftest/Command.cpp), but a replayer asks `unavailableReason()`
// BEFORE calling `apply`, so the same question has to be answerable standing
// alone.
std::string inpaintUnavailable(const OpenDocument& doc, const JsonValue&) {
  const PixelOpRefusal why = inpaintRefusal(doc);
  if (why == PixelOpRefusal::None) return {};
  return pixelOpRefusalMessage(why, activeLayerOf(doc), "inpaint");
}

CommandResult doRemoveLightingGradient(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "filter_remove_lighting_gradient";
  float sigma = 0.0f;
  std::string why = readNumber(params, kId, "sigma", kRequired, &sigma);
  // Sigma 0 is not this op's identity -- it is the erase (ops/Filters.hpp
  // section 10: every ratio becomes exactly 1 and the layer flattens to a
  // single colour) -- but it is still refused here for §2's reason: a request
  // the engine cannot honour is a batch file written unmodified and reported
  // as a success unless something upstream of the engine says no by name.
  if (why.empty()) why = requireAbove(kId, "sigma", sigma, 0.0f);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyRemoveLightingGradient(doc, sigma), doc,
                          "remove lighting gradient");
}

CommandResult doOffset(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "filter_offset";
  float dxFraction = 0.0f;
  float dyFraction = 0.0f;
  std::string why = readNumber(params, kId, "dx_fraction", kOptional, &dxFraction);
  if (why.empty()) why = readNumber(params, kId, "dy_fraction", kOptional, &dyFraction);
  OffsetEdge edge = OffsetEdge::Wrap;
  if (why.empty()) why = readEnumByName(params, kId, "edge", offsetEdgeFromName, &edge);
  if (!why.empty()) return commandRefused(why);

  // The one arithmetic step this file's header (§3) says an adapter may not
  // invent: turning a stored fraction back into the whole-texel addressing
  // change `OffsetParams::dx`/`dy` actually are. This is not "the engine's
  // own default", the way every other conversion in this file is -- it is the
  // resolution-independence policy CommandCoverage.cpp's `Offset` row states,
  // and it belongs here rather than in `ops/Filters.hpp` because `ops/` has no
  // notion of "a fraction of the canvas" at all, only whole texels
  // (ops/Filters.hpp section 4).
  const double width = static_cast<double>(doc.document.width);
  const double height = static_cast<double>(doc.document.height);
  const int32_t dx =
      static_cast<int32_t>(std::lround(static_cast<double>(dxFraction) * width));
  const int32_t dy =
      static_cast<int32_t>(std::lround(static_cast<double>(dyFraction) * height));
  // The identity refusal (§2): a fraction that resolves to zero texels on
  // BOTH axes on THIS document is indistinguishable from an offset nobody
  // asked for, and `OffsetEdge::Wrap` at (0, 0) is a bit-exact copy either
  // way. A fraction that resolves to zero on only one axis is a real,
  // deliberate one-dimensional offset and is not refused.
  if (dx == 0 && dy == 0)
    return commandRefused(
        "refused: " + std::string(kId) +
        "'s \"dx_fraction\" and \"dy_fraction\" both resolve to zero texels on this "
        "document; an offset of (0, 0) is the identity.");

  const OffsetRequest request{dx, dy, edge};
  return fromFilterResult(applyOffset(doc, request), doc, "offset");
}

std::string offsetUnavailable(const OpenDocument& doc, const JsonValue&) {
  const PixelOpRefusal why = offsetRefusalFor(doc);
  if (why == PixelOpRefusal::None) return {};
  return pixelOpRefusalMessage(why, activeLayerOf(doc), "offset");
}

// ==========================================================================
// Delete Selection -- core/SelectionMask.hpp's `clearThroughSelection()`
// ==========================================================================
//
// Not through `pixelOpUnavailable()`/`applyPixelFilter()`: those two require
// `rgbTiles` specifically (app/StrokeSession.hpp's `PixelOpRefusal::
// NoRgbStore`), because every filter above needs a working-space RGBA buffer
// to run a kernel over. A clear needs nothing of the kind -- `core/
// SelectionMask.hpp` publishes a `clearThroughSelection()` overload for
// `PigmentTileStore` too, weighted by mass rather than by premultiplied alpha
// -- and `ui/MacPaintUI.cpp`'s own Delete gesture already clears whichever
// store a layer actually holds. A command that refused a Pigment layer here
// would be narrower than the menu item it registers, which is exactly the
// parity docs/automation.md asks for.
std::string deleteSelectionUnavailable(const OpenDocument& doc, const JsonValue&) {
  const Layer* target = activeLayerOf(doc);
  if (target == nullptr)
    return "refused: delete_selection has nothing to clear. Open a document, or add a layer "
          "in LAYERS.";
  if (target->locked)
    return "locked layer: \"" + target->name + "\" cannot take delete_selection. Clear its "
          "Lock in LAYERS.";
  if (!target->rgbTiles.has_value() && !target->pigmentTiles.has_value())
    return "\"" + target->name + "\" is " + layerKindName(target->kind) +
          " and holds no pixels for delete_selection.";
  return {};
}

CommandResult doDeleteSelection(OpenDocument& doc, const JsonValue&) {
  Layer* target = activeLayerOf(doc);
  // `unavailableReason()` already refused a null/locked/storeless target
  // before this runs (app/Command.cpp's `applyCommand()`) -- this null check
  // is the same belt-and-suspenders every applier in this file keeps rather
  // than trusting a caller that might reach `apply` directly.
  if (target == nullptr) return commandRefused(deleteSelectionUnavailable(doc, JsonValue::object()));

  size_t changed = 0;
  // A null `doc.selection` clears the whole layer -- `clearThroughSelection()`
  // documents that as its own default, matching every filter above: absent
  // selection means no restriction. That is what makes this row
  // `selectionBounded`, exactly as `filter_gaussian_blur` is.
  const Selection* sel = doc.selection ? &*doc.selection : nullptr;
  if (target->rgbTiles.has_value()) changed += clearThroughSelection(*target->rgbTiles, sel);
  if (target->pigmentTiles.has_value())
    changed += clearThroughSelection(*target->pigmentTiles, sel);

  CommandResult r;
  r.ok = true;
  r.changesPixels = true;
  r.texelsChanged = changed;
  r.status = "delete selection: " + std::to_string(changed) + " texels changed";
  if (changed > 0) doc.recordEdit("clear selection", EditKind::Content);
  return r;
}

// ==========================================================================
// Numeric Transform -- `ops/DocumentTransform`'s `transformLayer()`, driven by
// the same `composeNumericTransform()` the interactive dialog uses
// ==========================================================================
//
// **Deliberately scoped to `TransformTarget::Layer` alone.** The interactive
// dialog (`ui/MacPaintUI.cpp`'s `drawNumericTransformDialog()`) also drives
// `TransformTarget::SelectionPixels` when a selection is live, through
// `app/TransformSession`'s `beginSelectionPixels()`/`commit()`. This command
// does not reach for either: another track is making Free Transform work on a
// multi-layer selection through that exact session, and this file's brief is
// "do not touch app/TransformSession or core/LayerSetOps". So a live selection
// is refused here by name rather than silently taking the whole-layer
// reading -- the interactive dialog and the Free Transform gizmo remain the
// way to numeric-transform a selection's pixels; this command is the
// recordable half of the OTHER case, which is also the more common one.
//
// **The pivot is not a parameter.** `numericTransformPivot()` (the dialog's
// own helper) is the centre of the session's frozen `sourceBounds()`, itself
// `regionFromBounds(layerContentBounds(layer))` at the moment the gizmo began.
// This command instead recomputes that same quantity from the REPLAYING
// document's active layer, every time it runs -- which is what lets a rotate
// or a non-uniform scale pivot correctly around a layer whose content bounds
// differ from the one the step was recorded against, with no fourth
// resolution-dependent number (docs/automation-plan.md §5's own suggestion,
// "pivot as a fraction of the layer/canvas bounds", would still need the
// layer's bounds at replay time to turn a fraction back into a point -- so
// recomputing the point directly is the same idea with one fewer parameter).
//
// `translate_x`/`translate_y` are the one pair of texel-valued parameters
// here (`app/Batch.cpp`'s `kPixelUnitParams` lists them); `rotate_degrees` and
// the two scale percentages are resolution-independent numbers, exactly as
// docs/automation-plan.md §5's unit rule asks: a value is pixel-unit only when
// its MEANING is relative to the resolution, and a rotation or a scale factor
// is not.
std::string numericTransformRefusal(const OpenDocument& doc) {
  const char* kId = "numeric_transform";
  const std::optional<size_t> li = activeLayerIndex(doc);
  if (!li) return std::string("refused: ") + kId + " has no layer to act on.";
  const Layer& layer = doc.document.layers[*li];
  if (layer.locked)
    return "locked layer: \"" + layer.name + "\" cannot take " + kId + ". Clear its Lock in LAYERS.";
  if (doc.selection)
    return std::string("refused: ") + kId +
          " acts on the whole active layer, and a selection is live. This command does not "
          "carry a selection-bounded transform (that stays the interactive Numeric Transform "
          "dialog's and Free Transform's job) -- deselect first, or use one of those.";
  if (layer.kind == LayerKind::Text)
    return "\"" + layer.name + "\" is a Text layer: its geometry is a Mat3 on TextContent, not "
          "tile-store pixels, and " + std::string(kId) + " does not carry that path.";
  if (!layer.rgbTiles.has_value() && !layer.pigmentTiles.has_value())
    return "\"" + layer.name + "\" is " + layerKindName(layer.kind) + " and holds no pixels for " +
          std::string(kId) + ".";
  if (layerContentBounds(layer).empty)
    return "\"" + layer.name + "\" has no content -- nothing for " + std::string(kId) +
          " to transform.";
  return {};
}

std::string numericTransformUnavailable(const OpenDocument& doc, const JsonValue&) {
  return numericTransformRefusal(doc);
}

CommandResult doNumericTransform(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "numeric_transform";
  float rotateDeg = 0.0f;
  float scaleXPercent = 100.0f;
  float scaleYPercent = 100.0f;
  float translateX = 0.0f;
  float translateY = 0.0f;
  std::string why = readNumber(params, kId, "rotate_degrees", kOptional, &rotateDeg);
  if (why.empty()) why = readNumber(params, kId, "scale_x_percent", kOptional, &scaleXPercent);
  if (why.empty()) why = readNumber(params, kId, "scale_y_percent", kOptional, &scaleYPercent);
  if (why.empty()) why = readNumber(params, kId, "translate_x", kOptional, &translateX);
  if (why.empty()) why = readNumber(params, kId, "translate_y", kOptional, &translateY);
  if (!why.empty()) return commandRefused(why);
  if (scaleXPercent <= 0.0f)
    return commandRefused(refuseValue(kId, "scale_x_percent", "a positive percentage"));
  if (scaleYPercent <= 0.0f)
    return commandRefused(refuseValue(kId, "scale_y_percent", "a positive percentage"));

  const std::string refusal = numericTransformRefusal(doc);
  if (!refusal.empty()) return commandRefused(refusal);

  const std::optional<size_t> li = activeLayerIndex(doc);
  const LayerBounds bounds = layerContentBounds(doc.document.layers[*li]);
  const DocumentRegion region = regionFromBounds(bounds);
  const Point2 pivot{static_cast<float>(region.x) + static_cast<float>(region.width) * 0.5f,
                     static_cast<float>(region.y) + static_cast<float>(region.height) * 0.5f};

  const Mat3 m = composeNumericTransform(rotateDeg, scaleXPercent / 100.0f,
                                         scaleYPercent / 100.0f, translateX, translateY, pivot);
  // TransformSession::commit()'s own rule, matched exactly (app/
  // TransformSession.cpp): an identity transform succeeds and records
  // nothing, rather than being refused as a would-be no-op the way a zero
  // sigma is above. Numeric Transform's own dialog can produce this
  // legitimately -- every field at its default is "apply nothing", which the
  // dialog itself treats as a harmless commit, not a mistake to catch.
  if (m.m == mat3Identity().m) {
    CommandResult r;
    r.ok = true;
    r.changesPixels = true;
    r.status = std::string(kId) + ": identity request, nothing changed";
    return r;
  }

  const LayerTransformResult t =
      transformLayer(doc.document, *li, m, DocumentTransformParams{});
  if (!t.ok) return commandRefused(t.error);
  doc.recordEdit(t.editLabel, EditKind::Structural);
  CommandResult r;
  r.ok = true;
  r.changesPixels = true;
  r.texelsChanged = 1;
  r.status = t.editLabel;
  return r;
}

// ==========================================================================
// Adjustments -- app/AdjustmentOps.hpp's fifteen, plus its four solvers
// ==========================================================================

// **Why `channels` is an array of three objects and not fifteen flat keys.**
//
// `applyLevelsAdjustment()` takes `std::array<LevelsParams, 3>` -- R, G and B
// each with their own independent five-field struct. The flattened shape
// (`"r_black_in"`, `"g_black_in"`, ...) was the alternative and is rejected on
// three counts, each a property of this format rather than taste:
//
//  1. **The grouping would exist only in a naming convention.** A reader would
//     have to know that the `r_` prefix means "channel 0" to reassemble the
//     struct; the nesting says it structurally, and a JSON reader that knows
//     nothing about Levels still round-trips it correctly.
//  2. **`ops/PointOps.hpp`'s own composite convention stops being expressible.**
//     That header states a composite Levels adjustment IS the caller passing
//     the same struct three times. Nested, an action writes one object and
//     repeats it, and a diff of two such files shows one object changing. Flat,
//     the same edit touches five keys in three places and the diff shows
//     fifteen lines -- which is P5's "readable and diffable" lost for nothing.
//  3. **Fifteen optional keys cannot be length-checked.** `"channels"` with two
//     entries is a refusal that names the problem; a file missing every `b_`
//     key is indistinguishable from one that meant the blue channel's defaults.
//
// Curves takes the same shape one level deeper -- three ARRAYS of control
// points, because a `Curve` is a variable-length `std::vector<CurvePoint>` and
// a flattening would have to encode its length somewhere too.
std::string readLevelsChannels(const JsonValue& params, const char* id,
                               std::array<LevelsParams, 3>* out) {
  const JsonValue* v = params.find("channels");
  if (v == nullptr || v->isNull()) return refuseMissing(id, "channels");
  if (!v->isArray() || v->size() != 3)
    return refuseValue(id, "channels",
                       "an array of exactly 3 objects, one each for red, green and blue");
  for (size_t c = 0; c < 3; ++c) {
    const JsonValue& channel = v->at(c);
    if (!channel.isObject())
      return refuseValue(id, "channels", "an array of exactly 3 objects (entry " +
                                             std::to_string(c) + " is not an object)");
    LevelsParams& lp = (*out)[c];
    // Each field defaults from `LevelsParams` itself -- see the readers'
    // section above on why the default lives there and not here.
    std::string why = readNumber(channel, id, "black_in", kOptional, &lp.blackIn);
    if (why.empty()) why = readNumber(channel, id, "white_in", kOptional, &lp.whiteIn);
    if (why.empty()) why = readNumber(channel, id, "gamma", kOptional, &lp.gamma);
    if (why.empty()) why = readNumber(channel, id, "black_out", kOptional, &lp.blackOut);
    if (why.empty()) why = readNumber(channel, id, "white_out", kOptional, &lp.whiteOut);
    if (!why.empty()) return why;
  }
  return {};
}

CommandResult doLevels(OpenDocument& doc, const JsonValue& params) {
  std::array<LevelsParams, 3> channels{};
  const std::string why = readLevelsChannels(params, "adjust_levels", &channels);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyLevelsAdjustment(doc, channels), doc, "levels");
}

CommandResult doCurves(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "adjust_curves";
  const JsonValue* v = params.find("channels");
  if (v == nullptr || v->isNull()) return commandRefused(refuseMissing(kId, "channels"));
  if (!v->isArray() || v->size() != 3)
    return commandRefused(refuseValue(
        kId, "channels", "an array of exactly 3 control-point lists, one each for R, G and B"));
  std::array<Curve, 3> channels{};
  for (size_t c = 0; c < 3; ++c) {
    const JsonValue& list = v->at(c);
    if (!list.isArray())
      return commandRefused(refuseValue(
          kId, "channels", "an array of 3 arrays (entry " + std::to_string(c) + " is not one)"));
    for (size_t i = 0; i < list.size(); ++i) {
      const JsonValue& point = list.at(i);
      // Both coordinates are required per point. A point with only an `x` is
      // not a curve with a defaulted `y` -- it is a file that lost half a
      // number, and reading it as `y = 0` would bend the curve to black at
      // that abscissa and report success.
      if (!point.isObject() || !point.hasNumber("x") || !point.hasNumber("y"))
        return commandRefused(refuseValue(kId, "channels",
                                          "control points that each carry a numeric \"x\" and "
                                          "\"y\" (channel " +
                                              std::to_string(c) + ", point " + std::to_string(i) +
                                              " does not)"));
      CurvePoint cp;
      std::string why = readNumber(point, kId, "x", kRequired, &cp.x);
      if (why.empty()) why = readNumber(point, kId, "y", kRequired, &cp.y);
      if (!why.empty()) return commandRefused(why);
      channels[c].push_back(cp);
    }
  }
  // A channel with fewer than two points is the identity for that channel, per
  // app/AdjustmentOps.hpp -- the engine's own documented behaviour, so it is
  // NOT refused here. An empty list is how a file says "leave blue alone",
  // which is a thing an author means on purpose.
  return fromFilterResult(applyCurvesAdjustment(doc, channels), doc, "curves");
}

CommandResult doExposure(OpenDocument& doc, const JsonValue& params) {
  ExposureParams p;
  const std::string why = readNumber(params, "adjust_exposure", "stops", kRequired, &p.stops);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyExposureAdjustment(doc, p), doc, "exposure");
}

CommandResult doChannelMixer(OpenDocument& doc, const JsonValue& params) {
  const char* kId = "adjust_channel_mixer";
  const JsonValue* v = params.find("matrix");
  if (v == nullptr || v->isNull()) return commandRefused(refuseMissing(kId, "matrix"));
  if (!v->isArray() || v->size() != 3)
    return commandRefused(refuseValue(
        kId, "matrix", "an array of 3 rows (one per output channel), each of 4 numbers"));
  ChannelMixerParams p;
  for (size_t r = 0; r < 3; ++r) {
    const JsonValue& row = v->at(r);
    if (!row.isArray() || row.size() != 4)
      return commandRefused(refuseValue(kId, "matrix",
                                        "rows of exactly 4 numbers -- R, G, B and the offset "
                                        "(row " +
                                            std::to_string(r) + " is not)"));
    for (size_t k = 0; k < 4; ++k) {
      const JsonValue& e = row.at(k);
      if (!e.isNumber() || !std::isfinite(e.asNumber()))
        return commandRefused(refuseValue(kId, "matrix", "finite numbers throughout (row " +
                                                             std::to_string(r) + ", column " +
                                                             std::to_string(k) + " is not)"));
      p.matrix[r][k] = static_cast<float>(e.asNumber());
    }
  }
  return fromFilterResult(applyChannelMixerAdjustment(doc, p), doc, "channel mixer");
}

CommandResult doDesaturate(OpenDocument& doc, const JsonValue&) {
  // No parameters, matching Photoshop and matching `applyDesaturate()`'s own
  // signature: there is nothing a user could set, so there is nothing to read
  // and nothing that could be missing.
  return fromFilterResult(applyDesaturate(doc), doc, "desaturate");
}

CommandResult doBrightnessContrast(OpenDocument& doc, const JsonValue& params) {
  GainOffsetGammaParams p;
  const char* kId = "adjust_brightness_contrast";
  std::string why = requireAnyOf(params, kId, {"gain", "offset", "gamma"});
  if (why.empty()) why = readNumber(params, kId, "gain", kOptional, &p.gain);
  if (why.empty()) why = readNumber(params, kId, "offset", kOptional, &p.offset);
  if (why.empty()) why = readNumber(params, kId, "gamma", kOptional, &p.gamma);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyBrightnessContrast(doc, p), doc, "brightness/contrast");
}

CommandResult doHueSaturation(OpenDocument& doc, const JsonValue& params) {
  HueSaturationParams p;
  const char* kId = "adjust_hue_saturation";
  // `colorize` counts as one of the keys that make this a real request: a step
  // that only sets `colorize` to true is a full colorize at the struct's own
  // default target, which is a picture, not a no-op.
  std::string why =
      requireAnyOf(params, kId, {"hue_degrees", "saturation", "lightness", "colorize"});
  if (why.empty()) why = readNumber(params, kId, "hue_degrees", kOptional, &p.hueDegrees);
  if (why.empty()) why = readNumber(params, kId, "saturation", kOptional, &p.saturation);
  if (why.empty()) why = readNumber(params, kId, "lightness", kOptional, &p.lightness);
  if (why.empty()) why = readBool(params, kId, "colorize", &p.colorize);
  if (why.empty())
    why = readNumber(params, kId, "colorize_hue_degrees", kOptional, &p.colorizeHueDegrees);
  if (why.empty())
    why = readNumber(params, kId, "colorize_saturation", kOptional, &p.colorizeSaturation);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyHueSaturationAdjustment(doc, p), doc, "hue/saturation");
}

CommandResult doVibrance(OpenDocument& doc, const JsonValue& params) {
  VibranceParams p;
  const char* kId = "adjust_vibrance";
  std::string why = readNumber(params, kId, "amount", kRequired, &p.amount);
  if (why.empty()) why = readFixedArray(params, kId, "luma_weights", kOptional, &p.lumaWeights);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyVibranceAdjustment(doc, p), doc, "vibrance");
}

CommandResult doColorBalance(OpenDocument& doc, const JsonValue& params) {
  ColorBalanceParams p;
  const char* kId = "adjust_color_balance";
  std::string why = requireAnyOf(params, kId, {"shadows", "midtones", "highlights"});
  if (why.empty()) why = readFixedArray(params, kId, "shadows", kOptional, &p.shadowsLift);
  if (why.empty()) why = readFixedArray(params, kId, "midtones", kOptional, &p.midtonesGamma);
  if (why.empty()) why = readFixedArray(params, kId, "highlights", kOptional, &p.highlightsGain);
  if (why.empty()) why = readBool(params, kId, "preserve_luminosity", &p.preserveLuminosity);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyColorBalanceAdjustment(doc, p), doc, "color balance");
}

CommandResult doBlackAndWhite(OpenDocument& doc, const JsonValue& params) {
  BlackAndWhiteParams p;
  const char* kId = "adjust_black_and_white";
  std::string why =
      requireAnyOf(params, kId, {"reds", "yellows", "greens", "cyans", "blues", "magentas"});
  if (why.empty()) why = readNumber(params, kId, "reds", kOptional, &p.reds);
  if (why.empty()) why = readNumber(params, kId, "yellows", kOptional, &p.yellows);
  if (why.empty()) why = readNumber(params, kId, "greens", kOptional, &p.greens);
  if (why.empty()) why = readNumber(params, kId, "cyans", kOptional, &p.cyans);
  if (why.empty()) why = readNumber(params, kId, "blues", kOptional, &p.blues);
  if (why.empty()) why = readNumber(params, kId, "magentas", kOptional, &p.magentas);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyBlackAndWhiteAdjustment(doc, p), doc, "black and white");
}

CommandResult doPhotoFilter(OpenDocument& doc, const JsonValue& params) {
  PhotoFilterParams p;
  const char* kId = "adjust_photo_filter";
  // `density` and not `color` is the required one, because `density == 0` is
  // the identity **regardless of colour** (ops/ColorOps.hpp says so on the
  // field): a step naming only a colour would be a warm filter at zero
  // strength, which changes nothing and reports success.
  std::string why = readNumber(params, kId, "density", kRequired, &p.density);
  if (why.empty()) why = requireAbove(kId, "density", p.density, 0.0f);
  if (why.empty()) why = readFixedArray(params, kId, "color", kOptional, &p.color);
  if (why.empty()) why = readBool(params, kId, "preserve_luminosity", &p.preserveLuminosity);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyPhotoFilterAdjustment(doc, p), doc, "photo filter");
}

CommandResult doPosterize(OpenDocument& doc, const JsonValue& params) {
  PosterizeParams p;
  const char* kId = "adjust_posterize";
  const std::string why = readWhole(params, kId, "levels", kRequired, &p.levels);
  if (!why.empty()) return commandRefused(why);
  // `applyPosterize()` returns its input unchanged for `levels <= 0`. That is
  // the engine's documented degenerate case, not an error -- but as a recorded
  // step it is a no-op reported as a success, so it is refused here. `levels
  // == 1` is NOT refused: ops/ToneOps.hpp defines it as the single-bin card,
  // which is a real (if extreme) picture.
  if (p.levels < 1)
    return commandRefused(refuseValue(
        kId, "levels", "at least 1; ops/ToneOps returns its input unchanged below that"));
  return fromFilterResult(applyPosterizeAdjustment(doc, p), doc, "posterize");
}

CommandResult doThreshold(OpenDocument& doc, const JsonValue& params) {
  ThresholdParams p;
  const char* kId = "adjust_threshold";
  // Both keys optional, and no `requireAnyOf()`, because a default-constructed
  // `ThresholdParams` is a **full black/white split at 0.5**, not an identity
  // -- ops/ToneOps.hpp's identity-default section states that exception for
  // this struct and for Invert explicitly. An empty `adjust_threshold` step
  // therefore does something, which is the test `requireAnyOf()` applies.
  std::string why = readNumber(params, kId, "threshold", kOptional, &p.threshold);
  if (why.empty()) why = readNumber(params, kId, "amount", kOptional, &p.amount);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyThresholdAdjustment(doc, p), doc, "threshold");
}

CommandResult doGradientMap(OpenDocument& doc, const JsonValue& params) {
  GradientMapParams p;
  const char* kId = "adjust_gradient_map";
  const JsonValue* v = params.find("stops");
  if (v == nullptr || v->isNull()) return commandRefused(refuseMissing(kId, "stops"));
  if (!v->isArray() || v->size() == 0)
    return commandRefused(refuseValue(kId, "stops", "a non-empty array of colour stops"));
  for (size_t i = 0; i < v->size(); ++i) {
    const JsonValue& stop = v->at(i);
    if (!stop.isObject())
      return commandRefused(refuseValue(
          kId, "stops", "objects (entry " + std::to_string(i) + " is not one)"));
    ColorStop cs;
    std::string why = readNumber(stop, kId, "position", kRequired, &cs.position);
    if (why.empty()) why = readFixedArray(stop, kId, "color", kRequired, &cs.color);
    if (why.empty()) why = readNumber(stop, kId, "midpoint", kOptional, &cs.midpoint);
    if (!why.empty()) return commandRefused(why);
    p.stops.colorStops.push_back(cs);
  }
  // **Only colour stops.** `applyGradientMapAdjustment()` never consults the
  // opacity ramp -- ops/MonoOps.hpp states it, because the whole point-op
  // family "never sees alpha, never touches it". A key for opacity stops here
  // would be a control the file could set and the op would silently ignore.
  const std::string why = readFixedArray(params, kId, "luma_weights", kOptional, &p.lumaWeights);
  if (!why.empty()) return commandRefused(why);
  // The stop list must be sorted ascending -- ops/Gradient.hpp makes that the
  // CALLER's contract, and `sortGradientStops()` is for an editor after a drag.
  // Sorting silently here would let a file whose stops are in the wrong order
  // render differently from what it says, so it is refused, naming the entry.
  for (size_t i = 1; i < p.stops.colorStops.size(); ++i)
    if (p.stops.colorStops[i].position < p.stops.colorStops[i - 1].position)
      return commandRefused(refuseValue(
          kId, "stops", "sorted ascending by \"position\" (entry " + std::to_string(i) +
                            " comes before the one above it) -- ops/Gradient makes that the "
                            "caller's contract, and sorting it here would make the file render "
                            "differently from what it reads as"));
  return fromFilterResult(applyGradientMapAdjustment(doc, p), doc, "gradient map");
}

CommandResult doInvert(OpenDocument& doc, const JsonValue& params) {
  InvertParams p;
  const char* kId = "adjust_invert";
  // No required parameter and no `requireAnyOf()`: ops/ToneOps.hpp is explicit
  // that a default-constructed Invert INVERTS (amount 1), deliberately breaking
  // the family's identity-default rule, so an empty step is a full invert and
  // does something.
  std::string why = readEnumByName(params, kId, "domain", invertDomainFromName, &p.domain);
  if (why.empty()) why = readNumber(params, kId, "amount", kOptional, &p.amount);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyInvert(doc, p), doc, "invert");
}

// The four solvers. Each is `applyX(doc, tuning)` and nothing else: a solver
// is "a parameter solver, not an op" (docs/operations.md §1.2), so the adapter
// has no parameters of its own beyond the tuning the applier already takes.
std::string readAutoTuning(const JsonValue& params, const char* id, AutoLevelsParams* out) {
  const std::string why = readNumber(params, id, "clip_fraction", kOptional, &out->clipFraction);
  if (!why.empty()) return why;
  // The UPPER bound is `ops/AutoLevels`' own documented clamp to [0, 0.49] and
  // is deliberately not repeated here -- a second copy of 0.49 is the drift
  // app/PixelOpBridge.hpp exists to prevent. A NEGATIVE fraction is refused,
  // because "minus one per cent of the pixels" is not a quantity the clamp
  // could be interpreting; it is a caller error the clamp would hide.
  if (out->clipFraction < 0.0f)
    return refuseValue(id, "clip_fraction", "zero or a positive fraction of the pixels");
  return {};
}

CommandResult doAutoTone(OpenDocument& doc, const JsonValue& params) {
  AutoLevelsParams tuning;
  const std::string why = readAutoTuning(params, "adjust_auto_tone", &tuning);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyAutoTone(doc, tuning), doc, "auto tone");
}

CommandResult doAutoContrast(OpenDocument& doc, const JsonValue& params) {
  AutoLevelsParams tuning;
  const std::string why = readAutoTuning(params, "adjust_auto_contrast", &tuning);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyAutoContrast(doc, tuning), doc, "auto contrast");
}

CommandResult doAutoColor(OpenDocument& doc, const JsonValue& params) {
  AutoLevelsParams tuning;
  const std::string why = readAutoTuning(params, "adjust_auto_color", &tuning);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyAutoColor(doc, tuning), doc, "auto color");
}

CommandResult doEqualize(OpenDocument& doc, const JsonValue& params) {
  AutoLevelsParams tuning;
  const std::string why = readAutoTuning(params, "adjust_equalize", &tuning);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyEqualize(doc, tuning), doc, "equalize");
}

// ==========================================================================
// The document's own geometry
// ==========================================================================

// Shared by `image_size` and `canvas_size`, because the two agree exactly
// about what an extent is and disagree only about what happens to the pixels
// inside it. Two copies of the whole-pixel check would be two chances for one
// of them to start accepting a fractional width.
std::string readExtent(const JsonValue& params, const char* id, uint32_t* w, uint32_t* h) {
  int32_t width = 0;
  int32_t height = 0;
  std::string why = readWhole(params, id, "width", kRequired, &width);
  if (why.empty()) why = readWhole(params, id, "height", kRequired, &height);
  if (!why.empty()) return why;
  if (width < 1) return refuseValue(id, "width", "at least 1 pixel");
  if (height < 1) return refuseValue(id, "height", "at least 1 pixel");
  *w = static_cast<uint32_t>(width);
  *h = static_cast<uint32_t>(height);
  return {};
}

CommandResult doImageSize(OpenDocument& doc, const JsonValue& params) {
  uint32_t w = 0;
  uint32_t h = 0;
  std::string why = readExtent(params, "image_size", &w, &h);
  if (!why.empty()) return commandRefused(why);
  ResampleKernel kernel = ResampleKernel::CatmullRom;
  const std::string kernelName = params.stringOr("kernel", "");
  if (!kernelName.empty()) {
    const std::optional<ResampleKernel> k = resampleKernelFromName(kernelName);
    if (!k) return commandRefused("refused: no resample kernel is named \"" + kernelName + "\".");
    kernel = *k;
  }
  return fromDocumentOutcome(applyImageSize(doc, w, h, kernel), "image size");
}

CommandResult doCanvasSize(OpenDocument& doc, const JsonValue& params) {
  uint32_t w = 0;
  uint32_t h = 0;
  const char* kId = "canvas_size";
  std::string why = readExtent(params, kId, &w, &h);
  if (!why.empty()) return commandRefused(why);
  // Centre is the default because it is what a canvas-size dialog opens on,
  // and because it is the one anchor whose result does not depend on which
  // edge the author had in mind. Named, never an ordinal -- see
  // `canvasAnchorName()` in ops/Transform.hpp, which argues why THIS enum in
  // particular would be the worst one to key by position.
  CanvasAnchor anchor = CanvasAnchor::Center;
  if (why = readEnumByName(params, kId, "anchor", canvasAnchorFromName, &anchor); !why.empty())
    return commandRefused(why);
  // `applyCanvasSize()` reports a `DocumentOpOutcome`, which cannot say whether
  // the extent moved -- see `fromDocumentOutcome()`'s own note. That is the
  // applier's shape, not something to work around here by comparing extents in
  // the adapter: a second answer to "did this change anything" is exactly what
  // app/CommandSupport.hpp exists to prevent.
  return fromDocumentOutcome(applyCanvasSize(doc, w, h, anchor), "canvas size");
}

CommandResult doCropToSelection(OpenDocument& doc, const JsonValue&) {
  // No parameters, and specifically no marquee coordinates. The region comes
  // from `OpenDocument::selection`, which docs/automation-plan.md §7 says is
  // session state that never reaches a file -- and recording the rectangle
  // instead "is the wrong answer; they are meaningless at another resolution".
  // What makes this step deterministic on replay is the selection command that
  // precedes it, which is another track's row.
  return fromDocumentTransform(applyCropToSelection(doc), doc, "crop to selection");
}

CommandResult doTrimToContent(OpenDocument& doc, const JsonValue&) {
  return fromDocumentTransform(applyTrimToContent(doc), doc, "trim to content");
}

}  // namespace

void registerImageCommands(std::vector<CommandSpec>* out) {
  // `paramNames` is the list `--selftest` checks every key an adapter reads
  // against, and the ACTIONS panel walks to build its editor. A key read above
  // and missing here is a control the panel cannot offer; a key here and never
  // read is a control that does nothing. Both are asserted, so this list is
  // load-bearing rather than documentation.

  // ---- document geometry -------------------------------------------------
  //
  // **Three of these four are the rows `selectionBounded` was added for.**
  // `image_size`, `canvas_size` and `trim_to_content` all change pixels and
  // none of them is restricted by the selection -- they act on the whole
  // document by construction, and `trim_to_content` reads the layers' content
  // rather than any marquee. The recorder used to police them anyway, because
  // it decided "is this selection-bounded?" from `CommandResult::changesPixels`
  // (app/Command.hpp on the field), so recording a resize under a live
  // marquee was refused for a reason that does not apply to it.
  //
  // `crop_to_selection` is the exception, and it is the one row in the table
  // that is bounded without going through `pixelOpUnavailable` -- the region
  // it crops to IS the selection. It is asserted as that named exception in
  // app/selftest/Command.cpp section H, so a second one has to be looked at by
  // a human rather than joining a growing list.
  out->push_back({"image_size", "Image Size", {"width", "height", "kernel"}, documentUnavailable,
                  doImageSize});
  out->push_back({"canvas_size", "Canvas Size", {"width", "height", "anchor"}, documentUnavailable,
                  doCanvasSize});
  out->push_back({"crop_to_selection", "Crop to Selection", {}, documentUnavailable,
                  doCropToSelection, /*selectionBounded=*/true});
  out->push_back({"trim_to_content", "Trim to Content", {}, documentUnavailable, doTrimToContent});

  // `delete_selection` joins `crop_to_selection` as the second row bounded by
  // the selection without going through `pixelOpUnavailable` -- it needs
  // `deleteSelectionUnavailable()` instead because it must accept a Pigment
  // layer, which the shared pixel-op bridge does not. Both are named as the
  // exception in app/selftest/Command.cpp section H.
  out->push_back({"delete_selection", "Delete", {}, deleteSelectionUnavailable, doDeleteSelection,
                  /*selectionBounded=*/true});

  // `numeric_transform` acts on the whole active layer (never bounded by the
  // selection -- a live one is refused by name instead, see `doNumericTransform()`),
  // exactly as `image_size`/`canvas_size`/`trim_to_content` above it are not
  // bounded either.
  out->push_back({"numeric_transform",
                  "Transform",
                  {"rotate_degrees", "scale_x_percent", "scale_y_percent", "translate_x",
                   "translate_y"},
                  numericTransformUnavailable,
                  doNumericTransform});

  // ---- the Filter menu's seven, plus three more ---------------------------
  out->push_back({"filter_gaussian_blur", "Gaussian Blur", {"sigma"}, pixelOpUnavailable,
                  doGaussianBlur, /*selectionBounded=*/true});
  out->push_back({"filter_sharpen", "Sharpen", {"strength"}, pixelOpUnavailable, doSharpen,
                  /*selectionBounded=*/true});
  out->push_back({"filter_unsharp_mask",
                  "Unsharp Mask",
                  {"amount", "radius", "blur_kind", "threshold"},
                  pixelOpUnavailable,
                  doUnsharpMask, /*selectionBounded=*/true});
  out->push_back({"filter_add_noise",
                  "Add Noise",
                  {"amount", "distribution", "monochrome", "seed"},
                  pixelOpUnavailable,
                  doAddNoise, /*selectionBounded=*/true});
  out->push_back({"filter_emboss",
                  "Emboss",
                  {"amount", "dx", "dy", "depth"},
                  pixelOpUnavailable,
                  doEmboss, /*selectionBounded=*/true});
  out->push_back({"filter_median", "Median", {"radius"}, pixelOpUnavailable, doMedian,
                  /*selectionBounded=*/true});
  out->push_back({"filter_motion_blur",
                  "Motion Blur",
                  {"radius", "angle_radians"},
                  pixelOpUnavailable,
                  doMotionBlur, /*selectionBounded=*/true});
  // `filter_inpaint` is the one row here NOT bounded through the shared
  // `pixelOpUnavailable()` bridge -- `inpaintUnavailable()` calls
  // `inpaintRefusal()` instead, because an absent selection is a hard refusal
  // for this op rather than "the whole canvas" (app/FilterOps.hpp's own
  // argument). It is still `selectionBounded`: the recorder's channel-match
  // rule (app/Recorder.hpp §4) is exactly the protection this op needs against
  // a live, unsaved marquee, even though the flag's usual reading ("absent
  // means whole canvas") does not literally hold for it -- see
  // app/selftest/Command.cpp section H, which names this row as the second
  // bounded exception beside `crop_to_selection`.
  out->push_back({"filter_inpaint", "Inpaint", {"radius"}, inpaintUnavailable, doInpaint,
                  /*selectionBounded=*/true});
  out->push_back({"filter_remove_lighting_gradient",
                  "Remove Lighting Gradient",
                  {"sigma"},
                  pixelOpUnavailable,
                  doRemoveLightingGradient, /*selectionBounded=*/true});
  // NOT selectionBounded: `offsetUnavailable()` refuses outright under ANY
  // live selection (`offsetRefusalFor()`'s `SelectionActive`), so an absent
  // one is never "the whole canvas" reading that needed protecting -- it is
  // the ONLY reading this op has.
  out->push_back({"filter_offset",
                  "Offset",
                  {"dx_fraction", "dy_fraction", "edge"},
                  offsetUnavailable,
                  doOffset});

  // ---- Image > Adjustments -----------------------------------------------
  out->push_back({"adjust_levels", "Levels", {"channels"}, pixelOpUnavailable, doLevels,
                  /*selectionBounded=*/true});
  out->push_back({"adjust_curves", "Curves", {"channels"}, pixelOpUnavailable, doCurves,
                  /*selectionBounded=*/true});
  out->push_back({"adjust_exposure", "Exposure", {"stops"}, pixelOpUnavailable, doExposure,
                  /*selectionBounded=*/true});
  out->push_back(
      {"adjust_channel_mixer", "Channel Mixer", {"matrix"}, pixelOpUnavailable, doChannelMixer,
       /*selectionBounded=*/true});
  out->push_back({"adjust_desaturate", "Desaturate", {}, pixelOpUnavailable, doDesaturate,
                  /*selectionBounded=*/true});
  out->push_back({"adjust_brightness_contrast",
                  "Brightness/Contrast",
                  {"gain", "offset", "gamma"},
                  pixelOpUnavailable,
                  doBrightnessContrast, /*selectionBounded=*/true});
  out->push_back({"adjust_hue_saturation",
                  "Hue/Saturation",
                  {"hue_degrees", "saturation", "lightness", "colorize", "colorize_hue_degrees",
                   "colorize_saturation"},
                  pixelOpUnavailable,
                  doHueSaturation, /*selectionBounded=*/true});
  out->push_back({"adjust_vibrance",
                  "Vibrance",
                  {"amount", "luma_weights"},
                  pixelOpUnavailable,
                  doVibrance, /*selectionBounded=*/true});
  out->push_back({"adjust_color_balance",
                  "Color Balance",
                  {"shadows", "midtones", "highlights", "preserve_luminosity"},
                  pixelOpUnavailable,
                  doColorBalance, /*selectionBounded=*/true});
  out->push_back({"adjust_black_and_white",
                  "Black & White",
                  {"reds", "yellows", "greens", "cyans", "blues", "magentas"},
                  pixelOpUnavailable,
                  doBlackAndWhite, /*selectionBounded=*/true});
  out->push_back({"adjust_photo_filter",
                  "Photo Filter",
                  {"density", "color", "preserve_luminosity"},
                  pixelOpUnavailable,
                  doPhotoFilter, /*selectionBounded=*/true});
  out->push_back({"adjust_posterize", "Posterize", {"levels"}, pixelOpUnavailable, doPosterize,
                  /*selectionBounded=*/true});
  out->push_back({"adjust_threshold", "Threshold", {"threshold", "amount"}, pixelOpUnavailable,
                  doThreshold, /*selectionBounded=*/true});
  out->push_back({"adjust_gradient_map",
                  "Gradient Map",
                  {"stops", "luma_weights"},
                  pixelOpUnavailable,
                  doGradientMap, /*selectionBounded=*/true});
  out->push_back(
      {"adjust_invert", "Invert", {"amount", "domain"}, pixelOpUnavailable, doInvert,
       /*selectionBounded=*/true});
  out->push_back(
      {"adjust_auto_tone", "Auto Tone", {"clip_fraction"}, pixelOpUnavailable, doAutoTone,
       /*selectionBounded=*/true});
  out->push_back({"adjust_auto_contrast",
                  "Auto Contrast",
                  {"clip_fraction"},
                  pixelOpUnavailable,
                  doAutoContrast, /*selectionBounded=*/true});
  out->push_back(
      {"adjust_auto_color", "Auto Color", {"clip_fraction"}, pixelOpUnavailable, doAutoColor,
       /*selectionBounded=*/true});
  out->push_back(
      {"adjust_equalize", "Equalize", {"clip_fraction"}, pixelOpUnavailable, doEqualize,
       /*selectionBounded=*/true});
}


// ==========================================================================
// The encoders (app/CommandsImage.hpp)
// ==========================================================================
//
// Directly under the readers they feed, which is the whole of this header's
// argument for where they live. Every key spelled below appears verbatim in a
// `read*()` call above; nothing here validates, and nothing here defaults --
// see app/CommandsImage.hpp §2 and §3.
namespace {

// The two shapes repeated often enough to be worth naming: a fixed float
// triple (`luma_weights`, a colour, a colour-balance band) and a bare object.
JsonValue jsonTriple(const std::array<float, 3>& v) {
  JsonValue a = JsonValue::array();
  for (float f : v) a.push(JsonValue::number(f));
  return a;
}

Command command(const char* id, JsonValue params) {
  Command c;
  c.id = id;
  c.params = std::move(params);
  return c;
}

}  // namespace

Command gaussianBlurCommand(float sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return command("filter_gaussian_blur", std::move(p));
}

Command sharpenCommand(float strength) {
  JsonValue p = JsonValue::object();
  p.set("strength", JsonValue::number(strength));
  return command("filter_sharpen", std::move(p));
}

Command unsharpMaskCommand(const UnsharpParams& u) {
  // One `radius` key whose meaning follows `blur_kind`, exactly as
  // `doUnsharpMask()` reads it -- see its own comment on why two keys would be
  // worse. So the encoder must consult the kind too, and writing the field
  // that does not belong to it would produce a step the reader ignores.
  JsonValue p = JsonValue::object();
  p.set("blur_kind", JsonValue::string(blurKindName(u.blur.kind)));
  p.set("radius", JsonValue::number(u.blur.kind == BlurKind::Box
                                        ? static_cast<double>(u.blur.boxRadius)
                                        : static_cast<double>(u.blur.sigma)));
  p.set("amount", JsonValue::number(u.amount));
  p.set("threshold", JsonValue::number(u.threshold));
  return command("filter_unsharp_mask", std::move(p));
}

Command addNoiseCommand(const NoiseParams& n) {
  JsonValue p = JsonValue::object();
  p.set("amount", JsonValue::number(n.amount));
  p.set("distribution", JsonValue::string(noiseDistributionName(n.distribution)));
  p.set("monochrome", JsonValue::boolean(n.monochrome));
  // `doAddNoise()` refuses a seed above 2^53 because a JSON number is a double
  // and a larger one would round. The dialog's own seed is a small counter, so
  // this cannot fire from the UI -- but the cast is written as the reader's
  // own bound rather than as an assumption about the dialog, because the next
  // caller of this function may not be a dialog.
  p.set("seed", JsonValue::number(static_cast<double>(n.seed)));
  return command("filter_add_noise", std::move(p));
}

Command embossCommand(const EmbossParams& e) {
  JsonValue p = JsonValue::object();
  p.set("dx", JsonValue::number(e.dx));
  p.set("dy", JsonValue::number(e.dy));
  p.set("depth", JsonValue::number(e.depth));
  p.set("amount", JsonValue::number(e.amount));
  return command("filter_emboss", std::move(p));
}

Command medianCommand(const MedianParams& m) {
  JsonValue p = JsonValue::object();
  p.set("radius", JsonValue::number(m.radius));
  return command("filter_median", std::move(p));
}

Command motionBlurCommand(const MotionBlurParams& m) {
  JsonValue p = JsonValue::object();
  p.set("radius", JsonValue::number(m.radius));
  p.set("angle_radians", JsonValue::number(m.angleRadians));
  return command("filter_motion_blur", std::move(p));
}

Command levelsCommand(const std::array<LevelsParams, 3>& channels) {
  JsonValue list = JsonValue::array();
  for (const LevelsParams& c : channels) {
    JsonValue one = JsonValue::object();
    one.set("black_in", JsonValue::number(c.blackIn));
    one.set("white_in", JsonValue::number(c.whiteIn));
    one.set("gamma", JsonValue::number(c.gamma));
    one.set("black_out", JsonValue::number(c.blackOut));
    one.set("white_out", JsonValue::number(c.whiteOut));
    list.push(std::move(one));
  }
  JsonValue p = JsonValue::object();
  p.set("channels", std::move(list));
  return command("adjust_levels", std::move(p));
}

Command curvesCommand(const std::array<Curve, 3>& channels) {
  JsonValue list = JsonValue::array();
  for (const Curve& curve : channels) {
    JsonValue points = JsonValue::array();
    for (const CurvePoint& cp : curve) {
      JsonValue one = JsonValue::object();
      one.set("x", JsonValue::number(cp.x));
      one.set("y", JsonValue::number(cp.y));
      points.push(std::move(one));
    }
    // An EMPTY list is written, not skipped: `doCurves()` reads a channel with
    // fewer than two points as the identity for that channel, which is what a
    // dialog whose blue curve was never touched means. Skipping it would make
    // the array shorter than three and the whole step a refusal.
    list.push(std::move(points));
  }
  JsonValue p = JsonValue::object();
  p.set("channels", std::move(list));
  return command("adjust_curves", std::move(p));
}

Command exposureCommand(const ExposureParams& e) {
  JsonValue p = JsonValue::object();
  p.set("stops", JsonValue::number(e.stops));
  return command("adjust_exposure", std::move(p));
}

Command channelMixerCommand(const ChannelMixerParams& m) {
  JsonValue rows = JsonValue::array();
  for (const std::array<float, 4>& row : m.matrix) {
    JsonValue one = JsonValue::array();
    for (float v : row) one.push(JsonValue::number(v));
    rows.push(std::move(one));
  }
  JsonValue p = JsonValue::object();
  p.set("matrix", std::move(rows));
  return command("adjust_channel_mixer", std::move(p));
}

Command desaturateCommand() { return command("adjust_desaturate", JsonValue::object()); }

Command brightnessContrastCommand(const GainOffsetGammaParams& g) {
  JsonValue p = JsonValue::object();
  p.set("gain", JsonValue::number(g.gain));
  p.set("offset", JsonValue::number(g.offset));
  p.set("gamma", JsonValue::number(g.gamma));
  return command("adjust_brightness_contrast", std::move(p));
}

Command hueSaturationCommand(const HueSaturationParams& h) {
  JsonValue p = JsonValue::object();
  p.set("hue_degrees", JsonValue::number(h.hueDegrees));
  p.set("saturation", JsonValue::number(h.saturation));
  p.set("lightness", JsonValue::number(h.lightness));
  p.set("colorize", JsonValue::boolean(h.colorize));
  p.set("colorize_hue_degrees", JsonValue::number(h.colorizeHueDegrees));
  p.set("colorize_saturation", JsonValue::number(h.colorizeSaturation));
  return command("adjust_hue_saturation", std::move(p));
}

Command vibranceCommand(const VibranceParams& v) {
  JsonValue p = JsonValue::object();
  p.set("amount", JsonValue::number(v.amount));
  p.set("luma_weights", jsonTriple(v.lumaWeights));
  return command("adjust_vibrance", std::move(p));
}

Command colorBalanceCommand(const ColorBalanceParams& c) {
  JsonValue p = JsonValue::object();
  p.set("shadows", jsonTriple(c.shadowsLift));
  p.set("midtones", jsonTriple(c.midtonesGamma));
  p.set("highlights", jsonTriple(c.highlightsGain));
  p.set("preserve_luminosity", JsonValue::boolean(c.preserveLuminosity));
  return command("adjust_color_balance", std::move(p));
}

Command blackAndWhiteCommand(const BlackAndWhiteParams& b) {
  JsonValue p = JsonValue::object();
  p.set("reds", JsonValue::number(b.reds));
  p.set("yellows", JsonValue::number(b.yellows));
  p.set("greens", JsonValue::number(b.greens));
  p.set("cyans", JsonValue::number(b.cyans));
  p.set("blues", JsonValue::number(b.blues));
  p.set("magentas", JsonValue::number(b.magentas));
  return command("adjust_black_and_white", std::move(p));
}

Command photoFilterCommand(const PhotoFilterParams& f) {
  JsonValue p = JsonValue::object();
  p.set("density", JsonValue::number(f.density));
  p.set("color", jsonTriple(f.color));
  p.set("preserve_luminosity", JsonValue::boolean(f.preserveLuminosity));
  return command("adjust_photo_filter", std::move(p));
}

Command posterizeCommand(const PosterizeParams& p2) {
  JsonValue p = JsonValue::object();
  p.set("levels", JsonValue::number(p2.levels));
  return command("adjust_posterize", std::move(p));
}

Command thresholdCommand(const ThresholdParams& t) {
  JsonValue p = JsonValue::object();
  p.set("threshold", JsonValue::number(t.threshold));
  p.set("amount", JsonValue::number(t.amount));
  return command("adjust_threshold", std::move(p));
}

Command gradientMapCommand(const GradientMapParams& g) {
  JsonValue stops = JsonValue::array();
  for (const ColorStop& s : g.stops.colorStops) {
    JsonValue one = JsonValue::object();
    one.set("position", JsonValue::number(s.position));
    one.set("color", jsonTriple(s.color));
    one.set("midpoint", JsonValue::number(s.midpoint));
    stops.push(std::move(one));
  }
  // The opacity ramp is deliberately not written: `applyGradientMapAdjustment()`
  // never consults it, and `doGradientMap()` therefore has no key for it. See
  // that adapter's own note.
  JsonValue p = JsonValue::object();
  p.set("stops", std::move(stops));
  p.set("luma_weights", jsonTriple(g.lumaWeights));
  return command("adjust_gradient_map", std::move(p));
}

Command invertCommand() { return command("adjust_invert", JsonValue::object()); }
Command autoToneCommand() { return command("adjust_auto_tone", JsonValue::object()); }
Command autoContrastCommand() { return command("adjust_auto_contrast", JsonValue::object()); }
Command autoColorCommand() { return command("adjust_auto_color", JsonValue::object()); }
Command equalizeCommand() { return command("adjust_equalize", JsonValue::object()); }

Command imageSizeCommand(uint32_t width, uint32_t height, ResampleKernel kernel) {
  JsonValue p = JsonValue::object();
  p.set("width", JsonValue::number(width));
  p.set("height", JsonValue::number(height));
  p.set("kernel", JsonValue::string(resampleKernelName(kernel)));
  return command("image_size", std::move(p));
}

Command canvasSizeCommand(uint32_t width, uint32_t height, CanvasAnchor anchor) {
  JsonValue p = JsonValue::object();
  p.set("width", JsonValue::number(width));
  p.set("height", JsonValue::number(height));
  p.set("anchor", JsonValue::string(canvasAnchorName(anchor)));
  return command("canvas_size", std::move(p));
}

Command cropToSelectionCommand() { return command("crop_to_selection", JsonValue::object()); }
Command trimToContentCommand() { return command("trim_to_content", JsonValue::object()); }

Command inpaintCommand(int32_t radius) {
  JsonValue p = JsonValue::object();
  p.set("radius", JsonValue::number(radius));
  return command("filter_inpaint", std::move(p));
}

Command removeLightingGradientCommand(float sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return command("filter_remove_lighting_gradient", std::move(p));
}

Command offsetCommand(float dxFraction, float dyFraction, OffsetEdge edge) {
  JsonValue p = JsonValue::object();
  p.set("dx_fraction", JsonValue::number(dxFraction));
  p.set("dy_fraction", JsonValue::number(dyFraction));
  p.set("edge", JsonValue::string(offsetEdgeName(edge)));
  return command("filter_offset", std::move(p));
}

Command deleteSelectionCommand() { return command("delete_selection", JsonValue::object()); }

Command numericTransformCommand(float rotateDegrees, float scaleXPercent, float scaleYPercent,
                                float translateX, float translateY) {
  JsonValue p = JsonValue::object();
  p.set("rotate_degrees", JsonValue::number(rotateDegrees));
  p.set("scale_x_percent", JsonValue::number(scaleXPercent));
  p.set("scale_y_percent", JsonValue::number(scaleYPercent));
  p.set("translate_x", JsonValue::number(translateX));
  p.set("translate_y", JsonValue::number(translateY));
  return command("numeric_transform", std::move(p));
}

}  // namespace np
