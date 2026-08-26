#include "app/selftest/Support.hpp"

#include "io/Capabilities.hpp"
#include "ui/FileDialog.hpp"

#include <SDL3/SDL_dialog.h>

#include <thread>

// SDL's own filter validator, which `SDL_ShowFileDialogWithProperties()` runs
// before it will show anything.
//
// **Declared here rather than reimplemented, and that is the point of section
// B below.** SDL's rule is "`[a-zA-Z0-9_.-]`, `;`-separated, no empty piece,
// or a lone `*`". Writing that rule out again and testing our filters against
// the copy would prove that the copy agrees with itself. This is the function
// the real call runs; it is a plain external symbol in the vendored archive
// (`nm -g libSDL3.a` shows `T _validate_filters`), even though it is not in a
// public header -- which is why it is declared by hand.
//
// Returns null when the filters are acceptable, and the reason otherwise.
extern "C" const char* validate_filters(const SDL_DialogFileFilter* filters, int nfilters);

namespace np {

// ---------------------------------------------------------------------------
// The OS file panel (ui/FileDialog.hpp).
//
// --selftest cannot put a panel up: that needs a window and a person. What it
// can do is assert the parts that fail *silently* -- a filter pattern SDL
// refuses (the panel simply never appears, and File > Open looks like it does
// nothing), a `*` that turns filtering off for the whole panel, a filter list
// that has drifted from what this build can actually read, and a mailbox that
// strands a request so every later panel is refused.
// ---------------------------------------------------------------------------
bool runFileDialogTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ---- A. every purpose has a plan, and it says the right thing ----------
  //
  // `fileDialogPlanFor()` has no `default:`, so a purpose added without a
  // plan is a build failure. What that cannot catch is a plan that is present
  // but empty, or a save panel declared as an open one -- which would put an
  // `NSOpenPanel` in front of someone trying to save, and an `NSOpenPanel`
  // cannot name a file that does not exist yet.
  {
    constexpr FileDialogPurpose kAll[] = {
        FileDialogPurpose::OpenDocument, FileDialogPurpose::ImportImage,
        FileDialogPurpose::ImportBrushes, FileDialogPurpose::SaveDocument,
        FileDialogPurpose::SaveCopy,     FileDialogPurpose::ExportImage,
    };
    static_assert(sizeof(kAll) / sizeof(kAll[0]) == kFileDialogPurposeCount,
                  "a purpose was added without being listed here");
    bool allLabelled = true;
    for (const FileDialogPurpose p : kAll) {
      const FileDialogPlan plan = fileDialogPlanFor(p);
      if (plan.title == nullptr || *plan.title == '\0') allLabelled = false;
      if (plan.accept == nullptr || *plan.accept == '\0') allLabelled = false;
    }
    check(allLabelled, "file panel: every purpose has a title and an accept label");

    check(!fileDialogPlanFor(FileDialogPurpose::OpenDocument).save &&
              !fileDialogPlanFor(FileDialogPurpose::ImportImage).save &&
              !fileDialogPlanFor(FileDialogPurpose::ImportBrushes).save,
          "file panel: open and import ask for an open panel");
    check(fileDialogPlanFor(FileDialogPurpose::SaveDocument).save &&
              fileDialogPlanFor(FileDialogPurpose::SaveCopy).save &&
              fileDialogPlanFor(FileDialogPurpose::ExportImage).save,
          "file panel: save, save-a-copy and export ask for a save panel");
    // The two save purposes exist only so that the panel can say which one it
    // is; if their words were the same they would not need to be two.
    check(std::string(fileDialogPlanFor(FileDialogPurpose::SaveDocument).title) !=
              std::string(fileDialogPlanFor(FileDialogPurpose::SaveCopy).title),
          "file panel: Save As and Save a Copy do not say the same thing");
  }

  // ---- B. every pattern this build ships is one SDL will accept ----------
  //
  // The failure this catches has no symptom of its own: SDL calls the
  // callback with `NULL` and never shows a panel, so the menu item appears to
  // do nothing at all.
  {
    constexpr FileDialogFilterSet kSets[] = {
        FileDialogFilterSet::Openable,
        FileDialogFilterSet::Images,
        FileDialogFilterSet::Brushes,
        FileDialogFilterSet::Documents,
    };
    static_assert(sizeof(kSets) / sizeof(kSets[0]) == kFileDialogFilterSetCount,
                  "a filter set was added without being listed here");

    bool allValid = true;
    bool allNonEmpty = true;
    bool noWildcard = true;
    bool noDots = true;
    for (const FileDialogFilterSet set : kSets) {
      const std::vector<FileDialogFilterRow>& rows = fileDialogFilters(set);
      if (rows.empty()) allNonEmpty = false;
      std::vector<SDL_DialogFileFilter> sdlRows;
      sdlRows.reserve(rows.size());
      for (const FileDialogFilterRow& r : rows) {
        if (r.name.empty() || r.pattern.empty()) allNonEmpty = false;
        if (r.pattern.find('*') != std::string::npos) noWildcard = false;
        // Not SDL's rule -- SDL allows a dot -- but the cocoa backend takes
        // only the part after the last one, so "*.png" and ".png" would
        // silently become "png" while "a.b.png" would become "png" too. Bare
        // extensions are what this module documents itself as producing.
        if (r.pattern.find('.') != std::string::npos) noDots = false;
        sdlRows.push_back(SDL_DialogFileFilter{r.name.c_str(), r.pattern.c_str()});
      }
      if (!sdlRows.empty() &&
          validate_filters(sdlRows.data(), static_cast<int>(sdlRows.size())) != nullptr)
        allValid = false;
    }
    check(allNonEmpty, "file panel: every filter set has rows, each with a name and a pattern");
    check(allValid, "file panel: SDL's own validate_filters() accepts every shipped pattern");
    check(noWildcard,
          "file panel: no pattern contains '*' (cocoa treats one as 'do not filter')");
    check(noDots, "file panel: patterns are bare extensions, no dots");

    // **The check above has to be able to fail**, or it proves nothing about
    // the patterns -- only that `validate_filters` returns null. Two patterns
    // SDL's rule rejects, run through the same call.
    const SDL_DialogFileFilter kBadChar{"bad", "*.png"};
    const SDL_DialogFileFilter kBadEmpty{"bad", "png;;jpg"};
    check(validate_filters(&kBadChar, 1) != nullptr && validate_filters(&kBadEmpty, 1) != nullptr,
          "file panel: ...and that validator rejects patterns it should");
  }

  // ---- C. the filter lists track this build, not a hard-coded list -------
  //
  // io/Capabilities.hpp's table is computed from what is actually linked, so
  // a build without OpenImageIO cannot read EXR, TIFF, HDR, DPX or PSD. A
  // filter list written out by hand would go on offering them, and the user
  // would pick one and be told the file could not be read -- by a panel that
  // had just told them it could.
  {
    const std::vector<FileDialogFilterRow>& images = fileDialogFilters(FileDialogFilterSet::Images);
    std::string joined;
    for (const FileDialogFilterRow& r : images) joined += r.pattern + ";";

    bool everyReadableOffered = true;
    bool nothingUnreadableOffered = true;
    for (const FormatCapability& cap : allFormatCapabilities()) {
      const char* ext = imageFormatExtension(cap.format);
      if (ext == nullptr || *ext == '\0') continue;  // camera raw has no single one
      // Matched as a whole `;`-delimited piece, not as a substring: "tif" is
      // a substring of nothing here today, but "png" would match "apng" and
      // the day someone adds one this test would stop meaning anything.
      const bool offered = (";" + joined).find(";" + std::string(ext) + ";") != std::string::npos;
      if (cap.canRead && !offered) everyReadableOffered = false;
      if (!cap.canRead && offered) nothingUnreadableOffered = false;
    }
    check(everyReadableOffered, "file panel: every format this build reads is offered on import");
    check(nothingUnreadableOffered, "file panel: no format it cannot read is offered");

    // PNG/JPEG/TGA/BMP are stb-backed in every configuration
    // (io/Capabilities.cpp's kStbCapabilities), so this holds whatever the
    // build options say -- which makes it a check on the wiring rather than
    // on the options.
    check(joined.find("png") != std::string::npos && joined.find("jpg") != std::string::npos,
          "file panel: ...and PNG and JPEG are there in every configuration");

    // Save panels: macOS appends the FIRST allowed type to a bare filename,
    // so the order in the pattern is not cosmetic. A document typed as
    // "study" must become "study.npaint", not "study.exr".
    const std::vector<FileDialogFilterRow>& docs =
        fileDialogFilters(FileDialogFilterSet::Documents);
    check(!docs.empty() && docs[0].pattern.rfind("npaint", 0) == 0,
          "file panel: a saved document's first offered extension is .npaint");

    // Open offers documents first for the same reason it offers pictures at
    // all: app/OpenAnyFile decides from the bytes, and the list is only how
    // the user finds the file.
    const std::vector<FileDialogFilterRow>& openable =
        fileDialogFilters(FileDialogFilterSet::Openable);
    check(openable.size() >= 2 && openable[0].pattern.rfind("npaint", 0) == 0,
          "file panel: Open offers documents and pictures, documents first");

    check(fileDialogFilters(FileDialogFilterSet::Brushes).size() == 1 &&
              fileDialogFilters(FileDialogFilterSet::Brushes)[0].pattern == "abr",
          "file panel: Import Brushes offers .abr and nothing else");
  }

  // ---- D. the mailbox ----------------------------------------------------
  //
  // The one piece of this module that a wrong answer strands rather than
  // annoys: a request that never ends leaves `pending()` true forever, and
  // every panel afterwards -- Open, Save As, Export -- is refused for the
  // rest of the session with no panel anywhere to explain why.
  {
    FileDialogMailbox box;
    check(!box.pending(), "file dialog mailbox: idle to begin with");
    check(box.beginRequest(FileDialogPurpose::SaveDocument),
          "file dialog mailbox: a request on an idle box is accepted");
    check(box.pending() && box.purpose() == FileDialogPurpose::SaveDocument,
          "file dialog mailbox: ...and it remembers which one");
    check(!box.beginRequest(FileDialogPurpose::OpenDocument),
          "file dialog mailbox: a second request while one is in flight is refused");
    check(box.purpose() == FileDialogPurpose::SaveDocument,
          "file dialog mailbox: ...and the refused one did not overwrite the first");
    check(!box.take().has_value(),
          "file dialog mailbox: nothing to take before the callback has run");

    // **Posted from another thread**, because that is what SDL3/SDL_dialog.h
    // says may happen: "the callback may be called from a different thread
    // than the one the function was invoked on". A mailbox that only works
    // when posted from the taking thread would pass every single-threaded
    // test and then deadlock or tear on the one backend that does it.
    FileDialogOutcome chosen;
    chosen.chose = true;
    chosen.path = "/tmp/np-file-dialog-test.npaint";
    std::thread poster([&box, chosen]() { box.post(chosen); });
    poster.join();

    check(box.pending(), "file dialog mailbox: still pending until the outcome is taken");
    const std::optional<FileDialogOutcome> got = box.take();
    check(got.has_value() && got->chose && got->path == chosen.path,
          "file dialog mailbox: the cross-thread outcome arrives intact");
    check(!box.pending(), "file dialog mailbox: taking the outcome ends the request");
    check(!box.take().has_value(), "file dialog mailbox: an outcome is taken exactly once");

    // A callback with no request in flight is dropped rather than kept. Kept,
    // it would be handed to the *next* request -- and for a save panel that
    // means writing a document over a file the user picked for something
    // else.
    box.post(chosen);
    check(!box.take().has_value() && !box.pending(),
          "file dialog mailbox: a callback with no request in flight is dropped");
    check(box.beginRequest(FileDialogPurpose::OpenDocument) && box.purpose() ==
              FileDialogPurpose::OpenDocument,
          "file dialog mailbox: ...and the box is reusable afterwards");
    box.post(FileDialogOutcome{});
    (void)box.take();
  }

  // ---- E. the default directory -----------------------------------------
  //
  // `SDL_PROP_FILE_DIALOG_LOCATION_STRING` reaches `setDirectoryURL:`
  // unchecked, so a path that is not a directory is not a no-op -- it is
  // AppKit's problem, and its behaviour there is not something to rely on.
  {
    check(fileDialogDirectoryOf("").empty(), "file panel: an empty path asks for no directory");
    check(fileDialogDirectoryOf("study.npaint").empty(),
          "file panel: a bare filename asks for no directory");
    check(fileDialogDirectoryOf("/tmp/study.npaint") == "/tmp",
          "file panel: a real path gives its folder");
    check(fileDialogDirectoryOf("/tmp/np-no-such-folder-9d2f/study.npaint").empty(),
          "file panel: a folder that is not there asks for no directory");
  }

  return ok;
}

}  // namespace np
