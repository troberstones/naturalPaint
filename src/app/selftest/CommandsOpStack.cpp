#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
#include "color/Space.hpp"
#include "core/Channels.hpp"
#include "core/LayerOps.hpp"
#include "core/OpStack.hpp"
#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"
#include "core/SelectionRefine.hpp"
#include "ops/Feather.hpp"
#include "ops/PointOps.hpp"
#include "ops/ToneOps.hpp"
// Section H only, and for one purpose. The five refine rows call the engine
// directly (app/CommandsOpStack.cpp §4 -- app/ must not include ui/), so until
// step 2 deletes the UI half there are two decoders of the same dialog values.
// This is the one translation unit that can see both, so it is where they are
// held to the same answer rather than to a comment.
#include "ui/MacPaintUI.hpp"

namespace np {

// --- the two functions under test that no header declares yet -------------
//
// `opToJson()` / `opFromJson()` are defined with external linkage in
// app/CommandsOpStack.cpp and are declared here rather than in a header, on
// purpose and temporarily.
//
// They belong to `io/ActionFile` (docs/automation-plan.md step 4), which does
// not exist yet. Putting them in app/Command.hpp meanwhile would put them in
// the one header six parallel branches are all appending registration
// declarations to this week -- app/Command.hpp says in as many words that the
// families were split into separate translation units because "one shared
// table literal would be one shared conflict", and adding two unrelated
// declarations to the shared header would walk straight back into it. So the
// declaration lives at the one other site that needs it until step 4 gives it
// a home.
JsonValue opToJson(const Op& op, std::string* errorOut);
bool opFromJson(const JsonValue& value, Op* out, std::string* errorOut);
const char* opKindId(PointOpKind kind) noexcept;

namespace {

constexpr int kPointOpKindCount = static_cast<int>(PointOpKind::Threshold) + 1;

// **Bit patterns, never `==`.** Two floats one ULP apart compare unequal here
// and would compare equal under any tolerance a reviewer would think
// reasonable -- and one ULP is exactly what a writer that printed `%.6g`, or a
// reader that went through a `float` where a `double` was needed, would lose.
// `==` would also call `-0.0f` and `0.0f` the same number, which they are not
// in a file that claims to round-trip exactly.
bool sameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

bool sameBits(const std::array<float, 3>& a, const std::array<float, 3>& b) {
  for (size_t i = 0; i < 3; ++i)
    if (!sameBits(a[i], b[i])) return false;
  return true;
}

// Field-for-field, per kind. Deliberately NOT one `memcmp` over the whole
// `Op`: it holds three `std::vector<CurvePoint>`s (whose bytes are pointers)
// and has padding between members, so a struct-wide compare would be reading
// indeterminate bytes and comparing heap addresses -- it would fail for two
// Ops that are genuinely identical, and could pass over a curve whose points
// differ.
//
// Only the fields the kind selects are compared, which is core/OpStack.hpp's
// own contract: "every field not selected by `pointKind` sits at its
// default-constructed value and is never read by anything in this module". A
// comparison that insisted on the unread fields too would be asserting
// something the format does not promise.
bool opsIdenticalBits(const Op& a, const Op& b) {
  if (a.opClass != b.opClass) return false;
  if (a.enabled != b.enabled) return false;
  if (a.opClass != OpClass::PointA) return true;
  if (a.pointKind != b.pointKind) return false;
  switch (a.pointKind) {
    case PointOpKind::Levels:
      for (size_t c = 0; c < 3; ++c) {
        const LevelsParams& x = a.levels[c];
        const LevelsParams& y = b.levels[c];
        if (!sameBits(x.blackIn, y.blackIn) || !sameBits(x.whiteIn, y.whiteIn) ||
            !sameBits(x.gamma, y.gamma) || !sameBits(x.blackOut, y.blackOut) ||
            !sameBits(x.whiteOut, y.whiteOut))
          return false;
      }
      return true;
    case PointOpKind::Curves:
      for (size_t c = 0; c < 3; ++c) {
        if (a.curves[c].size() != b.curves[c].size()) return false;
        for (size_t i = 0; i < a.curves[c].size(); ++i) {
          if (!sameBits(a.curves[c][i].x, b.curves[c][i].x) ||
              !sameBits(a.curves[c][i].y, b.curves[c][i].y))
            return false;
        }
      }
      return true;
    case PointOpKind::Exposure:
      return sameBits(a.exposure.stops, b.exposure.stops);
    case PointOpKind::Saturation:
      return sameBits(a.saturation.scale, b.saturation.scale) &&
             sameBits(a.saturation.lumaWeights, b.saturation.lumaWeights);
    case PointOpKind::Grayscale:
      return sameBits(a.grayscale.lumaWeights, b.grayscale.lumaWeights);
    case PointOpKind::ChannelMixer:
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c)
          if (!sameBits(a.channelMixer.matrix[r][c], b.channelMixer.matrix[r][c])) return false;
      return true;
    case PointOpKind::Invert:
      return a.invert.domain == b.invert.domain && sameBits(a.invert.amount, b.invert.amount);
    case PointOpKind::Posterize:
      return a.posterize.levels == b.posterize.levels;
    case PointOpKind::Threshold:
      return sameBits(a.threshold.threshold, b.threshold.threshold) &&
             sameBits(a.threshold.amount, b.threshold.amount);
  }
  return false;
}

// An op of `kind` with every one of its fields set to a value that is NOT the
// struct's default, and no two fields sharing a value.
//
// Both halves matter and both have caught this shape of test being green for
// the wrong reason elsewhere in this suite:
//
//  * **Not the default**: a reader that drops a field entirely leaves it at
//    the default, so a fixture built from defaults round-trips perfectly
//    through a reader that reads nothing at all.
//  * **No two equal**: a reader that transposes two fields (writes `blackOut`
//    where `whiteOut` belongs) is invisible if both hold the same number.
//
// Several values are `std::nextafterf()` of their neighbour -- adjacent
// floats, one ULP apart. They are indistinguishable to any writer that prints
// fewer than nine significant digits and to any comparison with a tolerance,
// which is precisely why they are here: io/Json promises the shortest decimal
// that reads back to the *identical* double, and this is what tests that
// promise rather than a weaker one.
Op makeDistinctOp(PointOpKind kind) {
  Op op;
  op.opClass = OpClass::PointA;
  op.pointKind = kind;
  // Not the default (`true`), so a reader that ignores "enabled" is caught by
  // every kind rather than by none.
  op.enabled = false;
  switch (kind) {
    case PointOpKind::Levels:
      for (size_t c = 0; c < 3; ++c) {
        const float base = 0.01f + static_cast<float>(c) * 0.11f;
        op.levels[c].blackIn = base;
        op.levels[c].whiteIn = std::nextafterf(base, 10.0f);
        op.levels[c].gamma = base + 1.37f;
        op.levels[c].blackOut = base + 2.71f;
        op.levels[c].whiteOut = std::nextafterf(base + 2.71f, 10.0f);
      }
      break;
    case PointOpKind::Curves:
      // Different point counts per channel, and an empty one: a reader that
      // read channel 0's count and reused it for all three would pass with
      // three equal-length curves.
      op.curves[0] = {{0.0f, 0.125f}, {0.5f, std::nextafterf(0.5f, 1.0f)}, {1.0f, 0.9375f}};
      op.curves[1] = {{0.25f, 0.3125f}, {0.75f, 0.8125f}};
      op.curves[2] = {};
      break;
    case PointOpKind::Exposure:
      op.exposure.stops = -1.3125f;
      break;
    case PointOpKind::Saturation:
      op.saturation.scale = 1.6875f;
      op.saturation.lumaWeights = {0.3125f, std::nextafterf(0.3125f, 1.0f), 0.375f};
      break;
    case PointOpKind::Grayscale:
      op.grayscale.lumaWeights = {0.5f, 0.25f, std::nextafterf(0.25f, 1.0f)};
      break;
    case PointOpKind::ChannelMixer:
      for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c)
          op.channelMixer.matrix[r][c] = 0.0625f * static_cast<float>(r * 4 + c + 1);
      break;
    case PointOpKind::Invert:
      // Display rather than the default Linear, and an amount that is not the
      // default 1 (ops/ToneOps.hpp: a default-constructed InvertParams is a
      // FULL invert, not an identity).
      op.invert.domain = InvertParams::Domain::Display;
      op.invert.amount = 0.6875f;
      break;
    case PointOpKind::Posterize:
      op.posterize.levels = 7;
      break;
    case PointOpKind::Threshold:
      op.threshold.threshold = 0.40625f;
      op.threshold.amount = std::nextafterf(0.40625f, 1.0f);
      break;
  }
  return op;
}

// A one-layer RGB document with a name the action addresses by.
OpenDocument makeOpStackDocument() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "opstack");
  od.document.layers[0].name = "Base";
  Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
  for (int32_t y = 0; y < kTileSize; ++y)
    for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, {0.25f, 0.5f, 0.75f, 1.0f});
  od.recordEdit("op-stack fixture", EditKind::Content);
  return od;
}

// Two selections agree, texel for texel, over the whole canvas.
//
// **Every texel, not a handful of probes.** Section H uses this to hold the
// command rows and ui/MacPaintUI.cpp's dialog boundary to the same answer, and
// the drift it is guarding against -- a missing sRGB decode, an edge band
// clamped in one place and not the other -- shows up as a soft edge a few
// texels wide. Three sample points would miss exactly that and report the two
// identical. `std::nullopt` on either side means "no restriction", which
// `selectionCoverageAt()` already reports as 1.0 everywhere, so an absent
// selection and a full one compare equal here on purpose: they are the same
// restriction.
bool sameCoverage(const std::optional<Selection>& a, const std::optional<Selection>& b,
                  int32_t width, int32_t height) {
  const Selection* lhs = a.has_value() ? &*a : nullptr;
  const Selection* rhs = b.has_value() ? &*b : nullptr;
  for (int32_t y = 0; y < height; ++y)
    for (int32_t x = 0; x < width; ++x)
      if (selectionCoverageAt(lhs, PixelCoord{x, y}) != selectionCoverageAt(rhs, PixelCoord{x, y}))
        return false;
  return true;
}

JsonValue opParams(const JsonValue& op) {
  JsonValue params = JsonValue::object();
  params.set("layer", JsonValue::string("Base"));
  params.set("op", op);
  return params;
}

}  // namespace

// app/CommandsOpStack (docs/automation-plan.md step 1) -- the command rows
// that carry an *op* as a parameter, and the selection rows that make every
// destructive step around them mean what it meant when it was recorded.
//
// **What this section proves, and why none of it is covered elsewhere.**
//
//  * **All nine PointOpKinds survive command -> JSON text -> command ->
//    applied, bit for bit.** Not "to within a tolerance": every float is
//    compared with `memcmp`, and several of the fixture's fields are adjacent
//    floats one ULP apart, so a writer that printed six significant digits or
//    a reader that went through a `float` where a `double` was needed fails
//    here and passes any friendlier comparison. The sweep is driven off
//    `PointOpKind`'s own count, so a tenth kind is asserted the day it is
//    added rather than the day someone remembers to list it.
//  * **The fixture is hostile on purpose**: no field is left at its default
//    (a reader that drops a field entirely would round-trip a default-built
//    fixture perfectly) and no two fields of one op share a value (a reader
//    that transposes two fields is invisible when both hold the same number).
//  * **A hand-typed fixture**, written at a keyboard rather than produced by
//    `opToJson()`, decodes to the op it names -- io/OpSerial's own discipline,
//    and the only thing that separates "the reader agrees with the writer"
//    from "the reader is right".
//  * **An op kind nothing knows refuses, naming it**, and **a non-PointA op
//    refuses rather than being stored inert**. This is the one place the
//    document rule and the action rule are deliberately opposite:
//    `OpClass::Unknown` exists so a document written by a newer build
//    round-trips through this one (PRD I10), and an action that did the same
//    would replay a grade with one op missing and write files that look
//    correct. Both directions are asserted -- the reader refuses, and nothing
//    reaches the stack.
//  * **`load_channel_as_selection` on a document with no such channel refuses,
//    naming the channel**, and **save then load round-trips a selection**.
//    docs/automation-plan.md §7's third trap: a selection is session state and
//    is never in a document file, so an action that cannot name a saved
//    channel silently applies every destructive step that follows to the whole
//    canvas.
//
// Headless, GPU-free and filesystem-free.
bool runCommandsOpStackTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("  -- A. the rows are registered --\n");
  {
    bool allPresent = true;
    for (const char* id : {"add_layer_op", "remove_layer_op", "move_layer_op", "set_layer_op",
                           "set_layer_op_enabled", "select_all", "deselect", "invert_selection",
                           "save_selection_as_channel", "load_channel_as_selection"}) {
      if (findCommand(id) == nullptr) allPresent = false;
    }
    check(allPresent, "table: all ten op-stack and selection rows are registered");

    // The ids are what a `.npaction` file holds, so a kind id that collided
    // with a command id, or one that was not lower_snake_case, would be a
    // format defect rather than a style one.
    bool idsWellFormed = true;
    for (int k = 0; k < kPointOpKindCount; ++k) {
      const std::string id = opKindId(static_cast<PointOpKind>(k));
      if (id.empty()) idsWellFormed = false;
      for (char c : id)
        if (!((c >= 'a' && c <= 'z') || c == '_')) idsWellFormed = false;
    }
    check(idsWellFormed, "kinds: every kind id is a non-empty lower_snake_case name");

    // The one that would otherwise be found by a file opening wrong: the ids
    // must be distinct, or two kinds decode to whichever the loop reaches
    // first.
    bool idsDistinct = true;
    for (int a = 0; a < kPointOpKindCount; ++a)
      for (int b = a + 1; b < kPointOpKindCount; ++b)
        if (std::string(opKindId(static_cast<PointOpKind>(a))) ==
            opKindId(static_cast<PointOpKind>(b)))
          idsDistinct = false;
    check(idsDistinct, "kinds: no two kinds share an id");

    // **Not the panel's label.** `pointOpKindName()` is UI copy and is free to
    // be reworded; keying the format by it is what docs/automation-plan.md §5
    // forbids. Asserting they differ for at least one kind is what stops a
    // later "simplification" from replacing one with the other.
    check(std::string(opKindId(PointOpKind::ChannelMixer)) == "channel_mixer" &&
              std::string(pointOpKindName(PointOpKind::ChannelMixer)) == "Channel Mixer",
          "kinds: the file id is not the panel label");
  }

  std::printf("  -- B. all nine kinds round-trip bit for bit --\n");
  {
    bool everyKindRoundTrips = true;
    bool everyKindReachedTheStack = true;
    std::string firstFailingKind;
    for (int k = 0; k < kPointOpKindCount; ++k) {
      const PointOpKind kind = static_cast<PointOpKind>(k);
      const Op original = makeDistinctOp(kind);

      // command -> JSON
      std::string encodeError;
      const JsonValue encoded = opToJson(original, &encodeError);
      if (!encoded.isObject()) {
        everyKindRoundTrips = false;
        if (firstFailingKind.empty()) firstFailingKind = opKindId(kind);
        continue;
      }

      // -> text -> JSON again. Through the real serialised form, not through
      // the in-memory JsonValue: an action is a *file*, and a JsonValue
      // handed straight back would never exercise the number writer that this
      // whole bit-exactness claim rests on.
      JsonValue params = opParams(encoded);
      const std::string text = params.write();
      JsonValue reparsed;
      if (!parseJson(text, "action", &reparsed)) {
        everyKindRoundTrips = false;
        if (firstFailingKind.empty()) firstFailingKind = opKindId(kind);
        continue;
      }

      // -> command -> applied
      OpenDocument od = makeOpStackDocument();
      const CommandResult added = applyCommand(od, Command{"add_layer_op", reparsed});
      if (!added.ok || od.document.layers[0].ops.size() != 1) {
        everyKindReachedTheStack = false;
        if (firstFailingKind.empty()) firstFailingKind = opKindId(kind);
        continue;
      }
      if (!opsIdenticalBits(od.document.layers[0].ops.at(0), original)) {
        everyKindRoundTrips = false;
        if (firstFailingKind.empty()) firstFailingKind = opKindId(kind);
      }
    }
    check(everyKindReachedTheStack,
          "round trip: every kind's step applies and lands one op on the stack");
    check(everyKindRoundTrips && firstFailingKind.empty(),
          "round trip: all nine kinds survive command->text->command bit for bit");
    check(kPointOpKindCount == 9, "round trip: the sweep really covered nine kinds");

    // The comparator itself, proven able to fail. A round-trip assertion whose
    // comparator always returns true is green forever, which is exactly the
    // shape of test this suite has been bitten by before.
    Op a = makeDistinctOp(PointOpKind::Exposure);
    Op b = a;
    b.exposure.stops = std::nextafterf(a.exposure.stops, 10.0f);
    check(!opsIdenticalBits(a, b),
          "round trip: the comparator rejects a one-ULP difference");
    Op c = a;
    c.exposure.stops = -0.0f;
    Op d = a;
    d.exposure.stops = 0.0f;
    check(!opsIdenticalBits(c, d), "round trip: the comparator separates -0.0 from 0.0");
  }

  std::printf("  -- C. a hand-typed op decodes to what it says --\n");
  {
    // Typed at a keyboard, not produced by `opToJson()` -- io/OpSerial's own
    // fixture discipline. A round-trip test alone cannot tell a correct
    // encoder/decoder pair from two that agree on the same mistake.
    const char* kHandTyped =
        "{ \"layer\": \"Base\","
        "  \"op\": { \"kind\": \"saturation\", \"enabled\": false,"
        "            \"scale\": 1.5,"
        "            \"lumaWeights\": [0.25, 0.5, 0.25] } }";
    JsonValue params;
    const bool parsed = parseJson(kHandTyped, "hand-typed action", &params);
    OpenDocument od = makeOpStackDocument();
    const CommandResult added =
        parsed ? applyCommand(od, Command{"add_layer_op", params}) : CommandResult{};
    const bool landed = added.ok && od.document.layers[0].ops.size() == 1;
    // `OpStack::at()` throws on an empty stack, so the failing path reads a
    // default `Op` rather than taking the suite down with it: a section that
    // crashes reports nothing at all, where one that fails names the defect.
    const Op absent;
    const Op& got = landed ? od.document.layers[0].ops.at(0) : absent;
    check(parsed && landed && got.opClass == OpClass::PointA &&
              got.pointKind == PointOpKind::Saturation && !got.enabled &&
              sameBits(got.saturation.scale, 1.5f) &&
              sameBits(got.saturation.lumaWeights, std::array<float, 3>{0.25f, 0.5f, 0.25f}),
          "hand-typed: a keyboard-written op decodes field for field");

    // An omitted field keeps the params struct's own default rather than a
    // second default invented here -- ops/ToneOps.hpp is explicit that a
    // default InvertParams is a FULL invert, and an action that said nothing
    // about `amount` must mean that and not an identity.
    JsonValue bare;
    const bool bareParsed = parseJson(
        "{ \"layer\": \"Base\", \"op\": { \"kind\": \"invert\" } }", "bare", &bare);
    OpenDocument od2 = makeOpStackDocument();
    const CommandResult bareAdded =
        bareParsed ? applyCommand(od2, Command{"add_layer_op", bare}) : CommandResult{};
    check(bareParsed && bareAdded.ok && od2.document.layers[0].ops.size() == 1 &&
              od2.document.layers[0].ops.at(0).enabled &&
              sameBits(od2.document.layers[0].ops.at(0).invert.amount, InvertParams{}.amount) &&
              od2.document.layers[0].ops.at(0).invert.domain == InvertParams{}.domain,
          "hand-typed: an omitted field keeps the params struct's own default");
  }

  std::printf("  -- D. what this build cannot evaluate REFUSES --\n");
  {
    OpenDocument od = makeOpStackDocument();

    JsonValue unknownKind = JsonValue::object();
    unknownKind.set("kind", JsonValue::string("chromatic_aberration"));
    const CommandResult refusedKind =
        applyCommand(od, Command{"add_layer_op", opParams(unknownKind)});
    check(!refusedKind.ok && contains(refusedKind.status, "chromatic_aberration"),
          "refusal: an op kind nothing knows refuses, naming it");
    check(od.document.layers[0].ops.size() == 0,
          "refusal: an unknown kind stores nothing at all");

    // The class case, which is the one the plan singles out. A newer build's
    // class-B blur is not a point op and must not be treated as one; nor may
    // it be kept as an inert entry the way a *document* keeps it, because an
    // action is executed and a stack with a step missing composites happily
    // and writes files that look correct.
    JsonValue spatial = JsonValue::object();
    spatial.set("class", JsonValue::string("spatial_b"));
    spatial.set("kind", JsonValue::string("exposure"));
    spatial.set("stops", JsonValue::number(1.0));
    const CommandResult refusedClass =
        applyCommand(od, Command{"add_layer_op", opParams(spatial)});
    check(!refusedClass.ok && contains(refusedClass.status, "spatial_b"),
          "refusal: a non-PointA op refuses, naming its class");
    check(od.document.layers[0].ops.size() == 0,
          "refusal: a non-PointA op is not stored as an inert entry");

    // And the write direction: PRD P6's converter turns a document's existing
    // op stack into an action, and a stack holding an entry this build could
    // not read must refuse rather than quietly produce an action one op short.
    Op unknown;
    unknown.opClass = OpClass::Unknown;
    unknown.unrecognised = {0x01, 0x02, 0x03};
    std::string encodeError;
    const JsonValue encoded = opToJson(unknown, &encodeError);
    check(encoded.isNull() && contains(encodeError, "unrecognised op"),
          "refusal: an unrecognised op refuses to be written into an action");

    // The `Op` a refusal leaves behind must be untouched, not half-filled --
    // otherwise a caller that ignored the `false` would grade with garbage.
    Op sentinel = makeDistinctOp(PointOpKind::Posterize);
    const Op before = sentinel;
    JsonValue bad = JsonValue::object();
    bad.set("kind", JsonValue::string("nope"));
    std::string why;
    const bool decoded = opFromJson(bad, &sentinel, &why);
    check(!decoded && opsIdenticalBits(sentinel, before),
          "refusal: a refused decode leaves the caller's Op untouched");

    JsonValue noOp = JsonValue::object();
    noOp.set("layer", JsonValue::string("Base"));
    const CommandResult missing = applyCommand(od, Command{"add_layer_op", noOp});
    check(!missing.ok && contains(missing.status, "\"op\""),
          "refusal: a step with no op at all refuses, naming the key");
  }

  std::printf("  -- E. the four stack mutators --\n");
  {
    OpenDocument od = makeOpStackDocument();
    for (const PointOpKind kind :
         {PointOpKind::Exposure, PointOpKind::Grayscale, PointOpKind::Posterize}) {
      std::string err;
      applyCommand(od, Command{"add_layer_op", opParams(opToJson(makeDistinctOp(kind), &err))});
    }
    check(od.document.layers[0].ops.size() == 3, "mutators: three adds append three ops, in order");
    check(od.document.layers[0].ops.at(0).pointKind == PointOpKind::Exposure &&
              od.document.layers[0].ops.at(2).pointKind == PointOpKind::Posterize,
          "mutators: add appends to the top of the stack");

    JsonValue move = JsonValue::object();
    move.set("layer", JsonValue::string("Base"));
    move.set("from", JsonValue::number(0));
    move.set("to", JsonValue::number(2));
    check(applyCommand(od, Command{"move_layer_op", move}).ok &&
              od.document.layers[0].ops.at(2).pointKind == PointOpKind::Exposure,
          "mutators: move_layer_op moves the named op to the given index");

    JsonValue enable = JsonValue::object();
    enable.set("layer", JsonValue::string("Base"));
    enable.set("index", JsonValue::number(1));
    enable.set("enabled", JsonValue::boolean(true));
    check(applyCommand(od, Command{"set_layer_op_enabled", enable}).ok &&
              od.document.layers[0].ops.at(1).enabled,
          "mutators: set_layer_op_enabled sets the flag it is given");

    // Omitting `enabled` must refuse rather than default: an action that meant
    // to disable an op would otherwise silently enable it.
    JsonValue noFlag = JsonValue::object();
    noFlag.set("layer", JsonValue::string("Base"));
    noFlag.set("index", JsonValue::number(1));
    const CommandResult noFlagResult = applyCommand(od, Command{"set_layer_op_enabled", noFlag});
    check(!noFlagResult.ok && contains(noFlagResult.status, "enabled"),
          "mutators: set_layer_op_enabled refuses rather than defaulting the flag");

    std::string err;
    JsonValue replace = opParams(opToJson(makeDistinctOp(PointOpKind::Threshold), &err));
    replace.set("index", JsonValue::number(1));
    check(applyCommand(od, Command{"set_layer_op", replace}).ok &&
              od.document.layers[0].ops.at(1).pointKind == PointOpKind::Threshold &&
              od.document.layers[0].ops.size() == 3,
          "mutators: set_layer_op replaces in place rather than appending");

    JsonValue remove = JsonValue::object();
    remove.set("layer", JsonValue::string("Base"));
    remove.set("index", JsonValue::number(0));
    check(applyCommand(od, Command{"remove_layer_op", remove}).ok &&
              od.document.layers[0].ops.size() == 2,
          "mutators: remove_layer_op removes one entry");

    // Out of range is core/LayerOps' own guard -- a sentence rather than the
    // `std::out_of_range` `core::OpStack` would throw -- and it must reach the
    // command result rather than escaping into a replay loop.
    JsonValue far = JsonValue::object();
    far.set("layer", JsonValue::string("Base"));
    far.set("index", JsonValue::number(99));
    const CommandResult outOfRange = applyCommand(od, Command{"remove_layer_op", far});
    check(!outOfRange.ok && !outOfRange.status.empty(),
          "mutators: an out-of-range op index refuses with a sentence");

    JsonValue negative = JsonValue::object();
    negative.set("layer", JsonValue::string("Base"));
    negative.set("index", JsonValue::number(-1));
    const CommandResult negativeResult = applyCommand(od, Command{"remove_layer_op", negative});
    check(!negativeResult.ok && contains(negativeResult.status, "whole op index"),
          "mutators: a negative op index refuses instead of wrapping to an enormous one");

    // Addressed by NAME: an action recorded on one document is replayed on
    // another whose layers differ in order, and an index would silently grade
    // a different layer.
    JsonValue elsewhere = JsonValue::object();
    elsewhere.set("layer", JsonValue::string("Detail"));
    elsewhere.set("index", JsonValue::number(0));
    const CommandResult noLayer = applyCommand(od, Command{"remove_layer_op", elsewhere});
    check(!noLayer.ok && contains(noLayer.status, "Detail"),
          "mutators: a layer this document lacks refuses, naming it");
  }

  std::printf("  -- F. the selection rows --\n");
  {
    OpenDocument od = makeOpStackDocument();
    check(!od.selection.has_value(), "selection: a fresh document has no selection");

    const uint64_t revisionBefore = od.selectionRevision;
    check(applyCommand(od, Command{"select_all", JsonValue::object()}).ok &&
              od.selection.has_value() && od.selectionRevision > revisionBefore,
          "selection: select_all engages a selection and bumps the revision");
    check(selectionCoverageAt(&*od.selection, PixelCoord{5, 5}) == 1.0f,
          "selection: select_all really covers the canvas");

    check(applyCommand(od, Command{"deselect", JsonValue::object()}).ok &&
              !od.selection.has_value(),
          "selection: deselect leaves absence, not an empty selection");
    check(od.lastDeselected.has_value(),
          "selection: deselect feeds Reselect, exactly as the UI route does");

    // §7 turned into a refusal: the UI silently ignores Invert with nothing
    // selected, and a batch cannot afford that silence.
    const CommandResult noSelection = applyCommand(od, Command{"invert_selection", JsonValue::object()});
    check(!noSelection.ok && contains(noSelection.status, "no restriction"),
          "selection: invert_selection with nothing selected refuses rather than no-ops");

    // A real region, so that "the whole canvas" and "this rectangle" cannot be
    // confused by a test that only ever selected everything.
    od.selection = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);
    ++od.selectionRevision;
    check(applyCommand(od, Command{"invert_selection", JsonValue::object()}).ok &&
              selectionCoverageAt(&*od.selection, PixelCoord{16, 16}) == 0.0f &&
              selectionCoverageAt(&*od.selection, PixelCoord{40, 40}) == 1.0f,
          "selection: invert_selection swaps inside for outside");
  }

  std::printf("  -- G. the session/document line: save and load a channel --\n");
  {
    OpenDocument od = makeOpStackDocument();
    od.selection = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);

    // The refusal that matters most, because its absence is silent: a step
    // naming a channel the document lacks would otherwise leave the selection
    // alone and let every destructive step that followed apply to the whole
    // canvas.
    JsonValue missing = JsonValue::object();
    missing.set("channel", JsonValue::string("Mask"));
    const CommandResult noChannel = applyCommand(od, Command{"load_channel_as_selection", missing});
    check(!noChannel.ok && contains(noChannel.status, "Mask"),
          "channels: load_channel_as_selection refuses a channel this document lacks, by name");
    check(od.selection.has_value() &&
              selectionCoverageAt(&*od.selection, PixelCoord{16, 16}) == 1.0f,
          "channels: the refusal changed nothing -- the live selection is untouched");

    const CommandResult noName =
        applyCommand(od, Command{"load_channel_as_selection", JsonValue::object()});
    check(!noName.ok && contains(noName.status, "channel"),
          "channels: a step with no channel name refuses, naming the key");

    // Save, throw the live selection away, load it back.
    JsonValue save = JsonValue::object();
    save.set("channel", JsonValue::string("Mask"));
    const CommandResult saved = applyCommand(od, Command{"save_selection_as_channel", save});
    check(saved.ok && saved.warnings.empty() && od.document.channels.size() == 1 &&
              od.document.channels[0].name == "Mask",
          "channels: save_selection_as_channel writes a named channel into the document");

    applyCommand(od, Command{"deselect", JsonValue::object()});
    check(!od.selection.has_value(), "channels: the live selection really was thrown away");

    const CommandResult loaded = applyCommand(od, Command{"load_channel_as_selection", missing});
    bool sameEverywhere = loaded.ok && od.selection.has_value();
    if (sameEverywhere) {
      const Selection expected = selectRectangle(8.0f, 8.0f, 24.0f, 24.0f);
      for (int32_t y = 0; y < 64 && sameEverywhere; ++y)
        for (int32_t x = 0; x < 64; ++x) {
          if (selectionCoverageAt(&*od.selection, PixelCoord{x, y}) !=
              selectionCoverageAt(&expected, PixelCoord{x, y})) {
            sameEverywhere = false;
            break;
          }
        }
    }
    check(sameEverywhere, "channels: save then load round-trips the selection texel for texel");

    // Saving with no selection: absent means "no restriction", not
    // "everything", so there is no coverage to write and a full-coverage
    // channel would be an invention.
    applyCommand(od, Command{"deselect", JsonValue::object()});
    const CommandResult noLive = applyCommand(od, Command{"save_selection_as_channel", save});
    check(!noLive.ok && contains(noLive.status, "no restriction"),
          "channels: saving with nothing selected refuses rather than inventing coverage");

    // --- the uniquify trap, and the two answers it gets -------------------
    //
    // The ENGINE appends under a uniquified name and never replaces, and
    // core/Channels.hpp argues why: destroying a saved selection has to be a
    // delete. The COMMAND refuses instead, because an action is executed
    // repeatedly and headlessly -- a step that lands as "Mask" the first time
    // and "Mask 2" the second makes every later `load_channel_as_selection
    // "Mask"` bind to a previous run's region and report success.
    //
    // Both are asserted, because "the command refuses" alone would also pass
    // if someone had made the engine refuse too, which would be the wrong fix
    // in the other direction.
    od.selection = selectRectangle(0.0f, 0.0f, 8.0f, 8.0f);
    const CommandResult second = applyCommand(od, Command{"save_selection_as_channel", save});
    check(!second.ok && contains(second.status, "Mask"),
          "channels: a second save under a taken name is refused, naming the channel");
    check(contains(second.status, "OLDER"),
          "channels: and the refusal says what would have gone wrong, not just that it did");
    check(od.document.channels.size() == 1,
          "channels: the refusal left the document with the one channel it had");

    // The precondition is where a replayer asks, so it has to give the same
    // answer before anything has run -- that is the whole point of refusing
    // here rather than warning after.
    const CommandSpec* spec = findCommand("save_selection_as_channel");
    check(spec != nullptr && !spec->unavailableReason(od, save).empty(),
          "channels: and the PRECONDITION says so, before a destructive step has run");

    // The engine, unchanged, called directly. A command-level policy must not
    // be mistaken for a change to what `saveSelectionAsChannel()` does.
    const size_t appended = saveSelectionAsChannel(od.document, *od.selection, "Mask");
    check(od.document.channels.size() == 2 && appended == 1 &&
              od.document.channels[1].name != "Mask" && od.document.channels[0].name == "Mask",
          "channels: the engine still appends under a uniquified name rather than replacing");
  }

  std::printf("  -- H. PRD E4/E8/E9's five refines, as command rows --\n");
  {
    // These were five of app/CommandCoverage's eight "not yet registered"
    // gaps. What each has to prove is not that the engine works -- section
    // app/selftest/SelectMenu.cpp already holds the engine and the dialogs to
    // each other -- but the two things a *command* row adds: that every number
    // the dialog was holding in a static now travels in the step, and that the
    // second decoder this creates gives the identical answer to the first.
    bool allPresent = true;
    for (const char* id : {"select_grow", "select_shrink", "select_feather",
                           "select_colour_range", "select_luminance_range"})
      if (findCommand(id) == nullptr) allPresent = false;
    check(allPresent, "refine: all five rows are registered");

    const int32_t w = 64, h = 64;
    auto rectDoc = []() {
      OpenDocument od = makeOpStackDocument();
      od.selection = selectRectangle(16.0f, 16.0f, 48.0f, 48.0f);
      return od;
    };

    JsonValue r8 = JsonValue::object();
    r8.set("radius", JsonValue::number(8.0));

    // --- the parameters that used to live in a dialog static ---------------
    {
      OpenDocument od = rectDoc();
      const CommandResult noRadius =
          applyCommand(od, Command{"select_grow", JsonValue::object()});
      check(!noRadius.ok && contains(noRadius.status, "radius"),
            "refine: a missing radius refuses, and says which key");

      JsonValue zero = JsonValue::object();
      zero.set("radius", JsonValue::number(0.0));
      const CommandResult zeroRadius = applyCommand(od, Command{"select_grow", zero});
      check(!zeroRadius.ok,
            "refine: a radius of zero refuses rather than recording a documented no-op");

      // The one a slider cannot express and a file can:
      // `shrinkSelection(s, r) == growSelection(s, -r)`, so a negative radius
      // makes a step that says "shrink" grow.
      JsonValue negative = JsonValue::object();
      negative.set("radius", JsonValue::number(-8.0));
      const CommandResult grew = applyCommand(od, Command{"select_shrink", negative});
      check(!grew.ok && contains(grew.status, "opposite"),
            "refine: a negative radius refuses rather than silently doing the opposite");

      OpenDocument bare = makeOpStackDocument();
      const CommandResult nothingSelected = applyCommand(bare, Command{"select_grow", r8});
      check(!nothingSelected.ok && contains(nothingSelected.status, "no restriction"),
            "refine: grow with nothing selected refuses -- there is no edge to move");
      check(!bare.selection.has_value() && bare.refineUndoStack.empty(),
            "refine: and the refusal pushed nothing onto the refine-undo stack");
    }

    // --- the anti-drift cross-check ---------------------------------------
    //
    // The command decodes the step; ui/MacPaintUI.cpp's boundary decodes the
    // dialog. Two decoders of one set of values is the shape
    // docs/automation-plan.md §7 calls out for op encodings ("two encoders of
    // one op list will drift"), and it applies here for as long as both exist.
    {
      OpenDocument od = rectDoc();
      const Selection before = *od.selection;
      check(applyCommand(od, Command{"select_grow", r8}).ok,
            "refine: select_grow ran");
      check(sameCoverage(od.selection,
                         applySelectRefineAction(MenuAction::SelectGrow, before, 8.0f), w, h),
            "refine: select_grow agrees with the Grow dialog, texel for texel");

      OpenDocument shrunk = rectDoc();
      check(applyCommand(shrunk, Command{"select_shrink", r8}).ok &&
                sameCoverage(shrunk.selection,
                             applySelectRefineAction(MenuAction::SelectShrink, before, 8.0f), w, h),
            "refine: select_shrink agrees with the Shrink dialog");
      // The assertion that stops the three rows from being wired to one
      // engine function: a shrink is not a grow.
      check(!sameCoverage(od.selection, shrunk.selection, w, h),
            "refine: and select_shrink is not select_grow for the same radius");

      OpenDocument feathered = rectDoc();
      check(applyCommand(feathered, Command{"select_feather", r8}).ok &&
                sameCoverage(feathered.selection,
                             applySelectRefineAction(MenuAction::SelectFeather, before, 8.0f), w,
                             h),
            "refine: select_feather agrees with the Feather dialog");
    }

    // --- Undo Refine still has something to pop ----------------------------
    //
    // `MenuAction::SelectUndoRefine` is classified NotRecordable precisely
    // because `refineUndoStack` is session state -- but that is only an
    // honest answer if the *recordable* half keeps filling it. A command row
    // that installed its result without pushing would leave step 2's
    // call-site migration silently deleting a menu item's effect, which is
    // the one thing that migration promises not to do.
    {
      OpenDocument od = rectDoc();
      const Selection before = *od.selection;
      check(od.refineUndoStack.empty(), "undo refine: the stack starts empty");
      check(applyCommand(od, Command{"select_grow", r8}).ok && od.refineUndoStack.size() == 1,
            "undo refine: a refine issued as a command pushes exactly one entry");
      check(undoLastRefine(od) && sameCoverage(od.selection, before, w, h) &&
                od.refineUndoStack.empty(),
            "undo refine: and Select > Undo Refine restores exactly what it replaced");
    }

    // --- the colour travels in the step, decoded ---------------------------
    {
      OpenDocument od = makeOpStackDocument();  // filled linear {0.25, 0.5, 0.75, 1}
      const CommandResult noColour =
          applyCommand(od, Command{"select_colour_range", JsonValue::object()});
      check(!noColour.ok && contains(noColour.status, "session state"),
            "colour range: a step with no colour refuses, and says why there is no default");

      auto colourStep = [](float r, float g, float b) {
        JsonValue c = JsonValue::array();
        c.push(JsonValue::number(static_cast<double>(r)));
        c.push(JsonValue::number(static_cast<double>(g)));
        c.push(JsonValue::number(static_cast<double>(b)));
        JsonValue p = JsonValue::object();
        p.set("colour", std::move(c));
        return p;
      };

      // **The assertion the sRGB decode lives or dies by.** The layer is a
      // flat linear {0.25, 0.5, 0.75}; the step carries the DISPLAY encoding
      // of that colour, which is what the swatch showed. A row that forwarded
      // the three numbers straight to `selectColourRange()` -- which wants
      // straight linear -- would be asking for a much brighter colour and
      // would select nothing, and a row that decoded twice would select
      // nothing either.
      const JsonValue encoded =
          colourStep(srgbEncode(0.25f), srgbEncode(0.5f), srgbEncode(0.75f));
      check(applyCommand(od, Command{"select_colour_range", encoded}).ok &&
                od.selection.has_value() &&
                selectionCoverageAt(&*od.selection, PixelCoord{32, 32}) == 1.0f,
            "colour range: the step's sRGB colour is decoded, and finds the layer's texels");

      // The same three numbers read as if they were already linear: a
      // different colour, far enough away that the default tolerance does not
      // reach it. This is what makes the assertion above about the decode
      // rather than about the tolerance being generous.
      OpenDocument raw = makeOpStackDocument();
      const CommandResult wrong =
          applyCommand(raw, Command{"select_colour_range", colourStep(0.25f, 0.5f, 0.75f)});
      check(wrong.ok && raw.selection.has_value() &&
                selectionCoverageAt(&*raw.selection, PixelCoord{32, 32}) == 0.0f,
            "colour range: the undecoded numbers name a different colour and select nothing");

      OpenDocument viaDialog = makeOpStackDocument();
      const std::array<float, 3> swatch = {srgbEncode(0.25f), srgbEncode(0.5f), srgbEncode(0.75f)};
      const Layer* src = activeLayerOf(viaDialog);
      check(src != nullptr && src->rgbTiles.has_value() &&
                sameCoverage(od.selection,
                             applySelectColourRangeAction(swatch, kFloodDefaultTolerance,
                                                          kFloodDefaultEdgeBand, *src->rgbTiles, w,
                                                          h),
                             w, h),
            "colour range: agrees with the Colour Range dialog, texel for texel");

      OpenDocument noPixels = makeOpStackDocument();
      noPixels.document.layers[0].rgbTiles.reset();
      const CommandResult noSource =
          applyCommand(noPixels, Command{"select_colour_range", encoded});
      check(!noSource.ok && contains(noSource.status, "sample"),
            "colour range: a layer with no RGB to sample refuses, rather than selecting nothing");
    }

    // --- the luminance band ------------------------------------------------
    {
      OpenDocument od = makeOpStackDocument();
      JsonValue half = JsonValue::object();
      half.set("low", JsonValue::number(0.0));
      const CommandResult onlyLow = applyCommand(od, Command{"select_luminance_range", half});
      check(!onlyLow.ok && contains(onlyLow.status, "high"),
            "luminance range: a band missing one end refuses rather than defaulting to 0..1");

      const std::array<float, 4> texel = {0.25f, 0.5f, 0.75f, 1.0f};
      const float luma = selectionLuminanceOf(texel);
      JsonValue band = JsonValue::object();
      band.set("low", JsonValue::number(static_cast<double>(luma) - 0.05));
      band.set("high", JsonValue::number(static_cast<double>(luma) + 0.05));
      check(applyCommand(od, Command{"select_luminance_range", band}).ok &&
                od.selection.has_value() &&
                selectionCoverageAt(&*od.selection, PixelCoord{32, 32}) == 1.0f,
            "luminance range: a band around the layer's own luminance selects it");

      OpenDocument viaDialog = makeOpStackDocument();
      const Layer* src = activeLayerOf(viaDialog);
      check(src != nullptr &&
                sameCoverage(od.selection,
                             applySelectLuminanceRangeAction(luma - 0.05f, luma + 0.05f,
                                                             kFloodDefaultEdgeBand, *src->rgbTiles,
                                                             w, h),
                             w, h),
            "luminance range: agrees with the Luminance Range dialog, texel for texel");

      // core/SelectionRefine.hpp: "low > high selects nothing (an empty band
      // is empty, not inverted)". The dialog says so in yellow beside the
      // sliders. In a batch there is nobody to say it to, so it has to be a
      // warning on the result rather than thirty files that quietly selected
      // nothing.
      OpenDocument inverted = makeOpStackDocument();
      JsonValue backwards = JsonValue::object();
      backwards.set("low", JsonValue::number(0.9));
      backwards.set("high", JsonValue::number(0.1));
      const CommandResult empty =
          applyCommand(inverted, Command{"select_luminance_range", backwards});
      check(empty.ok && !empty.warnings.empty() && contains(empty.warnings[0], "nothing"),
            "luminance range: a backwards band warns that it selected nothing");
    }
  }

  return ok;
}

}  // namespace np
