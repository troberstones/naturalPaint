#include "app/Memory.hpp"

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
