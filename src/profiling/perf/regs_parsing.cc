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

#include <cinttypes>
#include <memory>
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_types.h"

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
  switch (static_cast<uint8_t>(arch)) {  // cast to please -Wswitch-enum
    case CpuArch::kArm64:
      return (1ULL << PERF_REG_ARM64_MAX) - 1;
    case CpuArch::kArm:
      return (1ULL << PERF_REG_ARM_MAX) - 1;
    // perf on x86_64 doesn't allow sampling ds/es/fs/gs registers. See
    // arch/x86/kernel/perf_regs.c in the kernel.
    case CpuArch::kX86_64:
      return (((1ULL << PERF_REG_X86_64_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
              ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
              ~(1ULL << PERF_REG_X86_GS));
    // Note: excluding these segment registers might not be necessary on x86,
    // but they won't be used anyway (so follow x64).
    case CpuArch::kX86:
      return ((1ULL << PERF_REG_X86_32_MAX) - 1) & ~(1ULL << PERF_REG_X86_DS) &
             ~(1ULL << PERF_REG_X86_ES) & ~(1ULL << PERF_REG_X86_FS) &
             ~(1ULL << PERF_REG_X86_GS);
    case CpuArch::kRiscv64:
      return (1ULL << PERF_REG_RISCV_MAX) - 1;
    default:
      PERFETTO_FATAL("Unsupported architecture");
  }
}

// Adjusts the given architecture enum based on the ABI (as recorded in the perf
// sample). Note: we do not support 64 bit samples on a 32 bit daemon build, so
// this only converts from 64 bit to 32 bit architectures.
// TODO(rsavitski): on riscv64, are 32 bit userspace processes possible?
CpuArch ArchForAbi(CpuArch arch, uint64_t abi) {
  if (arch == CpuArch::kArm64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return CpuArch::kArm;
  }
  if (arch == CpuArch::kX86_64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return CpuArch::kX86;
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

}  // namespace

uint64_t PerfUserRegsMaskForArch(CpuArch arch) {
  return PerfUserRegsMask(arch);
}

// Assumes that the sampling was configured with
// |PerfUserRegsMaskForArch(CurrentCpuArch())|.
std::unique_ptr<CpuRegisters> ReadPerfUserRegsData(const char** data) {
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
    return nullptr;
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
  if (requested_arch == CpuArch::kArm64 &&
      sampled_abi == PERF_SAMPLE_REGS_ABI_32) {
    raw_regs.regs[PERF_REG_ARM_PC] = raw_regs.regs[PERF_REG_ARM64_PC];
  }

  *data = parse_pos;  // adjust caller's parsing position

  CpuArch sampled_arch = ArchForAbi(requested_arch, sampled_abi);
  return std::make_unique<CpuRegisters>(CpuRegisters::FromKernelRegs(
      sampled_arch, raw_regs.regs, RawRegisterData::kMaxSize));
}

}  // namespace profiling
}  // namespace perfetto
