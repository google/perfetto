/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_PROFILING_UNWIND_CPU_REGISTERS_H_
#define SRC_PROFILING_UNWIND_CPU_REGISTERS_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

struct CpuRegisters {
  static constexpr size_t kMaxRegs = 34;

  CpuArch arch = CpuArch::kUnknown;
  uint64_t raw_data[kMaxRegs] = {0};
  uint64_t sp = 0;  // stack pointer
  uint64_t pc = 0;  // program counter

  void Reset() {
    arch = CpuArch::kUnknown;
    std::memset(raw_data, 0, sizeof(raw_data));
    sp = 0;
    pc = 0;
  }

  // From Kernel perf_event_sample.
  static CpuRegisters FromKernelRegs(CpuArch target_arch,
                                     const uint64_t* raw_regs,
                                     size_t num_regs);

  static CpuRegisters FromUserRegs(CpuArch target_arch, const void* raw_data);
};

}  // namespace profiling
}  // namespace perfetto
#endif  // SRC_PROFILING_UNWIND_CPU_REGISTERS_H_
