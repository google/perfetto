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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/string_view.h"

namespace perfetto {
namespace profiling {

// TODO(safayat): support 32-bit riscv and other architectures.
enum CpuArch : uint8_t {
  kUnknown = 0,
  kArm,
  kArm64,
  kX86,
  kX86_64,
  kRiscv64,
};

inline constexpr CpuArch CurrentCpuArch() {
#if PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_ARM64)
  return CpuArch::kArm64;
#elif PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_X86_64)
  return CpuArch::kX86_64;
#elif PERFETTO_BUILDFLAG(PERFETTO_ARCH_CPU_RISCV)
  return CpuArch::kRiscv64;
#elif defined(__arm__)
  return CpuArch::kArm;
#elif defined(__i386__)
  return CpuArch::kX86;
#else
  return CpuArch::kUnknown;
#endif
}

inline std::string ToString(CpuArch arch) {
  switch (arch) {
    case CpuArch::kUnknown:
      return "kUnknown";
    case CpuArch::kArm:
      return "kArm";
    case CpuArch::kArm64:
      return "kArm64";
    case CpuArch::kX86:
      return "kX86";
    case CpuArch::kX86_64:
      return "kX86_64";
    case CpuArch::kRiscv64:
      return "kRiscv64";
  }
  return "UNKNOWN";
}

enum class UnwindMode { kUnwindStack, kFramePointer, kKernelFramePointer };

enum class SymbolStatus : uint8_t {
  kNotAttempted = 0,
  kResolved = 1,
  kFailure = 2,
};

// Metadata for single memory mappings in proc/<pid>/maps
//
// Example `/proc/<pid>/maps` line:
//    [map_start]-[map_end]  [perm] [offset] [dev] [inode]  [map_name]
//   7f7c18042000-7f7c1806a000 r--p 00000000 fd:01 37890545 <binary_file>
//   7f7c1806a000-7f7c181d3000 r-xp 00028000 fd:01 37890545 <binary_file>
struct MapInfo {
  // ELF GNU build-id for offline symbolization
  std::string build_id;
  std::string map_name;
  uint64_t map_start = 0;
  uint64_t map_end = 0;
  // r-xp segment offset
  uint64_t map_offset = 0;
  // first r--p segment start offset
  uint64_t map_start_offset = 0;
  // ELF load bias (`p_vaddr - p_offset` of the executable `PT_LOAD` segment),
  // needed to convert `rel_pc` to the ELF symbol table's virtual address.
  uint64_t map_load_bias = 0;
};

struct FrameData {
  uint64_t pc = 0;
  uint64_t rel_pc = 0;
  uint64_t sp = 0;

  // Symbol resolution
  std::string function_name;
  uint64_t function_offset = 0;
  SymbolStatus symbol_status = SymbolStatus::kNotAttempted;

  // Future improvement: cache these to avoid per-frame allocation.
  std::optional<MapInfo> map_info;
};

enum class UnwindErrorCode : uint8_t {
  kNone = 0,
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

struct UnwindErrorData {
  UnwindErrorCode code;
  uint64_t address;  // Only valid when code is ERROR_MEMORY_INVALID.
                     // Indicates the failing address.
};

// A bit map of warnings, multiple warnings can be set at the same time.
enum UnwindWarning : uint64_t {
  kNone = 0,
  kDexPcNotInMap = 0x1,  // A dex pc was found, but it doesn't exist
                         // in any valid map.
};

inline std::string ToString(UnwindErrorCode error) {
  switch (error) {
    case UnwindErrorCode::kNone:
      return "NONE";
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
  uint64_t warnings = 0;
  std::vector<FrameData> frames;

  bool success() const { return error_code == UnwindErrorCode::kNone; }

  UnwindResult() = default;
  UnwindResult(UnwindErrorCode e, uint64_t w, std::vector<FrameData> f)
      : error_code(e), warnings(w), frames(std::move(f)) {}

  UnwindResult(const UnwindResult&) = delete;
  UnwindResult& operator=(const UnwindResult&) = delete;
  UnwindResult(UnwindResult&&) __attribute__((unused)) = default;
  UnwindResult& operator=(UnwindResult&&) = default;
};

}  // namespace profiling
}  // namespace perfetto
#endif  // SRC_PROFILING_UNWIND_UNWIND_TYPES_H_
