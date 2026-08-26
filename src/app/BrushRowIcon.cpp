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
  // **Cleared for the identical reason.** `BrushRow` (app/BrushLibraryFile.hpp
  // §4) is deliberately the CHEAP half of a preset -- seven scalars, no
  // bitmap, matching how it already carries no links -- so a row for a
  // procedural brush must not preview with whatever sampled tip happens to be
  // loaded on `live` right now. Left unset, this row's icon shows the round
  // procedural tip until the library is actually picked and `presetIndex`
  // stops being `kNoPresetIndex`, which is the same "not loaded yet" honesty
  // the links clear above already states for dynamics.
  as.tipBitmap.reset();
  // Identical reasoning, for the identical field shape: a `BrushRow` carries
  // no Dual Brush second tip either (brush/Library.hpp's own comment on
  // `BrushPreset::dualTip`), so this row's icon must not preview whatever
  // dual tip happens to be loaded on the live brush right now.
  as.dualTip.reset();
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
