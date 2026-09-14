#include "app/PatternLibrary.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "app/DabLibrary.hpp"
#include "brush/BrushModel.hpp"
#include "brush/Library.hpp"
#include "color/Space.hpp"
#include "io/ImageDecode.hpp"
#include "io/PsPatterns.hpp"
#include "ops/PointOps.hpp"
#include "stb_image.h"

namespace fs = std::filesystem;

namespace np {
namespace {

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

std::string lowerExtension(const std::string& relPath) {
  std::string ext = fs::path(relPath).extension().string();
  if (!ext.empty() && ext[0] == '.') ext.erase(0, 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext;
}

// What `extractAbrPatterns()` writes: `<uuid>.png`, directly under its root.
bool isExtractedName(const std::string& relPath) {
  if (relPath.find('/') != std::string::npos || lowerExtension(relPath) != "png") return false;
  const std::string stem = fs::path(relPath).stem().string();
  return stem.size() >= 8 && std::all_of(stem.begin(), stem.end(), [](unsigned char c) {
           return std::isalnum(c) != 0 || c == '-';
         });
}

// Formats stb reads a header from without decoding; the others go through
// io/ImageDecode's OpenImageIO fallback and learn their size when decoded.
bool stbReadsHeader(const std::string& ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "tga" || ext == "bmp";
}
bool decodedOnlyWithOiio(const std::string& ext) {
  return ext == "tif" || ext == "tiff" || ext == "psd" || ext == "exr";
}

std::string entryKey(PatternRoot root, const std::string& relPath) {
  return (root == PatternRoot::User ? "u/" : "i/") + relPath;
}

}  // namespace

std::string patternsUserRootPath() { return defaultDabRootPath() + "/patterns"; }

void PatternLibrary::setRoots(std::string userRoot, std::string importedRoot) {
  userRoot_ = std::move(userRoot);
  importedRoot_ = std::move(importedRoot);
  entries_.clear();
  ++generation_;
}

const PatternEntry* PatternLibrary::find(const std::string& id) const noexcept {
  for (const PatternEntry& e : entries_)
    if (e.id == id) return &e;
  return nullptr;
}

std::vector<std::string> PatternLibrary::rescan() {
  std::vector<std::string> notes;
  std::unordered_map<std::string, PatternEntry> previous;
  for (PatternEntry& e : entries_) previous.emplace(entryKey(e.root, e.relPath), std::move(e));
  entries_.clear();
  bool changed = false;

  const std::pair<const std::string*, PatternRoot> roots[] = {
      {&userRoot_, PatternRoot::User},
      {&importedRoot_, PatternRoot::Imported},
  };
  for (const auto& [rootPtr, which] : roots) {
    const std::string& root = *rootPtr;
    std::error_code ec;
    // Neither folder is created by a scan: a missing one simply holds nothing.
    if (root.empty() || !fs::is_directory(root, ec)) continue;

    std::vector<std::string> relPaths;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                             ec),
         end;
         it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec) || ec) continue;
      const fs::path rel = fs::relative(it->path(), root, ec);
      if (ec || rel.empty() || rel.filename().string()[0] == '.') continue;
      relPaths.push_back(rel.generic_string());
    }
    // Sorted so the grid's order is the same on every platform and every run.
    std::sort(relPaths.begin(), relPaths.end());

    for (const std::string& relPath : relPaths) {
      const fs::path full = fs::path(root) / relPath;
      PatternEntry e;
      if (!statFile(full, e.sizeBytes, e.mtimeNs)) continue;

      const std::string key = entryKey(which, relPath);
      if (auto found = previous.find(key); found != previous.end() &&
                                           found->second.sizeBytes == e.sizeBytes &&
                                           found->second.mtimeNs == e.mtimeNs) {
        entries_.push_back(std::move(found->second));
        previous.erase(found);
        continue;
      }

      const std::string ext = lowerExtension(relPath);
      if (which == PatternRoot::Imported && !isExtractedName(relPath)) {
        notes.push_back(relPath + ": not a paper this application extracted");
        continue;
      }
      if (stbReadsHeader(ext)) {
        int w = 0, h = 0, comp = 0;
        if (stbi_info(full.string().c_str(), &w, &h, &comp) == 0) {
          notes.push_back(relPath + ": not an image this build can read");
          continue;
        }
        e.width = w;
        e.height = h;
      } else if (!decodedOnlyWithOiio(ext)) {
        notes.push_back(relPath + ": not an image");
        continue;
      }
      if (e.width > kMaxPatternDimension || e.height > kMaxPatternDimension) {
        notes.push_back(relPath + ": " + std::to_string(e.width) + "x" + std::to_string(e.height) +
                        " exceeds the " + std::to_string(kMaxPatternDimension) + " px limit");
        continue;
      }

      e.relPath = relPath;
      e.root = which;
      if (which == PatternRoot::Imported) {
        e.id = fs::path(relPath).stem().string();
        const auto named = names_.find(e.id);
        e.name = named != names_.end() ? named->second : "Pattern " + e.id.substr(0, 8);
      } else {
        e.id = "file:" + relPath;
        e.name = fs::path(relPath).stem().string();
      }
      entries_.push_back(std::move(e));
      changed = true;
    }
  }
  if (changed || !previous.empty()) ++generation_;
  return notes;
}

std::shared_ptr<const PaperField> PatternLibrary::resolve(const std::string& id) {
  for (PatternEntry& e : entries_) {
    if (e.id != id) continue;
    if (e.field != nullptr) return e.field;

    const fs::path full =
        fs::path(e.root == PatternRoot::User ? userRoot_ : importedRoot_) / e.relPath;
    const std::vector<uint8_t> bytes = readFileBytes(full);
    if (bytes.empty()) return nullptr;
    ++decodeCount_;

    PaperField field;
    int w = 0, h = 0, comp = 0;
    // One channel through stb, so an extracted paper's bytes come back exactly;
    // stb weights an RGB image's channels into that one itself.
    if (unsigned char* px = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                  &w, &h, &comp, 1)) {
      field.width = w;
      field.height = h;
      field.height8.assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h));
      stbi_image_free(px);
    } else {
      const DecodedImage img = decodeImageLinear(bytes.data(), bytes.size());
      if (!img.valid()) return nullptr;
      field = paperFieldFromDecodedImage(img.width, img.height, img.pixels);
    }
    if (field.width <= 0 || field.height <= 0 || field.width > kMaxPatternDimension ||
        field.height > kMaxPatternDimension)
      return nullptr;

    e.width = field.width;
    e.height = field.height;
    e.field = std::make_shared<const PaperField>(std::move(field));
    ++generation_;
    return e.field;
  }
  return nullptr;
}

void PatternLibrary::noteName(const std::string& id, const std::string& name) {
  if (id.empty() || name.empty()) return;
  names_[id] = name;
  for (PatternEntry& e : entries_)
    if (e.id == id && e.root == PatternRoot::Imported) e.name = name;
}

PaperField paperFieldFromDecodedImage(uint32_t width, uint32_t height,
                                      const std::vector<float>& linearRgba) {
  PaperField out;
  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
  if (width == 0 || height == 0 || linearRgba.size() != texels * 4) return out;
  out.width = static_cast<int32_t>(width);
  out.height = static_cast<int32_t>(height);
  out.height8.resize(texels);
  for (size_t i = 0; i < texels; ++i) {
    const float luma =
        computeLuma({linearRgba[i * 4], linearRgba[i * 4 + 1], linearRgba[i * 4 + 2]});
    const float encoded = std::clamp(srgbEncode(luma), 0.0f, 1.0f);
    out.height8[i] = static_cast<uint8_t>(std::lround(encoded * 255.0f));
  }
  return out;
}

std::vector<uint8_t> patternThumbnailRgba(const PaperField& field, int cell) {
  const int side = std::max(1, cell);
  std::vector<uint8_t> out(static_cast<size_t>(side) * static_cast<size_t>(side) * 4, 0);
  if (field.width <= 0 || field.height <= 0 ||
      field.height8.size() != static_cast<size_t>(field.width) * static_cast<size_t>(field.height))
    return out;
  const int crop = std::min(field.width, field.height);
  for (int y = 0; y < side; ++y) {
    const int sy = std::min(crop - 1, y * crop / side);
    for (int x = 0; x < side; ++x) {
      const int sx = std::min(crop - 1, x * crop / side);
      const uint8_t v = field.height8[static_cast<size_t>(sy) * field.width + sx];
      const size_t d = (static_cast<size_t>(y) * side + x) * 4;
      out[d] = out[d + 1] = out[d + 2] = v;
      out[d + 3] = 255;
    }
  }
  return out;
}

bool selectPattern(PsTexture& texture, PatternLibrary& patterns, const std::string& id) {
  if (id.empty()) {
    texture.pattern = PatternRef{};
    return true;
  }
  std::shared_ptr<const PaperField> field = patterns.resolve(id);
  if (field == nullptr) return false;
  const PatternEntry* entry = patterns.find(id);
  texture.pattern.id = id;
  texture.pattern.name = entry != nullptr ? entry->name : id;
  texture.pattern.field = std::move(field);
  texture.enabled = true;
  return true;
}

size_t resolvePatternIds(BrushLibrary& lib, PatternLibrary& patterns,
                         std::vector<std::string>* notesOut) {
  size_t resolved = 0;
  for (BrushPreset& p : lib.presets) {
    PatternRef& ref = p.model.texture.pattern;
    if (ref.id.empty()) continue;
    patterns.noteName(ref.id, ref.name);
    if (ref.field == nullptr) ref.field = patterns.resolve(ref.id);
    if (ref.field != nullptr) {
      ++resolved;
      continue;
    }
    if (notesOut)
      notesOut->push_back("'" + p.name + "': the pattern '" +
                          (ref.name.empty() ? ref.id : ref.name) +
                          "' is in neither pattern folder -- its Texture panel falls back to "
                          "Paper Grain");
  }
  return resolved;
}

}  // namespace np
