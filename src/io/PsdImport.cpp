#include "io/PsdImport.hpp"

#include <algorithm>
#include <cstring>

#include "color/Space.hpp"
#include "core/Blend.hpp"
#include "core/Tile.hpp"
#include "io/AbrBrushes.hpp"  // checkedAdd() -- shared overflow-safe addition

// io/PsdImport implementation. Every design decision is argued in
// io/PsdImport.hpp; this file holds the mechanics.
namespace np {
namespace {

// --- A bounds-checked cursor, in the AbrBrushes.cpp/Descriptor.cpp mould --
//
// AbrBrushes.cpp reads by passing an explicit offset to each free function
// (`readU16(bytes, at, out)`); io/Descriptor.hpp instead threads one cursor
// through the whole parse ("every read goes through one cursor that checks
// left() before it moves"). PSD's own nesting is deeper than either of
// those files' -- header, then four sections, then per-layer records, then
// per-channel data, then per-layer tagged-info blocks -- so this file takes
// Descriptor's shape: a `Cursor` that advances only on a successful,
// `checkedAdd()`-guarded read and turns permanently invalid the first time
// one fails, so a long chain of reads can be written as a chain of `&&`
// rather than a ladder of `if (!x) return false;`. Every single byte access
// underneath it still goes through `checkedAdd()` before touching memory,
// so the safety property is identical to the free-function style; this is
// only a readability choice for the deeper nesting here.
class Cursor {
 public:
  explicit Cursor(std::span<const uint8_t> data) : data_(data) {}

  bool ok() const noexcept { return ok_; }
  size_t pos() const noexcept { return pos_; }
  size_t remaining() const noexcept { return ok_ ? data_.size() - pos_ : 0; }
  size_t size() const noexcept { return data_.size(); }
  // The whole underlying buffer, for a caller that needs to carve out a
  // sub-cursor scoped to fewer bytes than "the rest of the file" -- see
  // `readLayerRecord()`'s extra-data field, which must not let a corrupt
  // inner length field read into the NEXT layer record's bytes even though
  // those bytes are themselves perfectly in-bounds of `bytes` as a whole.
  std::span<const uint8_t> data() const noexcept { return data_; }

  // Jumps to an absolute offset that has already been proven in-bounds by
  // the caller (every call site below computes `target` as `pos() +
  // alreadyCheckedLength`). Never advances past `data_.size()`; doing so
  // marks the cursor invalid rather than silently clamping, so a caller's
  // bookkeeping bug shows up as a refusal instead of a wrong resync point.
  [[nodiscard]] bool seek(size_t target) noexcept {
    if (!ok_ || target > data_.size()) {
      ok_ = false;
      return false;
    }
    pos_ = target;
    return true;
  }

  [[nodiscard]] bool skip(size_t n) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, n, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u8(uint8_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 1, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = data_[pos_];
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u16(uint16_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 2, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u32(uint32_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 4, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = (static_cast<uint32_t>(data_[pos_]) << 24) |
          (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
          (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
          static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool i16(int16_t& out) noexcept {
    uint16_t bits = 0;
    if (!u16(bits)) return false;
    out = static_cast<int16_t>(bits);
    return true;
  }

  [[nodiscard]] bool i32(int32_t& out) noexcept {
    uint32_t bits = 0;
    if (!u32(bits)) return false;
    out = static_cast<int32_t>(bits);
    return true;
  }

  // `n` raw bytes, as a subspan into the original buffer (no copy). The
  // returned span's lifetime is `data_`'s, which outlives this whole parse
  // (the caller of `importPsd()` owns `bytes` for the call's duration).
  [[nodiscard]] bool bytes(size_t n, std::span<const uint8_t>& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, n, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = data_.subspan(pos_, n);
    pos_ = end;
    return true;
  }

  // Four raw bytes, verbatim, for a signature or a blend-mode key -- never
  // treated as text (some blend keys are Latin-1 mush like `hue `, not
  // valid UTF-8), only ever compared byte-for-byte.
  [[nodiscard]] bool fourcc(std::array<char, 4>& out) noexcept {
    std::span<const uint8_t> s;
    if (!bytes(4, s)) return false;
    out = {static_cast<char>(s[0]), static_cast<char>(s[1]), static_cast<char>(s[2]),
           static_cast<char>(s[3])};
    return true;
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

bool fourccEquals(const std::array<char, 4>& fourcc, const char* lit) noexcept {
  return std::memcmp(fourcc.data(), lit, 4) == 0;
}

std::string fourccToString(const std::array<char, 4>& fourcc) {
  return std::string(fourcc.data(), 4);
}

// --- PackBits (Photoshop's per-scanline RLE) --------------------------------
//
// io/AbrBrushes.cpp's own `decodePackBits()` documents the algorithm and the
// "decode as one continuous stream rather than resynchronising at each row"
// choice at length; this is the identical algorithm, rewritten here rather
// than shared. AbrBrushes.cpp's version is `static` to that translation
// unit (anonymous namespace) and is sized for a `samp` block's own u16
// row-length table, which happens to be the same width PSD's (non-PSB) RLE
// channel data uses -- but the two call sites read genuinely different file
// formats, and reaching across translation units for one ~25-line function
// was judged not worth the coupling. `rows` is the channel's own height
// (`bottom - top`), and `expected` is `width * height * bytesPerSample`.
bool decodePackBits(std::span<const uint8_t> body, uint32_t rows, size_t expected,
                    std::vector<uint8_t>& out) {
  Cursor c(body);

  // The row-length table: `rows` big-endian u16 compressed-byte-counts,
  // summed for the total compressed byte count -- decoding the whole
  // concatenated stream in one pass rather than row by row, exactly the
  // choice AbrBrushes.cpp's own `decodePackBits()` argues (a well-formed
  // PackBits run never straddles a Photoshop-written row boundary, so the
  // two approaches agree on well-formed input, and this form still cannot
  // read past `body` on malformed input because every access below is
  // bounds-checked first).
  uint64_t total = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    uint16_t rowLen = 0;
    if (!c.u16(rowLen)) return false;
    total += rowLen;
  }
  if (total > c.remaining()) return false;
  std::span<const uint8_t> stream;
  if (!c.bytes(static_cast<size_t>(total), stream)) return false;

  out.clear();
  out.reserve(expected);
  size_t p = 0;
  while (p < stream.size() && out.size() < expected) {
    const int8_t n = static_cast<int8_t>(stream[p]);
    ++p;
    if (n == -128) {
      continue;  // PackBits' own no-op control byte
    } else if (n < 0) {
      // Run: repeat the next byte (-n + 1) times.
      if (p >= stream.size()) return false;
      const size_t count = static_cast<size_t>(-static_cast<int>(n) + 1);
      const uint8_t b = stream[p];
      ++p;
      for (size_t k = 0; k < count && out.size() < expected; ++k) out.push_back(b);
    } else {
      // Literal: the next (n + 1) bytes, verbatim.
      const size_t count = static_cast<size_t>(n) + 1;
      size_t next = 0;
      if (!checkedAdd(p, count, next) || next > stream.size()) return false;
      out.insert(out.end(), stream.data() + p, stream.data() + p + count);
      p = next;
    }
  }
  return out.size() == expected;
}

// --- Blend mode mapping ------------------------------------------------------
//
// io/PsdImport.hpp argues each mapping (and the deliberate absence of the
// rest) at length; this table is just the wire keys next to the
// core::BlendMode each one is an exact match for. Every key not listed here
// is reported by name and left as Normal -- see `mapBlendKey()`.
struct BlendKeyMap {
  const char* psdKey;  // exactly 4 bytes, including a trailing space where
                       // Photoshop pads a short key with one
  BlendMode mode;
};
constexpr BlendKeyMap kBlendKeyMap[] = {
    {"norm", BlendMode::Normal},
    {"mul ", BlendMode::Multiply},
    {"scrn", BlendMode::Screen},
    // Darken is an exact per-channel minimum and Lighten an exact per-channel
    // maximum of source and backdrop -- io/PsdImport.hpp's own header
    // derives why these two, alone among Photoshop's non-`norm` keys with no
    // literal core::BlendMode counterpart, are still an EXACT match rather
    // than an approximation.
    {"dark", BlendMode::Min},
    {"lite", BlendMode::Max},
};

// `std::nullopt`-free by design: every key maps to a BlendMode, and the
// bool return says whether that mapping was an EXACT one (found in the
// table above) or the "no equivalent, reported and left as Normal" fallback
// io/PsdImport.hpp promises. The caller uses the bool to decide whether to
// warn; it never changes the returned mode, since Normal is the answer
// either way (the fallback's `BlendMode::Normal` and `norm`'s own entry
// happen to produce the same value, which is what makes "exact or
// Normal-and-warn" a total function rather than a partial one).
BlendMode mapBlendKey(const std::array<char, 4>& key, bool& exactMatch) noexcept {
  for (const BlendKeyMap& entry : kBlendKeyMap) {
    if (fourccEquals(key, entry.psdKey)) {
      exactMatch = true;
      return entry.mode;
    }
  }
  exactMatch = false;
  return BlendMode::Normal;
}

// --- One parsed layer, before its pixels are packed into a Layer's tiles ---

struct ParsedChannel {
  int16_t id = 0;      // 0/1/2 = R/G/B; -1 = alpha; -2/-3 = a layer mask
  uint32_t length = 0;  // this channel's own byte count, PSD (non-PSB) width
};

struct ParsedLayer {
  int32_t top = 0, left = 0, bottom = 0, right = 0;
  std::vector<ParsedChannel> channels;
  std::array<char, 4> blendKey{'n', 'o', 'r', 'm'};
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  bool hidden = false;
  std::string name;      // luni, if present; else the Pascal name
};

// Photoshop's own thirteen Additional-Layer-Information keys that widen to
// an 8-byte length in PSB -- unused by this build (PSB is refused, see
// io/PsdImport.hpp), kept here as the record of the research this module's
// PSB decision rests on, so a future implementer does not have to re-derive
// it: LMsk, Lr16, Lr32, Layr, Mt16, Mt32, Mtrn, Alph, FMsk, lnk2, FEid,
// FXid, PxSD (Adobe's published spec, "Additional Layer Information";
// cross-checked against psd-tools' `TaggedBlock._BIG_KEYS`, which names the
// identical thirteen keys under their own long constant names).

// Reads one layer record's header (everything up to and including its own
// tagged-information blocks) starting at `c`'s current position, which must
// be the start of the rectangle. Does NOT read this layer's channel image
// data -- that lives later in the file, after every layer record, in the
// same order (io/PsdImport.hpp's "Layer stacking order" section already
// established the order channel data appears in is irrelevant to this
// module's own stacking decision; it still has to be walked in file order
// to reach the next layer record).
bool readLayerRecord(Cursor& c, ParsedLayer& layer, std::string& error) {
  if (!(c.i32(layer.top) && c.i32(layer.left) && c.i32(layer.bottom) && c.i32(layer.right))) {
    error = "layer record: truncated rectangle at byte " + std::to_string(c.pos());
    return false;
  }
  // A well-formed rectangle never inverts. This is one of the deliberately
  // adversarial fixtures in --selftest, and refusing the whole file rather
  // than clamping is the same "no half-built document" discipline
  // io/Descriptor.hpp and io/NpaintFile hold themselves to: a clamped
  // rectangle would decode SOME pixels for a layer whose own bytes never
  // described a valid one.
  if (layer.bottom < layer.top || layer.right < layer.left) {
    error = "layer record: inverted rectangle (top=" + std::to_string(layer.top) +
            " left=" + std::to_string(layer.left) + " bottom=" + std::to_string(layer.bottom) +
            " right=" + std::to_string(layer.right) + ") at byte " + std::to_string(c.pos());
    return false;
  }
  // Widened to int64_t before subtracting: `right`/`left`/`bottom`/`top` are
  // each independently-read int32_t values from an untrusted file, and nothing
  // above bounds them against each other -- `right = 2000000000, left =
  // -2000000000` passes the "not inverted" check just above while `right -
  // left` in 32-bit arithmetic is signed overflow (undefined behaviour). No
  // legitimate PSD layer is anywhere near this large (30,000px is the whole
  // canvas's own ceiling), so this also doubles as the "implausible size"
  // refusal PSB's higher 300,000px ceiling would otherwise need a separate
  // check for.
  const int64_t layerWidth64 = static_cast<int64_t>(layer.right) - static_cast<int64_t>(layer.left);
  const int64_t layerHeight64 = static_cast<int64_t>(layer.bottom) - static_cast<int64_t>(layer.top);
  if (layerWidth64 > 30000 || layerHeight64 > 30000) {
    error = "layer record: implausible size " + std::to_string(layerWidth64) + "x" +
            std::to_string(layerHeight64) + " at byte " + std::to_string(c.pos());
    return false;
  }

  uint16_t numChannels = 0;
  if (!c.u16(numChannels)) {
    error = "layer record: truncated channel count at byte " + std::to_string(c.pos());
    return false;
  }
  // 56 is the file header's own documented ceiling on the TOTAL channel
  // count of the image; no single layer can plausibly declare more. A
  // hostile count here is exactly the "claims more than the buffer holds"
  // shape this module's fuzz-adjacent tests target -- refused before a
  // single `ParsedChannel` is reserved, not after an oversized allocation.
  constexpr uint16_t kMaxChannelsPerLayer = 56;
  if (numChannels > kMaxChannelsPerLayer) {
    error = "layer record: implausible channel count " + std::to_string(numChannels) +
            " (max " + std::to_string(kMaxChannelsPerLayer) + ") at byte " +
            std::to_string(c.pos());
    return false;
  }
  layer.channels.reserve(numChannels);
  for (uint16_t i = 0; i < numChannels; ++i) {
    ParsedChannel ch;
    int16_t id16 = 0;
    uint32_t len = 0;
    if (!(c.i16(id16) && c.u32(len))) {
      error = "layer record: truncated channel info table at byte " + std::to_string(c.pos());
      return false;
    }
    ch.id = id16;
    ch.length = len;
    layer.channels.push_back(ch);
  }

  std::array<char, 4> sig{};
  if (!c.fourcc(sig) || !fourccEquals(sig, "8BIM")) {
    error = "layer record: blend mode signature is not '8BIM' at byte " + std::to_string(c.pos());
    return false;
  }
  if (!c.fourcc(layer.blendKey)) {
    error = "layer record: truncated blend mode key at byte " + std::to_string(c.pos());
    return false;
  }

  uint8_t flags = 0;
  uint8_t filler = 0;
  if (!(c.u8(layer.opacity) && c.u8(layer.clipping) && c.u8(flags) && c.u8(filler))) {
    error = "layer record: truncated opacity/clipping/flags at byte " + std::to_string(c.pos());
    return false;
  }
  // **Bit 1 of `flags` means HIDDEN, not visible**, despite the published
  // spec's own table calling it "visible" -- io/PsdImport.hpp's header
  // states the evidence (an independent reader's behaviour, not this
  // module's guess) and why it mattered enough to check rather than trust
  // the table's own English.
  constexpr uint8_t kFlagHidden = 0x02;
  layer.hidden = (flags & kFlagHidden) != 0;

  uint32_t extraLen = 0;
  if (!c.u32(extraLen)) {
    error = "layer record: truncated extra-data length at byte " + std::to_string(c.pos());
    return false;
  }
  const size_t extraStart = c.pos();
  size_t extraEnd = 0;
  if (!checkedAdd(extraStart, extraLen, extraEnd) || extraEnd > c.size()) {
    error = "layer record: extra-data length " + std::to_string(extraLen) +
            " runs past the end of the file (at byte " + std::to_string(extraStart) + ")";
    return false;
  }

  // Everything from here to `extraEnd` is parsed through a SUB-CURSOR
  // scoped to exactly `[extraStart, extraEnd)`, rather than continuing to
  // read through `c` against the whole file. That is not a style choice:
  // `c`'s own bounds check only ever proves a read stays inside the whole
  // file, and mask/blending-ranges/name are each fronted by their own
  // length field that this module does not otherwise validate against
  // anything narrower than that. A corrupt (but still in-file-bounds) mask
  // length would, read through `c` directly, walk straight past this
  // layer's own `extraEnd` and into the NEXT layer record's bytes -- no
  // out-of-bounds read (still safe), but silently wrong data read as this
  // layer's name or tagged blocks, and a parse that appears to succeed with
  // scrambled metadata rather than refusing. Scoping `ec` to exactly
  // `extraLen` bytes makes that structurally impossible: every read below
  // is bounds-checked against `extraEnd` for free, by being bounds-checked
  // against `ec`'s own size.
  Cursor ec(c.data().subspan(extraStart, extraLen));

  // Layer mask / adjustment layer data: a self-length-prefixed block this
  // module does not interpret (io/PsdImport.hpp: masks are walked past, not
  // imported, in this landing). Skipping by its own declared size, rather
  // than by the fixed 0/20/36-byte shapes the spec's table enumerates,
  // means this module never has to reproduce that table's internal layout
  // at all -- it only has to trust the one length field every shape shares.
  uint32_t maskLen = 0;
  if (!ec.u32(maskLen) || !ec.skip(maskLen)) {
    error = "layer record: truncated layer mask data at byte " + std::to_string(extraStart + ec.pos());
    return false;
  }

  // Layer blending ranges: same shape, same treatment.
  uint32_t blendRangesLen = 0;
  if (!ec.u32(blendRangesLen) || !ec.skip(blendRangesLen)) {
    error = "layer record: truncated layer blending ranges at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }

  // Layer name: Pascal string (1-byte length, then that many bytes),
  // padded so the WHOLE field (length byte included) is a multiple of 4.
  uint8_t nameLen = 0;
  if (!ec.u8(nameLen)) {
    error = "layer record: truncated layer name at byte " + std::to_string(extraStart + ec.pos());
    return false;
  }
  std::span<const uint8_t> nameBytes;
  if (!ec.bytes(nameLen, nameBytes)) {
    error = "layer record: layer name runs past the extra-data field at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }
  layer.name.assign(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
  const size_t consumedSoFar = 1 + static_cast<size_t>(nameLen);
  const size_t padded = (consumedSoFar + 3) & ~static_cast<size_t>(3);
  if (!ec.skip(padded - consumedSoFar)) {
    error = "layer record: layer name padding runs past the extra-data field at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }

  // Additional Layer Information: a sequence of `8BIM`/`8B64` + 4-char key +
  // 4-byte length + data, filling the rest of this layer's extra-data
  // field. Walked for exactly one key, `luni` (the Unicode layer name,
  // preferred over the Pascal one per io/PsdImport.hpp) -- every other key
  // (`lrFX`, `lsct`, adjustment-layer parameters, ...) is skipped by its own
  // declared length without being interpreted, which is what lets this
  // module stay correct in the face of Additional Layer Information kinds
  // it has never heard of: it only ever needs to know how many bytes to
  // step over, never what is in them.
  while (ec.remaining() >= 12) {
    const size_t blockStart = extraStart + ec.pos();
    std::array<char, 4> blockSig{};
    std::array<char, 4> key{};
    uint32_t blockLen = 0;
    if (!(ec.fourcc(blockSig) && ec.fourcc(key) && ec.u32(blockLen))) {
      error = "layer record: truncated additional layer info block at byte " +
              std::to_string(blockStart);
      return false;
    }
    if (!fourccEquals(blockSig, "8BIM") && !fourccEquals(blockSig, "8B64")) {
      error = "layer record: additional layer info block has signature '" +
              fourccToString(blockSig) + "', not '8BIM'/'8B64', at byte " +
              std::to_string(blockStart);
      return false;
    }
    std::span<const uint8_t> blockData;
    if (!ec.bytes(blockLen, blockData)) {
      error = "layer record: additional layer info block '" + fourccToString(key) +
              "' length " + std::to_string(blockLen) + " runs past the extra-data field (at byte " +
              std::to_string(blockStart) + ")";
      return false;
    }
    if (fourccEquals(key, "luni")) {
      // A UnicodeString: 4-byte code-unit count, then that many UTF-16BE
      // code units -- io/PsdImport.hpp's own header on why this is decoded
      // directly rather than routed through io/Descriptor's Action
      // Descriptor reader: `luni` is not an Action Descriptor value at all,
      // it is the format's own plain UnicodeString primitive, so the
      // Descriptor module's framing (a Key, an osType, ...) does not apply
      // here.
      Cursor nc(blockData);
      uint32_t units = 0;
      if (nc.u32(units)) {
        // `units` is a uint32_t, so `units * 2` is at most ~8.6 billion --
        // well inside size_t's 64-bit range on every platform this project
        // targets (Darwin/macOS only; nothing elsewhere in this tree guards
        // a 32-bit size_t either). `nc.bytes()` bounds-checks the result
        // against `blockData` regardless, so an oversized `units` from a
        // hostile file is still refused, just by that check rather than by
        // an overflow guard on the multiplication.
        std::span<const uint8_t> codeUnitBytes;
        if (nc.bytes(static_cast<size_t>(units) * 2, codeUnitBytes)) {
          std::u16string utf16;
          utf16.reserve(units);
          for (uint32_t i = 0; i < units; ++i) {
            const uint16_t unit = static_cast<uint16_t>((codeUnitBytes[i * 2] << 8) |
                                                         codeUnitBytes[i * 2 + 1]);
            utf16.push_back(static_cast<char16_t>(unit));
          }
          // Photoshop writes a trailing NUL inside `luni`'s own counted
          // length (observed convention, not stated in the published spec
          // table -- io/PsdImport.hpp's own header names this class of gap).
          // Stripped here so a re-opened name does not grow a literal NUL
          // glyph on every round trip through an application that does not
          // also strip it.
          while (!utf16.empty() && utf16.back() == u'\0') utf16.pop_back();
          // UTF-16 -> UTF-8, by hand rather than a locale-dependent
          // conversion facet: only the BMP is needed for this landing (a
          // lone surrogate is replaced with U+FFFD, matching
          // io/Descriptor.cpp's own repair policy for the identical
          // failure mode in a different Adobe string encoding), and every
          // codepoint here is either ASCII, in the 2-byte UTF-8 range, or a
          // full BMP character in the 3-byte range -- astral characters
          // would need surrogate-pair reassembly this loop does not
          // attempt, which is a stated, narrow gap rather than a silent
          // truncation: an unpaired surrogate becomes U+FFFD rather than
          // being emitted as invalid UTF-8.
          std::string utf8;
          for (size_t i = 0; i < utf16.size(); ++i) {
            uint32_t cp = static_cast<uint16_t>(utf16[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
              const uint32_t lo = static_cast<uint16_t>(utf16[i + 1]);
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
              } else {
                cp = 0xFFFD;
              }
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
              cp = 0xFFFD;
            }
            if (cp < 0x80) {
              utf8.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
              utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
          }
          layer.name = std::move(utf8);
        }
      }
      // A `luni` block this module could not even read as a UnicodeString
      // (truncated inside its own bytes) is not a whole-file refusal --
      // the Pascal name already read above is a perfectly good fallback,
      // and one damaged optional block should not cost the rest of a
      // document that otherwise parses cleanly.
    }
  }

  // Resync to the extra-data field's own declared end rather than trusting
  // that the tagged-block walk consumed it to the byte -- the same
  // "jump to the outer length's own end" discipline io/AbrBrushes.cpp's
  // 8BIM section walk uses, and for the identical reason: a field this
  // module does not fully interpret (an Additional Layer Information kind
  // with no case above) must not be able to desynchronise everything after
  // it.
  if (!c.seek(extraEnd)) {
    error = "layer record: internal: extra-data end is not reachable";
    return false;
  }
  return true;
}

// Decodes one channel's own byte span (already sliced to exactly its
// declared length by the caller) into `expected` raw samples (1 or 2 bytes
// each, per `bytesPerSample`), honouring only the two compressions this
// module reads.
bool decodeChannelData(std::span<const uint8_t> channelSpan, uint32_t width, uint32_t height,
                       int bytesPerSample, std::vector<uint8_t>& rawSamples,
                       std::string& error) {
  Cursor c(channelSpan);
  uint16_t compression = 0;
  if (!c.u16(compression)) {
    error = "channel data: truncated compression field";
    return false;
  }
  const size_t expected = static_cast<size_t>(width) * height * static_cast<size_t>(bytesPerSample);
  std::span<const uint8_t> body = channelSpan.subspan(c.pos());

  if (compression == 0) {  // Raw
    if (body.size() != expected) {
      error = "channel data: raw data is " + std::to_string(body.size()) + " bytes, expected " +
              std::to_string(expected);
      return false;
    }
    rawSamples.assign(body.begin(), body.end());
    return true;
  }
  if (compression == 1) {  // RLE / PackBits
    if (!decodePackBits(body, height, expected, rawSamples)) {
      error = "channel data: PackBits stream did not decode to the expected " +
              std::to_string(expected) + " bytes";
      return false;
    }
    return true;
  }
  // ZIP (2) or ZIP-with-prediction (3): refused by name, for the whole
  // file -- io/PsdImport.hpp's header states why (no zlib dependency in
  // this tree) and why this is total rather than per-layer.
  error = "channel data: compression mode " + std::to_string(compression) +
          " (ZIP" + std::string(compression == 3 ? " with prediction" : "") +
          ") is not supported -- this build has no zlib dependency, and a ZIP-compressed "
          "PSD layer is refused rather than decoded into garbage or silently dropped";
  return false;
}

// Packs one layer's decoded straight-alpha, linear-light RGBA samples into
// `layer`'s tile storage at document coordinates offset by (`left`, `top`).
//
// This is io/ImageIO.cpp's `writeDecodedImageIntoLayer()` with one added
// offset, deliberately kept as its own function rather than a change to
// that one: `writeDecodedImageIntoLayer()`'s own contract (io/ImageIO.hpp)
// is fixed at document coordinates [0, width) x [0, height) -- every other
// caller relies on that -- and a PSD layer's own rectangle can sit anywhere
// on the canvas, including partly or wholly off it (negative `left`/`top`,
// or a `right`/`bottom` past the canvas edge, both legal in the format and
// both exercised by --selftest's "offset layer bounds" fixture).
// core/Tile.hpp's `floorDiv`/`floorMod` already handle negative document
// coordinates correctly (see that header's own comment), so no clamping is
// needed here at all -- a tile is simply allocated wherever the layer's
// pixels land, on or off the nominal canvas.
void writeLayerPixelsAt(std::span<const float> straightRgba, uint32_t width, uint32_t height,
                        int32_t left, int32_t top, Layer& layer) {
  if (width == 0 || height == 0 || !layer.rgbTiles.has_value()) return;
  TileStore& tiles = *layer.rgbTiles;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float* src = &straightRgba[(static_cast<size_t>(y) * width + x) * 4];
      const float a = src[3];
      const std::array<float, 4> premultiplied{src[0] * a, src[1] * a, src[2] * a, a};
      const PixelCoord doc{left + static_cast<int32_t>(x), top + static_cast<int32_t>(y)};
      tiles.getOrCreate(tileCoordAt(doc)).writePixel(tileLocalOffset(doc), premultiplied);
    }
  }
}

}  // namespace

PsdImportResult importPsd(std::span<const uint8_t> bytes) {
  PsdImportResult result;
  Cursor c(bytes);

  auto fail = [&](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    result.document = Document{};
    return result;
  };

  // --- File Header Section ---------------------------------------------
  std::array<char, 4> signature{};
  if (!c.fourcc(signature) || !fourccEquals(signature, "8BPS"))
    return fail("not a PSD file: missing '8BPS' signature.");

  uint16_t version = 0;
  if (!c.u16(version)) return fail("PSD file header is truncated (no version word).");
  if (version == 2) {
    // PSB, the Large Document Format: refused by name -- io/PsdImport.hpp's
    // header states the field-width research this decision rests on and
    // why it is refused rather than attempted.
    return fail(
        "this is a PSB (Large Document Format, version 2) file. This build's PSD reader "
        "handles PSD (version 1) only -- PSB widens several length fields from 4 to 8 "
        "bytes and is refused by name rather than misparsed as a PSD.");
  }
  if (version != 1) return fail("unrecognised PSD version " + std::to_string(version) + ".");

  if (!c.skip(6)) return fail("PSD file header is truncated (reserved bytes).");

  uint16_t channelCount = 0;
  uint32_t height = 0, width = 0;
  uint16_t depth = 0, colorMode = 0;
  if (!(c.u16(channelCount) && c.u32(height) && c.u32(width) && c.u16(depth) &&
        c.u16(colorMode)))
    return fail("PSD file header is truncated.");

  if (channelCount < 1 || channelCount > 56)
    return fail("PSD file header declares " + std::to_string(channelCount) +
               " channels, outside the format's own 1-56 range.");
  if (width == 0 || height == 0)
    return fail("PSD file header declares a " + std::to_string(width) + "x" +
               std::to_string(height) + " canvas.");
  if (width > 30000 || height > 30000)
    return fail("PSD file header declares a " + std::to_string(width) + "x" +
               std::to_string(height) +
               " canvas, past PSD's own 30,000px limit (that limit is what PSB, which "
               "this build does not read, exists to raise).");
  if (depth != 8 && depth != 16)
    return fail("PSD file header declares " + std::to_string(depth) +
               "-bit channels; this build reads 8- and 16-bit only.");
  if (colorMode != 3) {
    const char* modeName = "unknown";
    switch (colorMode) {
      case 0: modeName = "Bitmap"; break;
      case 1: modeName = "Grayscale"; break;
      case 2: modeName = "Indexed"; break;
      case 4: modeName = "CMYK"; break;
      case 7: modeName = "Multichannel"; break;
      case 8: modeName = "Duotone"; break;
      case 9: modeName = "Lab"; break;
    }
    return fail(std::string("PSD file header declares ") + modeName + " colour mode (" +
               std::to_string(colorMode) +
               "); this build reads RGB (mode 3) only, and does not guess a conversion for "
               "the others.");
  }
  const int bytesPerSample = depth == 16 ? 2 : 1;

  // --- Color Mode Data Section (skipped: only indexed/duotone carry any) -
  uint32_t colorDataLen = 0;
  if (!c.u32(colorDataLen) || !c.skip(colorDataLen))
    return fail("PSD Color Mode Data section is truncated.");

  // --- Image Resources Section (skipped) ---------------------------------
  uint32_t imageResLen = 0;
  if (!c.u32(imageResLen) || !c.skip(imageResLen))
    return fail("PSD Image Resources section is truncated.");

  // --- Layer and Mask Information Section --------------------------------
  uint32_t layerMaskInfoLen = 0;
  if (!c.u32(layerMaskInfoLen))
    return fail("PSD Layer and Mask Information section length is truncated.");
  const size_t layerMaskInfoStart = c.pos();
  size_t layerMaskInfoEnd = 0;
  if (!checkedAdd(layerMaskInfoStart, layerMaskInfoLen, layerMaskInfoEnd) ||
      layerMaskInfoEnd > c.size())
    return fail("PSD Layer and Mask Information section length (" +
               std::to_string(layerMaskInfoLen) + ") runs past the end of the file.");

  if (layerMaskInfoLen == 0) {
    // A flat PSD (or one saved with Maximize Compatibility off): no layer
    // section at all. Not this module's file to open -- app/OpenAnyFile.cpp
    // falls back to the existing flattened decode path for exactly this
    // case. See `PsdImportResult::noLayerData`.
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD carries no layer information (a flat composite only).";
    return result;
  }

  uint32_t layerInfoLen = 0;
  if (!c.u32(layerInfoLen)) return fail("PSD Layer info section length is truncated.");
  const size_t layerInfoStart = c.pos();
  size_t layerInfoEnd = 0;
  if (!checkedAdd(layerInfoStart, layerInfoLen, layerInfoEnd) || layerInfoEnd > layerMaskInfoEnd)
    return fail("PSD Layer info section length (" + std::to_string(layerInfoLen) +
               ") runs past its own Layer and Mask Information section.");

  if (layerInfoLen < 2) {
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD's layer info section is empty.";
    return result;
  }

  int16_t layerCountRaw = 0;
  if (!c.i16(layerCountRaw)) return fail("PSD layer count is truncated.");
  // A negative count means "the first alpha channel of the composite holds
  // the merged result's own transparency" -- a fact about the composite
  // this module never reads, so only the absolute value (the actual number
  // of layer records that follow) matters here.
  const int32_t layerCount =
      layerCountRaw < 0 ? -static_cast<int32_t>(layerCountRaw) : layerCountRaw;

  if (layerCount == 0) {
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD declares zero layers.";
    return result;
  }

  std::vector<ParsedLayer> layers;
  layers.reserve(static_cast<size_t>(layerCount));
  for (int32_t i = 0; i < layerCount; ++i) {
    ParsedLayer layer;
    std::string layerError;
    if (!readLayerRecord(c, layer, layerError))
      return fail("layer " + std::to_string(i) + ": " + layerError);
    layers.push_back(std::move(layer));
  }
  // Each layer record's own `extraLen` is only checked against the WHOLE
  // FILE by `readLayerRecord()` (see that function's own comment on why a
  // tighter per-field bound isn't practical field by field). This is the
  // aggregate check that catches what those individual checks cannot: a
  // record whose declared extra-data length is large enough to run past
  // the Layer info section's own declared end while still fitting inside
  // the file as a whole -- a real corruption this module would otherwise
  // read straight through, silently, rather than refuse.
  if (c.pos() > layerInfoEnd)
    return fail("layer records overran the declared Layer info section length (reached byte " +
               std::to_string(c.pos()) + ", section ends at " + std::to_string(layerInfoEnd) +
               ").");

  // --- Channel image data: one block per channel, per layer, same order -
  //
  // Decoded straight into a Document built fresh here (not
  // Document::createBlank(), which hands out exactly one layer -- this
  // module needs N, and building the vector directly is Document::
  // createBlank()'s own shape done N times rather than a second, competing
  // way to construct a Document).
  Document doc;
  doc.width = static_cast<int32_t>(width);
  doc.height = static_cast<int32_t>(height);
  doc.workingSpace = WorkingSpace{};
  doc.layers.reserve(layers.size());

  for (size_t li = 0; li < layers.size(); ++li) {
    ParsedLayer& pl = layers[li];
    const uint32_t layerWidth = static_cast<uint32_t>(pl.right - pl.left);
    const uint32_t layerHeight = static_cast<uint32_t>(pl.bottom - pl.top);
    const size_t pixelCount = static_cast<size_t>(layerWidth) * layerHeight;

    // Straight (non-premultiplied), linear-light RGBA -- default fully
    // transparent black, so a channel this layer's own table does not list
    // (no R, no G, no B, or no alpha) reads as its correct implicit value:
    // 0 for a missing colour channel, and 1.0 for a missing alpha is set
    // explicitly below, matching io/ImageDecode.cpp's own "sources without
    // an alpha channel decode as fully opaque" convention.
    std::vector<float> pixels(pixelCount * 4, 0.0f);
    bool hasAlphaChannel = false;

    for (const ParsedChannel& ch : pl.channels) {
      std::span<const uint8_t> channelSpan;
      if (!c.bytes(ch.length, channelSpan))
        return fail("layer " + std::to_string(li) + ": channel data length " +
                   std::to_string(ch.length) + " runs past the end of the file.");

      // A layer mask channel (-2 real, -3 user-supplied-when-both-present):
      // walked past to stay aligned with the next layer, not imported --
      // io/PsdImport.hpp's stated scope limit.
      if (ch.id != 0 && ch.id != 1 && ch.id != 2 && ch.id != -1) continue;
      if (pixelCount == 0) continue;  // an empty layer: nothing to decode

      std::vector<uint8_t> raw;
      std::string channelError;
      if (!decodeChannelData(channelSpan, layerWidth, layerHeight, bytesPerSample, raw,
                             channelError))
        return fail("layer " + std::to_string(li) + " (" + pl.name + "): " + channelError);

      const float maxValue = bytesPerSample == 2 ? 65535.0f : 255.0f;
      const int channelIndex = ch.id < 0 ? 3 : static_cast<int>(ch.id);  // R=0,G=1,B=2,A=3
      if (ch.id == -1) hasAlphaChannel = true;

      for (size_t px = 0; px < pixelCount; ++px) {
        uint32_t sample;
        if (bytesPerSample == 2)
          sample = (static_cast<uint32_t>(raw[px * 2]) << 8) | raw[px * 2 + 1];
        else
          sample = raw[px];
        const float encoded01 = static_cast<float>(sample) / maxValue;
        // RGB is sRGB-encoded and is linearised; alpha is linear opacity
        // and passes through unchanged -- io/PsdImport.hpp's "Colour space"
        // section, the identical rule io/ImageDecode.cpp already applies to
        // every other 8-/16-bit format this codebase reads.
        pixels[px * 4 + static_cast<size_t>(channelIndex)] =
            channelIndex == 3 ? encoded01 : srgbDecode(encoded01);
      }
    }
    if (!hasAlphaChannel) {
      for (size_t px = 0; px < pixelCount; ++px) pixels[px * 4 + 3] = 1.0f;
    }

    Layer layer;
    layer.kind = LayerKind::RGB;
    layer.rgbTiles.emplace();
    writeLayerPixelsAt(pixels, layerWidth, layerHeight, pl.left, pl.top, layer);

    layer.name = pl.name;
    layer.opacity = static_cast<float>(pl.opacity) / 255.0f;
    layer.visible = !pl.hidden;
    // The bottom layer can never be clipped (core/Layer.hpp's own stated
    // invariant, enforced elsewhere by core::setLayerClipped() -- this
    // module builds `doc.layers` directly rather than through that
    // function, so it owes the same invariant by hand). PSD layer 0 in
    // file order is this document's bottom layer (io/PsdImport.hpp's
    // "Layer stacking order" section), so it alone is exempted.
    layer.clipped = (pl.clipping != 0) && (li != 0);

    bool exactBlend = false;
    const BlendMode mode = mapBlendKey(pl.blendKey, exactBlend);
    layer.blend = blendModeName(mode);
    if (!exactBlend) {
      result.warnings.push_back("layer '" + (pl.name.empty() ? std::string("(unnamed)") : pl.name) +
                                "': PSD blend mode '" + fourccToString(pl.blendKey) +
                                "' has no equivalent in this build and was imported as Normal.");
    }

    doc.layers.push_back(std::move(layer));
  }
  // The symmetric check to the one after the layer-records loop: the
  // channel data this module just read must not have run past the Layer
  // info section's own declared end either.
  if (c.pos() > layerInfoEnd)
    return fail("channel image data overran the declared Layer info section length (reached "
               "byte " + std::to_string(c.pos()) + ", section ends at " +
               std::to_string(layerInfoEnd) + ").");

  result.ok = true;
  result.document = std::move(doc);
  return result;
}

}  // namespace np
