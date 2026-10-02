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

#include "src/profiling/unwind/cpu_registers.h"

#include <algorithm>

#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

namespace {

static_assert(CpuRegisters::kMaxRegs >=
                  std::max({static_cast<size_t>(PERF_REG_ARM_MAX),
                            static_cast<size_t>(PERF_REG_ARM64_MAX),
                            static_cast<size_t>(PERF_REG_X86_64_MAX),
                            static_cast<size_t>(PERF_REG_RISCV_MAX)}),
              "CpuRegisters::kMaxRegs is too small for perf registers");

}  // namespace

// static
CpuRegisters CpuRegisters::FromKernelRegs(CpuArch target_arch,
                                          const uint64_t* raw_regs,
                                          size_t num_regs) {
  CpuRegisters r;
  r.arch = target_arch;
  if (raw_regs && num_regs > 0) {
    const size_t regs_to_copy = (num_regs < kMaxRegs ? num_regs : kMaxRegs);
    std::memcpy(r.raw_data, raw_regs, regs_to_copy * sizeof(uint64_t));
  }

  switch (target_arch) {
    case CpuArch::kArm64:
      r.sp = r.raw_data[PERF_REG_ARM64_SP];
      r.pc = r.raw_data[PERF_REG_ARM64_PC];
      break;
    case CpuArch::kArm:
      r.sp = r.raw_data[PERF_REG_ARM_SP];
      r.pc = r.raw_data[PERF_REG_ARM_PC];
      break;
    case CpuArch::kX86_64:
    case CpuArch::kX86:
      r.sp = r.raw_data[PERF_REG_X86_SP];
      r.pc = r.raw_data[PERF_REG_X86_IP];
      break;
    case CpuArch::kRiscv64:
      r.sp = r.raw_data[PERF_REG_RISCV_SP];
      r.pc = r.raw_data[PERF_REG_RISCV_PC];
      break;
    case CpuArch::kUnknown:
      break;
  }
  return r;
}

// DWARF register numbers mapping (compatible with libunwindstack AsmGetRegs).
// https://github.com/llvm/llvm-project/blob/main/libunwind/include/libunwind.h#L251
// static
CpuRegisters CpuRegisters::FromUserRegs(CpuArch target_arch,
                                        const void* raw_data) {
  CpuRegisters r;
  if (!raw_data) {
    return r;
  }

  switch (target_arch) {
    case CpuArch::kX86_64: {
      r.arch = CpuArch::kX86_64;
      const auto* raw = reinterpret_cast<const uint64_t*>(raw_data);
      r.raw_data[PERF_REG_X86_AX] = raw[0];
      r.raw_data[PERF_REG_X86_DX] = raw[1];
      r.raw_data[PERF_REG_X86_CX] = raw[2];
      r.raw_data[PERF_REG_X86_BX] = raw[3];
      r.raw_data[PERF_REG_X86_SI] = raw[4];
      r.raw_data[PERF_REG_X86_DI] = raw[5];
      r.raw_data[PERF_REG_X86_BP] = raw[6];
      r.raw_data[PERF_REG_X86_SP] = raw[7];
      r.raw_data[PERF_REG_X86_R8] = raw[8];
      r.raw_data[PERF_REG_X86_R9] = raw[9];
      r.raw_data[PERF_REG_X86_R10] = raw[10];
      r.raw_data[PERF_REG_X86_R11] = raw[11];
      r.raw_data[PERF_REG_X86_R12] = raw[12];
      r.raw_data[PERF_REG_X86_R13] = raw[13];
      r.raw_data[PERF_REG_X86_R14] = raw[14];
      r.raw_data[PERF_REG_X86_R15] = raw[15];
      r.raw_data[PERF_REG_X86_IP] = raw[16];
      r.sp = r.raw_data[PERF_REG_X86_SP];
      r.pc = r.raw_data[PERF_REG_X86_IP];
      return r;
    }
    case CpuArch::kX86: {
      r.arch = CpuArch::kX86;
      const auto* raw = reinterpret_cast<const uint32_t*>(raw_data);
      r.raw_data[PERF_REG_X86_AX] = raw[0];
      r.raw_data[PERF_REG_X86_CX] = raw[1];
      r.raw_data[PERF_REG_X86_DX] = raw[2];
      r.raw_data[PERF_REG_X86_BX] = raw[3];
      r.raw_data[PERF_REG_X86_SP] = raw[4];
      r.raw_data[PERF_REG_X86_BP] = raw[5];
      r.raw_data[PERF_REG_X86_SI] = raw[6];
      r.raw_data[PERF_REG_X86_DI] = raw[7];
      r.raw_data[PERF_REG_X86_IP] = raw[8];
      r.sp = r.raw_data[PERF_REG_X86_SP];
      r.pc = r.raw_data[PERF_REG_X86_IP];
      return r;
    }
    case CpuArch::kArm64: {
      r.arch = CpuArch::kArm64;
      const auto* raw = reinterpret_cast<const uint64_t*>(raw_data);
      std::memcpy(&r.raw_data[0], raw, sizeof(uint64_t) * PERF_REG_ARM64_MAX);
      r.sp = r.raw_data[PERF_REG_ARM64_SP];
      r.pc = r.raw_data[PERF_REG_ARM64_PC];
      return r;
    }
    case CpuArch::kArm: {
      r.arch = CpuArch::kArm;
      const auto* raw = reinterpret_cast<const uint32_t*>(raw_data);
      for (size_t i = 0; i < PERF_REG_ARM_MAX; ++i) {
        r.raw_data[i] = raw[i];
      }
      r.sp = r.raw_data[PERF_REG_ARM_SP];
      r.pc = r.raw_data[PERF_REG_ARM_PC];
      return r;
    }
    case CpuArch::kRiscv64: {
      r.arch = CpuArch::kRiscv64;
      const auto* raw = reinterpret_cast<const uint64_t*>(raw_data);
      std::memcpy(&r.raw_data[0], raw, sizeof(uint64_t) * PERF_REG_RISCV_MAX);
      r.sp = r.raw_data[PERF_REG_RISCV_SP];
      r.pc = r.raw_data[PERF_REG_RISCV_PC];
      return r;
    }
    case CpuArch::kUnknown: {
      r.arch = CpuArch::kUnknown;
      return r;
    }
  }
}

}  // namespace profiling
}  // namespace perfetto
