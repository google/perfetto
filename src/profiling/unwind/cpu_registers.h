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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"

namespace perfetto {
namespace profiling {

enum class CpuArch : uint8_t {
  UNKNOWN = 0,
  ARM,
  ARM64,
  X86,
  X86_64,
  RISCV64,
};

constexpr CpuArch CurrentCpuArch() {
#if PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_ARM64)
  return CpuArch::ARM64;
#elif PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)
  return CpuArch::X86_64;
#elif PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_RISCV)
  return CpuArch::RISCV64;
#elif defined(__arm__)
  return CpuArch::ARM;
#elif defined(__i386__)
  return CpuArch::X86;
#else
  return CpuArch::UNKNOWN;
#endif
}

// Per-architecture register indices.
// x86     : DWARF ordering (IA-32 ABI).
// x86_64  : DWARF ordering (System V ABI).
// ARM     : DWARF / user_pt_regs / perf_event ordering
// ARM64   : user_pt_regs ordering, 0..31 matches with DWARF.
// RISCV64 : user_pt_regs / perf_event + vlenb(32). 1..31 matches with DWARF.

// 0..15 : matches DWARF / user_pt_regs / perf_event ordering
enum ArmReg : uint16_t {
  ARM_REG_R0 = 0,
  ARM_REG_R1 = 1,
  ARM_REG_R2 = 2,
  ARM_REG_R3 = 3,
  ARM_REG_R4 = 4,
  ARM_REG_R5 = 5,
  ARM_REG_R6 = 6,
  ARM_REG_R7 = 7,
  ARM_REG_R8 = 8,
  ARM_REG_R9 = 9,
  ARM_REG_R10 = 10,
  ARM_REG_R11 = 11,
  ARM_REG_R12 = 12,
  ARM_REG_R13 = 13,
  ARM_REG_R14 = 14,
  ARM_REG_R15 = 15,
  ARM_REG_MAX = 16,

  ARM_REG_FP = ARM_REG_R11,
  ARM_REG_SP = ARM_REG_R13,
  ARM_REG_LR = ARM_REG_R14,
  ARM_REG_PC = ARM_REG_R15,
};

// Fully matches user_pt_regs registers.
// 0-31: DWARF/perf_event/user_pt_regs registers.
// 32 (PC): perf_event and user_pt_regs.
// 33 (PSTATE): user_pt_regs only.
enum Arm64Reg : uint16_t {
  ARM64_REG_X0 = 0,
  ARM64_REG_X1 = 1,
  ARM64_REG_X2 = 2,
  ARM64_REG_X3 = 3,
  ARM64_REG_X4 = 4,
  ARM64_REG_X5 = 5,
  ARM64_REG_X6 = 6,
  ARM64_REG_X7 = 7,
  ARM64_REG_X8 = 8,
  ARM64_REG_X9 = 9,
  ARM64_REG_X10 = 10,
  ARM64_REG_X11 = 11,
  ARM64_REG_X12 = 12,
  ARM64_REG_X13 = 13,
  ARM64_REG_X14 = 14,
  ARM64_REG_X15 = 15,
  ARM64_REG_X16 = 16,
  ARM64_REG_X17 = 17,
  ARM64_REG_X18 = 18,
  ARM64_REG_X19 = 19,
  ARM64_REG_X20 = 20,
  ARM64_REG_X21 = 21,
  ARM64_REG_X22 = 22,
  ARM64_REG_X23 = 23,
  ARM64_REG_X24 = 24,
  ARM64_REG_X25 = 25,
  ARM64_REG_X26 = 26,
  ARM64_REG_X27 = 27,
  ARM64_REG_X28 = 28,
  ARM64_REG_X29 = 29,
  ARM64_REG_X30 = 30,
  ARM64_REG_SP = 31,
  ARM64_REG_PC = 32,
  ARM64_REG_PSTATE = 33,
  ARM64_REG_MAX = 34,

  ARM64_REG_FP = ARM64_REG_X29,
  ARM64_REG_LR = ARM64_REG_X30,
};

// Following DWARF (IA-32 ABI) register ordering.
enum X86Reg : uint16_t {
  X86_REG_EAX = 0,
  X86_REG_ECX = 1,
  X86_REG_EDX = 2,
  X86_REG_EBX = 3,
  X86_REG_ESP = 4,
  X86_REG_EBP = 5,
  X86_REG_ESI = 6,
  X86_REG_EDI = 7,
  X86_REG_EIP = 8,
  X86_REG_EFL = 9,
  X86_REG_CS = 10,
  X86_REG_SS = 11,
  X86_REG_DS = 12,
  X86_REG_ES = 13,
  X86_REG_FS = 14,
  X86_REG_GS = 15,
  X86_REG_MAX = 16,

  X86_REG_FP = X86_REG_EBP,
};

// Following DWARF (System V ABI) register ordering.
enum X86_64Reg : uint16_t {
  X86_64_REG_RAX = 0,
  X86_64_REG_RDX = 1,
  X86_64_REG_RCX = 2,
  X86_64_REG_RBX = 3,
  X86_64_REG_RSI = 4,
  X86_64_REG_RDI = 5,
  X86_64_REG_RBP = 6,
  X86_64_REG_RSP = 7,
  X86_64_REG_R8 = 8,
  X86_64_REG_R9 = 9,
  X86_64_REG_R10 = 10,
  X86_64_REG_R11 = 11,
  X86_64_REG_R12 = 12,
  X86_64_REG_R13 = 13,
  X86_64_REG_R14 = 14,
  X86_64_REG_R15 = 15,
  X86_64_REG_RIP = 16,
  X86_64_REG_MAX = 17,

  X86_64_REG_FP = X86_64_REG_RBP,
};

// 0 (PC): following perf_event/user_pt_regs registers.
// 1..31: following DWARF/perf_event/user_pt_regs registers.
// 32 (vlenb): DWARF register (0x1c22).
enum Riscv64Reg : uint16_t {
  RISCV64_REG_PC = 0,
  RISCV64_REG_RA = 1,
  RISCV64_REG_SP = 2,
  RISCV64_REG_GP = 3,
  RISCV64_REG_TP = 4,
  RISCV64_REG_T0 = 5,
  RISCV64_REG_T1 = 6,
  RISCV64_REG_T2 = 7,
  RISCV64_REG_S0 = 8,
  RISCV64_REG_S1 = 9,
  RISCV64_REG_A0 = 10,
  RISCV64_REG_A1 = 11,
  RISCV64_REG_A2 = 12,
  RISCV64_REG_A3 = 13,
  RISCV64_REG_A4 = 14,
  RISCV64_REG_A5 = 15,
  RISCV64_REG_A6 = 16,
  RISCV64_REG_A7 = 17,
  RISCV64_REG_S2 = 18,
  RISCV64_REG_S3 = 19,
  RISCV64_REG_S4 = 20,
  RISCV64_REG_S5 = 21,
  RISCV64_REG_S6 = 22,
  RISCV64_REG_S7 = 23,
  RISCV64_REG_S8 = 24,
  RISCV64_REG_S9 = 25,
  RISCV64_REG_S10 = 26,
  RISCV64_REG_S11 = 27,
  RISCV64_REG_T3 = 28,
  RISCV64_REG_T4 = 29,
  RISCV64_REG_T5 = 30,
  RISCV64_REG_T6 = 31,
  RISCV64_REG_REAL_MAX = 32,
  // This is the last real register, vlenb is a special register value.
  RISCV64_REG_VLENB = RISCV64_REG_REAL_MAX,
  RISCV64_REG_MAX = 33,

  RISCV64_REG_FP = RISCV64_REG_S0,
};

struct CpuRegisters {
  static constexpr size_t kMaxRegs =
      std::max<size_t>({ARM_REG_MAX, ARM64_REG_MAX, X86_REG_MAX, X86_64_REG_MAX,
                        RISCV64_REG_MAX});

  CpuRegisters() = default;
  explicit CpuRegisters(CpuArch a) : arch(a) {}

  CpuArch arch = CpuArch::UNKNOWN;
  uint64_t regs[kMaxRegs] = {};

  uint64_t& operator[](size_t index) {
    PERFETTO_DCHECK(index < kMaxRegs);
    return regs[index];
  }

  uint64_t operator[](size_t index) const {
    PERFETTO_DCHECK(index < kMaxRegs);
    return regs[index];
  }

  uint64_t sp() const {
    switch (arch) {
      case CpuArch::ARM:
        return regs[ARM_REG_SP];
      case CpuArch::ARM64:
        return regs[ARM64_REG_SP];
      case CpuArch::X86:
        return regs[X86_REG_ESP];
      case CpuArch::X86_64:
        return regs[X86_64_REG_RSP];
      case CpuArch::RISCV64:
        return regs[RISCV64_REG_SP];
      case CpuArch::UNKNOWN:
        return 0;
    }
    return 0;
  }

  void set_sp(uint64_t val) {
    switch (arch) {
      case CpuArch::ARM:
        regs[ARM_REG_SP] = static_cast<uint32_t>(val);
        break;
      case CpuArch::ARM64:
        regs[ARM64_REG_SP] = val;
        break;
      case CpuArch::X86:
        regs[X86_REG_ESP] = static_cast<uint32_t>(val);
        break;
      case CpuArch::X86_64:
        regs[X86_64_REG_RSP] = val;
        break;
      case CpuArch::RISCV64:
        regs[RISCV64_REG_SP] = val;
        break;
      case CpuArch::UNKNOWN:
        break;
    }
  }

  uint64_t pc() const {
    switch (arch) {
      case CpuArch::ARM:
        return regs[ARM_REG_PC];
      case CpuArch::ARM64:
        return regs[ARM64_REG_PC];
      case CpuArch::X86:
        return regs[X86_REG_EIP];
      case CpuArch::X86_64:
        return regs[X86_64_REG_RIP];
      case CpuArch::RISCV64:
        return regs[RISCV64_REG_PC];
      case CpuArch::UNKNOWN:
        return 0;
    }
    return 0;
  }

  void set_pc(uint64_t val) {
    switch (arch) {
      case CpuArch::ARM:
        regs[ARM_REG_PC] = static_cast<uint32_t>(val);
        break;
      case CpuArch::ARM64:
        regs[ARM64_REG_PC] = val;
        break;
      case CpuArch::X86:
        regs[X86_REG_EIP] = static_cast<uint32_t>(val);
        break;
      case CpuArch::X86_64:
        regs[X86_64_REG_RIP] = val;
        break;
      case CpuArch::RISCV64:
        regs[RISCV64_REG_PC] = val;
        break;
      case CpuArch::UNKNOWN:
        break;
    }
  }
};

}  // namespace profiling
}  // namespace perfetto
#endif  // SRC_PROFILING_UNWIND_CPU_REGISTERS_H_
