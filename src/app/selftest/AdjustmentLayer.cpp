#include "app/selftest/Support.hpp"

namespace np {

bool runAdjustmentLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, and why nearly everything here is at exactly zero -----
  //
  // **The fixtures are chosen so that the whole chain is exact**, which is a
  // stronger claim than any tolerance and is available here in a way it was
  // not for steps 3 and 4 (a Kubelka-Munk latent and an f16 mask sample are
  // both lossy by construction; an adjustment layer stores nothing at all).
  // Three properties do it:
  //
  //   * every fixture texel's alpha is a **power of two** (1.0 or 0.5), so
  //     `applyPointOpsPremultiplied()`'s un-premultiply and re-premultiply are
  //     an exact division and an exact multiplication rather than two
  //     roundings;
  //   * the two ops used for the numeric claims are exact on dyadic inputs --
  //     Exposure at +1 stop is `std::exp2(1.0f)` = 2.0f, a multiply by a power
  //     of two, and a ChannelMixer identity row with a 0.25 offset is
  //     `1*r + 0*g + 0*b + 0.25`, all exactly representable;
  //   * every coverage used (0, 0.25, 0.5, 0.75, 1) is dyadic, so the lerp in
  //     `adjustedPremultiplied()` is exact too.
  //
  // So the references below are exact float literals, compared with `==`, and
  // the byte-identity claims are `memcmp`. **kAdjustTol is used in exactly one
  // place** -- the probe-versus-flattener agreement, where the flattener's own
  // final un-premultiply is one correctly-rounded division. Half an ulp at
  // results in [0.25, 1) is 2^-25 = 2.98e-8; bounded at 1.0e-7, 3.4x, the
  // identical derivation runLayerStackTest(), runBlendTest(),
  // runPigmentLayerTest() and runLayerMaskTest() each restate for themselves.
  constexpr float kAdjustTol = 1.0e-7f;

  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const size_t i = (static_cast<size_t>(y) * img.width + x) * 4;
    return {img.pixels[i], img.pixels[i + 1], img.pixels[i + 2], img.pixels[i + 3]};
  };
  auto sameImage = [](const DecodedImage& a, const DecodedImage& b) {
    return a.pixels.size() == b.pixels.size() && !a.pixels.empty() &&
           std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size() * sizeof(float)) == 0;
  };
  auto writeRgb = [](Document& doc, size_t layerIndex, int32_t x, int32_t y,
                     const std::array<float, 4>& premultiplied) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].rgbTiles->getOrCreate(tileCoordAt(at))
        .writePixel(tileLocalOffset(at), premultiplied);
  };
  auto writeMask = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float v) {
    const PixelCoord at{x, y};
    doc.layers[layerIndex].mask->getOrCreate(tileCoordAt(at)).writeCoverage(tileLocalOffset(at), v);
  };
  // Exposure at +1 stop: an exact doubling of straight linear RGB.
  auto exposureOp = [](float stops) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::Exposure;
    op.exposure.stops = stops;
    return op;
  };
  // The identity channel mixer plus a constant offset on all three rows: an
  // exact `+ k` on straight linear RGB, and -- crucially for the ordering
  // section -- an affine op that does NOT commute with a multiply.
  auto offsetOp = [](float k) {
    Op op;
    op.opClass = OpClass::PointA;
    op.pointKind = PointOpKind::ChannelMixer;
    for (size_t i = 0; i < 3; ++i) op.channelMixer.matrix[i][3] = k;
    return op;
  };

  // --- 1. The kind: an Adjustment layer holds nothing but its stack ------
  {
    const Layer adj = makeAdjustmentLayer("Curves 1");
    check(adj.kind == LayerKind::Adjustment && !adj.rgbTiles.has_value() &&
              !adj.pigmentTiles.has_value() && !adj.mask.has_value() && adj.ops.size() == 0,
          "kind: makeAdjustmentLayer() engages NO tile store of any kind and starts with an "
          "empty stack -- the kind's definition, not an unfinished factory");
    check(std::string(layerKindGlyph(LayerKind::Adjustment)) == "\xE2\x96\xA4" &&
              std::string(layerRowSubLine(adj)) ==
                  "ADJUSTMENT \xC2\xB7 NORMAL \xC2\xB7 100%",
          "kind: docs/ui.md §3.2's row glyph and sub-line, and an empty stack prints NO op "
          "marker -- so every row written before this step is unchanged");
    Layer withOps = adj;
    withOps.ops.add(exposureOp(1.0f));
    check(contains(layerRowSubLine(withOps), "\xC2\xB7 1 OP") &&
              !contains(layerRowSubLine(withOps), "1 OPS"),
          "kind: a one-entry stack reads `- 1 OP` -- on an Adjustment layer the stack is the "
          "only content the row could describe, since the layer holds no pixels");
    withOps.ops.add(offsetOp(0.25f));
    check(contains(layerRowSubLine(withOps), "\xC2\xB7 2 OPS"),
          "kind: and two read `- 2 OPS`");

    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    check(addLayer(doc, 1, makeAdjustmentLayer("adj")).ok && doc.layers.size() == 2 &&
              doc.layers[1].kind == LayerKind::Adjustment,
          "kind: core/LayerOps adds one to a document like any other layer -- the stack model "
          "needed no new operation for this kind");
    check(addLayerMask(doc, 1).ok && doc.layers[1].mask.has_value(),
          "kind: and it takes a mask, which is PRD D13's whole mechanism (dodge and burn is "
          "\"a brush painting into an adjustment layer's mask\") -- step 4 predicted exactly "
          "this and needed no change for it");
  }

  // --- 2. io/OpSerial: the format `np:ops` finally has ------------------
  {
    // Every one of the six kinds, with non-default params, round-tripped and
    // compared field by field at EXACTLY zero -- the bit patterns are what
    // travel, so "nearly equal" would mean a bug.
    OpStack stack;
    Op levels;
    levels.pointKind = PointOpKind::Levels;
    for (size_t c = 0; c < 3; ++c) {
      levels.levels[c] = LevelsParams{0.01f * static_cast<float>(c + 1), 0.93f, 2.2f, 0.03f,
                                      0.97f};
    }
    stack.add(levels);
    Op curves;
    curves.pointKind = PointOpKind::Curves;
    curves.curves[0] = {{0.0f, 0.0f}, {0.31f, 0.47f}, {1.0f, 1.0f}};
    curves.curves[1] = {{0.0f, 0.125f}, {1.0f, 0.875f}};
    curves.curves[2] = {};  // an unauthored channel is a real, distinct state
    curves.enabled = false;  // and disabled must survive too
    stack.add(curves);
    stack.add(exposureOp(-1.75f));
    Op sat;
    sat.pointKind = PointOpKind::Saturation;
    sat.saturation.scale = 1.4f;
    sat.saturation.lumaWeights = {0.3f, 0.6f, 0.1f};
    stack.add(sat);
    Op gray;
    gray.pointKind = PointOpKind::Grayscale;
    gray.grayscale.lumaWeights = {0.25f, 0.5f, 0.25f};
    stack.add(gray);
    Op mixer;
    mixer.pointKind = PointOpKind::ChannelMixer;
    for (size_t i = 0; i < 3; ++i)
      for (size_t j = 0; j < 4; ++j)
        mixer.channelMixer.matrix[i][j] = 0.1f * static_cast<float>(i * 4 + j + 1);
    stack.add(mixer);
    Op spatial;
    spatial.opClass = OpClass::SpatialB;  // a fixture only -- see core/OpStack.hpp
    spatial.enabled = false;
    stack.add(spatial);

    const std::string encoded = serializeOpStack(stack);
    OpStack back;
    std::string why;
    const bool decoded = deserializeOpStack(encoded, &back, &why);
    check(decoded && back.size() == stack.size(),
          "opserial: a stack holding all six PointOpKinds plus a class-B entry round-trips "
          "with its length intact");
    bool identical = decoded && back.size() == stack.size();
    for (size_t i = 0; i < stack.size() && identical; ++i) {
      const Op& a = stack.at(i);
      const Op& b = back.at(i);
      if (a.opClass != b.opClass || a.enabled != b.enabled || a.pointKind != b.pointKind)
        identical = false;
      for (size_t c = 0; c < 3 && identical; ++c) {
        if (std::memcmp(&a.levels[c], &b.levels[c], sizeof(LevelsParams)) != 0) identical = false;
        if (a.curves[c].size() != b.curves[c].size()) identical = false;
        for (size_t k = 0; k < a.curves[c].size() && identical; ++k)
          if (a.curves[c][k].x != b.curves[c][k].x || a.curves[c][k].y != b.curves[c][k].y)
            identical = false;
      }
      if (a.exposure.stops != b.exposure.stops) identical = false;
      if (a.saturation.scale != b.saturation.scale ||
          a.saturation.lumaWeights != b.saturation.lumaWeights)
        identical = false;
      if (a.grayscale.lumaWeights != b.grayscale.lumaWeights) identical = false;
      if (a.channelMixer.matrix != b.channelMixer.matrix) identical = false;
    }
    check(identical,
          "opserial: and every field of every entry comes back BIT-IDENTICAL -- including a "
          "disabled entry, a 3-point curve, a 2-point curve and an unauthored empty one, "
          "which are three different states");
    check(decoded && serializeOpStack(back) == encoded,
          "opserial: re-encoding the decoded stack reproduces the identical string, so the "
          "encoding is a function of the stack and not of how it was built");
    std::printf("  [measured] a 7-entry stack (all six point kinds + a class-B entry) encodes "
                "to %zu characters of np:ops\n", encoded.size());

    // **The fixture that does not share the encoder's assumptions.** Typed out
    // by hand, byte by byte, exactly as io/NpaintFile's 52-byte PSD fixture is
    // -- if the encoder and the decoder ever agreed on something wrong
    // together, the round-trip check above would still pass and this one would
    // not.
    //
    //   0200                  u16 opCount = 2
    //   0a000000              u32 bodyLength = 10
    //     0000                  u16 class 0 = PointA
    //     0200                  u16 kind  2 = Exposure
    //     01                    u8  enabled
    //     00                    u8  reserved
    //     0000803f              f32 1.0 (0x3F800000, little-endian)
    //   0a000000              u32 bodyLength = 10
    //     0700                  u16 class 7 -- NOT a class this build knows
    //     0000                  u16 kind (meaningless for an unknown class)
    //     01                    u8  enabled
    //     00                    u8  reserved
    //     deadbeef              4 bytes of a newer build's params
    const std::string handBuilt =
        "npops1:02000a0000000000020001000000803f0a000000070000000100deadbeef";
    OpStack hand;
    const bool handOk = deserializeOpStack(handBuilt, &hand, &why);
    check(handOk && hand.size() == 2,
          "opserial: a HAND-BUILT 60-character payload -- written byte by byte from the spec, "
          "not produced by this module -- decodes to two entries");
    if (handOk && hand.size() == 2) {
      check(hand.at(0).opClass == OpClass::PointA &&
                hand.at(0).pointKind == PointOpKind::Exposure && hand.at(0).enabled &&
                hand.at(0).exposure.stops == 1.0f && hand.at(0).unrecognised.empty(),
            "opserial: entry 0 is exactly the Exposure(+1 stop) the hand-written bytes spell, "
            "at exactly 1.0f");
      const std::vector<uint8_t> wantRaw{0x07, 0x00, 0x00, 0x00, 0x01,
                                          0x00, 0xde, 0xad, 0xbe, 0xef};
      check(hand.at(1).opClass == OpClass::Unknown && hand.at(1).unrecognised == wantRaw,
            "opserial: entry 1 declares a class this build has no name for, so it becomes "
            "OpClass::Unknown holding its own 10 bytes VERBATIM -- PRD I10 at the entry level");
      check(serializeOpStack(hand) == handBuilt,
            "opserial: and re-encoding gives back the hand-written string CHARACTER FOR "
            "CHARACTER, unknown entry and all -- an op this build cannot evaluate survives a "
            "round trip rather than being dropped");
      // Inert, and inert through the machinery that already existed rather
      // than through a new special case.
      check(layerPointOps(hand).size() == 1,
            "opserial: the unknown entry produces NO PointOp -- detectRuns() breaks a run at "
            "every non-PointA entry, so `Unknown` needed no new code to be un-evaluatable");
      const std::vector<OpRun> runs = hand.detectRuns();
      check(runs.size() == 1 && runs[0].startIndex == 0 && runs[0].endIndex == 1,
            "opserial: and it SPLITS the run rather than being folded into one -- a newer "
            "build's op sitting between two point ops must not let them collapse across it");
    }

    // Every way a value can be malformed *as a container*, each refused by
    // name rather than half-read.
    OpStack sink;
    std::string e1, e2, e3, e4, e5, e6;
    const bool r1 = deserializeOpStack("npops2:0000", &sink, &e1);
    const bool r2 = deserializeOpStack("npops1:000", &sink, &e2);
    const bool r3 = deserializeOpStack("npops1:0g", &sink, &e3);
    const bool r4 = deserializeOpStack("npops1:0100", &sink, &e4);
    const bool r5 = deserializeOpStack("npops1:0000ff", &sink, &e5);
    const bool r6 = deserializeOpStack("", &sink, &e6);
    check(!r1 && !r2 && !r3 && !r4 && !r5 && !r6,
          "opserial: a v2 tag, an odd payload, a non-hex digit, a truncated record, a trailing "
          "byte and an empty value are all refused");
    check(contains(e1, "npops2:") && contains(e1, "npops1:") && contains(e2, "odd") &&
              contains(e3, "hex digit") && contains(e4, "truncated") &&
              contains(e5, "follow the") && contains(e6, "npops1:"),
          "opserial: and each refusal names what it saw and what it wanted -- a version tag is "
          "the prefix precisely so a build says \"I read v1\" instead of misreading a v2 "
          "payload");
    check(sink.size() == 0,
          "opserial: a refused value leaves the destination stack untouched, so a caller "
          "cannot end up with half a grade");

    // The two forward-compatibility rules that are NOT container errors.
    // Levels with 64 params bytes where this build's parse consumes 60: a
    // newer build added a field, and reading the leading 60 would be guessing
    // that the fields it shares kept their meaning.
    const std::string longLevels =
        "npops1:0100" "46000000" "0000" "0000" "01" "00" + std::string(64 * 2, '0');
    OpStack longer;
    check(deserializeOpStack(longLevels, &longer, &why) && longer.size() == 1 &&
              longer.at(0).opClass == OpClass::Unknown && longer.at(0).unrecognised.size() == 70,
          "opserial: a Levels record whose body is LONGER than this build's parse consumes is "
          "carried whole as Unknown, not half-read -- a newer build's extra field would "
          "otherwise be dropped on the next save");
    check(deserializeOpStack(longLevels, &longer, &why) && serializeOpStack(longer) == longLevels,
          "opserial: and it re-encodes identically");
    // The reserved byte: written 0, and a non-zero value means a newer build
    // gave it a meaning this build cannot honour.
    const std::string reservedUsed = "npops1:01000a0000000000020001010000803f";
    OpStack reserved;
    check(deserializeOpStack(reservedUsed, &reserved, &why) && reserved.size() == 1 &&
              reserved.at(0).opClass == OpClass::Unknown,
          "opserial: an otherwise-valid Exposure record whose RESERVED byte is non-zero is "
          "Unknown too -- the byte this build writes as 0 is the one a newer build would use "
          "to change what the rest of the record means");
    // An unknown *point op kind* under a known class, the other half of the rule.
    const std::string unknownKind = "npops1:0100" "06000000" "0000" "6300" "01" "00";
    OpStack uk;
    check(deserializeOpStack(unknownKind, &uk, &why) && uk.size() == 1 &&
              uk.at(0).opClass == OpClass::Unknown,
          "opserial: a PointA record naming kind 99 -- a point op this build has no "
          "implementation for -- is Unknown as well, not a Levels op by default");

    check(serializeOpStack(OpStack{}) == "npops1:0000",
          "opserial: an empty stack encodes to a well-formed zero-count payload rather than an "
          "empty string, which this OpenImageIO drops (io/NpaintFile writes no attribute at "
          "all for one, which is a different decision made in a different place)");
  }

  // --- 3. It transforms the composite below, against exact references ----
  {
    // Two RGB layers under one Adjustment layer at +1 stop. Every reference
    // here is an exact float literal, for the reasons stated at the top: an
    // alpha of 1.0 or 0.5 makes the un-premultiply exact, and exp2(1) = 2.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});   // opaque
    writeRgb(doc, 0, 2, 2, {0.125f, 0.25f, 0.375f, 0.5f});  // half-covered
    addLayer(doc, 1, makeAdjustmentLayer("expose"));
    doc.layers[1].ops.add(exposureOp(1.0f));

    const DecodedImage flat = flattenDocumentToLinear(doc);
    const std::array<float, 4> opaque = pixelOf(flat, 1, 1);
    const std::array<float, 4> half = pixelOf(flat, 2, 2);
    const std::array<float, 4> empty = pixelOf(flat, 5, 5);
    std::printf("  [measured] +1 stop over an opaque (0.25, 0.50, 0.75) texel -> (%.4f, %.4f, "
                "%.4f) a=%.4f; over a half-covered one -> (%.4f, %.4f, %.4f) a=%.4f; over "
                "nothing -> (%.4f, %.4f, %.4f) a=%.4f\n",
                static_cast<double>(opaque[0]), static_cast<double>(opaque[1]),
                static_cast<double>(opaque[2]), static_cast<double>(opaque[3]),
                static_cast<double>(half[0]), static_cast<double>(half[1]),
                static_cast<double>(half[2]), static_cast<double>(half[3]),
                static_cast<double>(empty[0]), static_cast<double>(empty[1]),
                static_cast<double>(empty[2]), static_cast<double>(empty[3]));
    check(opaque[0] == 0.5f && opaque[1] == 1.0f && opaque[2] == 1.5f && opaque[3] == 1.0f,
          "grade: an opaque texel is EXACTLY doubled -- straight (0.25, 0.5, 0.75) becomes "
          "(0.5, 1.0, 1.5), including above 1.0, because ops/PointOps deliberately does not "
          "clamp and the working space is scene-linear");
    check(half[0] == 0.5f && half[1] == 1.0f && half[2] == 1.5f && half[3] == 0.5f,
          "grade: a HALF-COVERED texel is doubled in STRAIGHT colour and keeps its coverage -- "
          "the grade goes through applyPointOpsPremultiplied()'s un-premultiply bracket, not "
          "over the premultiplied numbers");
    check(empty[0] == 0.0f && empty[1] == 0.0f && empty[2] == 0.0f && empty[3] == 0.0f,
          "grade: and a texel with nothing beneath it is untouched -- an exposure of +1 stop "
          "on no colour is no colour");

    // The same claim as a whole-image byte-identity, which is what catches a
    // grade that leaked one channel or one row.
    Document byHand = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(byHand, 0, 1, 1, {0.5f, 1.0f, 1.5f, 1.0f});
    writeRgb(byHand, 0, 2, 2, {0.25f, 0.5f, 0.75f, 0.5f});
    check(sameImage(flat, flattenDocumentToLinear(byHand)),
          "grade: the whole composite is BYTE-IDENTICAL to a document holding the graded "
          "values directly -- so nothing outside the two painted texels moved");

    // Alpha. The claim `adjustedPremultiplied()` makes by construction.
    Document noAdj = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(noAdj, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    writeRgb(noAdj, 0, 2, 2, {0.125f, 0.25f, 0.375f, 0.5f});
    const std::vector<float> withAdj = compositeDocumentPremultiplied(doc);
    const std::vector<float> without = compositeDocumentPremultiplied(noAdj);
    bool alphaIdentical = withAdj.size() == without.size();
    bool colourMoved = false;
    for (size_t i = 0; i + 3 < withAdj.size() && alphaIdentical; i += 4) {
      if (std::memcmp(&withAdj[i + 3], &without[i + 3], sizeof(float)) != 0) alphaIdentical = false;
      for (int c = 0; c < 3; ++c)
        if (withAdj[i + c] != without[i + c]) colourMoved = true;
    }
    check(alphaIdentical && colourMoved,
          "alpha: every accumulated alpha is BIT-IDENTICAL with and without the adjustment "
          "layer, while colour is not -- a grade is a colour operation and coverage is not "
          "colour, so the walk writes three channels and never the fourth");
  }

  // --- 4. Opacity and a mask are \"how much of the adjustment applies\" ----
  {
    Document base = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(base, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    const DecodedImage plain = flattenDocumentToLinear(base);

    Document doc = base;
    addLayer(doc, 1, makeAdjustmentLayer("expose"));
    doc.layers[1].ops.add(exposureOp(1.0f));

    // Opacity 0: an exact no-op, and the strongest form of that claim --
    // byte-identical to the layer not being there at all.
    doc.layers[1].opacity = 0.0f;
    check(sameImage(flattenDocumentToLinear(doc), plain),
          "opacity: at opacity 0 the composite is BYTE-IDENTICAL to the document with no "
          "adjustment layer in it -- a skip, not a lerp by zero, which is a multiply and an "
          "add and therefore not the identity on every float");
    doc.layers[1].opacity = 1.0f;
    doc.layers[1].visible = false;
    check(sameImage(flattenDocumentToLinear(doc), plain),
          "opacity: and so is a hidden one, which is the same rule every other kind already "
          "obeys");
    doc.layers[1].visible = true;

    // Opacity 1: exactly what the op stack computed, assigned rather than
    // lerped -- `below + 1.0f*(g - below)` is two roundings and is not `g`.
    const std::array<float, 4> full = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(full[0] == 0.5f && full[1] == 1.0f && full[2] == 1.5f,
          "opacity: at opacity 1 the result is EXACTLY the graded value, not a lerp that "
          "lands near it");

    // And the middle, exact because the fixture is dyadic.
    doc.layers[1].opacity = 0.5f;
    const std::array<float, 4> mid = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    std::printf("  [measured] opacity 0.5 between (0.2500, 0.5000, 0.7500) and (0.5000, "
                "1.0000, 1.5000) -> (%.4f, %.4f, %.4f)\n",
                static_cast<double>(mid[0]), static_cast<double>(mid[1]),
                static_cast<double>(mid[2]));
    check(mid[0] == 0.375f && mid[1] == 0.75f && mid[2] == 1.125f && mid[3] == 1.0f,
          "opacity: 0.5 is exactly halfway between the ungraded and graded values -- the same "
          "lerp identity opacity already means for a source layer, applied to a transform");

    // An empty stack, which is every layer any earlier `.npaint` carries.
    Document emptyStack = base;
    addLayer(emptyStack, 1, makeAdjustmentLayer("nothing yet"));
    check(sameImage(flattenDocumentToLinear(emptyStack), plain),
          "opacity: an adjustment layer with an EMPTY stack is byte-identically absent too -- "
          "adding one must never itself change what is on screen");

    // The mask: per texel, and it composes with opacity as a plain product,
    // the same claim step 4 made for a source layer.
    Document masked = base;
    writeRgb(masked, 0, 2, 2, {0.25f, 0.5f, 0.75f, 1.0f});
    writeRgb(masked, 0, 3, 3, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(masked, 1, makeAdjustmentLayer("expose"));
    masked.layers[1].ops.add(exposureOp(1.0f));
    addLayerMask(masked, 1);
    writeMask(masked, 1, 1, 1, 1.0f);   // full adjustment
    writeMask(masked, 1, 2, 2, 0.5f);   // half
    writeMask(masked, 1, 3, 3, 0.0f);   // none
    const DecodedImage maskedFlat = flattenDocumentToLinear(masked);
    const std::array<float, 4> mFull = pixelOf(maskedFlat, 1, 1);
    const std::array<float, 4> mHalf = pixelOf(maskedFlat, 2, 2);
    const std::array<float, 4> mNone = pixelOf(maskedFlat, 3, 3);
    std::printf("  [measured] one adjustment layer, mask 1.0 / 0.5 / 0.0 on three texels of "
                "the same colour: (%.4f, %.4f, %.4f) / (%.4f, %.4f, %.4f) / (%.4f, %.4f, "
                "%.4f)\n",
                static_cast<double>(mFull[0]), static_cast<double>(mFull[1]),
                static_cast<double>(mFull[2]), static_cast<double>(mHalf[0]),
                static_cast<double>(mHalf[1]), static_cast<double>(mHalf[2]),
                static_cast<double>(mNone[0]), static_cast<double>(mNone[1]),
                static_cast<double>(mNone[2]));
    check(mFull[0] == 0.5f && mFull[1] == 1.0f && mFull[2] == 1.5f &&
              mHalf[0] == 0.375f && mHalf[1] == 0.75f && mHalf[2] == 1.125f &&
              mNone[0] == 0.25f && mNone[1] == 0.5f && mNone[2] == 0.75f,
          "mask: three texels of the same colour under one adjustment layer come out fully "
          "graded, half graded and UNGRADED -- per-texel control of how much of the "
          "adjustment applies, which is PRD D13's entire mechanism");
    check(mFull[3] == 1.0f && mHalf[3] == 1.0f && mNone[3] == 1.0f,
          "mask: and none of the three had its coverage touched");

    // mask x opacity == their product, byte for byte, as step 4 proved for a
    // source layer.
    Document maskHalfOpacityHalf = base;
    addLayer(maskHalfOpacityHalf, 1, makeAdjustmentLayer("expose"));
    maskHalfOpacityHalf.layers[1].ops.add(exposureOp(1.0f));
    maskHalfOpacityHalf.layers[1].opacity = 0.5f;
    addLayerMask(maskHalfOpacityHalf, 1);
    writeMask(maskHalfOpacityHalf, 1, 1, 1, 0.5f);
    Document opacityQuarter = base;
    addLayer(opacityQuarter, 1, makeAdjustmentLayer("expose"));
    opacityQuarter.layers[1].ops.add(exposureOp(1.0f));
    opacityQuarter.layers[1].opacity = 0.25f;
    check(sameImage(flattenDocumentToLinear(maskHalfOpacityHalf),
                    flattenDocumentToLinear(opacityQuarter)),
          "mask: opacity 0.5 under a 0.5 mask composites BYTE-IDENTICALLY to opacity 0.25 with "
          "no mask -- both reach adjustedPremultiplied() carrying the single scalar 0.25");
    Document opacityHalf = base;
    addLayer(opacityHalf, 1, makeAdjustmentLayer("expose"));
    opacityHalf.layers[1].ops.add(exposureOp(1.0f));
    opacityHalf.layers[1].opacity = 0.5f;
    check(!sameImage(flattenDocumentToLinear(maskHalfOpacityHalf),
                     flattenDocumentToLinear(opacityHalf)),
          "mask: with opacity 0.5 and NO mask as the negative control, so the identity above "
          "is not passing because the mask does nothing");
  }

  // --- 5. Scope: everything below, and nothing above --------------------
  {
    // Four layers, three of them holding one non-overlapping texel each, with
    // the adjustment sitting third. PRD C5 says "the composite below it", so
    // both layers under it are graded -- not merely the one directly beneath,
    // which would be a CLIPPING mask (PRD C9, PLAN.md step 9) and is a
    // different feature that is deliberately not built here.
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("middle"));
    writeRgb(doc, 1, 2, 2, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(doc, 2, makeAdjustmentLayer("expose"));
    doc.layers[2].ops.add(exposureOp(1.0f));
    addLayer(doc, 3, makeRgbLayer("above"));
    writeRgb(doc, 3, 3, 3, {0.25f, 0.5f, 0.75f, 1.0f});

    const DecodedImage flat = flattenDocumentToLinear(doc);
    const std::array<float, 4> bottom = pixelOf(flat, 1, 1);
    const std::array<float, 4> middle = pixelOf(flat, 2, 2);
    const std::array<float, 4> above = pixelOf(flat, 3, 3);
    std::printf("  [measured] the same (0.2500, 0.5000, 0.7500) on three layers around one "
                "adjustment: two below -> (%.4f, %.4f, %.4f) and (%.4f, %.4f, %.4f); one "
                "above -> (%.4f, %.4f, %.4f)\n",
                static_cast<double>(bottom[0]), static_cast<double>(bottom[1]),
                static_cast<double>(bottom[2]), static_cast<double>(middle[0]),
                static_cast<double>(middle[1]), static_cast<double>(middle[2]),
                static_cast<double>(above[0]), static_cast<double>(above[1]),
                static_cast<double>(above[2]));
    check(bottom[0] == 0.5f && bottom[1] == 1.0f && bottom[2] == 1.5f && middle[0] == 0.5f &&
              middle[1] == 1.0f && middle[2] == 1.5f,
          "scope: BOTH layers below the adjustment are graded, not just the one directly "
          "beneath -- PRD C5 says \"the composite below it\", and restricting it to the layer "
          "beneath is PRD C9's clipping mask, a different feature");
    check(above[0] == 0.25f && above[1] == 0.5f && above[2] == 0.75f,
          "scope: and the layer ABOVE it is untouched, to the ulp -- the walk is bottom to "
          "top, so \"below\" is exactly what has already accumulated");

    // An adjustment layer at the very bottom has nothing under it at all.
    Document loneAdj = Document::createBlank(8, 8, WorkingSpace{});
    loneAdj.layers[0] = makeAdjustmentLayer("over nothing");
    loneAdj.layers[0].ops.add(exposureOp(3.0f));
    const DecodedImage lone = flattenDocumentToLinear(loneAdj);
    bool allZero = lone.valid();
    for (const float v : lone.pixels)
      if (v != 0.0f) allZero = false;
    check(allZero,
          "scope: an adjustment layer over NOTHING composites to fully transparent black "
          "everywhere -- +3 stops of no colour is no colour, and the alpha-0 skip is what "
          "keeps applyPointOpsPremultiplied()'s divide-by-zero guard out of this path rather "
          "than making a fourth copy of it");
    Document blank = Document::createBlank(8, 8, WorkingSpace{});
    blank.layers.clear();
    check(sameImage(lone, flattenDocumentToLinear(blank)) || lone.pixels == std::vector<float>(8 * 8 * 4, 0.0f),
          "scope: byte-identically so");
  }

  // --- 6. Stacking order: two adjustments compose, in order -------------
  {
    // Exposure (a multiply by 2) and a channel-mixer offset (a `+ 0.25`) do
    // NOT commute, and both are exact on dyadic inputs -- so the two orders
    // have different exact answers and neither needs a tolerance.
    //
    //   expose then offset : 0.25 -> 0.50 -> 0.75
    //   offset then expose : 0.25 -> 0.50 -> 1.00
    auto build = [&](bool exposeFirst) {
      Document d = Document::createBlank(8, 8, WorkingSpace{});
      writeRgb(d, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
      addLayer(d, 1, makeAdjustmentLayer(exposeFirst ? "expose" : "offset"));
      d.layers[1].ops.add(exposeFirst ? exposureOp(1.0f) : offsetOp(0.25f));
      addLayer(d, 2, makeAdjustmentLayer(exposeFirst ? "offset" : "expose"));
      d.layers[2].ops.add(exposeFirst ? offsetOp(0.25f) : exposureOp(1.0f));
      return d;
    };
    const std::array<float, 4> a = pixelOf(flattenDocumentToLinear(build(true)), 1, 1);
    const std::array<float, 4> b = pixelOf(flattenDocumentToLinear(build(false)), 1, 1);
    std::printf("  [measured] (0.2500, 0.5000, 0.7500) through two adjustment layers: "
                "expose-then-offset -> (%.4f, %.4f, %.4f); offset-then-expose -> (%.4f, %.4f, "
                "%.4f)\n",
                static_cast<double>(a[0]), static_cast<double>(a[1]), static_cast<double>(a[2]),
                static_cast<double>(b[0]), static_cast<double>(b[1]), static_cast<double>(b[2]));
    check(a[0] == 0.75f && a[1] == 1.25f && a[2] == 1.75f,
          "order: the LOWER adjustment layer runs first -- x2 then +0.25 gives 0.75, 1.25, "
          "1.75 exactly");
    check(b[0] == 1.0f && b[1] == 1.5f && b[2] == 2.0f,
          "order: swapping the two layers gives 1.0, 1.5, 2.0 instead -- two adjustment layers "
          "compose in stack order, and the pair chosen does not commute so the test could "
          "actually fail");

    // The same two ops in one stack on one layer must equal the two-layer
    // form, which is what makes \"a layer's stack\" and \"a stack of layers\"
    // the same evaluation order rather than two conventions.
    Document oneLayer = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(oneLayer, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(oneLayer, 1, makeAdjustmentLayer("both"));
    oneLayer.layers[1].ops.add(exposureOp(1.0f));
    oneLayer.layers[1].ops.add(offsetOp(0.25f));
    check(sameImage(flattenDocumentToLinear(oneLayer), flattenDocumentToLinear(build(true))),
          "order: two ops in one adjustment layer's stack are BYTE-IDENTICAL to the same two "
          "ops in two stacked adjustment layers -- one evaluation order, not two");
  }

  // --- 7. The blend mode an adjustment layer cannot honour --------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(doc, 1, makeAdjustmentLayer("multiply me"));
    doc.layers[1].ops.add(exposureOp(1.0f));
    doc.layers[1].blend = "multiply";

    std::vector<std::string> warnings;
    const std::vector<float> walked = compositeDocumentPremultiplied(doc, &warnings);
    bool named = false;
    for (const std::string& w : warnings)
      if (contains(w, "multiply") && contains(w, "Adjustment") && contains(w, "no source"))
        named = true;
    check(warnings.size() == 1 && named,
          "blend: an Adjustment layer carrying `multiply` is warned about BY NAME -- a blend "
          "mode combines a source with a backdrop and an adjustment layer has no source, so "
          "the operand that would play src is the backdrop itself");
    Document asNormal = doc;
    asNormal.layers[1].blend = "normal";
    const std::vector<float> normalWalk = compositeDocumentPremultiplied(asNormal);
    check(walked.size() == normalWalk.size() &&
              std::memcmp(walked.data(), normalWalk.data(), walked.size() * sizeof(float)) == 0,
          "blend: and it is composited BYTE-IDENTICALLY to the same layer at `normal` -- "
          "approximate, and said so, never silently");
    std::vector<std::string> none;
    compositeDocumentPremultiplied(asNormal, &none);
    check(none.empty(),
          "blend: an Adjustment layer at `normal` warns about nothing, so the warning above "
          "is about the mode and not about the kind");
    check(contains(adjustmentLayerBlendWarning(3, doc.layers[1]), "layer 3") &&
              contains(adjustmentLayerBlendWarning(3, doc.layers[1]), "\"multiply me\""),
          "blend: the sentence names the layer by index and by its user-facing name, the "
          "io/Export refusal style every other warning here follows");
  }

  // --- 8. The probe and the flattener agree -----------------------------
  {
    Document doc = Document::createBlank(8, 8, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.25f, 0.5f, 0.75f, 1.0f});
    addLayer(doc, 1, makeAdjustmentLayer("expose"));
    doc.layers[1].ops.add(exposureOp(1.0f));
    doc.layers[1].opacity = 0.75f;
    addLayerMask(doc, 1);
    writeMask(doc, 1, 1, 1, 0.5f);

    ProbeParams all;
    all.sampleAllLayers = true;
    const ProbeSample sample = probePixel(doc, PixelCoord{1, 1}, all);
    const std::array<float, 4> flat = pixelOf(flattenDocumentToLinear(doc), 1, 1);
    check(std::fabs(sample.linear[0] - flat[0]) <= kAdjustTol &&
              std::fabs(sample.linear[1] - flat[1]) <= kAdjustTol &&
              std::fabs(sample.linear[2] - flat[2]) <= kAdjustTol &&
              std::fabs(sample.linear[3] - flat[3]) <= kAdjustTol,
          "probe: the eyedropper and the export agree on a masked, faded adjustment layer -- "
          "both call core/Composite's own adjustedPremultiplied(), so they cannot grow two "
          "answers");
    // An adjustment layer holds no colour, so a document of nothing else must
    // still probe as transparent black rather than as \"something graded\".
    Document onlyAdj = Document::createBlank(8, 8, WorkingSpace{});
    onlyAdj.layers[0] = makeAdjustmentLayer("alone");
    onlyAdj.layers[0].ops.add(exposureOp(2.0f));
    const ProbeSample none = probePixel(onlyAdj, PixelCoord{1, 1}, all);
    check(none.linear[3] == 0.0f && none.linear[0] == 0.0f,
          "probe: a document of nothing but adjustment layers probes as transparent black -- "
          "an adjustment layer contributes no colour, so it does not make the probe report "
          "one");
    ProbeParams own;
    own.sampleAllLayers = false;
    own.activeLayerIndex = 1;
    const ProbeSample layerOwn = probePixel(doc, PixelCoord{1, 1}, own);
    check(layerOwn.linear[3] == 0.0f,
          "probe: single-layer mode on an adjustment layer reports transparent black -- the "
          "question is what the layer holds, and this kind holds no pixels at all");
  }

  // --- 9. The regression boundary, re-made across the new code path -----
  {
    Document doc = Document::createBlank(16, 16, WorkingSpace{});
    writeRgb(doc, 0, 1, 1, {0.3f, 0.6f, 0.9f, 1.0f});
    addLayer(doc, 1, makeRgbLayer("second"));
    writeRgb(doc, 1, 9, 9, {0.125f, 0.25f, 0.375f, 0.5f});
    std::vector<float> sum(16 * 16 * 4, 0.0f);
    for (const Layer& layer : doc.layers) {
      for (const auto& [coord, tile] : *layer.rgbTiles) {
        const PixelCoord origin = tileOrigin(coord);
        for (int32_t ty = 0; ty < kTileSize; ++ty) {
          for (int32_t tx = 0; tx < kTileSize; ++tx) {
            const int32_t dx = origin.x + tx, dy = origin.y + ty;
            if (dx < 0 || dx >= 16 || dy < 0 || dy >= 16) continue;
            const std::array<float, 4> px = tile.readPixel(PixelCoord{tx, ty});
            for (int i = 0; i < 4; ++i)
              sum[(static_cast<size_t>(dy) * 16 + static_cast<size_t>(dx)) * 4 + i] += px[i];
          }
        }
      }
    }
    const std::vector<float> walked = compositeDocumentPremultiplied(doc);
    check(walked.size() == sum.size() &&
              std::memcmp(walked.data(), sum.data(), sum.size() * sizeof(float)) == 0,
          "regression: a non-overlapping multi-layer document with NO adjustment layer still "
          "composites byte-identically to the plain sum -- the new branch costs a document "
          "without one not even an ulp");
  }

  // --- 10. The `.npaint` round trip: the decision this step turned on ----
  {
    const char* kPath = "selftest_adjust.npaint";
    const char* kBare = "selftest_adjust_bare.npaint";
    const char* kAgain = "selftest_adjust_again.npaint";
    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);

    auto bytesWithoutCapDate = [](const char* path) -> std::vector<unsigned char> {
      std::ifstream in(path, std::ios::binary);
      std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
      static const std::string kNeedle = "capDate";
      for (size_t i = 0; i + kNeedle.size() <= b.size(); ++i) {
        if (std::memcmp(b.data() + i, kNeedle.data(), kNeedle.size()) != 0) continue;
        for (size_t j = i; j < std::min(i + 47, b.size()); ++j) b[j] = 0;
      }
      return b;
    };

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    writeRgb(doc, 0, 3, 4, {0.5f, 0.25f, 0.125f, 1.0f});
    addLayer(doc, 1, makeAdjustmentLayer("Curves 1"));

    // The stack-free file first: it is the reference for the property that
    // makes this format change safe.
    const NpaintSaveResult bare = saveNpaint(doc, kBare);
    check(bare.ok,
          "npaint: an RGB layer plus a stack-free Adjustment layer saves -- the "
          "kind io/NpaintFile refused outright until this step");

    // Now give both layers a stack, including one entry this build cannot
    // interpret, so the round trip has to carry an unknown op through a FILE
    // and not merely through a string.
    doc.layers[0].ops.add(exposureOp(-0.5f));
    Op curves;
    curves.pointKind = PointOpKind::Curves;
    curves.curves[0] = {{0.0f, 0.0f}, {0.25f, 0.4f}, {1.0f, 1.0f}};
    doc.layers[0].ops.add(curves);
    doc.layers[1].ops.add(exposureOp(1.0f));
    OpStack withUnknown;
    std::string why;
    const bool builtUnknown = deserializeOpStack(
        "npops1:02000a0000000000020001000000803f0a000000070000000100deadbeef", &withUnknown,
        &why);
    check(builtUnknown, "npaint: the hand-built stack with an unknown entry is available");
    doc.layers[1].ops = withUnknown;
    addLayerMask(doc, 1);
    writeMask(doc, 1, 5, 6, 0.25f);

    const NpaintSaveResult saved = saveNpaint(doc, kPath);
    check(saved.ok && saved.partsWritten == 3 && saved.warnings.empty(),
          "npaint: it saves as three parts with NOTHING approximate about it -- until this "
          "step every non-empty op stack produced a warning naming what the file could not "
          "hold");
    const NpaintLoadResult back = loadNpaint(kPath);
    check(back.ok && back.warnings.empty() && back.document.layers.size() == 2,
          "npaint: and it loads back clean, with both layers");
    if (back.ok && back.document.layers.size() == 2) {
      check(serializeOpStack(back.document.layers[0].ops) == serializeOpStack(doc.layers[0].ops) &&
                back.document.layers[0].ops.size() == 2,
            "npaint: the RGB layer's two-entry stack -- an Exposure and a three-point Curves "
            "-- round-trips, so `np:ops` is not an Adjustment-only feature");
      check(back.document.layers[1].kind == LayerKind::Adjustment &&
                !back.document.layers[1].rgbTiles.has_value() &&
                !back.document.layers[1].pigmentTiles.has_value(),
            "npaint: the Adjustment layer comes back as an Adjustment layer holding no "
            "pixel storage of any kind");
      check(back.document.layers[1].ops.size() == 2 &&
                back.document.layers[1].ops.at(1).opClass == OpClass::Unknown &&
                back.document.layers[1].ops.at(1).unrecognised ==
                    doc.layers[1].ops.at(1).unrecognised,
            "npaint: including the entry whose op class this build has no name for -- its "
            "bytes survive a full save/load through OpenEXR, in position, PRD I10 at the "
            "entry level rather than at the attribute level");
      check(back.document.layers[1].mask.has_value() &&
                back.document.layers[1].mask->occupiedTileCount() == 1,
            "npaint: and its mask, in the one channel EXR requires the part to have at all "
            "(a zero-channel part is refused by this OpenImageIO with \"Missing or empty "
            "channel list in header\" -- measured)");
      check(sameImage(flattenDocumentToLinear(doc),
                      flattenDocumentToLinear(back.document)),
            "npaint: the reloaded document composites BYTE-IDENTICALLY to the saved one, "
            "which is the whole point -- an adjustment layer whose stack did not survive "
            "would be a layer with no content left");
    }

    // np:mask on an Adjustment part: the one rule this format has nowhere
    // else, because the channel's presence cannot carry the distinction.
    Document noMask = doc;
    removeLayerMask(noMask, 1);
    const NpaintSaveResult noMaskSaved = saveNpaint(noMask, kAgain);
    const NpaintLoadResult noMaskBack = loadNpaint(kAgain);
    check(noMaskSaved.ok && noMaskBack.ok && noMaskBack.document.layers.size() == 2 &&
              !noMaskBack.document.layers[1].mask.has_value(),
          "npaint: an Adjustment layer with NO mask loads back with `Layer::mask` "
          "disengaged, although its part still carries a `mask` channel -- np:mask says "
          "which, since presence cannot");

    // The property the whole change rests on: a document with no op stacks
    // produces exactly the bytes it produced before this step.
    Document stripped = doc;
    stripped.layers[0].ops = OpStack{};
    stripped.layers[1].ops = OpStack{};
    removeLayerMask(stripped, 1);
    std::remove(kAgain);
    const NpaintSaveResult again = saveNpaint(stripped, kAgain);
    check(again.ok && !bytesWithoutCapDate(kBare).empty() &&
              bytesWithoutCapDate(kBare) == bytesWithoutCapDate(kAgain),
          "npaint: emptying every op stack gives back a file BYTE-IDENTICAL to the one "
          "written before they existed (OpenImageIO's capDate masked, which HEAD's own two "
          "runs differ in too) -- np:ops is written only for a non-empty stack");
    check(bytesWithoutCapDate(kPath).size() > bytesWithoutCapDate(kBare).size(),
          "npaint: and the file WITH stacks really is bigger, so the check above is not "
          "passing because nothing was ever written");

    // PRD I10 for an `np:ops` this build cannot parse: written through the
    // carry, refused by the reader with a named warning, and put back
    // unchanged on the next save.
    Document carried = Document::createBlank(128, 128, WorkingSpace{});
    writeRgb(carried, 0, 1, 1, {0.5f, 0.5f, 0.5f, 1.0f});
    NpaintCarry inject;
    inject.layerAttributes.resize(1);
    NpaintAttribute future;
    future.name = "np:ops";
    future.type = NpaintAttribute::Type::String;
    future.stringValue = "npops2:cafe";
    inject.layerAttributes[0].push_back(future);
    std::remove(kAgain);
    const NpaintSaveResult futureSave = saveNpaint(carried, kAgain, {}, &inject);
    const NpaintLoadResult futureBack = loadNpaint(kAgain);
    bool warnedByName = false;
    for (const std::string& w : futureBack.warnings)
      if (contains(w, "npops2:") && contains(w, "npops1:")) warnedByName = true;
    check(futureSave.ok && futureBack.ok && warnedByName &&
              futureBack.document.layers.size() == 1 &&
              futureBack.document.layers[0].ops.size() == 0,
          "npaint: an np:ops this build cannot decode -- a v2 tag -- is warned about by "
          "name and the layer opens with no stack rather than with a guess");
    bool carriedBack = false;
    if (futureBack.carry.layerAttributes.size() == 1)
      for (const NpaintAttribute& a : futureBack.carry.layerAttributes[0])
        if (a.name == "np:ops" && a.stringValue == "npops2:cafe") carriedBack = true;
    check(carriedBack,
          "npaint: and it is CARRIED rather than dropped, so the next save writes it back "
          "unchanged -- an op stack is the one attribute whose loss would empty a whole "
          "layer, which is exactly what PRD I10 exists to prevent");
    std::remove(kPath);
    const NpaintSaveResult rewritten = saveNpaint(futureBack.document, kPath, {},
                                                  &futureBack.carry);
    const NpaintLoadResult reread = loadNpaint(kPath);
    bool stillThere = false;
    if (reread.ok && reread.carry.layerAttributes.size() == 1)
      for (const NpaintAttribute& a : reread.carry.layerAttributes[0])
        if (a.name == "np:ops" && a.stringValue == "npops2:cafe") stillThere = true;
    check(rewritten.ok && stillThere,
          "npaint: proven by writing it back out and reading it again, not by inspecting the "
          "carry and assuming");

    // The malformed-document refusal for this kind, the mirror of the ones
    // step 3 added for RGB and Pigment.
    Document malformed = Document::createBlank(128, 128, WorkingSpace{});
    malformed.layers[0] = makeAdjustmentLayer("bad");
    malformed.layers[0].rgbTiles.emplace();
    const NpaintSaveResult refused = saveNpaint(malformed, kPath);
    check(!refused.ok && contains(refused.error, "Adjustment") &&
              contains(refused.error, "holds no pixels"),
          "npaint: an Adjustment layer carrying pixel tiles is refused by index, name and "
          "kind -- its part has no channel to put them in, so writing would drop them");

    for (const char* p : {kPath, kBare, kAgain}) std::remove(p);
    check(std::fopen(kPath, "rb") == nullptr && std::fopen(kBare, "rb") == nullptr &&
              std::fopen(kAgain, "rb") == nullptr,
          "npaint: every scratch file this section wrote is removed");
  }

  std::printf("[selftest] adjustment layers %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


// ==========================================================================
// PLAN.md Phase 5 step 6 -- COW tiles
// ==========================================================================


}  // namespace np
