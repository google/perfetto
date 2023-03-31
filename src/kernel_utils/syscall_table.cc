/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "src/kernel_utils/syscall_table.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <sys/utsname.h>
#endif

#include "src/kernel_utils/syscalls_aarch32.h"
#include "src/kernel_utils/syscalls_aarch64.h"
#include "src/kernel_utils/syscalls_armeabi.h"
#include "src/kernel_utils/syscalls_x86.h"
#include "src/kernel_utils/syscalls_x86_64.h"

namespace perfetto {

template <typename T>
constexpr size_t GetSyscalls(const T&) {
  static_assert(std::extent<T>::value <= kMaxSyscalls,
                "kMaxSyscalls too small");
  return std::extent<T>::value;
}

SyscallTable::SyscallTable(Architecture arch) {
  static const char* kSyscalls_Unknown[] = {nullptr};

  switch (arch) {
    case kArmEabi:
      syscall_count_ = GetSyscalls(kSyscalls_ArmEabi);
      syscall_table_ = &kSyscalls_ArmEabi[0];
      break;
    case kAarch32:
      syscall_count_ = GetSyscalls(kSyscalls_Aarch32);
      syscall_table_ = &kSyscalls_Aarch32[0];
      break;
    case kAarch64:
      syscall_count_ = GetSyscalls(kSyscalls_Aarch64);
      syscall_table_ = &kSyscalls_Aarch64[0];
      break;
    case kX86_64:
      syscall_count_ = GetSyscalls(kSyscalls_x86_64);
      syscall_table_ = &kSyscalls_x86_64[0];
      break;
    case kX86:
      syscall_count_ = GetSyscalls(kSyscalls_x86);
      syscall_table_ = &kSyscalls_x86[0];
      break;
    case kUnknown:
      syscall_count_ = 0;
      syscall_table_ = &kSyscalls_Unknown[0];
      break;
  }
}

Architecture SyscallTable::ArchFromString(base::StringView machine) {
  if (machine == "aarch64") {
    return kAarch64;
  } else if (machine == "armv8l") {
    return kArmEabi;
  } else if (machine == "armv7l") {
    return kAarch32;
  } else if (machine == "x86_64") {
    return kX86_64;
  } else if (machine == "i686") {
    return kX86;
  } else {
    return kUnknown;
  }
}

SyscallTable SyscallTable::FromCurrentArch() {
  Architecture arch = kUnknown;

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  struct utsname uname_info;
  if (uname(&uname_info) == 0) {
    arch = ArchFromString(uname_info.machine);
  }
#endif

  return SyscallTable(arch);
}

std::optional<size_t> SyscallTable::GetByName(const std::string& name) const {
  for (size_t i = 0; i < syscall_count_; i++) {
    if (name == syscall_table_[i]) {
      return i;
    }
  }
  return std::nullopt;
}

const char* SyscallTable::GetById(size_t id) const {
  if (id < syscall_count_) {
    return syscall_table_[id];
  }
  return nullptr;
}

}  // namespace perfetto
