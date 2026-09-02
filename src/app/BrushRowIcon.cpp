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
  // `BrushState`'s own radius/hardness/roundness/angle/spacing are gone
  // (Part 5) -- `as.model.tip.*` is what `brushTipFor()` actually reads now,
  // so the row's geometry has to land there instead. `BrushRow`'s own seven
  // scalars are untouched by that deletion (a separate, cheap struct,
  // app/BrushLibraryFile.hpp §4) -- only the destination changed.
  //
  // **`as.model` is reset to a plain default first, not left as `live`'s.**
  // The identical reasoning `as.links = BrushLinkSet{}` below already states
  // for the old link matrix: a row carries no Variance data of its own
  // (Size/Angle/Roundness/Scatter jitter included), and inheriting the live
  // brush's would draw an icon that changes when an unrelated brush is
  // edited -- the same "not loaded yet" identity the links clear used to be
  // the whole story for, now restated for the field that actually drives a
  // dab's per-site jitter.
  as.model = BrushModel{};
  as.model.tip.diameterPx = row.radius * 2.0f;
  as.model.tip.hardness = row.hardness;
  as.model.tip.roundness = row.roundness;
  as.model.tip.angleDeg = row.angle;
  // `row.spacing` is RADII (`app/BrushLibraryFile.cpp`'s `brushRowFor()`, and
  // the old, now-deleted `BrushPreset::spacing` scalar's own unit) --
  // `spacingPercent` is a percentage OF THE DIAMETER, so `* 50` is the
  // conversion, not a bare `* 100` (`app/StrokeSession::brushTipFor()`'s
  // `tip.spacing` comment names the same factor of two).
  as.model.tip.spacingPercent = row.spacing * 50.0f;
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
