#include "app/CommandsFill.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/CommandSupport.hpp"
#include "app/PixelOpBridge.hpp"
#include "core/Blend.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "ops/Fill.hpp"
#include "ops/Pattern.hpp"

// app/CommandsFill -- the command rows for PRD D26: `fill` and `stroke`, both
// through `ops/Fill.hpp`'s one engine (that header's own comment argues why
// stroke is not a second engine).
//
// Every adapter here follows `app/CommandsImage.cpp`'s own rule, restated
// once rather than reworded: read the parameters, refuse what is missing or
// out of range **by name**, call the engine that already exists, and
// translate the result through app/CommandSupport.hpp. Nothing below
// reimplements `ops/Fill`, `core/SelectionRefine`'s grow/shrink, or
// `app/FilterOps.hpp`'s `compositeFilterResult()`.
//
// **`fill` reaches `applyPixelFilter()` directly** (app/PixelOpBridge.hpp),
// exactly as every other menu pixel op does: `ops/Fill::fillTiles()` matches
// that header's `Engine` shape, so the selection bound, the layer refusal and
// the one-history-entry rule all come for free and cannot drift from the
// twenty-six ops that already share them.
//
// **`stroke` cannot**, and that is a deliberate, narrow exception rather than
// a second copy of the bridge. `applyPixelFilter()`'s template composites its
// engine's output through `OpenDocument::selection` itself; a stroke's pixels
// belong on a BAND around that selection's edge (built by
// `core/SelectionRefine.hpp`'s grow/shrink, `ops/Fill.hpp`'s header explains
// the arithmetic), not inside it. So `doStroke()` below calls `fillTiles()`
// and `compositeFilterResult()` directly, passing the band in place of
// `doc.selection` -- the same two functions `applyPixelFilter()` calls,
// unbundled by exactly the one substitution stroke needs and no more.
namespace np {
namespace {

// ==========================================================================
// The parameter readers -- app/CommandsImage.cpp's own small copies, kept
// local rather than shared: each command family file is a separate
// translation unit precisely so two of them being built in parallel do not
// conflict (app/Command.hpp's own "where the rows come from" section), and a
// shared reader header would be one more file in that conflict set for a
// dozen one-line functions.
// ==========================================================================

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
  if (!std::isfinite(d)) return refuseValue(id, key, "a finite number");
  *io = static_cast<float>(d);
  return {};
}

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

// ==========================================================================
// The three small enum<->name vocabularies this family adds. `GradientKind`
// and `GradientSpread` have never crossed a file before this track (the
// gradient TOOL crosses its own state through `AppState`, never through a
// command -- app/GradientTool.hpp's own header), so their wire names are
// coined here rather than found.
// ==========================================================================

std::optional<GradientKind> gradientKindFromWireName(std::string_view name) {
  if (name == "linear") return GradientKind::Linear;
  if (name == "radial") return GradientKind::Radial;
  if (name == "angular") return GradientKind::Angular;
  return std::nullopt;
}

const char* gradientKindWireName(GradientKind k) {
  switch (k) {
    case GradientKind::Linear: return "linear";
    case GradientKind::Radial: return "radial";
    case GradientKind::Angular: return "angular";
  }
  return "linear";
}

std::optional<GradientSpread> gradientSpreadFromWireName(std::string_view name) {
  if (name == "pad") return GradientSpread::Pad;
  if (name == "repeat") return GradientSpread::Repeat;
  if (name == "reflect") return GradientSpread::Reflect;
  return std::nullopt;
}

const char* gradientSpreadWireName(GradientSpread s) {
  switch (s) {
    case GradientSpread::Pad: return "pad";
    case GradientSpread::Repeat: return "repeat";
    case GradientSpread::Reflect: return "reflect";
  }
  return "pad";
}

std::optional<StrokeLocation> strokeLocationFromName(std::string_view name) {
  if (name == "inside") return StrokeLocation::Inside;
  if (name == "center") return StrokeLocation::Center;
  if (name == "outside") return StrokeLocation::Outside;
  return std::nullopt;
}

const char* strokeLocationName(StrokeLocation l) {
  switch (l) {
    case StrokeLocation::Inside: return "inside";
    case StrokeLocation::Center: return "center";
    case StrokeLocation::Outside: return "outside";
  }
  return "center";
}

std::optional<FillSource> fillSourceFromName(std::string_view name) {
  if (name == "color") return FillSource::Color;
  if (name == "pattern") return FillSource::Pattern;
  if (name == "gradient") return FillSource::Gradient;
  return std::nullopt;
}

const char* fillSourceName(FillSource s) {
  switch (s) {
    case FillSource::Color: return "color";
    case FillSource::Pattern: return "pattern";
    case FillSource::Gradient: return "gradient";
  }
  return "color";
}

// ==========================================================================
// `fill` and `stroke` share every key but `width`/`location`: both draw from
// a colour, a defined pattern or a gradient, through a blend mode and an
// opacity. One reader for that shared block, called by both `doFill()` and
// `doStroke()`, so the two commands cannot describe the same JSON shape two
// different ways.
// ==========================================================================

std::string readFillSourceParams(const JsonValue& params, const char* id, FillParams* p) {
  std::optional<FillSource> source;
  {
    const JsonValue* v = params.find("source");
    if (v == nullptr || v->isNull()) return refuseMissing(id, "source");
    if (!v->isString()) return refuseValue(id, "source", "a name, written as a string");
    source = fillSourceFromName(v->asString());
    if (!source)
      return std::string("refused: ") + id + " does not know a \"source\" named \"" +
             v->asString() + "\".";
  }
  p->source = *source;

  switch (*source) {
    case FillSource::Color: {
      const std::string why = readFixedArray<3>(params, id, "color", kRequired, &p->color);
      if (!why.empty()) return why;
      break;
    }
    case FillSource::Pattern: {
      const std::string name = params.stringOr("pattern", "");
      if (name.empty()) return refuseMissing(id, "pattern");
      const Pattern* pattern = sessionPatterns().findByName(name);
      if (pattern == nullptr)
        return std::string("refused: ") + id + " needs a pattern named \"" + name +
               "\" already defined in this session; patterns are session state and are not "
               "saved with a document, so Define Pattern has to run again after a restart.";
      if (!pattern->valid())
        return std::string("refused: the pattern named \"") + name + "\" holds no texels.";
      p->pattern = pattern;
      std::string why = readWhole(params, id, "pattern_origin_x", kOptional, &p->patternOriginX);
      if (why.empty())
        why = readWhole(params, id, "pattern_origin_y", kOptional, &p->patternOriginY);
      if (!why.empty()) return why;
      break;
    }
    case FillSource::Gradient: {
      if (params.find("gradient_kind") == nullptr) return refuseMissing(id, "gradient_kind");
      std::string why = readEnumByName(params, id, "gradient_kind", gradientKindFromWireName,
                                       &p->gradientGeometry.kind);
      if (why.empty())
        why = readEnumByName(params, id, "gradient_spread", gradientSpreadFromWireName,
                             &p->gradientGeometry.spread);
      if (why.empty())
        why = readNumber(params, id, "gradient_x0", kRequired, &p->gradientGeometry.x0);
      if (why.empty())
        why = readNumber(params, id, "gradient_y0", kRequired, &p->gradientGeometry.y0);
      if (why.empty())
        why = readNumber(params, id, "gradient_x1", kRequired, &p->gradientGeometry.x1);
      if (why.empty())
        why = readNumber(params, id, "gradient_y1", kRequired, &p->gradientGeometry.y1);
      if (!why.empty()) return why;

      const JsonValue* colorStops = params.find("gradient_color_stops");
      if (colorStops == nullptr || !colorStops->isArray() || colorStops->size() == 0)
        return refuseValue(id, "gradient_color_stops", "a non-empty array of colour stops");
      for (size_t i = 0; i < colorStops->size(); ++i) {
        const JsonValue& stop = colorStops->at(i);
        if (!stop.isObject())
          return refuseValue(id, "gradient_color_stops",
                             "objects (entry " + std::to_string(i) + " is not one)");
        ColorStop cs;
        why = readNumber(stop, id, "position", kRequired, &cs.position);
        if (why.empty()) why = readFixedArray<3>(stop, id, "color", kRequired, &cs.color);
        if (why.empty()) why = readNumber(stop, id, "midpoint", kOptional, &cs.midpoint);
        if (!why.empty()) return why;
        p->gradientStops.colorStops.push_back(cs);
      }
      for (size_t i = 1; i < p->gradientStops.colorStops.size(); ++i)
        if (p->gradientStops.colorStops[i].position < p->gradientStops.colorStops[i - 1].position)
          return refuseValue(id, "gradient_color_stops",
                             "sorted ascending by \"position\" -- ops/Gradient makes that the "
                             "caller's contract, and sorting it here would make the file render "
                             "differently from what it reads as");

      const JsonValue* opacityStops = params.find("gradient_opacity_stops");
      if (opacityStops != nullptr && !opacityStops->isNull()) {
        if (!opacityStops->isArray())
          return refuseValue(id, "gradient_opacity_stops", "an array of opacity stops");
        for (size_t i = 0; i < opacityStops->size(); ++i) {
          const JsonValue& stop = opacityStops->at(i);
          if (!stop.isObject())
            return refuseValue(id, "gradient_opacity_stops",
                               "objects (entry " + std::to_string(i) + " is not one)");
          OpacityStop os;
          why = readNumber(stop, id, "position", kRequired, &os.position);
          if (why.empty()) why = readNumber(stop, id, "opacity", kRequired, &os.opacity);
          if (why.empty()) why = readNumber(stop, id, "midpoint", kOptional, &os.midpoint);
          if (!why.empty()) return why;
          p->gradientStops.opacityStops.push_back(os);
        }
        for (size_t i = 1; i < p->gradientStops.opacityStops.size(); ++i)
          if (p->gradientStops.opacityStops[i].position <
              p->gradientStops.opacityStops[i - 1].position)
            return refuseValue(id, "gradient_opacity_stops",
                               "sorted ascending by \"position\", the same contract the colour "
                               "stops keep");
      }
      break;
    }
  }

  std::string why = readEnumByName(params, id, "blend", blendModeFromName, &p->blend);
  if (why.empty()) why = readNumber(params, id, "opacity", kOptional, &p->opacity);
  if (!why.empty()) return why;
  if (p->opacity < 0.0f)
    return refuseValue(id, "opacity", "zero or a positive fraction of the source's own coverage");
  if (p->blend == BlendMode::Mix)
    return std::string("refused: ") + id +
           "'s blend cannot be \"mix\" -- that blend is a Kubelka-Munk lerp between two Pigment "
           "layers' latents (core/Blend.hpp), and a fill's source is a colour, a pattern or a "
           "gradient, none of which has a latent to offer.";
  return {};
}

// ==========================================================================
// Readers
// ==========================================================================

CommandResult doFill(OpenDocument& doc, const JsonValue& params) {
  FillParams p;
  const char* kId = "fill";
  const std::string why = readFillSourceParams(params, kId, &p);
  if (!why.empty()) return commandRefused(why);
  return fromFilterResult(applyPixelFilter(doc, fillTiles, p, "fill"), doc, "fill");
}

// A selection whose coverage is the layer's own alpha. Stroked with nothing
// selected, as Photoshop does: grow/shrink put the traced edge where alpha
// crosses 50% (core/SelectionRefine.hpp's coverage/distance identity), so an
// antialiased edge stays sub-texel. Alpha under 1/510 quantises to unselected.
Selection layerContentSelection(const TileStore& tiles) {
  Selection sel;
  for (const auto& [coord, tile] : tiles) {
    SelectionTile coverage{};
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x)
        coverage.writeCoverage(PixelCoord{x, y}, tile.readPixel(PixelCoord{x, y})[3]);
    if (!coverage.selectsNothing()) sel.tiles.getOrCreate(coord) = coverage;
  }
  return sel;
}

std::string readStrokeParams(const JsonValue& params, StrokeParams* sp) {
  const char* kId = "stroke";
  std::string why = readFillSourceParams(params, kId, &sp->fill);
  if (why.empty()) why = readNumber(params, kId, "width", kRequired, &sp->width);
  if (!why.empty()) return why;
  if (!(sp->width > 0.0f))
    return refuseValue(kId, "width", "greater than zero; a zero-width stroke is the identity, "
                                     "which in a batch is a file written unmodified and reported "
                                     "as a success");
  return readEnumByName(params, kId, "location", strokeLocationFromName, &sp->location);
}

// Everything `stroke` decides, from a const document: the command and the
// Stroke dialog's preview both call this. `*composedOut` is the whole layer
// as the stroke would leave it.
std::string computeStroke(const OpenDocument& doc, const StrokeParams& sp, TileStore* composedOut,
                          size_t* changedOut) {
  const Layer* target = activeLayerOf(doc);
  const PixelOpRefusal refusal = pixelOpRefusalFor(target);
  if (refusal != PixelOpRefusal::None) return pixelOpRefusalMessage(refusal, target, "stroke");

  const TileStore original = *target->rgbTiles;
  Selection contentEdge;
  if (!doc.selection.has_value()) {
    contentEdge = layerContentSelection(original);
    if (selectionSelectsNothing(contentEdge))
      return "refused: stroke has no edge to trace. Nothing is selected, and the layer \"" +
             target->name + "\" has no non-transparent pixels to stroke around.";
  }
  const Selection& sel = doc.selection.has_value() ? *doc.selection : contentEdge;

  // The band `ops/Fill.hpp`'s header describes: `Inside` and `Outside` put
  // the whole width to one side of the selection's own edge; `Center` splits
  // it, `width / 2` to each side, which is exactly `grow` and `shrink` by
  // half the width -- `core/SelectionRefine.hpp`'s own single-sign `grow`
  // covers all three by the sign and magnitude of its argument alone.
  Selection band;
  switch (sp.location) {
    case StrokeLocation::Inside:
      band = combineSelections(sel, growSelection(sel, -sp.width), SelectionCombine::Subtract);
      break;
    case StrokeLocation::Outside:
      band = combineSelections(growSelection(sel, sp.width), sel, SelectionCombine::Subtract);
      break;
    case StrokeLocation::Center:
      band = combineSelections(growSelection(sel, sp.width * 0.5f),
                               growSelection(sel, -sp.width * 0.5f), SelectionCombine::Subtract);
      break;
  }

  const PixelRect canvasRect{0, 0, doc.document.width, doc.document.height};
  TileStore filtered;
  // `readStrokeParams()` already validated these; a failure here is that
  // reader's bug, named rather than reported as a 0-texel success.
  if (!fillTiles(original, canvasRect, sp.fill, &filtered))
    return "refused: stroke's fill parameters did not validate.";

  *composedOut = original;
  *changedOut = compositeFilterResult(original, filtered, canvasRect, &band, *composedOut);
  return {};
}

CommandResult doStroke(OpenDocument& doc, const JsonValue& params) {
  StrokeParams sp;
  std::string why = readStrokeParams(params, &sp);
  if (!why.empty()) return commandRefused(why);
  TileStore composed;
  size_t changed = 0;
  why = computeStroke(doc, sp, &composed, &changed);
  if (!why.empty()) return commandRefused(why);

  CommandResult r;
  r.ok = true;
  r.changesPixels = true;
  r.texelsChanged = changed;
  if (changed == 0) {
    r.status = "stroke: 0 texels changed";
    return r;
  }
  *activeLayerOf(doc)->rgbTiles = std::move(composed);
  doc.recordEdit("stroke", EditKind::Content);
  r.status = "stroke: " + std::to_string(changed) + " texels changed";
  return r;
}

}  // namespace

void registerFillCommands(std::vector<CommandSpec>* out) {
  const std::vector<std::string> fillParamNames{
      "source",       "color",           "pattern",  "pattern_origin_x",       "pattern_origin_y",
      "gradient_kind", "gradient_spread", "gradient_x0", "gradient_y0", "gradient_x1", "gradient_y1",
      "gradient_color_stops", "gradient_opacity_stops", "blend", "opacity"};

  out->push_back({"fill", "Fill", fillParamNames, pixelOpUnavailable, doFill,
                  /*selectionBounded=*/true});

  std::vector<std::string> strokeParamNames = fillParamNames;
  strokeParamNames.push_back("width");
  strokeParamNames.push_back("location");
  // `selectionBounded = true` even though an absent selection means "trace
  // the layer's content edge" rather than "the whole canvas": `app/Command.hpp`'s
  // own comment on `filter_inpaint` is the identical argument -- the recorder's
  // channel-match rule (app/Recorder.hpp §4) is the protection a live,
  // unsaved marquee needs, because replaying without it strokes a DIFFERENT
  // edge (the layer's, not the marquee's) than the one recorded.
  out->push_back({"stroke", "Stroke", strokeParamNames, pixelOpUnavailable, doStroke,
                  /*selectionBounded=*/true});
}

// ==========================================================================
// The encoders (beside the readers above)
// ==========================================================================

namespace {

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

// Every key `readFillSourceParams()` reads, written by the one case that
// applies to `f.source`. Shared by `fillCommand()` and `strokeCommand()` so
// the two encoders cannot describe the same `FillParams` two different ways.
void writeFillSourceParams(JsonValue& p, const FillParams& f) {
  p.set("source", JsonValue::string(fillSourceName(f.source)));
  switch (f.source) {
    case FillSource::Color:
      p.set("color", jsonTriple(f.color));
      break;
    case FillSource::Pattern:
      p.set("pattern", JsonValue::string(f.pattern != nullptr ? f.pattern->name : std::string()));
      p.set("pattern_origin_x", JsonValue::number(f.patternOriginX));
      p.set("pattern_origin_y", JsonValue::number(f.patternOriginY));
      break;
    case FillSource::Gradient: {
      p.set("gradient_kind", JsonValue::string(gradientKindWireName(f.gradientGeometry.kind)));
      p.set("gradient_spread",
            JsonValue::string(gradientSpreadWireName(f.gradientGeometry.spread)));
      p.set("gradient_x0", JsonValue::number(f.gradientGeometry.x0));
      p.set("gradient_y0", JsonValue::number(f.gradientGeometry.y0));
      p.set("gradient_x1", JsonValue::number(f.gradientGeometry.x1));
      p.set("gradient_y1", JsonValue::number(f.gradientGeometry.y1));
      JsonValue colorStops = JsonValue::array();
      for (const ColorStop& s : f.gradientStops.colorStops) {
        JsonValue one = JsonValue::object();
        one.set("position", JsonValue::number(s.position));
        one.set("color", jsonTriple(s.color));
        one.set("midpoint", JsonValue::number(s.midpoint));
        colorStops.push(std::move(one));
      }
      p.set("gradient_color_stops", std::move(colorStops));
      JsonValue opacityStops = JsonValue::array();
      for (const OpacityStop& s : f.gradientStops.opacityStops) {
        JsonValue one = JsonValue::object();
        one.set("position", JsonValue::number(s.position));
        one.set("opacity", JsonValue::number(s.opacity));
        one.set("midpoint", JsonValue::number(s.midpoint));
        opacityStops.push(std::move(one));
      }
      p.set("gradient_opacity_stops", std::move(opacityStops));
      break;
    }
  }
  p.set("blend", JsonValue::string(blendModeName(f.blend)));
  p.set("opacity", JsonValue::number(f.opacity));
}

}  // namespace

std::string previewFillCommand(const OpenDocument& doc, const Command& command, TileStore* layerOut,
                               size_t* changedOut) {
  *changedOut = 0;
  if (command.id == "fill") {
    FillParams p;
    const std::string why = readFillSourceParams(command.params, "fill", &p);
    if (!why.empty()) return why;
    // `doFill()` reaches the same function through `applyPixelFilter()`.
    const FilterOpResult r = computePixelFilter(doc, fillTiles, p, layerOut);
    if (r.refusal != PixelOpRefusal::None)
      return pixelOpRefusalMessage(r.refusal, activeLayerOf(doc), "fill");
    *changedOut = r.texelsChanged;
    return {};
  }
  if (command.id == "stroke") {
    StrokeParams sp;
    const std::string why = readStrokeParams(command.params, &sp);
    if (!why.empty()) return why;
    return computeStroke(doc, sp, layerOut, changedOut);
  }
  return "refused: \"" + command.id + "\" is neither fill nor stroke.";
}

Command fillCommand(const FillParams& f) {
  JsonValue p = JsonValue::object();
  writeFillSourceParams(p, f);
  return command("fill", std::move(p));
}

Command strokeCommand(const StrokeParams& sp) {
  JsonValue p = JsonValue::object();
  writeFillSourceParams(p, sp.fill);
  p.set("width", JsonValue::number(sp.width));
  p.set("location", JsonValue::string(strokeLocationName(sp.location)));
  return command("stroke", std::move(p));
}

Command definePatternCommand(const std::string& name) {
  JsonValue p = JsonValue::object();
  p.set("name", JsonValue::string(name));
  return command("define_pattern", std::move(p));
}

}  // namespace np
