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

}  // namespace np

#else  // Linux

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace np {
namespace {

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

}  // namespace

// Linux source: /proc/self/statm, whose second field is the process's
// resident set size in pages (proc(5)) -- see Memory.hpp for why this is
// the same quantity as MACH_TASK_BASIC_INFO's resident_size, not merely an
// approximation of it.
size_t currentResidentBytes() {
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

}  // namespace np

#endif
