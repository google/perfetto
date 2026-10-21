/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/profiling/perf/regs_parsing.h"

#include <linux/perf_event.h>
#include <stdint.h>
#include <unistd.h>

#include <cstring>

#include "perfetto/base/logging.h"
#include "src/profiling/unwind/cpu_registers.h"

// kernel uapi headers
#include <uapi/asm-arm/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#include <uapi/asm-x86/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#define perf_event_arm_regs perf_event_arm64_regs
#include <uapi/asm-arm64/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK
#undef perf_event_arm_regs
#include <uapi/asm-riscv/asm/perf_regs.h>
#undef PERF_REG_EXTENDED_MASK

namespace perfetto {
namespace profiling {

namespace {

constexpr size_t constexpr_max(size_t x, size_t y) {
  return x > y ? x : y;
}

template <typename T>
const char* ReadValue(T* value_out, const char* ptr) {
  memcpy(value_out, reinterpret_cast<const void*>(ptr), sizeof(T));
  return ptr + sizeof(T);
}

// Supported configurations:
// * 32 bit daemon, 32 bit userspace
// * 64 bit daemon, mixed bitness userspace
// Therefore give the kernel the mask corresponding to our build architecture.
// Register parsing handles the mixed userspace ABI cases.
// For simplicity, we ask for as many registers as we can, even if not all of
// them will be used during unwinding.
// TODO(rsavitski): cleanly detect 32 bit traced_perf builds being side-loaded
// onto a system with 64 bit userspace processes.
uint64_t PerfUserRegsMask(CpuArch arch) {
  switch (arch) {
    case CpuArch::ARM64:
      return (1ULL << PERF_REG_ARM64_MAX) - 1;
    case CpuArch::ARM:
      return (1ULL << PERF_REG_ARM_MAX) - 1;
    // perf on x86_64 doesn't allow sampling ds/es/fs/gs registers. See
    // arch/x86/kernel/perf_regs.c in the kernel.
    case CpuArch::X86_64:
      return (((1ULL << PERF_REG_X86_64_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
              ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
              ~(1ULL << PERF_REG_X86_GS));
    // Note: excluding these segment registers might not be necessary on x86,
    // but they won't be used anyway (so follow x64).
    case CpuArch::X86:
      return ((1ULL << PERF_REG_X86_32_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
             ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
             ~(1ULL << PERF_REG_X86_GS);
    case CpuArch::RISCV64:
      return (1ULL << PERF_REG_RISCV_MAX) - 1;
    case CpuArch::UNKNOWN:
      break;
  }
  PERFETTO_FATAL("Unsupported architecture");
}

// Adjusts the given architecture enum based on the ABI (as recorded in the perf
// sample). Note: we do not support 64 bit samples on a 32 bit daemon build, so
// this only converts from 64 bit to 32 bit architectures.
// TODO(rsavitski): on riscv64, are 32 bit userspace processes possible?
CpuArch ArchForAbi(CpuArch arch, uint64_t abi) {
  if (arch == CpuArch::ARM64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return CpuArch::ARM;
  }
  if (arch == CpuArch::X86_64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return CpuArch::X86;
  }
  return arch;
}

// Register values as an array, indexed using the kernel uapi perf_events.h enum
// values. Unsampled values will be left as zeroes.
struct RawRegisterData {
  static constexpr uint64_t kMaxSize =
      constexpr_max(constexpr_max(PERF_REG_ARM_MAX, PERF_REG_ARM64_MAX),
                    constexpr_max(PERF_REG_X86_64_MAX, PERF_REG_RISCV_MAX));
  uint64_t regs[kMaxSize] = {};
};

// Converts the kernel perf_event |RawRegisterData| array (indexed by
// PERF_REG_*) into a |CpuRegisters| struct (indexed by DWARF register numbers).
// For architectures where the perf and DWARF register indices match (ARM64,
// RISCV64, ARM), registers are copied in bulk; for X86/X86_64, where the
// register orderings differ, registers are mapped individually.
CpuRegisters PerfRegsToCpuRegs(const RawRegisterData& raw_regs, CpuArch arch) {
  if (arch == CpuArch::ARM64) {
    static_assert(
        static_cast<int>(ARM64_REG_X0) == static_cast<int>(PERF_REG_ARM64_X0) &&
            static_cast<int>(ARM64_REG_X0) == 0,
        "register layout mismatch");
    static_assert(
        static_cast<int>(ARM64_REG_PC) == static_cast<int>(PERF_REG_ARM64_PC),
        "register layout mismatch");
    CpuRegisters cpu_regs(CpuArch::ARM64);
    memcpy(cpu_regs.regs, raw_regs.regs,
           sizeof(uint64_t) * (PERF_REG_ARM64_PC + 1));
    return cpu_regs;
  }

  if (arch == CpuArch::ARM) {
    static_assert(
        static_cast<int>(ARM_REG_R0) == static_cast<int>(PERF_REG_ARM_R0) &&
            static_cast<int>(ARM_REG_R0) == 0,
        "register layout mismatch");
    static_assert(
        static_cast<int>(ARM_REG_MAX) == static_cast<int>(PERF_REG_ARM_MAX),
        "register layout mismatch");
    // As with arm64, the layouts match, but we need to downcast to u32.
    CpuRegisters cpu_regs(CpuArch::ARM);
    for (size_t i = 0; i < ARM_REG_MAX; i++) {
      cpu_regs.regs[i] = static_cast<uint32_t>(raw_regs.regs[i]);
    }
    return cpu_regs;
  }

  if (arch == CpuArch::X86_64) {
    // We've sampled more registers than what libunwindstack will use. Don't
    // copy over cs/ss/flags.
    CpuRegisters cpu_regs(CpuArch::X86_64);
    cpu_regs.regs[X86_64_REG_RAX] = raw_regs.regs[PERF_REG_X86_AX];
    cpu_regs.regs[X86_64_REG_RBX] = raw_regs.regs[PERF_REG_X86_BX];
    cpu_regs.regs[X86_64_REG_RCX] = raw_regs.regs[PERF_REG_X86_CX];
    cpu_regs.regs[X86_64_REG_RDX] = raw_regs.regs[PERF_REG_X86_DX];
    cpu_regs.regs[X86_64_REG_R8] = raw_regs.regs[PERF_REG_X86_R8];
    cpu_regs.regs[X86_64_REG_R9] = raw_regs.regs[PERF_REG_X86_R9];
    cpu_regs.regs[X86_64_REG_R10] = raw_regs.regs[PERF_REG_X86_R10];
    cpu_regs.regs[X86_64_REG_R11] = raw_regs.regs[PERF_REG_X86_R11];
    cpu_regs.regs[X86_64_REG_R12] = raw_regs.regs[PERF_REG_X86_R12];
    cpu_regs.regs[X86_64_REG_R13] = raw_regs.regs[PERF_REG_X86_R13];
    cpu_regs.regs[X86_64_REG_R14] = raw_regs.regs[PERF_REG_X86_R14];
    cpu_regs.regs[X86_64_REG_R15] = raw_regs.regs[PERF_REG_X86_R15];
    cpu_regs.regs[X86_64_REG_RDI] = raw_regs.regs[PERF_REG_X86_DI];
    cpu_regs.regs[X86_64_REG_RSI] = raw_regs.regs[PERF_REG_X86_SI];
    cpu_regs.regs[X86_64_REG_RBP] = raw_regs.regs[PERF_REG_X86_BP];
    cpu_regs.regs[X86_64_REG_RSP] = raw_regs.regs[PERF_REG_X86_SP];
    cpu_regs.regs[X86_64_REG_RIP] = raw_regs.regs[PERF_REG_X86_IP];
    return cpu_regs;
  }

  if (arch == CpuArch::X86) {
    // We've sampled more registers than what libunwindstack will use. Don't
    // copy over cs/ss/flags.
    // Both raw_regs.regs and cpu_regs.regs are uint64_t, but the x86 registers
    // are only 32-bit. We mask them to avoid writing incorrect values.
    CpuRegisters cpu_regs(CpuArch::X86);
    cpu_regs.regs[X86_REG_EAX] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_AX]);
    cpu_regs.regs[X86_REG_EBX] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_BX]);
    cpu_regs.regs[X86_REG_ECX] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_CX]);
    cpu_regs.regs[X86_REG_EDX] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_DX]);
    cpu_regs.regs[X86_REG_EBP] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_BP]);
    cpu_regs.regs[X86_REG_EDI] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_DI]);
    cpu_regs.regs[X86_REG_ESI] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_SI]);
    cpu_regs.regs[X86_REG_ESP] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_SP]);
    cpu_regs.regs[X86_REG_EIP] =
        static_cast<uint32_t>(raw_regs.regs[PERF_REG_X86_IP]);
    return cpu_regs;
  }

  if (arch == CpuArch::RISCV64) {
    static_assert(static_cast<int>(RISCV64_REG_PC) ==
                          static_cast<int>(PERF_REG_RISCV_PC) &&
                      static_cast<int>(RISCV64_REG_PC) == 0,
                  "register layout mismatch");
    static_assert(static_cast<int>(RISCV64_REG_VLENB) ==
                      static_cast<int>(PERF_REG_RISCV_MAX),
                  "register layout mismatch");
    // Register layout matches, copy the raw data to the cpu_regs.regs array.
    CpuRegisters cpu_regs(CpuArch::RISCV64);
    memcpy(cpu_regs.regs, raw_regs.regs, sizeof(uint64_t) * PERF_REG_RISCV_MAX);
    return cpu_regs;
  }

  PERFETTO_FATAL("Unsupported architecture");
}

}  // namespace

uint64_t PerfUserRegsMaskForArch(CpuArch arch) {
  return PerfUserRegsMask(arch);
}

// Assumes that the sampling was configured with
// |PerfUserRegsMaskForArch(CurrentCpuArch())|.
std::optional<CpuRegisters> ReadPerfUserRegsData(const char** data) {
  CpuArch requested_arch = CurrentCpuArch();

  // Layout, assuming a sparse bitmask requesting r1 and r15:
  // userspace thread: [u64 abi] [u64 r1] [u64 r15]
  // kernel thread:    [u64 abi]
  const char* parse_pos = *data;
  uint64_t sampled_abi;
  parse_pos = ReadValue(&sampled_abi, parse_pos);

  // ABI_NONE means there were no registers, as we've sampled a kernel thread,
  // which doesn't have userspace registers.
  if (sampled_abi == PERF_SAMPLE_REGS_ABI_NONE) {
    *data = parse_pos;  // adjust caller's parsing position
    return std::nullopt;
  }

  // Unpack the densely-packed register values into |RawRegisterData|, which has
  // a value for every register (unsampled registers will be left at zero).
  RawRegisterData raw_regs{};
  uint64_t regs_mask = PerfUserRegsMaskForArch(requested_arch);
  for (size_t i = 0; regs_mask && (i < RawRegisterData::kMaxSize); i++) {
    if (regs_mask & (1ULL << i)) {
      parse_pos = ReadValue(&raw_regs.regs[i], parse_pos);
    }
  }

  // Special case: we've requested arm64 registers from a 64 bit kernel, but
  // ended up sampling a 32 bit arm userspace process. The 32 bit execution
  // state of the target process was saved by the exception entry in an
  // ISA-specific way. The userspace R0-R14 end up saved as arm64 W0-W14, but
  // the program counter (R15 on arm32) is still in PERF_REG_ARM64_PC (the 33rd
  // register). So we can take the kernel-dumped 64 bit register state, reassign
  // the PC into the R15 slot, and treat the resulting RawRegisterData as an
  // arm32 register bank. See "Fundamentals of ARMv8-A" (ARM DOC
  // 100878_0100_en), page 28.
  // x86-64 doesn't need any such fixups.
  if (requested_arch == CpuArch::ARM64 &&
      sampled_abi == PERF_SAMPLE_REGS_ABI_32) {
    raw_regs.regs[PERF_REG_ARM_PC] = raw_regs.regs[PERF_REG_ARM64_PC];
  }

  *data = parse_pos;  // adjust caller's parsing position

  CpuArch sampled_arch = ArchForAbi(requested_arch, sampled_abi);
  return PerfRegsToCpuRegs(raw_regs, sampled_arch);
}

}  // namespace profiling
}  // namespace perfetto
