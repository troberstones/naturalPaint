#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "core/Blend.hpp"

// io/PsdBlendKeys -- the one table mapping Photoshop's 4-character blend-mode
// keys onto `core::BlendMode`, in BOTH directions.
//
// --- Why this is shared rather than private to the importer ----------------
//
// This table lived in io/PsdImport.cpp's anonymous namespace from the day the
// importer landed, which was correct while a reader was the only thing that
// needed it. PLAN.md phase 15's PSD **export** makes that a problem: a writer
// must emit the same four bytes the reader accepts, and the only two ways to
// get that without sharing the table are to write a second one (a second place
// for `"mul"` to lose its trailing space) or to derive the key from the mode's
// UI label (which would silently invent a key for a mode Photoshop has no name
// for). Both failure modes are invisible in a file that opens: Photoshop reads
// an unrecognised key as Normal without complaining, so a mis-padded key ships
// a document that is confidently, quietly wrong. One table, two directions,
// asserted against itself -- the identical argument io/PackBits.hpp makes for
// owning both the encoder and its inverse.
//
// The reader's behaviour is unchanged by the move: `mapBlendKey()` below is
// the same function, the same rows in the same order, and the same
// "exact-or-Normal-and-warn" contract io/PsdImport.hpp documents. That
// importer is verified layer-for-layer against three real Photoshop files
// (io/PsdImport.hpp names them), so this promotion is a relocation, not a
// rewrite.
//
// --- The trailing spaces are load-bearing ----------------------------------
//
// **Photoshop pads a 3-character key with a SPACE (0x20), never with a NUL.**
// Five rows carry one: `"mul "`, `"div "`, `"hue "`, `"sat "`, `"lum "`. Both
// directions compare or emit all four bytes -- io/PsdImport.cpp's
// `fourccEquals()` is a 4-byte `memcmp`, and io/PsdWrite.hpp's `fourcc()`
// writes exactly four -- so a key written three-and-a-NUL fails to match on
// the way back in, with no error anywhere. app/selftest/PsdBlendKeys.cpp
// asserts the space byte per row rather than trusting the source literal to
// look right, because a NUL and a space look identical in a diff.
//
// --- What "exact" means, and why only two rows are ------------------------
//
// **`dark`->Min and `lite`->Max are EXACT matches; nothing else in the table
// is.** Per-channel minimum and maximum are order-preserving, so they commute
// with any monotone transfer function: Photoshop's Darken/Lighten and this
// codebase's `Min`/`Max` agree pixel-for-pixel regardless of whether the
// comparison happens before or after gamma encoding. Every other row is an
// approximation, and for one shared reason -- **this codebase composites in
// premultiplied LINEAR light (core/Blend.hpp) and Photoshop's default is to
// blend in the document's usually gamma-encoded space** ("Blend RGB Colors
// Using Gamma 1.0" off). Multiplication, addition, and every one of the
// light-family and non-separable formulas fail to commute with that transfer
// function. So the *mode* selection is right in both directions and the
// *arithmetic* is not bit-identical to Photoshop's; that gap is stated, not
// closed, and reading a key or writing one does not change it.
//
// Note that `mapBlendKey()` reports every table row as `exactMatch = true`.
// That flag answers "was this key in the table?", which is what the caller
// needs to decide whether to warn -- it is not a claim about arithmetic. The
// two senses of the word are kept apart here deliberately; there is no third
// bucket between "in the table" and "unknown key, warn and fall back".
//
// --- Why a missing key is nullptr-and-warn, never a substitution ----------
//
// `psdBlendKeyFor()` returns `nullptr` for a `BlendMode` this build has no
// Photoshop key for, and the caller is expected to warn by name and write
// `norm`. It does NOT pick a visually-near key. The rule is the one
// io/AbrBrushes.hpp and io/PsdImport.hpp already hold themselves to for the
// opposite direction: **say what was dropped rather than approximate it.** A
// substituted key produces a file that opens, renders, and is wrong in a way
// the user has no way to notice -- strictly worse than a file that opens with
// a stated omission. As of this writing exactly one mode has no key:
// `BlendMode::Mix`, this codebase's own Kubelka-Munk latent lerp, which
// Photoshop has no concept of at all (docs/blend-mode-gaps.md's own table
// records it as "no PSD key"). `BlendMode::Mix` is also the one mode
// `BlendModeInfo::compositesPixels` is false for, so it could never have been
// written as a per-pixel Photoshop mode anyway.
//
// A blend mode added to `core::BlendMode` later must land on one side of that
// line or the other **consciously**: app/selftest/PsdBlendKeys.cpp asserts
// that every enumerator is either in this table or in an explicit, named
// unmapped list, and states that list out in full rather than checking a
// count -- a count is the thing two branches can independently move to the
// same wrong number.

namespace np {

// One row of the table: the wire key, and the mode it names.
struct PsdBlendKeyEntry {
  const char* psdKey;  // exactly 4 bytes, including a trailing space where
                       // Photoshop pads a short key with one
  BlendMode mode;
};

// The whole table, in file order. Exposed so a test can walk it -- and so a
// writer that wants to enumerate what it can emit does not have to guess.
// Every `mode` appears at most once, which is what makes the reverse lookup
// below unambiguous; app/selftest/PsdBlendKeys.cpp asserts that.
std::span<const PsdBlendKeyEntry> psdBlendKeys() noexcept;

// PSD key -> BlendMode. `exactMatch` is set true when `key` was found in the
// table and false for the "unknown key" fallback, which returns
// `BlendMode::Normal`. `std::nullopt`-free by design: Normal is the answer
// either way, so this is a total function and the bool is what tells the
// caller whether to warn. `norm`'s own row and the fallback deliberately
// produce the same mode.
BlendMode mapBlendKey(const std::array<char, 4>& key, bool& exactMatch) noexcept;

// Returns the 4-character PSD key for a mode, or nullptr when this build has
// no key for it. Never a silent substitution.
const char* psdBlendKeyFor(BlendMode mode) noexcept;

}  // namespace np
