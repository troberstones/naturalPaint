#include "app/selftest/Support.hpp"

#include <cstring>

#include "io/Json.hpp"

namespace np {

// io/Json (docs/automation-plan.md step 0) -- the reader, the escaper and the
// small ordered document model, promoted out of io/ExportAs.cpp and
// app/Keymap.cpp when the third consumer (io/ActionFile) made the copy count
// three.
//
// **What this section is really guarding.** The promotion had to be
// behaviour-neutral for two existing file formats and is asserted as such by
// the export-preset and keymap sections, which read their own real files
// through it. What THEY cannot see is the part that is new: a document model,
// a writer, and the two properties the action format depends on --
//
//  * **numbers survive the round trip exactly**, because a parameter written
//    to an action and read back one significant digit short is a filter that
//    quietly does something else; and
//  * **key order survives a rewrite**, because PRD P5 asks that actions be
//    diffable, and a model that reorders keys makes every re-save diff the
//    whole file.
//
// Both are asserted below against hand-typed text rather than against this
// module's own output, for the reason io/OpSerial's section already states: a
// fixture that shares the encoder's assumptions cannot catch the encoder being
// wrong. Headless, GPU-free and filesystem-free.
bool runJsonTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- the reader, driven the way its two existing callers drive it ------

  {
    JsonReader r("  \"hello\"  ", "fixture");
    std::string s;
    check(r.parseString(&s) && s == "hello" && r.ok(), "reader: a plain string, leading space skipped");
  }
  {
    // Every escape the writer can emit, typed out by hand.
    JsonReader r("\"a\\\"b\\\\c\\/d\\ne\\tf\\rg\\bh\\fi\\u0041j\"", "fixture");
    std::string s;
    const bool parsed = r.parseString(&s);
    check(parsed && s == "a\"b\\c/d\ne\tf\rg\bh\fiAj", "reader: every escape the writer emits decodes");
  }
  {
    JsonReader r("\"unterminated", "fixture");
    std::string s;
    check(!r.parseString(&s) && r.failed() && contains(r.error(), "unterminated string"),
          "reader: an unterminated string fails, and says so");
  }
  {
    JsonReader r("{", "presets.json");
    check(!r.expect('[') && contains(r.error(), "presets.json") && contains(r.error(), "at byte 0"),
          "reader: the error names the source and the byte");
  }
  {
    JsonReader r("1 -2.5 3e2 4E-3", "fixture");
    double a = 0, b = 0, c = 0, d = 0;
    const bool all = r.parseNumber(&a) && r.parseNumber(&b) && r.parseNumber(&c) && r.parseNumber(&d);
    check(all && a == 1.0 && b == -2.5 && c == 300.0 && d == 0.004, "reader: integers, signs and exponents");
  }
  {
    JsonReader r("[\"a\",\"b\"]", "fixture");
    std::vector<std::string> out;
    check(r.parseStringArray(&out) && out.size() == 2 && out[0] == "a" && out[1] == "b",
          "reader: parseStringArray, the keymap's shape");
  }
  {
    // skipValue is what lets an older build read a newer build's file: the
    // unknown key's whole value has to be consumed, however nested.
    JsonReader r("{\"unknown\":{\"a\":[1,{\"b\":null}]},\"next\":7}", "fixture");
    std::string key;
    double next = 0.0;
    const bool walked = r.expect('{') && r.parseString(&key) && r.expect(':') && r.skipValue() &&
                        r.expect(',') && r.parseString(&key) && r.expect(':') && r.parseNumber(&next);
    check(walked && key == "next" && next == 7.0, "reader: skipValue consumes a nested unknown field whole");
  }
  {
    // Only the FIRST failure is kept -- everything after a parse error is
    // noise about a position the parser should not have reached.
    JsonReader r("x", "fixture");
    r.fail("first");
    r.fail("second");
    check(contains(r.error(), "first") && !contains(r.error(), "second"),
          "reader: the first error is the one that is kept");
  }

  // --- escapeJson, including the widening the promotion performed ---------

  check(escapeJson("plain") == "plain", "escapeJson: an ordinary string is unchanged");
  check(escapeJson("a\"b\\c") == "a\\\"b\\\\c", "escapeJson: quote and backslash, as before");
  check(escapeJson("a\nb\tc") == "a\\nb\\tc", "escapeJson: newline and tab, which the old copy passed raw");
  check(escapeJson(std::string("a\x01""b")) == "a\\u0001b", "escapeJson: a control byte becomes \\u00XX");
  {
    // The claim the widening rests on: for every byte outside the set it
    // already handled, output is byte-for-byte what it was. Asserted over the
    // whole printable range rather than over an example.
    bool unchanged = true;
    for (int c = 0x20; c < 0x7f; ++c) {
      if (c == '"' || c == '\\') continue;
      const std::string one(1, static_cast<char>(c));
      if (escapeJson(one) != one) unchanged = false;
    }
    check(unchanged, "escapeJson: every printable byte but \" and \\ is untouched");
  }

  // --- the document model -------------------------------------------------

  {
    // Hand-typed, in the shape io/ExportAs writes, so the assertions below
    // are about the parser and not about a value this test just encoded.
    const char* kFixture =
        "{\n"
        "  \"version\": 1,\n"
        "  \"presets\": [\n"
        "    { \"name\": \"Web \\u0041\", \"format\": \"png\", \"depth\": 8, \"linear\": false },\n"
        "    { \"name\": \"Plate\",   \"format\": \"exr\", \"depth\": 16, \"linear\": true }\n"
        "  ]\n"
        "}\n";
    JsonValue doc;
    std::string err;
    const bool parsed = parseJson(kFixture, "fixture", &doc, &err);
    check(parsed && err.empty(), "DOM: a hand-typed preset file parses");
    check(doc.isObject() && doc.numberOr("version", -1) == 1.0, "DOM: a scalar member reads back");
    const JsonValue* presets = doc.find("presets");
    check(presets != nullptr && presets->isArray() && presets->size() == 2, "DOM: an array member has both entries");
    if (presets != nullptr && presets->size() == 2) {
      check(presets->at(0).stringOr("name", "") == "Web A", "DOM: a \\u escape inside a nested string decodes");
      check(presets->at(1).numberOr("depth", -1) == 16.0 && presets->at(1).boolOr("linear", false),
            "DOM: number and bool members of a nested object");
    }
    check(doc.find("absent") == nullptr && doc.numberOr("absent", 42.0) == 42.0,
          "DOM: an absent key is null, and the fallback is returned");
    check(doc.numberOr("presets", 42.0) == 42.0, "DOM: a key of the wrong kind returns the fallback too");
    check(doc.at(0).isNull(), "DOM: indexing an object as an array yields null, not a crash");
  }
  {
    JsonValue o = JsonValue::object();
    o.set("b", JsonValue::number(1));
    o.set("a", JsonValue::number(2));
    o.set("b", JsonValue::number(3));
    check(o.members().size() == 2 && o.members()[0].first == "b" && o.members()[1].first == "a" &&
              o.numberOr("b", 0) == 3.0,
          "DOM: set() replaces in place, so key order survives a rewrite");
  }
  {
    JsonValue doc;
    check(parseJson("{\"k\":1,\"k\":2}", "fixture", &doc) && doc.numberOr("k", 0) == 1.0,
          "DOM: a duplicate key keeps the first, and does not refuse the file");
  }
  {
    JsonValue doc;
    std::string err;
    check(!parseJson("{\"a\":1} {\"b\":2}", "fixture", &doc, &err) && contains(err, "trailing content"),
          "DOM: a second document after the first is refused");
  }
  {
    std::string deep(40, '[');
    JsonValue doc;
    std::string err;
    check(!parseJson(deep, "fixture", &doc, &err) && contains(err, "nested too deeply"),
          "DOM: nesting past the shared depth limit is refused");
  }

  // --- the writer, and the property the action format rests on ------------

  {
    JsonValue o = JsonValue::object();
    o.set("width", JsonValue::number(512));
    o.set("sigma", JsonValue::number(static_cast<double>(4.0f)));
    const std::string dense = o.write(-1);
    check(dense == "{\"width\":512,\"sigma\":4}", "writer: integral values write without a decimal point");
  }
  {
    // The round trip that matters: every parameter in an action came from a
    // float, so a float written and read back must be the identical float --
    // not close, identical, compared as bits.
    const float kValues[] = {0.1f, 1.0f / 3.0f, 3.14159265f, 1e-9f, -2.5e7f, 0.0f, -0.0f, 65504.0f};
    bool exact = true;
    for (float v : kValues) {
      JsonValue o = JsonValue::object();
      o.set("v", JsonValue::number(static_cast<double>(v)));
      JsonValue back;
      if (!parseJson(o.write(-1), "roundtrip", &back)) {
        exact = false;
        break;
      }
      const float got = static_cast<float>(back.numberOr("v", 1.0));
      if (std::memcmp(&got, &v, sizeof(float)) != 0) exact = false;
    }
    check(exact, "writer: every float round-trips to the identical bit pattern");
  }
  {
    // Doubles that nine significant digits cannot carry must fall back to
    // seventeen rather than silently losing precision.
    const double v = 0.1234567890123456789;
    JsonValue o = JsonValue::object();
    o.set("v", JsonValue::number(v));
    JsonValue back;
    check(parseJson(o.write(-1), "roundtrip", &back) && back.numberOr("v", 0.0) == v,
          "writer: a double past nine digits falls back to seventeen");
  }
  {
    JsonValue o = JsonValue::object();
    o.set("nan", JsonValue::number(std::nan("")));
    check(o.write(-1) == "{\"nan\":null}", "writer: a non-finite number writes null, not invalid JSON");
  }
  {
    JsonValue empty = JsonValue::object();
    JsonValue arr = JsonValue::array();
    check(empty.write(-1) == "{}" && arr.write(-1) == "[]", "writer: empty object and array stay on one line");
  }
  {
    // Pretty output must itself be readable back, and a second pass must be
    // byte-identical to the first -- that is what makes a re-save diff only
    // what the user changed.
    JsonValue o = JsonValue::object();
    o.set("name", JsonValue::string("Height prep"));
    JsonValue steps = JsonValue::array();
    JsonValue step = JsonValue::object();
    step.set("cmd", JsonValue::string("image_size"));
    step.set("width", JsonValue::number(512));
    steps.push(std::move(step));
    o.set("steps", std::move(steps));
    const std::string first = o.write(2);
    JsonValue back;
    const bool reparsed = parseJson(first, "roundtrip", &back);
    check(reparsed && back.write(2) == first, "writer: pretty output re-reads to a byte-identical rewrite");
  }

  return ok;
}

}  // namespace np
