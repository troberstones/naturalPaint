#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// io/Json -- the project's one JSON reader, its escaper, and a small ordered
// document model.
//
// **This file exists because the count reached three, which is the trigger
// io/ExportAs.cpp wrote down for itself.** That file's own comment above its
// private copy said it in as many words: "Deliberately a second copy rather
// than a promotion of Keymap.cpp's... a *third* consumer is when this becomes
// a shared header rather than a judgement call." The third consumer is
// io/ActionFile (docs/automation-plan.md step 4), so the promotion is due, and
// it happens before that consumer is written rather than after -- four copies
// for the length of one step is how a fourth copy becomes permanent.
//
// There is still no JSON library vendored anywhere in this project (checked
// third_party/ and cmake/Dependencies.cmake). Hand-rolling is unchanged as the
// decision; what changes is that it is hand-rolled once.
//
// ==========================================================================
// (1) Two readers were merged, and the union is what each one already needed
// ==========================================================================
//
// `io/ExportAs.cpp`'s copy stored its error and published it (`error()`), and
// carried `parseNumber()`, `skipValue()` and `consume()`. `app/Keymap.cpp`'s
// copy printed its error to stderr with a `[keymap]` prefix and carried
// `parseStringArray()`. Neither had the other's half.
//
// The merged reader **stores** the error, because storing composes and
// printing does not: a caller that wants Keymap's stderr line writes it from
// `error()`, and a caller that wants ExportPresetStore's `problems()` list
// appends it. The one visible consequence is that the keymap's diagnostic now
// ends "(at byte N)" rather than "(near offset N)" -- the same number, the
// wording io/ExportAs already used, and nothing asserts either string.
//
// `parseString()` is the pointer-out form. Keymap's `std::optional` form was
// the minority spelling and an overload set differing only in return type is
// not one, so its four call sites moved rather than the API growing a second
// name for one operation.
//
// ==========================================================================
// (2) JsonValue's object is an ordered vector, not a map, and that is a
//     format decision rather than a performance one
// ==========================================================================
//
// PRD P5 asks that actions be "human-readable, and diffable". A document
// model whose object is a `std::map` or an unordered container writes its keys
// back in the container's order, so re-saving a file someone hand-edited
// reorders it and the diff is the whole file. An ordered vector of pairs
// writes keys in the order they were authored or inserted, so a one-value edit
// diffs as one line. Objects here hold a handful of keys, so lookup being
// linear is not a cost worth a second data structure.
//
// **Duplicate keys keep the first.** JSON does not forbid them and the
// alternatives are worse: last-wins silently honours the key a reader did not
// see first, and refusing outright turns a merge artefact into a file that
// will not open at all.
namespace np {

// --- The reader ----------------------------------------------------------
//
// A pull parser: the caller drives it against a schema it already knows. It
// is not a validator and does not build anything -- `parseJson()` below is
// the layer that does, and it is written against this.
class JsonReader {
 public:
  JsonReader(std::string_view text, std::string_view label) : s_(text), label_(label) {}

  bool ok() const { return !failed_; }
  bool failed() const { return failed_; }

  // "<label>: <what> (at byte N)", or empty while nothing has failed. Only the
  // FIRST failure is kept: everything after a parse error is noise about a
  // position the parser should never have reached.
  const std::string& error() const { return error_; }

  void fail(const std::string& what);

  void skipWs();

  // The next non-whitespace character without consuming it; '\0' at end.
  char peek();

  bool expect(char c);

  bool parseString(std::string* out);

  // A JSON array of strings. The opening '[' has NOT been consumed yet.
  bool parseStringArray(std::vector<std::string>* out);

  bool parseNumber(double* out);

  // Consumes and discards any value, so an unrecognised field is skipped
  // rather than fatal -- a newer build's extra key must not stop an older one
  // from reading the fields it does understand.
  bool skipValue(int depth = 0);

  // Consumes one byte with no checking. Only valid straight after a `peek()`
  // that told the caller what that byte is.
  void consume() { ++i_; }

  size_t offset() const { return i_; }

  // The deepest nesting any parse here accepts. Shared by `skipValue()` and
  // `parseJson()` so a file cannot be readable by one and not the other.
  static constexpr int kMaxDepth = 16;

 private:
  std::string_view s_;
  std::string_view label_;
  size_t i_ = 0;
  bool failed_ = false;
  std::string error_;
};

// --- The escaper ---------------------------------------------------------
//
// **Widened in the move, deliberately.** io/ExportAs.cpp's copy escaped `"`
// and `\` and nothing else, so any control character in a string -- a newline
// pasted into a preset name -- was written raw and produced a file that would
// not parse back. That is a bug rather than a simplification, and the fix
// cannot change the output for any string that did not already round-trip
// wrong: for every byte >= 0x20 other than the two it already handled, this
// emits exactly what it emitted before.
std::string escapeJson(std::string_view s);

// --- The document model --------------------------------------------------

class JsonValue {
 public:
  enum class Kind { Null, Bool, Number, String, Array, Object };

  JsonValue() = default;

  static JsonValue null() { return JsonValue(); }
  static JsonValue boolean(bool v);
  static JsonValue number(double v);
  static JsonValue string(std::string v);
  static JsonValue array();
  static JsonValue object();

  Kind kind() const { return kind_; }
  bool isNull() const { return kind_ == Kind::Null; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isNumber() const { return kind_ == Kind::Number; }
  bool isString() const { return kind_ == Kind::String; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isObject() const { return kind_ == Kind::Object; }

  bool asBool(bool fallback = false) const { return isBool() ? bool_ : fallback; }
  double asNumber(double fallback = 0.0) const { return isNumber() ? number_ : fallback; }
  const std::string& asString() const { return string_; }

  // Array access. `size()` is 0 for anything that is not an array or object.
  size_t size() const;
  const JsonValue& at(size_t i) const;
  void push(JsonValue v);

  // Object access. `find()` returns nullptr when the key is absent OR when
  // this is not an object, which is the answer a caller wants in both cases.
  const JsonValue* find(std::string_view key) const;
  // Inserts, or replaces the value of an existing key **in place**, so key
  // order survives a rewrite (see this header's §2).
  void set(std::string_view key, JsonValue v);
  const std::vector<std::pair<std::string, JsonValue>>& members() const { return members_; }

  // Typed convenience readers. Each returns `fallback` when the key is
  // absent or holds the wrong kind, and `has*()` is how a caller tells "absent"
  // from "present and equal to the fallback" when that distinction matters.
  bool hasNumber(std::string_view key) const;
  double numberOr(std::string_view key, double fallback) const;
  bool boolOr(std::string_view key, bool fallback) const;
  std::string stringOr(std::string_view key, std::string_view fallback) const;

  // Serialised. `indent >= 0` pretty-prints with that many spaces per level;
  // `indent < 0` writes one dense line. Numbers are written in the shortest
  // form that reads back to the identical double -- see Json.cpp's
  // `writeNumber()` for why that is a correctness requirement here and not a
  // cosmetic one.
  std::string write(int indent = 2) const;

 private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<JsonValue> elements_;
  std::vector<std::pair<std::string, JsonValue>> members_;
};

// Parses `text` into `*out`. Returns false and fills `*errorOut` (when given)
// on the first malformed byte. `label` names the source in that message.
bool parseJson(std::string_view text, std::string_view label, JsonValue* out,
               std::string* errorOut = nullptr);

}  // namespace np
