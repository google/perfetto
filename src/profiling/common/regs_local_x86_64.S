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

// Register layout matches libunwindstack's AsmGetRegsX86_64.S for
// wire protocol compatibility. Register order:
//   rax, rdx, rcx, rbx, rsi, rdi, rbp, rsp,
//   r8, r9, r10, r11, r12, r13, r14, r15, rip
// (17 registers * 8 bytes = 136 bytes)

  .text
  .global PerfettoAsmGetRegs
  .balign 16
  .type PerfettoAsmGetRegs, @function
PerfettoAsmGetRegs:
  .cfi_startproc
  movq %rax, (%rdi)
  movq %rdx, 8(%rdi)
  movq %rcx, 16(%rdi)
  movq %rbx, 24(%rdi)
  movq %rsi, 32(%rdi)
  movq %rdi, 40(%rdi)
  movq %rbp, 48(%rdi)

  /* RSP */
  lea 8(%rsp), %rax
  movq %rax, 56(%rdi)

  movq %r8, 64(%rdi)
  movq %r9, 72(%rdi)
  movq %r10, 80(%rdi)
  movq %r11, 88(%rdi)
  movq %r12, 96(%rdi)
  movq %r13, 104(%rdi)
  movq %r14, 112(%rdi)
  movq %r15, 120(%rdi)

  /* RIP */
  movq (%rsp), %rax
  movq %rax, 128(%rdi)
  ret

  .cfi_endproc
  .size PerfettoAsmGetRegs, .-PerfettoAsmGetRegs
