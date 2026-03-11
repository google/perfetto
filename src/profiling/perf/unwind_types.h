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

#ifndef SRC_PROFILING_PERF_UNWIND_TYPES_H_
#define SRC_PROFILING_PERF_UNWIND_TYPES_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "src/profiling/common/regs_common.h"

namespace perfetto {
namespace profiling {

// Backend-agnostic representation of a single unwound frame.
// Replaces unwindstack::FrameData in the pipeline.
struct UnwindFrame {
  uint64_t rel_pc = 0;
  uint64_t pc = 0;
  uint64_t sp = 0;
  std::string function_name;
  uint64_t function_offset = 0;

  // Mapping info (from /proc/pid/maps or equivalent).
  std::string map_name;
  uint64_t map_start = 0;
  uint64_t map_end = 0;
  uint64_t map_exact_offset = 0;
  uint64_t map_elf_start_offset = 0;
  uint64_t map_load_bias = 0;
  std::string build_id;
};

// Backend-agnostic unwinding error codes.
// Replaces unwindstack::ErrorCode in the pipeline.
enum class UnwindErrorCode : uint8_t {
  kNone = 0,
  kMemoryInvalid,
  kUnwindInfo,
  kUnsupported,
  kInvalidMap,
  kMaxFramesExceeded,
  kRepeatedFrame,
  kInvalidElf,
  kSystemCall,
  kThreadTimeout,
  kThreadDoesNotExist,
  kBadArch,
  kMapsParse,
  kInvalidParameter,
  kPtraceCall,
};

std::string StringifyUnwindError(UnwindErrorCode e);

// Backend-agnostic register state parsed from a perf sample or client dump.
// The unwinding backend creates and interprets this.
struct RegisterData {
  // Stack pointer value (needed for stack overlay memory).
  uint64_t sp = 0;
  // Raw register values. The layout depends on the source:
  // - Perf samples: packed in perf register mask order.
  // - Client dumps (ParseClientRegs): in AsmGetRegs/libunwindstack native
  //   order, with |arch| set to the client architecture.
  std::vector<uint64_t> regs;
  // PERF_SAMPLE_REGS_ABI_* value indicating the ABI of the sampled process.
  uint64_t abi = 0;
  // When set to a value other than kArchUnknown, indicates that |regs| are in
  // client (AsmGetRegs/libunwindstack native) order rather than perf order.
  ArchEnum arch = ArchEnum::kArchUnknown;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_UNWIND_TYPES_H_
