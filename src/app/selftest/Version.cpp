#include "app/selftest/Support.hpp"

#include "app/Version.hpp"

// `naturalPaint --version` exits before SDL/GPU/window init, so this section
// cannot run the binary and read stdout -- it calls the same production
// function the flag prints (`versionString()`), the free function that makes
// this testable at all. Headless and GPU-free.
namespace np {

bool runVersionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::string v = versionString();
  std::printf("  [version] %s\n", v.c_str());

  // Format: "naturalPaint <semver> (<hex7+>[-dirty]|unknown)".
  const std::string prefix = "naturalPaint ";
  check(v.rfind(prefix, 0) == 0, "versionString() starts with \"naturalPaint \"");

  const size_t openParen = v.find(" (");
  const bool wrapped = openParen != std::string::npos && !v.empty() && v.back() == ')';
  check(wrapped, "versionString() wraps the hash field in \" (\" ... \")\"");

  if (wrapped) {
    auto isDigits = [](const std::string& s) {
      return !s.empty() && std::all_of(s.begin(), s.end(),
                                       [](char c) { return c >= '0' && c <= '9'; });
    };
    const std::string semver = v.substr(prefix.size(), openParen - prefix.size());
    const size_t d1 = semver.find('.');
    const size_t d2 = d1 == std::string::npos ? std::string::npos : semver.find('.', d1 + 1);
    const bool semverShape =
        d1 != std::string::npos && d2 != std::string::npos &&
        semver.find('.', d2 + 1) == std::string::npos &&
        isDigits(semver.substr(0, d1)) && isDigits(semver.substr(d1 + 1, d2 - d1 - 1)) &&
        isDigits(semver.substr(d2 + 1));
    check(semverShape, "the version field is MAJOR.MINOR.PATCH, every part all-digit");

    const std::string hash = v.substr(openParen + 2, v.size() - (openParen + 2) - 1);
    if (hash == "unknown") {
      check(true, "outside a git checkout the hash field reads \"unknown\"");
    } else {
      std::string hex = hash;
      const bool dirty = hex.size() > 6 && hex.substr(hex.size() - 6) == "-dirty";
      if (dirty) hex.resize(hex.size() - 6);
      const bool hexShape =
          hex.size() >= 7 && std::all_of(hex.begin(), hex.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          });
      check(hexShape, "the hash is at least 7 lowercase hex digits, optionally -dirty");
    }
  }

  // This checkout IS a git repository -- tools/scatter/setup-here.sh pins a
  // branch here -- so the "outside a git checkout" fallback must never be
  // what this particular run measured.
  check(v.find("(unknown)") == std::string::npos,
        "this checkout is a git repository, so the hash is never \"unknown\" here");

  std::printf("[selftest] version %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
