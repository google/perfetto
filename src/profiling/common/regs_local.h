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

#ifndef SRC_PROFILING_COMMON_REGS_LOCAL_H_
#define SRC_PROFILING_COMMON_REGS_LOCAL_H_

#include "src/profiling/common/regs_common.h"

namespace perfetto {
namespace profiling {

// Returns the ArchEnum for the currently running architecture.
inline ArchEnum CurrentArch() {
#if defined(__arm__)
  return kArchArm;
#elif defined(__aarch64__)
  return kArchArm64;
#elif defined(__i386__)
  return kArchX86;
#elif defined(__x86_64__)
  return kArchX86_64;
#elif defined(__riscv)
  return kArchRiscv64;
#else
  return kArchUnknown;
#endif
}

// Captures all CPU registers into the provided buffer. The buffer must be
// at least kMaxRegisterDataSize bytes. The register layout matches
// libunwindstack's AsmGetRegs for wire protocol compatibility.
//
// On ARM, AArch64, and RISC-V, this is implemented via inline assembly.
// On x86/x86_64, this is implemented in a standalone .S file.

#if defined(__arm__)

inline __attribute__((__always_inline__)) void AsmGetRegs(void* reg_data) {
  asm volatile(
#if defined(__thumb__)
      ".align 2\n"
      "bx pc\n"
      "nop\n"
#endif
      ".code 32\n"
      "stmia %[base], {r0-r12}\n"
      "add r2, %[base], #52\n"
      "mov r3, r13\n"
      "mov r4, r14\n"
      "mov r5, r15\n"
      "stmia r2, {r3-r5}\n"
#if defined(__thumb__)
      "orr %[base], pc, #1\n"
      "bx %[base]\n"
#endif
      : [base] "+r"(reg_data)
      :
      : "r2", "r3", "r4", "r5", "memory");
}

#elif defined(__aarch64__)

inline __attribute__((__always_inline__)) void AsmGetRegs(void* reg_data) {
  asm volatile(
      "1:\n"
      "stp x0, x1, [%[base], #0]\n"
      "stp x2, x3, [%[base], #16]\n"
      "stp x4, x5, [%[base], #32]\n"
      "stp x6, x7, [%[base], #48]\n"
      "stp x8, x9, [%[base], #64]\n"
      "stp x10, x11, [%[base], #80]\n"
      "stp x12, x13, [%[base], #96]\n"
      "stp x14, x15, [%[base], #112]\n"
      "stp x16, x17, [%[base], #128]\n"
      "stp x18, x19, [%[base], #144]\n"
      "stp x20, x21, [%[base], #160]\n"
      "stp x22, x23, [%[base], #176]\n"
      "stp x24, x25, [%[base], #192]\n"
      "stp x26, x27, [%[base], #208]\n"
      "stp x28, x29, [%[base], #224]\n"
      "str x30, [%[base], #240]\n"
      "mov x12, sp\n"
      "adr x13, 1b\n"
      "stp x12, x13, [%[base], #248]\n"
      : [base] "+r"(reg_data)
      :
      : "x12", "x13", "memory");
}

#elif defined(__riscv)

inline __attribute__((__always_inline__)) void AsmGetRegs(void* reg_data) {
  asm volatile(
      "1:\n"
      "sd ra, 8(%[base])\n"
      "sd sp, 16(%[base])\n"
      "sd gp, 24(%[base])\n"
      "sd tp, 32(%[base])\n"
      "sd t0, 40(%[base])\n"
      "sd t1, 48(%[base])\n"
      "sd t2, 56(%[base])\n"
      "sd s0, 64(%[base])\n"
      "sd s1, 72(%[base])\n"
      "sd a0, 80(%[base])\n"
      "sd a1, 88(%[base])\n"
      "sd a2, 96(%[base])\n"
      "sd a3, 104(%[base])\n"
      "sd a4, 112(%[base])\n"
      "sd a5, 120(%[base])\n"
      "sd a6, 128(%[base])\n"
      "sd a7, 136(%[base])\n"
      "sd s2, 144(%[base])\n"
      "sd s3, 152(%[base])\n"
      "sd s4, 160(%[base])\n"
      "sd s5, 168(%[base])\n"
      "sd s6, 176(%[base])\n"
      "sd s7, 184(%[base])\n"
      "sd s8, 192(%[base])\n"
      "sd s9, 200(%[base])\n"
      "sd s10, 208(%[base])\n"
      "sd s11, 216(%[base])\n"
      "sd t3, 224(%[base])\n"
      "sd t4, 232(%[base])\n"
      "sd t5, 240(%[base])\n"
      "sd t6, 248(%[base])\n"
      "csrr t1, 0xc22\n"
      "sd t1, 256(%[base])\n"
      "la t1, 1b\n"
      "sd t1, 0(%[base])\n"
      : [base] "+r"(reg_data)
      :
      : "t1", "memory");
}

#elif defined(__i386__) || defined(__x86_64__)

// Implemented in regs_local_x86_64.S / regs_local_x86.S.
extern "C" void PerfettoAsmGetRegs(void* regs);

inline __attribute__((__always_inline__)) void AsmGetRegs(void* reg_data) {
  PerfettoAsmGetRegs(reg_data);
}

#endif

// Returns the byte offset within the register data buffer where the stack
// pointer is stored, or -1 on unknown architectures. Used on RISC-V where
// __builtin_frame_address(0) may not produce a usable stack pointer.
inline ssize_t GetStackPointerOffset(ArchEnum arch) {
  switch (arch) {
    case kArchX86:
      return 7 * static_cast<ssize_t>(sizeof(uint32_t));  // X86_REG_SP
    case kArchX86_64:
      return 7 * static_cast<ssize_t>(sizeof(uint64_t));  // X86_64_REG_SP
    case kArchArm:
      return 13 * static_cast<ssize_t>(sizeof(uint32_t));  // ARM_REG_SP
    case kArchArm64:
      return 31 * static_cast<ssize_t>(sizeof(uint64_t));  // ARM64_REG_SP
    case kArchRiscv64:
      return 2 * static_cast<ssize_t>(sizeof(uint64_t));  // RISCV64_REG_SP
    case kArchUnknown:
    case kArchMips:
    case kArchMips64:
      return -1;
  }
  return -1;
}

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_COMMON_REGS_LOCAL_H_
