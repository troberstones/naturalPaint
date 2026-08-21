#include "io/Descriptor.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

namespace np {
namespace {

// The smallest an item can be on the wire, used only to turn an absurd
// declared count into this module's own refusal instead of a loop that
// discovers the truth one bounds check later.
//
// A keyed item is a uint32 length, at least one byte of key (a zero length
// means four, so the shortest possible key text is the one-byte explicit case)
// and the four-character type. A list element has no key at all, so it is the
// four type bytes and, for `bool`, nothing else.
//
// Deliberately a *lower* bound rather than a realistic one: this guard exists
// to refuse the impossible, and a guard that refused a legal file would be a
// far worse bug than the allocation it was avoiding.
constexpr size_t kMinKeyedItemBytes = 9;
constexpr size_t kMinListItemBytes = 4;

// Renders a four-character type key, or any key text, safely into a message.
// The bytes come from an untrusted file and may be anything at all, including
// a newline that would break a message into two lines or a NUL that would end
// one early.
std::string printable(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x20 && u < 0x7F) {
      out.push_back(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", u);
      out += buf;
    }
  }
  return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80u) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

// One open container during the parse. The parser keeps a vector of these
// instead of recursing -- see Descriptor.hpp on why neither the parse nor the
// destruction of the result is allowed to use the C stack for depth that comes
// out of a file.
struct Frame {
  uint32_t node = 0;
  uint32_t remaining = 0;
  bool keyed = true;
};

struct Parser {
  std::span<const uint8_t> b;
  size_t pos = 0;
  DescriptorParseOptions opt;

  DescriptorTree tree;
  std::vector<std::string> warnings;
  std::string error;

  // What is currently being read, in the middle of a sentence: "the class id of
  // item 'Brsh'". Set before each read group so that the one truncation message
  // below can name the thing rather than the byte.
  std::string ctx;

  size_t left() const noexcept { return b.size() - pos; }

  bool fail(std::string message) {
    error = std::move(message);
    return false;
  }

  // The single truncation refusal. Every read in this file goes through it, so
  // there is exactly one place that can get the comparison wrong, and it is
  // this one.
  bool take(size_t n) {
    if (left() >= n) return true;
    return fail("descriptor refused: reading " + ctx + " at byte " + std::to_string(pos) +
                " needs " + std::to_string(n) + " more byte" + (n == 1 ? "" : "s") +
                ", but only " + std::to_string(left()) + " of this " +
                std::to_string(b.size()) +
                "-byte buffer remain. The descriptor is truncated. Nothing was parsed: an "
                "Action Descriptor puts no length in front of a value, so a reader that "
                "loses its place has nothing to resynchronise on and every byte after the "
                "cut would be read as some other type's payload.");
  }

  bool readRaw(void* dst, size_t n) {
    if (!take(n)) return false;
    if (n != 0) std::memcpy(dst, b.data() + pos, n);
    pos += n;
    return true;
  }

  bool readU8(uint8_t* out) {
    if (!take(1)) return false;
    *out = b[pos++];
    return true;
  }

  bool readU16(uint16_t* out) {
    uint8_t v[2];
    if (!readRaw(v, 2)) return false;
    *out = static_cast<uint16_t>((static_cast<uint16_t>(v[0]) << 8) | v[1]);
    return true;
  }

  bool readU32(uint32_t* out) {
    uint8_t v[4];
    if (!readRaw(v, 4)) return false;
    *out = (static_cast<uint32_t>(v[0]) << 24) | (static_cast<uint32_t>(v[1]) << 16) |
           (static_cast<uint32_t>(v[2]) << 8) | static_cast<uint32_t>(v[3]);
    return true;
  }

  bool readU64(uint64_t* out) {
    uint8_t v[8];
    if (!readRaw(v, 8)) return false;
    uint64_t r = 0;
    for (const uint8_t byte : v) r = (r << 8) | byte;
    *out = r;
    return true;
  }

  // Big-endian IEEE-754 binary64, as a bit pattern rather than a conversion --
  // the same reason io/OpSerial writes bit patterns: a decimal round trip is a
  // nearly exact one, and a brush diameter that is nearly right is a brush that
  // paints a different mark.
  bool readF64(double* out) {
    uint64_t bits = 0;
    if (!readU64(&bits)) return false;
    static_assert(sizeof(double) == sizeof(uint64_t));
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  // The Key quirk, in one place. See Descriptor.hpp: a length of zero means
  // exactly four bytes of key text, any other length means itself.
  bool readKey(std::string* out) {
    const size_t at = pos;
    uint32_t declared = 0;
    if (!readU32(&declared)) return false;
    const size_t n = (declared == 0) ? 4u : static_cast<size_t>(declared);
    if (n > left()) {
      return fail("descriptor refused: " + ctx + " at byte " + std::to_string(at) +
                  " declares a length of " + std::to_string(declared) + ", so its text needs " +
                  std::to_string(n) + " bytes, but only " + std::to_string(left()) +
                  " of this " + std::to_string(b.size()) +
                  "-byte buffer remain. A key is either a length of 0 followed by exactly "
                  "four characters, or an explicit length followed by that many -- this one "
                  "is neither, so the reader cannot tell where the key ends, let alone where "
                  "the value after it begins. Nothing was parsed.");
    }
    out->assign(reinterpret_cast<const char*>(b.data() + pos), n);
    pos += n;
    return true;
  }

  // A Photoshop UnicodeString: a uint32 count of UTF-16 code units, then that
  // many big-endian units. Returned transcoded to UTF-8, because every other
  // string in this codebase is UTF-8 and a second convention would leak into
  // io/BrushIR, the layer name, and eventually the UI.
  //
  // Photoshop's writer includes a trailing NUL in the count. Exactly one
  // trailing NUL is dropped; any others are kept, because a NUL in the middle
  // of a name is data this reader has no business deciding about.
  bool readUnicodeString(std::string* out) {
    const size_t at = pos;
    uint32_t units = 0;
    if (!readU32(&units)) return false;
    if (static_cast<size_t>(units) > left() / 2) {
      return fail("descriptor refused: " + ctx + " at byte " + std::to_string(at) +
                  " declares " + std::to_string(units) + " UTF-16 code units, which is " +
                  std::to_string(static_cast<uint64_t>(units) * 2) + " bytes, but only " +
                  std::to_string(left()) + " of this " + std::to_string(b.size()) +
                  "-byte buffer remain. Nothing was parsed and nothing was allocated for it.");
    }

    std::string text;
    text.reserve(static_cast<size_t>(units));
    size_t replaced = 0;
    uint32_t i = 0;
    while (i < units) {
      uint16_t hi = 0;
      if (!readU16(&hi)) return false;  // cannot fire: bounded above
      ++i;
      uint32_t cp = hi;
      if (hi >= 0xD800u && hi <= 0xDBFFu) {
        // A high surrogate must be followed by a low one. Anything else is a
        // malformed string rather than a malformed *file*, so it is repaired
        // and reported instead of refused -- a brush preset whose name has one
        // bad code unit is still a brush preset.
        if (i < units) {
          const size_t save = pos;
          uint16_t lo = 0;
          if (!readU16(&lo)) return false;
          if (lo >= 0xDC00u && lo <= 0xDFFFu) {
            ++i;
            cp = 0x10000u + ((static_cast<uint32_t>(hi) - 0xD800u) << 10) +
                 (static_cast<uint32_t>(lo) - 0xDC00u);
          } else {
            pos = save;  // the low half was somebody else's code unit
            cp = 0xFFFDu;
            ++replaced;
          }
        } else {
          cp = 0xFFFDu;
          ++replaced;
        }
      } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
        cp = 0xFFFDu;  // an unpaired low surrogate
        ++replaced;
      }
      appendUtf8(text, cp);
    }

    if (!text.empty() && text.back() == '\0') text.pop_back();

    // One warning per string, carrying the count. A file with a million lone
    // surrogates must not produce a million std::strings of complaint -- the
    // report is meant to be read.
    if (replaced != 0) {
      warnings.push_back("descriptor: " + ctx + " at byte " + std::to_string(at) + " contained " +
                         std::to_string(replaced) +
                         " unpaired UTF-16 surrogate code unit" + (replaced == 1 ? "" : "s") +
                         ", each replaced with U+FFFD. The rest of the string is intact.");
    }

    *out = std::move(text);
    return true;
  }

  // "" for the root, "Brsh/dynamics/1/useTipDynamics" for a keyed item inside
  // the second element of a list. Built from parent links rather than tracked,
  // so it cannot drift out of step with the tree.
  std::string pathOf(uint32_t index) const {
    if (index == kDescriptorNoParent || index >= tree.nodes.size()) return {};
    std::vector<std::string> parts;
    uint32_t cur = index;
    while (cur < tree.nodes.size()) {
      const DescriptorNode& n = tree.nodes[cur];
      const uint32_t parent = n.parent;
      if (parent == kDescriptorNoParent) break;
      if (!n.key.empty()) {
        parts.push_back(printable(n.key));
      } else {
        const DescriptorNode& p = tree.nodes[parent];
        size_t ordinal = 0;
        for (size_t i = 0; i < p.children.size(); ++i) {
          if (p.children[i] == cur) {
            ordinal = i;
            break;
          }
        }
        parts.push_back(std::to_string(ordinal));
      }
      cur = parent;
    }
    std::string out;
    for (size_t i = parts.size(); i-- > 0;) {
      if (!out.empty()) out.push_back('/');
      out += parts[i];
    }
    return out;
  }

  // How to name a container in a message when it is the root and has no path.
  std::string describeContainer(uint32_t index) const {
    const std::string p = pathOf(index);
    return p.empty() ? std::string("the root descriptor") : ("'" + p + "'");
  }

  bool newNode(uint32_t parent, DescriptorType type, std::string key, size_t offset,
               uint32_t* out) {
    if (tree.nodes.size() >= static_cast<size_t>(opt.maxNodes)) {
      return fail("descriptor refused: this descriptor holds more than " +
                  std::to_string(opt.maxNodes) +
                  " items (DescriptorParseOptions::maxNodes), reached at byte " +
                  std::to_string(offset) +
                  ". Nothing was parsed. That is far past anything Photoshop writes, so this "
                  "is either a corrupt file or one built to exhaust memory; raise the option "
                  "deliberately if a real file genuinely needs it.");
    }
    DescriptorNode node;
    node.type = type;
    node.key = std::move(key);
    node.parent = parent;
    node.offset = offset;
    tree.nodes.push_back(std::move(node));
    *out = static_cast<uint32_t>(tree.nodes.size() - 1);
    if (parent != kDescriptorNoParent) tree.nodes[parent].children.push_back(*out);
    return true;
  }

  // Declared counts are checked against what is actually left before the loop
  // that would consume them, so an absurd count refuses in this module's words
  // rather than by running out of buffer four bytes later with a message about
  // a type key.
  bool checkCount(uint32_t count, size_t minBytesEach, uint32_t node) {
    if (static_cast<size_t>(count) <= left() / minBytesEach) return true;
    return fail("descriptor refused: " + describeContainer(node) + " at byte " +
                std::to_string(tree.nodes[node].offset) + " declares " + std::to_string(count) +
                " items, which need at least " +
                std::to_string(static_cast<uint64_t>(count) * minBytesEach) +
                " bytes, but only " + std::to_string(left()) + " of this " +
                std::to_string(b.size()) +
                "-byte buffer remain. Nothing was parsed and nothing was reserved for it.");
  }

  bool readDescriptorBody(uint32_t node, std::vector<Frame>& frames) {
    const std::string where = describeContainer(node);
    ctx = "the class name of " + where;
    if (!readUnicodeString(&tree.nodes[node].className)) return false;
    ctx = "the class id of " + where;
    std::string classId;
    if (!readKey(&classId)) return false;
    tree.nodes[node].classId = std::move(classId);
    ctx = "the item count of " + where;
    uint32_t count = 0;
    if (!readU32(&count)) return false;
    if (!checkCount(count, kMinKeyedItemBytes, node)) return false;
    if (!pushFrame(frames, Frame{node, count, true})) return false;
    return true;
  }

  bool pushFrame(std::vector<Frame>& frames, Frame frame) {
    if (frames.size() >= static_cast<size_t>(opt.maxDepth)) {
      return fail("descriptor refused: " + describeContainer(frame.node) + " at byte " +
                  std::to_string(tree.nodes[frame.node].offset) + " would nest " +
                  std::to_string(frames.size() + 1) + " containers deep, past the limit of " +
                  std::to_string(opt.maxDepth) +
                  " (DescriptorParseOptions::maxDepth). Nothing was parsed. This reader itself "
                  "is iterative and has no stack to overflow, but anything that walks the "
                  "result recursively does, so the limit is enforced here where the refusal "
                  "can still name a byte offset.");
    }
    // A container declaring zero items is well formed and simply has no
    // children; pushing it and popping it on the next turn of the loop is
    // simpler than a special case, and costs one iteration.
    frames.push_back(frame);
    return true;
  }

  bool run() {
    uint32_t rootIndex = 0;
    if (!newNode(kDescriptorNoParent, DescriptorType::Descriptor, std::string(), pos,
                 &rootIndex))
      return false;

    std::vector<Frame> frames;
    if (!readDescriptorBody(rootIndex, frames)) return false;

    while (!frames.empty()) {
      if (frames.back().remaining == 0) {
        frames.pop_back();
        continue;
      }
      // Everything needed from the frame is copied out *before* any push, which
      // would reallocate the vector and dangle a reference to it.
      const uint32_t parentIndex = frames.back().node;
      const bool keyed = frames.back().keyed;
      const size_t ordinal = tree.nodes[parentIndex].children.size();
      frames.back().remaining -= 1;

      const std::string parentWhere = describeContainer(parentIndex);
      std::string key;
      if (keyed) {
        ctx = "the key of item " + std::to_string(ordinal) + " of " + parentWhere;
        if (!readKey(&key)) return false;
      }

      const size_t typeOffset = pos;
      char code[4] = {};
      ctx = keyed ? ("the type of item '" + printable(key) + "' of " + parentWhere)
                  : ("the type of element " + std::to_string(ordinal) + " of " + parentWhere);
      if (!readRaw(code, 4)) return false;
      const std::string_view type(code, 4);

      const std::string valueOf =
          keyed ? ("the value of item '" + printable(key) + "' of " + parentWhere)
                : ("the value of element " + std::to_string(ordinal) + " of " + parentWhere);

      uint32_t index = 0;
      auto make = [&](DescriptorType t) { return newNode(parentIndex, t, key, typeOffset, &index); };

      if (type == "Objc" || type == "GlbO") {
        if (!make(type == "Objc" ? DescriptorType::Descriptor : DescriptorType::GlobalObject))
          return false;
        if (!readDescriptorBody(index, frames)) return false;
      } else if (type == "VlLs") {
        if (!make(DescriptorType::List)) return false;
        ctx = "the element count of " + describeContainer(index);
        uint32_t count = 0;
        if (!readU32(&count)) return false;
        if (!checkCount(count, kMinListItemBytes, index)) return false;
        if (!pushFrame(frames, Frame{index, count, false})) return false;
      } else if (type == "doub") {
        if (!make(DescriptorType::Double)) return false;
        ctx = valueOf;
        double v = 0.0;
        if (!readF64(&v)) return false;
        tree.nodes[index].payload = v;
      } else if (type == "UntF") {
        if (!make(DescriptorType::UnitFloat)) return false;
        ctx = "the unit of " + valueOf;
        char unit[4] = {};
        if (!readRaw(unit, 4)) return false;
        ctx = valueOf;
        double v = 0.0;
        if (!readF64(&v)) return false;
        tree.nodes[index].payload = DescriptorUnitFloat{std::string(unit, 4), v};
      } else if (type == "TEXT") {
        if (!make(DescriptorType::Text)) return false;
        ctx = valueOf;
        std::string text;
        if (!readUnicodeString(&text)) return false;
        tree.nodes[index].payload = std::move(text);
      } else if (type == "enum") {
        if (!make(DescriptorType::Enumerated)) return false;
        DescriptorEnumerated e;
        ctx = "the enumeration id of " + valueOf;
        if (!readKey(&e.typeId)) return false;
        ctx = "the enumerated value of " + valueOf;
        if (!readKey(&e.valueId)) return false;
        tree.nodes[index].payload = std::move(e);
      } else if (type == "long") {
        if (!make(DescriptorType::Integer)) return false;
        ctx = valueOf;
        uint32_t v = 0;
        if (!readU32(&v)) return false;
        tree.nodes[index].payload = static_cast<int32_t>(v);
      } else if (type == "comp") {
        if (!make(DescriptorType::LargeInteger)) return false;
        ctx = valueOf;
        uint64_t v = 0;
        if (!readU64(&v)) return false;
        tree.nodes[index].payload = static_cast<int64_t>(v);
      } else if (type == "bool") {
        if (!make(DescriptorType::Boolean)) return false;
        ctx = valueOf;
        uint8_t v = 0;
        if (!readU8(&v)) return false;
        tree.nodes[index].payload = (v != 0);
      } else if (type == "type" || type == "GlbC") {
        if (!make(DescriptorType::Class)) return false;
        ctx = "the class name of " + valueOf;
        if (!readUnicodeString(&tree.nodes[index].className)) return false;
        ctx = "the class id of " + valueOf;
        std::string classId;
        if (!readKey(&classId)) return false;
        tree.nodes[index].classId = std::move(classId);
      } else if (type == "alis" || type == "tdta") {
        if (!make(type == "alis" ? DescriptorType::Alias : DescriptorType::RawData)) return false;
        ctx = "the length of " + valueOf;
        const size_t at = pos;
        uint32_t length = 0;
        if (!readU32(&length)) return false;
        if (static_cast<size_t>(length) > left()) {
          return fail("descriptor refused: " + valueOf + " at byte " + std::to_string(at) +
                      " declares " + std::to_string(length) + " bytes of payload, but only " +
                      std::to_string(left()) + " of this " + std::to_string(b.size()) +
                      "-byte buffer remain. Nothing was parsed and nothing was allocated for "
                      "it.");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        if (!readRaw(bytes.data(), bytes.size())) return false;
        tree.nodes[index].payload = std::move(bytes);
      } else {
        // The refusal Descriptor.hpp argues for at length. An Action Descriptor
        // has no length in front of a value, so this is genuinely unrecoverable
        // rather than merely unimplemented.
        const std::string what = printable(type);
        std::string note;
        if (type == "obj ")
          note =
              " That is an object reference -- a count followed by items of seven further "
              "shapes (prop, Clss, Enmr, rele, Idnt, indx, name).";
        else if (type == "ObAr")
          note = " That is an object array (Photoshop CS and later).";
        else if (type == "UnFl")
          note = " That is a unit-float list (Photoshop CS and later).";
        else if (type == "Pth ")
          note = " That is a file path structure.";
        return fail("descriptor refused: " + (keyed ? ("item '" + printable(key) + "'")
                                                    : ("element " + std::to_string(ordinal))) +
                    " of " + parentWhere + ", at byte " + std::to_string(typeOffset) +
                    ", has type '" + what + "', which io/Descriptor does not parse." + note +
                    " An Action Descriptor puts no length in front of a value, so an "
                    "unrecognised type cannot be stepped over -- every byte after it would be "
                    "read as some other type's payload, and the reader would report values it "
                    "invented. Nothing was parsed. io/Descriptor.hpp lists what adding this "
                    "type would take.");
      }
    }
    return true;
  }
};

DescriptorParseResult parseFrom(std::span<const uint8_t> bytes, size_t start,
                                const DescriptorParseOptions& options) {
  DescriptorParseResult result;

  // An option of zero would make every file refuse, or worse, make the depth
  // guard compare against a limit nothing can satisfy. Refused as a programming
  // error, in the caller's own terms, rather than mistaken for a bad file.
  if (options.maxDepth == 0 || options.maxNodes == 0) {
    result.error =
        "descriptor refused: DescriptorParseOptions has maxDepth " +
        std::to_string(options.maxDepth) + " and maxNodes " + std::to_string(options.maxNodes) +
        "; both must be at least 1. Nothing was parsed. This is a caller error rather than a "
        "problem with the file.";
    return result;
  }

  Parser p;
  p.b = bytes;
  p.pos = start;
  p.opt = options;
  if (!p.run()) {
    result.error = std::move(p.error);
    // The tree is deliberately dropped rather than handed back partial. See
    // Descriptor.hpp: half a brush preset paints a mark the file does not
    // describe, and nothing in the document would say so.
    return result;
  }

  result.ok = true;
  result.tree = std::move(p.tree);
  result.bytesConsumed = p.pos;
  result.warnings = std::move(p.warnings);
  return result;
}

// Renders one node's own line for dumpDescriptorTree(). Deterministic for a
// given tree: no offsets, no addresses, and doubles through %.17g, which is the
// shortest form that round-trips a binary64 exactly.
std::string dumpValue(const DescriptorNode& node) {
  char buf[64];
  switch (node.type) {
    case DescriptorType::Descriptor:
    case DescriptorType::GlobalObject: {
      std::string s = std::string(descriptorTypeKey(node.type)) + " '" + printable(node.classId) +
                      "' " + std::to_string(node.children.size()) + " items";
      if (!node.className.empty()) s += " name=\"" + printable(node.className) + "\"";
      return s;
    }
    case DescriptorType::List:
      return "VlLs " + std::to_string(node.children.size()) + " items";
    case DescriptorType::Class: {
      std::string s = "type '" + printable(node.classId) + "'";
      if (!node.className.empty()) s += " name=\"" + printable(node.className) + "\"";
      return s;
    }
    case DescriptorType::Double:
      std::snprintf(buf, sizeof(buf), "%.17g", std::get<double>(node.payload));
      return "doub " + std::string(buf);
    case DescriptorType::UnitFloat: {
      const auto& u = std::get<DescriptorUnitFloat>(node.payload);
      std::snprintf(buf, sizeof(buf), "%.17g", u.value);
      return "UntF " + printable(u.unit) + " " + buf;
    }
    case DescriptorType::Text:
      return "TEXT \"" + printable(std::get<std::string>(node.payload)) + "\"";
    case DescriptorType::Enumerated: {
      const auto& e = std::get<DescriptorEnumerated>(node.payload);
      return "enum " + printable(e.typeId) + "." + printable(e.valueId);
    }
    case DescriptorType::Integer:
      return "long " + std::to_string(std::get<int32_t>(node.payload));
    case DescriptorType::LargeInteger:
      return "comp " + std::to_string(std::get<int64_t>(node.payload));
    case DescriptorType::Boolean:
      return std::string("bool ") + (std::get<bool>(node.payload) ? "true" : "false");
    case DescriptorType::Alias:
    case DescriptorType::RawData: {
      const auto& raw = std::get<std::vector<uint8_t>>(node.payload);
      std::string s = std::string(descriptorTypeKey(node.type)) + " " +
                      std::to_string(raw.size()) + " bytes";
      if (!raw.empty()) {
        s += ' ';
        // Capped, so a megabyte of `tdta` does not become a megabyte of dump.
        const size_t shown = raw.size() < 16 ? raw.size() : 16;
        for (size_t i = 0; i < shown; ++i) {
          std::snprintf(buf, sizeof(buf), "%02x", raw[i]);
          s += buf;
        }
        if (shown < raw.size()) s += "...";
      }
      return s;
    }
  }
  return "?";
}

}  // namespace

const char* descriptorTypeKey(DescriptorType type) noexcept {
  switch (type) {
    case DescriptorType::Descriptor: return "Objc";
    case DescriptorType::GlobalObject: return "GlbO";
    case DescriptorType::List: return "VlLs";
    case DescriptorType::UnitFloat: return "UntF";
    case DescriptorType::Double: return "doub";
    case DescriptorType::Text: return "TEXT";
    case DescriptorType::Enumerated: return "enum";
    case DescriptorType::Integer: return "long";
    case DescriptorType::LargeInteger: return "comp";
    case DescriptorType::Boolean: return "bool";
    case DescriptorType::Class: return "type";
    case DescriptorType::Alias: return "alis";
    case DescriptorType::RawData: return "tdta";
  }
  return "????";
}

const char* descriptorTypeName(DescriptorType type) noexcept {
  switch (type) {
    case DescriptorType::Descriptor: return "nested descriptor";
    case DescriptorType::GlobalObject: return "global object";
    case DescriptorType::List: return "list";
    case DescriptorType::UnitFloat: return "unit float";
    case DescriptorType::Double: return "double";
    case DescriptorType::Text: return "text";
    case DescriptorType::Enumerated: return "enumerated";
    case DescriptorType::Integer: return "integer";
    case DescriptorType::LargeInteger: return "large integer";
    case DescriptorType::Boolean: return "boolean";
    case DescriptorType::Class: return "class";
    case DescriptorType::Alias: return "alias";
    case DescriptorType::RawData: return "raw data";
  }
  return "unknown";
}

DescriptorRef DescriptorTree::root() const noexcept { return DescriptorRef(this, 0); }

DescriptorRef::DescriptorRef(const DescriptorTree* tree, uint32_t index) noexcept {
  if (tree == nullptr || index >= tree->nodes.size()) return;
  tree_ = tree;
  node_ = &tree->nodes[index];
  index_ = index;
}

DescriptorType DescriptorRef::type() const noexcept {
  return node_ != nullptr ? node_->type : DescriptorType::Descriptor;
}

std::string_view DescriptorRef::key() const noexcept {
  return node_ != nullptr ? std::string_view(node_->key) : std::string_view();
}

std::string_view DescriptorRef::className() const noexcept {
  return node_ != nullptr ? std::string_view(node_->className) : std::string_view();
}

std::string_view DescriptorRef::classId() const noexcept {
  return node_ != nullptr ? std::string_view(node_->classId) : std::string_view();
}

size_t DescriptorRef::offset() const noexcept { return node_ != nullptr ? node_->offset : 0; }

std::string DescriptorRef::path() const {
  if (node_ == nullptr) return {};
  std::vector<std::string> parts;
  uint32_t cur = index_;
  while (cur < tree_->nodes.size()) {
    const DescriptorNode& n = tree_->nodes[cur];
    if (n.parent == kDescriptorNoParent || n.parent >= tree_->nodes.size()) break;
    if (!n.key.empty()) {
      parts.push_back(n.key);
    } else {
      const DescriptorNode& p = tree_->nodes[n.parent];
      size_t ordinal = 0;
      for (size_t i = 0; i < p.children.size(); ++i) {
        if (p.children[i] == cur) {
          ordinal = i;
          break;
        }
      }
      parts.push_back(std::to_string(ordinal));
    }
    cur = n.parent;
  }
  std::string out;
  for (size_t i = parts.size(); i-- > 0;) {
    if (!out.empty()) out.push_back('/');
    out += parts[i];
  }
  return out;
}

size_t DescriptorRef::childCount() const noexcept {
  return node_ != nullptr ? node_->children.size() : 0;
}

DescriptorRef DescriptorRef::child(size_t index) const noexcept {
  if (node_ == nullptr || index >= node_->children.size()) return {};
  return DescriptorRef(tree_, node_->children[index]);
}

DescriptorRef DescriptorRef::field(std::string_view key) const noexcept {
  if (node_ == nullptr || key.empty()) return {};
  for (const uint32_t childIndex : node_->children) {
    if (childIndex < tree_->nodes.size() && tree_->nodes[childIndex].key == key)
      return DescriptorRef(tree_, childIndex);
  }
  return {};
}

std::optional<double> DescriptorRef::asDouble() const noexcept {
  if (node_ == nullptr || node_->type != DescriptorType::Double) return std::nullopt;
  return std::get<double>(node_->payload);
}

std::optional<DescriptorUnitFloat> DescriptorRef::asUnitFloat() const {
  if (node_ == nullptr || node_->type != DescriptorType::UnitFloat) return std::nullopt;
  return std::get<DescriptorUnitFloat>(node_->payload);
}

std::optional<int32_t> DescriptorRef::asInteger() const noexcept {
  if (node_ == nullptr || node_->type != DescriptorType::Integer) return std::nullopt;
  return std::get<int32_t>(node_->payload);
}

std::optional<int64_t> DescriptorRef::asLargeInteger() const noexcept {
  if (node_ == nullptr || node_->type != DescriptorType::LargeInteger) return std::nullopt;
  return std::get<int64_t>(node_->payload);
}

std::optional<bool> DescriptorRef::asBoolean() const noexcept {
  if (node_ == nullptr || node_->type != DescriptorType::Boolean) return std::nullopt;
  return std::get<bool>(node_->payload);
}

std::optional<DescriptorEnumerated> DescriptorRef::asEnumerated() const {
  if (node_ == nullptr || node_->type != DescriptorType::Enumerated) return std::nullopt;
  return std::get<DescriptorEnumerated>(node_->payload);
}

std::optional<std::string_view> DescriptorRef::asText() const noexcept {
  if (node_ == nullptr || node_->type != DescriptorType::Text) return std::nullopt;
  return std::string_view(std::get<std::string>(node_->payload));
}

const std::vector<uint8_t>* DescriptorRef::asRawData() const noexcept {
  if (node_ == nullptr ||
      (node_->type != DescriptorType::RawData && node_->type != DescriptorType::Alias))
    return nullptr;
  return &std::get<std::vector<uint8_t>>(node_->payload);
}

DescriptorParseResult parseActionDescriptor(std::span<const uint8_t> bytes,
                                            const DescriptorParseOptions& options) {
  return parseFrom(bytes, 0, options);
}

DescriptorParseResult parseVersionedActionDescriptor(std::span<const uint8_t> bytes,
                                                     const DescriptorParseOptions& options) {
  DescriptorParseResult result;
  if (bytes.size() < 4) {
    result.error = "descriptor refused: a versioned Action Descriptor begins with a 4-byte "
                   "version word, but this buffer is only " +
                   std::to_string(bytes.size()) + " byte" + (bytes.size() == 1 ? "" : "s") +
                   " long. Nothing was parsed.";
    return result;
  }
  const uint32_t version = (static_cast<uint32_t>(bytes[0]) << 24) |
                           (static_cast<uint32_t>(bytes[1]) << 16) |
                           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
  if (version != kActionDescriptorVersion) {
    result.error =
        "descriptor refused: the descriptor version word at byte 0 is " +
        std::to_string(version) + ", and the only version Adobe has ever written, and the only "
        "one this reader knows the framing of, is " +
        std::to_string(kActionDescriptorVersion) +
        ". Nothing was parsed. The version exists precisely to say the framing changed, and "
        "the framing is the only thing this format gives a reader -- there are no lengths in "
        "front of values -- so reading on would produce values invented rather than read.";
    return result;
  }
  return parseFrom(bytes, 4, options);
}

std::string dumpDescriptorTree(const DescriptorTree& tree) {
  if (tree.empty()) return {};
  std::string out;
  // Iterative, for the same reason the parser is: this runs on trees that came
  // out of files, and a dump must not be the thing that overflows the stack.
  struct Item {
    uint32_t index;
    uint32_t depth;
  };
  std::vector<Item> stack{{0u, 0u}};
  while (!stack.empty()) {
    const Item item = stack.back();
    stack.pop_back();
    if (item.index >= tree.nodes.size()) continue;
    const DescriptorNode& node = tree.nodes[item.index];
    out.append(static_cast<size_t>(item.depth) * 2, ' ');
    if (node.parent != kDescriptorNoParent) {
      if (!node.key.empty()) {
        out += "'" + printable(node.key) + "' ";
      } else {
        size_t ordinal = 0;
        if (node.parent < tree.nodes.size()) {
          const DescriptorNode& p = tree.nodes[node.parent];
          for (size_t i = 0; i < p.children.size(); ++i)
            if (p.children[i] == item.index) {
              ordinal = i;
              break;
            }
        }
        out += "[" + std::to_string(ordinal) + "] ";
      }
    }
    out += dumpValue(node);
    out.push_back('\n');
    for (size_t i = node.children.size(); i-- > 0;)
      stack.push_back({node.children[i], item.depth + 1});
  }
  return out;
}

}  // namespace np
