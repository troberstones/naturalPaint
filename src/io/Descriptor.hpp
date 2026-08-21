#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// io/Descriptor (PLAN.md "12 -- Import brushes", first bullet: "`io/Descriptor`
// -- Action Descriptor reader (`Objc`/`VlLs`/`UntF`/`doub`/`TEXT`/`enum`),
// reusable for PSD later"). PRD G7 (".abr import including dynamics, not tips
// alone") and PRD G9 ("Import emits a report naming everything dropped rather
// than silently approximating").
//
// **This module parses bytes this project did not write and cannot trust.**
// That single sentence decides almost every other decision in this header, and
// it is the difference between this module and io/OpSerial, which parses a
// payload this build itself produced and stored inside its own file. An `.abr`
// arrives from a marketplace, a forum post, or a zip somebody mailed the user.
// So the module's first-order property is not fidelity, and not coverage of
// Adobe's type zoo -- it is that **no input, however hostile, causes a read
// outside the caller's buffer.** Everything below either serves that or gets
// out of its way.
//
// --- What an Action Descriptor is ----------------------------------------
//
// Adobe's key/value tree. It is what Photoshop's scripting layer passes around,
// and it is also how a `.abr` brush preset and a good half of a modern `.psd`'s
// layer metadata are actually stored, which is why one reader serves both and
// why PLAN.md says "reusable for PSD later" rather than putting this in
// `brush/`.
//
// A descriptor is:
//
//     UnicodeString  className     usually empty; a human label, not an id
//     Key            classId       what kind of thing this describes
//     uint32         itemCount
//     itemCount x:
//       Key          key           this item's name
//       char[4]      osType        the type of the value that follows
//       ...          value         framed by the type, and by nothing else
//
// A list (`VlLs`) is the same minus the keys: a count, then that many
// `osType` + value pairs.
//
// Everything is big-endian. Doubles are IEEE-754 binary64, big-endian.
//
// --- The Key quirk, stated first because it is where readers break ---------
//
// A `Key` is *not* four characters. It is:
//
//     uint32 length
//     if length == 0:  exactly 4 bytes of key text
//     else:            `length` bytes of key text
//
// Zero means four. That inversion is the classic source of bugs in third-party
// readers, and it bites in both directions: a reader that always takes four
// bytes desynchronises on the first long key (Photoshop writes plenty --
// `useTipDynamics`, `brushGroupName`), and a reader that always trusts the
// length reads four bytes of key text as a length. It applies to the item key,
// to a descriptor's `classId`, and to both halves of an `enum` -- three places
// per nested level, so getting it wrong once corrupts everything after it.
//
// `--selftest` covers a zero length, an explicit length, an explicit length of
// *exactly 4* (legal, and the case a reader that special-cases 4 gets wrong),
// and a length that runs off the end of the buffer.
//
// A key's bytes are technically MacRoman. Every key Photoshop actually emits is
// ASCII, and this module copies the bytes through into a `std::string`
// unchanged rather than transcoding -- so a non-ASCII key survives as its own
// bytes and compares equal to itself, which is all any consumer needs of it.
//
// --- Storage: a flat node array, not a tree of variants --------------------
//
// The obvious shape is `std::variant<Descriptor, List, double, ...>` nodes
// owning their children. It was rejected, and the reason is the untrusted-input
// property above rather than taste.
//
// **A recursively-owning tree destroys recursively.** A file containing five
// thousand nested `Objc` items -- eleven bytes each, so a 60 KB file -- builds a
// five-thousand-deep chain of nodes, and `~Node` then walks it on the C stack,
// in the *caller's* frame, after this module has already returned success. No
// amount of bounds checking in the parser prevents that crash, because the
// parser is no longer running when it happens. A depth cap papers over it, but
// then the cap is the only thing between a brush file and a stack overflow, and
// a cap is a number somebody eventually raises. `std::vector<DescriptorNode>`
// destroys in a loop, at any depth, and needs nobody to be careful.
//
// So: every node lives in one `DescriptorTree::nodes` vector, and a container
// node names its children by **index** into that vector. The parse is
// correspondingly iterative -- an explicit `std::vector` of open frames, no
// recursion in the parser either -- which means the depth limit below is not
// load-bearing for *this* module's safety at all. It exists for consumers,
// which will walk the tree recursively because that is the natural way to walk
// a tree, and it is much better that io/AbrImport meets a refusal here than a
// SIGSEGV of its own later.
//
// Three consequences worth stating rather than discovering:
//
//  * **`std::variant` is still used, one level down.** A node's *payload* is a
//    variant over leaf types only -- double, integers, bool, string, bytes, and
//    the two small structs. There is no cycle through it, so none of the
//    recursion argument applies, and it buys the thing a variant is good at:
//    a typed read that cannot silently return the wrong field. Flat storage and
//    variant payloads are not alternatives; the rejected design was the
//    *recursive ownership*, not the variant.
//  * **The index-based children push a class of bug onto the reader.** That is
//    the real cost, and it is why `DescriptorRef` below exists: consumers are
//    given a cursor and never an index. An out-of-range or default-constructed
//    cursor is *valid to use* -- it reports `!valid()`, its `field()` returns
//    another invalid cursor, and its typed reads return `std::nullopt` -- so
//    `root().field("Brsh").field("Dmtr").asUnitFloat()` is a legal expression
//    against a file that contains none of those things. The alternative,
//    checking every hop, is the kind of ladder people stop writing correctly by
//    the fourth level.
//  * **A `DescriptorRef` points into a particular `DescriptorTree`.** Moving or
//    destroying the tree invalidates every cursor into it, exactly as it would
//    invalidate an iterator. Cursors are for walking a tree you are holding, not
//    for storing.
//
// --- What is parsed, and what is refused ----------------------------------
//
// Parsed, keyed by the `osType` on the wire:
//
//     Objc  nested descriptor          GlbO  the same, "global object"
//     VlLs  list                       type  class          GlbC  the same
//     UntF  unit float (unit + double) doub  double
//     TEXT  UTF-16BE string            enum  enumerated (typeId + valueId)
//     long  int32                      comp  int64 ("large integer")
//     bool  one byte                   tdta  raw data (uint32 length + bytes)
//     alis  alias (uint32 length + bytes; opaque, kept verbatim)
//
// PLAN.md's line names six of those; the other seven cost a case label each and
// are the difference between reading most brush presets and reading half of one.
//
// **Refused, by name, and this is the important half:**
//
//     obj   object reference   ObAr  object array   UnFl  unit float list
//     Pth   path
//
// An Action Descriptor **carries no length in front of a value.** The type key
// is the only thing that says how many bytes the value occupies. So an
// unrecognised type is not skippable -- there is no count to step over, and
// every byte after it would be read as something else. A reader that "ignores
// unknown types" is a reader that resynchronises onto garbage and then reports
// a brush diameter it invented.
//
// That is why an unknown type is a **refusal naming the type, the item's path
// and the byte offset**, and not a warning. It is also the sharpest difference
// between this module and io/OpSerial, whose whole PRD I10 carry-through rests
// on a `bodyLength` in front of every record: Adobe's format does not have one,
// so the "keep what you do not understand" trick is simply not available here.
// The four types above are deferrals with a stated unblocking condition -- each
// needs its own framing implemented -- and not silent gaps.
//
// **`obj ` is the one most likely to be met first**, because Photoshop uses a
// reference wherever a descriptor names a live document object. It is a
// `uint32` count followed by items that are themselves seven different shapes
// (`prop`, `Clss`, `Enmr`, `rele`, `Idnt`, `indx`, `name`). Nothing in a brush
// preset is expected to need it. It is worth a follow-up step when a real file
// is found that carries one -- at which point the refusal message is what says
// so, by name.
//
// --- Refusals ------------------------------------------------------------
//
// PRD G9 is the requirement this module's error handling is shaped by: an
// import "names everything dropped rather than silently approximating". The
// wording follows io/NpaintFile.cpp and io/ExportAs.cpp -- name the thing, name
// the reason, name the alternative -- and every refusal from here additionally
// names **a byte offset and the path through the tree**, because for a foreign
// binary file that is the only way for the message to be actionable at all.
//
// A refusal is total: `ok` is false and the tree is empty. A partially-parsed
// descriptor is not offered, and that is deliberate. A truncated brush preset
// that yielded "the first four of its nine dynamics" would be a preset that
// paints differently from the one the file describes, with nothing in the
// document to say so -- the same trade io/NpaintFile refuses on the save side.
// The caller gets the bytes it handed in back unharmed and a sentence saying
// where the file stopped making sense.
//
// **Warnings are the other half**, and are for things that are recoverable
// rather than fatal: a lone UTF-16 surrogate becomes U+FFFD and says so. They
// carry the same path-and-offset shape as refusals so the import report PRD G9
// asks for can print them without reformatting.
//
// --- Bounds checking -----------------------------------------------------
//
// Every read goes through one cursor that checks `left()` before it moves, and
// every length or count read out of the file is checked against **what actually
// remains** before a single byte is allocated. So a `tdta` claiming 4 GB inside
// a 60-byte file is refused rather than attempted, and a `VlLs` claiming
// 4 billion elements never reaches a `reserve()`.
//
// The node count is bounded twice over: once by `DescriptorParseOptions::
// maxNodes`, and once for free by the format itself, since no item occupies
// fewer than four bytes on the wire and therefore no buffer can produce more
// nodes than it has bytes. The explicit cap is there so that the *refusal* is
// this module's sentence rather than a bad_alloc.
//
// `--selftest` asserts this the only way worth asserting it: every fixture,
// good and bad, is parsed out of a buffer whose last byte abuts an `mprotect`ed
// guard page, and every one-byte-shorter prefix of the good fixtures is parsed
// the same way. A read one byte past the end is a SIGSEGV, not a silent pass.
// That covers both build configurations identically, because nothing here
// depends on OpenImageIO (PLAN.md 1.5).
//
// --- Honest statement of what has not been verified ------------------------
//
// **No real `.abr` or `.psd` file was available on the machine this was written
// on.** Every fixture in `--selftest` is synthetic, written by a builder in the
// test itself, and therefore proves that this reader agrees with this author's
// reading of Adobe's published "Descriptor structure" -- not that it agrees
// with Photoshop. The framing is simple enough and well enough documented that
// this is a small risk for the types above, and the guard-page and truncation
// work is unaffected by it either way (a hostile file is hostile whether or not
// the happy path is right). But the first real file is still the test that
// matters, it belongs to io/AbrImport, and until it runs nobody should describe
// this module as proven against Photoshop.
namespace np {

// --- Types on the wire ----------------------------------------------------

enum class DescriptorType {
  Descriptor,    // 'Objc' -- children, keyed
  GlobalObject,  // 'GlbO' -- identical framing to 'Objc'
  List,          // 'VlLs' -- children, unkeyed
  UnitFloat,     // 'UntF'
  Double,        // 'doub'
  Text,          // 'TEXT'
  Enumerated,    // 'enum'
  Integer,       // 'long'
  LargeInteger,  // 'comp'
  Boolean,       // 'bool'
  Class,         // 'type' / 'GlbC'
  Alias,         // 'alis'
  RawData,       // 'tdta'
};

// The four-character key this type is written as. `Descriptor` and
// `GlobalObject` are distinct types here even though their framing is
// identical, because a consumer that round-trips a descriptor (PSD export, one
// day) must put back the code it read.
const char* descriptorTypeKey(DescriptorType type) noexcept;

// The English name, for messages: "nested descriptor", "unit float", ...
const char* descriptorTypeName(DescriptorType type) noexcept;

// A `UntF`. The unit is a bare four-character code -- **not** a Key, so the
// zero-length quirk does not apply to it. Photoshop's set is `#Ang` degrees,
// `#Rsl` pixels-per-inch, `#Rlt` distance, `#Nne` none, `#Prc` percent, `#Pxl`
// pixels. Kept as the raw code rather than an enum: an unrecognised unit is a
// value this module can carry perfectly well and has no business refusing, and
// deciding what `#Prc` *means* for a given brush key is io/BrushIR's job, not a
// byte reader's.
struct DescriptorUnitFloat {
  std::string unit;
  double value = 0.0;
};

// An `enum`. Both halves are Keys, so both are subject to the zero-length
// quirk. `typeId` is the enumeration ("brushType"), `valueId` the member
// ("computed").
struct DescriptorEnumerated {
  std::string typeId;
  std::string valueId;
};

// One node. Which fields carry meaning depends entirely on `type`; the typed
// accessors on DescriptorRef are the supported way to read them and will not
// hand back a field its type does not own.
struct DescriptorNode {
  DescriptorType type = DescriptorType::Descriptor;

  // This item's key. Empty for the root and for every element of a `VlLs`,
  // which are positional and genuinely have no key -- not "have an empty one".
  std::string key;

  // `Objc` / `GlbO` / `type` / `GlbC` only. `className` is the leading
  // UnicodeString, which Photoshop writes empty far more often than not;
  // `classId` is the Key after it and is the field that actually identifies
  // the thing.
  std::string className;
  std::string classId;

  // Leaf payload. `monostate` for the container types and for `Class`, whose
  // whole content is the two fields above.
  std::variant<std::monostate, double, int32_t, int64_t, bool, std::string,
               DescriptorUnitFloat, DescriptorEnumerated, std::vector<uint8_t>>
      payload;

  // Indices into DescriptorTree::nodes. Empty for every non-container type.
  std::vector<uint32_t> children;

  // Index of the parent, or kDescriptorNoParent for the root. Carried so a
  // cursor can report a path ("Brsh/dynamics/1/useTipDynamics") without the
  // caller having tracked one.
  uint32_t parent = 0;

  // Offset, in the buffer handed to the parser, of this item's four-character
  // type key. Named in warnings and available to consumers building the PRD G9
  // import report -- "dropped Brsh/Ptrn at byte 41233" is a sentence somebody
  // can act on; "dropped a pattern" is not.
  size_t offset = 0;
};

inline constexpr uint32_t kDescriptorNoParent = 0xFFFFFFFFu;

// --- The parsed tree ------------------------------------------------------

class DescriptorRef;

// Node 0, when the vector is non-empty, is always the root descriptor.
//
// `nodes` is public because this is a parse result and not an invariant-bearing
// object -- io/NpaintFile's NpaintCarry and NpaintRawPart are public data for
// the same reason. Write it only from the parser; read it through `root()`.
class DescriptorTree {
 public:
  std::vector<DescriptorNode> nodes;

  bool empty() const noexcept { return nodes.empty(); }
  size_t nodeCount() const noexcept { return nodes.size(); }

  // A cursor at the root descriptor, or an invalid cursor when the tree is
  // empty (which is exactly when the parse was refused).
  DescriptorRef root() const noexcept;
};

// A cursor into a DescriptorTree. See the header comment: an invalid cursor is
// usable, propagates, and reads as absent, so a walk through a file that turns
// out not to contain what was expected is an expression rather than a ladder.
class DescriptorRef {
 public:
  DescriptorRef() = default;
  DescriptorRef(const DescriptorTree* tree, uint32_t index) noexcept;

  bool valid() const noexcept { return node_ != nullptr; }
  explicit operator bool() const noexcept { return valid(); }

  // Meaningful only when valid(); `DescriptorType::Descriptor` otherwise, which
  // is why every caller should ask valid() or use a typed accessor instead.
  DescriptorType type() const noexcept;
  std::string_view key() const noexcept;
  std::string_view className() const noexcept;
  std::string_view classId() const noexcept;
  size_t offset() const noexcept;

  // "" for the root; "Brsh" for its child; "Brsh/dynamics/1/useTipDynamics"
  // for a keyed item inside the list element at index 1. List elements
  // contribute their index, since they have no key of their own.
  std::string path() const;

  // Children, for `Objc` / `GlbO` / `VlLs`. Zero for everything else -- a leaf
  // is not an error to iterate, it is simply empty.
  size_t childCount() const noexcept;
  DescriptorRef child(size_t index) const noexcept;

  // The first child whose key matches, or an invalid cursor. Linear: a
  // descriptor's item count is a handful, and a map per node would cost an
  // allocation per node to save a comparison per lookup.
  //
  // Never matches a `VlLs` element, which has no key -- so `field("")` finds
  // nothing rather than finding the first list element.
  DescriptorRef field(std::string_view key) const noexcept;

  // Typed reads. **Each requires an exact type match and performs no
  // coercion**: `asDouble()` on a `long` is `std::nullopt`, not 3.0.
  //
  // That is the same rule io/OpSerial's reader follows and it is here for the
  // same reason -- a reader that quietly widens a `long` to a double is a
  // reader that cannot tell its caller which one the file said, and PRD G9's
  // report is built entirely out of being able to say that. A consumer that
  // genuinely accepts either writes the two calls, which is three lines and is
  // visible in review.
  std::optional<double> asDouble() const noexcept;
  std::optional<DescriptorUnitFloat> asUnitFloat() const;
  std::optional<int32_t> asInteger() const noexcept;
  std::optional<int64_t> asLargeInteger() const noexcept;
  std::optional<bool> asBoolean() const noexcept;
  std::optional<DescriptorEnumerated> asEnumerated() const;
  // UTF-8, decoded from the file's UTF-16BE. Absent unless the type is `TEXT`.
  std::optional<std::string_view> asText() const noexcept;
  // `tdta` and `alis` both. Null unless the type is one of those two; the
  // pointer is into the tree and lives as long as it does.
  const std::vector<uint8_t>* asRawData() const noexcept;

 private:
  const DescriptorTree* tree_ = nullptr;
  const DescriptorNode* node_ = nullptr;
  uint32_t index_ = kDescriptorNoParent;
};

// --- Parsing --------------------------------------------------------------

// The `uint32` version word that precedes a descriptor everywhere this project
// will meet one: `.abr`'s `desc` block and every descriptor-bearing PSD key.
// Adobe has only ever written 16.
inline constexpr uint32_t kActionDescriptorVersion = 16;

struct DescriptorParseOptions {
  // Refused beyond this many open containers. Not needed for this parser,
  // which is iterative -- see the header comment -- but a promise to consumers
  // that a tree they walk recursively has a bounded depth. 64 is far past
  // anything Photoshop writes (a brush preset's deepest path is about six) and
  // far short of anything that overflows a stack.
  uint32_t maxDepth = 64;

  // A ceiling on nodes, so a pathological-but-well-formed file refuses in this
  // module's words instead of in the allocator's. The format bounds this for
  // free at one node per four bytes; this is the belt to that's braces.
  uint32_t maxNodes = 1u << 20;
};

struct DescriptorParseResult {
  bool ok = false;

  // Non-empty exactly when !ok. Names the byte offset, the path through the
  // tree, what was expected and what was actually there. See the header on why
  // there is no partial result to go with it.
  std::string error;

  // Empty when !ok.
  DescriptorTree tree;

  // How many bytes the descriptor occupied. **Trailing bytes are not an
  // error** -- a descriptor is nearly always embedded in a container that has
  // its own framing and padding after it -- so this is how io/AbrImport will
  // know where the next `8BIM` block starts. Zero when !ok.
  size_t bytesConsumed = 0;

  // Recoverable observations, each naming a path and an offset. PRD G9's report
  // is assembled from these plus io/AbrImport's own.
  std::vector<std::string> warnings;
};

// Parses a bare descriptor -- className, classId, count, items -- from the
// front of `bytes`.
//
// Reads no byte outside `bytes`, for any content of `bytes` whatsoever. That is
// this function's contract and the reason the module exists in the shape it
// does.
DescriptorParseResult parseActionDescriptor(std::span<const uint8_t> bytes,
                                            const DescriptorParseOptions& options = {});

// The same, preceded by the four-byte version word, which must be
// kActionDescriptorVersion. A version this build has not seen is **refused by
// name**, not ignored: the version exists precisely to say the framing changed,
// and the framing is the only thing this reader has (see the header on the
// absence of value lengths). `bytesConsumed` includes the four bytes.
DescriptorParseResult parseVersionedActionDescriptor(std::span<const uint8_t> bytes,
                                                     const DescriptorParseOptions& options = {});

// A stable, human-readable rendering of a whole tree, one line per node,
// indented by depth. For `--selftest` (which asserts entire trees against a
// literal, so a framing regression shows as a diff rather than as one wrong
// accessor) and for the eventual import report and any debugging in between.
//
// Deliberately free of byte offsets, so the same descriptor dumps identically
// wherever it was embedded.
std::string dumpDescriptorTree(const DescriptorTree& tree);

}  // namespace np
