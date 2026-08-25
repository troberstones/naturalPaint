#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

  float radius = 20.0f;
  float hardness = 0.35f;
  float spacing = 0.25f;
  float roundness = 1.0f;
  float angle = 0.0f;
  float load = 0.9f;
  float wetness = 1.3f;
  BrushLinkSet links;
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
bool presetMatches(const BrushPreset& preset, float radius, float hardness, float spacing,
                   float roundness, float angle, float load, float wetness,
                   const BrushLinkSet& links);

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
