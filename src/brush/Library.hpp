#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "brush/BrushModel.hpp"
#include "brush/Deposit.hpp"
#include "brush/Dynamics.hpp"

namespace np {

// The brush library: named brushes you can pick, and the rule for when the one
// you are painting with has drifted from the one you picked.
//
// This is what the BRUSH LIBRARY pane lists and what the BRUSH EDITOR panel
// (design "naturalPaint Panels" turn 4a) edits. The two are deliberately
// separate panes: choosing a brush and authoring one are different acts, done
// at different rates, and the design's own editor is a full 322 px column of
// controls that would bury a list it shared a pane with.

// **A preset holds what makes a brush a brush, and nothing else.**
//
// Notably absent: the pigment. `BrushState::pigment` is the *loaded* colour --
// the editor's own section calls it LOADED PIGMENT, not the brush's pigment --
// and a library entry that restored a colour would mean picking a brush
// silently repaints in whatever colour it was saved with. Also absent:
// `BrushState::tool` (which tool is selected is not a property of a brush) and
// the editor's own cell selection (UI state).
struct BrushPreset {
  std::string name;

  // Which imported `.abr` library this preset came from, or 0 for the four
  // built-ins and for anything the user made with Duplicate.
  //
  // **An id, not an index and not a path**, and both halves of that are load
  // bearing (app/BrushLibraryFile.hpp §4):
  //
  //   * Not an index into the loaded-library list, because unloading a library
  //     shifts every later index down by one -- so the presets of the library
  //     *after* the one that was unloaded would silently start claiming to
  //     belong to the one before it, and the next unload would delete the
  //     wrong brushes. An id is minted once per import and never reused within
  //     a session, so nothing can be mistaken for anything else.
  //   * Not the source path, which is 60-200 bytes repeated across every
  //     preset of a pack, has to be rewritten on every relocation, and would
  //     make two libraries imported from the same file indistinguishable. The
  //     path lives once, on the library record.
  //
  // **0 for a Duplicate is deliberate, not an oversight.** `presetFromBrush()`
  // leaves this at its default, so duplicating an imported brush produces a
  // preset the user owns -- and unloading the library it was copied from does
  // not take the copy with it. That is the only way to keep a brush from a
  // pack you are about to remove, and --selftest asserts it.
  //
  // Deliberately NOT compared by `presetMatches()` below: where a preset came
  // from is not part of what makes a brush that brush, and the EDITED badge
  // must not trip because of it.
  uint32_t libraryId = 0;

  // **True for exactly the four presets `defaultBrushLibrary()` constructs
  // below, and for nothing else.** app/UserBrushLibrary.hpp needs to tell a
  // shipped default apart from a preset the user made -- both carry
  // `libraryId == 0`, so that field alone cannot answer "is this one of mine
  // to overwrite, or one of the app's to fork from" (PRD G6's Save button).
  // Not persisted anywhere: it is set here, at construction, by the one
  // function that builds the four, and every other constructor path
  // (`presetFromBrush()`, a `.abr` import, a user-presets.txt read) leaves it
  // at its default `false`, which is the answer every one of those paths
  // needs. Deliberately absent from `presetMatches()`'s comparison, same as
  // `libraryId` above: provenance is not part of what makes a brush that
  // brush, and the EDITED badge must not trip because of it.
  bool builtin = false;

  // Radius/hardness/spacing/roundness/angle used to live here as their own
  // five scalars, read by `app/StrokeSession::brushTipFor()` alongside
  // `links` below. They are gone: `model` (below) is now authoritative for
  // all five -- `model.tip.diameterPx / 2.0f` for radius,
  // `model.tip.hardness` for hardness, `model.tip.spacingPercent / 100.0f`
  // for spacing, `model.tip.roundness` for roundness, `model.tip.angleDeg`
  // for angle -- and every reader of the old fields was moved to read the
  // model equivalent instead, mechanically, at the same relocation
  // `tipBitmap`/`dualTip`/`grain` below already went through when THEY
  // gained a durable home. `load` and `wetness` stay: naturalPaint's own two
  // concepts, not Photoshop's, with no equivalent in `BrushModel`.
  float load = 0.9f;
  float wetness = 1.3f;
  BrushLinkSet links;

  // Mirrors `BrushTip::scatterBothAxes` -- Photoshop's Scatter panel "Both
  // Axes" checkbox, off by default (docs/reachability-audit.md B5). Carried
  // here so an imported or hand-saved brush keeps it; `app/StrokeSession`'s
  // `applyPresetToBrush()`/`presetFromBrush()` copy it in lockstep with every
  // field above, the same way `tipBitmap` below moves only in lockstep with
  // picking a whole preset -- there is no independent slider for it yet, so
  // it is deliberately absent from `presetMatches()`'s comparison below, for
  // that field's own stated reason.
  bool scatterBothAxes = false;

  // A `.abr` sampled bitmap tip (brush/Deposit.hpp §2c), or null for the
  // procedural round/elliptical tip every built-in and every hand-authored
  // preset has always had. Set once, by `io/AbrBrushes.cpp`, from the `samp`
  // block's decoded pixels; never built anywhere else.
  //
  // **Not persisted by `app/UserBrushLibraryStore`, deliberately, and this is
  // the same call app/BrushLibraryFile.hpp §4 already made for a preset's
  // OTHER expensive-to-derive half.** That header's row cache stores a
  // preset's seven scalars and rasterises an icon from them on demand,
  // explicitly leaving `BrushLinkSet` -- "the expensive half... the half a
  // library row does not draw" -- to be re-read from the `.abr` the next time
  // it is actually picked. A decoded bitmap is the same shape of cost for the
  // same reason: it is `.abr`-derived, it can be kilobytes per brush, and it
  // is reproducible for free by reading the file again, which is exactly what
  // `useLibrary()` already does once per session. Threading it through
  // `UserBrushLibraryStore`'s line-based `scalars`/`link`/`point` format --
  // itself frozen positional fields, `app/UserBrushLibrary.hpp` §1 -- would
  // mean inventing binary-blob-in-a-text-file framing for state that already
  // has a durable, canonical home: the `.abr` file on disk.
  //
  // **The one case this does not cover, stated rather than left to be
  // discovered:** `Duplicate` on a sampled-tip preset copies this pointer
  // (`presetFromBrush()`), so the duplicate previews and paints with the same
  // bitmap right up until `Save`. `UserBrushLibraryStore::serialize()` then
  // writes the duplicate's seven scalars and links only -- there is no slot
  // for a bitmap in that format, by the design above -- so a saved duplicate
  // of a sampled-tip brush reloads next launch as the round procedural tip.
  // That is a real gap and not a subtle one, but it is a **narrower** one than
  // it looks: it costs a re-Duplicate from the still-loaded library, and it
  // only bites a preset the user chose to fork in the first place, versus
  // every session paying to keep every imported library's bitmaps resident
  // for brushes that may never be picked. Closing it is future work -- most
  // plausibly a fourth `UserBrushLibraryStore` line naming the source `.abr`
  // and the sample id to re-resolve on load, which is a real feature with its
  // own failure mode (the source file moved or was edited) and was scoped out
  // of this step rather than built in a hurry.
  //
  // **That gap is now closed, and not the way the paragraph above guessed.**
  // Re-resolving from the source `.abr` would inherit that file's whole
  // lifetime -- moved, edited, on a volume that is not mounted. Instead each
  // imported tip is written once to `dabs-imported/<uuid>.png`
  // (app/DabLibrary's `extractAbrTips()`) and `dabId` below names it, so the
  // bitmap stops depending on the pack at all. The paragraph is kept rather
  // than rewritten because its accounting of the cost -- why a bitmap is not
  // in `user-presets.txt` and why keeping every library's tips resident was
  // refused -- is still exactly why the answer is a file in a folder.
  std::shared_ptr<const BrushTipBitmap> tipBitmap;

  // The dab-library id this preset's tip came from: `abr:<uuid>` for an
  // extracted Photoshop tip, `file:`/`gbr:`/`gih:` for one out of the user's
  // own folder (app/DabLibrary.hpp §3), empty for a procedural tip.
  //
  // **This, and not `tipBitmap`, is what `user-presets.txt` persists.** It is
  // one short line of text pointing at a file with a durable home, which is
  // what makes Duplicate -> Save -> relaunch keep the bitmap and what makes
  // unloading the source library stop taking it away. The pointer above stays
  // the thing everything downstream reads, resolved from this id on load.
  //
  // An id that no longer resolves -- the user deleted the PNG -- leaves
  // `tipBitmap` null and the brush falls back to its procedural tip, which is
  // exactly what it does today for a preset that never had one. A missing
  // file is a visibly different brush, not a crash and not an empty one.
  std::string dabId;

  // A Dual Brush's second tip (brush/Deposit.hpp §2d) -- Photoshop's own
  // second tip, stamped through this one and combined by `dualBlend`. Null
  // for every built-in, every hand-authored preset, and any `.abr` brush
  // whose Dual Brush is off or whose blend mode this build does not
  // composite (`io/AbrBrushes.hpp`'s `dualBrushes`/`dualBrushUnsupportedBlend`
  // counters).
  //
  // Threaded exactly like `tipBitmap` above -- set once by `io/AbrBrushes.cpp`,
  // carried by pointer through `applyPresetToBrush()`/`presetFromBrush()`/
  // `brushTipFor()`, never deep-copied -- and not persisted by
  // `UserBrushLibraryStore` for the identical reason: `.abr`-derived,
  // reproducible for free by re-reading the file, and a duplicate of a
  // dual-brush preset therefore reloads next launch with its second tip gone,
  // exactly as a duplicate sampled-tip preset reloads with its bitmap gone.
  std::shared_ptr<const BrushTip> dualTip;
  // Only meaningful when `dualTip` is set.
  DualBrushBlend dualBlend = DualBrushBlend::Multiply;

  // Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp). Unlike `tipBitmap`
  // and `dualTip` above, this DOES have its own slider (the BRUSH EDITOR's
  // PAPER GRAIN section) and is therefore compared by `presetMatches()`
  // below, the same as `radius`/`hardness`/every other field a control moves
  // independently -- and persisted by `app/UserBrushLibraryStore`, as its own
  // `grain` line rather than as a fourth `scalars` count: growing that line's
  // required field count would make an OLDER `user-presets.txt` (seven
  // floats, no eighth) fail to parse at all under `takeFloats()`'s
  // exact-count contract, dropping the whole preset it belongs to. A new,
  // separate keyword an older build does not recognise is instead preserved
  // verbatim by that parser's own forward-compatible "a key this version does
  // not know" branch -- no code needed here to earn that.
  GrainParams grain;

  // Photoshop's own Brush Settings panel, in its own shape (brush/BrushModel.hpp),
  // carried beside the fourteen scalars/pointers above rather than folded into
  // them.
  //
  // **Why a whole second copy of the brush lives here.** `io/AbrBrushes.cpp`
  // decodes every one of the ~117 fields Photoshop writes -- Texture, Transfer,
  // Scatter Count, the Dual Brush's own cadence, the tool options -- and until
  // this field existed that decode was thrown away the moment `importAbrBrushes()`
  // returned: `AbrImportResult::models` was a vector parallel to `presets` that
  // died with the import call, and the only thing that ever read a `BrushModel`
  // was `--abr-report`'s table. Everything that actually painted, `applyPresetToBrush()`
  // below, worked from the lossy 14-scalar projection alone. The cost of that
  // was invisible until Duplicate: `presetFromBrush()` had nowhere to WRITE a
  // model, because `BrushState` had none either, so duplicating an imported
  // preset silently discarded its texture pattern, its transfer curves, its
  // blend mode, its dual tip's own scatter -- everything this struct's other
  // fields have no room for. This field is that room.
  //
  // **Deliberately inert for now.** Nothing reads `model` to paint yet --
  // `brushTipFor()` and the stroke engine still work exclusively from
  // `radius`/`hardness`/`links`/`grain`/etc above, so this commit changes no
  // pixel. It exists so the model has somewhere durable to live WHILE it is
  // carried in lockstep with every other field by `applyPresetToBrush()` and
  // `presetFromBrush()` (app/StrokeSession.cpp) -- the same lockstep discipline
  // `tipBitmap` and `dualTip` above already follow, and for the identical
  // reason: a field only one of the two directions copies is a field
  // Duplicate quietly drops.
  //
  // Not yet compared by `presetMatches()`, and not yet persisted by
  // `app/UserBrushLibraryStore` -- both are the next step, not this one. A
  // model with no reader has no independent control that could disagree with
  // the fourteen scalars above the way `grain`'s slider can, so leaving it out
  // of the EDITED-badge comparison cannot yet produce a wrong badge; that
  // reasoning stops holding the moment something in `ui/` gets its own control
  // over a `BrushModel` field, at which point this paragraph needs revisiting
  // exactly as `grain`'s own comment above describes for itself.
  BrushModel model;
};

struct BrushLibrary {
  std::vector<BrushPreset> presets;
  // Which preset the brush was last loaded from. It is an index into
  // `presets`, and it survives editing: the editor's EDITED badge means "the
  // live brush no longer matches presets[active]", which needs the index to
  // still point at what was picked.
  size_t active = 0;
};

// The brushes a fresh install starts with.
//
// Four rather than one, and each one differs in something the DYNAMICS matrix
// can show: the point of shipping a library at all is that opening a second
// brush teaches what the first one's links were doing. A single default would
// leave the matrix looking like decoration.
BrushLibrary defaultBrushLibrary();

// Whether `preset` describes the brush these values are. Used for the EDITED
// badge, so it compares exactly the fields a preset carries -- a brush whose
// only difference is its loaded pigment is NOT edited, because the pigment is
// not part of the brush.
//
// Float equality rather than a tolerance, deliberately. Every one of these
// values arrives from a slider or from `applyPreset()`, so two that should be
// equal are bit-equal; a tolerance would make a brush nudged by less than the
// tolerance read as unedited and lose the change on the next pick.
//
// **Takes no `tipBitmap` parameter, and that is not an oversight.** Every
// path that can change `BrushState::tipBitmap` -- `applyPresetToBrush()`,
// `presetFromBrush()` -- copies it in lockstep with every field this function
// already compares, and there is no slider or independent mutator that can
// move it on its own (unlike, say, `radius`, which a slider changes without
// touching anything else). So the eight fields already checked can never
// agree while `tipBitmap` disagrees, for as long as that stays true; adding a
// ninth parameter here would touch every call site for a comparison that
// cannot currently fail differently from the ones already made. Revisit if a
// future control ever lets a bitmap tip be swapped independently of picking a
// whole preset.
//
// `dualTip`/`dualBlend` are excluded for the identical reason and by the
// identical argument: both move only in lockstep with a whole preset, so the
// eight fields already checked can never agree while either of those two
// disagree.
//
// **`grain` IS a parameter here, unlike `tipBitmap`/`dualTip` above, and for
// the mirror-image reason: it has its own control** (the BRUSH EDITOR's
// PAPER GRAIN section, `ui/MacPaintUI.cpp`) that moves it independently of
// picking a whole preset, exactly as `radius` and every scalar already
// checked does. Leaving it out would mean dragging the GRAIN slider alone
// left the preset header lying that nothing had changed -- the identical
// failure this function's own header paragraph exists to prevent for every
// other independently-driven field.
bool presetMatches(const BrushPreset& preset, float radius, float hardness, float spacing,
                   float roundness, float angle, float load, float wetness,
                   const BrushLinkSet& links, const GrainParams& grain);

// Whether two link sets describe the same relationships. Order-insensitive:
// the set is a flat vector, but a matrix cell is a cell, so two sets holding
// the same links in a different order are the same configuration and must not
// read as edited.
bool linkSetsEqual(const BrushLinkSet& a, const BrushLinkSet& b);

// A name no existing preset has, derived from `wanted` by appending a counter.
// Duplicated names are not refused -- a library that rejects "Round Bristle 03"
// because one exists is a library that makes you invent names -- but they are
// made distinct, because the pane lists by name and two identical rows cannot
// be told apart.
std::string uniquePresetName(const BrushLibrary& lib, const std::string& wanted);

}  // namespace np
