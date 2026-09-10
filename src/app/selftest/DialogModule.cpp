// --selftest section: ADR-0010, "every modal dialog goes through ui/Dialog".
//
// This section reads SOURCE, not behaviour, and that is the point. The rule
// it guards is one the compiler cannot see: a bare `ImGui::BeginPopupModal()`
// in src/ui compiles, opens, draws, and is a perfectly good dialog by every
// runtime measure this suite has -- except that it has no Escape key, its
// labels trail their controls, its commit button is wherever its author put
// it, and its red is whatever its author typed. That is exactly how the first
// thirty-four dialogs came to be (docs/modal-screenshots/README.md section 4),
// one at a time, each green. So the guard is a scan of the tree the binary was
// built from: `BeginPopupModal(` appears in code (comments stripped) exactly
// once in src/ui, in ui/Dialog.cpp, and the two colour literals the module
// retired never come back.
//
// Two things a green run here does NOT mean, said now so nobody reads it in:
//
//   * It does not mean a dialog that uses beginDialog() uses it well. A
//     dialog can call beginDialog() and then lay out trailing labels by hand.
//     The contact sheet (tools/modal-shots/capture_modals.sh) is where a
//     human sees that; this section only closes the mechanical loophole.
//   * It does not guard `BeginPopup()`. Context menus, the tool flyout and the
//     combo popups are legitimately non-modal popups, and there is no textual
//     test that tells a menu from a dialog written as a menu (S9's Add Guide
//     was the latter). ADR-0010 leaves that one to review.
//
// The scan is over NP_UI_SOURCE_DIR, the src/ui of the checkout that built
// this binary (same idiom as NP_SVG_TEST_DIR: a compile-time source-tree path
// for a developer/CI-time assertion). The section asserts it found a
// plausible number of files and found ui/Dialog.cpp with its one call, so an
// empty or wrong directory reads as a FAIL and not as a tree with no
// violations -- the "test that tests a copy" shape, closed at the source.

#include "app/SelfTest.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef NP_UI_SOURCE_DIR
#error "NP_UI_SOURCE_DIR must be defined by CMake -- see src/CMakeLists.txt"
#endif

namespace np {

namespace {

// Files under src/ui that may call BeginPopupModal() directly, with the
// reason each is allowed to. The table exists so an exception is a diff to
// THIS file with a sentence beside it, not a quiet omission from the scan --
// the same shape as the toolImplemented() exception table. It is empty, and
// the section asserts that it is, so growing it is a visible act.
struct BareModalException {
  const char* file;
  const char* why;
};
const std::vector<BareModalException> kAllowedBareModals = {
    // (none)
};

// Strips // and /* */ comments so a comment that *mentions* BeginPopupModal()
// (ui/MacPaintUI.cpp has a dozen) is not counted as a call. String literals
// are respected so a `//` inside quotes does not truncate the line.
std::string stripComments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  bool inLine = false, inBlock = false, inString = false;
  for (size_t i = 0; i < src.size(); ++i) {
    const char c = src[i];
    const char n = i + 1 < src.size() ? src[i + 1] : '\0';
    if (inLine) {
      if (c == '\n') { inLine = false; out += c; }
      continue;
    }
    if (inBlock) {
      if (c == '*' && n == '/') { inBlock = false; ++i; }
      else if (c == '\n') out += c;
      continue;
    }
    if (inString) {
      out += c;
      if (c == '\\' && n != '\0') { out += n; ++i; }
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '/' && n == '/') { inLine = true; ++i; continue; }
    if (c == '/' && n == '*') { inBlock = true; ++i; continue; }
    if (c == '"') inString = true;
    out += c;
  }
  return out;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
  int n = 0;
  for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
    ++n;
  return n;
}

}  // namespace

bool runDialogModuleTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::filesystem::path uiDir = NP_UI_SOURCE_DIR;
  std::error_code ec;
  check(std::filesystem::is_directory(uiDir, ec),
        "dialog module: NP_UI_SOURCE_DIR is the src/ui this binary was built from");

  struct Hit {
    std::string file;
    int modals;
    int redLiteral;
    int amberLiteral;
  };
  std::vector<Hit> hits;
  int filesScanned = 0;
  bool sawDialogCpp = false;
  int dialogCppModals = 0;
  for (const auto& entry : std::filesystem::directory_iterator(uiDir, ec)) {
    const std::string ext = entry.path().extension().string();
    if (ext != ".cpp" && ext != ".mm") continue;
    std::ifstream in(entry.path(), std::ios::binary);
    if (!in) continue;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string code = stripComments(ss.str());
    ++filesScanned;
    const Hit h{entry.path().filename().string(), countOccurrences(code, "BeginPopupModal("),
                countOccurrences(code, "0.95f, 0.45f, 0.40f"),
                countOccurrences(code, "0.92f, 0.78f, 0.35f")};
    if (h.file == "Dialog.cpp") {
      sawDialogCpp = true;
      dialogCppModals = h.modals;
      continue;
    }
    if (h.modals > 0 || h.redLiteral > 0 || h.amberLiteral > 0) hits.push_back(h);
  }

  // The scan saw a real tree: as of ADR-0010 src/ui holds 24 translation
  // units, and a directory that yields fewer than a dozen is the wrong
  // directory, not a tidier one.
  check(filesScanned >= 12, "dialog module: the scan covered a plausible src/ui (>= 12 files)");
  check(sawDialogCpp && dialogCppModals == 1,
        "dialog module: ui/Dialog.cpp holds exactly one BeginPopupModal() -- the module's own");

  // The rule itself, one line per offender so a FAIL names the file.
  bool clean = true;
  for (const Hit& h : hits) {
    bool allowed = false;
    for (const BareModalException& e : kAllowedBareModals)
      if (h.file == e.file) allowed = true;
    if (h.modals > 0 && !allowed) {
      std::printf("    %s: %d bare BeginPopupModal() call(s) -- write it against ui/Dialog "
                  "(beginDialog/dialogFooter), or add it to kAllowedBareModals with a reason\n",
                  h.file.c_str(), h.modals);
      clean = false;
    }
    if (h.redLiteral > 0 || h.amberLiteral > 0) {
      std::printf("    %s: %d red / %d amber colour literal(s) -- use dialogStatusColor() / "
                  "kError / kWarning (ui/AtelierTheme.hpp)\n",
                  h.file.c_str(), h.redLiteral, h.amberLiteral);
      clean = false;
    }
  }
  check(clean, "dialog module: no bare BeginPopupModal() and no retired colour literal in src/ui");
  check(kAllowedBareModals.empty(), "dialog module: the bare-modal exception table is empty");

  return ok;
}

}  // namespace np
