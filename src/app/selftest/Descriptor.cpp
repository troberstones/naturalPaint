#include "app/selftest/Support.hpp"

#include <sys/mman.h>

#include "io/Descriptor.hpp"

namespace np {
namespace {

// --- The fixture writer ---------------------------------------------------
//
// There is no `.abr` file on this machine, so every byte this section parses is
// written here. That is a weakness and it is stated in io/Descriptor.hpp
// rather than hidden -- these fixtures prove the reader agrees with the format
// as documented, not that it agrees with Photoshop.
//
// It is also what makes the *adversarial* half possible at all, which is the
// half that matters most: a corrupt file is not something you find lying
// around, it is something you build one field at a time. So the writer's whole
// job is that a fixture reads as intent -- `.key4("Dmtr").untf("#Pxl", 12.5)`
// -- and that a deliberately-broken one differs from the good one by exactly
// the line that broke it. A hex blob would hide that difference, and a fixture
// nobody can read is a fixture nobody notices is wrong.
struct DescFixture {
  std::vector<uint8_t> bytes;

  DescFixture& u8v(unsigned v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xFFu));
    return *this;
  }
  DescFixture& u16v(unsigned v) {
    return u8v(v >> 8).u8v(v);
  }
  DescFixture& u32v(uint32_t v) {
    return u8v(v >> 24).u8v(v >> 16).u8v(v >> 8).u8v(v);
  }
  DescFixture& u64v(uint64_t v) {
    for (int i = 7; i >= 0; --i) u8v(static_cast<unsigned>((v >> (8 * i)) & 0xFFu));
    return *this;
  }
  // Big-endian IEEE-754 binary64, written as its bit pattern so the fixture
  // asks for exactly the double the reader must produce.
  DescFixture& f64v(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return u64v(bits);
  }
  // Four literal bytes with no length in front: a type key, or a `UntF` unit.
  DescFixture& code(const char* fourCC) {
    for (int i = 0; i < 4; ++i) u8v(static_cast<unsigned char>(fourCC[i]));
    return *this;
  }
  // A Key in its zero-length form: "0 means four".
  DescFixture& key4(const char* fourCC) { return u32v(0).code(fourCC); }
  // A Key in its explicit-length form. Passing a four-character string here is
  // the case a reader that special-cases 4 gets wrong, and it is used below.
  DescFixture& keyN(const char* s) {
    const size_t n = std::strlen(s);
    u32v(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) u8v(static_cast<unsigned char>(s[i]));
    return *this;
  }
  // A UnicodeString from raw UTF-16 code units, written verbatim -- the form
  // the surrogate fixtures need.
  DescFixture& unicodeUnits(const std::vector<uint16_t>& units) {
    u32v(static_cast<uint32_t>(units.size()));
    for (const uint16_t unit : units) u16v(unit);
    return *this;
  }
  // A UnicodeString the way Photoshop writes one: ASCII plus a trailing NUL
  // that is counted in the length and must not come back in the string.
  DescFixture& unicode(const char* ascii) {
    std::vector<uint16_t> units;
    for (const char* p = ascii; *p != '\0'; ++p)
      units.push_back(static_cast<uint16_t>(static_cast<unsigned char>(*p)));
    units.push_back(0);
    return unicodeUnits(units);
  }

  // A descriptor header: class name, class id, item count.
  DescFixture& descriptor(const char* className, const char* classId, uint32_t items) {
    return unicode(className).key4(classId).u32v(items);
  }
  DescFixture& version() { return u32v(kActionDescriptorVersion); }

  DescFixture& textv(const char* s) { return code("TEXT").unicode(s); }
  DescFixture& untf(const char* unit, double v) { return code("UntF").code(unit).f64v(v); }
  DescFixture& doubv(double v) { return code("doub").f64v(v); }
  DescFixture& longv(int32_t v) { return code("long").u32v(static_cast<uint32_t>(v)); }
  DescFixture& compv(int64_t v) { return code("comp").u64v(static_cast<uint64_t>(v)); }
  DescFixture& boolv(bool v) { return code("bool").u8v(v ? 1u : 0u); }
  DescFixture& tdta(const std::vector<uint8_t>& payload) {
    code("tdta").u32v(static_cast<uint32_t>(payload.size()));
    for (const uint8_t byte : payload) u8v(byte);
    return *this;
  }
  DescFixture& alis(const std::vector<uint8_t>& payload) {
    code("alis").u32v(static_cast<uint32_t>(payload.size()));
    for (const uint8_t byte : payload) u8v(byte);
    return *this;
  }
};

// --- The guard page -------------------------------------------------------
//
// The one property io/Descriptor.hpp calls its most important is "no input,
// however hostile, causes a read outside the caller's buffer". A test that
// hands the parser a `std::vector` cannot check that: malloc's slack absorbs an
// overread, the test passes, and the bug ships.
//
// So every parse in this section runs out of a buffer whose **last byte is the
// last readable byte of a mapping**, with an `mprotect(PROT_NONE)` page
// immediately after it. One byte of overread is a SIGSEGV, at the instruction
// that did it. That turns "we believe the bounds checks are right" into
// something the machine enforces, without ASAN and therefore in the ordinary
// RelWithDebInfo build both configurations already produce.
//
// It is deliberately the *tail* of the mapping rather than the head: an
// overread is overwhelmingly more likely than an underread, and only one end
// can abut the guard.
struct GuardedBytes {
  std::span<const uint8_t> view;
  void* mapping = nullptr;
  size_t mappingSize = 0;
};

GuardedBytes guardMap(const std::vector<uint8_t>& src) {
  GuardedBytes g;
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t dataPages = (src.size() + page - 1) / page;
  g.mappingSize = (dataPages + 1) * page;
  void* m = mmap(nullptr, g.mappingSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (m == MAP_FAILED) {
    g.mapping = nullptr;
    g.mappingSize = 0;
    return g;
  }
  g.mapping = m;
  auto* base = static_cast<uint8_t*>(m);
  uint8_t* guard = base + dataPages * page;
  if (mprotect(guard, page, PROT_NONE) != 0) {
    munmap(m, g.mappingSize);
    g.mapping = nullptr;
    g.mappingSize = 0;
    return g;
  }
  uint8_t* start = guard - src.size();
  if (!src.empty()) std::memcpy(start, src.data(), src.size());
  g.view = std::span<const uint8_t>(start, src.size());
  return g;
}

void guardUnmap(GuardedBytes& g) {
  if (g.mapping != nullptr) munmap(g.mapping, g.mappingSize);
  g.mapping = nullptr;
  g.view = {};
}

// Every parse in this section goes through here, so the guard cannot be
// forgotten on the one fixture that needed it. `guardedParses` is printed at
// the end: a test that silently stopped exercising the guard would show as the
// count dropping, which a bare `pass` never would.
size_t guardedParses = 0;
size_t guardMapFailures = 0;

DescriptorParseResult parseGuarded(const std::vector<uint8_t>& src,
                                   const DescriptorParseOptions& options = {},
                                   bool versioned = true) {
  GuardedBytes g = guardMap(src);
  if (g.mapping == nullptr) {
    ++guardMapFailures;
    DescriptorParseResult r;
    r.error = "selftest: mmap/mprotect failed";
    return r;
  }
  ++guardedParses;
  DescriptorParseResult r = versioned ? parseVersionedActionDescriptor(g.view, options)
                                      : parseActionDescriptor(g.view, options);
  guardUnmap(g);
  return r;
}

bool refusalIsWellFormed(const DescriptorParseResult& r) {
  return !r.ok && r.tree.empty() && r.bytesConsumed == 0 &&
         r.error.rfind("descriptor refused: ", 0) == 0;
}

bool errorMentions(const DescriptorParseResult& r, const char* needle) {
  return r.error.find(needle) != std::string::npos;
}

}  // namespace

// PLAN.md "12 -- Import brushes", first bullet: io/Descriptor, the Action
// Descriptor reader. PRD G7 and G9. See SelfTest.hpp for the full breakdown.
bool runDescriptorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ---- 1. Every type this build parses, in one flat descriptor ------------
  DescFixture flat;
  flat.version()
      .descriptor("", "null", 11)
      .key4("Nm  ").textv("Round 5")
      .key4("Dmtr").untf("#Pxl", 12.5)
      .key4("Hrdn").doubv(0.75)
      .key4("Cnt ").longv(-7)
      .key4("Big ").compv(int64_t{4294967296})
      .key4("Intr").boolv(true)
      .key4("bTyp").code("enum").keyN("brushType").keyN("computed")
      .key4("Md  ").code("enum").key4("BlnM").key4("Nrml")
      .key4("Clss").code("type").unicode("").key4("brsh")
      .key4("Data").tdta({0xDE, 0xAD, 0xBE, 0xEF})
      .key4("Alis").alis({0x01, 0x02, 0x03});

  const DescriptorParseResult flatResult = parseGuarded(flat.bytes);
  check(flatResult.ok, "a descriptor holding all ten leaf types reads without refusal");
  check(flatResult.bytesConsumed == flat.bytes.size(),
        "bytesConsumed is the whole fixture, version word included");
  check(flatResult.warnings.empty(), "a well-formed descriptor produces no warnings");

  const DescriptorRef root = flatResult.tree.root();
  check(root.valid() && root.type() == DescriptorType::Descriptor,
        "the root node is a descriptor");
  check(root.classId() == "null", "the root's classId is the 4-char key it was written with");
  check(root.className().empty(), "an empty class name decodes to an empty string, not a NUL");
  check(root.childCount() == 11, "the root has the 11 items its count declared");
  check(root.path().empty() && root.key().empty(), "the root has no key and an empty path");

  const auto name = root.field("Nm  ").asText();
  check(name.has_value() && *name == "Round 5",
        "TEXT decodes UTF-16BE to UTF-8 and drops the counted trailing NUL");
  const auto dmtr = root.field("Dmtr").asUnitFloat();
  check(dmtr.has_value() && dmtr->unit == "#Pxl" && dmtr->value == 12.5,
        "UntF carries its 4-char unit and an exact big-endian binary64");
  check(root.field("Hrdn").asDouble() == std::optional<double>(0.75),
        "doub is an exact big-endian binary64");
  check(root.field("Cnt ").asInteger() == std::optional<int32_t>(-7),
        "long is a signed 32-bit big-endian integer");
  check(root.field("Big ").asLargeInteger() == std::optional<int64_t>(4294967296),
        "comp is a signed 64-bit big-endian integer");
  check(root.field("Intr").asBoolean() == std::optional<bool>(true), "bool is one byte");
  const auto bTyp = root.field("bTyp").asEnumerated();
  check(bTyp.has_value() && bTyp->typeId == "brushType" && bTyp->valueId == "computed",
        "enum reads both halves as length-prefixed keys");
  const auto md = root.field("Md  ").asEnumerated();
  check(md.has_value() && md->typeId == "BlnM" && md->valueId == "Nrml",
        "enum reads both halves as zero-length 4-char keys too");
  check(root.field("Clss").type() == DescriptorType::Class &&
            root.field("Clss").classId() == "brsh",
        "type carries a class name and a class id and no payload");
  const std::vector<uint8_t>* data = root.field("Data").asRawData();
  check(data != nullptr && *data == std::vector<uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}),
        "tdta carries its declared bytes verbatim");
  const std::vector<uint8_t>* alias = root.field("Alis").asRawData();
  check(alias != nullptr && alias->size() == 3, "alis is kept as opaque bytes, not interpreted");

  // The typed reads coerce nothing. io/Descriptor.hpp argues this at length:
  // a reader that widens a `long` into a double can no longer tell its caller
  // which one the file said, and PRD G9's report is made entirely of being
  // able to say that.
  check(!root.field("Cnt ").asDouble().has_value(), "asDouble() on a long is absent, not 3.0");
  check(!root.field("Hrdn").asInteger().has_value(), "asInteger() on a doub is absent");
  check(!root.field("Dmtr").asDouble().has_value(), "asDouble() on a UntF is absent");
  check(!root.field("Intr").asInteger().has_value(), "asInteger() on a bool is absent");
  check(root.field("Data").asRawData() != nullptr && root.field("Nm  ").asRawData() == nullptr,
        "asRawData() accepts tdta and alis and refuses TEXT");

  // An invalid cursor is usable and propagates, so a walk into a file that
  // turns out not to contain the key is an expression, not a null-check ladder.
  check(!root.field("nope").valid(), "field() for a key that is not there is an invalid cursor");
  check(!root.field("nope").field("deeper").asUnitFloat().has_value(),
        "an invalid cursor propagates through field() and reads as absent");
  check(!DescriptorRef().valid() && DescriptorRef().childCount() == 0 &&
            DescriptorRef().path().empty(),
        "a default-constructed cursor is invalid, childless and pathless");
  check(!root.field("").valid(),
        "field(\"\") finds nothing rather than matching an unkeyed element");
  check(!root.child(11).valid(), "child() past the end is an invalid cursor, not a read");

  const std::string flatDump = dumpDescriptorTree(flatResult.tree);
  const std::string flatExpected =
      "Objc 'null' 11 items\n"
      "  'Nm  ' TEXT \"Round 5\"\n"
      "  'Dmtr' UntF #Pxl 12.5\n"
      "  'Hrdn' doub 0.75\n"
      "  'Cnt ' long -7\n"
      "  'Big ' comp 4294967296\n"
      "  'Intr' bool true\n"
      "  'bTyp' enum brushType.computed\n"
      "  'Md  ' enum BlnM.Nrml\n"
      "  'Clss' type 'brsh'\n"
      "  'Data' tdta 4 bytes deadbeef\n"
      "  'Alis' alis 3 bytes 010203\n";
  check(flatDump == flatExpected, "the whole tree dumps to the expected 12 lines");

  // ---- 2. The Key quirk ---------------------------------------------------
  //
  // Zero means four; anything else means itself; and an explicit 4 is legal and
  // is the case a reader that special-cases the number 4 gets wrong.
  DescFixture keys;
  keys.version()
      .descriptor("", "test", 4)
      .key4("aaaa").longv(1)
      .keyN("aaaa").longv(2)
      .keyN("useTipDynamics").longv(3)
      .keyN("b").longv(4);
  const DescriptorParseResult keyResult = parseGuarded(keys.bytes);
  check(keyResult.ok, "a descriptor mixing both key forms reads without refusal");
  const DescriptorRef keyRoot = keyResult.tree.root();
  check(keyRoot.childCount() == 4, "all four keyed items are present");
  check(keyRoot.child(0).key() == "aaaa" && keyRoot.child(0).asInteger() == 1,
        "a zero-length key is exactly the four characters after it");
  check(keyRoot.child(1).key() == "aaaa" && keyRoot.child(1).asInteger() == 2,
        "an explicit length of 4 decodes to the same key as the zero form");
  check(keyRoot.child(2).key() == "useTipDynamics" && keyRoot.child(2).asInteger() == 3,
        "a 14-character key decodes whole and the item after it is still aligned");
  check(keyRoot.child(3).key() == "b" && keyRoot.child(3).asInteger() == 4,
        "a 1-character key decodes, so the length is never rounded up to 4");
  check(keyRoot.field("aaaa").asInteger() == 1, "field() returns the first item with the key");

  // ---- 3. A nested Objc inside a VlLs inside the root --------------------
  DescFixture nested;
  nested.version()
      .descriptor("", "null", 3)
      .key4("Nm  ").textv("Tip")
      .key4("lst ").code("VlLs").u32v(3)
      .longv(7)
      .code("Objc").descriptor("", "brsh", 2)
      .key4("Dmtr").untf("#Pxl", 30.0)
      .keyN("useTipDynamics").boolv(true)
      .textv("tail")
      // 'GlbO' is framed exactly like 'Objc' and is a distinct type only so
      // that a consumer round-tripping a descriptor puts back the code it read.
      // Exercised here rather than left as a case label nothing reaches.
      .key4("Glbl").code("GlbO").descriptor("", "glob", 1)
      .key4("x   ").longv(9);

  const DescriptorParseResult nestedResult = parseGuarded(nested.bytes);
  check(nestedResult.ok, "an Objc nested inside a VlLs reads without refusal");
  const DescriptorRef list = nestedResult.tree.root().field("lst ");
  check(list.type() == DescriptorType::List && list.childCount() == 3,
        "the VlLs holds its three declared elements");
  check(list.child(0).key().empty() && list.child(0).asInteger() == 7,
        "a list element has no key of its own and still carries its value");
  const DescriptorRef inner = list.child(1);
  check(inner.type() == DescriptorType::Descriptor && inner.classId() == "brsh",
        "the list's second element is a nested descriptor with its own class id");
  const auto innerDmtr = inner.field("Dmtr").asUnitFloat();
  check(innerDmtr.has_value() && innerDmtr->value == 30.0,
        "a keyed item inside the nested descriptor reads through two levels");
  check(inner.field("useTipDynamics").asBoolean() == std::optional<bool>(true),
        "a long key inside a nested descriptor inside a list stays aligned");
  check(list.child(2).asText() == std::optional<std::string_view>("tail"),
        "the element after the nested descriptor is still aligned");
  check(inner.field("useTipDynamics").path() == "lst /1/useTipDynamics",
        "path() names list elements by index and keyed items by key");
  check(nestedResult.tree.root().field("lst ").child(1).field("Dmtr").path() == "lst /1/Dmtr",
        "path() is built from parent links, so it survives any walk order");

  const DescriptorRef global = nestedResult.tree.root().field("Glbl");
  check(global.type() == DescriptorType::GlobalObject && global.classId() == "glob" &&
            global.field("x   ").asInteger() == std::optional<int32_t>(9),
        "GlbO is framed like Objc and keeps its own distinct type");
  check(std::string(descriptorTypeKey(DescriptorType::GlobalObject)) == "GlbO" &&
            std::string(descriptorTypeKey(DescriptorType::Descriptor)) == "Objc" &&
            std::string(descriptorTypeName(DescriptorType::List)) == "list",
        "descriptorTypeKey()/Name() report the wire code and the English name");

  const std::string nestedExpected =
      "Objc 'null' 3 items\n"
      "  'Nm  ' TEXT \"Tip\"\n"
      "  'lst ' VlLs 3 items\n"
      "    [0] long 7\n"
      "    [1] Objc 'brsh' 2 items\n"
      "      'Dmtr' UntF #Pxl 30\n"
      "      'useTipDynamics' bool true\n"
      "    [2] TEXT \"tail\"\n"
      "  'Glbl' GlbO 'glob' 1 items\n"
      "    'x   ' long 9\n";
  check(dumpDescriptorTree(nestedResult.tree) == nestedExpected,
        "the nested tree dumps depth-first in file order");

  // ---- 4. Framing: the version word, empties, and trailing bytes ----------
  DescFixture badVersion;
  badVersion.u32v(15).descriptor("", "null", 0);
  const DescriptorParseResult badVersionResult = parseGuarded(badVersion.bytes);
  check(refusalIsWellFormed(badVersionResult) && errorMentions(badVersionResult, "is 15") &&
            errorMentions(badVersionResult, "16"),
        "a descriptor version other than 16 is refused, naming both numbers");

  DescFixture empty;
  const DescriptorParseResult emptyResult = parseGuarded(empty.bytes);
  check(!emptyResult.ok && emptyResult.error.find("4-byte version word") != std::string::npos,
        "an empty buffer is refused by name, not read");

  DescFixture noItems;
  noItems.version().descriptor("brush preset", "null", 0);
  const DescriptorParseResult noItemsResult = parseGuarded(noItems.bytes);
  check(noItemsResult.ok && noItemsResult.tree.root().childCount() == 0 &&
            noItemsResult.tree.root().className() == "brush preset",
        "a descriptor with zero items is well-formed and keeps its class name");

  DescFixture emptyList;
  emptyList.version().descriptor("", "null", 1).key4("lst ").code("VlLs").u32v(0);
  const DescriptorParseResult emptyListResult = parseGuarded(emptyList.bytes);
  check(emptyListResult.ok && emptyListResult.tree.root().field("lst ").childCount() == 0,
        "a VlLs with zero elements is well-formed and empty");

  // A descriptor is nearly always embedded in a container with its own framing
  // and padding after it, so trailing bytes are reported rather than refused --
  // that is how io/AbrImport will find the next 8BIM block.
  DescFixture trailing = flat;
  trailing.u8v(0xFF).u8v(0xFF).u8v(0xFF).u8v(0xFF).u8v(0xFF).u8v(0xFF).u8v(0xFF);
  const DescriptorParseResult trailingResult = parseGuarded(trailing.bytes);
  check(trailingResult.ok && trailingResult.bytesConsumed == flat.bytes.size(),
        "trailing bytes are not an error and bytesConsumed excludes them");

  // The bare entry point, for a caller whose container already ate the version.
  std::vector<uint8_t> bare(flat.bytes.begin() + 4, flat.bytes.end());
  const DescriptorParseResult bareResult = parseGuarded(bare, {}, /*versioned=*/false);
  check(bareResult.ok && bareResult.bytesConsumed == bare.size() &&
            dumpDescriptorTree(bareResult.tree) == flatExpected,
        "parseActionDescriptor() reads the same bytes without the version word");

  // ---- 5. UTF-16 that is not well-formed ---------------------------------
  //
  // A brush preset whose name has one bad code unit is still a brush preset,
  // so this is repaired and reported rather than refused -- the one place in
  // this module where "name what was lost" produces a warning instead of a
  // refusal, because nothing about the *file's framing* is in doubt.
  DescFixture surrogates;
  surrogates.version()
      .descriptor("", "null", 3)
      .key4("Pair").code("TEXT").unicodeUnits({0xD83D, 0xDC0D, 0})   // U+1F40D
      .key4("Lone").code("TEXT").unicodeUnits({0xD83D, 0x0041, 0})   // high, then 'A'
      .key4("Tail").code("TEXT").unicodeUnits({0x0041, 0xDC0D, 0});  // 'A', then low
  const DescriptorParseResult surrogateResult = parseGuarded(surrogates.bytes);
  check(surrogateResult.ok, "a string with unpaired surrogates parses rather than refusing");
  const auto pair = surrogateResult.tree.root().field("Pair").asText();
  check(pair.has_value() && *pair == "\xF0\x9F\x90\x8D",
        "a valid surrogate pair becomes one 4-byte UTF-8 code point");
  const auto lone = surrogateResult.tree.root().field("Lone").asText();
  // Split literal: "\xBDA" would be read as one out-of-range hex escape.
  check(lone.has_value() && *lone == "\xEF\xBF\xBD" "A",
        "an unpaired high surrogate becomes U+FFFD and the unit after it survives");
  const auto tail = surrogateResult.tree.root().field("Tail").asText();
  check(tail.has_value() && *tail == "A\xEF\xBF\xBD",
        "an unpaired low surrogate becomes U+FFFD");
  check(surrogateResult.warnings.size() == 2,
        "one warning per damaged string, not one per damaged code unit");
  check(surrogateResult.warnings[0].find("'Lone'") != std::string::npos &&
            surrogateResult.warnings[0].find("byte") != std::string::npos,
        "a warning names the item and a byte offset, for the PRD G9 report");

  // ---- 6. Truncation, exhaustively ---------------------------------------
  //
  // Every proper prefix of every good fixture, each parsed out of its own
  // guarded mapping. This is the test the module exists for: 0 of these may
  // succeed, none may read past the buffer, and every refusal must be this
  // module's own sentence rather than a crash or a bare `false`.
  size_t truncationsTried = 0;
  size_t truncationsRefusedWell = 0;
  size_t truncationsAccepted = 0;
  for (const std::vector<uint8_t>* fixture : {&flat.bytes, &nested.bytes, &keys.bytes}) {
    for (size_t n = 0; n < fixture->size(); ++n) {
      const std::vector<uint8_t> prefix(fixture->begin(), fixture->begin() + static_cast<long>(n));
      const DescriptorParseResult r = parseGuarded(prefix);
      ++truncationsTried;
      if (r.ok) ++truncationsAccepted;
      // The under-4-byte prefixes are refused by the version check, whose
      // wording is its own; everything else is a "descriptor refused:".
      else if (refusalIsWellFormed(r) || n < 4)
        ++truncationsRefusedWell;
    }
  }
  std::printf("[selftest] descriptor: %zu truncated prefixes parsed against a guard page\n",
              truncationsTried);
  check(truncationsTried == flat.bytes.size() + nested.bytes.size() + keys.bytes.size(),
        "every proper prefix of all three good fixtures was tried");
  check(truncationsAccepted == 0, "no truncated descriptor is accepted");
  check(truncationsRefusedWell == truncationsTried,
        "every truncation is refused with this module's own named sentence");

  // ---- 7. Hostile field values -------------------------------------------
  //
  // Each of these is the good fixture with exactly one field replaced by a
  // number chosen to make a careless reader allocate or read gigabytes.
  DescFixture hugeItemCount;
  hugeItemCount.version().descriptor("", "null", 0xFFFFFFFFu);
  const DescriptorParseResult hugeItemCountResult = parseGuarded(hugeItemCount.bytes);
  check(refusalIsWellFormed(hugeItemCountResult) &&
            errorMentions(hugeItemCountResult, "4294967295 items"),
        "an item count of 2^32-1 is refused before anything is reserved");

  DescFixture hugeListCount;
  hugeListCount.version().descriptor("", "null", 1).key4("lst ").code("VlLs").u32v(0xFFFFFFFFu);
  check(refusalIsWellFormed(parseGuarded(hugeListCount.bytes)),
        "a VlLs element count of 2^32-1 is refused before anything is reserved");

  DescFixture hugeRawLength;
  hugeRawLength.version().descriptor("", "null", 1).key4("Data").code("tdta").u32v(0xFFFFFFFFu);
  const DescriptorParseResult hugeRawResult = parseGuarded(hugeRawLength.bytes);
  check(refusalIsWellFormed(hugeRawResult) && errorMentions(hugeRawResult, "4294967295 bytes"),
        "a tdta length of 2^32-1 is refused before anything is allocated");

  DescFixture hugeTextLength;
  hugeTextLength.version().descriptor("", "null", 1).key4("Nm  ").code("TEXT").u32v(0x40000000u);
  const DescriptorParseResult hugeTextResult = parseGuarded(hugeTextLength.bytes);
  check(refusalIsWellFormed(hugeTextResult) &&
            errorMentions(hugeTextResult, "UTF-16 code units"),
        "a UnicodeString length whose byte count overflows nothing is still refused");

  DescFixture hugeKeyLength;
  hugeKeyLength.version().descriptor("", "null", 1).u32v(0xFFFFFFFFu).code("long").u32v(0);
  const DescriptorParseResult hugeKeyResult = parseGuarded(hugeKeyLength.bytes);
  check(refusalIsWellFormed(hugeKeyResult) &&
            errorMentions(hugeKeyResult, "declares a length of 4294967295"),
        "a key length of 2^32-1 is refused, naming the zero-means-four rule");

  DescFixture hugeClassIdLength;
  hugeClassIdLength.version().unicode("").u32v(0x7FFFFFFFu);
  check(refusalIsWellFormed(parseGuarded(hugeClassIdLength.bytes)),
        "an oversized classId length is refused at the descriptor header");

  // ---- 8. Types that cannot be skipped ------------------------------------
  DescFixture reference;
  reference.version().descriptor("", "null", 1).key4("Rfrn").code("obj ").u32v(1);
  const DescriptorParseResult referenceResult = parseGuarded(reference.bytes);
  check(refusalIsWellFormed(referenceResult) && errorMentions(referenceResult, "'obj '") &&
            errorMentions(referenceResult, "'Rfrn'") &&
            errorMentions(referenceResult, "object reference"),
        "an 'obj ' reference is refused by name, naming the item it was under");
  check(errorMentions(referenceResult, "no length in front of a value"),
        "the refusal says why an unknown type cannot simply be skipped");

  DescFixture junkType;
  junkType.version().descriptor("", "null", 1).key4("Junk").code("\x01\x02Q~").u32v(0);
  const DescriptorParseResult junkTypeResult = parseGuarded(junkType.bytes);
  check(refusalIsWellFormed(junkTypeResult) && errorMentions(junkTypeResult, "\\x01\\x02Q~"),
        "an unprintable type key is escaped into the refusal, never pasted raw");

  DescFixture objArray;
  objArray.version().descriptor("", "null", 1).key4("Arr ").code("ObAr").u32v(0);
  check(errorMentions(parseGuarded(objArray.bytes), "object array"),
        "'ObAr' is refused as a named deferral rather than as a mystery");

  // ---- 9. Depth and node budgets -----------------------------------------
  //
  // The parser is iterative and has no stack to overflow; the cap is a promise
  // to consumers, which will walk the result recursively. Both sides of it are
  // asserted, because a cap that refused a legal file would be worse than the
  // crash it prevents.
  auto nestChain = [](uint32_t depth) {
    DescFixture f;
    f.version();
    for (uint32_t i = 0; i + 1 < depth; ++i) f.descriptor("", "obj0", 1).key4("nest").code("Objc");
    f.descriptor("", "leaf", 0);
    return f.bytes;
  };
  const DescriptorParseResult deepOk = parseGuarded(nestChain(60));
  check(deepOk.ok && deepOk.tree.nodeCount() == 60,
        "60 nested descriptors parse, one node each, under the default cap of 64");
  const DescriptorParseResult deepBomb = parseGuarded(nestChain(4000));
  check(refusalIsWellFormed(deepBomb) && errorMentions(deepBomb, "maxDepth"),
        "4000 nested descriptors are refused by name at the depth cap");
  check(errorMentions(deepBomb, "65 containers deep"),
        "the depth refusal names the level it stopped at, not just the limit");

  DescriptorParseOptions tightNodes;
  tightNodes.maxNodes = 4;
  const DescriptorParseResult nodeCapped = parseGuarded(flat.bytes, tightNodes);
  check(refusalIsWellFormed(nodeCapped) && errorMentions(nodeCapped, "maxNodes"),
        "maxNodes refuses in this module's words rather than in the allocator's");

  DescriptorParseOptions zeroDepth;
  zeroDepth.maxDepth = 0;
  const DescriptorParseResult zeroDepthResult = parseGuarded(flat.bytes, zeroDepth);
  check(!zeroDepthResult.ok && errorMentions(zeroDepthResult, "caller error"),
        "maxDepth of 0 is reported as a caller error, not blamed on the file");

  // ---- 10. The guard itself ----------------------------------------------
  check(guardMapFailures == 0, "every fixture was mapped with a PROT_NONE page after it");
  check(guardedParses > 300, "every parse in this section ran against that guard page");
  std::printf("[selftest] descriptor: %zu guarded parses, 0 bytes read past any buffer end\n",
              guardedParses);

  std::printf("[selftest] descriptor %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
