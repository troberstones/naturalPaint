#include "app/SvgReport.hpp"

#include <cstdio>
#include <map>
#include <string>

#include "color/Space.hpp"
#include "core/Path.hpp"
#include "core/VectorShape.hpp"
#include "io/SvgImport.hpp"

namespace np {

int runSvgReport(const char* path) {
  const SvgImportResult r = importSvgFile(path);
  std::printf("svg-report: %s\n", path);
  if (!r.ok) {
    std::fprintf(stderr, "svg-report: import failed: %s\n", r.error.c_str());
    return 1;
  }

  std::printf("\nviewport %.3f x %.3f px, %zu shape(s)\n\n", static_cast<double>(r.widthPx),
              static_cast<double>(r.heightPx), r.shapes.size());

  std::printf("%-4s %-24s %-8s %-6s %-5s %-40s %s\n", "#", "name", "subpaths", "anchors", "clip",
              "fill", "stroke");
  std::printf("%-4s %-24s %-8s %-6s %-5s %-40s %s\n", "----", "------------------------",
              "--------", "------", "-----", "----------------------------------------",
              "----------------------------------------");

  for (size_t i = 0; i < r.shapes.size(); ++i) {
    const VectorShape& s = r.shapes[i];
    size_t anchors = 0;
    for (const SubPath& sub : s.path.subpaths) anchors += sub.anchors.size();

    char fillBuf[64];
    char strokeBuf[80];
    {
      std::string tmp;
      if (!s.fill.on) {
        std::snprintf(fillBuf, sizeof(fillBuf), "none");
      } else {
        std::snprintf(fillBuf, sizeof(fillBuf), "srgb(%.2f,%.2f,%.2f) a=%.2f",
                      static_cast<double>(srgbEncode(s.fill.rgba[0])),
                      static_cast<double>(srgbEncode(s.fill.rgba[1])),
                      static_cast<double>(srgbEncode(s.fill.rgba[2])),
                      static_cast<double>(s.fill.rgba[3]));
      }
      if (!s.stroke.on) {
        std::snprintf(strokeBuf, sizeof(strokeBuf), "none");
      } else {
        std::snprintf(strokeBuf, sizeof(strokeBuf), "srgb(%.2f,%.2f,%.2f) a=%.2f w=%.2f",
                      static_cast<double>(srgbEncode(s.stroke.rgba[0])),
                      static_cast<double>(srgbEncode(s.stroke.rgba[1])),
                      static_cast<double>(srgbEncode(s.stroke.rgba[2])),
                      static_cast<double>(s.stroke.rgba[3]),
                      static_cast<double>(s.strokeStyle.width));
      }
    }

    std::printf("%-4zu %-24.24s %-8zu %-6zu %-5s %-40s %s\n", i,
                s.name.empty() ? "(unnamed)" : s.name.c_str(), s.path.subpaths.size(), anchors,
                s.clip.has_value() ? "Y" : "n", fillBuf, strokeBuf);
  }

  const PathBounds bounds = vectorShapesBounds(r.shapes);
  if (bounds.valid) {
    std::printf("\noverall bounds (incl. stroke outset): (%.3f, %.3f) - (%.3f, %.3f)\n",
                static_cast<double>(bounds.minX), static_cast<double>(bounds.minY),
                static_cast<double>(bounds.maxX), static_cast<double>(bounds.maxY));
  } else {
    std::printf("\noverall bounds: -- EMPTY --\n");
  }

  std::printf("\n-- refusals (%zu) --\n", r.refusals.size());
  if (r.refusals.empty()) {
    std::printf("  (none)\n");
  } else {
    for (const std::string& ref : r.refusals) std::printf("  %s\n", ref.c_str());
  }

  return 0;
}

}  // namespace np
