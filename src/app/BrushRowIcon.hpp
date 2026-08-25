#pragma once

#include <array>

#include "app/BrushLibraryFile.hpp"
#include "app/DabPreview.hpp"

// app/BrushRowIcon -- **one library row, as three rasterisable dabs.**
//
// The whole rationale is `app/BrushLibraryFile.hpp` §4 ("Lazy: what a row
// needs, and the tension with the icon"), and it is not repeated here. The
// short form: the cache stores the seven numbers an icon needs rather than the
// icon, so a row draws with its `.abr` unread and there are no pixels on disk
// to go stale.
//
// **This is a separate header purely for a dependency reason**, and it is
// worth saying so rather than leaving it to be inferred: `app/AppState.hpp`
// holds a `BrushLibraryStore`, and `app/DabPreview.hpp` needs `BrushState`,
// which lives in `app/AppState.hpp`. Declaring these two functions in
// `app/BrushLibraryFile.hpp` closes that cycle. Two headers is the cheapest
// honest fix; the alternatives were holding the store behind a pointer (a heap
// allocation in the *Idle* state, which PRD A2 measures) or moving
// `BrushState` out of `AppState.hpp`, which is a refactor of a file three
// other things are editing.
//
// The split it happens to produce is a real one anyway: below this line is a
// rendering concern, above it is a file and a cache.
namespace np {

// The three tips a row's icon rasterises through `rasteriseDabPreview()`.
//
// `live` supplies the pigment and nothing else: a preset carries no colour
// (brush/Library.hpp is explicit that it must not), and a preview in an
// invented colour would be a picture of a brush the user does not have. The
// colour loaded right now is the colour this brush would actually paint, which
// is the only honest one available.
//
// **All three cells come out identical**, because a row has no links and so
// nothing varies with pressure. That is app/DabPreview §2's pressure family
// collapsing to a point, and it is the visible form of "the dynamics have not
// been read yet" -- which the pane says in words beside it rather than leaving
// to be read off three matching pictures.
std::array<BrushTip, kDabPreviewCells> brushRowIconTips(const BrushRow& row,
                                                        const BrushState& live,
                                                        const MixboxLut& lut);

// The same, for a preset that *is* loaded -- the real family, through the same
// `dabPreviewTipsFor()` the BRUSH EDITOR's own preview uses, so a row's icon
// and the editor's preview cannot disagree about what a brush looks like.
std::array<BrushTip, kDabPreviewCells> brushPresetIconTips(const BrushPreset& preset,
                                                           const BrushState& live,
                                                           const MixboxLut& lut);

}  // namespace np
