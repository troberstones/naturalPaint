#pragma once

#include <cstddef>
#include <string>
#include <utility>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/LayerOps.hpp"

// core/LayerOpRefusal -- the five helpers that build a `LayerOpResult`.
//
// ==========================================================================
// §1  Why this file exists at all
// ==========================================================================
//
// `fail`, `succeed`, `describe`, `inRange` and `notLocked` were written twice,
// once in core/LayerOps.cpp's anonymous namespace and once in core/Merge.cpp's.
// core/Merge.cpp said so in a comment and gave the reason: the helpers have
// internal linkage in core/LayerOps.cpp, so sharing them means giving them
// external linkage somewhere, and that was "not made in the same step that
// introduces the second caller". That step is over; this is the promotion it
// deferred, and the second copy is deleted rather than kept in sync by eye.
//
// The reason to bother is not the thirty lines. It is that the two copies had
// **already drifted** -- see §3 -- in exactly the way two hand-synchronised
// copies of a user-visible sentence drift, which is silently and in the half
// nobody re-reads.
//
// ==========================================================================
// §2  Why a header of its own, and not core/LayerOps.hpp
// ==========================================================================
//
// core/Merge.cpp's own note proposed "promoting three of them to core/LayerOps'
// public surface". That is the wrong home, for two reasons that are worth
// stating because the proposal is the obvious one:
//
//   * core/LayerOps.hpp is the **operation** index -- addLayer, removeLayer,
//     moveLayer, and the paragraph that defines what `locked` means. A caller
//     that includes it wants to edit a layer, not to construct the result type
//     by hand. `fail()` and `succeed()` are how the *implementations* spell
//     their return statement; nothing outside src/core/ has any business
//     calling them, and putting them in the operation header advertises them
//     to every UI translation unit that wants `setLayerVisible`.
//
//   * **There is one namespace, `np`** (PLAN.md; core/ is a directory grouping,
//     never a C++ namespace). So the moment these stop being anonymous, the
//     names `fail`, `succeed` and `describe` are global to the whole binary.
//     core/LayerCompOps.cpp still has its *own* anonymous-namespace `fail` and
//     `succeed` -- a third copy, left alone here because deduplicating it is a
//     different file and a different step -- and it reaches core/LayerOps.hpp
//     through core/LayerCompOps.hpp. An anonymous namespace's members are found
//     by ordinary unqualified lookup in the *enclosing* namespace, so an
//     `np::fail` and a file-local `fail` are two candidates in one lookup set
//     and every call site in that file stops compiling: "call to 'fail' is
//     ambiguous". That is compiled and confirmed, not predicted -- and it would
//     be a compile error in a file this change is not supposed to touch. (It is
//     not an ADL effect, which is the plausible-sounding wrong explanation:
//     `fail(std::string)` has no np-associated argument at all.)
//
// Both problems go away with a header only the implementations include, and
// names that say which family they belong to. Hence the `layerOp` prefix: it is
// not decoration, it is what a short name has to become when internal linkage
// stops hiding it. The prefix is also the thing that lets core/LayerCompOps.cpp
// adopt these later without a rename cascade.
//
// ==========================================================================
// §3  The drift, and which behaviour survived
// ==========================================================================
//
// Four of the five were byte-identical. `notLocked` was **not**: the two copies
// refused with different second sentences.
//
//   core/LayerOps.cpp:  "A locked layer's content and its own place in the
//                        stack are frozen -- only its visibility and the lock
//                        itself can still be changed (core/LayerOps.hpp).
//                        Unlock it first."
//   core/Merge.cpp:     "A merge destroys the layers it merges, which is
//                        exactly the edit a lock exists to refuse
//                        (core/LayerOps.hpp). Unlock it first."
//
// Neither is wrong, and picking one would have made a refusal message worse for
// half its callers -- the merge sentence says the thing a user who just hit
// "Merge Down" needs to hear, and the general sentence says the thing a user
// who just hit "Rename" needs to hear. So **both survive**, as the `why`
// argument, which is required rather than defaulted. A default would have
// re-created the drift with better manners: the merge call sites would have
// picked up the general sentence by omission, and nothing would have said so.
//
// `kLockedLayerFrozen` is the general rule, quoted from core/LayerOps.hpp's own
// definition of `locked`; `kLockedMergeDestroys` is core/Merge's. A new caller
// picks one or writes a third, and has to look at both to do it.

namespace np {

// --- The two `notLocked` reasons ------------------------------------------
//
// The sentence that follows "<op> refused: layer 2 (\"Line pass\") is locked."
// Kept as named constants rather than string literals at the call sites so the
// thirteen core/LayerOps call sites cannot drift apart from each other the way
// the two files did.

inline constexpr const char* kLockedLayerFrozen =
    "A locked layer's content and its own place in the stack are frozen "
    "-- only its visibility and the lock itself can still be changed (core/LayerOps.hpp). "
    "Unlock it first.";

inline constexpr const char* kLockedMergeDestroys =
    "A merge destroys the layers it merges, which is exactly the edit a "
    "lock exists to refuse (core/LayerOps.hpp). Unlock it first.";

inline LayerOpResult layerOpFail(std::string message) {
  LayerOpResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

inline LayerOpResult layerOpSucceed(std::string label, size_t index) {
  LayerOpResult r;
  r.ok = true;
  r.editLabel = std::move(label);
  r.index = index;
  return r;
}

// "layer 2 (\"Line pass\")" -- how every refusal and every edit label names a
// layer. The index is always present because it is the only thing guaranteed to
// identify the layer (names are not unique and may be empty).
inline std::string layerOpDescribe(const Document& doc, size_t index) {
  std::string s = "layer " + std::to_string(index);
  if (index < doc.layers.size() && !doc.layers[index].name.empty())
    s += " (\"" + doc.layers[index].name + "\")";
  return s;
}

// The one bounds check, so every operation refuses an out-of-range index with
// the same sentence rather than five slightly different ones.
inline bool layerOpInRange(const Document& doc, size_t index, const char* what,
                           LayerOpResult* out) {
  if (index < doc.layers.size()) return true;
  *out = layerOpFail(std::string(what) + " refused: there is no layer at index " +
                     std::to_string(index) + " -- this document has " +
                     std::to_string(doc.layers.size()) +
                     " layer(s), indexed from 0 at the bottom of the stack.");
  return false;
}

// The lock refusal, in one place. `what` is the operation's own noun, so the
// message reads "remove layer refused: ..."; `why` is the second sentence, and
// §3 above is why it is a parameter and why it has no default.
inline bool layerOpNotLocked(const Document& doc, size_t index, const char* what,
                             const char* why, LayerOpResult* out) {
  if (!doc.layers[index].locked) return true;
  *out = layerOpFail(std::string(what) + " refused: " + layerOpDescribe(doc, index) +
                     " is locked. " + why);
  return false;
}

}  // namespace np
