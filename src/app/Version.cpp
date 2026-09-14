#include "app/Version.hpp"

#include "app/VersionInfo.hpp"  // generated: kVersionNumber, kVersionGitHash

namespace np {

std::string versionString() {
  return std::string("naturalPaint ") + kVersionNumber + " (" + kVersionGitHash + ")";
}

}  // namespace np
