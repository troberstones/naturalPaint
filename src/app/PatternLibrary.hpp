#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "brush/Grain.hpp"

// app/PatternLibrary -- the papers a brush's Texture panel can use, as files.
//
// Two folders beside the dab folders: `patterns/`, the user's own and never
// written by the application, and `patterns-imported/`, where a `.abr`'s scanned
// papers are extracted (app/DabLibrary's `extractAbrPatterns()`). An image
// dropped in `patterns/` is a paper; there is no import step.
//
// A scan stats files and reads image headers for their size; it decodes
// nothing. A paper is decoded the first time something asks for it -- a
// thumbnail on screen, or a preset that names it -- and kept until its file
// changes.
//
// Ids: an extracted paper keeps the bare uuid its `.abr` gave it, which is what
// `PatternRef::id` and a saved preset already hold. A user's file is
// `file:<relpath>`, the spelling app/DabLibrary uses for dabs.

namespace np {

struct BrushLibrary;
struct PsTexture;

enum class PatternRoot : uint8_t { User, Imported };

struct PatternEntry {
  std::string id;
  std::string name;
  std::string relPath;
  PatternRoot root = PatternRoot::User;
  int32_t width = 0;  // from the header; 0 until decoded for a format stb cannot read
  int32_t height = 0;
  uint64_t sizeBytes = 0;
  int64_t mtimeNs = 0;
  std::shared_ptr<const PaperField> field;  // decoded on first use
};

std::string patternsUserRootPath();

class PatternLibrary {
 public:
  void setRoots(std::string userRoot, std::string importedRoot);
  const std::string& userRoot() const noexcept { return userRoot_; }

  // Lists both folders again. An unchanged file keeps its decoded paper; a file
  // whose size or mtime moved is decoded again when next asked for. Returns one
  // note per file that is not a paper, naming it.
  std::vector<std::string> rescan();

  const std::vector<PatternEntry>& entries() const noexcept { return entries_; }
  const PatternEntry* find(const std::string& id) const noexcept;
  std::shared_ptr<const PaperField> resolve(const std::string& id);

  // An extracted file is named by its uuid alone; the paper's name lives in the
  // presets that use it. Remembered across rescans and `setRoots()`.
  void noteName(const std::string& id, const std::string& name);

  size_t decodeCount() const noexcept { return decodeCount_; }
  // Moves whenever a thumbnail could have changed, so a picker re-uploads.
  uint64_t generation() const noexcept { return generation_; }

 private:
  std::string userRoot_;
  std::string importedRoot_;
  std::vector<PatternEntry> entries_;
  std::unordered_map<std::string, std::string> names_;
  size_t decodeCount_ = 0;
  uint64_t generation_ = 1;
};

// A decoded image as paper height: luminance, encoded back to an sRGB byte so a
// greyscale file's bytes are its heights, the same bytes an extracted paper has.
PaperField paperFieldFromDecodedImage(uint32_t width, uint32_t height,
                                      const std::vector<float>& linearRgba);

// The paper's top-left square, sampled to `cell` x `cell` opaque grey RGBA. A
// crop and not a squash: a paper's tooth is its scale, and a squashed thumbnail
// would show a different paper.
std::vector<uint8_t> patternThumbnailRgba(const PaperField& field, int cell);

// Puts the paper `id` names on a Texture panel and turns the panel on, since a
// paper was asked for. "" clears the pattern and leaves the switch alone. An id
// that does not resolve leaves the panel as it was and returns false.
bool selectPattern(PsTexture& texture, PatternLibrary& patterns, const std::string& id);

// For every preset that names a pattern: teaches the library its name, and
// gives a saved preset back the paper its file does not carry. Returns how many
// have their paper; one note per pattern neither folder holds.
size_t resolvePatternIds(BrushLibrary& lib, PatternLibrary& patterns,
                         std::vector<std::string>* notesOut);

}  // namespace np
