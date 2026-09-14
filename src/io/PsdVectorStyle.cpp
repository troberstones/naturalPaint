#include "io/PsdVectorStyle.hpp"

#include <algorithm>
#include <cmath>
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

// `Clr `'s three channels as straight linear RGB, without the alpha
// `readClrColor()` also writes. A gradient stop carries no alpha of its own --
// transparency lives in the separate `Trns` list -- so the four-component form
// would be inventing a field.
bool readClrRgb(DescriptorRef parent, std::array<float, 3>& rgb) {
  std::array<float, 4> rgba{0.0f, 0.0f, 0.0f, 1.0f};
  if (!readClrColor(parent, rgba)) return false;
  rgb = {rgba[0], rgba[1], rgba[2]};
  return true;
}

// Photoshop's stop position: a `long` in 0..4096, not a percentage and not a
// fraction. Clamped rather than refused -- a position outside the range is a
// stop outside the ramp, which core/Gradient's flat extrapolation already
// handles, and refusing a whole gradient over one would lose the picture for
// a detail nothing can see.
float locationToPosition(const std::optional<int32_t>& lctn) {
  if (!lctn) return 0.0f;
  return std::clamp(static_cast<float>(*lctn) / 4096.0f, 0.0f, 1.0f);
}

// `Mdpn`, a `long` percentage in 0..100. core/Gradient clamps the extremes
// itself (both send the skew exponent to infinity), so this only normalises.
float midpointFromPercent(const std::optional<int32_t>& mdpn) {
  if (!mdpn) return 0.5f;
  return std::clamp(static_cast<float>(*mdpn) / 100.0f, 0.0f, 1.0f);
}

// `GrdT`'s five members. Two map onto one `GradientKind` each, `Rflc` maps
// onto Linear-with-Reflect (which is what "reflected" IS: the ramp mirrored
// about its start), and `Dmnd` has nothing to map onto at all.
bool mapGradientType(const std::string& valueId, PsdGradientPlacement& out) {
  if (valueId == "Lnr ") {
    out.kind = GradientKind::Linear;
    out.spread = GradientSpread::Pad;
    return true;
  }
  if (valueId == "Rdl ") {
    out.kind = GradientKind::Radial;
    out.spread = GradientSpread::Pad;
    return true;
  }
  if (valueId == "Angl") {
    out.kind = GradientKind::Angular;
    out.spread = GradientSpread::Pad;  // Angular wraps and ignores this
    return true;
  }
  if (valueId == "Rflc") {
    out.kind = GradientKind::Linear;
    out.spread = GradientSpread::Reflect;
    return true;
  }
  // `Dmnd` -- a diamond gradient, whose parameter is a Chebyshev-style
  // distance no `GradientKind` expresses. Refused by name rather than
  // approximated with a radial, which would be visibly a different shape.
  return false;
}

// The `GdFl` descriptor's root, into a ramp plus a placement. `false` with a
// warning appended means "recognisably a gradient this build cannot express",
// which the caller turns into `fill.on = false`.
bool readGradientFill(DescriptorRef root, std::string_view carrier, PsdGradientFill& out,
                      std::vector<std::string>& warnings) {
  const std::string named(carrier);
  const DescriptorRef grad = root.field("Grad");
  if (!grad.valid()) {
    warnings.push_back(named + ": a gradient fill with no 'Grad' descriptor; fill left off");
    return false;
  }

  if (const std::optional<std::string_view> name = grad.field("Nm  ").asText())
    out.name = std::string(*name);

  // A NOISE gradient has no stop list at all -- it is a random ramp generated
  // from a seed and a colour model, and nothing here can reproduce it. Named
  // rather than silently arriving as an empty ramp.
  if (const std::optional<DescriptorEnumerated> form = grad.field("GrdF").asEnumerated()) {
    if (form->valueId != "CstS") {
      warnings.push_back(named + ": gradient form '" + form->valueId +
                         "' is not a custom-stop ramp (it is most likely a noise gradient, "
                         "which has no stop list to read); fill left off");
      return false;
    }
  }

  const DescriptorRef colors = grad.field("Clrs");
  for (size_t i = 0; i < colors.childCount(); ++i) {
    const DescriptorRef stop = colors.child(i);
    ColorStop cs;
    cs.position = locationToPosition(stop.field("Lctn").asInteger());
    cs.midpoint = midpointFromPercent(stop.field("Mdpn").asInteger());
    if (!readClrRgb(stop, cs.color)) {
      warnings.push_back(named + ": gradient colour stop " + std::to_string(i) +
                         " has no Clr/RGBC colour; it is skipped");
      continue;
    }
    // `Clry` = `FrgC`/`BckC` means "whatever the swatch holds when this is
    // drawn". `GradientStops` has no such field, so the stop imports with the
    // colour Photoshop last resolved into `Clr ` -- which is a real colour and
    // not a guess, but stops tracking the swatch. Named, because that is a
    // difference a user would otherwise discover by changing the foreground.
    if (const std::optional<DescriptorEnumerated> type = stop.field("Type").asEnumerated()) {
      if (type->valueId == "FrgC" || type->valueId == "BckC") {
        warnings.push_back(named + ": gradient colour stop " + std::to_string(i) +
                           " tracks Photoshop's " +
                           (type->valueId == "FrgC" ? "foreground" : "background") +
                           " swatch; it imports as the fixed colour the file last stored for "
                           "it and no longer follows a swatch");
      }
    }
    out.stops.colorStops.push_back(cs);
  }

  if (out.stops.colorStops.empty()) {
    warnings.push_back(named +
                       ": a gradient fill whose colour-stop list is empty or unreadable; fill "
                       "left off rather than painted black");
    return false;
  }

  const DescriptorRef trns = grad.field("Trns");
  for (size_t i = 0; i < trns.childCount(); ++i) {
    const DescriptorRef stop = trns.child(i);
    OpacityStop os;
    os.position = locationToPosition(stop.field("Lctn").asInteger());
    os.midpoint = midpointFromPercent(stop.field("Mdpn").asInteger());
    // `Opct` is a `#Prc`, so 0..100 rather than 0..1.
    if (const std::optional<DescriptorUnitFloat> op = stop.field("Opct").asUnitFloat())
      os.opacity = std::clamp(static_cast<float>(op->value / 100.0), 0.0f, 1.0f);
    else if (const std::optional<double> d = stop.field("Opct").asDouble())
      os.opacity = std::clamp(static_cast<float>(*d / 100.0), 0.0f, 1.0f);
    out.stops.opacityStops.push_back(os);
  }

  // Photoshop writes both lists ascending, but the sort is the render path's
  // stated precondition and costs nothing on lists this size -- and a file
  // from anywhere is untrusted input.
  sortGradientStops(out.stops);

  if (const std::optional<DescriptorEnumerated> type = root.field("Type").asEnumerated()) {
    if (!mapGradientType(type->valueId, out.placement)) {
      warnings.push_back(named + ": gradient type '" + type->valueId +
                         "' has no receiving field (this build has linear, radial and angular "
                         "only); fill left off rather than drawn as a different shape");
      return false;
    }
  }

  if (const std::optional<DescriptorUnitFloat> angle = root.field("Angl").asUnitFloat())
    out.placement.angleDegrees = angle->value;
  else if (const std::optional<double> d = root.field("Angl").asDouble())
    out.placement.angleDegrees = *d;

  if (const std::optional<DescriptorUnitFloat> scale = root.field("Scl ").asUnitFloat())
    out.placement.scalePercent = scale->value;
  else if (const std::optional<double> d = root.field("Scl ").asDouble())
    out.placement.scalePercent = *d;

  const DescriptorRef offset = root.field("Ofst");
  if (offset.valid()) {
    if (const std::optional<DescriptorUnitFloat> h = offset.field("Hrzn").asUnitFloat())
      out.placement.offsetXPercent = h->value;
    if (const std::optional<DescriptorUnitFloat> v = offset.field("Vrtc").asUnitFloat())
      out.placement.offsetYPercent = v->value;
  }

  // `Rvrs` is applied to the STOPS here rather than carried to the renderer,
  // so nothing downstream has to know about it -- and swapping the two
  // geometry endpoints instead would have been wrong for Radial (whose ramp
  // runs centre to rim) and for Angular (whose sweep direction is fixed).
  if (root.field("Rvrs").asBoolean().value_or(false)) reverseGradientStops(out.stops);

  // `Dthr` (dither) has no receiving field and needs none: ops/Gradient.hpp §3
  // measures that f16 output cannot band, so Photoshop's dither would be
  // correcting a problem this build does not have. Silent on purpose.
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
      } else if (content.valid() && content.field("Grad").valid()) {
        // A gradient STROKE. `Paint` can receive one, but resolving its
        // placement needs the STROKE's outline rather than the fill's, and no
        // sample file has one to check that against -- so it is named rather
        // than guessed. io/PsdVectorStyle.hpp states the cut.
        out.warnings.push_back(
            "vstk: strokeStyleContent is a gradient; a gradient STROKE is not imported (its "
            "placement would have to be resolved against the stroke outline, which nothing "
            "here has measured); stroke left off");
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
  std::optional<PsdGradientFill> gradientFill;

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
    fillCarrierNamed = true;
    const DescriptorParseResult gdflResult =
        parseVersionedActionDescriptor(blocks.gdfl.subspan(kPsdGdflDescriptorSkip));
    if (!gdflResult.ok) {
      error = "GdFl: " + gdflResult.error;
      return false;
    }
    PsdGradientFill grad;
    if (readGradientFill(gdflResult.tree.root(), "GdFl", grad, out.warnings))
      gradientFill = std::move(grad);
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
      } else if (fillType == "GdFl") {
        PsdGradientFill grad;
        if (readGradientFill(vscgResult.tree.root(), "vscg/GdFl", grad, out.warnings))
          gradientFill = std::move(grad);
      } else if (fillType == "PtFl") {
        out.warnings.push_back(
            "vscg: fill kind 'PtFl' (pattern) has no receiving field; fill left off");
      } else {
        out.warnings.push_back("vscg: unrecognised fill-type tag '" + fillType +
                               "'; fill left off");
      }
    }
  }

  if (fillEnabled && haveFillColor) {
    out.fill.on = true;
    out.fill.rgba = fillRgba;
  } else if (fillEnabled && gradientFill.has_value()) {
    // `fill.on` stays FALSE here: the caller turns it on once it has appended
    // the gradient to the document's table and knows the index. See
    // io/PsdVectorStyle.hpp on why the index has exactly one writer.
    out.fillGradient = std::move(gradientFill);
  } else if (fillEnabled && blocks.soco.empty() && blocks.ptfl.empty() && blocks.gdfl.empty() &&
             blocks.vscg.empty()) {
    out.warnings.push_back(
        "fillEnabled but no fill block (SoCo/PtFl/GdFl/vscg) is present; fill left off");
  }

  return true;
}

GradientGeometry psdGradientGeometryFor(const PsdGradientPlacement& placement,
                                       const PathBounds& bounds) {
  GradientGeometry g;
  g.kind = placement.kind;
  g.spread = placement.spread;
  if (!bounds.valid) return g;

  const double w = static_cast<double>(bounds.maxX) - static_cast<double>(bounds.minX);
  const double h = static_cast<double>(bounds.maxY) - static_cast<double>(bounds.minY);
  const double scale = placement.scalePercent / 100.0;

  const double cx = (static_cast<double>(bounds.minX) + static_cast<double>(bounds.maxX)) * 0.5 +
                    w * placement.offsetXPercent / 100.0;
  const double cy = (static_cast<double>(bounds.minY) + static_cast<double>(bounds.maxY)) * 0.5 +
                    h * placement.offsetYPercent / 100.0;

  constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
  const double a = placement.angleDegrees * kDegToRad;
  const double ca = std::cos(a);
  // NEGATED: Photoshop's angle is measured on a y-UP screen and document space
  // is y-DOWN, so 90 degrees must point toward SMALLER y. Dropping this sign
  // flips every non-axis-aligned gradient vertically, which looks plausible
  // and is wrong -- the header names it as one of the three readings to check
  // against the first real file.
  const double sa = -std::sin(a);

  g.x0 = static_cast<float>(cx);
  g.y0 = static_cast<float>(cy);

  if (placement.kind == GradientKind::Angular) {
    // Only the DIRECTION matters: `gradientParameterAt()` measures the sample's
    // angle relative to p0->p1 and wraps. A unit vector keeps it non-degenerate
    // for any bounds, including a zero-area one.
    g.x1 = static_cast<float>(cx + ca);
    g.y1 = static_cast<float>(cy + sa);
    return g;
  }

  if (placement.kind == GradientKind::Radial) {
    // Half the DIAGONAL, so the ramp reaches the corners rather than stopping
    // at the edge midpoints. A reading, not a measurement -- see the header.
    const double radius = std::sqrt(w * w + h * h) * 0.5 * scale;
    g.x1 = static_cast<float>(cx + radius);
    g.y1 = static_cast<float>(cy);
    return g;
  }

  // Linear, and Reflected (which is Linear with a Reflect spread). The length
  // is the bounds projected onto the ramp direction: the span a ramp needs to
  // cross the shape at that angle.
  const double length = (std::fabs(w * ca) + std::fabs(h * std::sin(a))) * scale;
  if (placement.spread == GradientSpread::Reflect) {
    // Reflected runs from the CENTRE outward and mirrors, so p0 is the centre
    // and the ramp is half as long.
    g.x1 = static_cast<float>(cx + ca * length * 0.5);
    g.y1 = static_cast<float>(cy + sa * length * 0.5);
    return g;
  }
  g.x0 = static_cast<float>(cx - ca * length * 0.5);
  g.y0 = static_cast<float>(cy - sa * length * 0.5);
  g.x1 = static_cast<float>(cx + ca * length * 0.5);
  g.y1 = static_cast<float>(cy + sa * length * 0.5);
  return g;
}

}  // namespace np
