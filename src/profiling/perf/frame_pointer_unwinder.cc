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

#include <cinttypes>
#include <cstdint>
#include <cstring>

#include "perfetto/base/logging.h"
#include "src/profiling/unwind/perf_regs.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

void FramePointerUnwinder::Unwind() {
  if (!IsArchSupported()) {
    PERFETTO_ELOG("Unsupported architecture: %s", ToString(arch_).c_str());
    last_error_.code = UnwindErrorCode::kUnsupported;
    return;
  }

  if (context_ == nullptr) {
    PERFETTO_ELOG("No unwind context provided");
    last_error_.code = UnwindErrorCode::kInvalidMap;
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
    case CpuArch::kArm64:
      fp = regs_->raw_data[PERF_REG_ARM64_X29];
      break;
    case CpuArch::kX86_64:
      fp = regs_->raw_data[PERF_REG_X86_BP];
      break;
    case CpuArch::kRiscv64:
      fp = regs_->raw_data[PERF_REG_RISCV_S0];
      break;
    case CpuArch::kUnknown:
    case CpuArch::kArm:
    case CpuArch::kX86:
        // not supported
        ;
  }
  uint64_t sp = regs_->sp;
  uint64_t pc = regs_->pc;

  for (size_t i = 0; i < max_frames_; i++) {
    if (!IsFrameValid(fp, sp))
      return;

    std::optional<FrameData> frame =
        context_->BuildFrameFromPc(pc, resolve_names_);
    if (!frame) {
      last_error_.code = UnwindErrorCode::kInvalidMap;
      return;
    }

    frame->sp = sp;
    frames_.push_back(std::move(*frame));
    // move to the next frame
    fp = DecodeFrame(fp, &pc, &sp);
  }
}

uint64_t FramePointerUnwinder::DecodeFrame(uint64_t fp,
                                           uint64_t* next_pc,
                                           uint64_t* next_sp) {
  // Ensure there's not a stack overflow.
  if (__builtin_add_overflow(fp, sizeof(uint64_t) * 2, next_sp))
    return 0;

  size_t offset = static_cast<size_t>(fp - regs_->sp);
  uint64_t next_fp = 0;

  std::memcpy(&next_fp, stack_data_ + offset, sizeof(uint64_t));
  std::memcpy(next_pc, stack_data_ + offset + sizeof(uint64_t),
              sizeof(uint64_t));

  return next_fp;
}

bool FramePointerUnwinder::IsFrameValid(uint64_t fp, uint64_t sp) {
  uint64_t align_mask = 0;
  switch (arch_) {
    case CpuArch::kArm64:
      align_mask = 0x1;
      break;
    case CpuArch::kX86_64:
      align_mask = 0xf;
      break;
    case CpuArch::kRiscv64:
      align_mask = 0x7;
      break;
    case CpuArch::kUnknown:
    case CpuArch::kArm:
    case CpuArch::kX86:
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
