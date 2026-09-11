#include "app/selftest/Support.hpp"

#include "io/PsdBlendKeys.hpp"

namespace np {

// ---------------------------------------------------------------------------
// io/PsdBlendKeys -- the one Photoshop-blend-key table, now read in BOTH
// directions (PLAN.md phase 15's PSD writer needs mode -> key; io/PsdImport
// has always needed key -> mode). See app/SelfTest.hpp for this section's
// contents list and io/PsdBlendKeys.hpp for why the table is shared.
// ---------------------------------------------------------------------------
bool runPsdBlendKeysTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto key4 = [](const char* s) {
    return std::array<char, 4>{s[0], s[1], s[2], s[3]};
  };

  const std::span<const PsdBlendKeyEntry> table = psdBlendKeys();

  // --- The table's own shape ---------------------------------------------
  {
    check(table.size() == 26,
          "table: twenty-six PSD keys -- every core::BlendMode except `Mix`, which "
          "Photoshop has no concept of");

    // Both directions are a linear scan of these rows, so a duplicated mode
    // would make the reverse lookup silently pick whichever row came first
    // and a duplicated key would make one row dead. Neither is visible by
    // reading the table.
    bool keysUnique = true, modesUnique = true;
    for (size_t i = 0; i < table.size(); ++i) {
      for (size_t j = i + 1; j < table.size(); ++j) {
        if (std::memcmp(table[i].psdKey, table[j].psdKey, 4) == 0) keysUnique = false;
        if (table[i].mode == table[j].mode) modesUnique = false;
      }
    }
    check(keysUnique && modesUnique,
          "table: every key and every mode appears exactly once, so neither "
          "direction of the lookup is ambiguous");
  }

  // --- Four bytes, and the pad byte is a SPACE and not a NUL --------------
  //
  // Photoshop pads a 3-character key with 0x20. A key written as three
  // characters and a NUL terminator compiles, reads correctly in a diff, and
  // fails every 4-byte comparison at run time with no error anywhere -- the
  // reader's `fourccEquals()` and the writer's `fourcc()` both take all four
  // bytes. So the byte itself is asserted, not the literal's appearance.
  {
    bool fourBytes = true, padIsSpace = true;
    size_t padded = 0;
    for (const PsdBlendKeyEntry& e : table) {
      if (std::strlen(e.psdKey) != 4) fourBytes = false;
      for (int i = 0; i < 4; ++i) {
        if (e.psdKey[i] == '\0') padIsSpace = false;
      }
      if (e.psdKey[3] == ' ') ++padded;
    }
    check(fourBytes, "keys: every key is exactly four bytes long");
    check(padIsSpace,
          "keys: no key contains a NUL -- Photoshop pads a short key with a SPACE "
          "(0x20), and a NUL would silently never match");
    // `mul `, `div `, `hue `, `sat `, `lum ` -- named here rather than
    // counted somewhere else, because five is the kind of number a merge can
    // move without moving the rows.
    check(padded == 5,
          "keys: exactly five keys are space-padded (`mul `, `div `, `hue `, `sat `, "
          "`lum `)");
    bool namedPadded =
        psdBlendKeyFor(BlendMode::Multiply) != nullptr &&
        std::memcmp(psdBlendKeyFor(BlendMode::Multiply), "mul ", 4) == 0 &&
        std::memcmp(psdBlendKeyFor(BlendMode::ColorDodge), "div ", 4) == 0 &&
        std::memcmp(psdBlendKeyFor(BlendMode::Hue), "hue ", 4) == 0 &&
        std::memcmp(psdBlendKeyFor(BlendMode::Saturation), "sat ", 4) == 0 &&
        std::memcmp(psdBlendKeyFor(BlendMode::Luminosity), "lum ", 4) == 0;
    check(namedPadded,
          "keys: the five space-padded keys are the five expected ones, byte for byte");
  }

  // --- Round trip: key -> mode -> key, on the identical four bytes --------
  {
    bool roundTrips = true;
    for (const PsdBlendKeyEntry& e : table) {
      bool exact = false;
      const BlendMode m = mapBlendKey(key4(e.psdKey), exact);
      if (!exact || m != e.mode) {
        roundTrips = false;
        continue;
      }
      const char* back = psdBlendKeyFor(m);
      if (back == nullptr || std::memcmp(back, e.psdKey, 4) != 0) roundTrips = false;
    }
    check(roundTrips,
          "round trip: every row's key -> mode -> key returns the identical four bytes, "
          "with exactMatch true");
  }

  // --- The tripwire: every BlendMode is triaged --------------------------
  //
  // A blend mode added to core::BlendMode later must land on one side of
  // this line CONSCIOUSLY -- in the table, or in the named list below --
  // rather than silently exporting as Normal. Written as the actual list of
  // unmapped enumerators rather than as a count, because a count is exactly
  // what two branches can independently move to the same wrong number (see
  // app/selftest/ControlsLayout.cpp:88, where that happened to `kAll` and
  // the build caught it only because the list was asserted next to the
  // number).
  {
    // The complete set of core::BlendMode values this build has NO Photoshop
    // key for, and why:
    //
    //  * `Mix` -- this codebase's own Kubelka-Munk pigment latent lerp. Not a
    //    Photoshop mode and not even a per-pixel one (`BlendModeInfo::
    //    compositesPixels` is false for exactly this mode), so there is no
    //    key to write and no near-enough key worth substituting.
    //    docs/blend-mode-gaps.md's own table records it as "no PSD key".
    const BlendMode kNoPsdKey[] = {BlendMode::Mix};
    static_assert(sizeof(kNoPsdKey) / sizeof(kNoPsdKey[0]) == 1,
                  "kNoPsdKey must list every core::BlendMode with no PSD key");

    std::string untriaged;
    bool unmappedAreNull = true;
    for (const BlendModeInfo& info : allBlendModes()) {
      bool named = false;
      for (const BlendMode m : kNoPsdKey)
        if (m == info.mode) named = true;
      const char* k = psdBlendKeyFor(info.mode);
      if (named) {
        // A mode on the "no key" list must actually have no key -- otherwise
        // the list is stale in the other direction and the tripwire passes
        // while describing something untrue.
        if (k != nullptr) unmappedAreNull = false;
      } else if (k == nullptr) {
        if (!untriaged.empty()) untriaged += ", ";
        untriaged += info.name;
      }
    }
    check(untriaged.empty(),
          "tripwire: every core::BlendMode enumerator is either in the key table or "
          "in this file's named kNoPsdKey list -- a new mode cannot export as Normal "
          "by omission");
    check(unmappedAreNull,
          "tripwire: every mode on kNoPsdKey really has no key, so the list cannot go "
          "stale in the other direction");
    check(psdBlendKeyFor(BlendMode::Mix) == nullptr,
          "unmapped: `Mix` returns nullptr, never a substituted key -- the caller warns "
          "by name rather than writing a mode Photoshop would render differently");
    // Table size plus unmapped list must account for the whole enum. Counted
    // from `allBlendModes()` rather than hardcoded, so this cannot drift.
    check(table.size() + (sizeof(kNoPsdKey) / sizeof(kNoPsdKey[0])) == allBlendModes().size(),
          "tripwire: the table and kNoPsdKey together cover every enumerator exactly "
          "once (26 + 1 == 27)");
  }

  // --- An unknown key still falls back exactly as it always did ----------
  {
    bool exact = true;
    const BlendMode m = mapBlendKey(key4("diss"), exact);
    check(m == BlendMode::Normal && !exact,
          "unknown key: `diss` (Dissolve, deliberately out of scope) maps to Normal "
          "with exactMatch FALSE, so the importer warns by name");
    // `pass` is a group-compositing flag, not a blend -- io/PsdImport.cpp
    // handles it before it ever reaches this table, and if one did reach it,
    // Normal-and-warn is the right answer.
    exact = true;
    check(mapBlendKey(key4("pass"), exact) == BlendMode::Normal && !exact,
          "unknown key: `pass` is not a blend key and does not become one here");
    // The failure the trailing-space discipline exists to prevent, asserted
    // from the outside: three characters and a NUL is NOT `mul `.
    exact = true;
    const std::array<char, 4> mulNul = {'m', 'u', 'l', '\0'};
    check(mapBlendKey(mulNul, exact) == BlendMode::Normal && !exact,
          "unknown key: `mul` + NUL does NOT match `mul ` -- the exact silent failure "
          "a three-character key would cause");
  }

  // --- The importer's own behaviour, unchanged by the move ---------------
  //
  // The keys below are the ones io/PsdImport.hpp names as present in the
  // three real Photoshop files that importer is verified against (`colr`,
  // `lddg`), plus one from each of the table's argued groups. Asserted as
  // literal expectations rather than by walking the table, so a row edited
  // in both directions at once still reddens here.
  {
    struct Expect {
      const char* key;
      BlendMode mode;
    };
    const Expect kExpected[] = {
        {"norm", BlendMode::Normal},   {"mul ", BlendMode::Multiply},
        {"scrn", BlendMode::Screen},   {"dark", BlendMode::Min},
        {"lite", BlendMode::Max},      {"lddg", BlendMode::Plus},
        {"diff", BlendMode::Difference}, {"smud", BlendMode::Exclusion},
        {"over", BlendMode::Overlay},  {"sLit", BlendMode::SoftLight},
        {"hLit", BlendMode::HardLight}, {"colr", BlendMode::Color},
        {"lum ", BlendMode::Luminosity}, {"dkCl", BlendMode::DarkerColor},
    };
    bool asBefore = true;
    for (const Expect& e : kExpected) {
      bool exact = false;
      if (mapBlendKey(key4(e.key), exact) != e.mode || !exact) asBefore = false;
    }
    check(asBefore,
          "importer: fourteen real keys -- including `colr` and `lddg` from the three "
          "verified Photoshop files -- map to the same modes as before the table moved");

    // `dark`/`lite` are the two rows io/PsdBlendKeys.hpp calls EXACT matches
    // (min and max are order-preserving, so they commute with any transfer
    // function). Pinned by identity here so a well-meaning edit that
    // "improved" them onto some other mode has to argue with a test.
    check(std::memcmp(psdBlendKeyFor(BlendMode::Min), "dark", 4) == 0 &&
              std::memcmp(psdBlendKeyFor(BlendMode::Max), "lite", 4) == 0,
          "exactness: the two EXACT rows are Min<->`dark` and Max<->`lite`, in both "
          "directions");
  }

  return ok;
}

}  // namespace np
