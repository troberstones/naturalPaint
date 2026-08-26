#include "ui/FileDialog.hpp"

#include "io/Capabilities.hpp"
#include "io/NpaintFile.hpp"  // kNpaintExtension

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_properties.h>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace np {

FileDialogPlan fileDialogPlanFor(FileDialogPurpose purpose) noexcept {
  // No `default:`. `-Werror=switch` (src/CMakeLists.txt) turns a purpose added
  // without a plan into a build failure rather than a panel that comes up
  // titled "" with an accept button reading "".
  switch (purpose) {
    case FileDialogPurpose::OpenDocument:
      return FileDialogPlan{false, "Open", "Open", FileDialogFilterSet::Openable};
    case FileDialogPurpose::ImportImage:
      return FileDialogPlan{false, "Import Image", "Import", FileDialogFilterSet::Images};
    case FileDialogPurpose::ImportBrushes:
      return FileDialogPlan{false, "Import Brushes", "Import", FileDialogFilterSet::Brushes};
    case FileDialogPurpose::SaveDocument:
      return FileDialogPlan{true, "Save As", "Save", FileDialogFilterSet::Documents};
    case FileDialogPurpose::SaveCopy:
      // "Save a Copy" is the whole point of this being its own case: the
      // document does not become bound to what the user picks here
      // (app/DocumentLifecycle.hpp), and the panel is the last place that can
      // say so before a file is written.
      return FileDialogPlan{true, "Save a Copy", "Save Copy", FileDialogFilterSet::Documents};
    case FileDialogPurpose::ExportImage:
      // The filter set is overridden per request by the Export As panel --
      // see `requestFileDialogWithFilter()`. `Images` is what it falls back
      // to if it is ever called through the plain entry point.
      return FileDialogPlan{true, "Export", "Export", FileDialogFilterSet::Images};
  }
  return FileDialogPlan{};
}

namespace {

// Every extension this build can *read*, in `ImageFormat` declaration order,
// with the ones that have no single extension dropped. Camera raw is the one
// such case and io/Capabilities.hpp argues there why it returns "" rather
// than inventing one.
std::string readableImageExtensions() {
  std::string out;
  for (const FormatCapability& cap : allFormatCapabilities()) {
    if (!cap.canRead) continue;
    const char* ext = imageFormatExtension(cap.format);
    if (ext == nullptr || *ext == '\0') continue;
    if (!out.empty()) out += ';';
    out += ext;
  }
  return out;
}

// `kNpaintExtension` is ".npaint"; SDL's pattern syntax is dotless.
std::string npaintPattern() {
  std::string ext = kNpaintExtension;
  if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
  // `.exr` is the same container under a different name (io/NpaintFile.hpp),
  // and a document saved as one still opens as a document because
  // app/OpenAnyFile decides from the bytes.
  return ext + ";exr";
}

std::vector<FileDialogFilterRow> buildFilters(FileDialogFilterSet set) {
  std::vector<FileDialogFilterRow> rows;
  const std::string images = readableImageExtensions();
  switch (set) {
    case FileDialogFilterSet::Openable:
      rows.push_back({"naturalPaint documents", npaintPattern()});
      if (!images.empty()) rows.push_back({"Images", images});
      break;
    case FileDialogFilterSet::Images:
      if (!images.empty()) rows.push_back({"Images", images});
      break;
    case FileDialogFilterSet::Brushes:
      rows.push_back({"Photoshop brush libraries", "abr"});
      break;
    case FileDialogFilterSet::Documents:
      rows.push_back({"naturalPaint document", npaintPattern()});
      break;
  }
  // A row with an empty pattern is not a harmless no-op: SDL's
  // `validate_filters()` rejects the whole call ("Empty pattern not
  // allowed"), the callback fires with `NULL`, and the panel never appears.
  // The only way to get one here is a build in which nothing is readable,
  // which cannot happen (PNG/JPEG/TGA/BMP are stb-backed in every
  // configuration) -- but "cannot happen" is how the silent no-op gets in.
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [](const FileDialogFilterRow& r) { return r.pattern.empty(); }),
             rows.end());
  return rows;
}

FileDialogMailbox g_mailbox;
SDL_Window* g_parentWindow = nullptr;

// Storage for the in-flight request. Written only on the main thread, and
// only while no request is pending -- which is what `beginRequest()`'s
// refusal enforces (ui/FileDialog.hpp, "One request at a time").
//
// `g_activeStrings` holds each row's name and pattern; `g_activeFilters`
// points into it. It is reserved to its final size before anything is pushed,
// so no reallocation can move a string the panel is still reading.
std::vector<std::string> g_activeStrings;
std::vector<SDL_DialogFileFilter> g_activeFilters;
// Kept alive rather than destroyed at the end of the call. SDL's header
// states the lifetime requirement for the *filters array* only and says
// nothing about the property set; the cocoa backend reads every property
// synchronously before its sheet begins, so destroying it immediately would
// in fact be safe here. Holding it until the next request costs one
// allocation and removes the question.
SDL_PropertiesID g_activeProps = 0;

void SDLCALL onDialogFinished(void* userdata, const char* const* filelist, int /*filterIndex*/) {
  // **This may not be the main thread.** SDL3/SDL_dialog.h says so outright.
  // Nothing here touches AppState, ImGui, or any of this module's request
  // storage -- it fills a value and posts it under the mailbox's mutex.
  FileDialogOutcome outcome;
  if (filelist == nullptr) {
    const char* err = SDL_GetError();
    outcome.error = (err != nullptr && *err != '\0')
                        ? err
                        : "The file panel could not be shown, and the platform gave no reason.";
  } else if (filelist[0] == nullptr) {
    outcome.cancelled = true;
  } else {
    outcome.chose = true;
    outcome.path = filelist[0];
  }
  static_cast<FileDialogMailbox*>(userdata)->post(std::move(outcome));
}

bool showDialog(FileDialogPurpose purpose, const std::string& defaultDirectory,
                const std::vector<FileDialogFilterRow>& rows) {
  if (!g_mailbox.beginRequest(purpose)) return false;

  if (g_activeProps != 0) {
    SDL_DestroyProperties(g_activeProps);
    g_activeProps = 0;
  }

  g_activeStrings.clear();
  g_activeStrings.reserve(rows.size() * 2);
  for (const FileDialogFilterRow& row : rows) {
    g_activeStrings.push_back(row.name);
    g_activeStrings.push_back(row.pattern);
  }
  g_activeFilters.clear();
  g_activeFilters.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i)
    g_activeFilters.push_back(
        SDL_DialogFileFilter{g_activeStrings[i * 2].c_str(), g_activeStrings[i * 2 + 1].c_str()});

  const FileDialogPlan plan = fileDialogPlanFor(purpose);

  g_activeProps = SDL_CreateProperties();
  if (g_activeProps == 0) {
    // The request has already begun, so it has to end with an outcome rather
    // than a `false` return -- otherwise the mailbox stays pending forever
    // and every later Open is refused for the rest of the session.
    FileDialogOutcome failed;
    const char* err = SDL_GetError();
    failed.error = (err != nullptr && *err != '\0') ? err : "Could not prepare the file panel.";
    g_mailbox.post(std::move(failed));
    return true;
  }

  if (!g_activeFilters.empty()) {
    SDL_SetPointerProperty(g_activeProps, SDL_PROP_FILE_DIALOG_FILTERS_POINTER,
                           g_activeFilters.data());
    SDL_SetNumberProperty(g_activeProps, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER,
                          static_cast<Sint64>(g_activeFilters.size()));
  }
  if (g_parentWindow != nullptr)
    SDL_SetPointerProperty(g_activeProps, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_parentWindow);
  if (!defaultDirectory.empty())
    SDL_SetStringProperty(g_activeProps, SDL_PROP_FILE_DIALOG_LOCATION_STRING,
                          defaultDirectory.c_str());
  SDL_SetStringProperty(g_activeProps, SDL_PROP_FILE_DIALOG_TITLE_STRING, plan.title);
  SDL_SetStringProperty(g_activeProps, SDL_PROP_FILE_DIALOG_ACCEPT_STRING, plan.accept);
  SDL_SetBooleanProperty(g_activeProps, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);

  SDL_ShowFileDialogWithProperties(
      plan.save ? SDL_FILEDIALOG_SAVEFILE : SDL_FILEDIALOG_OPENFILE, onDialogFinished, &g_mailbox,
      g_activeProps);
  return true;
}

}  // namespace

const std::vector<FileDialogFilterRow>& fileDialogFilters(FileDialogFilterSet set) {
  // One cache per set, built on first use. The capability table behind it
  // probes OpenImageIO (io/Capabilities.hpp: "computed once, on first call
  // ... never at startup"), so this deliberately does not run until someone
  // asks for a panel.
  static const std::vector<FileDialogFilterRow> kOpenable = buildFilters(FileDialogFilterSet::Openable);
  static const std::vector<FileDialogFilterRow> kImages = buildFilters(FileDialogFilterSet::Images);
  static const std::vector<FileDialogFilterRow> kBrushes = buildFilters(FileDialogFilterSet::Brushes);
  static const std::vector<FileDialogFilterRow> kDocuments =
      buildFilters(FileDialogFilterSet::Documents);
  switch (set) {
    case FileDialogFilterSet::Openable: return kOpenable;
    case FileDialogFilterSet::Images: return kImages;
    case FileDialogFilterSet::Brushes: return kBrushes;
    case FileDialogFilterSet::Documents: return kDocuments;
  }
  return kOpenable;
}

bool FileDialogMailbox::beginRequest(FileDialogPurpose purpose) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_) return false;
  pending_ = true;
  purpose_ = purpose;
  outcome_.reset();
  return true;
}

void FileDialogMailbox::post(FileDialogOutcome outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  // A callback with no request in flight is dropped rather than stored. The
  // alternative -- keeping it -- would hand the *next* request someone else's
  // answer, which for a save panel means writing a document over a file the
  // user picked for something else.
  if (!pending_) return;
  outcome_ = std::move(outcome);
}

std::optional<FileDialogOutcome> FileDialogMailbox::take() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!outcome_) return std::nullopt;
  FileDialogOutcome taken = std::move(*outcome_);
  outcome_.reset();
  pending_ = false;
  return taken;
}

bool FileDialogMailbox::pending() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_;
}

FileDialogPurpose FileDialogMailbox::purpose() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return purpose_;
}

void setFileDialogParentWindow(SDL_Window* window) { g_parentWindow = window; }

bool requestFileDialog(FileDialogPurpose purpose, const std::string& defaultDirectory) {
  return showDialog(purpose, defaultDirectory, fileDialogFilters(fileDialogPlanFor(purpose).filters));
}

bool requestFileDialogWithFilter(FileDialogPurpose purpose, const std::string& defaultDirectory,
                                 const FileDialogFilterRow& row) {
  if (row.pattern.empty())
    return showDialog(purpose, defaultDirectory,
                      fileDialogFilters(fileDialogPlanFor(purpose).filters));
  return showDialog(purpose, defaultDirectory, std::vector<FileDialogFilterRow>{row});
}

bool fileDialogPending() { return g_mailbox.pending(); }

FileDialogPurpose fileDialogPurpose() { return g_mailbox.purpose(); }

std::optional<FileDialogOutcome> takeFileDialogOutcome() { return g_mailbox.take(); }

std::string fileDialogDirectoryOf(const std::string& path) {
  if (path.empty()) return {};
  std::error_code ec;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (parent.empty()) return {};
  // A directory that does not exist is worse than none: the cocoa backend
  // hands it to `setDirectoryURL:` unchecked, and AppKit's behaviour for a
  // missing directory is not something to rely on. Falling back to empty
  // means "let the OS choose", which is the right answer for a document
  // whose folder has been deleted since it was opened.
  if (!std::filesystem::is_directory(parent, ec)) return {};
  return parent.string();
}

}  // namespace np
