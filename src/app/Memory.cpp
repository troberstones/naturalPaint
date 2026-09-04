#include "app/Memory.hpp"

#if defined(__APPLE__)

#include <mach/mach.h>
#include <mach/mach_init.h>
#include <mach/task.h>
#include <mach/task_info.h>

namespace np {

size_t currentResidentBytes() {
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  const kern_return_t kr = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                                     reinterpret_cast<task_info_t>(&info), &count);
  if (kr != KERN_SUCCESS) return 0;
  return static_cast<size_t>(info.resident_size);
}

size_t currentFootprintBytes() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  const kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                     reinterpret_cast<task_info_t>(&info), &count);
  if (kr != KERN_SUCCESS) return 0;
  // phys_footprint arrived in TASK_VM_INFO_REV1. A kernel that predates it
  // answers TASK_VM_INFO successfully but writes back a shorter count, leaving
  // the field untouched rather than zeroed -- so trust the count the kernel
  // reported, not the struct we zero-initialised.
  if (count < TASK_VM_INFO_REV1_COUNT) return 0;
  return static_cast<size_t>(info.phys_footprint);
}

// Not needed on macOS -- see Memory.hpp's doc comment on the struct. The
// 80 MB + 29.5 MB ceiling in IdleMemory.cpp was derived once, by hand, with
// a standalone probe; there is nothing for this build to measure at test
// time.
IdleDependencyChainBytes idleDependencyChainBytes() { return IdleDependencyChainBytes{}; }

}  // namespace np

#else  // Linux

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace np {
namespace {

// Case-insensitive substring test -- every classification below is a
// distribution-supplied .so's filename, and neither Ubuntu nor Mesa is
// consistent about case ("libVkLayer_MESA_device_select.so" vs
// "libvulkan.so").
bool containsCi(const std::string& haystackLower, const char* needle) {
  return haystackLower.find(needle) != std::string::npos;
}

std::string toLower(const std::string& s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Classifies one mapped-file path from /proc/self/smaps into the bucket its
// resident bytes belong to, per Memory.hpp's IdleDependencyChainBytes doc
// comment. Bracketed pseudo-paths ("[heap]", "[stack]", ...) and the unnamed
// anonymous mappings malloc backs (empty path) are the process's own state,
// not a dependency chain -- left uncredited so they count only against
// IdleMemory.cpp's 80 MB core budget, same as the executable's own mapping
// and the small set of base-runtime libraries every SDL/X11 app links
// regardless of OpenImageIO or Vulkan.
enum class LibraryChain { kCore, kVulkanDriver, kOiioChain };

LibraryChain classifyMappedLibrary(const std::string& path) {
  if (path.empty() || path.front() == '[') return LibraryChain::kCore;
  const std::string lower = toLower(path);

  // The Vulkan loader, every ICD it dlopen()s while enumerating physical
  // devices (only lvp/lavapipe is ever selected on this machine -- the
  // rest are the same probe cost a real GPU's own ICD would have paid),
  // LLVM (lavapipe JIT-compiles its rasterizer), and the Mesa glue that
  // rides along with any of that (Gallium, EGL, GLX, DRM/GBM).
  static const char* const kDriverNeedles[] = {
      "vulkan", "llvm", "vklayer", "gallium", "libdrm", "libgbm",
      "egl",    "glx",  "mesa",
  };
  for (const char* needle : kDriverNeedles) {
    if (containsCi(lower, needle)) return LibraryChain::kVulkanDriver;
  }

  // The base runtime every SDL3/X11 app on Linux links, independent of
  // OpenImageIO or Vulkan -- the analogue of what macOS's 80 MB core
  // ceiling already covers for "SDL3's own video subsystem init" (see
  // IdleMemory.cpp). Everything past this point that is still a named
  // shared library got loaded for exactly one reason: OpenImageIO.
  static const char* const kCoreNeedles[] = {
      "naturalpaint", "libc.so",   "libc-",     "libm.so",    "libstdc++",
      "libgcc_s",     "ld-linux",  "linux-vdso", "libsdl",     "libx11",
      "libxext",      "libxau",    "libxdmcp",  "libxcb",     "libxrender",
      "libxfixes",    "libz.so",
  };
  for (const char* needle : kCoreNeedles) {
    if (containsCi(lower, needle)) return LibraryChain::kCore;
  }

  return LibraryChain::kOiioChain;
}

// Whether `line` is a mapping's header line ("7f0000000000-7f0000021000
// r-xp 00000000 fe:00 12345  /path") rather than one of the `Key: value`
// field lines that follow it (Rss:, Size:, Anonymous:, ...). A leading hex
// digit alone does NOT discriminate this, which is what makes this its own
// function rather than an inline `isxdigit(line[0])`: two of those field
// names -- "Anonymous:" and "AnonHugePages:" -- start with 'A', and 'A'-'F'
// are valid hex digits regardless of case. What no field name ever contains
// is a '-' in its first token, so check for the dash a header's two
// addresses are joined by instead.
bool isMappingHeaderLine(const char* line) {
  const char* p = line;
  bool sawHexBeforeDash = false;
  while (std::isxdigit(static_cast<unsigned char>(*p))) {
    ++p;
    sawHexBeforeDash = true;
  }
  if (!sawHexBeforeDash || *p != '-') return false;
  ++p;
  bool sawHexAfterDash = false;
  while (std::isxdigit(static_cast<unsigned char>(*p))) {
    ++p;
    sawHexAfterDash = true;
  }
  return sawHexAfterDash && *p == ' ';
}

// Parses /proc/self/smaps once: sums each mapping's `Rss:` line into a
// running total keyed by that mapping's file path (a shared library's text,
// rodata and data segments are separate smaps entries sharing one path), and
// buckets the totals through classifyMappedLibrary(). See Memory.hpp for why
// this is captured once, on the first call to currentResidentBytes(), not
// wherever idleDependencyChainBytes() itself first gets called.
IdleDependencyChainBytes computeIdleDependencyChainBytesFromSmaps() {
  IdleDependencyChainBytes result;
  std::FILE* f = std::fopen("/proc/self/smaps", "r");
  if (f == nullptr) return result;

  char line[1024];
  std::string currentPath;
  LibraryChain currentChain = LibraryChain::kCore;
  bool haveMapping = false;

  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (isMappingHeaderLine(line)) {
      // Field 6 (0-indexed 5), if present, is the mapped file's path; absent
      // for anonymous mappings. Skip the five fixed fields, then take the
      // rest of the line verbatim (a path can itself contain spaces).
      const char* p = line;
      int fieldsSkipped = 0;
      while (fieldsSkipped < 5 && *p != '\0') {
        while (*p == ' ') ++p;
        while (*p != ' ' && *p != '\0' && *p != '\n') ++p;
        ++fieldsSkipped;
      }
      while (*p == ' ') ++p;
      size_t len = std::strlen(p);
      while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) --len;
      currentPath.assign(p, len);
      currentChain = classifyMappedLibrary(currentPath);
      haveMapping = true;
    } else if (haveMapping && std::strncmp(line, "Rss:", 4) == 0) {
      const long kb = std::strtol(line + 4, nullptr, 10);
      if (kb > 0) {
        const size_t bytes = static_cast<size_t>(kb) * 1024;
        if (currentChain == LibraryChain::kVulkanDriver) {
          result.vulkanDriverBytes += bytes;
        } else if (currentChain == LibraryChain::kOiioChain) {
          result.oiioChainBytes += bytes;
        }
      }
    }
  }
  std::fclose(f);
  return result;
}

// Linux source: scans /proc/self/status for a "<field>:   <N> kB" line and
// returns N (in kB), or -1 if the field is missing or the file can't be
// read/parsed. Used only by currentFootprintBytes() below.
long readStatusFieldKb(const char* field) {
  std::FILE* f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return -1;
  char line[256];
  long value = -1;
  const size_t fieldLen = std::strlen(field);
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (std::strncmp(line, field, fieldLen) == 0) {
      value = std::strtol(line + fieldLen, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return value;
}

// Cached on first call -- see Memory.hpp's IdleDependencyChainBytes doc
// comment for why the first call is the one that matters (it is main.cpp's
// idle-RSS capture, before this process has done anything else) and why a
// function-local static, rather than computing this fresh each time
// idleDependencyChainBytes() is asked, is the entire point.
const IdleDependencyChainBytes& cachedIdleDependencyChainBytes() {
  static const IdleDependencyChainBytes captured = computeIdleDependencyChainBytesFromSmaps();
  return captured;
}

}  // namespace

// Linux source: /proc/self/statm, whose second field is the process's
// resident set size in pages (proc(5)) -- see Memory.hpp for why this is
// the same quantity as MACH_TASK_BASIC_INFO's resident_size, not merely an
// approximation of it.
size_t currentResidentBytes() {
  // Side effect, deliberate: see cachedIdleDependencyChainBytes() and
  // Memory.hpp's doc comment on IdleDependencyChainBytes. This is the first
  // call to currentResidentBytes() in the process's life (main.cpp's idle
  // line), so it is also the only correct moment to snapshot the
  // Vulkan-driver and OpenImageIO-chain resident bytes -- app/selftest/
  // IdleMemory.cpp's check does not run until much later.
  cachedIdleDependencyChainBytes();

  std::FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  long totalPages = 0;
  long residentPages = 0;
  const int n = std::fscanf(f, "%ld %ld", &totalPages, &residentPages);
  std::fclose(f);
  if (n != 2) return 0;
  const long pageSize = sysconf(_SC_PAGESIZE);
  if (pageSize <= 0) return 0;
  return static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize);
}

// Linux source: RssAnon + RssShmem from /proc/self/status -- see Memory.hpp
// for what this does and does not have in common with phys_footprint.
size_t currentFootprintBytes() {
  const long anonKb = readStatusFieldKb("RssAnon:");
  const long shmemKb = readStatusFieldKb("RssShmem:");
  if (anonKb < 0 || shmemKb < 0) return 0;
  return (static_cast<size_t>(anonKb) + static_cast<size_t>(shmemKb)) * 1024;
}

// See Memory.hpp's doc comment on IdleDependencyChainBytes. Returns whatever
// cachedIdleDependencyChainBytes() captured on the first call to
// currentResidentBytes() -- if that has not happened yet (nothing has called
// it), computes and caches it now, from whatever this process's memory map
// looks like at the moment of this call instead.
IdleDependencyChainBytes idleDependencyChainBytes() { return cachedIdleDependencyChainBytes(); }

}  // namespace np

#endif
