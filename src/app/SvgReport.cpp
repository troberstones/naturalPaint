#include "app/SvgReport.hpp"

#include <cstdio>
#include <map>
#include <string>

#include "color/Space.hpp"
#include "core/Path.hpp"
#include "core/TextContent.hpp"
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

  // The `<text>` elements that stayed editable (io/SvgImport.hpp section 7).
  // `below` is the field section 7a exists for: it is what says where in the
  // painting order this block sits, and reading it against the shape table
  // above is how "the label should be UNDER that rectangle" gets checked
  // against the same file open in the exporting application.
  std::printf("\n-- text blocks (%zu) --\n", r.texts.size());
  if (r.texts.empty()) {
    std::printf("  (none)\n");
  } else {
    std::printf("  %-4s %-16s %-6s %-22s %-7s %-20s %s\n", "#", "name", "below", "font", "size",
                "origin (top-left)", "text");
    for (size_t i = 0; i < r.texts.size(); ++i) {
      const SvgTextBlock& t = r.texts[i];
      char origin[48];
      std::snprintf(origin, sizeof(origin), "(%.2f, %.2f)",
                    static_cast<double>(t.content.origin.x),
                    static_cast<double>(t.content.origin.y));
      std::string flags;
      if (t.content.style.bold) flags += " bold";
      if (t.content.style.italic) flags += " italic";
      std::printf("  %-4zu %-16.16s %-6zu %-22.22s %-7.2f %-20s \"%s\"%s\n", i,
                  t.name.empty() ? "(unnamed)" : t.name.c_str(), t.shapesBefore,
                  t.content.style.fontFamily.c_str(),
                  static_cast<double>(t.content.style.sizePx), origin, t.content.utf8.c_str(),
                  flags.c_str());
      // The PAINTED extent, not the layout box -- core/TextContent.hpp is
      // explicit about the difference, and this is the number to compare
      // against a selection rectangle drawn round the same label in the
      // exporting application.
      const PathBounds tb = textContentBounds(t.content);
      if (tb.valid) {
        std::printf("       painted extent (%.3f, %.3f) - (%.3f, %.3f)\n",
                    static_cast<double>(tb.minX), static_cast<double>(tb.minY),
                    static_cast<double>(tb.maxX), static_cast<double>(tb.maxY));
      } else {
        std::printf("       painted extent: -- EMPTY (nothing shaped) --\n");
      }
    }
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
