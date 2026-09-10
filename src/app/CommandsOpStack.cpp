#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "app/CommandSupport.hpp"
#include "color/Space.hpp"
#include "core/Channels.hpp"
#include "core/LayerOps.hpp"
#include "core/OpStack.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "ops/Feather.hpp"
#include "ops/FloodFill.hpp"
#include "ops/PointOps.hpp"
#include "ops/ToneOps.hpp"

// app/CommandsOpStack -- the command rows for a layer's own non-destructive op
// stack, and for the selection.
//
// **The two belong together, and the reason is the action file.** An op-stack
// step carries an op, and an op's text encoding is io/ActionFile's problem;
// a selection step is what makes every *destructive* step in the same file
// mean what it meant when it was recorded (docs/automation-plan.md §7 -- a
// selection is session state and is never in a document file, so an action
// that does not record it silently applies to the whole canvas).
//
// ==========================================================================
// (1) An op travels as a JSON object keyed by its KIND NAME
// ==========================================================================
//
// The op-stack rows are the only commands in the table whose parameter is a
// whole other data structure rather than a number or a name, so the encoding
// is argued here rather than left to fall out of whatever the first adapter
// happened to write.
//
//     { "cmd": "add_layer_op", "layer": "Base",
//       "op": { "kind": "curves", "enabled": true,
//               "curves": [ [ {"x": 0.0, "y": 0.0} ], [], [] ] } }
//
// **Keyed by the kind NAME, and by neither of the two numbers that already
// exist for the same thing.** io/OpSerial's `npops1:` payload keys a record by
// an explicit wire code (0 Levels, 1 Curves, ... 8 Threshold), and
// `core::PointOpKind` has an ordinal that today happens to agree with it.
// Neither belongs in this file:
//
//  * The **ordinal** moves the moment someone inserts rather than appends.
//    core/OpStack.hpp says "appended, never inserted" precisely because it is
//    load-bearing, and a format that depends on a comment staying obeyed is a
//    format that will one day open a file confidently wrong.
//  * The **wire code** is io/OpSerial's private business and is stable, but it
//    is a number in a file a human is meant to read and diff (PRD P5).
//    `"kind": 6` tells a reviewer nothing; `"kind": "invert"` tells them
//    everything.
//
// It is not `pointOpKindName()` either, and that is the trap this paragraph
// exists to name. That function returns the GRADE panel's row text -- "Channel
// Mixer", capitals and a space -- which is UI copy of exactly the kind
// docs/automation-plan.md §5 forbids keying a format by. Renaming a panel row
// must not change what an existing action file means, so the ids below are
// their own table, lower_snake_case like every other command id, and the two
// lists are deliberately allowed to differ.
//
// `opKindId()` is a switch rather than a table literal, so `-Werror=switch`
// (already on, src/CMakeLists.txt) stops the build when a tenth PointOpKind
// lands without an id -- the same reason ui/MacPaintUI.cpp derives its kind
// combo from `pointOpKindName()` rather than from a hand-maintained parallel
// array, after a hand-maintained one left that combo silently short.
//
// ==========================================================================
// (2) Every field of every kind is written, and the floats survive exactly
// ==========================================================================
//
// The keys under `"op"` are the params structs' own member names, verbatim
// (`blackIn`, `lumaWeights`, `matrix`, `stops`). Restating them under prettier
// names would be one more mapping to keep in step with ops/PointOps.hpp and
// ops/ToneOps.hpp, for no gain.
//
// One consequence worth stating: `"levels"` carries an array of three
// per-channel objects under `"kind": "levels"` and a plain bin count under
// `"kind": "posterize"`, because `Op::levels` and `PosterizeParams::levels`
// are both called that. It is unambiguous -- `"kind"` is read first and each
// kind has its own reader -- and it is preferable to inventing a name that
// does not match the struct the adapter fills.
//
// Exactness is io/Json's rather than this file's: `JsonValue::write()` emits
// the shortest decimal that reads back to the *identical* double, and every
// `float` here widens to a double losslessly and narrows back the same way.
// So this file's whole obligation for the round trip is **not to drop a
// field**, which is what the nine-kind sweep in app/selftest/CommandsOpStack
// asserts -- comparing bit patterns rather than with `==`, because a near-miss
// that `==` accepts is the exact defect that assertion exists to catch.
//
// ==========================================================================
// (3) A step this build cannot evaluate REFUSES; it does not become an inert
//     entry
// ==========================================================================
//
// core/OpStack.hpp's `OpClass::Unknown` exists so that a *document* written by
// a newer build survives a round trip through this one: the record is carried
// verbatim, in place, inert, and written back unchanged (PRD I10). That is the
// right answer for a document and the wrong answer for an action, and the two
// must not be confused:
//
//     a document is *read and written*; an action is *executed*.
//
// An action step storing an op this build cannot evaluate would replay as a
// stack whose grade is missing one entry, composite happily, and write thirty
// output files that look correct and are not (docs/automation-plan.md §7). So
// `opFromJson()` refuses -- by name -- both an unknown kind id and any
// `"class"` other than `point_a`, and it refuses *before* `addLayerOp()` is
// called, so nothing is stored at all.
//
// ==========================================================================
// (4) The five refine rows, and why a selection command is in this table at
//     all
// ==========================================================================
//
// PRD E4/E8/E9's grow, shrink, feather, colour range and luminance range write
// `OpenDocument::selection`, which is session state and never reaches a file
// (app/DocumentLifecycle.hpp says so deliberately). That looks at first like
// the disqualifier docs/automation-plan.md §1 states -- "a command is
// recordable iff it can be expressed as a function of an `OpenDocument`
// alone" -- and it is not, because `OpenDocument` is exactly what the rule
// names. `selection` is a member of it. What the rule excludes is `AppState`:
// the window, the tool, the view, the recent-file list. `select_all`,
// `deselect` and `invert_selection` were registered on that reading already;
// these five are the same argument with parameters.
//
// **Every number the dialog held becomes an explicit parameter, and the
// colour travels by value.** ui/MacPaintUI.cpp's five popups keep their
// radius, tolerance, band and swatch in dialog-local statics, and the swatch
// in particular is a colour the *session* is holding. A row that reached for
// it -- for the swatch, or for `AppState`'s foreground -- would replay
// differently on Tuesday than it did on Monday, on the same document, and
// report success both times. So `select_colour_range` carries its colour in
// the step: `"colour": [r, g, b]`, display-encoded sRGB, the same three
// numbers the swatch showed.
//
// **The engine is called directly, not through ui/MacPaintUI.hpp's
// `applySelectRefineAction()` family.** Those four functions are the *UI's*
// dialog-to-engine boundary and app/ must not include ui/ -- §7 of the plan
// again, the UI path calls the document path and never the reverse. That
// leaves two boundaries doing the same decode until step 2 migrates the call
// sites and deletes the UI half, so app/selftest/CommandsOpStack.cpp asserts
// the two agree bit for bit rather than trusting a comment to keep them in
// step.
//
// **A refine pushes the selection it replaced onto `refineUndoStack`**, which
// is what makes Select > Undo Refine keep working once step 2 routes the menu
// items through `applyCommand()`. Not doing it would turn that migration --
// which is supposed to change no behaviour at all -- into the silent deletion
// of a menu item's effect.
//
// **None of the five is `selectionBounded`** (app/Command.hpp). The live
// selection is what they operate *on*, not a mask that bounds where they
// reach, and an absent one is a named refusal from their precondition rather
// than the silent "no restriction means the whole canvas" that flag exists to
// police.
namespace np {

// --- the op encoding -----------------------------------------------------

// The stable id for one point-op kind. A switch, so a tenth kind cannot be
// added without the build stopping here -- see this file's §1.
const char* opKindId(PointOpKind kind) noexcept {
  switch (kind) {
    case PointOpKind::Levels:       return "levels";
    case PointOpKind::Curves:       return "curves";
    case PointOpKind::Exposure:     return "exposure";
    case PointOpKind::Saturation:   return "saturation";
    case PointOpKind::Grayscale:    return "grayscale";
    case PointOpKind::ChannelMixer: return "channel_mixer";
    case PointOpKind::Invert:       return "invert";
    case PointOpKind::Posterize:    return "posterize";
    case PointOpKind::Threshold:    return "threshold";
  }
  // Unreachable for a valid enumerator, and an empty string rather than a
  // name so that a value cast in from outside the enum can never match a
  // `"kind"` a file asked for.
  return "";
}

// The only class id this build will execute. A `"class"` key is optional and
// defaults to this; anything else is a newer build's op and is refused rather
// than stored (§3).
constexpr const char* kPointAClassId = "point_a";

namespace {

// Derived from the enum rather than restated beside it, for the reason
// ui/MacPaintUI.cpp:1462 gives about its own kind combo: a hand-maintained
// count is the one seam a new PointOpKind reaches without `-Wswitch` saying a
// word.
constexpr int kPointOpKindCount = static_cast<int>(PointOpKind::Threshold) + 1;

JsonValue numberJson(float v) { return JsonValue::number(static_cast<double>(v)); }

JsonValue weightsJson(const std::array<float, 3>& w) {
  JsonValue a = JsonValue::array();
  for (float v : w) a.push(numberJson(v));
  return a;
}

// Three floats from an array-valued key. An absent key keeps the params
// struct's own default; a key of the wrong shape is a sentence, so a hand
// written action that typed `lumaWeight` is told what it got wrong instead of
// silently grading against Rec.709.
std::string readWeights(const JsonValue& op, const char* key, std::array<float, 3>* out) {
  const JsonValue* a = op.find(key);
  if (a == nullptr) return {};
  if (!a->isArray() || a->size() != 3)
    return std::string("refused: an op's \"") + key + "\" must be an array of three numbers.";
  for (size_t i = 0; i < 3; ++i) {
    if (!a->at(i).isNumber())
      return std::string("refused: an op's \"") + key + "\" must be an array of three numbers.";
    (*out)[i] = static_cast<float>(a->at(i).asNumber());
  }
  return {};
}

float floatOr(const JsonValue& v, const char* key, float fallback) {
  return static_cast<float>(v.numberOr(key, static_cast<double>(fallback)));
}

}  // namespace

// One `core::Op` as the JSON an action file carries.
//
// Refuses a non-PointA entry rather than encoding it, which is the *write*
// half of §3: PRD P6's converter ("any document's existing op stack is already
// one") must not turn a stack containing an unrecognised entry into an action
// that quietly grades with one fewer op than the document did. Returns a null
// JsonValue and fills `*errorOut` in that case.
JsonValue opToJson(const Op& op, std::string* errorOut) {
  if (op.opClass != OpClass::PointA) {
    if (errorOut != nullptr) {
      *errorOut = "refused: this op stack holds a " + opDisplayName(op) +
                  ", which is not a point op. An action is executed, so a step it cannot "
                  "evaluate is refused rather than carried inert -- carrying it would write "
                  "files that look correct and are graded with one op missing.";
    }
    return JsonValue::null();
  }
  JsonValue out = JsonValue::object();
  // Key order is the order these are set, and io/Json preserves it (PRD P5's
  // diffability). `kind` goes first, because it is what a reader needs before
  // any other key means anything.
  out.set("kind", JsonValue::string(opKindId(op.pointKind)));
  out.set("enabled", JsonValue::boolean(op.enabled));
  switch (op.pointKind) {
    case PointOpKind::Levels: {
      JsonValue channels = JsonValue::array();
      for (const LevelsParams& p : op.levels) {
        JsonValue c = JsonValue::object();
        c.set("blackIn", numberJson(p.blackIn));
        c.set("whiteIn", numberJson(p.whiteIn));
        c.set("gamma", numberJson(p.gamma));
        c.set("blackOut", numberJson(p.blackOut));
        c.set("whiteOut", numberJson(p.whiteOut));
        channels.push(std::move(c));
      }
      out.set("levels", std::move(channels));
      break;
    }
    case PointOpKind::Curves: {
      JsonValue channels = JsonValue::array();
      for (const Curve& curve : op.curves) {
        JsonValue points = JsonValue::array();
        for (const CurvePoint& p : curve) {
          JsonValue pt = JsonValue::object();
          pt.set("x", numberJson(p.x));
          pt.set("y", numberJson(p.y));
          points.push(std::move(pt));
        }
        channels.push(std::move(points));
      }
      out.set("curves", std::move(channels));
      break;
    }
    case PointOpKind::Exposure:
      out.set("stops", numberJson(op.exposure.stops));
      break;
    case PointOpKind::Saturation:
      out.set("scale", numberJson(op.saturation.scale));
      out.set("lumaWeights", weightsJson(op.saturation.lumaWeights));
      break;
    case PointOpKind::Grayscale:
      out.set("lumaWeights", weightsJson(op.grayscale.lumaWeights));
      break;
    case PointOpKind::ChannelMixer: {
      JsonValue rows = JsonValue::array();
      for (const std::array<float, 4>& row : op.channelMixer.matrix) {
        JsonValue r = JsonValue::array();
        for (float v : row) r.push(numberJson(v));
        rows.push(std::move(r));
      }
      out.set("matrix", std::move(rows));
      break;
    }
    case PointOpKind::Invert:
      // The domain travels as a name for the same reason the kind does:
      // `InvertParams::Domain` is an enum and io/OpSerial writes it as a u16,
      // and neither an ordinal nor a wire code belongs in a file a human
      // diffs.
      out.set("domain", JsonValue::string(op.invert.domain == InvertParams::Domain::Display
                                              ? "display"
                                              : "linear"));
      out.set("amount", numberJson(op.invert.amount));
      break;
    case PointOpKind::Posterize:
      out.set("levels", JsonValue::number(static_cast<double>(op.posterize.levels)));
      break;
    case PointOpKind::Threshold:
      out.set("threshold", numberJson(op.threshold.threshold));
      out.set("amount", numberJson(op.threshold.amount));
      break;
  }
  return out;
}

// The inverse. Returns false, leaving `*out` untouched, with `*errorOut`
// naming what was wrong -- a malformed `"op"`, a class this build will not
// execute, or a kind id it does not know.
//
// **Every absent field falls back to the params struct's own default**, which
// is why this constructs an `Op` and overwrites rather than assembling fields
// from scratch: ops/ToneOps.hpp argues at length that a default-constructed
// `InvertParams` deliberately means a *full* invert rather than an identity,
// and re-deciding any such default here would give an action a different
// meaning from the panel that recorded it.
bool opFromJson(const JsonValue& value, Op* out, std::string* errorOut) {
  auto refuse = [&](std::string why) {
    if (errorOut != nullptr) *errorOut = std::move(why);
    return false;
  };
  if (!value.isObject())
    return refuse("refused: this step needs an \"op\" object carrying at least a \"kind\".");

  // §3. An explicit class is optional; when present it must be the one class
  // this build can evaluate. A newer build's `"class": "spatial_b"` is refused
  // here, by name, and nothing is stored.
  const JsonValue* cls = value.find("class");
  if (cls != nullptr && (!cls->isString() || cls->asString() != kPointAClassId)) {
    const std::string named = cls->isString() ? cls->asString() : std::string("(not a name)");
    return refuse("refused: this op's class is \"" + named +
                  "\", and this build can only execute \"" + kPointAClassId +
                  "\" ops. A document may carry an op it cannot evaluate, because a document "
                  "is read and written; an action is executed, and a step that is skipped "
                  "writes a file that looks correct and is not.");
  }

  const JsonValue* kindValue = value.find("kind");
  if (kindValue == nullptr || !kindValue->isString())
    return refuse("refused: this op has no \"kind\" name. An op is keyed by its kind name, "
                  "never by a wire code and never by an enum ordinal.");
  const std::string kindId = kindValue->asString();
  std::optional<PointOpKind> found;
  for (int k = 0; k < kPointOpKindCount; ++k) {
    const PointOpKind candidate = static_cast<PointOpKind>(k);
    if (kindId == opKindId(candidate)) {
      found = candidate;
      break;
    }
  }
  if (!found)
    return refuse("refused: this build knows no op kind called \"" + kindId +
                  "\". An action from a newer build is refused rather than run with the step "
                  "skipped.");

  Op op;
  op.opClass = OpClass::PointA;
  op.pointKind = *found;
  op.enabled = value.boolOr("enabled", true);
  switch (*found) {
    case PointOpKind::Levels: {
      const JsonValue* channels = value.find("levels");
      if (channels != nullptr) {
        if (!channels->isArray() || channels->size() != 3)
          return refuse("refused: a levels op's \"levels\" must be an array of three "
                        "per-channel objects (R, G, B).");
        for (size_t i = 0; i < 3; ++i) {
          const JsonValue& c = channels->at(i);
          if (!c.isObject())
            return refuse("refused: a levels op's \"levels\" must be an array of three "
                          "per-channel objects (R, G, B).");
          LevelsParams& p = op.levels[i];
          p.blackIn = floatOr(c, "blackIn", p.blackIn);
          p.whiteIn = floatOr(c, "whiteIn", p.whiteIn);
          p.gamma = floatOr(c, "gamma", p.gamma);
          p.blackOut = floatOr(c, "blackOut", p.blackOut);
          p.whiteOut = floatOr(c, "whiteOut", p.whiteOut);
        }
      }
      break;
    }
    case PointOpKind::Curves: {
      const JsonValue* channels = value.find("curves");
      if (channels != nullptr) {
        if (!channels->isArray() || channels->size() != 3)
          return refuse("refused: a curves op's \"curves\" must be an array of three "
                        "per-channel point lists (R, G, B).");
        for (size_t i = 0; i < 3; ++i) {
          const JsonValue& points = channels->at(i);
          if (!points.isArray())
            return refuse("refused: a curves op's \"curves\" must be an array of three "
                          "per-channel point lists (R, G, B).");
          Curve curve;
          curve.reserve(points.size());
          for (size_t j = 0; j < points.size(); ++j) {
            const JsonValue& pt = points.at(j);
            if (!pt.isObject() || !pt.hasNumber("x") || !pt.hasNumber("y"))
              return refuse("refused: a curve control point must be an object with an \"x\" "
                            "and a \"y\".");
            curve.push_back(CurvePoint{static_cast<float>(pt.numberOr("x", 0.0)),
                                       static_cast<float>(pt.numberOr("y", 0.0))});
          }
          // Deliberately NOT sorted here. ops/PointOps.hpp states that
          // ascending-by-x is the caller's contract, and silently reordering
          // an author's control points would make a hand-edited action mean
          // something other than what it says.
          op.curves[i] = std::move(curve);
        }
      }
      break;
    }
    case PointOpKind::Exposure:
      op.exposure.stops = floatOr(value, "stops", op.exposure.stops);
      break;
    case PointOpKind::Saturation: {
      op.saturation.scale = floatOr(value, "scale", op.saturation.scale);
      const std::string why = readWeights(value, "lumaWeights", &op.saturation.lumaWeights);
      if (!why.empty()) return refuse(why);
      break;
    }
    case PointOpKind::Grayscale: {
      const std::string why = readWeights(value, "lumaWeights", &op.grayscale.lumaWeights);
      if (!why.empty()) return refuse(why);
      break;
    }
    case PointOpKind::ChannelMixer: {
      const JsonValue* rows = value.find("matrix");
      if (rows != nullptr) {
        if (!rows->isArray() || rows->size() != 3)
          return refuse("refused: a channel_mixer op's \"matrix\" must be three rows of four "
                        "numbers each.");
        for (size_t r = 0; r < 3; ++r) {
          const JsonValue& row = rows->at(r);
          if (!row.isArray() || row.size() != 4)
            return refuse("refused: a channel_mixer op's \"matrix\" must be three rows of four "
                          "numbers each.");
          for (size_t c = 0; c < 4; ++c) {
            if (!row.at(c).isNumber())
              return refuse("refused: a channel_mixer op's \"matrix\" must be three rows of "
                            "four numbers each.");
            op.channelMixer.matrix[r][c] = static_cast<float>(row.at(c).asNumber());
          }
        }
      }
      break;
    }
    case PointOpKind::Invert: {
      const JsonValue* domain = value.find("domain");
      if (domain != nullptr) {
        if (!domain->isString() ||
            (domain->asString() != "linear" && domain->asString() != "display")) {
          const std::string named =
              domain->isString() ? domain->asString() : std::string("(not a name)");
          return refuse("refused: an invert op's \"domain\" is \"" + named +
                        "\"; it must be \"linear\" or \"display\".");
        }
        op.invert.domain = domain->asString() == "display" ? InvertParams::Domain::Display
                                                           : InvertParams::Domain::Linear;
      }
      op.invert.amount = floatOr(value, "amount", op.invert.amount);
      break;
    }
    case PointOpKind::Posterize: {
      const JsonValue* levels = value.find("levels");
      if (levels != nullptr) {
        if (!levels->isNumber() || levels->asNumber() != std::floor(levels->asNumber()))
          return refuse("refused: a posterize op's \"levels\" must be a whole bin count.");
        op.posterize.levels = static_cast<int>(levels->asNumber());
      }
      break;
    }
    case PointOpKind::Threshold:
      op.threshold.threshold = floatOr(value, "threshold", op.threshold.threshold);
      op.threshold.amount = floatOr(value, "amount", op.threshold.amount);
      break;
  }
  *out = std::move(op);
  return true;
}

namespace {

// --- shared parameter reading -------------------------------------------

// The `"op"` member of a step, decoded. `resolveTarget()`'s shape: an empty
// string when it worked.
std::string readOp(const JsonValue& params, const char* commandId, Op* out) {
  const JsonValue* value = params.find("op");
  if (value == nullptr) return std::string("refused: ") + commandId + " needs an \"op\" object.";
  std::string why;
  if (!opFromJson(*value, out, &why)) return why;
  return {};
}

// A whole, non-negative index. `core::LayerOps` already bounds-checks the
// value against the stack and returns a sentence rather than throwing (its own
// header says why that guard is there), so this only has to reject what cannot
// become a `size_t` at all -- a negative or fractional one, which would
// otherwise wrap to an enormous index and be refused with a number nobody
// typed.
std::string readIndex(const JsonValue& params, const char* key, const char* commandId,
                      size_t* out) {
  if (!params.hasNumber(key))
    return std::string("refused: ") + commandId + " needs a \"" + key + "\" op index.";
  const double v = params.numberOr(key, -1.0);
  if (v < 0.0 || v != std::floor(v))
    return std::string("refused: ") + commandId + "'s \"" + key +
           "\" must be a whole op index of at least 0.";
  *out = static_cast<size_t>(v);
  return {};
}

// --- the op-stack adapters ----------------------------------------------

CommandResult doAddLayerOp(OpenDocument& doc, const JsonValue& params) {
  size_t layer = 0;
  const std::string target = resolveTarget(doc, params, &layer);
  if (!target.empty()) return commandRefused(target);
  Op op;
  const std::string why = readOp(params, "add_layer_op", &op);
  if (!why.empty()) return commandRefused(why);
  return fromDocumentOpResult(recordLayerEdit(doc, addLayerOp(doc.document, layer, std::move(op))),
                              "add layer op");
}

CommandResult doRemoveLayerOp(OpenDocument& doc, const JsonValue& params) {
  size_t layer = 0;
  const std::string target = resolveTarget(doc, params, &layer);
  if (!target.empty()) return commandRefused(target);
  size_t index = 0;
  const std::string why = readIndex(params, "index", "remove_layer_op", &index);
  if (!why.empty()) return commandRefused(why);
  return fromDocumentOpResult(recordLayerEdit(doc, removeLayerOp(doc.document, layer, index)),
                              "remove layer op");
}

CommandResult doMoveLayerOp(OpenDocument& doc, const JsonValue& params) {
  size_t layer = 0;
  const std::string target = resolveTarget(doc, params, &layer);
  if (!target.empty()) return commandRefused(target);
  size_t from = 0;
  std::string why = readIndex(params, "from", "move_layer_op", &from);
  if (!why.empty()) return commandRefused(why);
  size_t to = 0;
  why = readIndex(params, "to", "move_layer_op", &to);
  if (!why.empty()) return commandRefused(why);
  return fromDocumentOpResult(recordLayerEdit(doc, moveLayerOp(doc.document, layer, from, to)),
                              "move layer op");
}

CommandResult doSetLayerOp(OpenDocument& doc, const JsonValue& params) {
  size_t layer = 0;
  const std::string target = resolveTarget(doc, params, &layer);
  if (!target.empty()) return commandRefused(target);
  size_t index = 0;
  std::string why = readIndex(params, "index", "set_layer_op", &index);
  if (!why.empty()) return commandRefused(why);
  Op op;
  why = readOp(params, "set_layer_op", &op);
  if (!why.empty()) return commandRefused(why);
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerOp(doc.document, layer, index, std::move(op))), "set layer op");
}

CommandResult doSetLayerOpEnabled(OpenDocument& doc, const JsonValue& params) {
  size_t layer = 0;
  const std::string target = resolveTarget(doc, params, &layer);
  if (!target.empty()) return commandRefused(target);
  size_t index = 0;
  const std::string why = readIndex(params, "index", "set_layer_op_enabled", &index);
  if (!why.empty()) return commandRefused(why);
  const JsonValue* enabled = params.find("enabled");
  if (enabled == nullptr || !enabled->isBool())
    return commandRefused("refused: set_layer_op_enabled needs an \"enabled\" true or false. "
                          "Defaulting it would let an action that meant to disable an op "
                          "silently enable it.");
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerOpEnabled(doc.document, layer, index, enabled->asBool())),
      "set layer op enabled");
}

// --- the selection adapters ----------------------------------------------
//
// **These write `doc.selection` and bump `doc.selectionRevision` here rather
// than calling `ui::installSelection()`, and that is not an oversight.** That
// function has internal linkage inside ui/MacPaintUI.cpp and is the UI's route
// to the same three fields; docs/automation-plan.md §7 is explicit that the UI
// path calls the document path and never the reverse, because a command that
// reaches for session state stops being replayable headlessly and `--batch`
// then starts differing from the interactive path silently.
//
// The one piece of bookkeeping duplicated rather than shared is
// `lastDeselected`, and it is duplicated deliberately: Reselect must undo a
// deselect issued from an action exactly as it undoes one issued from a key.
// When step 2's call-site migration hoists `installSelection()` out of the UI,
// this helper is what it should become.
void installSelectionForCommand(OpenDocument& doc, std::optional<Selection> selection) {
  if (!selection.has_value() && doc.selection.has_value()) doc.lastDeselected = doc.selection;
  doc.selection = std::move(selection);
  ++doc.selectionRevision;
}

CommandResult selectionChanged(std::string status) {
  CommandResult r;
  r.ok = true;
  // `changesPixels` stays false: a selection command changes no texel by
  // construction, so the replayer's "this step changed nothing" warning must
  // not fire on it (app/Command.hpp's note on that field).
  r.status = std::move(status);
  return r;
}

CommandResult doSelectAll(OpenDocument& doc, const JsonValue&) {
  installSelectionForCommand(doc, selectAll(doc.document.width, doc.document.height));
  return selectionChanged("select all: the whole canvas is selected");
}

CommandResult doDeselect(OpenDocument& doc, const JsonValue&) {
  installSelectionForCommand(doc, std::nullopt);
  return selectionChanged("deselect: nothing is selected, so nothing is restricted");
}

CommandResult doInvertSelection(OpenDocument& doc, const JsonValue&) {
  installSelectionForCommand(
      doc, invertSelection(*doc.selection, doc.document.width, doc.document.height));
  return selectionChanged("invert selection: done");
}

// **Deliberately a refusal rather than the UI's silent no-op.** ui/MacPaintUI
// ignores Cmd-Shift-I with nothing selected and argues for it: absent means
// "no restriction", so its honest complement selects nothing, and a single
// keystroke should not be able to leave the editor refusing every edit with no
// marching ants to explain why. In an action that same silence is the failure
// mode docs/automation-plan.md §7 names -- a step that quietly did not run, in
// a batch of thirty files that all report success -- so here it is named.
std::string invertSelectionUnavailable(const OpenDocument& doc, const JsonValue&) {
  if (!doc.selection.has_value())
    return "refused: invert_selection needs a selection to invert, and this document has none. "
           "An absent selection means \"no restriction\" rather than \"everything\", so "
           "inverting it would select nothing and refuse every step that followed.";
  return {};
}

// The channel name a selection step addresses. Never optional: the whole point
// of these two rows is that an action names a saved channel, and a default
// would pick a channel the author never wrote down.
std::string readChannelName(const JsonValue& params, const char* commandId, std::string* out) {
  const JsonValue* name = params.find("channel");
  if (name == nullptr || !name->isString() || name->asString().empty())
    return std::string("refused: ") + commandId +
           " needs a \"channel\" name. A selection is session state and is never written to a "
           "document file, so naming a saved channel is the only way an action can carry one; "
           "recording marquee coordinates would be meaningless at another resolution.";
  *out = name->asString();
  return {};
}

std::string saveSelectionUnavailable(const OpenDocument& doc, const JsonValue& params) {
  std::string name;
  const std::string why = readChannelName(params, "save_selection_as_channel", &name);
  if (!why.empty()) return why;
  if (!doc.selection.has_value())
    return "refused: save_selection_as_channel needs an active selection, and this document has "
           "none. An absent selection means \"no restriction\" rather than \"everything\", so "
           "there is no coverage to write into a channel.";
  return {};
}

CommandResult doSaveSelectionAsChannel(OpenDocument& doc, const JsonValue& params) {
  std::string name;
  const std::string why = readChannelName(params, "save_selection_as_channel", &name);
  if (!why.empty()) return commandRefused(why);
  const size_t index = saveSelectionAsChannel(doc.document, *doc.selection, name);
  // **A channel is appended under a uniquified name and never replaces one**
  // (core/Channels.hpp: destroying a saved selection has to be a delete). So
  // the name that comes back may not be the name that was asked for, and a
  // later `load_channel_as_selection` step naming the requested one would load
  // the OLDER channel and bound every following step to the wrong region.
  // That is a warning, never a silent success.
  const std::string actual = doc.document.channels[index].name;
  // A channel is `Document` data -- unlike the active selection, it is in
  // history snapshots and is written to the file (app/DocumentLifecycle.hpp on
  // `OpenDocument::selection`) -- so this one selection command really is an
  // undoable structural edit, and the other four are not.
  doc.recordEdit("save selection as channel", EditKind::Structural);
  CommandResult r = selectionChanged("save selection as channel: \"" + actual + "\"");
  if (actual != name) {
    r.warnings.push_back("\"" + name +
                         "\" was already a channel in this document, so the selection was saved "
                         "as \"" +
                         actual + "\" instead. A later step loading \"" + name +
                         "\" will load the earlier channel, not this one.");
  }
  return r;
}

// The channel has to exist, and the refusal has to name it. This is the
// precondition rather than a check inside the applier because
// docs/automation-plan.md §5 has the replayer consult preconditions before it
// applies anything, so a missing channel stops the run before a single
// destructive step has run against the whole canvas.
std::string loadChannelUnavailable(const OpenDocument& doc, const JsonValue& params) {
  std::string name;
  const std::string why = readChannelName(params, "load_channel_as_selection", &name);
  if (!why.empty()) return why;
  if (findChannel(doc.document, name) == nullptr)
    return "refused: this document has no channel named \"" + name +
           "\". Without it every step that follows would apply to the whole canvas instead of "
           "the region the action was recorded against.";
  return {};
}

CommandResult doLoadChannelAsSelection(OpenDocument& doc, const JsonValue& params) {
  std::string name;
  const std::string why = readChannelName(params, "load_channel_as_selection", &name);
  if (!why.empty()) return commandRefused(why);
  std::optional<Selection> loaded = loadChannelAsSelection(doc.document, name);
  // `std::nullopt` means there is no such channel. Answering a lookup failure
  // by installing an empty, engaged selection would disable the editor
  // (core/SelectionMask.hpp's three-absences section), so it is a refusal
  // rather than a fallback -- and it stays here as well as in the precondition
  // because `applyCommand()` is not the only future caller of an applier.
  if (!loaded)
    return commandRefused("refused: this document has no channel named \"" + name + "\".");
  installSelectionForCommand(doc, std::move(loaded));
  return selectionChanged("load channel as selection: \"" + name + "\"");
}

// --- the five refine adapters (§4) ---------------------------------------

// The app-side twin of `ui::installRefinedSelection()`: push what is being
// replaced onto the refine-undo stack, then install. Duplicated rather than
// shared for the reason `installSelectionForCommand()` above is duplicated --
// the UI's copy has internal linkage in ui/MacPaintUI.cpp and app/ must not
// include ui/ -- and it exists at all so that step 2's call-site migration,
// which is meant to change no behaviour, does not quietly delete Select >
// Undo Refine.
//
// Pushed BEFORE the install moves `doc.selection` out from under this read,
// which is the ordering that makes "one entry per refine" true rather than
// aspirational; the UI copy states the same thing at its own push.
void installRefinedSelectionForCommand(OpenDocument& doc, std::optional<Selection> refined) {
  doc.refineUndoStack.push_back(doc.selection);
  installSelectionForCommand(doc, std::move(refined));
}

// A finite, strictly positive pixel radius.
//
// **Zero is refused rather than run.** `growSelection(s, 0)` is a documented
// no-op (core/SelectionRefine.hpp), and a no-op step in a thirty-file batch is
// the exact silent success docs/automation-plan.md §7 is written against --
// the same reason `filter_gaussian_blur` refuses a sigma of zero.
//
// **Negative is refused rather than folded.** `shrinkSelection()` is defined
// as `growSelection(selection, -radius)`, so a negative radius makes a step
// that says "shrink" grow. In a dialog that double negative is visible on a
// slider clamped to [0, 500]; in a file a reviewer reads, `"radius": -8` under
// `"cmd": "select_shrink"` is a line that means the opposite of what it says.
std::string readRadius(const JsonValue& params, const char* commandId, float* out) {
  if (!params.hasNumber("radius"))
    return std::string("refused: ") + commandId + " needs a \"radius\" in pixels.";
  const double v = params.numberOr("radius", 0.0);
  if (!std::isfinite(v) || v <= 0.0)
    return std::string("refused: ") + commandId +
           "'s \"radius\" must be a finite number greater than zero. A radius of zero is a "
           "documented no-op, and a negative one would make this step do the opposite of what "
           "it is named.";
  *out = static_cast<float>(v);
  return {};
}

// The precondition Grow, Shrink and Feather share, and it is the same one
// `ui::selectRefineEnabled()` gives the three menu items: all three engine
// functions take a `const Selection&`, so there is no way to hand them "no
// restriction" at all.
std::string refineRadiusUnavailable(const OpenDocument& doc, const JsonValue& params,
                                    const char* commandId) {
  float radius = 0.0f;
  const std::string why = readRadius(params, commandId, &radius);
  if (!why.empty()) return why;
  if (!doc.selection.has_value())
    return std::string("refused: ") + commandId +
           " needs a selection to move the edge of, and this document has none. An absent "
           "selection means \"no restriction\" rather than \"everything\", so there is no edge.";
  return {};
}

std::string growUnavailable(const OpenDocument& doc, const JsonValue& params) {
  return refineRadiusUnavailable(doc, params, "select_grow");
}
std::string shrinkUnavailable(const OpenDocument& doc, const JsonValue& params) {
  return refineRadiusUnavailable(doc, params, "select_shrink");
}
std::string featherUnavailable(const OpenDocument& doc, const JsonValue& params) {
  return refineRadiusUnavailable(doc, params, "select_feather");
}

CommandResult doSelectGrow(OpenDocument& doc, const JsonValue& params) {
  const std::string why = refineRadiusUnavailable(doc, params, "select_grow");
  if (!why.empty()) return commandRefused(why);
  float radius = 0.0f;
  readRadius(params, "select_grow", &radius);
  installRefinedSelectionForCommand(doc, growSelection(*doc.selection, radius));
  return selectionChanged("select grow: the edge moved out by " + std::to_string(radius) + " px");
}

CommandResult doSelectShrink(OpenDocument& doc, const JsonValue& params) {
  const std::string why = refineRadiusUnavailable(doc, params, "select_shrink");
  if (!why.empty()) return commandRefused(why);
  float radius = 0.0f;
  readRadius(params, "select_shrink", &radius);
  installRefinedSelectionForCommand(doc, shrinkSelection(*doc.selection, radius));
  return selectionChanged("select shrink: the edge moved in by " + std::to_string(radius) + " px");
}

CommandResult doSelectFeather(OpenDocument& doc, const JsonValue& params) {
  const std::string why = refineRadiusUnavailable(doc, params, "select_feather");
  if (!why.empty()) return commandRefused(why);
  float radius = 0.0f;
  readRadius(params, "select_feather", &radius);
  installRefinedSelectionForCommand(doc, featherSelection(*doc.selection, radius));
  return selectionChanged("select feather: the edge was softened over " + std::to_string(radius) +
                          " px");
}

// The RGB source the two range rows sample. Same predicate as
// `ui::selectRangeEnabled()`: neither takes a `Selection` at all (PRD E9), so
// unlike the three above they need nothing already selected -- only a layer
// with pixels in it.
std::string rangeSourceUnavailable(const OpenDocument& doc, const char* commandId) {
  const Layer* target = activeLayerOf(doc);
  if (target == nullptr || !target->rgbTiles.has_value())
    return std::string("refused: ") + commandId +
           " samples the active layer's pixels, and this document's active layer has no RGB "
           "channel to sample.";
  return {};
}

// The swatch, carried by value. Three display-encoded sRGB numbers -- what
// `ImGui::ColorEdit3` holds and what `applySelectColourRangeAction()` takes --
// and **required**, because every candidate default is somebody's session
// state rather than the engine's: the dialog's own 0.5 grey, or `AppState`'s
// foreground. §4 is about exactly this key.
std::string readColour(const JsonValue& params, std::array<float, 3>* out) {
  const JsonValue* c = params.find("colour");
  if (c == nullptr)
    return "refused: select_colour_range needs a \"colour\": three display-encoded sRGB numbers. "
           "It is required rather than defaulted because the only available defaults -- the "
           "dialog's swatch, or the foreground colour -- are session state, and a step that "
           "reached for one would select a different band on a different day and report success "
           "both times.";
  if (!c->isArray() || c->size() != 3)
    return "refused: select_colour_range's \"colour\" must be an array of three numbers "
           "(display-encoded sRGB).";
  for (size_t i = 0; i < 3; ++i) {
    if (!c->at(i).isNumber())
      return "refused: select_colour_range's \"colour\" must be an array of three numbers "
             "(display-encoded sRGB).";
    (*out)[i] = static_cast<float>(c->at(i).asNumber());
  }
  return {};
}

std::string colourRangeUnavailable(const OpenDocument& doc, const JsonValue& params) {
  std::array<float, 3> swatch{};
  const std::string why = readColour(params, &swatch);
  if (!why.empty()) return why;
  return rangeSourceUnavailable(doc, "select_colour_range");
}

CommandResult doSelectColourRange(OpenDocument& doc, const JsonValue& params) {
  const std::string why = colourRangeUnavailable(doc, params);
  if (!why.empty()) return commandRefused(why);
  std::array<float, 3> swatch{};
  readColour(params, &swatch);

  SelectionRangeParams range;
  // `tolerance` and `edge_band` DO default, and to the engine struct's own
  // numbers rather than to a dialog's -- the same rule `opFromJson()` follows
  // ("every absent field falls back to the params struct's own default"). They
  // are `ops/FloodFill`'s constants, so an action that names neither behaves
  // exactly as the magic wand does, which is what a reader of the file would
  // assume.
  range.tolerance = floatOr(params, "tolerance", range.tolerance);
  // Clamped here as well as inside the engine, for the reason
  // `applySelectColourRangeAction()` gives: a caller inspecting the params it
  // is about to pass should see the value that will actually be used.
  range.edgeBand = std::min(floatOr(params, "edge_band", range.edgeBand), range.tolerance);

  // sRGB -> STRAIGHT LINEAR. `selectColourRange()` names that convention on
  // its own parameter; skipping the decode selects a band roughly twice as
  // dark as the colour in the file, which reads as a colour-management bug
  // rather than a missing conversion.
  const std::array<float, 4> linear = {srgbDecode(swatch[0]), srgbDecode(swatch[1]),
                                       srgbDecode(swatch[2]), 1.0f};
  const Layer* target = activeLayerOf(doc);
  installRefinedSelectionForCommand(
      doc, selectColourRange(*target->rgbTiles, linear, doc.document.width, doc.document.height,
                             range));
  return selectionChanged("select colour range: done");
}

std::string luminanceRangeUnavailable(const OpenDocument& doc, const JsonValue& params) {
  if (!params.hasNumber("low") || !params.hasNumber("high"))
    return "refused: select_luminance_range needs a \"low\" and a \"high\" -- the band, in "
           "display-encoded luminance. Defaulting them to the struct's 0..1 would select very "
           "nearly everything while looking like a deliberate band.";
  return rangeSourceUnavailable(doc, "select_luminance_range");
}

CommandResult doSelectLuminanceRange(OpenDocument& doc, const JsonValue& params) {
  const std::string why = luminanceRangeUnavailable(doc, params);
  if (!why.empty()) return commandRefused(why);

  SelectionLuminanceRange band;
  band.low = floatOr(params, "low", band.low);
  band.high = floatOr(params, "high", band.high);
  band.edgeBand = floatOr(params, "edge_band", band.edgeBand);

  const Layer* target = activeLayerOf(doc);
  installRefinedSelectionForCommand(
      doc, selectLuminanceRange(*target->rgbTiles, doc.document.width, doc.document.height, band));
  CommandResult r = selectionChanged("select luminance range: done");
  // core/SelectionRefine.hpp: "low > high selects nothing (an empty band is
  // empty, not inverted)". The dialog says so in yellow beside the sliders; in
  // an action there is nobody to say it to, so it is a warning on the result
  // rather than a step that quietly selected nothing.
  if (band.low > band.high)
    r.warnings.push_back(
        "select_luminance_range's \"low\" is above its \"high\", so this selected nothing rather "
        "than everything outside the band.");
  return r;
}

}  // namespace

void registerOpStackCommands(std::vector<CommandSpec>* out) {
  out->push_back(
      {"add_layer_op", "Add Layer Op", {"layer", "op"}, layerTargetUnavailable, doAddLayerOp});
  out->push_back({"remove_layer_op", "Remove Layer Op", {"layer", "index"}, layerTargetUnavailable,
                  doRemoveLayerOp});
  out->push_back({"move_layer_op", "Move Layer Op", {"layer", "from", "to"}, layerTargetUnavailable,
                  doMoveLayerOp});
  out->push_back({"set_layer_op", "Set Layer Op", {"layer", "index", "op"}, layerTargetUnavailable,
                  doSetLayerOp});
  out->push_back({"set_layer_op_enabled", "Enable/Disable Layer Op", {"layer", "index", "enabled"},
                  layerTargetUnavailable, doSetLayerOpEnabled});

  // The selection rows take no `"layer"`, so their preconditions are about the
  // selection and the document, never about a target.
  out->push_back({"select_all", "Select All", {}, documentAlwaysAvailable, doSelectAll});
  out->push_back({"deselect", "Deselect", {}, documentAlwaysAvailable, doDeselect});
  out->push_back({"invert_selection", "Invert Selection", {}, invertSelectionUnavailable,
                  doInvertSelection});
  out->push_back({"save_selection_as_channel", "Save Selection as Channel", {"channel"},
                  saveSelectionUnavailable, doSaveSelectionAsChannel});
  out->push_back({"load_channel_as_selection", "Load Channel as Selection", {"channel"},
                  loadChannelUnavailable, doLoadChannelAsSelection});

  // PRD E4/E8/E9's five refines (§4). Menu order, which is the order the
  // ACTIONS panel lists them in.
  out->push_back({"select_grow", "Grow Selection", {"radius"}, growUnavailable, doSelectGrow});
  out->push_back(
      {"select_shrink", "Shrink Selection", {"radius"}, shrinkUnavailable, doSelectShrink});
  out->push_back(
      {"select_feather", "Feather Selection", {"radius"}, featherUnavailable, doSelectFeather});
  out->push_back({"select_colour_range",
                  "Colour Range",
                  {"colour", "tolerance", "edge_band"},
                  colourRangeUnavailable,
                  doSelectColourRange});
  out->push_back({"select_luminance_range",
                  "Luminance Range",
                  {"low", "high", "edge_band"},
                  luminanceRangeUnavailable,
                  doSelectLuminanceRange});
}

}  // namespace np
