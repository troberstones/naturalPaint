#pragma once

#include "ui/MenuModel.hpp"

// app/CommandCoverage -- the classification that makes the command table's
// exhaustiveness checkable, and makes a NEW menu action fail the BUILD until
// somebody decides which side of the line it is on.
//
// docs/automation-plan.md step 1's stated gate. It could not be written by any
// of the six tracks that filled the table, because each of them could see only
// its own family; it is the one assertion that has to look at every vocabulary
// at once.
//
// ==========================================================================
// (1) An exhaustive switch, not a list
// ==========================================================================
//
// `coverageFor()` is a `switch` over every `MenuAction`, and `-Werror=switch`
// is on (src/CMakeLists.txt:808, which argues at length for exactly this use).
// So adding an enumerator to `ui/MenuModel.hpp` does not merely fail a test --
// it fails to compile until it is classified here. A hand-kept `std::vector`
// of every action would have been a second list to forget to update, which is
// the failure `allLayerCommands()` exists to have fixed once already.
//
// ==========================================================================
// (2) Three categories, and the third one is the honest one
// ==========================================================================
//
// `Registered` and `NotRecordable` would be a tidy pair and a lie. There is a
// third state -- an action that IS a function of an `OpenDocument`, and that
// nobody has written a row for yet -- and folding it into either of the other
// two is how a gap becomes permanent. Folded into `NotRecordable` it acquires
// a fake justification; folded into `Registered` it fails the suite as though
// something were broken rather than merely absent.
//
// So `NotYetRegistered` is its own answer, every one of them carries the
// reason it has not been done, and `--selftest` prints the list and asserts
// its exact size. The list can shrink freely; it cannot GROW without the
// number in that assertion being changed by hand, which is the review this
// kind of gap never otherwise gets.
namespace np {

enum class CommandCoverageKind {
  // A row exists. `commandId` names it, or is null for the two *family*
  // actions whose id depends on their `param` -- `allLayerCommands()` and
  // `allLayerSetCommands()` are walked exhaustively by
  // app/selftest/CommandsLayers.cpp instead.
  Registered,
  // Not a function of an `OpenDocument`, so it is out by app/Command.hpp §1's
  // rule rather than by anyone's judgement. `reason` says which state it needs.
  NotRecordable,
  // A document edit with no row yet. `reason` says what is missing.
  NotYetRegistered,
};

struct CommandCoverage {
  CommandCoverageKind kind = CommandCoverageKind::NotRecordable;
  // Null unless `kind == Registered` and the action is not a family.
  const char* commandId = nullptr;
  // Null when `kind == Registered`; never null otherwise.
  const char* reason = nullptr;
};

CommandCoverage coverageFor(MenuAction action);

}  // namespace np
