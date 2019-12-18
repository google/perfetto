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

#include "src/profiling/perf/unwind_support.h"

#include <inttypes.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <unistd.h>
#include <memory>

#include <unwindstack/Elf.h>
#include <unwindstack/MachineArm.h>
#include <unwindstack/MachineArm64.h>
#include <unwindstack/Regs.h>
#include <unwindstack/RegsArm.h>
#include <unwindstack/RegsArm64.h>
#include <unwindstack/UserArm.h>
#include <unwindstack/UserArm64.h>

// TODO(rsavitski): this includes the kernel uapi constant definitions (for
// register sampling). For now hardcoded for in-tree builds (specifically,
// bionic/include/kernel/). Standalone builds will need to source the headers
// from elsewhere (without depending on the host machine's system headers).
#include <uapi/asm-arm/asm/perf_regs.h>
#include <uapi/asm-x86/asm/perf_regs.h>
#define perf_event_arm_regs perf_event_arm64_regs
#include <uapi/asm-arm64/asm/perf_regs.h>
#undef perf_event_arm_regs

namespace perfetto {
namespace profiling {

namespace {

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
// TODO(rsavitski): cleanly detect 32 bit builds being side-loaded onto a system
// with 64 bit userspace processes.
uint64_t PerfUserRegsMask(unwindstack::ArchEnum arch) {
  // TODO(rsavitski): support the rest of the architectures.
  switch (arch) {
    case unwindstack::ARCH_ARM64:
      return (1ULL << PERF_REG_ARM64_MAX) - 1;
    case unwindstack::ARCH_ARM:
      return ((1ULL << PERF_REG_ARM_MAX) - 1);
    default:
      PERFETTO_FATAL("Unsupported architecture (work in progress)");
  }
}

// Adjusts the given architecture enum based on the ABI (as recorded in the perf
// sample). Note: we do not support 64 bit samples on a 32 bit daemon build, so
// this only converts from 64 bit to 32 bit architectures.
unwindstack::ArchEnum ArchForAbi(unwindstack::ArchEnum arch, uint64_t abi) {
  if (arch == unwindstack::ARCH_ARM64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return unwindstack::ARCH_ARM;
  }
  if (arch == unwindstack::ARCH_X86_64 && abi == PERF_SAMPLE_REGS_ABI_32) {
    return unwindstack::ARCH_X86;
  }
  return arch;
}

// Register values as an array, indexed using the kernel uapi perf_events.h enum
// values. Unsampled values will be left as zeroes.
// TODO(rsavitski): support all relevant architectures (allocate enough space
// for the widest register bank).
struct RawRegisterData {
  static constexpr uint64_t kMaxSize = PERF_REG_ARM64_MAX;
  uint64_t regs[kMaxSize] = {};
};

std::unique_ptr<unwindstack::Regs> ToLibUnwindstackRegs(
    const RawRegisterData& raw_regs,
    unwindstack::ArchEnum arch) {
  // First converts the |RawRegisterData| array to libunwindstack's raw register
  // format, then constructs the relevant unwindstack::Regs subclass out of the
  // latter.
  if (arch == unwindstack::ARCH_ARM64) {
    static_assert(static_cast<int>(unwindstack::ARM64_REG_R0) ==
                      static_cast<int>(PERF_REG_ARM64_X0),
                  "register layout mismatch");
    static_assert(static_cast<int>(unwindstack::ARM64_REG_R30) ==
                      static_cast<int>(PERF_REG_ARM64_LR),
                  "register layout mismatch");

    unwindstack::arm64_user_regs arm64_user_regs;
    memset(&arm64_user_regs, 0, sizeof(arm64_user_regs));
    memcpy(&arm64_user_regs.regs[unwindstack::ARM64_REG_R0],
           &raw_regs.regs[PERF_REG_ARM64_X0],
           sizeof(uint64_t) * (PERF_REG_ARM64_LR - PERF_REG_ARM64_X0 + 1));
    arm64_user_regs.sp = raw_regs.regs[PERF_REG_ARM64_SP];
    arm64_user_regs.pc = raw_regs.regs[PERF_REG_ARM64_PC];

    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm64::Read(&arm64_user_regs));
  }

  if (arch == unwindstack::ARCH_ARM) {
    static_assert(static_cast<int>(unwindstack::ARM_REG_R0) ==
                      static_cast<int>(PERF_REG_ARM_R0),
                  "register layout mismatch");
    static_assert(static_cast<int>(unwindstack::ARM_REG_LAST) ==
                      static_cast<int>(PERF_REG_ARM_MAX),
                  "register layout mismatch");

    unwindstack::arm_user_regs arm_user_regs;
    memset(&arm_user_regs, 0, sizeof(arm_user_regs));
    for (size_t i = unwindstack::ARM_REG_R0; i < unwindstack::ARM_REG_LAST;
         i++) {
      arm_user_regs.regs[i] = static_cast<uint32_t>(raw_regs.regs[i]);
    }

    return std::unique_ptr<unwindstack::Regs>(
        unwindstack::RegsArm::Read(&arm_user_regs));
  }

  PERFETTO_FATAL("Unsupported architecture (work in progress)");
}

}  // namespace

uint64_t PerfUserRegsMaskForCurrentArch() {
  return PerfUserRegsMask(unwindstack::Regs::CurrentArch());
}

// Assumes that the sampling was configured with
// |PerfUserRegsMaskForCurrentArch|.
std::unique_ptr<unwindstack::Regs> ReadPerfUserRegsData(const char** data) {
  unwindstack::ArchEnum requested_arch = unwindstack::Regs::CurrentArch();

  // Layout, assuming a sparse bitmask requesting r1 and r15:
  // [u64 abi] [u64 r1] [u64 r15]
  const char* parse_pos = *data;
  uint64_t sampled_abi;
  parse_pos = ReadValue(&sampled_abi, parse_pos);
  PERFETTO_LOG("WIP: abi: %" PRIu64 "", sampled_abi);

  // Unpack the densely-packed register values into |RawRegisterData|, which has
  // a value for every register (unsampled registers will be left at zero).
  RawRegisterData raw_regs{};
  uint64_t regs_mask = PerfUserRegsMaskForCurrentArch();
  for (size_t i = 0; regs_mask && (i < RawRegisterData::kMaxSize); i++) {
    if (regs_mask & (1u << i)) {
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
  if (requested_arch == unwindstack::ARCH_ARM64 &&
      sampled_abi == PERF_SAMPLE_REGS_ABI_32) {
    raw_regs.regs[PERF_REG_ARM_PC] = raw_regs.regs[PERF_REG_ARM64_PC];
  }

  // Adjust caller's parsing position.
  *data = parse_pos;

  // ABI_NONE means there were no registers (e.g. we've sampled a kernel thread,
  // which doesn't have userspace registers). We still walk over the empty data
  // above, but return an empty result to the caller.
  if (sampled_abi == PERF_SAMPLE_REGS_ABI_NONE) {
    return nullptr;
  } else {
    unwindstack::ArchEnum sampled_arch =
        ArchForAbi(requested_arch, sampled_abi);
    return ToLibUnwindstackRegs(raw_regs, sampled_arch);
  }
}

}  // namespace profiling
}  // namespace perfetto
