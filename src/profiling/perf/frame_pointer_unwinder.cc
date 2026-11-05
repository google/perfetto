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
#include "src/profiling/perf/frame_pointer_unwinder.h"

#include <cstring>

#include "perfetto/base/logging.h"

namespace perfetto {
namespace profiling {

void FramePointerUnwinder::Unwind() {
  if (!IsArchSupported()) {
    PERFETTO_ELOG("Unsupported architecture: %d", static_cast<int>(arch_));
    last_error_ = UnwindErrorCode::kUnsupported;
    return;
  }

  if (state_ == nullptr) {
    PERFETTO_ELOG("No unwind state provided");
    last_error_ = UnwindErrorCode::kInvalidParam;
    return;
  }

  PERFETTO_DCHECK(stack_size_ > 0u);

  frames_.reserve(max_frames_);
  ClearErrors();
  TryUnwind();
}

void FramePointerUnwinder::TryUnwind() {
  uint64_t fp = 0;
  switch (arch_) {
    case CpuArch::ARM64:
      fp = (*regs_)[ARM64_REG_FP];
      break;
    case CpuArch::X86_64:
      fp = (*regs_)[X86_64_REG_FP];
      break;
    case CpuArch::RISCV64:
      fp = (*regs_)[RISCV64_REG_FP];
      break;
    case CpuArch::UNKNOWN:
    case CpuArch::ARM:
    case CpuArch::X86:
        // not supported
        ;
  }
  uint64_t sp = regs_->sp();
  uint64_t pc = regs_->pc();

  for (size_t i = 0; i < max_frames_; i++) {
    if (!IsFrameValid(fp, sp))
      return;

    // Looks up the mapping for |pc|, computes the ELF-relative PC and return
    // address adjustment, and optionally resolves the symbol name.
    FrameData frame =
        UnwindEngine::Get()->BuildFrameFromPc(state_, pc, resolve_names_);
    if (!frame.map_info) {
      last_error_ = UnwindErrorCode::kInvalidMap;
      return;
    }

    frames_.push_back(std::move(frame));
    fp = DecodeFrame(fp, &pc, &sp);
  }
}

uint64_t FramePointerUnwinder::DecodeFrame(uint64_t fp,
                                           uint64_t* next_pc,
                                           uint64_t* next_sp) {
  // Ensure there's not a stack overflow.
  if (__builtin_add_overflow(fp, sizeof(uint64_t) * 2, next_sp))
    return 0;

  size_t offset = static_cast<size_t>(fp - regs_->sp());
  uint64_t next_fp = 0;

  std::memcpy(&next_fp, stack_data_ + offset, sizeof(uint64_t));
  std::memcpy(next_pc, stack_data_ + offset + sizeof(uint64_t),
              sizeof(uint64_t));

  return next_fp;
}

bool FramePointerUnwinder::IsFrameValid(uint64_t fp, uint64_t sp) {
  uint64_t align_mask = 0;
  switch (arch_) {
    case CpuArch::ARM64:
      align_mask = 0x1;
      break;
    case CpuArch::X86_64:
      align_mask = 0xf;
      break;
    case CpuArch::RISCV64:
      align_mask = 0x7;
      break;
    case CpuArch::UNKNOWN:
    case CpuArch::ARM:
    case CpuArch::X86:
        // not supported
        ;
  }

  if (fp == 0 || fp <= sp)
    return false;

  // Ensure there's space on the stack to read two values: the caller's
  // frame pointer and the return address.
  uint64_t result;
  if (__builtin_add_overflow(fp, sizeof(uint64_t) * 2, &result))
    return false;

  return result <= stack_end_ && (fp & align_mask) == 0;
}

}  // namespace profiling
}  // namespace perfetto
