#include "io/PsdVectorStyle.hpp"

#include <string>
#include <string_view>

#include "color/Space.hpp"
#include "io/Descriptor.hpp"

// See io/PsdVectorStyle.hpp for the design. This is the first module in the
// tree to hand real Photoshop bytes to io/Descriptor -- every fixture in
// app/selftest/PsdVectorStyle.cpp is a hex dump of an actual `SoCo`/`vscg`/
// `vstk` payload, not a synthetic one this file's own writer produced.
namespace np {
namespace {

// `Clr `'s child keys are Photoshop's own four-character, space-padded RGBC
// fields ("Rd  ", "Grn ", "Bl  ") -- confirmed against real bytes, not typed
// from memory. Reads the 0..255 sRGB doubles and decodes them into `rgba`'s
// linear straight alpha; alpha is left at 1 (PSD's shape colour carries no
// alpha of its own). Returns false when `parent` has no `Clr ` field or that
// field is missing one of the three channels -- not a parse error, just
// nothing to paint with.
bool readClrColor(DescriptorRef parent, std::array<float, 4>& rgba) {
  DescriptorRef clr = parent.field("Clr ");
  if (!clr.valid()) return false;
  const std::optional<double> rd = clr.field("Rd  ").asDouble();
  const std::optional<double> gr = clr.field("Grn ").asDouble();
  const std::optional<double> bl = clr.field("Bl  ").asDouble();
  if (!rd || !gr || !bl) return false;
  rgba[0] = srgbDecode(static_cast<float>(*rd / 255.0));
  rgba[1] = srgbDecode(static_cast<float>(*gr / 255.0));
  rgba[2] = srgbDecode(static_cast<float>(*bl / 255.0));
  rgba[3] = 1.0f;
  return true;
}

LineCap mapLineCap(const std::string& valueId, std::vector<std::string>& warnings) {
  if (valueId == "strokeStyleButtCap") return LineCap::Butt;
  if (valueId == "strokeStyleRoundCap") return LineCap::Round;
  if (valueId == "strokeStyleSquareCap") return LineCap::Square;
  warnings.push_back("vstk: unrecognised strokeStyleLineCapType '" + valueId +
                     "'; defaulting to butt");
  return LineCap::Butt;
}

LineJoin mapLineJoin(const std::string& valueId, std::vector<std::string>& warnings) {
  if (valueId == "strokeStyleMiterJoin") return LineJoin::Miter;
  if (valueId == "strokeStyleRoundJoin") return LineJoin::Round;
  if (valueId == "strokeStyleBevelJoin") return LineJoin::Bevel;
  warnings.push_back("vstk: unrecognised strokeStyleLineJoinType '" + valueId +
                     "'; defaulting to miter");
  return LineJoin::Miter;
}

// `strokeStyleLineWidth`, `strokeStyleLineDashOffset` and each
// `strokeStyleLineDashSet` element are all `UntF`s that feed a `StrokeStyle`
// field measured in canvas pixels. The width in every fixture at hand is
// `#Pxl`, but the dash offset is `#Pnt` (points) in the very same file --
// genuinely a different unit, not a typo -- and nothing here has the
// resolution (`strokeStyleResolution` is a sibling field with no receiving
// slot either) needed to convert points to pixels correctly. Zero is zero in
// any unit, so a zero value is used as-is and silently, which is every sample
// seen so far; a NONZERO non-#Pxl value cannot be converted, and using its raw
// number as though it were already pixels would be a wrong value presented as
// a right one -- worse than a named absence, so it is dropped to zero and
// warned about by name instead.
float readPixelUnitFloat(const std::optional<DescriptorUnitFloat>& uf, std::string_view fieldName,
                        std::vector<std::string>& warnings) {
  if (!uf) return 0.0f;
  if (uf->unit == "#Pxl" || uf->value == 0.0) return static_cast<float>(uf->value);
  warnings.push_back("vstk: " + std::string(fieldName) + " is a nonzero value in unit '" +
                     uf->unit + "', not #Pxl; no conversion is available (strokeStyleResolution "
                     "has no receiving field) so it is dropped to zero rather than used "
                     "unconverted");
  return 0.0f;
}

}  // namespace

bool decodePsdVectorStyle(const PsdVectorStyleBlocks& blocks, PsdVectorStyle& out,
                          std::string& error) {
  out = PsdVectorStyle{};
  error.clear();

  // Defaults when there is no `vstk` at all: a fill-and-no-stroke-data layer
  // is filled (docs/psd-vector-shapes.md, "Where the colour is").
  bool fillEnabled = true;
  bool strokeEnabled = false;

  if (!blocks.vstk.empty()) {
    const DescriptorParseResult vstkResult =
        parseVersionedActionDescriptor(blocks.vstk.subspan(kPsdVstkDescriptorSkip));
    if (!vstkResult.ok) {
      error = "vstk: " + vstkResult.error;
      return false;
    }
    const DescriptorRef root = vstkResult.tree.root();

    fillEnabled = root.field("fillEnabled").asBoolean().value_or(true);
    strokeEnabled = root.field("strokeEnabled").asBoolean().value_or(false);

    out.strokeStyle.width =
        readPixelUnitFloat(root.field("strokeStyleLineWidth").asUnitFloat(),
                          "strokeStyleLineWidth", out.warnings);
    out.strokeStyle.dashOffset =
        readPixelUnitFloat(root.field("strokeStyleLineDashOffset").asUnitFloat(),
                          "strokeStyleLineDashOffset", out.warnings);
    if (const std::optional<double> miter = root.field("strokeStyleMiterLimit").asDouble())
      out.strokeStyle.miterLimit = static_cast<float>(*miter);

    if (const std::optional<DescriptorEnumerated> cap =
            root.field("strokeStyleLineCapType").asEnumerated())
      out.strokeStyle.cap = mapLineCap(cap->valueId, out.warnings);
    if (const std::optional<DescriptorEnumerated> join =
            root.field("strokeStyleLineJoinType").asEnumerated())
      out.strokeStyle.join = mapLineJoin(join->valueId, out.warnings);

    // `strokeStyleLineAlignment` other than centre has no receiving field:
    // core/PathStroke.hpp always centres the stroke on the path.
    if (const std::optional<DescriptorEnumerated> align =
            root.field("strokeStyleLineAlignment").asEnumerated()) {
      if (align->valueId != "strokeStyleAlignCenter") {
        out.warnings.push_back("vstk: strokeStyleLineAlignment '" + align->valueId +
                               "' has no receiving field; the stroke is centred anyway");
      }
    }

    const DescriptorRef dashSet = root.field("strokeStyleLineDashSet");
    if (dashSet.valid()) {
      for (size_t i = 0; i < dashSet.childCount(); ++i) {
        const DescriptorRef elem = dashSet.child(i);
        if (const std::optional<DescriptorUnitFloat> uf = elem.asUnitFloat()) {
          out.strokeStyle.dashes.push_back(
              readPixelUnitFloat(uf, "strokeStyleLineDashSet element", out.warnings));
        } else if (const std::optional<double> d = elem.asDouble()) {
          out.strokeStyle.dashes.push_back(static_cast<float>(*d));
        } else {
          out.warnings.push_back("vstk: strokeStyleLineDashSet element " + std::to_string(i) +
                                 " is neither a UntF nor a doub; skipped");
        }
      }
    }

    if (strokeEnabled) {
      const DescriptorRef content = root.field("strokeStyleContent");
      std::array<float, 4> strokeRgba{0.0f, 0.0f, 0.0f, 1.0f};
      if (content.valid() && readClrColor(content, strokeRgba)) {
        out.stroke.on = true;
        out.stroke.rgba = strokeRgba;
      } else {
        out.warnings.push_back(
            "vstk: strokeEnabled but strokeStyleContent has no solid Clr/RGBC colour; "
            "stroke left off");
      }
    }
  }

  // Fill precedence matches psd-tools' compositor: `SoCo`, then a standalone
  // `PtFl`, then a standalone `GdFl`, then `vscg`'s own embedded fill-type
  // tag (which repeats the same three-way choice one level down, because
  // `vscg` is where all nine of Apple's fill-bearing layers actually put it).
  // `fillCarrierNamed` tracks whether SoCo/PtFl/GdFl already settled the
  // question -- once one of them is present, `vscg`'s tag no longer gets a
  // vote, even though `vscg` (if also present) still has its bytes validated
  // below, so a malformed one is never silently ignored just because it lost
  // the precedence race. In both sample files a standalone PtFl/GdFl never
  // occurs alongside vscg, so that interaction is reasoned about, not
  // measured.
  bool haveFillColor = false;
  bool fillCarrierNamed = false;
  std::array<float, 4> fillRgba{0.0f, 0.0f, 0.0f, 1.0f};

  if (!blocks.soco.empty()) {
    fillCarrierNamed = true;
    const DescriptorParseResult socoResult =
        parseVersionedActionDescriptor(blocks.soco.subspan(kPsdSocoDescriptorSkip));
    if (!socoResult.ok) {
      error = "SoCo: " + socoResult.error;
      return false;
    }
    if (readClrColor(socoResult.tree.root(), fillRgba)) {
      haveFillColor = true;
    } else {
      out.warnings.push_back("SoCo: no Clr/RGBC colour found in the descriptor");
    }
  } else if (!blocks.ptfl.empty()) {
    // Pattern fill: `Paint` is solid-only (core/VectorShape.hpp), so this is a
    // named refusal, not a flat colour guessed from the pattern.
    fillCarrierNamed = true;
    out.warnings.push_back(
        "PtFl: a standalone pattern fill has no receiving field; fill left off");
  } else if (!blocks.gdfl.empty()) {
    // Gradient fill: same refusal, same reason -- never a flat colour guessed
    // from a gradient stop.
    fillCarrierNamed = true;
    out.warnings.push_back(
        "GdFl: a standalone gradient fill has no receiving field; fill left off");
  }

  if (!blocks.vscg.empty()) {
    if (blocks.vscg.size() < kPsdVscgDescriptorSkip) {
      error = "vscg: " + std::to_string(blocks.vscg.size()) +
             " bytes, too short to hold its 4-byte fill-type tag";
      return false;
    }
    const std::string fillType(reinterpret_cast<const char*>(blocks.vscg.data()),
                               kPsdVscgDescriptorSkip);
    const DescriptorParseResult vscgResult =
        parseVersionedActionDescriptor(blocks.vscg.subspan(kPsdVscgDescriptorSkip));
    if (!vscgResult.ok) {
      error = "vscg: " + vscgResult.error;
      return false;
    }
    if (!haveFillColor && !fillCarrierNamed) {
      if (fillType == "SoCo") {
        if (readClrColor(vscgResult.tree.root(), fillRgba)) {
          haveFillColor = true;
        } else {
          out.warnings.push_back("vscg: no Clr/RGBC colour found in the descriptor");
        }
      } else if (fillType == "PtFl" || fillType == "GdFl") {
        out.warnings.push_back("vscg: fill kind '" + fillType + "' (" +
                               (fillType == "PtFl" ? "pattern" : "gradient") +
                               ") has no receiving field; fill left off");
      } else {
        out.warnings.push_back("vscg: unrecognised fill-type tag '" + fillType +
                               "'; fill left off");
      }
    }
  }

  if (fillEnabled && haveFillColor) {
    out.fill.on = true;
    out.fill.rgba = fillRgba;
  } else if (fillEnabled && blocks.soco.empty() && blocks.ptfl.empty() && blocks.gdfl.empty() &&
             blocks.vscg.empty()) {
    out.warnings.push_back(
        "fillEnabled but no fill block (SoCo/PtFl/GdFl/vscg) is present; fill left off");
  }

  return true;
}

}  // namespace np
