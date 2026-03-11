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

#include "src/profiling/perf/unwind_types.h"

namespace perfetto {
namespace profiling {

std::string StringifyUnwindError(UnwindErrorCode e) {
  switch (e) {
    case UnwindErrorCode::kNone:
      return "NONE";
    case UnwindErrorCode::kMemoryInvalid:
      return "MEMORY_INVALID";
    case UnwindErrorCode::kUnwindInfo:
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
    case UnwindErrorCode::kThreadTimeout:
      return "THREAD_TIMEOUT";
    case UnwindErrorCode::kThreadDoesNotExist:
      return "THREAD_DOES_NOT_EXIST";
    case UnwindErrorCode::kBadArch:
      return "BAD_ARCH";
    case UnwindErrorCode::kMapsParse:
      return "MAPS_PARSE";
    case UnwindErrorCode::kInvalidParameter:
      return "INVALID_PARAMETER";
    case UnwindErrorCode::kPtraceCall:
      return "PTRACE_CALL";
  }
  return "UNKNOWN";
}

}  // namespace profiling
}  // namespace perfetto
