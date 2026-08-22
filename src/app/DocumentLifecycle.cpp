#include "app/DocumentLifecycle.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

// app/DocumentLifecycle -- implementation. Every design decision is argued in
// DocumentLifecycle.hpp; this file holds the mechanics and only comments where
// the mechanics themselves are not obvious from the header's contract.

namespace fs = std::filesystem;

namespace np {
namespace {

// The file format's own header line. Present so a human opening the file
// knows what it is and so a future format change has somewhere to announce
// itself; the reader tolerates its absence, because an absent comment cannot
// make a list of paths ambiguous.
constexpr const char* kRecentFileHeader = "# naturalPaint recent documents v1";

std::string trimmed(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
  return std::string(s.substr(b, e - b));
}

bool hasControlCharacter(std::string_view s) {
  for (unsigned char c : s)
    if (c < 0x20 || c == 0x7f) return true;
  return false;
}

// Every file this module writes changes what is on disk at that path, and
// OpenImageIO's ImageCache does not notice (io/TileResidency.hpp's measured
// "changed on disk after open" case). Worse than the stale-serve it documents:
// a residency opened *after* the rewrite stamps the *new* size and mtime, so
// its own staleness check passes while the cache underneath still holds the
// old tiles. Dropping the file's tiles at the moment the file changes is the
// only point at which that is cheap and unconditional.
//
// A no-op in the NP_USE_OIIO=OFF build and whenever no cache has been created,
// so calling it on every write costs nothing in either case.
void dropCachedTilesFor(const std::string& path) { tileCacheInvalidate(path); }

DocumentOpResult failure(std::string error) {
  DocumentOpResult r;
  r.ok = false;
  r.error = std::move(error);
  return r;
}

// Turns io/NpaintFile's save result into ours, carrying its message and its
// warnings verbatim -- this module never rewords a refusal, for the same
// reason io/ExportAs never reworded io/Export's.
DocumentOpResult fromSave(const NpaintSaveResult& save, const std::string& path) {
  DocumentOpResult r;
  r.ok = save.ok;
  r.error = save.error;
  r.warnings = save.warnings;
  r.path = path;
  return r;
}

// --- Incremental naming ---------------------------------------------------

// Splits `stem` into its base and its trailing `_v<digits>` version, if it has
// one. Returns false when the stem carries no version, leaving `*base` = stem.
bool splitVersionedStem(const std::string& stem, std::string* base, uint64_t* version,
                        size_t* digitWidth) {
  size_t end = stem.size();
  size_t d = end;
  while (d > 0 && std::isdigit(static_cast<unsigned char>(stem[d - 1]))) --d;
  const size_t digits = end - d;
  // Needs at least one digit, and the two characters `_v` immediately before
  // them. `_v` is the whole marker: a bare trailing number is not a version
  // (see the header's rules).
  if (digits == 0 || d < 2) return false;
  if (stem[d - 1] != 'v' || stem[d - 2] != '_') return false;
  // A run of digits longer than a uint64 can hold is not a version number,
  // it is a name that happens to be numeric.
  if (digits > 18) return false;
  *base = stem.substr(0, d - 2);
  *version = std::strtoull(stem.c_str() + d, nullptr, 10);
  *digitWidth = digits;
  return true;
}

std::string formatVersionedName(const std::string& base, uint64_t version, size_t digitWidth,
                                const std::string& ext) {
  std::string digits = std::to_string(version);
  // Padding grows but never truncates: a `_v999` series becomes `_v1000`
  // rather than losing a digit.
  while (digits.size() < digitWidth) digits.insert(digits.begin(), '0');
  return base + "_v" + digits + ext;
}

}  // namespace

// --- Identity -------------------------------------------------------------

DocumentId allocateDocumentId() {
  // Atomic because nothing in this codebase promises documents are only ever
  // created on the main thread, and a duplicated id would silently alias two
  // documents in whatever keys off it later.
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

// --- OpenDocument ---------------------------------------------------------

bool OpenDocument::amendEdit(std::string label, EditKind kind) {
  // History decides whether this is amendable at all; if it refuses, nothing
  // else here may happen either, or the revision would move for an edit that
  // no history entry records.
  if (!history.amend(label, document)) return false;

  // The revision still moves. It is what ui/DocumentTexture caches on, and the
  // document really did change -- another batch of tiles arrived. What does
  // NOT happen is a second `unsavedEdits` label: the list is for PRD I11's
  // "name what is unsaved" sentence, and one act should be named once however
  // many instalments it arrives in.
  ++revision;
  if (kind == EditKind::Structural) ++structuralRevision;
  return true;
}

void OpenDocument::recordEdit(std::string label, EditKind kind) {
  ++revision;
  if (kind == EditKind::Structural) ++structuralRevision;
  // History gets the label whether or not the capped `unsavedEdits` list keeps
  // it: the cap exists so a refusal message stays readable (PRD I11), and a
  // history entry is not a message. This is also why the copy happens before
  // the `std::move` below.
  history.record(label, document);
  if (unsavedEdits.size() < kMaxTrackedUnsavedEdits)
    unsavedEdits.push_back(std::move(label));
  else
    ++unsavedEditsDropped;
}

std::string OpenDocument::unsavedWorkSummary() const {
  if (!isDirty()) return {};
  const size_t total = unsavedEdits.size() + unsavedEditsDropped;
  std::string out = std::to_string(total);
  out += total == 1 ? " unsaved change" : " unsaved changes";
  if (!unsavedEdits.empty()) {
    out += " (";
    for (size_t i = 0; i < unsavedEdits.size(); ++i) {
      if (i) out += ", ";
      out += unsavedEdits[i];
    }
    if (unsavedEditsDropped)
      out += ", and " + std::to_string(unsavedEditsDropped) + " more";
    out += ")";
  }
  return out;
}

std::string documentDisplayName(const OpenDocument& doc) {
  if (!doc.path.empty()) {
    const std::string name = fs::path(doc.path).filename().string();
    if (!name.empty()) return name;
    return doc.path;
  }
  if (!doc.title.empty()) return doc.title;
  return "Untitled " + std::to_string(doc.id);
}

std::optional<size_t> activeLayerIndex(const OpenDocument& doc) {
  if (doc.document.layers.empty()) return std::nullopt;
  return std::min(doc.activeLayer, doc.document.layers.size() - 1);
}

const Layer* activeLayerOf(const OpenDocument& doc) {
  const std::optional<size_t> index = activeLayerIndex(doc);
  return index ? &doc.document.layers[*index] : nullptr;
}

Layer* activeLayerOf(OpenDocument& doc) {
  const std::optional<size_t> index = activeLayerIndex(doc);
  return index ? &doc.document.layers[*index] : nullptr;
}

void setActiveLayer(OpenDocument& doc, size_t index) {
  if (doc.document.layers.empty()) return;
  doc.activeLayer = std::min(index, doc.document.layers.size() - 1);
}

OpenDocument makeBlankOpenDocument(int32_t width, int32_t height, WorkingSpace space,
                                   std::string title) {
  OpenDocument doc;
  doc.id = allocateDocumentId();
  doc.document = Document::createBlank(width, height, space);
  doc.title = title.empty() ? "Untitled" : std::move(title);
  // The baseline entry: undo from the first edit lands on the blank document,
  // not on nothing. Costs one empty TileStore share -- createBlank() allocates
  // no tiles (PRD C2).
  doc.history.begin("new document", doc.document);
  return doc;
}

// --- DocumentSession ------------------------------------------------------

OpenDocument* DocumentSession::active() noexcept {
  return activeIndex_ < docs_.size() ? docs_[activeIndex_].get() : nullptr;
}

const OpenDocument* DocumentSession::active() const noexcept {
  return activeIndex_ < docs_.size() ? docs_[activeIndex_].get() : nullptr;
}

void DocumentSession::setActive(size_t index) noexcept {
  if (index < docs_.size()) activeIndex_ = index;
}

OpenDocument* DocumentSession::at(size_t index) noexcept {
  return index < docs_.size() ? docs_[index].get() : nullptr;
}

const OpenDocument* DocumentSession::at(size_t index) const noexcept {
  return index < docs_.size() ? docs_[index].get() : nullptr;
}

OpenDocument* DocumentSession::find(DocumentId id) noexcept {
  for (const std::unique_ptr<OpenDocument>& d : docs_)
    if (d->id == id) return d.get();
  return nullptr;
}

OpenDocument* DocumentSession::add(OpenDocument&& doc) {
  if (doc.id == 0) doc.id = allocateDocumentId();
  docs_.push_back(std::make_unique<OpenDocument>(std::move(doc)));
  activeIndex_ = docs_.size() - 1;
  return docs_.back().get();
}

bool DocumentSession::close(size_t index, bool discardUnsavedChanges, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (index >= docs_.size()) {
    if (errorOut)
      *errorOut = "close refused: there is no open document at index " + std::to_string(index) +
                  " (" + std::to_string(docs_.size()) + " open).";
    return false;
  }
  OpenDocument& doc = *docs_[index];
  if (doc.isDirty() && !discardUnsavedChanges) {
    if (errorOut)
      *errorOut = "close refused: '" + documentDisplayName(doc) + "' has " +
                  doc.unsavedWorkSummary() +
                  " that closing would discard. Save it first, or close it explicitly "
                  "discarding those changes.";
    return false;
  }
  docs_.erase(docs_.begin() + static_cast<std::ptrdiff_t>(index));
  if (docs_.empty())
    activeIndex_ = 0;
  else if (activeIndex_ >= docs_.size())
    activeIndex_ = docs_.size() - 1;
  return true;
}

// --- Open -----------------------------------------------------------------

DocumentOpResult openNpaintDocument(const std::string& path, OpenDocument* out,
                                    RecentDocuments* recent) {
  if (!out) return failure("open refused: no destination record was supplied.");
  if (path.empty()) return failure("open refused: no path was given.");

  const NpaintLoadResult loaded = loadNpaint(path);
  if (!loaded.ok) {
    DocumentOpResult r = failure(loaded.error);
    r.path = path;
    r.warnings = loaded.warnings;
    return r;
  }

  OpenDocument doc;
  doc.id = allocateDocumentId();
  doc.document = loaded.document;
  doc.carry = loaded.carry;
  doc.path = path;
  doc.residencyMode = TileResidencyMode::Eager;
  // Freshly read from the file: clean, with nothing unsaved.
  doc.revision = 0;
  doc.savedRevision = 0;
  // The baseline entry is the file as it was opened, so the oldest undo lands
  // on exactly what is on disk.
  doc.history.begin("opened", doc.document);
  *out = std::move(doc);

  DocumentOpResult r;
  r.ok = true;
  r.path = path;
  r.warnings = loaded.warnings;
  if (recent) {
    std::string addError;
    if (!recent->add(path, &addError) && !addError.empty()) r.warnings.push_back(addError);
  }
  return r;
}

// --- Revert ---------------------------------------------------------------

DocumentOpResult revertDocument(OpenDocument& doc, const RevertOptions& options) {
  if (!doc.hasPath())
    return failure("revert refused: '" + documentDisplayName(doc) +
                   "' has never been saved, so there is no last saved state to revert to. "
                   "Save it first (Save As) if you want a file to revert to later.");

  if (doc.isDirty() && !options.discardUnsavedChanges)
    return failure("revert refused: reverting '" + documentDisplayName(doc) +
                   "' would discard " + doc.unsavedWorkSummary() + " and replace them with '" +
                   doc.path +
                   "' as it is on disk. Confirm the discard explicitly if that is what you "
                   "want; there is no undo for it.");

  // The cache holds this file's tiles from before whatever changed it. Drop
  // them before reading, or a cached residency opened after this revert can
  // be served the previous contents.
  dropCachedTilesFor(doc.path);

  // Into a temporary first: a revert that fails must leave the in-memory
  // document exactly as it was, or a missing file turns a recoverable mistake
  // into data loss.
  const NpaintLoadResult loaded = loadNpaint(doc.path);
  if (!loaded.ok) {
    DocumentOpResult r = failure(loaded.error);
    r.path = doc.path;
    r.warnings = loaded.warnings;
    return r;
  }

  doc.document = loaded.document;
  // The carry is replaced, not merged: "the last saved state" is what the
  // file says, including which foreign parts it holds.
  doc.carry = loaded.carry;
  doc.residencyMode = TileResidencyMode::Eager;
  doc.unsavedEdits.clear();
  doc.unsavedEditsDropped = 0;
  // A new baseline, and the whole prior history released with it. This
  // deliberately makes the revert itself *not* undoable, which is what the
  // refusal above already promises in so many words ("there is no undo for
  // it") -- a shipped, user-facing promise this step does not get to change
  // silently. Snapshots survive: ADR-0005 says exempt "until dismissed", and a
  // revert is not a dismissal. See core/History.hpp on `begin()`.
  doc.history.begin("revert to saved", doc.document);
  // The document changed (back), so anything caching a derived product must
  // re-read -- but there is now nothing unsaved.
  ++doc.revision;
  doc.savedRevision = doc.revision;

  DocumentOpResult r;
  r.ok = true;
  r.path = doc.path;
  r.warnings = loaded.warnings;
  return r;
}

// --- Duplicate ------------------------------------------------------------

OpenDocument duplicateDocument(const OpenDocument& source) {
  OpenDocument copy;
  copy.id = allocateDocumentId();
  // Deep copies: Document holds Layers by value, a Layer holds its TileStore
  // by value, and a TileStore holds its Tiles by value.
  copy.document = source.document;
  // PRD I10: the copy holds the same unrecognised attributes and foreign
  // parts, so "duplicate, then save" is not where a newer build's data is
  // dropped.
  copy.carry = source.carry;
  // **Deliberately not copied.** An inherited path would make the duplicate's
  // first Save overwrite the original.
  copy.path.clear();
  copy.title = documentDisplayName(source) + " copy";
  // A cached residency is a claim on a file; the duplicate has none.
  copy.residencyMode = TileResidencyMode::Eager;
  copy.revision = 0;
  copy.savedRevision = 0;
  // **The source's history is deliberately not copied** -- `copy` is
  // default-constructed and every member is set by name, so `copy.history` is
  // empty here. That is this header's own `DocumentId` rule made real: a
  // duplicate is a different document, and inheriting the original's undo
  // stack would let an undo in the copy reinstate a state the copy never had.
  // `recordEdit()` on an empty history seeds it (core/History::record), so the
  // duplicate's baseline is the duplicated content and there is nothing before
  // it -- which is correct, because there was no earlier state of *this*
  // document.
  copy.recordEdit("duplicate of " + documentDisplayName(source));
  return copy;
}

// --- Layer operations ------------------------------------------------------

DocumentOpResult recordLayerEdit(OpenDocument& doc, LayerOpResult result) {
  DocumentOpResult out;
  out.ok = result.ok;
  if (!result.ok) {
    out.error = std::move(result.error);
    return out;
  }
  // Structural, always -- see the header. The label comes from core/LayerOps
  // rather than being re-invented here, so the History panel (PRD O2) and the
  // revert refusal (PRD I11) name the operation the same way.
  doc.recordEdit(std::move(result.editLabel), EditKind::Structural);
  return out;
}

// --- Save / Save As / Save a Copy ------------------------------------------

DocumentOpResult saveDocument(OpenDocument& doc, const NpaintSaveOptions& options,
                              RecentDocuments* recent) {
  if (!doc.hasPath())
    return failure("save refused: '" + documentDisplayName(doc) +
                   "' has never been saved and is not bound to a file. Use Save As to choose "
                   "one.");
  return saveDocumentAs(doc, doc.path, options, recent);
}

DocumentOpResult saveDocumentAs(OpenDocument& doc, const std::string& path,
                                const NpaintSaveOptions& options, RecentDocuments* recent) {
  if (path.empty()) return failure("save refused: no path was given.");

  const NpaintSaveResult save = saveNpaint(doc.document, path, options, &doc.carry);
  DocumentOpResult r = fromSave(save, path);
  if (!save.ok) return r;

  // The file at `path` is not what the cache thinks it is any more.
  dropCachedTilesFor(path);

  doc.path = path;
  doc.savedRevision = doc.revision;
  doc.unsavedEdits.clear();
  doc.unsavedEditsDropped = 0;
  if (recent) {
    std::string addError;
    if (!recent->add(path, &addError) && !addError.empty()) r.warnings.push_back(addError);
  }
  return r;
}

DocumentOpResult saveDocumentCopy(const OpenDocument& doc, const std::string& path,
                                  const NpaintSaveOptions& options) {
  if (path.empty()) return failure("save a copy refused: no path was given.");
  if (!doc.path.empty() && normalizeDocumentPath(path) == normalizeDocumentPath(doc.path))
    return failure(
        "save a copy refused: '" + path +
        "' is the file this document is already bound to, so this would not be a copy -- it "
        "would be an ordinary save wearing the wrong name. Use Save for that, or choose a "
        "different path.");

  // `doc` is const, so this function cannot rebind the path, cannot clear the
  // dirty state and cannot record an edit -- the whole difference from Save As
  // is enforced by the signature rather than by remembering not to.
  const NpaintSaveResult save = saveNpaint(doc.document, path, options, &doc.carry);
  DocumentOpResult r = fromSave(save, path);
  if (save.ok) dropCachedTilesFor(path);
  return r;
}

// --- Save incremental ------------------------------------------------------

bool nextIncrementalPath(const std::string& currentPath, std::string* outPath,
                         std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (!outPath) return false;
  if (currentPath.empty()) {
    if (errorOut) *errorOut = "save incremental refused: no path was given.";
    return false;
  }

  const fs::path p(currentPath);
  const fs::path dir = p.parent_path();
  const std::string ext = p.extension().string();
  const std::string stem = p.stem().string();
  if (stem.empty()) {
    if (errorOut)
      *errorOut = "save incremental refused: '" + currentPath +
                  "' has no file name to build a version number onto.";
    return false;
  }

  std::string base = stem;
  uint64_t currentVersion = 0;
  size_t digitWidth = 3;  // the scheme's minimum, used when the name has none
  const bool versioned = splitVersionedStem(stem, &base, &currentVersion, &digitWidth);

  // The highest existing sibling wins, so a gap is stepped over rather than
  // filled and an out-of-date file still produces the newest version.
  uint64_t highest = versioned ? currentVersion : 0;
  const fs::path scanDir = dir.empty() ? fs::path(".") : dir;
  std::error_code ec;
  fs::directory_iterator it(scanDir, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "save incremental refused: could not list the directory '" +
                  scanDir.string() + "' to find the newest version (" + ec.message() + ").";
    return false;
  }
  for (const fs::directory_entry& entry : it) {
    const fs::path& q = entry.path();
    if (q.extension().string() != ext) continue;
    std::string siblingBase;
    uint64_t siblingVersion = 0;
    size_t siblingWidth = 0;
    if (!splitVersionedStem(q.stem().string(), &siblingBase, &siblingVersion, &siblingWidth))
      continue;
    if (siblingBase != base) continue;
    highest = std::max(highest, siblingVersion);
  }

  uint64_t candidate = highest + 1;
  fs::path result = scanDir / formatVersionedName(base, candidate, digitWidth, ext);
  // The rule above cannot collide with a *matching* sibling, but it can
  // collide with a differently padded one (`_v04` beside `_v004`) or with a
  // file created since the scan. Never overwrite.
  int guard = 0;
  while (fs::exists(result, ec) && guard++ < 10000) {
    ++candidate;
    result = scanDir / formatVersionedName(base, candidate, digitWidth, ext);
  }

  // Preserve the caller's own spelling of the directory: a relative input
  // gives a relative output, so nothing silently absolutises a path a user
  // typed.
  *outPath = dir.empty() ? result.filename().string() : result.string();
  return true;
}

DocumentOpResult saveDocumentIncremental(OpenDocument& doc, const NpaintSaveOptions& options,
                                         RecentDocuments* recent) {
  if (!doc.hasPath())
    return failure("save incremental refused: '" + documentDisplayName(doc) +
                   "' has never been saved, so there is no version number to increment. Use "
                   "Save As to choose the first file name; the next incremental save then "
                   "builds on it.");

  std::string nextPath;
  std::string nameError;
  if (!nextIncrementalPath(doc.path, &nextPath, &nameError)) return failure(nameError);

  return saveDocumentAs(doc, nextPath, options, recent);
}

// --- Recent documents ------------------------------------------------------

std::string normalizeDocumentPath(const std::string& path) {
  if (path.empty()) return path;
  std::error_code ec;
  fs::path p = fs::absolute(fs::path(path), ec);
  if (ec) p = fs::path(path);
  return p.lexically_normal().string();
}

void RecentDocuments::loadFromString(std::string_view text, std::string_view sourceLabel) {
  entries_.clear();
  problems_.clear();
  error_.clear();

  std::istringstream in{std::string(text)};
  std::string line;
  size_t lineNumber = 0;
  std::vector<std::pair<size_t, std::string>> lines;
  while (std::getline(in, line)) {
    ++lineNumber;
    const std::string value = trimmed(line);
    if (value.empty() || value[0] == '#') continue;
    lines.emplace_back(lineNumber, value);
  }

  // Fed to add() **oldest first**, i.e. back to front, so that add()'s own
  // "most recent goes to the head, and an existing equal entry moves rather
  // than duplicates" rule produces the file's order without a second ordering
  // rule here. Reading front to back and reversing afterwards is the obvious
  // alternative and is wrong: a file containing the same path twice would come
  // out with the older occurrence in front.
  for (size_t i = lines.size(); i-- > 0;) {
    std::string addError;
    if (!add(lines[i].second, &addError))
      problems_.push_back(std::string(sourceLabel) + ":" + std::to_string(lines[i].first) +
                          ": " + addError);
  }
  std::reverse(problems_.begin(), problems_.end());  // report in line order
}

std::string RecentDocuments::serialize() const {
  std::string out = kRecentFileHeader;
  out += "\n";
  for (const RecentDocument& e : entries_) {
    out += e.path;
    out += "\n";
  }
  return out;
}

bool RecentDocuments::loadFromFile(const std::string& path) {
  entries_.clear();
  problems_.clear();
  error_.clear();
  std::error_code ec;
  if (!fs::exists(path, ec)) return true;  // nothing opened yet is not an error
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error_ = "recent documents: could not read '" + path + "'.";
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  loadFromString(buf.str(), path);
  return true;
}

bool RecentDocuments::saveToFile(const std::string& path, std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  const fs::path p(path);
  std::error_code ec;
  if (!p.parent_path().empty()) {
    fs::create_directories(p.parent_path(), ec);
    if (ec && !fs::exists(p.parent_path())) {
      if (errorOut)
        *errorOut = "recent documents: could not create the directory '" +
                    p.parent_path().string() + "' (" + ec.message() + ").";
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errorOut) *errorOut = "recent documents: could not open '" + path + "' for writing.";
    return false;
  }
  const std::string text = serialize();
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) {
    if (errorOut)
      *errorOut = "recent documents: '" + path + "' was opened but not fully written.";
    return false;
  }
  return true;
}

bool RecentDocuments::add(const std::string& path, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  const std::string value = trimmed(path);
  if (value.empty()) {
    if (errorOut) *errorOut = "recent documents: an empty path cannot be recorded.";
    return false;
  }
  if (hasControlCharacter(value)) {
    if (errorOut)
      *errorOut =
          "recent documents: '" + value +
          "' contains a control character, and the recent-documents file is one path per "
          "line -- recording it would corrupt the list. The document itself is unaffected; "
          "only its place in Open Recent is.";
    return false;
  }

  const std::string normalized = normalizeDocumentPath(value);
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized) {
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
  }
  RecentDocument entry;
  entry.path = normalized;
  entry.displayName = fs::path(normalized).filename().string();
  if (entry.displayName.empty()) entry.displayName = normalized;
  entries_.insert(entries_.begin(), std::move(entry));
  if (entries_.size() > kCapacity) entries_.resize(kCapacity);
  return true;
}

bool RecentDocuments::remove(const std::string& path) {
  const std::string normalized = normalizeDocumentPath(trimmed(path));
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].path == normalized) {
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

std::string defaultRecentDocumentsPath() {
  // The same shape, the same directory and the same override mechanism as
  // io/ExportAs' defaultExportPresetsPath(), deliberately -- one settings
  // location for this application, not two.
  if (const char* explicitPath = std::getenv("NP_RECENT_DOCUMENTS")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/recent-documents.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/recent-documents.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/recent-documents.txt";
#endif
  return "recent-documents.txt";
}

bool recentDocumentMissing(const std::string& path, std::string* whyOut) {
  if (whyOut) whyOut->clear();
  std::error_code ec;
  const bool exists = fs::exists(path, ec);
  if (ec) {
    // Distinguished from "gone": an unreadable parent directory is a
    // permissions or mount problem, and telling the user the file was deleted
    // would send them looking in the wrong place.
    if (whyOut)
      *whyOut = "'" + path + "' could not be checked (" + ec.message() + ").";
    return true;
  }
  if (exists) {
    std::error_code dirEc;
    if (fs::is_directory(path, dirEc)) {
      if (whyOut) *whyOut = "'" + path + "' is a directory now, not a document.";
      return true;
    }
    return false;
  }
  if (whyOut)
    *whyOut = "'" + path +
              "' is no longer there -- it has been moved, renamed, deleted, or is on a volume "
              "that is not mounted.";
  return true;
}

DocumentOpResult openRecentDocument(RecentDocuments& recent, size_t index, OpenDocument* out) {
  const std::vector<RecentDocument>& entries = recent.entries();
  if (index >= entries.size())
    return failure("open recent refused: there is no entry " + std::to_string(index) +
                   " in the recent-documents list (" + std::to_string(entries.size()) +
                   " entries).");

  const std::string path = entries[index].path;
  std::string why;
  if (recentDocumentMissing(path, &why))
    return failure(
        "open recent refused: " + why +
        " The entry has been kept in the list rather than dropped, so you can see what is "
        "missing; remove it deliberately if the file is gone for good.");

  return openNpaintDocument(path, out, &recent);
}

}  // namespace np
