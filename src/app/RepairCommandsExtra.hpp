#pragma once

#include <cstdint>

#include "app/Command.hpp"
#include "app/FilterOps.hpp"  // ContentAwareFillRequest, SeamHealRequest

// app/RepairCommandsExtra -- encoders for the repair dialogs
// (Content-Aware Fill, Seam Heal), following app/FilterCommandsExtra.hpp's
// own precedent: one small header for a dialog's encoder, implemented beside
// the reader that validates its keys (app/CommandsImage.cpp), so this header
// carries no logic of its own.
namespace np {

Command contentAwareFillCommand(const ContentAwareFillRequest& p);
Command seamHealCommand(const SeamHealRequest& p);

// The dialogs' "Recompute" button: a new seed, deterministically, from the
// one shown. **Bounded by MODULO, not by masking off the high bits** --
// `readSeed()` (app/CommandsImage.cpp, `filter_add_noise`'s own reasoning)
// refuses a seed above 2^53 because a JSON number is a double, exact only up
// to there. A mask (`& (2^53 - 1)`) would have thrown away only the top 11
// bits of a splitmix64 avalanche and left the low bits exactly as mixed;
// `% (2^53 + 1)` folds the WHOLE 64-bit avalanche back into range instead,
// which is what makes consecutive presses keep looking unrelated rather than
// merely losing their top few bits.
//
// **Cannot get stuck at a fixed point in practice, argued from the
// arithmetic rather than merely observed.** splitmix64's raw 64-bit output
// is at or below 2^53 for only 1 in 2^11 (2048) inputs -- 2^64 / (2^53+1) --
// so for the sequence this function actually walks, "the reduction changed
// nothing" is a ~1-in-2048 event each step, not the common case, and two
// CONSECUTIVE outputs colliding (the only way the sequence could visibly
// repeat) is a second, independent draw from that same small chance.
// app/selftest/PatchMatch.cpp's own section confirms this arithmetic rather
// than assuming it: for the seeds the suite actually starts from, the RAW
// splitmix64 step is checked to exceed 2^53 first, so the modulo is proven
// to be doing real work on the inputs the test then walks forward from.
uint64_t nextRepairSeed(uint64_t seed) noexcept;

}  // namespace np
