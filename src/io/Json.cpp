#include "io/Json.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace np {
namespace {

const JsonValue& nullValue() {
  static const JsonValue kNull;
  return kNull;
}

// Shortest exact: try 9 significant digits first, and only fall back to 17
// when 9 does not read back to the identical double.
//
// **This is a correctness requirement, not a tidiness one.** Every parameter
// that reaches this writer came from a `float`, and 9 significant digits
// round-trip a float exactly; 17 round-trips any double exactly but writes
// `0.10000000000000001` for `0.1f`, which makes a file nobody can hand-edit
// and a diff nobody can read (PRD P5). Integral values are written without a
// decimal point for the same reason -- `512`, not `512.0`, in a resize step
// someone is going to read.
std::string writeNumber(double v) {
  if (!std::isfinite(v)) {
    // JSON has no inf or NaN. Writing `null` keeps the file parseable and is
    // caught by the reader as "wrong kind" rather than as a syntax error at a
    // byte offset that means nothing to whoever has to fix it.
    return "null";
  }
  // Negative zero before the integral fast path, which would otherwise write
  // it as `0` and lose the sign bit. Caught by this module's own
  // round-trip-to-identical-bits assertion, which is the reason that
  // assertion compares bit patterns rather than values.
  if (v == 0.0) return std::signbit(v) ? "-0" : "0";
  if (v == static_cast<double>(static_cast<long long>(v)) && std::fabs(v) < 9.0e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    return buf;
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  if (std::strtod(buf, nullptr) == v) return buf;
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

void writeInto(const JsonValue& v, int indent, int depth, std::string* out) {
  const bool pretty = indent >= 0;
  const std::string pad = pretty ? std::string(static_cast<size_t>(indent) * (depth + 1), ' ') : "";
  const std::string padEnd = pretty ? std::string(static_cast<size_t>(indent) * depth, ' ') : "";
  const char* nl = pretty ? "\n" : "";
  const char* sp = pretty ? " " : "";

  switch (v.kind()) {
    case JsonValue::Kind::Null: *out += "null"; return;
    case JsonValue::Kind::Bool: *out += v.asBool() ? "true" : "false"; return;
    case JsonValue::Kind::Number: *out += writeNumber(v.asNumber()); return;
    case JsonValue::Kind::String:
      *out += '"';
      *out += escapeJson(v.asString());
      *out += '"';
      return;
    case JsonValue::Kind::Array: {
      if (v.size() == 0) {
        *out += "[]";
        return;
      }
      *out += '[';
      *out += nl;
      for (size_t i = 0; i < v.size(); ++i) {
        *out += pad;
        writeInto(v.at(i), indent, depth + 1, out);
        if (i + 1 < v.size()) *out += ',';
        *out += nl;
      }
      *out += padEnd;
      *out += ']';
      return;
    }
    case JsonValue::Kind::Object: {
      const auto& m = v.members();
      if (m.empty()) {
        *out += "{}";
        return;
      }
      *out += '{';
      *out += nl;
      for (size_t i = 0; i < m.size(); ++i) {
        *out += pad;
        *out += '"';
        *out += escapeJson(m[i].first);
        *out += "\":";
        *out += sp;
        writeInto(m[i].second, indent, depth + 1, out);
        if (i + 1 < m.size()) *out += ',';
        *out += nl;
      }
      *out += padEnd;
      *out += '}';
      return;
    }
  }
}

bool parseValue(JsonReader& r, int depth, JsonValue* out);

bool parseObjectBody(JsonReader& r, int depth, JsonValue* out) {
  *out = JsonValue::object();
  if (!r.expect('{')) return false;
  if (r.peek() == '}') return r.expect('}');
  for (;;) {
    std::string key;
    if (!r.parseString(&key) || !r.expect(':')) return false;
    JsonValue value;
    if (!parseValue(r, depth + 1, &value)) return false;
    // First wins -- see Json.hpp §2.
    if (out->find(key) == nullptr) out->set(key, std::move(value));
    if (r.peek() == ',') {
      r.consume();
      continue;
    }
    break;
  }
  return r.expect('}');
}

bool parseArrayBody(JsonReader& r, int depth, JsonValue* out) {
  *out = JsonValue::array();
  if (!r.expect('[')) return false;
  if (r.peek() == ']') return r.expect(']');
  for (;;) {
    JsonValue value;
    if (!parseValue(r, depth + 1, &value)) return false;
    out->push(std::move(value));
    if (r.peek() == ',') {
      r.consume();
      continue;
    }
    break;
  }
  return r.expect(']');
}

bool parseKeyword(JsonReader& r, JsonValue* out) {
  std::string word;
  while (std::isalpha(static_cast<unsigned char>(r.peek()))) {
    word += r.peek();
    r.consume();
  }
  if (word == "true") {
    *out = JsonValue::boolean(true);
    return true;
  }
  if (word == "false") {
    *out = JsonValue::boolean(false);
    return true;
  }
  if (word == "null") {
    *out = JsonValue::null();
    return true;
  }
  r.fail("expected true, false or null");
  return false;
}

bool parseValue(JsonReader& r, int depth, JsonValue* out) {
  if (depth > JsonReader::kMaxDepth) {
    r.fail("value nested too deeply");
    return false;
  }
  const char c = r.peek();
  if (c == '{') return parseObjectBody(r, depth, out);
  if (c == '[') return parseArrayBody(r, depth, out);
  if (c == '"') {
    std::string s;
    if (!r.parseString(&s)) return false;
    *out = JsonValue::string(std::move(s));
    return true;
  }
  if (c == 't' || c == 'f' || c == 'n') return parseKeyword(r, out);
  if (c == '\0') {
    r.fail("expected a value");
    return false;
  }
  double d = 0.0;
  if (!r.parseNumber(&d)) return false;
  *out = JsonValue::number(d);
  return true;
}

}  // namespace

// --------------------------------------------------------------- JsonReader

void JsonReader::fail(const std::string& what) {
  if (failed_) return;
  failed_ = true;
  error_ = std::string(label_) + ": " + what + " (at byte " + std::to_string(i_) + ")";
}

void JsonReader::skipWs() {
  while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
    ++i_;
}

char JsonReader::peek() {
  skipWs();
  return i_ < s_.size() ? s_[i_] : '\0';
}

bool JsonReader::expect(char c) {
  skipWs();
  if (i_ >= s_.size() || s_[i_] != c) {
    fail(std::string("expected '") + c + "'");
    return false;
  }
  ++i_;
  return true;
}

bool JsonReader::parseString(std::string* out) {
  skipWs();
  if (i_ >= s_.size() || s_[i_] != '"') {
    fail("expected a string");
    return false;
  }
  ++i_;
  out->clear();
  while (i_ < s_.size() && s_[i_] != '"') {
    char c = s_[i_++];
    if (c == '\\' && i_ < s_.size()) {
      const char e = s_[i_++];
      switch (e) {
        case '"': *out += '"'; break;
        case '\\': *out += '\\'; break;
        case '/': *out += '/'; break;
        case 'b': *out += '\b'; break;
        case 'f': *out += '\f'; break;
        case 'n': *out += '\n'; break;
        case 'r': *out += '\r'; break;
        case 't': *out += '\t'; break;
        // `\uXXXX` is decoded only for the ASCII range, which is every escape
        // this project's writer can produce (escapeJson() emits \u00XX for
        // control bytes and nothing else). A non-ASCII one keeps the old
        // behaviour of both copies -- the escape is dropped and its digits
        // are not consumed as text -- rather than growing a UTF-8 encoder
        // that no caller here needs.
        case 'u': {
          unsigned code = 0;
          int digits = 0;
          while (digits < 4 && i_ < s_.size() &&
                 std::isxdigit(static_cast<unsigned char>(s_[i_]))) {
            const char h = s_[i_++];
            const unsigned d = static_cast<unsigned>(std::isdigit(static_cast<unsigned char>(h))
                                                         ? h - '0'
                                                         : (std::tolower(h) - 'a' + 10));
            code = code * 16u + d;
            ++digits;
          }
          if (digits == 4 && code < 0x80u) *out += static_cast<char>(code);
          break;
        }
        default: *out += e; break;
      }
    } else {
      *out += c;
    }
  }
  if (i_ >= s_.size()) {
    fail("unterminated string");
    return false;
  }
  ++i_;  // closing quote
  return true;
}

bool JsonReader::parseStringArray(std::vector<std::string>* out) {
  if (!expect('[')) return false;
  if (peek() == ']') return expect(']');
  for (;;) {
    std::string v;
    if (!parseString(&v)) return false;
    out->push_back(std::move(v));
    if (peek() == ',') {
      consume();
      continue;
    }
    break;
  }
  return expect(']');
}

bool JsonReader::parseNumber(double* out) {
  skipWs();
  const size_t start = i_;
  while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' ||
                            s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
    ++i_;
  if (i_ == start) {
    fail("expected a number");
    return false;
  }
  *out = std::strtod(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
  return true;
}

bool JsonReader::skipValue(int depth) {
  if (depth > kMaxDepth) {
    fail("value nested too deeply");
    return false;
  }
  const char c = peek();
  if (c == '"') {
    std::string ignored;
    return parseString(&ignored);
  }
  if (c == '{') {
    if (!expect('{')) return false;
    if (peek() == '}') return expect('}');
    for (;;) {
      std::string key;
      if (!parseString(&key) || !expect(':') || !skipValue(depth + 1)) return false;
      if (peek() == ',') {
        consume();
        continue;
      }
      break;
    }
    return expect('}');
  }
  if (c == '[') {
    if (!expect('[')) return false;
    if (peek() == ']') return expect(']');
    for (;;) {
      if (!skipValue(depth + 1)) return false;
      if (peek() == ',') {
        consume();
        continue;
      }
      break;
    }
    return expect(']');
  }
  if (c == 't' || c == 'f' || c == 'n') {
    while (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) ++i_;
    return true;
  }
  double ignored = 0.0;
  return parseNumber(&ignored);
}

// -------------------------------------------------------------- escapeJson

std::string escapeJson(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    switch (c) {
      case '"': out += "\\\""; continue;
      case '\\': out += "\\\\"; continue;
      case '\b': out += "\\b"; continue;
      case '\f': out += "\\f"; continue;
      case '\n': out += "\\n"; continue;
      case '\r': out += "\\r"; continue;
      case '\t': out += "\\t"; continue;
      default: break;
    }
    if (u < 0x20u) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", u);
      out += buf;
      continue;
    }
    out += c;
  }
  return out;
}

// --------------------------------------------------------------- JsonValue

JsonValue JsonValue::boolean(bool v) {
  JsonValue j;
  j.kind_ = Kind::Bool;
  j.bool_ = v;
  return j;
}

JsonValue JsonValue::number(double v) {
  JsonValue j;
  j.kind_ = Kind::Number;
  j.number_ = v;
  return j;
}

JsonValue JsonValue::string(std::string v) {
  JsonValue j;
  j.kind_ = Kind::String;
  j.string_ = std::move(v);
  return j;
}

JsonValue JsonValue::array() {
  JsonValue j;
  j.kind_ = Kind::Array;
  return j;
}

JsonValue JsonValue::object() {
  JsonValue j;
  j.kind_ = Kind::Object;
  return j;
}

size_t JsonValue::size() const {
  if (kind_ == Kind::Array) return elements_.size();
  if (kind_ == Kind::Object) return members_.size();
  return 0;
}

const JsonValue& JsonValue::at(size_t i) const {
  if (kind_ != Kind::Array || i >= elements_.size()) return nullValue();
  return elements_[i];
}

void JsonValue::push(JsonValue v) {
  if (kind_ != Kind::Array) {
    kind_ = Kind::Array;
    elements_.clear();
  }
  elements_.push_back(std::move(v));
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (kind_ != Kind::Object) return nullptr;
  for (const auto& m : members_)
    if (m.first == key) return &m.second;
  return nullptr;
}

void JsonValue::set(std::string_view key, JsonValue v) {
  if (kind_ != Kind::Object) {
    kind_ = Kind::Object;
    members_.clear();
  }
  for (auto& m : members_) {
    if (m.first == key) {
      m.second = std::move(v);
      return;
    }
  }
  members_.emplace_back(std::string(key), std::move(v));
}

bool JsonValue::hasNumber(std::string_view key) const {
  const JsonValue* v = find(key);
  return v != nullptr && v->isNumber();
}

double JsonValue::numberOr(std::string_view key, double fallback) const {
  const JsonValue* v = find(key);
  return (v != nullptr && v->isNumber()) ? v->asNumber() : fallback;
}

bool JsonValue::boolOr(std::string_view key, bool fallback) const {
  const JsonValue* v = find(key);
  return (v != nullptr && v->isBool()) ? v->asBool() : fallback;
}

std::string JsonValue::stringOr(std::string_view key, std::string_view fallback) const {
  const JsonValue* v = find(key);
  return (v != nullptr && v->isString()) ? v->asString() : std::string(fallback);
}

std::string JsonValue::write(int indent) const {
  std::string out;
  writeInto(*this, indent, 0, &out);
  return out;
}

// --------------------------------------------------------------- parseJson

bool parseJson(std::string_view text, std::string_view label, JsonValue* out,
               std::string* errorOut) {
  JsonReader r(text, label);
  JsonValue value;
  if (!parseValue(r, 0, &value)) {
    if (errorOut) *errorOut = r.error();
    return false;
  }
  // Trailing content is a refusal, not a shrug: a file with a second document
  // after the first is a file someone half-edited, and reading only the first
  // half of it silently is how that goes unnoticed.
  if (r.peek() != '\0') {
    r.fail("trailing content after the top-level value");
    if (errorOut) *errorOut = r.error();
    return false;
  }
  *out = std::move(value);
  return true;
}

}  // namespace np
