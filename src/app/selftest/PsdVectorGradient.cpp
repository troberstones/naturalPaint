#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "color/Space.hpp"
#include "core/Gradient.hpp"
#include "core/Path.hpp"
#include "io/Descriptor.hpp"
#include "io/PsdVectorStyle.hpp"
#include "io/PsdVectorWrite.hpp"

namespace np {

// docs/psd-vector-shapes.md S2's PSD half: `GdFl` decoded into a `GradientDef`
// on the way in, and written back out on the way out.
//
// ==========================================================================
// What this section can and CANNOT prove, stated before any assertion
// ==========================================================================
//
// **No `.psd` on the machine this was written on contains a `GdFl` block.**
// Both samples were grepped and both have zero occurrences, so unlike
// app/selftest/PsdVectorStyle.cpp -- every one of whose fixtures is a hex dump
// of real Photoshop bytes -- the fixture below is this tree's own encoder's
// output. That makes the round trip a statement about internal consistency,
// not about Photoshop.
//
// Two things are done about that rather than shrugging at it:
//
//  1. **Section A asserts the encoded descriptor's full TREE against a
//     literal**, every key name spelled out. That does not make the names
//     right, but it puts them somewhere a reader can check them against
//     Adobe's documentation in one glance, and makes a typo in either
//     direction a visible diff rather than a round trip that still passes.
//     It is the same technique io/Descriptor.hpp names for its own fixtures.
//  2. **Section B round-trips through the DECODER, not through the encoder's
//     own field list.** A shared misunderstanding of the wire format would
//     survive that; a one-sided one -- a unit written as `#Prc` and read as a
//     fraction, a `Lctn` scaled by 100 instead of 4096, the y-negation applied
//     on one side only -- would not, and those are the mistakes actually
//     likely here.
//
// Headless, GPU-free, writes no files.
bool runPsdVectorGradientTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // The shape a gradient is placed against throughout: a 100 x 50 box whose
  // centre is (150, 125). Deliberately NOT square and NOT centred on the
  // origin, so a width/height swap or a dropped offset shows up.
  const PathBounds kBounds{true, 100.0f, 100.0f, 200.0f, 150.0f};

  auto twoStopRamp = []() {
    GradientDef g;
    g.name = "Foreground to Background";
    g.geometry.kind = GradientKind::Linear;
    g.geometry.spread = GradientSpread::Pad;
    g.geometry.x0 = 100.0f;
    g.geometry.y0 = 125.0f;
    g.geometry.x1 = 200.0f;
    g.geometry.y1 = 125.0f;
    g.stops.colorStops.push_back(ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f});
    g.stops.colorStops.push_back(ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f});
    return g;
  };

  // Decode a block this module just encoded, the way io/PsdImport would.
  auto decodeAsFill = [](const std::vector<uint8_t>& block, PsdVectorStyle& out,
                         std::string& error) {
    PsdVectorStyleBlocks blocks;
    blocks.gdfl = block;
    return decodePsdVectorStyle(blocks, out, error);
  };

  // ==========================================================================
  std::printf("  -- A. The encoded GdFl, key by key --\n");
  // ==========================================================================
  {
    GradientDef g = twoStopRamp();
    g.name = "Ramp";
    g.stops.colorStops[0].midpoint = 0.5f;
    g.stops.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
    g.stops.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});

    const std::vector<uint8_t> block = encodePsdGradientFillBlock(g, kBounds);
    check(!block.empty(), "GdFl: a two-stop ramp encodes to a non-empty block");

    const DescriptorParseResult parsed = parseVersionedActionDescriptor(block);
    check(parsed.ok, "GdFl: and io/Descriptor parses it back -- the framing is well-formed");
    if (!parsed.ok) std::printf("      parse error: %s\n", parsed.error.c_str());
    check(parsed.bytesConsumed == block.size(),
          "GdFl: the parse consumes the block EXACTLY -- no slack and no overrun");

    // The whole tree against a literal. Every four-character key here is
    // Adobe's, space padding included; `Angl`/`Scl `/`Opct` carry their unit
    // codes, and `Lctn`/`Mdpn` are `long`s in 0..4096 and 0..100.
    // The white stop's three channels, rendered the way the dump renders a
    // double. Computed rather than typed because `srgbEncode(1.0f)` is
    // 0.99999994 in float, not 1 -- 1.055f * 1 - 0.055f does not land exactly
    // on one -- so the field carries 254.99998480081558 and a literal "255"
    // here would be asserting something this build does not do. The transfer
    // function itself is asserted separately below, on a mid-grey stop, which
    // is the value that can actually tell an sRGB encode from a linear one.
    char whiteBuf[64];
    std::snprintf(whiteBuf, sizeof(whiteBuf), "%.17g",
                  static_cast<double>(srgbEncode(1.0f)) * 255.0);
    const std::string white(whiteBuf);
    const std::string want =
        "Objc 'null' 8 items\n"
        "  'Grad' Objc 'Grdn' 5 items\n"
        "    'Nm  ' TEXT \"Ramp\"\n"
        "    'GrdF' enum GrdF.CstS\n"
        "    'Intr' doub 4096\n"
        "    'Clrs' VlLs 2 items\n"
        "      [0] Objc 'Clrt' 4 items\n"
        "        'Clr ' Objc 'RGBC' 3 items\n"
        "          'Rd  ' doub 0\n"
        "          'Grn ' doub 0\n"
        "          'Bl  ' doub 0\n"
        "        'Type' enum Clry.UsrS\n"
        "        'Lctn' long 0\n"
        "        'Mdpn' long 50\n"
        "      [1] Objc 'Clrt' 4 items\n"
        "        'Clr ' Objc 'RGBC' 3 items\n"
        "          'Rd  ' doub " + white + "\n"
        "          'Grn ' doub " + white + "\n"
        "          'Bl  ' doub " + white + "\n"
        "        'Type' enum Clry.UsrS\n"
        "        'Lctn' long 4096\n"
        "        'Mdpn' long 50\n"
        "    'Trns' VlLs 2 items\n"
        "      [0] Objc 'TrnS' 3 items\n"
        "        'Opct' UntF #Prc 100\n"
        "        'Lctn' long 0\n"
        "        'Mdpn' long 50\n"
        "      [1] Objc 'TrnS' 3 items\n"
        "        'Opct' UntF #Prc 0\n"
        "        'Lctn' long 4096\n"
        "        'Mdpn' long 50\n"
        "  'Type' enum GrdT.Lnr \n"
        "  'Angl' UntF #Ang 0\n"
        "  'Scl ' UntF #Prc 100\n"
        "  'Ofst' Objc 'Pnt ' 2 items\n"
        "    'Hrzn' UntF #Prc 0\n"
        "    'Vrtc' UntF #Prc 0\n"
        "  'Dthr' bool true\n"
        "  'Rvrs' bool false\n"
        "  'Algn' bool true\n";
    const std::string got = parsed.ok ? dumpDescriptorTree(parsed.tree) : std::string();
    check(got == want, "GdFl: the whole descriptor tree matches, key for key and unit for unit");
    if (got != want) std::printf("      got:\n%s", got.c_str());

    // The colour transfer function, called out on its own because the tree
    // dump above would look right for a ramp whose endpoints are 0 and 1 even
    // if sRGB encoding were skipped -- srgbEncode(0) == 0 and
    // srgbEncode(1) == 1, the exact blind spot io/PsdVectorWrite.hpp names for
    // its own byte-identical SoCo assertion.
    GradientDef mid = twoStopRamp();
    mid.stops.colorStops[0].color = {0.5f, 0.5f, 0.5f};
    const DescriptorParseResult midParsed =
        parseVersionedActionDescriptor(encodePsdGradientFillBlock(mid, kBounds));
    const std::optional<double> red =
        midParsed.tree.root().field("Grad").field("Clrs").child(0).field("Clr ").field("Rd  ").asDouble();
    check(red.has_value() &&
              std::fabs(*red - static_cast<double>(srgbEncode(0.5f)) * 255.0) < 1e-6,
          "GdFl: a mid-grey stop goes out sRGB-ENCODED (188.0, not 127.5)");
  }

  // ==========================================================================
  std::printf("  -- B. The round trip through the decoder --\n");
  // ==========================================================================
  {
    GradientDef g = twoStopRamp();
    g.stops.colorStops[0].color = {0.25f, 0.0f, 0.0f};
    g.stops.colorStops[0].midpoint = 0.73f;
    g.stops.colorStops[1].color = {0.0f, 0.0f, 0.75f};
    g.stops.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
    g.stops.opacityStops.push_back(OpacityStop{0.5f, 0.25f, 0.5f});

    PsdVectorStyle style;
    std::string error;
    check(decodeAsFill(encodePsdGradientFillBlock(g, kBounds), style, error),
          "round trip: the block decodes");
    check(style.fillGradient.has_value(),
          "round trip: and lands in fillGradient, not in a warning");
    check(!style.fill.on,
          "round trip: fill.on stays FALSE -- the caller owns the table index, not the decoder");

    if (style.fillGradient.has_value()) {
      const PsdGradientFill& f = *style.fillGradient;
      check(f.name == "Foreground to Background", "round trip: the gradient's NAME survives");
      check(f.stops.colorStops.size() == 2 && f.stops.opacityStops.size() == 2,
            "round trip: both stop lists come back at their own lengths");

      // Colour, through sRGB encode and decode. The tolerance is the 0..255
      // quantisation of the descriptor's own field, not a fudge: a `Clr `
      // double is what Photoshop stores and it is bounded at 255.
      bool colorsMatch = true;
      for (size_t i = 0; i < 2 && i < f.stops.colorStops.size(); ++i)
        for (int c = 0; c < 3; ++c)
          if (std::fabs(f.stops.colorStops[i].color[c] - g.stops.colorStops[i].color[c]) > 2e-3f)
            colorsMatch = false;
      check(colorsMatch,
            "round trip: every stop colour returns within the 8-bit step the field itself has");

      check(f.stops.colorStops.size() == 2 &&
                std::fabs(f.stops.colorStops[0].midpoint - 0.73f) < 0.01f,
            "round trip: the midpoint survives its 0..100 integer field");
      check(f.stops.opacityStops.size() == 2 &&
                std::fabs(f.stops.opacityStops[1].opacity - 0.25f) < 1e-3f &&
                std::fabs(f.stops.opacityStops[1].position - 0.5f) < 1e-3f,
            "round trip: an opacity stop's #Prc value and its 0..4096 position both survive");

      // And the PLACEMENT, resolved back through the same function io/PsdImport
      // calls. This is the assertion that catches a one-sided angle sign or a
      // width/height swap, because the bounds are 100 x 50 and off-origin.
      const GradientGeometry geom = psdGradientGeometryFor(f.placement, kBounds);
      check(geom.kind == GradientKind::Linear && geom.spread == GradientSpread::Pad,
            "round trip: the kind and spread come back");
      check(std::fabs(geom.x0 - g.geometry.x0) < 0.01f &&
                std::fabs(geom.y0 - g.geometry.y0) < 0.01f &&
                std::fabs(geom.x1 - g.geometry.x1) < 0.01f &&
                std::fabs(geom.y1 - g.geometry.y1) < 0.01f,
            "round trip: and both geometry points land back where they started");
    }

    // A ramp at 30 degrees on a non-square shape -- the case where the angle
    // sign, the y-down negation and the projected-length rule all have to
    // agree between the two sides. An axis-aligned ramp cannot see any of them.
    GradientDef tilted = twoStopRamp();
    tilted.geometry.x0 = 120.0f;
    tilted.geometry.y0 = 140.0f;
    tilted.geometry.x1 = 180.0f;
    tilted.geometry.y1 = 110.0f;
    PsdVectorStyle tiltedStyle;
    if (decodeAsFill(encodePsdGradientFillBlock(tilted, kBounds), tiltedStyle, error) &&
        tiltedStyle.fillGradient.has_value()) {
      const GradientGeometry geom =
          psdGradientGeometryFor(tiltedStyle.fillGradient->placement, kBounds);
      check(std::fabs(geom.x0 - tilted.geometry.x0) < 0.05f &&
                std::fabs(geom.y0 - tilted.geometry.y0) < 0.05f &&
                std::fabs(geom.x1 - tilted.geometry.x1) < 0.05f &&
                std::fabs(geom.y1 - tilted.geometry.y1) < 0.05f,
            "round trip: a 30-degree ramp on a 100x50 shape returns to its own two points");
      check(tiltedStyle.fillGradient->placement.angleDegrees > 0.0,
            "round trip: and its angle is POSITIVE -- a ramp going UP the screen, y-up");
    } else {
      check(false, "round trip: the tilted ramp decodes");
    }

    // Radial and Reflected, the two kinds whose centre is p0 rather than the
    // midpoint. Getting that branch wrong halves or doubles the ramp.
    for (const auto& kv : {std::pair<GradientKind, GradientSpread>{GradientKind::Radial,
                                                                   GradientSpread::Pad},
                           std::pair<GradientKind, GradientSpread>{GradientKind::Linear,
                                                                   GradientSpread::Reflect}}) {
      GradientDef k = twoStopRamp();
      k.geometry.kind = kv.first;
      k.geometry.spread = kv.second;
      k.geometry.x0 = 150.0f;
      k.geometry.y0 = 125.0f;
      k.geometry.x1 = 190.0f;
      k.geometry.y1 = 125.0f;
      PsdVectorStyle st;
      const bool decoded = decodeAsFill(encodePsdGradientFillBlock(k, kBounds), st, error) &&
                           st.fillGradient.has_value();
      const GradientGeometry geom =
          decoded ? psdGradientGeometryFor(st.fillGradient->placement, kBounds)
                  : GradientGeometry{};
      check(decoded && geom.kind == k.geometry.kind && geom.spread == k.geometry.spread &&
                std::fabs(geom.x0 - 150.0f) < 0.05f && std::fabs(geom.x1 - 190.0f) < 0.05f,
            kv.first == GradientKind::Radial
                ? "round trip: a RADIAL keeps its centre and its radius"
                : "round trip: a REFLECTED ramp keeps its centre and its half-length");
    }
  }

  // ==========================================================================
  std::printf("  -- C. Reverse, and the midpoints that move with it --\n");
  // ==========================================================================
  {
    // `reverseGradientStops()` is what `Rvrs` becomes, and the midpoint
    // bookkeeping is the half that is easy to get wrong: a midpoint belongs to
    // the SEGMENT AFTER its stop, so reversing moves it by one AND flips it.
    GradientStops s;
    s.colorStops.push_back(ColorStop{0.0f, {1.0f, 0.0f, 0.0f}, 0.25f});
    s.colorStops.push_back(ColorStop{0.4f, {0.0f, 1.0f, 0.0f}, 0.8f});
    s.colorStops.push_back(ColorStop{1.0f, {0.0f, 0.0f, 1.0f}, 0.5f});

    GradientStops r = s;
    reverseGradientStops(r);
    check(r.colorStops.size() == 3 && r.colorStops[0].position == 0.0f &&
              std::fabs(r.colorStops[1].position - 0.6f) < 1e-6f &&
              r.colorStops[2].position == 1.0f,
          "reverse: positions become 1 - position and the list stays sorted ascending");
    check(r.colorStops[0].color[2] == 1.0f && r.colorStops[2].color[0] == 1.0f,
          "reverse: the colours are in the opposite order");
    check(std::fabs(r.colorStops[0].midpoint - 0.2f) < 1e-6f &&
              std::fabs(r.colorStops[1].midpoint - 0.75f) < 1e-6f,
          "reverse: each midpoint moves BACK one stop and flips -- 0.8 -> 0.2, 0.25 -> 0.75");

    // The property that makes all of the above mean something -- and the
    // **exact** places it holds, which is not "everywhere".
    //
    // The midpoint skew is `t^(ln 0.5 / ln m)`, and that family is not
    // symmetric: measured at m = 0.25, x = 0.75, `skew(1-x, 1-m)` is 0.0354
    // where `1 - skew(x, m)` is 0.134. So the reversed ramp mirrors the
    // original exactly at every STOP and at every segment's own 50 %-blend
    // position -- the two places the skew is pinned -- and drifts inside a
    // skewed segment. Asserting "everywhere" would be asserting something
    // false about a correct reversal; core/Gradient.hpp records the property.
    //
    // The five probes: the three stop positions, and the two 50 %-blend
    // positions (segment 0 spans [0, 0.4] with m = 0.25, so its blend lands at
    // 0.1; segment 1 spans [0.4, 1.0] with m = 0.8, so its blend lands at
    // 0.88).
    bool mirrors = true;
    for (const float t : {0.0f, 0.4f, 1.0f, 0.1f, 0.88f}) {
      const std::array<float, 3> a = gradientColorAt(s, t);
      const std::array<float, 3> b = gradientColorAt(r, 1.0f - t);
      for (int c = 0; c < 3; ++c)
        if (std::fabs(a[c] - b[c]) > 1e-4f) mirrors = false;
    }
    check(mirrors,
          "reverse: the reversed ramp mirrors the original at every stop AND every 50% blend");

    // And the other half of that finding, asserted rather than only written
    // down: INSIDE a skewed segment the two genuinely differ, so the probe set
    // above is a real restriction and not a tolerance nobody needed.
    const std::array<float, 3> inside = gradientColorAt(s, 0.3f);
    const std::array<float, 3> insideMirror = gradientColorAt(r, 0.7f);
    check(std::fabs(inside[0] - insideMirror[0]) > 0.05f,
          "reverse: and inside a SKEWED segment they differ -- the skew is not symmetric");

    // Reversing twice is the identity, which no partial implementation of the
    // midpoint rule satisfies.
    GradientStops twice = r;
    reverseGradientStops(twice);
    bool identity = twice.colorStops.size() == s.colorStops.size();
    for (size_t i = 0; identity && i < s.colorStops.size(); ++i)
      identity = std::fabs(twice.colorStops[i].position - s.colorStops[i].position) < 1e-6f &&
                 std::fabs(twice.colorStops[i].midpoint - s.colorStops[i].midpoint) < 1e-6f;
    check(identity, "reverse: applying it twice is the identity, midpoints included");
  }

  // ==========================================================================
  std::printf("  -- D. What a GdFl this build cannot express does instead --\n");
  // ==========================================================================
  {
    // Each of these is built by hand-editing a real encoded block's tree,
    // which is not possible through the encoder (it can only write what this
    // build can express) -- so they are built by writing the descriptor
    // directly, the way a foreign file would arrive.
    auto encodeRoot = [](const std::function<void(PsdWriter&)>& body, uint32_t itemCount) {
      PsdWriter w;
      w.u32(kActionDescriptorVersion);
      writePsdDescriptorHead(w, "null", itemCount);
      body(w);
      return w.take();
    };

    auto twoStopGradItem = [](PsdWriter& w, const char* form) {
      writePsdDescriptorObjectItem(w, "Grad", "Grdn", 3);
      writePsdDescriptorText(w, "Nm  ", "x");
      writePsdDescriptorEnum(w, "GrdF", "GrdF", form);
      writePsdDescriptorListItem(w, "Clrs", 1);
      writePsdDescriptorListObjectElement(w, "Clrt", 3);
      writePsdDescriptorObjectItem(w, "Clr ", "RGBC", 3);
      writePsdDescriptorDouble(w, "Rd  ", 255.0);
      writePsdDescriptorDouble(w, "Grn ", 0.0);
      writePsdDescriptorDouble(w, "Bl  ", 0.0);
      writePsdDescriptorInteger(w, "Lctn", 0);
      writePsdDescriptorInteger(w, "Mdpn", 50);
    };

    auto warningsMention = [](const PsdVectorStyle& s, const char* needle) {
      for (const std::string& w : s.warnings)
        if (w.find(needle) != std::string::npos) return true;
      return false;
    };

    // A DIAMOND gradient: a real Photoshop type with no receiving field.
    {
      const std::vector<uint8_t> block = encodeRoot(
          [&](PsdWriter& w) {
            twoStopGradItem(w, "CstS");
            writePsdDescriptorEnum(w, "Type", "GrdT", "Dmnd");
          },
          2);
      PsdVectorStyle style;
      std::string error;
      check(decodeAsFill(block, style, error), "Dmnd: the block still parses, so this is not a refusal of the file");
      check(!style.fillGradient.has_value() && !style.fill.on,
            "Dmnd: a diamond gradient leaves the fill OFF rather than drawing a radial");
      check(warningsMention(style, "Dmnd"), "Dmnd: and the warning names the type by its own tag");
    }

    // A NOISE gradient: `GrdF` is `ClNs` and there is no usable stop list.
    {
      const std::vector<uint8_t> block = encodeRoot(
          [&](PsdWriter& w) {
            twoStopGradItem(w, "ClNs");
            writePsdDescriptorEnum(w, "Type", "GrdT", "Lnr ");
          },
          2);
      PsdVectorStyle style;
      std::string error;
      check(decodeAsFill(block, style, error) && !style.fillGradient.has_value(),
            "ClNs: a noise gradient leaves the fill off");
      check(warningsMention(style, "noise"), "ClNs: and the warning says what it is");
    }

    // An EMPTY colour-stop list -- the shape a wrong `Clrs` key name would
    // produce, which is why it must be loud rather than a black fill.
    {
      const std::vector<uint8_t> block = encodeRoot(
          [&](PsdWriter& w) {
            writePsdDescriptorObjectItem(w, "Grad", "Grdn", 2);
            writePsdDescriptorText(w, "Nm  ", "x");
            writePsdDescriptorListItem(w, "Clrs", 0);
            writePsdDescriptorEnum(w, "Type", "GrdT", "Lnr ");
          },
          2);
      PsdVectorStyle style;
      std::string error;
      check(decodeAsFill(block, style, error) && !style.fillGradient.has_value() &&
                !style.fill.on,
            "empty Clrs: fill left off rather than painted black");
      check(warningsMention(style, "empty"), "empty Clrs: and it is named");
    }

    // `vstk.fillEnabled == false` must gate a GRADIENT fill exactly as it
    // gates a solid one -- the `PNG/4 - Layer.png` trap, one fill kind over.
    {
      GradientDef g = twoStopRamp();
      const std::vector<uint8_t> gdfl = encodePsdGradientFillBlock(g, kBounds);
      PsdWriter w;
      w.u32(kActionDescriptorVersion);
      writePsdDescriptorHead(w, "null", 2);
      writePsdDescriptorBool(w, "fillEnabled", false);
      writePsdDescriptorBool(w, "strokeEnabled", false);
      const std::vector<uint8_t> vstk = w.take();

      PsdVectorStyleBlocks blocks;
      blocks.gdfl = gdfl;
      blocks.vstk = vstk;
      PsdVectorStyle style;
      std::string error;
      check(decodePsdVectorStyle(blocks, style, error),
            "fillEnabled=false: the style decodes");
      check(!style.fillGradient.has_value() && !style.fill.on,
            "fillEnabled=false: a GRADIENT fill is disabled too, not only a solid one");
    }
  }

  std::printf("[selftest] psd vector gradient %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
