#include "app/BrushRowIcon.hpp"

#include "app/StrokeSession.hpp"

namespace np {

std::array<BrushTip, kDabPreviewCells> brushRowIconTips(const BrushRow& row,
                                                        const BrushState& live,
                                                        const MixboxLut& lut) {
  // A copy of the live brush with the row's geometry written over it, put
  // through `dabPreviewTipsFor()` -- so the icon and the BRUSH EDITOR's own
  // preview run the identical code and cannot disagree about what a brush
  // looks like. Building a `BrushTip` here by hand would be a second copy of
  // `brushTipFor()`'s load-to-flow mapping, which is precisely the drift
  // app/DabPreview §1 exists to prevent.
  BrushState as = live;
  as.radius = row.radius;
  as.hardness = row.hardness;
  as.roundness = row.roundness;
  as.angle = row.angle;
  as.spacing = row.spacing;
  as.load = row.load;
  // **Emptied, not left as the live brush's.** A row carries no links, and
  // borrowing the current brush's matrix would draw a picture of dynamics this
  // brush does not have -- an icon that changes when you edit an unrelated
  // brush. With no links `evaluateLinks()` returns the identity for every
  // target, so all three pressure cells come out identical: the pressure
  // family collapsed to a point, which is what "not loaded yet" looks like.
  as.links = BrushLinkSet{};
  DynamicInputs in;
  return dabPreviewTipsFor(as, lut, in);
}

std::array<BrushTip, kDabPreviewCells> brushPresetIconTips(const BrushPreset& preset,
                                                           const BrushState& live,
                                                           const MixboxLut& lut) {
  BrushState as = live;
  applyPresetToBrush(preset, as);
  DynamicInputs in;
  return dabPreviewTipsFor(as, lut, in);
}

}  // namespace np
