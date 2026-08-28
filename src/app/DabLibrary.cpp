#include "app/DabLibrary.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "color/Space.hpp"
#include "io/AbrBrushes.hpp"
#include "io/GimpBrush.hpp"
#include "io/Export.hpp"
#include "io/ImageDecode.hpp"
#include "ops/PointOps.hpp"

namespace fs = std::filesystem;

namespace np {
namespace {

// Bumped only when a row's field list changes. An index this build cannot
// read is discarded rather than half-parsed -- it is a cache, and the cost of
// throwing it away is one slower scan.
constexpr int kDabIndexVersion = 1;

// A tip larger than this is refused with a note rather than decoded. Bitmap
// tips are stamped per dab, so a 8000x8000 "brush" is a stall, not a brush;
// Photoshop's own sampled tips top out at 5000 and the largest in the four
// packs measured here is 1802. The bound is generous on purpose -- it exists
// to stop a mis-dropped photograph, not to have an opinion about tip size.
constexpr int32_t kMaxDabDimension = 4096;

bool hasExtension(const std::string& path, std::initializer_list<const char*> exts) {
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const char* e : exts)
    if (ext == e) return true;
  return false;
}

std::string escapeField(const std::string& s) {
  // The index is a tab-separated text file beside the other preference files,
  // and a relative path can legally contain a tab or a newline. Escaped
  // rather than quoted, because a quoted form needs a quote rule too.
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '\\') out += "\\\\";
    else if (c == '\t') out += "\\t";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out;
}

std::string unescapeField(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) { out += s[i]; continue; }
    ++i;
    if (s[i] == 't') out += '\t';
    else if (s[i] == 'n') out += '\n';
    else out += s[i];
  }
  return out;
}

std::vector<std::string> splitTabs(const std::string& line) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == '\t') {
      out.push_back(line.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

// The filename without its extension, which is what a picker should show.
// A `.gih` cell appends its own index, because N cells sharing one label is
// a picker nobody can use.
std::string displayNameFor(const std::string& relPath) {
  std::string name = fs::path(relPath).stem().string();
  return name.empty() ? relPath : name;
}

// One `stat`, in the terms the index stores. Returns false when the entry has
// gone away between iteration and here, which is an ordinary race on a folder
// the user is editing and not an error.
bool statFile(const fs::path& p, uint64_t& sizeOut, int64_t& mtimeOut) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec) return false;
  const auto mtime = fs::last_write_time(p, ec);
  if (ec) return false;
  sizeOut = static_cast<uint64_t>(size);
  mtimeOut = static_cast<int64_t>(mtime.time_since_epoch().count());
  return true;
}

std::vector<uint8_t> readFileBytes(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
}

BrushTipBitmap bitmapFromGimpTip(const GimpBrushTip& tip) {
  BrushTipBitmap out;
  out.width = tip.width;
  out.height = tip.height;
  out.alpha = tip.alpha;  // io/GimpBrush already normalises to 255 = coverage
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// §4's one rule, and the header's argument for it.
// ---------------------------------------------------------------------------
BrushTipBitmap coverageFromDecodedImage(uint32_t width, uint32_t height,
                                        const std::vector<float>& pixels) {
  BrushTipBitmap out;
  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (width == 0 || height == 0 || pixels.size() != texels * 4) return out;

  out.width = static_cast<int32_t>(width);
  out.height = static_cast<int32_t>(height);
  out.alpha.resize(texels);

  // "Non-trivial" = some texel is not fully opaque. A source with no alpha
  // channel decodes as opaque everywhere (io/ImageDecode.hpp), so this single
  // test covers both cases the header says want the same answer.
  bool hasAlpha = false;
  for (size_t i = 0; i < texels && !hasAlpha; ++i)
    if (pixels[i * 4 + 3] < 1.0f) hasAlpha = true;

  for (size_t i = 0; i < texels; ++i) {
    float coverage;
    if (hasAlpha) {
      coverage = pixels[i * 4 + 3];
    } else {
      // `computeLuma()` on linear RGB, then one `srgbEncode()` of the scalar
      // -- core/SelectionRefine's own order, restated in the header. A
      // straight `1 - linearLuma` would call a mid-grey three-quarters
      // opaque, which is not the tip the artist drew.
      const float luma = computeLuma({pixels[i * 4], pixels[i * 4 + 1], pixels[i * 4 + 2]});
      coverage = 1.0f - srgbEncode(luma);
    }
    coverage = std::clamp(coverage, 0.0f, 1.0f);
    // Round rather than truncate: `static_cast<uint8_t>(1.0f * 255.0f)` is
    // 255 only because 255.0f is exact, and every value below it would lose
    // half a level for no reason.
    out.alpha[i] = static_cast<uint8_t>(coverage * 255.0f + 0.5f);
  }
  return out;
}

uint64_t dabFingerprint(const BrushTipBitmap& bitmap) noexcept {
  // FNV-1a, 64-bit, with the dimensions mixed in first so a 4x9 and a 9x4 tip
  // carrying the same bytes are two fingerprints and not one.
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](uint8_t byte) {
    h ^= byte;
    h *= 1099511628211ull;
  };
  for (int shift = 0; shift < 32; shift += 8) mix(static_cast<uint8_t>(bitmap.width >> shift));
  for (int shift = 0; shift < 32; shift += 8) mix(static_cast<uint8_t>(bitmap.height >> shift));
  for (const uint8_t byte : bitmap.alpha) mix(byte);
  return h;
}

// ---------------------------------------------------------------------------
// Paths -- app/BrushLibraryFile's resolver, one variable for the whole library.
// ---------------------------------------------------------------------------
std::string defaultDabRootPath() {
  if (const char* explicitPath = std::getenv("NP_DAB_DIR")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home) return std::string(home) + "/Library/Application Support/naturalPaint";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint";
#endif
  // No HOME at all. app/BrushLibraryFile's own reasoning: the working
  // directory is a poor place for user data but a real, writable one, and an
  // empty path would turn "collect brush tips" into something that forgets.
  return ".";
}

std::string dabUserRootPath() { return defaultDabRootPath() + "/dabs"; }
std::string dabImportedRootPath() { return defaultDabRootPath() + "/dabs-imported"; }
std::string dabIndexPath() { return defaultDabRootPath() + "/dab-index.txt"; }

// ---------------------------------------------------------------------------
void DabLibrary::setRoots(std::string userRoot, std::string importedRoot,
                          std::string indexPath) {
  userRoot_ = std::move(userRoot);
  importedRoot_ = std::move(importedRoot);
  indexPath_ = std::move(indexPath);
  entries_.clear();
  refusals_.clear();
  indexLoaded_ = false;
}

const DabEntry* DabLibrary::find(const std::string& id) const noexcept {
  for (const DabEntry& e : entries_)
    if (e.id == id) return &e;
  return nullptr;
}

std::string DabLibrary::indexText() const {
  std::ostringstream out;
  out << "version " << kDabIndexVersion << "\n";
  // Refusals first, tagged, so a row's kind is decided by its first field and
  // an older parser (which skips rows it cannot make sense of) drops them
  // rather than reading one as an entry.
  for (const DabRefusal& r : refusals_) {
    out << "!\t" << escapeField(r.relPath) << '\t'
        << (r.root == DabRoot::User ? "user" : "imported") << '\t' << r.sizeBytes << '\t'
        << r.mtimeNs << '\t' << escapeField(r.note) << '\n';
  }
  for (const DabEntry& e : entries_) {
    out << escapeField(e.id) << '\t' << escapeField(e.relPath) << '\t'
        << (e.root == DabRoot::User ? "user" : "imported") << '\t'
        << static_cast<int>(e.source) << '\t' << e.frame << '\t' << e.width << '\t' << e.height
        << '\t' << e.fingerprint << '\t' << e.sizeBytes << '\t' << e.mtimeNs << '\t'
        << (e.haveSpacing ? 1 : 0) << '\t' << e.spacingPercent << '\t' << escapeField(e.name)
        << '\n';
  }
  return out.str();
}

void DabLibrary::parseIndex(const std::string& text) {
  entries_.clear();
  refusals_.clear();
  std::istringstream in(text);
  std::string line;
  bool sawVersion = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (!sawVersion) {
      sawVersion = true;
      // A version this build does not know is a cache from a newer build.
      // Discard the whole file rather than guess at its columns -- one
      // slower scan is the entire cost.
      if (line.rfind("version ", 0) != 0 ||
          std::atoi(line.c_str() + 8) != kDabIndexVersion) {
        entries_.clear();
        refusals_.clear();
        return;
      }
      continue;
    }
    const std::vector<std::string> f = splitTabs(line);
    if (!f.empty() && f[0] == "!") {
      if (f.size() < 6) continue;
      DabRefusal r;
      r.relPath = unescapeField(f[1]);
      r.root = (f[2] == "imported") ? DabRoot::Imported : DabRoot::User;
      r.sizeBytes = std::strtoull(f[3].c_str(), nullptr, 10);
      r.mtimeNs = std::strtoll(f[4].c_str(), nullptr, 10);
      r.note = unescapeField(f[5]);
      if (!r.relPath.empty()) refusals_.push_back(std::move(r));
      continue;
    }
    if (f.size() < 13) continue;  // a truncated row is skipped, not fatal
    DabEntry e;
    e.id = unescapeField(f[0]);
    e.relPath = unescapeField(f[1]);
    e.root = (f[2] == "imported") ? DabRoot::Imported : DabRoot::User;
    e.source = static_cast<DabSource>(std::atoi(f[3].c_str()));
    e.frame = std::atoi(f[4].c_str());
    e.width = std::atoi(f[5].c_str());
    e.height = std::atoi(f[6].c_str());
    e.fingerprint = std::strtoull(f[7].c_str(), nullptr, 10);
    e.sizeBytes = std::strtoull(f[8].c_str(), nullptr, 10);
    e.mtimeNs = std::strtoll(f[9].c_str(), nullptr, 10);
    e.haveSpacing = f[10] == "1";
    e.spacingPercent = static_cast<float>(std::atof(f[11].c_str()));
    e.name = unescapeField(f[12]);
    if (e.id.empty() || e.relPath.empty()) continue;
    entries_.push_back(std::move(e));
  }
}

void DabLibrary::loadIndex() {
  if (indexLoaded_) return;
  indexLoaded_ = true;
  if (indexPath_.empty()) return;
  std::ifstream in(indexPath_);
  if (!in) return;  // a first run, not an error
  std::ostringstream buf;
  buf << in.rdbuf();
  parseIndex(buf.str());
}

bool DabLibrary::decodeFile(const std::string& root, DabRoot which, const std::string& relPath,
                            std::vector<DabEntry>& out, std::string& note) {
  const fs::path full = fs::path(root) / relPath;
  const std::vector<uint8_t> bytes = readFileBytes(full);
  if (bytes.empty()) {
    note = relPath + ": empty or unreadable";
    return false;
  }
  ++decodeCount_;

  uint64_t sizeBytes = 0;
  int64_t mtimeNs = 0;
  statFile(full, sizeBytes, mtimeNs);

  auto push = [&](BrushTipBitmap bmp, const char* prefix, DabSource source, int32_t frame,
                  std::string name, bool haveSpacing, float spacing) {
    DabEntry e;
    e.id = std::string(prefix) + relPath;
    if (source == DabSource::Gimp && frame > 0) e.id += "#" + std::to_string(frame);
    e.name = std::move(name);
    e.relPath = relPath;
    e.root = which;
    e.source = source;
    e.frame = frame;
    e.width = bmp.width;
    e.height = bmp.height;
    e.fingerprint = dabFingerprint(bmp);
    e.haveSpacing = haveSpacing;
    e.spacingPercent = spacing;
    e.sizeBytes = sizeBytes;
    e.mtimeNs = mtimeNs;
    e.bitmap = std::make_shared<const BrushTipBitmap>(std::move(bmp));
    out.push_back(std::move(e));
  };

  if (hasExtension(relPath, {"gbr", "gih"})) {
    const bool pipe = hasExtension(relPath, {"gih"});
    const GimpBrushResult r =
        pipe ? readGimpBrushPipe(std::span<const uint8_t>(bytes))
             : readGimpBrush(std::span<const uint8_t>(bytes));
    if (!r.ok) {
      note = relPath + ": " + r.error;
      return false;
    }
    int32_t frame = 0;
    for (const GimpBrushTip& tip : r.tips) {
      if (tip.width > kMaxDabDimension || tip.height > kMaxDabDimension) {
        note = relPath + ": " + std::to_string(tip.width) + "x" + std::to_string(tip.height) +
               " exceeds the " + std::to_string(kMaxDabDimension) + " px limit";
        ++frame;
        continue;
      }
      // A hose cell keeps the file's own cell name when it has one, and falls
      // back to "<file> N" -- the standard lets a cell name be anything,
      // including empty.
      std::string name = tip.name.empty()
                             ? displayNameFor(relPath) + (pipe ? " " + std::to_string(frame + 1) : "")
                             : tip.name;
      push(bitmapFromGimpTip(tip), pipe ? "gih:" : "gbr:", DabSource::Gimp, pipe ? frame : 0,
           std::move(name), tip.haveSpacing, static_cast<float>(tip.spacingPercent));
      ++frame;
    }
    if (out.empty()) {
      if (note.empty()) note = relPath + ": no usable tips";
      return false;
    }
    return true;
  }

  std::string error;
  const DecodedImage img = decodeImageLinear(bytes.data(), bytes.size(), &error);
  if (!img.valid()) {
    note = relPath + ": " + (error.empty() ? "not an image this build can decode" : error);
    return false;
  }
  if (img.width > static_cast<uint32_t>(kMaxDabDimension) ||
      img.height > static_cast<uint32_t>(kMaxDabDimension)) {
    note = relPath + ": " + std::to_string(img.width) + "x" + std::to_string(img.height) +
           " exceeds the " + std::to_string(kMaxDabDimension) + " px limit";
    return false;
  }
  // **An extracted `.abr` tip is recognised by where it sits and what it is
  // called**, not by a sidecar or a marker inside the file: a `.png` directly
  // under the imported root whose stem is a bare uuid is one, and takes the
  // `abr:<uuid>` id the preset stores. That is the convention
  // `extractAbrTips()` writes to, stated in one place and read in one place.
  //
  // A file the user drops into `dabs-imported/` themselves and happens to name
  // like a uuid would be read the same way. That is a folder the header says
  // belongs to the application, so the collision is not one this build needs
  // to arbitrate -- and the consequence is a working dab under a surprising
  // id, not a broken one.
  const std::string stem = fs::path(relPath).stem().string();
  const bool looksLikeUuid =
      which == DabRoot::Imported && relPath.find('/') == std::string::npos &&
      hasExtension(relPath, {"png"}) && stem.size() >= 8 &&
      std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-';
      });
  if (looksLikeUuid) {
    DabEntry e;
    e.id = "abr:" + stem;
    e.name = stem;
    e.relPath = relPath;
    e.root = which;
    e.source = DabSource::Abr;
    BrushTipBitmap bmp = coverageFromDecodedImage(img.width, img.height, img.pixels);
    e.width = bmp.width;
    e.height = bmp.height;
    e.fingerprint = dabFingerprint(bmp);
    e.sizeBytes = sizeBytes;
    e.mtimeNs = mtimeNs;
    e.bitmap = std::make_shared<const BrushTipBitmap>(std::move(bmp));
    out.push_back(std::move(e));
    return true;
  }

  push(coverageFromDecodedImage(img.width, img.height, img.pixels), "file:", DabSource::Image, 0,
       displayNameFor(relPath), false, 0.0f);
  return true;
}

DabScanResult DabLibrary::rescan() {
  loadIndex();
  DabScanResult result;

  // The index, keyed by what a scan can compare without opening the file.
  std::unordered_map<std::string, std::vector<DabEntry>> previous;
  for (DabEntry& e : entries_) {
    const std::string key = (e.root == DabRoot::User ? "u/" : "i/") + e.relPath;
    previous[key].push_back(std::move(e));
  }
  entries_.clear();

  // The refusals the last scan recorded, keyed the same way. A file matching
  // one by `(size, mtime)` is not opened again; its note is replayed.
  std::unordered_map<std::string, DabRefusal> priorRefusals;
  for (DabRefusal& r : refusals_)
    priorRefusals[(r.root == DabRoot::User ? "u/" : "i/") + r.relPath] = std::move(r);
  refusals_.clear();

  std::vector<DabEntry> fresh;         // decoded this pass, id not yet final
  std::vector<DabEntry> carried;       // reused from the index, untouched

  const std::pair<const std::string&, DabRoot> roots[] = {
      {userRoot_, DabRoot::User},
      {importedRoot_, DabRoot::Imported},
  };
  for (const auto& [root, which] : roots) {
    if (root.empty()) continue;
    std::error_code ec;
    // **Neither root is created here** (header §"rescan"): a missing folder
    // finds nothing, which is the right answer on a first run and avoids
    // making directories because a picker opened.
    if (!fs::is_directory(root, ec)) continue;

    // Sorted, so the scan order -- and therefore which of two identical files
    // wins a fingerprint repair (§3) -- is the same on every platform and in
    // every run, rather than whatever order the filesystem happens to return.
    std::vector<std::string> relPaths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             ec),
         end;
         it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || ec) continue;
      const fs::path rel = fs::relative(it->path(), root, ec);
      if (ec || rel.empty()) continue;
      const std::string relStr = rel.generic_string();
      // A leading dot is the platform's own metadata (`.DS_Store`,
      // `._resource`), not a tip somebody dropped.
      if (!relStr.empty() && rel.filename().string()[0] == '.') continue;
      relPaths.push_back(relStr);
    }
    std::sort(relPaths.begin(), relPaths.end());

    for (const std::string& relPath : relPaths) {
      const fs::path full = fs::path(root) / relPath;
      uint64_t sizeBytes = 0;
      int64_t mtimeNs = 0;
      if (!statFile(full, sizeBytes, mtimeNs)) continue;

      const std::string key = (which == DabRoot::User ? "u/" : "i/") + relPath;

      // A file this build already declined, unchanged since. Replayed, not
      // reopened -- otherwise a folder of holiday photos costs one decode
      // attempt each, on every focus event.
      const auto priorRefusal = priorRefusals.find(key);
      if (priorRefusal != priorRefusals.end() &&
          priorRefusal->second.sizeBytes == sizeBytes &&
          priorRefusal->second.mtimeNs == mtimeNs) {
        ++result.rejected;
        result.notes.push_back(priorRefusal->second.note);
        refusals_.push_back(std::move(priorRefusal->second));
        priorRefusals.erase(priorRefusal);
        continue;
      }
      if (priorRefusal != priorRefusals.end()) priorRefusals.erase(priorRefusal);

      const auto found = previous.find(key);
      if (found != previous.end() && !found->second.empty() &&
          found->second.front().sizeBytes == sizeBytes &&
          found->second.front().mtimeNs == mtimeNs) {
        // **Unchanged: nothing is opened, nothing is decoded.** §2, and the
        // property `decodeCount()` exists to let `--selftest` assert.
        for (DabEntry& e : found->second) carried.push_back(std::move(e));
        result.unchanged += static_cast<int32_t>(found->second.size());
        previous.erase(found);
        continue;
      }
      if (found != previous.end()) previous.erase(found);

      std::vector<DabEntry> decoded;
      std::string note;
      if (!decodeFile(root, which, relPath, decoded, note)) {
        ++result.rejected;
        if (note.empty()) note = relPath + ": not a brush tip this build can read";
        result.notes.push_back(note);
        DabRefusal r;
        r.relPath = relPath;
        r.root = which;
        r.sizeBytes = sizeBytes;
        r.mtimeNs = mtimeNs;
        r.note = note;
        refusals_.push_back(std::move(r));
        continue;
      }
      for (DabEntry& e : decoded) fresh.push_back(std::move(e));
    }
  }

  // §3's rename repair. Anything still in `previous` was not seen this pass;
  // a fresh entry whose fingerprint matches one of them is that file under a
  // new name, and keeps its old id so a preset pointing at it still resolves.
  std::unordered_map<uint64_t, std::string> vanishedByFingerprint;
  for (auto& [key, list] : previous) {
    for (DabEntry& e : list) {
      result.removed += 1;
      if (e.fingerprint != 0) vanishedByFingerprint.emplace(e.fingerprint, e.id);
    }
  }
  for (DabEntry& e : fresh) {
    const auto match = vanishedByFingerprint.find(e.fingerprint);
    if (match != vanishedByFingerprint.end() && match->second != e.id) {
      e.id = match->second;
      vanishedByFingerprint.erase(match);
      ++result.repaired;
      --result.removed;
    } else {
      ++result.added;
    }
  }

  entries_ = std::move(carried);
  for (DabEntry& e : fresh) entries_.push_back(std::move(e));
  std::sort(entries_.begin(), entries_.end(),
            [](const DabEntry& a, const DabEntry& b) { return a.id < b.id; });

  // **An empty library writes nothing at all.** A scan that found no dabs and
  // has no index to update has nothing to record, and creating a ten-byte
  // stub in the user's Application Support because a picker opened is the
  // same impoliteness as creating the roots -- one this module already
  // refuses above and should not do by a side door. An existing index IS
  // rewritten when it empties out, because "everything was deleted" is a
  // fact worth keeping.
  std::sort(refusals_.begin(), refusals_.end(),
            [](const DabRefusal& a, const DabRefusal& b) { return a.relPath < b.relPath; });

  std::error_code exists;
  if (!indexPath_.empty() &&
      (!entries_.empty() || !refusals_.empty() || fs::exists(indexPath_, exists))) {
    std::error_code ec;
    fs::create_directories(fs::path(indexPath_).parent_path(), ec);
    std::ofstream out(indexPath_, std::ios::trunc);
    if (out) out << indexText();
  }
  return result;
}

std::shared_ptr<const BrushTipBitmap> DabLibrary::resolve(const std::string& id) {
  for (DabEntry& e : entries_) {
    if (e.id != id) continue;
    if (e.bitmap != nullptr) return e.bitmap;
    // Carried from the index, never opened. Decode now and keep it -- which
    // is why `entries()` can be a 500-row list on a folder that has cost one
    // stat per file and not one decode.
    std::vector<DabEntry> decoded;
    std::string note;
    const std::string& root = (e.root == DabRoot::User) ? userRoot_ : importedRoot_;
    if (!decodeFile(root, e.root, e.relPath, decoded, note)) return nullptr;
    for (DabEntry& d : decoded)
      if (d.id == id || (decoded.size() == 1 && d.frame == e.frame)) {
        e.bitmap = d.bitmap;
        return e.bitmap;
      }
    return nullptr;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Extraction -- the header's argument for the alpha-over-black encoding.
// ---------------------------------------------------------------------------
std::vector<std::string> extractAbrTips(
    const std::string& importedRoot,
    const std::vector<std::pair<std::string, BrushTipBitmap>>& tips,
    std::vector<std::string>* notesOut) {
  std::vector<std::string> ids;
  ids.reserve(tips.size());
  if (importedRoot.empty()) return ids;

  bool madeRoot = false;
  for (const auto& [uuid, bitmap] : tips) {
    if (uuid.empty() || bitmap.width <= 0 || bitmap.height <= 0) continue;
    if (bitmap.alpha.size() !=
        static_cast<size_t>(bitmap.width) * static_cast<size_t>(bitmap.height))
      continue;
    // A uuid arrives from a file and lands in a path, so it is checked rather
    // than trusted: anything that is not a plain hex-and-dash id is refused
    // instead of being asked to name a file. `..` and `/` are the reason.
    bool safe = !uuid.empty();
    for (const char c : uuid)
      if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) safe = false;
    if (!safe) {
      if (notesOut) notesOut->push_back("tip id '" + uuid + "' is not a plain uuid -- not written");
      continue;
    }

    const std::string id = "abr:" + uuid;
    const fs::path target = fs::path(importedRoot) / (uuid + ".png");
    std::error_code ec;
    if (fs::exists(target, ec)) {
      // Already extracted, by this import or an earlier one. The uuid names
      // the tip, so the file that is there IS this tip; rewriting it would
      // discard a touch-up the user made in an image editor.
      ids.push_back(id);
      continue;
    }

    // **Alpha over black** -- the header's §"Extraction" argument. Correct in
    // both branches of §4's rule, where a greyscale PNG would round-trip
    // inverted and a white-RGB one would make an opaque tip come out empty.
    std::vector<uint8_t> rgba(bitmap.alpha.size() * 4, 0);
    for (size_t i = 0; i < bitmap.alpha.size(); ++i) rgba[i * 4 + 3] = bitmap.alpha[i];
    const std::vector<uint8_t> png =
        encodePng8Rgba(static_cast<uint32_t>(bitmap.width),
                       static_cast<uint32_t>(bitmap.height), rgba.data());
    if (png.empty()) {
      if (notesOut) notesOut->push_back("tip " + uuid + " could not be encoded as a PNG");
      continue;
    }

    // The root is created HERE and not in `rescan()`: writing a tip is a
    // deliberate act with something to put in the folder, where a scan is
    // not (§"rescan"). Created once per call, only if there is a tip to write.
    if (!madeRoot) {
      fs::create_directories(importedRoot, ec);
      madeRoot = true;
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (notesOut) notesOut->push_back("tip " + uuid + ": " + target.string() + " is not writable");
      continue;
    }
    out.write(reinterpret_cast<const char*>(png.data()),
              static_cast<std::streamsize>(png.size()));
    if (!out) {
      if (notesOut) notesOut->push_back("tip " + uuid + ": write failed part way through");
      continue;
    }
    ids.push_back(id);
  }
  return ids;
}

size_t resolveDabIds(BrushLibrary& lib, DabLibrary& dabs, std::vector<std::string>* notesOut) {
  size_t resolved = 0;
  for (BrushPreset& p : lib.presets) {
    if (p.dabId.empty()) continue;
    // A preset that already HAS its bitmap is left alone: it came straight
    // out of a loaded `.abr` this session, and re-resolving would swap a tip
    // the library just decoded for a copy read back off disk. Same picture,
    // two allocations, and one more way for them to disagree.
    if (p.tipBitmap != nullptr) { ++resolved; continue; }
    if (auto bitmap = dabs.resolve(p.dabId)) {
      p.tipBitmap = std::move(bitmap);
      ++resolved;
      continue;
    }
    if (notesOut)
      notesOut->push_back("'" + p.name + "': the tip '" + p.dabId +
                          "' is no longer in the dab library -- it will paint with the round "
                          "procedural tip");
  }
  return resolved;
}

// ---------------------------------------------------------------------------
// `--dab-import`
// ---------------------------------------------------------------------------
int runDabImport(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::printf("could not open '%s'\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  const AbrImportResult imported = importAbrBrushes(std::span<const uint8_t>(bytes));
  if (!imported.ok) {
    std::printf("'%s': %s\n", path, imported.error.c_str());
    return 1;
  }

  std::vector<std::pair<std::string, BrushTipBitmap>> tips;
  for (const AbrSampledTip& tip : imported.tipSamples)
    if (tip.bitmap != nullptr) tips.emplace_back(tip.id, *tip.bitmap);

  std::vector<std::string> notes;
  const std::vector<std::string> ids = extractAbrTips(dabImportedRootPath(), tips, &notes);

  std::printf("%s\n", path);
  std::printf("  %zu preset(s), %zu sampled tip(s) decoded\n", imported.presets.size(),
              tips.size());
  std::printf("  %zu tip(s) now in %s\n", ids.size(), dabImportedRootPath().c_str());
  // How many presets can survive a relaunch is the number this flag exists to
  // report -- a pack whose brushes are all procedural writes nothing and that
  // is the correct outcome, not a failure.
  size_t withDab = 0;
  for (const BrushPreset& p : imported.presets)
    if (!p.dabId.empty()) ++withDab;
  std::printf("  %zu of %zu preset(s) now carry a `dab` id that outlives this pack\n", withDab,
              imported.presets.size());
  for (const std::string& note : notes) std::printf("  ! %s\n", note.c_str());
  return 0;
}

// ---------------------------------------------------------------------------
// `--dab-scan`
// ---------------------------------------------------------------------------
int runDabScan() {
  DabLibrary lib;
  lib.setRoots(dabUserRootPath(), dabImportedRootPath(), dabIndexPath());
  const DabScanResult r = lib.rescan();

  std::printf("dab library\n");
  std::printf("  user root      %s%s\n", dabUserRootPath().c_str(),
              fs::is_directory(dabUserRootPath()) ? "" : "   (does not exist yet)");
  std::printf("  imported root  %s%s\n", dabImportedRootPath().c_str(),
              fs::is_directory(dabImportedRootPath()) ? "" : "   (does not exist yet)");
  std::printf("  index          %s\n\n", dabIndexPath().c_str());

  std::printf("  %d added, %d carried unchanged, %d renamed, %d gone, %d refused\n",
              r.added, r.unchanged, r.repaired, r.removed, r.rejected);
  // The number the header's §2 claim rests on. Printed rather than described:
  // a scan that re-decoded everything would look identical in every other
  // line of this report.
  std::printf("  %zu file(s) decoded this scan\n\n", lib.decodeCount());

  if (!r.notes.empty()) {
    // A folder is a user interface (§"rejected"): a file that silently does
    // not appear is indistinguishable from one nothing noticed.
    std::printf("  refused:\n");
    for (const std::string& note : r.notes) std::printf("    %s\n", note.c_str());
    std::printf("\n");
  }

  if (lib.entries().empty()) {
    std::printf("  no dabs. Drop a PNG, a .gbr or a .gih into the user root above,\n");
    std::printf("  or import a Photoshop .abr to fill the imported one.\n");
    return 0;
  }

  std::printf("  %-40s %-9s %-8s %s\n", "id", "size", "root", "name");
  for (const DabEntry& e : lib.entries()) {
    char size[32];
    std::snprintf(size, sizeof(size), "%dx%d", e.width, e.height);
    std::printf("  %-40s %-9s %-8s %s%s\n", e.id.c_str(), size,
                e.root == DabRoot::User ? "user" : "imported", e.name.c_str(),
                e.haveSpacing ? "   (file suggests a spacing)" : "");
  }
  return 0;
}

}  // namespace np
