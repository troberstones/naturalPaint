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

}  // namespace np
