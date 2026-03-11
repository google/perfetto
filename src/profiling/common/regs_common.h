/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_PROFILING_COMMON_REGS_COMMON_H_
#define SRC_PROFILING_COMMON_REGS_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>

namespace perfetto {
namespace profiling {

// CPU architecture enum. Integer values match unwindstack::ArchEnum for
// wire protocol ABI compatibility.
enum ArchEnum : uint8_t {
  kArchUnknown = 0,
  kArchArm = 1,
  kArchArm64 = 2,
  kArchX86 = 3,
  kArchX86_64 = 4,
  kArchMips = 5,
  kArchMips64 = 6,
  kArchRiscv64 = 7,
};

// Register counts per architecture. These match the libunwindstack
// register counts (ARM_REG_LAST, ARM64_REG_LAST, etc.) to maintain
// wire protocol ABI compatibility.
constexpr size_t kArmRegCount = 16;      // ARM_REG_LAST
constexpr size_t kArm64RegCount = 34;    // ARM64_REG_LAST (includes PSTATE)
constexpr size_t kX86RegCount = 16;      // X86_REG_LAST
constexpr size_t kX86_64RegCount = 17;   // X86_64_REG_LAST
constexpr size_t kRiscv64RegCount = 33;  // RISCV64_REG_COUNT (includes VLENB)

// Maximum register data size in bytes. This is the size of the register
// buffer in the wire protocol and must not change without updating the
// wire protocol version.
constexpr size_t kMaxRegisterDataSize = std::max(
    {sizeof(uint32_t) * kArmRegCount, sizeof(uint64_t) * kArm64RegCount,
     sizeof(uint32_t) * kX86RegCount, sizeof(uint64_t) * kX86_64RegCount,
     sizeof(uint64_t) * kRiscv64RegCount});

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_COMMON_REGS_COMMON_H_
