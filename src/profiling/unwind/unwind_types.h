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

#ifndef SRC_PROFILING_UNWIND_UNWIND_TYPES_H_
#define SRC_PROFILING_UNWIND_UNWIND_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace perfetto {
namespace profiling {

enum class UnwindMode { kUnwindStack, kFramePointer, kKernelFramePointer };

enum class SymbolStatus : uint8_t {
  kNotAttempted = 0,
  kResolved = 1,
  kFailure = 2,
};

// Represents the additional ELF information associated with a PC.
struct UnwoundMapping {
  // `map_name`, `map_start`, `map_end`, `map_offset`: populated directly from
  // the /proc/<pid>/maps entry covering the frame's PC.
  // Example `/proc/<pid>/maps` line:
  //    [map_start]-[map_end]  [perm] [offset] [dev] [inode]  [map_name]
  //   7f7c18042000-7f7c1806a000 r--p 00000000 fd:01 37890545 <binary_file>
  //   7f7c1806a000-7f7c181d3000 r-xp 00028000 fd:01 37890545 <binary_file>
  std::string map_name;
  uint64_t map_start = 0;
  uint64_t map_end = 0;
  uint64_t map_offset = 0;

  // For split ELFs mapped across an initial
  // read-only (r--p) mapping (containing the ELF header) followed by an
  // executable (r-xp) mapping, the file offset of the initial mapping (0 if the
  // mapping starts at the beginning of the ELF).
  uint64_t map_start_offset = 0;

  // populated by parsing the underlying ELF binary
  // (p_vaddr - p_offset) of the executable PT_LOAD segment.
  // Needed to convert `rel_pc` to the ELF symbol table's virtual address.
  uint64_t map_load_bias = 0;

  // raw .note.gnu.build-id bytes;
  // 0 / empty if the mapping is not a readable ELF or lacks a build ID).
  std::string build_id;
};

struct FrameData {
  uint64_t pc = 0;
  uint64_t rel_pc = 0;
  uint64_t sp = 0;

  // Symbol resolution
  std::string function_name;
  uint64_t function_offset = 0;
  SymbolStatus symbol_status = SymbolStatus::kNotAttempted;

  // TODO(safayat): cache these to avoid per-frame allocation.
  std::optional<UnwoundMapping> map_info;
};

enum class UnwindErrorCode : uint8_t {
  kNone = 0,
  // Represents libunwindstack Warning::kDexPcNotInMap
  kDexPcNotInMap,
  kMemoryInvalid,
  kInvalidUnwindInfo,
  kUnsupported,
  kInvalidMap,
  kMaxFramesExceeded,
  kRepeatedFrame,
  kInvalidElf,
  kThreadDoesNotExist,
  kThreadTimeout,
  kSystemCall,
  kBadArch,
  kMapParseError,
  kInvalidParam,
  kPtraceCall,
  kUnknownError,
};

inline std::string ToString(UnwindErrorCode error) {
  switch (error) {
    case UnwindErrorCode::kNone:
      return "NONE";
    case UnwindErrorCode::kDexPcNotInMap:
      return "DEX_PC_NOT_IN_MAP";
    case UnwindErrorCode::kMemoryInvalid:
      return "MEMORY_INVALID";
    case UnwindErrorCode::kInvalidUnwindInfo:
      return "UNWIND_INFO";
    case UnwindErrorCode::kUnsupported:
      return "UNSUPPORTED";
    case UnwindErrorCode::kInvalidMap:
      return "INVALID_MAP";
    case UnwindErrorCode::kMaxFramesExceeded:
      return "MAX_FRAME_EXCEEDED";
    case UnwindErrorCode::kRepeatedFrame:
      return "REPEATED_FRAME";
    case UnwindErrorCode::kInvalidElf:
      return "INVALID_ELF";
    case UnwindErrorCode::kSystemCall:
      return "SYSTEM_CALL";
    case UnwindErrorCode::kThreadDoesNotExist:
      return "THREAD_DOES_NOT_EXIST";
    case UnwindErrorCode::kThreadTimeout:
      return "THREAD_TIMEOUT";
    case UnwindErrorCode::kBadArch:
      return "BAD_ARCH";
    case UnwindErrorCode::kMapParseError:
      return "MAPS_PARSE";
    case UnwindErrorCode::kInvalidParam:
      return "INVALID_PARAMETER";
    case UnwindErrorCode::kPtraceCall:
      return "PTRACE_CALL";
    case UnwindErrorCode::kUnknownError:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

struct UnwindResult {
  UnwindErrorCode error_code = UnwindErrorCode::kNone;
  // callstack frames are ordered from the leaf to root.
  std::vector<FrameData> frames;

  bool success() const { return error_code == UnwindErrorCode::kNone; }

  UnwindResult() = default;
  UnwindResult(UnwindErrorCode e, std::vector<FrameData> f)
      : error_code(e), frames(std::move(f)) {}

  UnwindResult(const UnwindResult&) = delete;
  UnwindResult& operator=(const UnwindResult&) = delete;
  UnwindResult(UnwindResult&&) = default;
  UnwindResult& operator=(UnwindResult&&) = default;
};

}  // namespace profiling
}  // namespace perfetto
#endif  // SRC_PROFILING_UNWIND_UNWIND_TYPES_H_
