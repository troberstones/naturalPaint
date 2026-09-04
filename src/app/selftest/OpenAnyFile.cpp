#include "app/selftest/Support.hpp"

#include "app/ImportImage.hpp"
#include "app/OpenAnyFile.hpp"

#include "core/CanvasLimits.hpp"
#include "io/FileKind.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Open accepts any file this build can read, and it decides which reader gets
// it from the file's **bytes**.
//
// See SelfTest.hpp for the full breakdown. The assertion this section exists
// for is section C's: a `.npaint` whose name ends in `.png`, and a PNG whose
// name ends in `.npaint`, each going to the right reader. Every other
// assertion here would still pass against a dispatch that tested the
// extension; those two are the ones that would not.
//
// **What identifies a `.npaint`, and why it is asserted rather than assumed.**
// There is no naturalPaint magic number -- the container is OpenEXR's (a
// `.npaint` is a multi-part tiled EXR, and PRD I8 says `.exr` is the same file
// under a different name). What makes it ours is an attribute named
// `np:version` in part 0's header, which `saveNpaint()` stamps on every file it
// writes. io/FileKind walks the EXR header to find it. That walk is a byte-level
// reading of a format spec, so section B does not take it on trust: it saves a
// real document through `saveNpaint()` and asserts the sniff calls the result a
// document. If OpenEXR's header layout is not what io/FileKind.cpp believes, or
// if that attribute is ever renamed, that assertion is what says so.
//
// Headless and GPU-free. Sections A, C-G hold in BOTH NP_USE_OIIO
// configurations -- the fixtures are PNG (stb decodes it either way) and
// hand-built byte strings. Section B's real-`.npaint` half needs a writer, so it
// is gated on `oiioBackendCompiledIn()` and says so out loud rather than going
// quiet; its synthetic half runs in both. Files are written into a scratch
// directory of this section's own, removed at the end, because "a file is
// refused by name" and "Save cannot overwrite the picture" are claims about a
// filesystem and cannot be asserted without one.
//
// **No fixture comes from the repository.** Every byte asserted on here is
// built in memory in this file. `runny_inkers.abr` and any `.psd` a reader
// might reach for are third-party material that must never be committed or
// depended on.
// ---------------------------------------------------------------------------
bool runOpenAnyFileTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "np-selftest-openanyfile";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);

  // --- fixtures -------------------------------------------------------------

  // An opaque PNG of a given size. Opaque throughout: the premultiply
  // arithmetic belongs to app/selftest/ImageIO.cpp and is not restated here --
  // what this section needs from a picture is that it decodes, and that it
  // decodes in an NP_USE_OIIO=OFF build too.
  auto pngBytes = [](int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i + 0] = 200;
      px[i + 1] = 120;
      px[i + 2] = 40;
      px[i + 3] = 255;
    }
    std::vector<uint8_t> out;
    stbi_write_png_to_func(&appendToVector, &out, w, h, 4, px.data(), w * 4);
    return out;
  };

  auto writeBytes = [&](const char* name, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path p = scratch / name;
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return p.string();
  };

  auto writeText = [&](const char* name, const std::string& text) {
    return writeBytes(name, std::vector<uint8_t>(text.begin(), text.end()));
  };

  // A hand-built OpenEXR header, to exercise io/FileKind's attribute walk
  // directly and -- crucially -- in **both** build configurations, where
  // `saveNpaint()` cannot be called at all.
  //
  // Format, from the OpenEXR spec: magic (4) + version field (4), then a
  // sequence of `name\0 type\0 int32le size, size bytes`, ending at a
  // zero-length name. Nothing here is a valid *image* -- there are no pixels --
  // and it does not need to be: what is under test is whether the walk finds
  // the marker without reading past the buffer.
  auto exrHeader = [](const std::vector<std::pair<std::string, std::string>>& attributes,
                      bool terminate) {
    std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
    for (const auto& [name, type] : attributes) {
      b.insert(b.end(), name.begin(), name.end());
      b.push_back(0);
      b.insert(b.end(), type.begin(), type.end());
      b.push_back(0);
      // A four-byte payload for every attribute, whatever its declared type:
      // the walk skips by the declared size and never interprets the bytes, so
      // one size keeps the fixture readable.
      const uint32_t size = 4;
      b.push_back(static_cast<uint8_t>(size & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
      b.insert(b.end(), {0x01, 0x00, 0x00, 0x00});
    }
    if (terminate) b.push_back(0);  // zero-length name: end of part 0's header
    return b;
  };

  // Part 0 with the marker buried behind two other attributes, so a walk that
  // only looked at the first attribute would fail this.
  const std::vector<uint8_t> npaintish =
      exrHeader({{"channels", "chlist"}, {"compression", "compression"},
                 {"np:version", "int"}, {"np:basis", "string"}},
                true);
  // The same shape with no marker: somebody else's EXR.
  const std::vector<uint8_t> plainExrish =
      exrHeader({{"channels", "chlist"}, {"compression", "compression"},
                 {"screenWindowWidth", "float"}},
                true);

  auto sniff = [](const std::vector<uint8_t>& b) {
    return sniffFileKind(b.empty() ? nullptr : b.data(), b.size());
  };

  // --- A. io/FileKind: what the bytes say, and never a byte further ---------
  std::printf("-- A. content sniff: the container, from the container --\n");
  {
    check(sniff(npaintish).kind == FileKind::NpaintDocument,
          "sniff: OpenEXR magic + an np:version attribute in part 0 is one of ours");
    check(sniff(plainExrish).kind == FileKind::Image,
          "sniff: OpenEXR magic with no np:version is somebody else's EXR, a picture -- "
          "not a document with no layers in it");
    check(sniff(plainExrish).format.has_value() &&
              *sniff(plainExrish).format == ImageFormat::Exr,
          "sniff: that EXR still reports the EXR format, so a refusal can name it");

    check(sniff(pngBytes(4, 4)).kind == FileKind::Image &&
              sniff(pngBytes(4, 4)).signature == "PNG",
          "sniff: a PNG is an image and is named PNG");

    // Every remaining signature, from a literal header. Each is the shortest
    // byte string io/FileKind can decide on, so a signature that was wrong by
    // one byte fails here rather than in a user's refusal message.
    struct SigCase { const char* label; std::vector<uint8_t> bytes; const char* expect; };
    const std::vector<SigCase> signatures = {
        {"JPEG", {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10}, "JPEG"},
        {"BMP", {'B', 'M', 0x00, 0x00, 0x00, 0x00}, "BMP"},
        {"PSD", {'8', 'B', 'P', 'S', 0x00, 0x01}, "Photoshop PSD"},
        {"TIFF LE", {0x49, 0x49, 0x2A, 0x00, 0x08, 0x00}, "TIFF"},
        {"TIFF BE", {0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x08}, "TIFF"},
        {"HDR", {'#', '?', 'R', 'A', 'D', 'I', 'A', 'N', 'C', 'E', '\n'}, "Radiance HDR"},
        {"DPX", {'S', 'D', 'P', 'X', 0x00, 0x00}, "DPX"},
        {"DPX byte-swapped", {'X', 'P', 'D', 'S', 0x00, 0x00}, "DPX"},
        {"Cineon", {0x80, 0x2A, 0x5F, 0xD7, 0x00, 0x00}, "Cineon"},
        {"GIF", {'G', 'I', 'F', '8', '9', 'a'}, "GIF"},
    };
    bool allSignatures = true;
    for (const SigCase& c : signatures) {
      const FileSniff s = sniff(c.bytes);
      if (s.kind != FileKind::Image || s.signature != c.expect) {
        allSignatures = false;
        std::printf("      %s sniffed as '%s' (%s)\n", c.label, s.signature.c_str(),
                    fileKindName(s.kind));
      }
    }
    check(allSignatures,
          "sniff: every leading signature this build knows is recognised by name");

    // TGA is the one format with nothing at the front, so it is matched from
    // the back. A file with the footer and nothing else is not a decodable TGA
    // and is not meant to be -- the sniff gates the message, never the attempt.
    std::vector<uint8_t> tga(64, 0x00);
    const char kFooter[] = "TRUEVISION-XFILE.";
    std::memcpy(tga.data() + tga.size() - sizeof(kFooter), kFooter, sizeof(kFooter));
    check(sniff(tga).kind == FileKind::Image && sniff(tga).signature == "TGA",
          "sniff: TGA is recognised from its trailing footer, having no leading magic");

    check(sniff({}).kind == FileKind::Unknown,
          "sniff: an empty buffer is Unknown rather than a guess");
    check(sniffFileKind(nullptr, 99).kind == FileKind::Unknown,
          "sniff: a null pointer is Unknown and does not dereference it");
    check(sniff({'h', 'e', 'l', 'l', 'o', '\n'}).kind == FileKind::Unknown,
          "sniff: plain text matches nothing");
    check(sniff({0x76}).kind == FileKind::Unknown,
          "sniff: one byte of the EXR magic is not the EXR magic");

    // --- the walk cannot read past its buffer -----------------------------
    //
    // Every one of these is a header truncated or malformed at a different
    // step, and every one must answer "not one of ours" rather than crash or
    // run on. Under ASan a walk that overran would fail here; without it, the
    // assertion still pins the answer.
    bool boundsHeld = true;
    std::vector<std::vector<uint8_t>> malformed;
    // Magic and version, then nothing at all.
    malformed.push_back({0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00});
    // A name that never terminates.
    {
      std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
      for (const char c : std::string("np:vers")) b.push_back(static_cast<uint8_t>(c));
      malformed.push_back(b);
    }
    // A complete name, then a type that never terminates.
    {
      std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
      for (const char c : std::string("channels")) b.push_back(static_cast<uint8_t>(c));
      b.push_back(0);
      for (const char c : std::string("chli")) b.push_back(static_cast<uint8_t>(c));
      malformed.push_back(b);
    }
    // Name, type, and only three of the four size bytes.
    {
      std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
      for (const char c : std::string("channels")) b.push_back(static_cast<uint8_t>(c));
      b.push_back(0);
      for (const char c : std::string("chlist")) b.push_back(static_cast<uint8_t>(c));
      b.push_back(0);
      b.insert(b.end(), {0x04, 0x00, 0x00});
      malformed.push_back(b);
    }
    // A size of 0xFFFFFFFF, which is the one that catches an implementation
    // testing `p + size > bufferSize` instead of `size > bufferSize - p`: the
    // addition wraps and the check passes.
    {
      std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
      for (const char c : std::string("channels")) b.push_back(static_cast<uint8_t>(c));
      b.push_back(0);
      for (const char c : std::string("chlist")) b.push_back(static_cast<uint8_t>(c));
      b.push_back(0);
      b.insert(b.end(), {0xFF, 0xFF, 0xFF, 0xFF});
      b.insert(b.end(), {0x01, 0x02, 0x03, 0x04});
      malformed.push_back(b);
    }
    // A valid header with no marker and no terminator: the walk runs out of
    // buffer rather than off a sentinel.
    malformed.push_back(exrHeader({{"channels", "chlist"}}, false));
    for (const std::vector<uint8_t>& b : malformed)
      if (sniffFileKind(b.data(), b.size()).kind == FileKind::NpaintDocument)
        boundsHeld = false;
    check(boundsHeld,
          "sniff: six malformed/truncated EXR headers, including a 0xFFFFFFFF attribute "
          "size, all answer 'not one of ours' rather than reading past the buffer");

    // The false positive the walk exists to avoid: the *string* np:version
    // appearing inside an attribute's value rather than as an attribute name.
    // A byte search of the header would call this a document and hand a
    // picture to the document reader.
    {
      std::vector<uint8_t> b = {0x76, 0x2F, 0x31, 0x01, 0x02, 0x00, 0x00, 0x00};
      const std::string name = "comment";
      const std::string type = "string";
      const std::string value = "np:version is what naturalPaint stamps";
      b.insert(b.end(), name.begin(), name.end());
      b.push_back(0);
      b.insert(b.end(), type.begin(), type.end());
      b.push_back(0);
      const uint32_t size = static_cast<uint32_t>(value.size());
      b.push_back(static_cast<uint8_t>(size & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
      b.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
      b.insert(b.end(), value.begin(), value.end());
      b.push_back(0);
      check(sniffFileKind(b.data(), b.size()).kind == FileKind::Image,
          "sniff: 'np:version' inside an attribute *value* is not a marker -- the walk "
          "reads names, so a byte search would call this EXR a document");
    }
  }

  // --- A2. SVG: a structural scan, not a substring search -------------------
  //
  // io/FileKind.hpp argues at length why "does the buffer contain `<svg`"
  // false-positives on an HTML page with an inline SVG, an XSL stylesheet,
  // and plain text that merely mentions the tag -- and why the fix is a
  // bounded walk of the XML prologue grammar instead. This section proves
  // both halves: every real prologue shape (a BOM, an XML declaration, a
  // comment, a doctype, all four together) still reaches the root element,
  // and the false positive the walk exists to prevent (case 8 below) does
  // not sneak through.
  std::printf("-- A2. content sniff: SVG, found structurally rather than by substring --\n");
  {
    auto sniffText = [&](const std::string& s) {
      return sniff(std::vector<uint8_t>(s.begin(), s.end()));
    };
    auto isSvg = [](const FileSniff& s) {
      return s.kind == FileKind::Vector && s.signature == "SVG" && !s.format.has_value();
    };

    const std::string minimalSvg = "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    check(isSvg(sniffText(minimalSvg)),
          "sniff: a minimal <svg/> is Vector, named 'SVG', with no ImageFormat -- SVG is "
          "not one of the raster formats io/Capabilities' enum names");

    check(isSvg(sniffText("\xEF\xBB\xBF" + minimalSvg)),
          "sniff: a leading UTF-8 BOM is skipped before the scan looks for the root tag");

    check(isSvg(sniffText("<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + minimalSvg)),
          "sniff: an XML declaration ahead of the root tag is skipped as a processing "
          "instruction");

    check(isSvg(sniffText("<!-- generated by an exporter -->" + minimalSvg)),
          "sniff: a comment ahead of the root tag is skipped");

    check(isSvg(sniffText(
              "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
              "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">" +
              minimalSvg)),
          "sniff: a doctype ahead of the root tag is skipped");

    check(isSvg(sniffText("\xEF\xBB\xBF  <?xml version=\"1.0\"?>  \n"
                          "<!-- a note -->  \t"
                          "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
                          "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">  \r\n" +
                          minimalSvg)),
          "sniff: BOM, whitespace, XML declaration, comment and doctype together are all "
          "skipped -- every prologue construct in one file, not just one at a time");

    check(isSvg(sniffText("<svg:svg xmlns:svg=\"http://www.w3.org/2000/svg\"/>")),
          "sniff: a namespace-prefixed root element ('svg:svg') is still recognised");

    // The false positive a substring search would fall for: an HTML page that
    // embeds an SVG is not itself an SVG. The root element here is 'html',
    // and the scan never looks past it.
    check(sniffText("<html><body><svg xmlns=\"http://www.w3.org/2000/svg\">"
                    "<circle r=\"1\"/></svg></body></html>")
              .kind == FileKind::Unknown,
          "sniff: an HTML page with an inline <svg> is NOT Vector -- the root element is "
          "'html', which a substring search on '<svg' would have missed entirely");

    check(sniffText("This file talks about <svg> tags, but it is not one.\n").kind ==
              FileKind::Unknown,
          "sniff: plain text that merely mentions '<svg' is Unknown -- its first "
          "non-whitespace byte is not '<' at all");

    // Unterminated prologue constructs: the scan must give up rather than
    // read past the end of a short buffer looking for a closer that is not
    // there.
    check(sniffText("<!-- a comment that never closes " + minimalSvg).kind ==
              FileKind::Unknown,
          "sniff: an unterminated comment is Unknown, found without reading past the "
          "buffer looking for '-->'");
    check(sniffText("<?xml version=\"1.0\" encoding=\"UTF-8\"").kind == FileKind::Unknown,
          "sniff: an unterminated XML declaration is Unknown, found without reading past "
          "the buffer looking for '?>'");

    // The boundary inputs every scan has to survive.
    check(sniff({}).kind == FileKind::Unknown,
          "sniff: empty input does not reach the SVG scan as anything but Unknown");
    check(sniffText("   \t\r\n").kind == FileKind::Unknown,
          "sniff: whitespace-only input is Unknown -- there is no root element to find");
    check(sniffText("<").kind == FileKind::Unknown,
          "sniff: a lone '<' is Unknown rather than a name-parse running off the end");

    // Every ordinary assertion above this section still holds with `Vector`
    // in the enum -- a switch that forgot a case would have failed to build
    // at all (-Werror=switch), and nothing here retests that; this is the
    // complementary check that the new kind does not change what an existing
    // fixture sniffs as.
    check(sniff(pngBytes(4, 4)).kind == FileKind::Image,
          "sniff: a PNG still sniffs as Image, not disturbed by the new Vector case");
  }

  // --- B. a real .npaint, saved and sniffed ---------------------------------
  //
  // The assertion that ties io/FileKind's spec-derived header walk to reality.
  // Everything above proves the walk does what its author intended; this proves
  // the intention matches what `saveNpaint()` actually writes.
  std::printf("-- B. the marker is the one saveNpaint really writes --\n");
  std::string realNpaintPath;
  if (!oiioBackendCompiledIn()) {
    std::printf("  %-58s %s\n",
                "real .npaint fixture: skipped, this build has no writer", "n/a");
  } else {
    const std::filesystem::path p = scratch / "real.npaint";
    Document doc = Document::createBlank(64, 48, WorkingSpace{});
    const NpaintSaveResult saved = saveNpaint(doc, p.string());
    check(saved.ok, "real .npaint fixture: saveNpaint() wrote one");
    if (saved.ok) {
      realNpaintPath = p.string();
      std::ifstream in(p, std::ios::binary);
      const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
      check(!bytes.empty() && sniffFileKind(bytes.data(), bytes.size()).kind ==
                                  FileKind::NpaintDocument,
            "real .npaint: sniffed from its own bytes as a naturalPaint document -- this "
            "is what pins the EXR header walk to what saveNpaint() writes");

      // The other side of the same coin: an EXR this application did not write
      // has no marker and must sniff as a picture, or every EXR in the world
      // would be handed to the document reader.
      const DecodedImage flat = flattenDocumentToLinear(doc);
      const ExportResult exr = encodeLinearImage(flat, WorkingSpace{}, ImageFormat::Exr,
                                                 ExportTargetSpace::Rec709Linear,
                                                 ExportBitDepth::Half);
      if (exr.ok)
        check(sniffFileKind(exr.bytes.data(), exr.bytes.size()).kind == FileKind::Image,
              "a plain EXR written by io/Export sniffs as a picture, not as a document");
    }
  }

  // --- C. dispatch is by content, and the extension is never consulted ------
  //
  // The two assertions this whole section exists for.
  std::printf("-- C. content dispatch: the name is not the file --\n");
  {
    // A PNG called `.npaint`. An extension test opens this with the document
    // reader and refuses it; content dispatch opens the picture.
    const std::string liar = writeBytes("picture.npaint", pngBytes(9, 7));
    const OpenAnyResult r = openAnyFileAsDocument(liar);
    check(r.ok && r.kind == FileKind::Image,
          "a PNG named '.npaint' opens as a picture -- the name is not consulted");
    check(r.ok && r.document.document.width == 9 && r.document.document.height == 7,
          "...and it is the picture's own size, so it really was decoded");

    // A file carrying the `np:version` marker, called `.png`. It is not a
    // *loadable* document -- the synthetic header has no pixels -- and that is
    // the point: what is asserted is which reader it reached, which the refusal
    // names. An extension test would have sent this to the image decoder.
    const std::string masquerade = writeBytes("document.png", npaintish);
    const OpenAnyResult m = openAnyFileAsDocument(masquerade);
    check(m.kind == FileKind::NpaintDocument,
          "a file carrying np:version named '.png' is routed to the document reader");
    check(!m.ok && contains(m.status, "is a naturalPaint document"),
          "...and when it will not load, the refusal says it was one of ours rather than "
          "reporting an unreadable picture");

    // With a writer available, the same claim end to end: a genuine `.npaint`
    // renamed `.png` does not merely dispatch correctly, it opens.
    if (!realNpaintPath.empty()) {
      const std::filesystem::path renamed = scratch / "renamed.png";
      std::filesystem::copy_file(realNpaintPath, renamed,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      const OpenAnyResult g = openAnyFileAsDocument(renamed.string());
      check(g.ok && g.kind == FileKind::NpaintDocument,
            "a real .npaint renamed '.png' opens as a document, layers and all");
      check(g.ok && g.document.document.width == 64 && g.document.document.height == 48,
            "...at the canvas size the document was saved with, not a decoded image's");
      check(g.ok && g.document.path == renamed.string(),
            "...and it IS bound to its file, unlike a picture -- a .npaint has one");
    }
  }

  // --- D. the three refusals, told apart -----------------------------------
  std::printf("-- D. refusals name the file and say which kind of wrong --\n");
  {
    const std::string garbage = writeText("notes.txt", "this is not a picture at all\n");
    const OpenAnyResult g = openAnyFileAsDocument(garbage);
    check(!g.ok, "a text file is refused");
    check(contains(g.status, "notes.txt"), "...naming the file");
    check(g.kind == FileKind::Unknown && contains(g.status, "match no image format"),
          "...as 'we do not read this', which is the refusal a user cannot act on");

    // A real PNG, cut off partway through. The signature is intact, so this is
    // the *other* failure -- the one a user can act on, by finding a better
    // copy of the file.
    std::vector<uint8_t> whole = pngBytes(32, 32);
    whole.resize(whole.size() / 2);
    const std::string cut = writeBytes("half.png", whole);
    const OpenAnyResult c = openAnyFileAsDocument(cut);
    check(!c.ok, "a truncated PNG is refused");
    check(contains(c.status, "half.png") && contains(c.status, "PNG"),
          "...naming both the file and what it was");
    check(c.kind == FileKind::Image && contains(c.status, "damaged or truncated"),
          "...as 'your file is broken', which is the refusal a user CAN act on");

    check(g.status != c.status && !contains(g.status, "damaged") &&
              !contains(c.status, "match no image format"),
          "the two refusals are genuinely different sentences -- an implementation that "
          "said 'could not be decoded' for both would pass every other check here");

    // The refusals that come before any sniff, each naming the file.
    check(!openAnyFileAsDocument("").ok, "an empty path is refused");
    const OpenAnyResult missing =
        openAnyFileAsDocument((scratch / "no-such-file.png").string());
    check(!missing.ok && contains(missing.status, "no-such-file.png"),
          "a path that does not exist is refused by name");
    const OpenAnyResult folder = openAnyFileAsDocument(scratch.string());
    check(!folder.ok && contains(folder.status, "folder"),
          "a folder is refused as a folder, not as an unreadable file");
    const std::string empty = writeBytes("empty.png", {});
    const OpenAnyResult e0 = openAnyFileAsDocument(empty);
    check(!e0.ok && contains(e0.status, "0 bytes"),
          "a zero-byte file is refused as empty rather than as undecodable");
    check(missing.document.document.layers.empty() && folder.document.document.layers.empty() &&
              e0.document.document.layers.empty() && g.document.document.layers.empty() &&
              c.document.document.layers.empty(),
          "no refusal leaves a half-built document behind");

    // An SVG OPENS, as a one-layer Vector document. This used to be the
    // fourth refusal ("recognised, but there is no importer yet"); io/SvgImport
    // landed, so the contract inverted and these assertions inverted with it
    // rather than being deleted.
    const std::string svgPath = writeText(
        "picture.svg",
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"30\">"
        "<rect x=\"1\" y=\"2\" width=\"10\" height=\"20\" fill=\"#ff0000\"/>"
        "<circle cx=\"20\" cy=\"15\" r=\"5\" fill=\"#00ff00\"/></svg>\n");
    const OpenAnyResult svg = openAnyFileAsDocument(svgPath);
    check(svg.ok, "an SVG opens, now that io/SvgImport exists");
    check(svg.kind == FileKind::Vector,
          "...and is reported as Vector, not mistaken for an unrecognised format");
    check(svg.document.document.layers.size() == 1 &&
              svg.document.document.layers[0].kind == LayerKind::Vector,
          "...as exactly one Vector layer -- not rasterised into an RGB one");
    check(svg.document.document.width == 40 && svg.document.document.height == 30,
          "...on a canvas sized from the <svg> viewport");
    check(svg.document.document.layers.size() == 1 &&
              svg.document.document.layers[0].shapes.size() == 2,
          "...carrying both shapes the file declares");
    // **The id assignment, which nothing else would catch.** io/SvgImport
    // leaves every id at zero (core/VectorShape.hpp: zero means unassigned)
    // and app/PenTool keys its selection on the id, so an import that skipped
    // this step would produce a layer whose shapes are all individually
    // unselectable -- every one of them answering to shape 0. That failure is
    // invisible until someone tries to click a shape, which no headless test
    // does, so it is asserted here at the seam that assigns them.
    if (svg.document.document.layers.size() == 1) {
      const Layer& v = svg.document.document.layers[0];
      bool idsUnique = true;
      for (size_t i = 0; i < v.shapes.size(); ++i) {
        if (v.shapes[i].id == 0) idsUnique = false;
        for (size_t j = i + 1; j < v.shapes.size(); ++j)
          if (v.shapes[i].id == v.shapes[j].id) idsUnique = false;
      }
      check(idsUnique, "...with every shape id assigned and distinct, so app/PenTool can "
                       "tell them apart (zero would make them all one shape)");
      check(v.nextShapeId > v.shapes.size(),
            "...and nextShapeId past the last one, so a shape added later cannot collide");
    }
    check(contains(svg.status, "picture.svg") && contains(svg.status, "SVG"),
          "...and the status names both the file and SVG");

    // **An SVG bigger than the adapter can draw is refused, not opened.**
    //
    // This assertion exists because of a merge, and the merge produced no
    // conflict. The SVG branch was written when `kMaxDocumentPresetDimension`
    // (32768) was the only canvas limit in the tree, so it bounded the
    // viewport against that; core/CanvasLimits landed in parallel with the
    // real ceiling, which on this adapter is 16384. The two commits touch
    // different lines of `openAnyFileAsDocument()` and text-merged cleanly,
    // leaving an SVG declaring a 20000px viewport passing the only check it
    // had and then aborting the process at the first
    // `wgpuDeviceCreateTexture` -- exactly the failure the ceiling exists to
    // prevent, reintroduced through a path neither side's tests covered.
    //
    // Driven at `maxCanvasDimension() + 1` rather than a literal, so this
    // stays meaningful on an adapter that reports something other than 16384.
    {
      const long long over = static_cast<long long>(maxCanvasDimension()) + 1;
      const std::string huge = writeText(
          "huge.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                          std::to_string(over) + "\" height=\"10\">"
                          "<rect x=\"0\" y=\"0\" width=\"5\" height=\"5\" fill=\"#000\"/></svg>\n");
      const OpenAnyResult r = openAnyFileAsDocument(huge);
      check(!r.ok, "an SVG wider than the adapter can draw is refused, not opened");
      check(r.kind == FileKind::Vector,
            "...still recognised as an SVG -- refused for its size, not misidentified");
      check(r.document.document.layers.empty(),
            "...and leaves no half-built document behind");

      // The complement, so the guard cannot pass by refusing every SVG --
      // exactly at the ceiling must still open.
      const std::string atLimit = writeText(
          "at-limit.svg", "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
                              std::to_string(maxCanvasDimension()) + "\" height=\"4\">"
                              "<rect x=\"0\" y=\"0\" width=\"2\" height=\"2\" fill=\"#000\"/></svg>\n");
      const OpenAnyResult ok2 = openAnyFileAsDocument(atLimit);
      check(ok2.ok && ok2.document.document.width == maxCanvasDimension(),
            "...while an SVG exactly AT the ceiling still opens");
    }
    // Bound to nothing, exactly as an opened picture is: a Cmd-S here would
    // otherwise write EXR bytes over the user's .svg.
    check(svg.document.path.empty(),
          "...and is bound to no path, so Save cannot overwrite the .svg with EXR bytes");
  }

  // --- E. what an opened picture IS, and why Save cannot go wrong -----------
  //
  // The title/path decision, asserted rather than described. Identity is
  // checked on ids and on `path`/`title` directly and never on
  // `documentDisplayName()`, which prefers the path's filename over the title
  // -- app/selftest/CloseDecision.cpp once failed for exactly that reason.
  std::printf("-- E. an opened picture is bound to nothing, deliberately --\n");
  {
    const std::string picture = writeBytes("photo.png", pngBytes(200, 140));
    OpenAnyResult r = openAnyFileAsDocument(picture);
    check(r.ok, "a PNG opens as a document");
    if (r.ok) {
      const Document& d = r.document.document;
      check(d.width == 200 && d.height == 140,
            "the document is the picture's size");
      check(d.layers.size() == 1, "it has exactly one layer");
      if (d.layers.size() == 1) {
        check(d.layers[0].kind == LayerKind::RGB, "that layer is RGB-kind");
        check(d.layers[0].rgbTiles.has_value() &&
                  d.layers[0].rgbTiles->occupiedTileCount() > 0,
              "its tile storage is engaged and holds tiles -- the pixels arrived, rather "
              "than an empty layer of the right size being reported as success");
        // 200x140 crosses x=128 **and** y=128 (`core/Tile.hpp`'s kTileSize is
        // 128, and 140 > 128), so 2x2 = 4 tiles. This line said 2x1 when it was
        // written -- the arithmetic dropped the second tile row, and it was the
        // one assertion in this file nobody could check without running it.
        // app/selftest/ImageIO.cpp's neighbouring 140x140 -> 4 says the same
        // thing from the other side: 140 crosses 128 on whichever axis it is.
        check(d.layers[0].rgbTiles->occupiedTileCount() == 4,
              "exactly the 2x2 tiles a 200x140 image spans, not a count tied to some "
              "other canvas (PRD C2)");
      }

      check(r.document.path.empty(),
            "**the document is bound to no file** -- this is what stops the next Save "
            "writing .npaint bytes over the user's photo.png");
      check(r.document.title == "photo.png",
            "its title is the picture's own name, so the tab is not 'Untitled'");
      check(r.document.isDirty(),
            "it is dirty from birth -- it holds a document that exists nowhere on disk, "
            "so a close asks about it and the journal checkpoints it");
      check(!r.document.history.empty() &&
                r.document.history.entries().size() == 1,
            "history is seeded with exactly the one baseline entry -- the picture as it "
            "arrived, with nothing before it, because there was no earlier state of this "
            "document");
      bool warnedAboutBinding = false;
      for (const std::string& w : r.warnings)
        if (contains(w, "not bound to a file")) warnedAboutBinding = true;
      check(warnedAboutBinding,
            "the user is told, at the moment it happens, that Save is unavailable until "
            "Save As -- the surprising half of the decision is not left to be discovered");

      // The property the whole decision exists for, exercised rather than
      // argued: Save refuses, and the picture on disk is untouched.
      const DocumentOpResult save = saveDocument(r.document);
      check(!save.ok, "**Save refuses** an opened picture rather than choosing a file");
      check(contains(save.error, "Save As"),
            "...and says to use Save As, which is where a destination gets chosen");

      const std::vector<uint8_t> before = pngBytes(200, 140);
      std::ifstream after(picture, std::ios::binary);
      const std::vector<uint8_t> now((std::istreambuf_iterator<char>(after)),
                                     std::istreambuf_iterator<char>());
      check(now == before,
            "photo.png is byte-for-byte what it was -- the refused Save wrote nothing "
            "anywhere near it");

      // And the deliberate act still works, to a file the user named.
      if (oiioBackendCompiledIn()) {
        const std::string dest = (scratch / "photo.npaint").string();
        const DocumentOpResult saveAs = saveDocumentAs(r.document, dest);
        check(saveAs.ok, "Save As writes it, to the path the user chose");
        check(r.document.path == dest,
              "...and rebinds, so the next Save goes there and not to the PNG");
        std::ifstream stillThere(picture, std::ios::binary);
        const std::vector<uint8_t> pngAfter((std::istreambuf_iterator<char>(stillThere)),
                                            std::istreambuf_iterator<char>());
        check(pngAfter == before, "...and photo.png is STILL untouched afterwards");
      }
    }
  }

  // --- F. Import already takes every format this build reads ----------------
  //
  // `importImageAsLayer()` reads raw bytes and hands them to
  // `decodeImageLinear()`, which sniffs content -- so it has accepted every
  // decodable format since it landed, and this section is the proof of that
  // rather than a change to it.
  //
  // Driven off `allFormatCapabilities()`, the live runtime query, so the set
  // asserted is whatever THIS build supports: adding a format to the enum, or
  // building without OpenImageIO, changes what is tested without changing a
  // line here. **Every fixture is encoded in memory** through
  // `encodeLinearImage()` -- nothing is read from the repository.
  //
  // The formats this cannot reach are the read-only ones, PSD and camera raw:
  // no encoder exists for either -- this OpenImageIO has no PSD writer, and
  // camera raw has no writer in ANY OpenImageIO build, LibRaw or not (a
  // sensor's raw format is never something this library, or LibRaw itself,
  // produces) -- and the only way to test them would be to commit a binary
  // fixture, which is forbidden. Named here so the gap is visible rather than
  // implied by an absence.
  std::printf("-- F. import: every format class this build reads --\n");
  {
    OpenDocument host = makeBlankOpenDocument(256, 256, WorkingSpace{}, "host");
    const size_t layersBefore = host.document.layers.size();

    DecodedImage src;
    src.width = 24;
    src.height = 16;
    src.pixels.assign(static_cast<size_t>(src.width) * src.height * 4, 1.0f);
    // Fully opaque and mid-grey: JPEG and HDR have no alpha channel at all and
    // io/Export refuses a partially transparent image for them by name, so an
    // alpha < 1 fixture would fail on the encoder rather than on the importer.
    for (size_t i = 0; i < src.pixels.size(); i += 4) {
      src.pixels[i + 0] = 0.25f;
      src.pixels[i + 1] = 0.50f;
      src.pixels[i + 2] = 0.75f;
      src.pixels[i + 3] = 1.0f;
    }

    size_t tested = 0;
    size_t accepted = 0;
    std::string notAccepted;
    std::string couldNotEncode;
    for (const FormatCapability& cap : allFormatCapabilities()) {
      if (!cap.canRead || !cap.canWrite) continue;
      // The first depth this format can be written at without the backend
      // silently substituting another (io/Capabilities' probe, not a table).
      std::optional<ExportBitDepth> depth;
      for (const ExportBitDepth d : {ExportBitDepth::UInt8, ExportBitDepth::UInt16,
                                     ExportBitDepth::Half, ExportBitDepth::Float32})
        if (!depth && cap.canWriteDepth(d)) depth = d;
      if (!depth) continue;
      // Radiance HDR and EXR carry linear values; the integer formats are
      // written sRGB-encoded, which is what io/ImageDecode assumes on the way
      // back in. Either target round-trips for this test's purpose, which is
      // "was the file accepted", not "are the pixels identical".
      const ExportTargetSpace space = exportBitDepthIsFloat(*depth)
                                          ? ExportTargetSpace::Rec709Linear
                                          : ExportTargetSpace::Rec709Srgb;
      const ExportResult encoded =
          encodeLinearImage(src, WorkingSpace{}, cap.format, space, *depth);
      if (!encoded.ok) {
        couldNotEncode += std::string(couldNotEncode.empty() ? "" : ", ") +
                          imageFormatName(cap.format);
        continue;
      }
      ++tested;
      std::string name = std::string("fixture.") + imageFormatExtension(cap.format);
      const std::string path = writeBytes(name.c_str(), encoded.bytes);
      const ImportImageResult imported = importImageAsLayer(host, path);
      if (imported.ok) {
        ++accepted;
      } else {
        notAccepted += std::string(notAccepted.empty() ? "" : ", ") +
                       imageFormatName(cap.format) + " (" + imported.status + ")";
      }
    }
    std::printf("      encoded and imported %zu format(s); %s\n", tested,
                couldNotEncode.empty()
                    ? "every readable+writable format produced a fixture"
                    : ("could not encode: " + couldNotEncode).c_str());
    check(tested >= 4,
          "at least PRD I1's four stb formats produced an in-memory fixture, in either "
          "build configuration");
    check(accepted == tested && notAccepted.empty(),
          "import accepts every format this build can read -- it sniffs content through "
          "decodeImageLinear() and has no format list of its own");
    if (!notAccepted.empty()) std::printf("      refused: %s\n", notAccepted.c_str());
    check(host.document.layers.size() == layersBefore + accepted,
          "one layer per accepted fixture, so a format that was 'accepted' without "
          "adding anything cannot pass");

    // The one thing content-sniffing import buys that a name-based one would
    // not: a picture whose extension lies.
    const std::string mislabelled = writeBytes("actually-a-png.tga", pngBytes(8, 8));
    const size_t before = host.document.layers.size();
    const ImportImageResult liar = importImageAsLayer(host, mislabelled);
    check(liar.ok && host.document.layers.size() == before + 1,
          "import takes a PNG named '.tga' -- the decoder reads bytes, never the name");
  }

  // --- G. the drop routing rule, as a pure function ------------------------
  std::printf("-- G. drag and drop: the rule, without a window --\n");
  {
    check(dropActionFor(FileKind::NpaintDocument, false) == DropAction::OpenAsDocument &&
              dropActionFor(FileKind::NpaintDocument, true) == DropAction::OpenAsDocument,
          "a dropped .npaint always OPENS, document open or not -- importing one would "
          "flatten its whole stack into a single layer via its composite");
    check(dropActionFor(FileKind::Image, false) == DropAction::OpenAsDocument,
          "a dropped picture with nothing open opens -- there is no canvas to put it on");
    check(dropActionFor(FileKind::Image, true) == DropAction::ImportAsLayer,
          "a dropped picture with a document open becomes a layer in it -- what the "
          "gesture means everywhere else");
    check(dropActionFor(FileKind::Unknown, false) == DropAction::Refuse &&
              dropActionFor(FileKind::Unknown, true) == DropAction::Refuse,
          "an unrecognised file is refused rather than attempted twice");
    // An SVG opens as a document, document already open or not -- like a
    // `.npaint` and unlike a picture. Importing one as a LAYER is not a
    // missing branch but an unmade decision (see dropActionFor()'s own
    // comment: the shapes are in the SVG viewport's coordinates, and placing
    // them into a differently-sized document has three defensible answers).
    check(dropActionFor(FileKind::Vector, false) == DropAction::OpenAsDocument &&
              dropActionFor(FileKind::Vector, true) == DropAction::OpenAsDocument,
          "a dropped SVG opens as a document, whether or not one is already open -- never "
          "silently placed into the open document at a guessed position");
  }

  // --- G2. drop destination: the enum the drop *point* resolves to ---------
  //
  // `DropDestination::Unspecified` is the value every caller written before
  // this enum existed still gets (it is the default argument on both
  // `dropActionFor()` and `applyDroppedFiles()`), and OpenAnyFile.hpp's own
  // comment on that value is a promise: "identical to before", not "close to
  // before". This section asserts that promise directly rather than trusting
  // it, alongside the one case a destination is allowed to change the answer.
  std::printf("-- G2. drag and drop: destination changes only the tab-strip case --\n");
  {
    for (const FileKind k :
        {FileKind::NpaintDocument, FileKind::Image, FileKind::Vector, FileKind::Unknown}) {
      for (const bool open : {false, true}) {
        check(dropActionFor(k, open) == dropActionFor(k, open, DropDestination::Unspecified),
              "Unspecified reproduces the old two-argument rule exactly, for every "
              "kind/open combination");
      }
    }

    // The one case destination changes the answer: the tab strip always
    // opens a picture as a new document, even with one already open.
    check(dropActionFor(FileKind::Image, true, DropDestination::TabStrip) ==
              DropAction::OpenAsDocument,
          "a picture dropped on the tab strip opens as a new document, even though a "
          "document is already open -- the whole point of this feature");
    check(dropActionFor(FileKind::Image, false, DropDestination::TabStrip) ==
              DropAction::OpenAsDocument,
          "...and still opens with nothing open, same as every other destination");

    // The canvas still means "add a layer" when a document is open --
    // ActiveDocument computes what Unspecified always has for that case.
    check(dropActionFor(FileKind::Image, true, DropDestination::ActiveDocument) ==
              DropAction::ImportAsLayer,
          "a picture dropped on the canvas still becomes a layer when a document is open "
          "-- the destination did not change what the canvas has always meant");
    check(dropActionFor(FileKind::Image, false, DropDestination::ActiveDocument) ==
              DropAction::OpenAsDocument,
          "...and opens as a document when nothing is open -- there is no canvas to add to");

    // A .npaint is never a layer, and an unrecognised file is never opened,
    // regardless of where either lands.
    for (const DropDestination d :
        {DropDestination::Unspecified, DropDestination::TabStrip,
         DropDestination::ActiveDocument}) {
      check(dropActionFor(FileKind::NpaintDocument, false, d) == DropAction::OpenAsDocument &&
                dropActionFor(FileKind::NpaintDocument, true, d) == DropAction::OpenAsDocument,
            "a .npaint always opens, whatever the destination and whether a document is "
            "already open -- a document is never a layer");
      check(dropActionFor(FileKind::Unknown, false, d) == DropAction::Refuse &&
                dropActionFor(FileKind::Unknown, true, d) == DropAction::Refuse,
            "an unrecognised file is refused regardless of destination");
      check(dropActionFor(FileKind::Vector, false, d) == DropAction::OpenAsDocument &&
                dropActionFor(FileKind::Vector, true, d) == DropAction::OpenAsDocument,
            "an SVG opens as a document regardless of destination and whether one is open "
            "-- every combination of the other two arguments, for the kind whose "
            "place-into-a-document rule is deliberately unmade");
    }
  }

  // --- G3. dropDestinationForPoint: the hit test, without a window ---------
  //
  // Bands built by hand, not by `atelierLayout()` -- this section is testing
  // the classifier's own point-in-rect arithmetic, and `atelierLayout()`'s
  // own arithmetic is app/selftest/AtelierChrome.cpp's job. Half-open rects
  // throughout ([x, x+w)), matching every other rect test in this codebase.
  std::printf("-- G3. drag and drop: classifying a point against the bands --\n");
  {
    // Only the two rectangles the classifier reads. The other bands used to
    // be set here too, which was misleading: it suggested the hit test knew
    // about the title bar and the tool palette, when in fact anything that is
    // neither of these two is Unspecified BY FALLTHROUGH, which is exactly
    // what the "20, 200" and "700, 200" cases below prove.
    DropBands bands;
    bands.tabStrip = DropBandRect{0.0f, 36.0f, 800.0f, 34.0f};
    bands.canvas = DropBandRect{52.0f, 116.0f, 600.0f, 400.0f};

    check(dropDestinationForPoint(400.0f, 50.0f, bands) == DropDestination::TabStrip,
          "a point inside the tab strip band classifies as TabStrip");
    check(dropDestinationForPoint(400.0f, 300.0f, bands) == DropDestination::ActiveDocument,
          "a point inside the canvas band classifies as ActiveDocument");
    check(dropDestinationForPoint(20.0f, 200.0f, bands) == DropDestination::Unspecified,
          "a point on the tool palette -- neither band -- classifies as Unspecified");
    check(dropDestinationForPoint(700.0f, 200.0f, bands) == DropDestination::Unspecified,
          "...the same for the right-hand column");
    check(dropDestinationForPoint(400.0f, 10.0f, bands) == DropDestination::Unspecified,
          "...the title bar");
    check(dropDestinationForPoint(400.0f, 520.0f, bands) == DropDestination::Unspecified,
          "...and the status bar");

    check(dropDestinationForPoint(0.0f, 36.0f, bands) == DropDestination::TabStrip,
          "the tab strip band's own top-left corner is inside it (half-open: the top "
          "edge counts)");
    check(dropDestinationForPoint(0.0f, 70.0f, bands) == DropDestination::Unspecified,
          "one point past its bottom edge (y=70, h=34 from y=36) is not");

    // ui/AtelierLayout.cpp collapses the tab strip to zero height when
    // `showTabStrip` is false (no document open, so nothing was drawn there)
    // -- an empty rect must never match, or a drop would be classified
    // against a band the chrome never actually rendered this frame.
    DropBands noTabStrip = bands;
    noTabStrip.tabStrip = DropBandRect{0.0f, 36.0f, 800.0f, 0.0f};
    check(dropDestinationForPoint(400.0f, 36.0f, noTabStrip) == DropDestination::Unspecified,
          "a zero-height tab strip band never matches, exactly as ui/AtelierLayout produces "
          "it when there is no document open to name");
  }

  // --- H. a whole drop, including twelve files at once ---------------------
  std::printf("-- H. drag and drop: one gesture, however many files --\n");
  {
    // Twelve pictures onto an empty session.
    std::vector<std::string> twelve;
    for (int i = 0; i < 12; ++i) {
      char name[64];
      std::snprintf(name, sizeof(name), "batch-%02d.png", i);
      twelve.push_back(writeBytes(name, pngBytes(16 + i, 16)));
    }
    DocumentSession empty;
    RecentDocuments recent;
    const DropOutcome batch = applyDroppedFiles(empty, &recent, twelve);
    check(batch.opened == 1 && batch.imported == 11 && batch.refused == 0,
          "twelve pictures onto an empty session: ONE document, eleven layers in it -- "
          "not twelve tabs, and not one file used and eleven dropped");
    check(empty.count() == 1,
          "...and the session really holds one document, so the counts are not a story");
    check(empty.active() != nullptr && empty.active()->document.layers.size() == 12,
          "...whose stack is the one opened layer plus the eleven imported ones");
    check(!batch.status.empty() && contains(batch.status, "12 files dropped"),
          "the gesture reports itself once, naming how many files it was");

    // The same twelve onto a session that already has a document: all layers.
    DocumentSession busy;
    busy.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "busy"));
    const size_t layersBefore = busy.active()->document.layers.size();
    const DropOutcome onto = applyDroppedFiles(busy, &recent, twelve);
    check(onto.opened == 0 && onto.imported == 12,
          "the same twelve onto an open document: twelve layers, no new tab");
    check(busy.count() == 1 &&
              busy.active()->document.layers.size() == layersBefore + 12,
          "...all of them in the document that was already there");

    // --- the layer a drop offers straight to a transform -------------------
    //
    // A dropped picture should be movable the instant it lands, so
    // `DropOutcome::transformableLayer` names the layer main.cpp puts into a
    // TransformSession. The whole content of the rule is WHEN IT IS ABSENT,
    // which is why five of these six assertions are about it being empty:
    // an index offered for the wrong gesture would start a transform on a
    // layer nobody pointed at, and a transform on the wrong layer looks
    // exactly like a transform on the right one until it is committed.
    check(!onto.transformableLayer,
          "twelve pictures imported at once offer NO layer to transform -- eleven of "
          "the twelve would be an arbitrary choice, so none is made");
    check(!batch.transformableLayer,
          "...and neither does the twelve-onto-an-empty-session drop, which opened one "
          "document and imported the rest");
    {
      DocumentSession solo;
      solo.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "solo"));
      const size_t before = solo.active()->document.layers.size();
      const std::string one = writeBytes("solo-drop.png", pngBytes(16, 16));
      const DropOutcome single = applyDroppedFiles(solo, &recent, {one});
      check(single.opened == 0 && single.imported == 1,
            "(setup) one picture onto an open document imports exactly one layer");
      check(single.transformableLayer.has_value(),
            "ONE picture onto an open document DOES offer a layer to transform -- the "
            "unambiguous gesture, and the one a user drags a photo in to perform");
      check(single.transformableLayer && *single.transformableLayer == before,
            "...and it names the layer that was actually imported (the new top of the "
            "stack), not a stale index or a guess at layers.size()");

      // Nothing to import into: the drop OPENS instead, and an opened document
      // is not something to start a transform on -- the picture IS the
      // document, so there is nothing to move it relative to.
      DocumentSession fresh;
      const DropOutcome opened = applyDroppedFiles(fresh, &recent, {one});
      check(opened.opened == 1 && opened.imported == 0 && !opened.transformableLayer,
            "the same picture onto an EMPTY session opens a document and offers nothing "
            "to transform -- the picture is the document");

      // A file that lands nowhere offers nothing, which is the case that would
      // otherwise carry an index left over from a previous loop iteration.
      DocumentSession refuseSession;
      refuseSession.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "refuse"));
      const std::vector<uint8_t> junkBytes = {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'};
      const std::string junk = writeBytes("not-a-picture.png", junkBytes);
      const DropOutcome refused = applyDroppedFiles(refuseSession, &recent, {junk});
      check(refused.refused == 1 && !refused.transformableLayer,
            "a refused file offers nothing to transform -- no index survives a drop "
            "where nothing landed");
    }

    // The same twelve, dropped on the TAB STRIP of a session that already has
    // a document open: twelve new documents, not one document with eleven
    // more layers -- OpenAnyFile.hpp's applyDroppedFiles() comment argues why
    // this is not folded into the "first opens, rest import" batching the
    // no-destination case uses: the tab strip means "a new document" and that
    // does not stop being true for file #2 just because file #1 already ran.
    DocumentSession busyTabStrip;
    OpenDocument* preExisting =
        busyTabStrip.add(makeBlankOpenDocument(64, 64, WorkingSpace{}, "busy"));
    const DropOutcome tabStripBatch = applyDroppedFiles(busyTabStrip, &recent, twelve,
                                                        DropDestination::TabStrip);
    check(tabStripBatch.opened == 12 && tabStripBatch.imported == 0,
          "twelve pictures dropped on the tab strip: twelve documents opened, none "
          "imported -- not the 'one document, eleven layers' rule a canvas drop uses");
    check(busyTabStrip.count() == 13,
          "...thirteen documents in the session: the one that was already open, plus the "
          "twelve just opened");
    check(preExisting->document.layers.size() == 1,
          "...and the document that was already open is untouched -- still its one "
          "original layer, because nothing was imported into it");

    // A mixture, resolved file by file in delivery order.
    DocumentSession mixed;
    std::vector<std::string> mixture = {
        writeText("junk.dat", "not a picture"),
        writeBytes("first.png", pngBytes(20, 20)),
        writeBytes("second.png", pngBytes(21, 21)),
    };
    const DropOutcome mix = applyDroppedFiles(mixed, &recent, mixture);
    check(mix.refused == 1 && mix.opened == 1 && mix.imported == 1,
          "a mixed drop resolves in order: the junk is refused, the first picture opens "
          "because nothing was, the second lands in what the first opened");
    check(mix.problems.size() == 1 && contains(mix.problems[0], "junk.dat"),
          "the refused file is named, so a drop that half-worked says which half");
    check(contains(mix.status, "junk.dat"),
          "...and that name reaches the status line rather than only the struct");
    // This drop imported exactly ONE picture -- the count half of the
    // transform rule is satisfied -- and it is still refused a transform,
    // because it also OPENED a document. That opening changed which document
    // is active, so the index would name a layer in a stack the user is no
    // longer looking at. This is the only fixture in the file where the two
    // halves of that rule disagree, which makes it the only one that can tell
    // them apart.
    check(mix.imported == 1 && !mix.transformableLayer,
          "a mixed drop that opened a document AND imported one picture offers NO "
          "layer to transform -- the count alone is not enough, because opening moved "
          "the active document out from under the index");

    // Everything failing is still one legible answer.
    DocumentSession none;
    std::vector<std::string> allBad;
    for (int i = 0; i < 20; ++i) {
      char name[64];
      std::snprintf(name, sizeof(name), "bad-%02d.dat", i);
      allBad.push_back(writeText(name, "no"));
    }
    const DropOutcome bad = applyDroppedFiles(none, &recent, allBad);
    check(bad.refused == 20 && bad.opened == 0 && bad.imported == 0,
          "twenty unreadable files: all twenty counted, none opened");
    check(bad.problems.size() == 8,
          "...but only eight named, so the status stays readable");
    check(contains(bad.status, "12 more not listed"),
          "...and the twelve it did not name are counted rather than lost");

    // A gesture that carried nothing -- SDL raises DROP_BEGIN when a drag
    // merely enters the window, so this is reachable.
    DocumentSession quiet;
    const DropOutcome nothing = applyDroppedFiles(quiet, &recent, {});
    check(!nothing.status.empty() && nothing.opened == 0 && nothing.imported == 0 &&
              nothing.refused == 0 && quiet.empty(),
          "a drop with no files changes nothing and still says something");
  }

  std::filesystem::remove_all(scratch, ec);

  std::printf("[selftest] open any file %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
