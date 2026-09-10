#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// io/PsdWrite -- the byte-level primitives every part of PSD export writes
// through, and nothing above them.
//
// **Why this is its own module, and why it landed before the writers that
// use it.** PLAN.md phase 15's PSD export is built as several independent
// pieces (the container and flattened composite; the layer section; masks
// and groups), and every one of them writes the same four things: big-endian
// integers, four-character keys, length-prefixed sections whose length is
// only known after the section is written, and Photoshop's two string
// shapes. Left to each piece, that is four chances to write the backpatch
// arithmetic differently -- exactly the argument io/PackBits.hpp already
// makes for itself ("a second copy would be a second place for an off-by-one
// to live, discoverable only by one of the two callers producing wrong
// pixels"). Here it is one implementation with one set of assertions.
//
// This module writes bytes. It knows nothing about PSD's sections, layers,
// colour or compression -- io/PsdExport owns all of that. The one PSD-shaped
// thing it does know is the two string encodings, because those are the
// pieces most easily got subtly wrong and least visible when they are (a
// mis-padded Pascal name desynchronises every byte after it in the record).
//
// --- Byte order --------------------------------------------------------
//
// **Every multi-byte field in a PSD is big-endian**, including on the
// little-endian machines this project runs on. Every writer below emits
// most-significant byte first, explicitly, by shifting -- never by
// memcpy-ing a native integer, which would be correct on exactly one
// architecture and silently wrong on the other. io/PsdImport.cpp's `Cursor`
// reads them back the same way; the two are inverses and
// app/selftest/PsdWrite.cpp asserts that against the reader's own arithmetic
// rather than against a second hand-written expectation.

namespace np {

// An append-only byte buffer with PSD's field shapes.
//
// Not bounds-checked in the way this directory's *readers* are, and
// deliberately so: a reader parses bytes from outside the project and must
// prove it cannot walk off a buffer it did not build, while a writer owns
// its own buffer and grows it. What replaces that contract here is the
// backpatch discipline below -- the one way a writer produces a
// structurally invalid file is by declaring a length it does not then
// write.
class PsdWriter {
 public:
  void u8(uint8_t v);
  void u16(uint16_t v);
  void u32(uint32_t v);
  void i16(int16_t v);
  void i32(int32_t v);

  // Exactly four bytes. `key` must be four characters -- Photoshop pads a
  // three-character key with a trailing SPACE (`"mul "`, `"lum "`), never
  // with a NUL, and io/PsdImport.cpp's `fourccEquals()` compares all four,
  // so a key written three-and-a-NUL silently fails to match on the way
  // back in. A `key` of any other length is a programming error and writes
  // nothing rather than writing something wrong; `ok()` goes false.
  void fourcc(const char* key);

  void raw(std::span<const uint8_t> b);
  void zeros(size_t count);

  // --- Length backpatching ------------------------------------------------
  //
  // PSD is full of sections whose `u32` length precedes content whose size
  // is not known until it has been written. `beginLengthU32()` writes a
  // zero placeholder and returns its offset; `endLengthU32()` overwrites it
  // with the number of bytes written since. The length NEVER counts the
  // four bytes of the length field itself -- that is the convention
  // io/PsdImport.cpp reads back at every one of its own section boundaries
  // (see its `layerMaskInfoStart`/`extraStart` handling, where the section
  // start is the position *after* the length word).
  //
  // Nesting is fine and is the normal case: the layer-and-mask-info length
  // encloses the layer-info length, which encloses each record's
  // extra-data length. Markers are plain offsets, so an inner
  // `endLengthU32()` simply completes before the outer one is asked for.
  [[nodiscard]] size_t beginLengthU32();
  void endLengthU32(size_t marker);

  // Pads with zeros until the number of bytes written since `sectionStart`
  // is a multiple of `multiple`. PSD pads at 2 (the layer info section) and
  // at 4 (a layer's Pascal name field). Returns the number of pad bytes
  // written, which the caller usually ignores and a test does not.
  size_t padTo(size_t multiple, size_t sectionStart);

  // --- Photoshop's two string shapes --------------------------------------

  // The legacy layer name: a 1-byte length, then that many bytes, then zero
  // padding so that the WHOLE field -- **the length byte included** -- is a
  // multiple of `padMultiple` (4 for a layer record's name; the format uses
  // this shape at other paddings elsewhere). io/PsdImport.cpp:600 reads back
  // exactly this arithmetic, including the "length byte included" part,
  // which is the half of it that is easy to get wrong and desynchronises
  // every field after it when it is.
  //
  // `utf8` is written as bytes and **truncated to 255**, on a UTF-8
  // character boundary rather than mid-sequence, so the field can never
  // carry half a code point. This string is lossy for non-ASCII names by
  // construction; the `luni` block below is what carries them, and a layer
  // record should write both, which is what Photoshop itself does.
  void pascalString(const std::string& utf8, size_t padMultiple);

  // A `luni` block's payload: a `u32` count of UTF-16 CODE UNITS (not code
  // points, and not bytes), then that many big-endian UTF-16 units.
  //
  // **Astral-plane characters become surrogate pairs and count as two
  // units.** io/PsdImport.hpp lists astral `luni` names among the things
  // its reader has never been checked against with a real file; this writer
  // is the first thing in the project that can produce one, so
  // app/selftest/PsdWrite.cpp asserts the pair arithmetic directly rather
  // than leaving it to a round trip.
  //
  // Invalid UTF-8 in `utf8` -- an unpaired continuation byte, a truncated
  // sequence, an overlong form, or a surrogate encoded as UTF-8 -- is
  // replaced by U+FFFD per byte-sequence rather than refused or passed
  // through. A layer name arrives here from a document that may have been
  // read from anywhere, and a name is not worth failing a save over; what
  // is worth avoiding is emitting a `luni` block that is not valid UTF-16,
  // which every other reader of the file would then have to guess about.
  void unicodeString(const std::string& utf8);

  // --- Result -------------------------------------------------------------

  // False once any programming error above has been detected (a `fourcc`
  // that was not four characters, an `endLengthU32` whose marker is not a
  // placeholder this writer produced, or a section that grew past 4 GiB and
  // cannot state its own length in a `u32`). A writer that is not `ok()`
  // has produced bytes that must not be written to a file: PSD export
  // refuses the whole save rather than emitting a structurally invalid
  // file, the same "a refusal is total" discipline io/Descriptor.hpp and
  // io/NpaintFile already hold.
  bool ok() const { return ok_; }

  size_t size() const { return bytes_.size(); }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::vector<uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<uint8_t> bytes_;
  bool ok_ = true;
};

// UTF-8 to UTF-16 code units, exposed for app/selftest/PsdWrite.cpp and for
// any future writer of another of PSD's UnicodeString-carrying blocks.
// Invalid input yields U+FFFD as described above; the result is always
// well-formed UTF-16.
std::vector<uint16_t> utf8ToUtf16(const std::string& utf8);

}  // namespace np
