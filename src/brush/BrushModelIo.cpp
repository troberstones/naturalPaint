#include "brush/BrushModelIo.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace np {
namespace {

// Nine significant digits: IEEE-754 binary32's documented round-trip width.
// Copied rather than shared, matching app/UserBrushLibrary.cpp's own note at
// its `f9()` (which cites app/BrushLibraryFile.cpp's): a six-line function is
// not worth a dependency between modules whose lifetimes this project's own
// convention keeps separate.
std::string f9(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return buf;
}

// ---------------------------------------------------------------------------
// One leaf, two directions. Overloaded rather than templated: the set of
// types a BrushModel leaf can be is CLOSED (bool, int32_t, float,
// std::string, and the two ordinal enums), so plain overload resolution on
// the concrete type is the whole dispatch `visitBrushModelFields()`'s generic
// `fn` needs -- and it means a leaf of some NEW type added to BrushModel
// without a matching overload here fails to COMPILE, rather than silently
// picking the nearest template instantiation.
// ---------------------------------------------------------------------------

std::string toFieldString(bool v) { return v ? "1" : "0"; }
std::string toFieldString(int32_t v) { return std::to_string(v); }
std::string toFieldString(float v) { return f9(v); }
std::string toFieldString(const std::string& v) { return v; }
// Ordinals, not names. These are persisted keys -- BrushModelIo.hpp's own
// file-format contract -- and Variance.hpp's own history is the reason why:
// `VarianceControl::Direction`/`InitialDirection` were assigned backwards
// once and shipped through a green suite because they were read off an enum
// that looked orderly. Writing the name would make a future rename of the
// ENUMERATOR silently reinterpret every saved file; writing the ordinal
// means a rename is invisible to this format, as it should be.
std::string toFieldString(VarianceControl v) { return std::to_string(static_cast<int>(v)); }
std::string toFieldString(CoverageBlend v) { return std::to_string(static_cast<int>(v)); }

// Parses the WHOLE of `s` into `out`, or returns false and leaves `out`
// untouched. "Whole" -- not "a prefix of" -- is what turns a numeric field
// with trailing junk (a half-typed hand edit, a stray space) into a rejected
// line instead of a silently truncated value; `brushModelApplyLine()`'s
// promise that a malformed value never partially applies depends on every
// overload below only assigning `out` on the success path.

bool parseField(const std::string& s, bool& out) {
  if (s == "0") { out = false; return true; }
  if (s == "1") { out = true; return true; }
  return false;
}

bool parseField(const std::string& s, int32_t& out) {
  if (s.empty()) return false;  // strtol("", &end) leaves end == s.c_str(), a vacuous "whole parse" of nothing
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end != s.c_str() + s.size()) return false;
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max())
    return false;
  out = static_cast<int32_t>(v);
  return true;
}

bool parseField(const std::string& s, float& out) {
  if (s.empty()) return false;  // same vacuous-parse trap as strtol above
  char* end = nullptr;
  // The C locale, same reasoning as app/UserBrushLibrary.cpp's own
  // takeFloats(): nothing in this application calls setlocale(), so '.' is
  // the decimal point both here and in f9() above.
  const float v = std::strtof(s.c_str(), &end);
  if (end != s.c_str() + s.size()) return false;
  out = v;
  return true;
}

bool parseField(const std::string& s, std::string& out) {
  // A value is "everything after the first space", verbatim -- pattern names
  // carry spaces for real (`ktw watercolor paper 2k17 b` is a shipped one) --
  // but a newline would let one file line masquerade as two, so it is the
  // one character this format still refuses inside a string value.
  if (s.find('\n') != std::string::npos) return false;
  out = s;
  return true;
}

// Range-checked against the FILE's own ordinals (Variance.hpp's comment on
// `VarianceControl`: 0, 2, 3, 5 and 6 are observed across 101 presets; 1, 4
// and 7 are inference, not unused), not against however many enumerators
// this particular build happens to declare -- a build that later adds an
// eighth control must not start accepting an ordinal no importer ever wrote.
bool parseField(const std::string& s, VarianceControl& out) {
  int32_t ord = 0;
  if (!parseField(s, ord)) return false;
  if (ord < 0 || ord > 7) return false;
  out = static_cast<VarianceControl>(ord);
  return true;
}

bool parseField(const std::string& s, CoverageBlend& out) {
  int32_t ord = 0;
  if (!parseField(s, ord)) return false;
  if (ord < 0 || ord > 9) return false;
  out = static_cast<CoverageBlend>(ord);
  return true;
}

}  // namespace

std::vector<std::string> brushModelToLines(const BrushModel& m) {
  // No `const_cast`. `visitBrushModelFields()` deduces its model type, so a
  // const model yields const leaves and a mutable one yields writable leaves
  // from a single template body (brush/BrushModelFields.hpp) -- which is what
  // lets serialising and parsing share one walk. An earlier version of this
  // file owned a mutable-only walk and cast the constness away here to reuse
  // it for reading; the cast was safe but it was a workaround for a fork that
  // no longer exists.
  std::vector<std::string> paths, mineVals;
  visitBrushModelFields(m, [&](const std::string& path, auto& ref) {
    paths.push_back(path);
    mineVals.push_back(toFieldString(ref));
  });

  // The baseline is a FRESH default, walked the same way, rather than a
  // field-by-field `==` against `m` -- `BrushModel` has no `operator==`, and
  // adding one just for this comparison would be a second place the field
  // list has to be kept in sync with `visitBrushModelFields()`.
  BrushModel def;
  std::vector<std::string> defaultVals;
  visitBrushModelFields(def, [&](const std::string&, auto& ref) {
    defaultVals.push_back(toFieldString(ref));
  });

  std::vector<std::string> lines;
  for (size_t i = 0; i < paths.size(); ++i) {
    if (mineVals[i] != defaultVals[i]) lines.push_back(paths[i] + " " + mineVals[i]);
  }
  return lines;
}

bool brushModelApplyLine(BrushModel& m, const std::string& line) {
  // The path is the FIRST whitespace-delimited token; the value is
  // everything after it, verbatim. Splitting on the first space rather than
  // trimming/tokenising the rest is what lets `options.blendMode Linear Burn`
  // keep its space instead of losing everything past the first word.
  const size_t sp = line.find(' ');
  if (sp == std::string::npos) return false;  // no value token at all
  const std::string path = line.substr(0, sp);
  const std::string value = line.substr(sp + 1);

  bool found = false;
  bool ok = false;
  visitBrushModelFields(m, [&](const std::string& p, auto& ref) {
    if (found || p != path) return;
    found = true;
    // `parseField()` only assigns to `ref` on its success path (see the
    // overloads above), so a malformed value leaves this one field -- and,
    // since nothing else in this lambda body ever fires for a path that
    // doesn't match, every OTHER field -- untouched.
    ok = parseField(value, ref);
  });
  return found && ok;
}

std::vector<std::string> brushModelFieldPaths() {
  BrushModel probe;
  std::vector<std::string> paths;
  visitBrushModelFields(probe, [&](const std::string& path, auto&) { paths.push_back(path); });
  return paths;
}

}  // namespace np
