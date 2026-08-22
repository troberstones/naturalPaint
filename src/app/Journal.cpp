#include "app/Journal.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include "io/NpaintFile.hpp"
#include "io/TileResidency.hpp"

// app/Journal -- implementation. Every design decision is argued in
// Journal.hpp; this file holds the mechanics and comments only where the
// mechanics themselves carry a decision.

namespace fs = std::filesystem;

namespace np {
namespace {

constexpr const char* kSessionFileName = "session.txt";
constexpr const char* kLockFileName = "session.lock";
constexpr const char* kSessionHeader = "# naturalPaint recovery session v1";
constexpr const char* kEntryHeader = "# naturalPaint journal entry v1";
// Both readers require this as the last line. A file that stops before it was
// truncated, which is the one thing a crash during a write can produce that
// looks like valid content.
constexpr const char* kTerminator = "end";

// --- Escaping --------------------------------------------------------------
//
// The sidecar is one `key value` per line, so a value containing a newline
// would corrupt it. app/DocumentLifecycle's recent list solves the same
// problem by **refusing** such a path, and could afford to: the cost of that
// refusal is one missing menu entry. Here the cost of refusing would be not
// journalling the document at all, which is data loss in the module whose
// entire job is to prevent it. So values are escaped instead, reversibly.
std::string escapeValue(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

std::string unescapeValue(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out += s[i];
      continue;
    }
    ++i;
    switch (s[i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case '\\': out += '\\'; break;
      // An unknown escape is kept verbatim rather than dropped: this reader's
      // job after a crash is to lose nothing it does not have to.
      default: out += '\\'; out += s[i]; break;
    }
  }
  return out;
}

// --- Integrity -------------------------------------------------------------
//
// FNV-1a 64. Not a cryptographic hash and not trying to be: the threat here is
// a half-written file and a bad sector, not an adversary. It is a dozen lines
// with no dependency, it runs at memory bandwidth, and the alternative (a
// dependency, or OpenSSL) would cost more than the problem is worth.
constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

uint64_t fnv1a64(const uint8_t* data, size_t size, uint64_t seed) {
  uint64_t h = seed;
  for (size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= kFnvPrime;
  }
  return h;
}

bool hashFile(const std::string& path, uint64_t* sizeOut, uint64_t* hashOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::vector<char> buffer(1u << 20);
  uint64_t total = 0;
  uint64_t h = kFnvOffsetBasis;
  while (in) {
    in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize got = in.gcount();
    if (got <= 0) break;
    total += static_cast<uint64_t>(got);
    h = fnv1a64(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(got), h);
  }
  *sizeOut = total;
  *hashOut = h;
  return true;
}

// --- Durability ------------------------------------------------------------
//
// `fsync()`, deliberately not `fcntl(F_FULLFSYNC)`.
//
// On macOS `fsync()` pushes the file's data out of the kernel's buffers to the
// device; `F_FULLFSYNC` additionally asks the device to flush its own volatile
// write cache. So `fsync()` covers a process crash, a kill, and a kernel
// panic -- every failure mode this application can actually produce -- while
// `F_FULLFSYNC` additionally covers sudden power loss. `--selftest` measures
// both on this machine and prints them; the gap is what pays for the choice.
// Measured on a 1 MiB write, three runs: `fsync` 0.16-0.94 ms against
// `F_FULLFSYNC` 7.4-21.4 ms -- between 8x and 112x, and always at least an
// order of magnitude. A journal write already stalls the caller, and paying
// that multiple on every one of them to cover the single failure mode where
// the machine also loses whatever was in flight everywhere else is a bad
// trade. A future "safest" preference is where F_FULLFSYNC belongs.
void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// A rename is only durable once the *directory* entry is. Cheap: a directory
// is a few blocks.
void syncDirectory(const std::string& path) { syncPath(path); }

bool writeFileAtomically(const std::string& path, const std::string& contents,
                         std::string* errorOut) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errorOut) *errorOut = "journal: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut) *errorOut = "journal: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "journal: could not rename '" + temp + "' into place (" + ec.message() + ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

std::string formatLocalTime(std::time_t t) {
  std::tm local{};
  ::localtime_r(&t, &local);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
  return buf;
}

std::string formatDirectoryStamp(std::time_t t) {
  std::tm local{};
  ::localtime_r(&t, &local);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &local);
  return buf;
}

std::string slotName(uint32_t slot, const char* extension) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "doc-%04u%s", slot, extension);
  return buf;
}

// --- The one-key-per-line reader both files share --------------------------
//
// Returns false when the header line is missing or the terminator never
// arrives -- the two ways a crash can leave a file that parses but is not
// complete. Keys are collected in order; repeated keys are kept, which is what
// makes `edit` a list without a second syntax.
bool readKeyedFile(const std::string& path, const char* header,
                   std::vector<std::pair<std::string, std::string>>* out,
                   std::string* errorOut) {
  out->clear();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errorOut) *errorOut = "'" + path + "' could not be read.";
    return false;
  }
  std::string line;
  bool sawHeader = false;
  bool sawEnd = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    if (!sawHeader) {
      if (line != header) {
        if (errorOut)
          *errorOut = "'" + path + "' does not start with '" + header +
                      "', so it is not a journal file this build wrote.";
        return false;
      }
      sawHeader = true;
      continue;
    }
    if (line == kTerminator) {
      sawEnd = true;
      break;
    }
    const size_t space = line.find(' ');
    const std::string key = space == std::string::npos ? line : line.substr(0, space);
    const std::string value = space == std::string::npos ? "" : line.substr(space + 1);
    out->emplace_back(key, value);
  }
  if (!sawHeader) {
    if (errorOut) *errorOut = "'" + path + "' is empty.";
    return false;
  }
  if (!sawEnd) {
    if (errorOut)
      *errorOut = "'" + path +
                  "' has no terminating 'end' line, so it was truncated -- the process writing "
                  "it stopped partway. It is refused rather than half-loaded.";
    return false;
  }
  return true;
}

std::string valueFor(const std::vector<std::pair<std::string, std::string>>& kv,
                     const char* key) {
  for (const auto& [k, v] : kv)
    if (k == key) return v;
  return {};
}

uint64_t uintFor(const std::vector<std::pair<std::string, std::string>>& kv, const char* key) {
  const std::string v = valueFor(kv, key);
  return v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10);
}

}  // namespace

// --- Availability ----------------------------------------------------------

bool journalAvailable() { return true; }

std::string journalUnavailableReason() {
  if (journalAvailable()) return {};
  // io/NpaintFile's own words, obtained by asking it rather than by copying
  // them -- app/DocumentLifecycle's rule about never inventing a second
  // vocabulary for a refusal that already exists. The probe passes a valid
  // 1x1 document, so it gets past saveNpaint()'s request checks and reaches
  // the backend gate, which is the branch we want; the gate returns before
  // any file is opened, so the path named here is never created.
  const NpaintSaveResult probe =
      saveNpaint(Document::createBlank(1, 1, WorkingSpace{}), "journal-availability-probe.npaint");
  return "the recovery journal is disabled in this build. " + probe.error;
}

std::string defaultJournalRootPath() {
  if (const char* explicitPath = std::getenv("NP_JOURNAL_DIR")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home) return std::string(home) + "/Library/Application Support/naturalPaint/recovery";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/recovery";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/recovery";
#endif
  return "naturalPaint-recovery";
}

// --- JournalSession --------------------------------------------------------

JournalSession::~JournalSession() {
  // Releases the lock and leaves the directory. See finishClean()'s comment
  // in the header: reaching here without it means the session did not end
  // normally, which is exactly when the work must survive.
  if (lockFd_ >= 0) {
    ::flock(lockFd_, LOCK_UN);
    ::close(lockFd_);
    lockFd_ = -1;
  }
}

std::string JournalSession::modelPathForSlot(uint32_t slot) const {
  return directory_ + "/" + slotName(slot, kNpaintExtension);
}

std::string JournalSession::sidecarPathForSlot(uint32_t slot) const {
  return directory_ + "/" + slotName(slot, ".journal");
}

bool JournalSession::begin(const JournalOptions& options, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (active()) {
    if (errorOut) *errorOut = "journal: this session has already begun at '" + directory_ + "'.";
    return false;
  }
  if (!journalAvailable()) {
    // No directory is created. A scratch directory with nothing writable into
    // it would be offered for recovery next launch and hold nothing.
    if (errorOut) *errorOut = journalUnavailableReason();
    return false;
  }

  const std::string root = options.rootDirectory.empty() ? defaultJournalRootPath()
                                                         : options.rootDirectory;
  std::error_code ec;
  fs::create_directories(root, ec);
  if (ec && !fs::exists(root)) {
    if (errorOut)
      *errorOut = "journal: could not create the scratch directory '" + root + "' (" +
                  ec.message() + "). This session has no crash recovery.";
    return false;
  }

  const std::time_t now = std::time(nullptr);
  const long pid = static_cast<long>(::getpid());
  std::string dir = root + "/session-" + formatDirectoryStamp(now) + "-" + std::to_string(pid);
  // Two sessions of the same pid in the same second is not reachable, but a
  // leftover directory of a pid that has been reused is: never write into
  // one that is already there.
  int suffix = 1;
  while (fs::exists(dir, ec) && suffix < 1000) dir = root + "/session-" +
      formatDirectoryStamp(now) + "-" + std::to_string(pid) + "-" + std::to_string(suffix++);
  fs::create_directories(dir, ec);
  if (ec && !fs::exists(dir)) {
    if (errorOut)
      *errorOut = "journal: could not create the session directory '" + dir + "' (" +
                  ec.message() + "). This session has no crash recovery.";
    return false;
  }

  const std::string lockPath = dir + "/" + kLockFileName;
  const int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    if (errorOut)
      *errorOut = "journal: could not create the lock file '" + lockPath + "' (" +
                  std::strerror(errno) + "). This session has no crash recovery.";
    fs::remove_all(dir, ec);
    return false;
  }
  // Non-blocking, and a failure is not fatal: the lock's only job is to keep
  // *this* session's directory out of another instance's recovery offer, and
  // a filesystem that cannot lock (see the header on network homes) should
  // cost recovery nothing. The directory being offered to a second instance
  // is harmless, because recovery never writes.
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0 && errorOut)
    *errorOut = "journal: '" + lockPath + "' could not be locked (" + std::strerror(errno) +
                "); this session's own directory may be offered for recovery by another "
                "instance. Journalling itself is unaffected.";

  std::string session = kSessionHeader;
  session += "\nstartedAtEpoch " + std::to_string(static_cast<long long>(now));
  session += "\nstartedAtLocal " + formatLocalTime(now);
  session += "\npid " + std::to_string(pid);
  session += "\n";
  session += kTerminator;
  session += "\n";
  std::string writeError;
  if (!writeFileAtomically(dir + "/" + kSessionFileName, session, &writeError)) {
    if (errorOut) *errorOut = writeError;
    ::flock(fd, LOCK_UN);
    ::close(fd);
    fs::remove_all(dir, ec);
    return false;
  }
  syncDirectory(dir);

  directory_ = dir;
  lockFd_ = fd;
  intervalSeconds_ = options.intervalSeconds;
  nextSlot_ = 1;
  entries_.clear();
  return true;
}

JournalDue journalWriteDue(const OpenDocument& doc, const JournalEntryState& state,
                           double nowSeconds, double intervalSeconds) {
  if (!doc.isDirty()) return JournalDue::No;
  if (!state.everWritten) return JournalDue::Structural;
  if (doc.structuralRevision != state.structuralRevision) return JournalDue::Structural;
  if (state.overdue) return JournalDue::Overdue;
  if (doc.revision != state.revision && (nowSeconds - state.lastWriteSeconds) >= intervalSeconds)
    return JournalDue::Interval;
  return JournalDue::No;
}

bool JournalSession::writeEntry(const OpenDocument& doc, std::string* errorOut,
                                double* secondsOut) {
  if (errorOut) errorOut->clear();
  if (secondsOut) *secondsOut = 0.0;
  if (!active()) {
    if (errorOut) *errorOut = "journal: no session is open, so there is nowhere to write.";
    return false;
  }

  const auto started = std::chrono::steady_clock::now();

  Entry& entry = entries_[doc.id];
  if (entry.slot == 0) entry.slot = nextSlot_++;
  const std::string modelPath = modelPathForSlot(entry.slot);
  const std::string sidecarPath = sidecarPathForSlot(entry.slot);
  // The temp keeps the `.npaint` extension: io/NpaintFile selects the
  // OpenImageIO plugin from the path, so `doc-0001.npaint.tmp` would be
  // handed to a plugin lookup that has never heard of `.tmp`.
  const std::string modelTemp = directory_ + "/" + slotName(entry.slot, ".tmp.npaint");

  // PRD O7: the same writer native save uses. The document's carry goes with
  // it, so PRD I10's unknown attributes and foreign parts are in the journal
  // too -- a recovered document that had dropped them would be a different
  // document.
  const NpaintSaveResult save = saveNpaint(doc.document, modelTemp, {}, &doc.carry);
  std::error_code ec;
  if (!save.ok) {
    if (errorOut) *errorOut = "journal: " + save.error;
    fs::remove(modelTemp, ec);
    return false;
  }

  // Read the model back to size and hash it. Two jobs in one pass: it fills
  // the sidecar's integrity record, and it is the cheapest available check
  // that the bytes are really on disk and readable -- a small down payment on
  // PRD I13, which does not exist yet.
  uint64_t modelBytes = 0, modelHash = 0;
  if (!hashFile(modelTemp, &modelBytes, &modelHash)) {
    if (errorOut)
      *errorOut = "journal: '" + modelTemp + "' was written but could not be read back.";
    fs::remove(modelTemp, ec);
    return false;
  }
  syncPath(modelTemp);
  fs::rename(modelTemp, modelPath, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "journal: could not rename '" + modelTemp + "' into place (" + ec.message() +
                  ").";
    fs::remove(modelTemp, ec);
    return false;
  }

  // The journal path is written only here and read only by
  // recoverDocument()'s loadNpaint(), which goes through ImageInput rather
  // than the ImageCache -- so no stale-tile hazard is reachable today. It is
  // invalidated anyway. io/TileResidency's measured failure (a cache serving
  // a rewritten file's previous tiles while a residency opened afterwards
  // passes its own size+mtime check) is a property of *any* path this process
  // rewrites, and the alternative to this line is a comment asserting that no
  // future caller will ever open a residency on a recovery file -- which is
  // exactly the thing a recovery path for a large document would want to do.
  // The call is a no-op when no cache exists, so being right costs nothing.
  tileCacheInvalidate(modelPath);

  std::string sidecar = kEntryHeader;
  sidecar += "\nid " + std::to_string(doc.id);
  sidecar += "\nslot " + std::to_string(entry.slot);
  sidecar += "\nmodel " + escapeValue(slotName(entry.slot, kNpaintExtension));
  sidecar += "\nmodelBytes " + std::to_string(modelBytes);
  sidecar += "\nmodelHash " + std::to_string(modelHash);
  sidecar += "\npath " + escapeValue(doc.path);
  sidecar += "\ntitle " + escapeValue(doc.title);
  sidecar += "\ndisplayName " + escapeValue(documentDisplayName(doc));
  sidecar += "\nrevision " + std::to_string(doc.revision);
  sidecar += "\nsavedRevision " + std::to_string(doc.savedRevision);
  sidecar += "\nstructuralRevision " + std::to_string(doc.structuralRevision);
  sidecar += "\nresidency " + std::string(tileResidencyModeName(doc.residencyMode));
  sidecar += "\neditsDropped " + std::to_string(doc.unsavedEditsDropped);
  for (const std::string& label : doc.unsavedEdits) sidecar += "\nedit " + escapeValue(label);
  sidecar += "\nunsavedSummary " + escapeValue(doc.unsavedWorkSummary());
  const std::time_t now = std::time(nullptr);
  sidecar += "\nwrittenAtEpoch " + std::to_string(static_cast<long long>(now));
  sidecar += "\nwrittenAtLocal " + formatLocalTime(now);
  sidecar += "\n";
  sidecar += kTerminator;
  sidecar += "\n";

  // The sidecar is written **after** the model, and it is what recovery keys
  // on. So the only state a crash between the two can leave is a model file
  // with a stale sidecar beside it (or none), which recovery refuses on the
  // size/hash check -- never a sidecar promising a model that is not there.
  std::string sidecarError;
  if (!writeFileAtomically(sidecarPath, sidecar, &sidecarError)) {
    if (errorOut) *errorOut = sidecarError;
    return false;
  }
  syncDirectory(directory_);

  entry.state.everWritten = true;
  entry.state.revision = doc.revision;
  entry.state.structuralRevision = doc.structuralRevision;
  entry.state.overdue = false;
  const auto finished = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(finished - started).count();
  // `lastWriteSeconds` is deliberately not touched here: it is measured on
  // the caller's clock, which only tick() has.
  if (secondsOut) *secondsOut = seconds;
  return true;
}

bool JournalSession::dropEntry(DocumentId id, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  const auto it = entries_.find(id);
  if (it == entries_.end()) return true;
  std::error_code ec;
  fs::remove(modelPathForSlot(it->second.slot), ec);
  fs::remove(sidecarPathForSlot(it->second.slot), ec);
  entries_.erase(it);
  return true;
}

JournalTickResult JournalSession::tick(const DocumentSession& documents,
                                       const JournalTickInput& in) {
  JournalTickResult result;
  if (!active()) return result;

  // Entries whose document is no longer open at all: the session closed it.
  // Removed here rather than by close(), so that a document closed by any
  // path -- including one this module has never heard of -- cannot leave a
  // journal behind claiming to be live work.
  std::vector<DocumentId> gone;
  for (const auto& [id, entry] : entries_) {
    bool stillOpen = false;
    for (size_t i = 0; i < documents.count(); ++i)
      if (documents.at(i) && documents.at(i)->id == id) stillOpen = true;
    if (!stillOpen) gone.push_back(id);
  }
  for (DocumentId id : gone) {
    dropEntry(id, nullptr);
    ++result.entriesDropped;
  }

  const OpenDocument* activeDoc = documents.active();
  for (size_t i = 0; i < documents.count(); ++i) {
    const OpenDocument* doc = documents.at(i);
    if (!doc) continue;

    // Clean and bound: the user's own file holds this content, so the journal
    // has no remaining job (ADR-0008). Clean and unbound (a blank document
    // nobody has touched) has nothing to journal in the first place.
    if (!doc->isDirty()) {
      if (entries_.count(doc->id)) {
        dropEntry(doc->id, nullptr);
        ++result.entriesDropped;
      }
      continue;
    }

    const auto it = entries_.find(doc->id);
    const JournalEntryState state = it == entries_.end() ? JournalEntryState{} : it->second.state;
    if (journalWriteDue(*doc, state, in.nowSeconds, intervalSeconds_) == JournalDue::No) continue;

    // PRD O10. Held back, remembered, and written on the first tick after the
    // stroke ends rather than at the next interval.
    if (in.strokeActive) {
      if (it != entries_.end()) it->second.state.overdue = true;
      ++result.deferredByStroke;
      continue;
    }

    std::string error;
    double seconds = 0.0;
    if (writeEntry(*doc, &error, &seconds)) {
      ++result.documentsWritten;
      result.writeSeconds += seconds;
      if (doc == activeDoc) result.activeDocumentWritten = true;
      entries_[doc->id].state.lastWriteSeconds = in.nowSeconds;
    } else {
      result.errors.push_back(error);
      // Not retried on the next tick at full speed: a directory that has
      // filled up would otherwise turn every frame into a failed write. The
      // interval clock is advanced as though the write had happened, and the
      // entry is marked as written-at-this-revision-attempt only through the
      // clock, so the next attempt is one interval away rather than one frame.
      Entry& e = entries_[doc->id];
      e.state.lastWriteSeconds = in.nowSeconds;
      e.state.overdue = false;
      e.state.everWritten = true;
      e.state.structuralRevision = doc->structuralRevision;
    }
  }
  return result;
}

bool JournalSession::finishClean(std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (!active()) return true;
  ::flock(lockFd_, LOCK_UN);
  ::close(lockFd_);
  lockFd_ = -1;
  std::error_code ec;
  fs::remove_all(directory_, ec);
  entries_.clear();
  if (ec) {
    if (errorOut)
      *errorOut = "journal: could not remove the scratch directory '" + directory_ + "' (" +
                  ec.message() + "). It will be offered for recovery next launch.";
    directory_.clear();
    return false;
  }
  directory_.clear();
  return true;
}

// --- Discovery -------------------------------------------------------------

namespace {

// The header's liveness rule, in one place. Returns true **only** when a
// successful probe found the lock held by someone else; every other outcome
// -- no lock file, cannot open it, `flock` unsupported -- returns false, so
// the session is offered.
bool sessionIsLive(const std::string& directory) {
  const std::string lockPath = directory + "/" + kLockFileName;
  std::error_code ec;
  if (!fs::exists(lockPath, ec)) return false;
  const int fd = ::open(lockPath.c_str(), O_RDWR);
  if (fd < 0) return false;
  const bool held = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
  if (!held) ::flock(fd, LOCK_UN);
  ::close(fd);
  return held;
}

RecoveryDocument readRecoveryDocument(const std::string& sidecarPath,
                                      const std::string& directory) {
  RecoveryDocument doc;
  doc.sidecarPath = sidecarPath;
  std::vector<std::pair<std::string, std::string>> kv;
  std::string error;
  if (!readKeyedFile(sidecarPath, kEntryHeader, &kv, &error)) {
    doc.problem = error;
    doc.displayName = fs::path(sidecarPath).stem().string();
    return doc;
  }

  doc.modelPath = directory + "/" + unescapeValue(valueFor(kv, "model"));
  doc.boundPath = unescapeValue(valueFor(kv, "path"));
  doc.displayName = unescapeValue(valueFor(kv, "displayName"));
  if (doc.displayName.empty()) doc.displayName = unescapeValue(valueFor(kv, "title"));
  if (doc.displayName.empty()) doc.displayName = fs::path(sidecarPath).stem().string();
  doc.unsavedSummary = unescapeValue(valueFor(kv, "unsavedSummary"));
  doc.writtenAtLocal = unescapeValue(valueFor(kv, "writtenAtLocal"));
  doc.modelBytes = uintFor(kv, "modelBytes");

  // The integrity check, before anything tries to parse the model as a file
  // format. Size first because it is the cheap answer to the common failure
  // (a truncated write), hash second for the rarer one (a partially rewritten
  // block).
  uint64_t actualBytes = 0, actualHash = 0;
  if (!hashFile(doc.modelPath, &actualBytes, &actualHash)) {
    doc.problem = "the journalled document '" + doc.modelPath +
                  "' named by this entry is missing or unreadable, so there is nothing to "
                  "recover from it.";
    return doc;
  }
  if (actualBytes != doc.modelBytes) {
    doc.problem = "the journalled document '" + doc.modelPath + "' is " +
                  std::to_string(actualBytes) + " bytes, but the journal recorded " +
                  std::to_string(doc.modelBytes) +
                  " -- it was truncated, most likely by the crash that happened during the "
                  "write. It is refused rather than half-loaded.";
    return doc;
  }
  if (actualHash != uintFor(kv, "modelHash")) {
    doc.problem = "the journalled document '" + doc.modelPath +
                  "' is the recorded length but its contents do not match the hash the journal "
                  "recorded, so some of it was not written or has been damaged since. It is "
                  "refused rather than half-loaded.";
    return doc;
  }

  doc.intact = true;
  return doc;
}

}  // namespace

std::vector<RecoverySession> discoverRecoverySessions(const std::string& root) {
  std::vector<RecoverySession> sessions;
  const std::string dir = root.empty() ? defaultJournalRootPath() : root;
  std::error_code ec;
  if (!fs::exists(dir, ec)) return sessions;

  fs::directory_iterator it(dir, ec);
  if (ec) return sessions;
  for (const fs::directory_entry& entry : it) {
    if (!entry.is_directory(ec)) continue;
    const std::string sessionDir = entry.path().string();
    if (sessionIsLive(sessionDir)) continue;

    RecoverySession session;
    session.directory = sessionDir;

    std::vector<std::pair<std::string, std::string>> kv;
    std::string error;
    if (readKeyedFile(sessionDir + "/" + kSessionFileName, kSessionHeader, &kv, &error)) {
      session.startedAtLocal = unescapeValue(valueFor(kv, "startedAtLocal"));
      session.startedAtEpoch = static_cast<int64_t>(uintFor(kv, "startedAtEpoch"));
      session.pid = static_cast<long>(uintFor(kv, "pid"));
    } else {
      // A session whose own descriptor is unreadable is still listed, with
      // the directory name standing in for the date it could not read. The
      // documents inside it may be perfectly good, and this is the module
      // whose whole purpose is not to give up on damaged state.
      session.problems.push_back(error);
      session.startedAtLocal = entry.path().filename().string();
    }

    fs::directory_iterator files(sessionDir, ec);
    if (ec) {
      session.problems.push_back("'" + sessionDir + "' could not be listed (" + ec.message() +
                                 ").");
    } else {
      std::vector<std::string> sidecars;
      for (const fs::directory_entry& f : files)
        if (f.path().extension() == ".journal") sidecars.push_back(f.path().string());
      std::sort(sidecars.begin(), sidecars.end());
      for (const std::string& s : sidecars)
        session.documents.push_back(readRecoveryDocument(s, sessionDir));
    }
    sessions.push_back(std::move(session));
  }

  // Newest first: the session a user is most likely to want is the one they
  // just lost.
  std::sort(sessions.begin(), sessions.end(),
            [](const RecoverySession& a, const RecoverySession& b) {
              if (a.startedAtEpoch != b.startedAtEpoch) return a.startedAtEpoch > b.startedAtEpoch;
              return a.directory > b.directory;
            });
  return sessions;
}

DocumentOpResult recoverDocument(const RecoveryDocument& entry, OpenDocument* out) {
  DocumentOpResult result;
  if (!out) {
    result.error = "recover refused: no destination record was supplied.";
    return result;
  }
  if (!entry.intact) {
    result.error = "recover refused: " +
                   (entry.problem.empty() ? std::string("this journal entry is not usable.")
                                          : entry.problem);
    result.path = entry.modelPath;
    return result;
  }

  // Re-read the sidecar rather than trusting the discovery snapshot: what is
  // being restored is a document's dirty state, and reading it twice from the
  // file is cheaper than deciding how stale the snapshot is allowed to be.
  std::vector<std::pair<std::string, std::string>> kv;
  std::string error;
  if (!readKeyedFile(entry.sidecarPath, kEntryHeader, &kv, &error)) {
    result.error = "recover refused: " + error;
    result.path = entry.sidecarPath;
    return result;
  }

  const NpaintLoadResult loaded = loadNpaint(entry.modelPath);
  if (!loaded.ok) {
    // The scratch directory is untouched on this path, deliberately: a
    // recovery that could destroy the journal it failed to read would be
    // worse than no recovery at all.
    result.error = loaded.error;
    result.path = entry.modelPath;
    result.warnings = loaded.warnings;
    return result;
  }

  OpenDocument doc;
  doc.id = allocateDocumentId();
  doc.document = loaded.document;
  doc.carry = loaded.carry;
  doc.path = unescapeValue(valueFor(kv, "path"));
  doc.title = unescapeValue(valueFor(kv, "title"));
  doc.residencyMode = TileResidencyMode::Eager;
  doc.revision = uintFor(kv, "revision");
  doc.savedRevision = uintFor(kv, "savedRevision");
  doc.structuralRevision = uintFor(kv, "structuralRevision");
  doc.unsavedEditsDropped = static_cast<size_t>(uintFor(kv, "editsDropped"));
  for (const auto& [k, v] : kv)
    if (k == "edit") doc.unsavedEdits.push_back(unescapeValue(v));
  // Only dirty documents are ever journalled, so a restored state that looks
  // clean means the sidecar disagrees with that invariant. Rather than hand
  // back a document the user would be told matches a file it may not match,
  // make the disagreement visible as an edit.
  if (!doc.isDirty()) doc.recordEdit("recovered from the journal");

  *out = std::move(doc);
  result.ok = true;
  result.path = entry.modelPath;
  result.warnings = loaded.warnings;
  return result;
}

bool discardRecoverySession(const RecoverySession& session, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (session.directory.empty()) {
    if (errorOut) *errorOut = "discard refused: no recovery directory was named.";
    return false;
  }
  std::error_code ec;
  fs::remove_all(session.directory, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "discard failed: '" + session.directory + "' could not be removed (" +
                  ec.message() + ").";
    return false;
  }
  return true;
}

}  // namespace np
