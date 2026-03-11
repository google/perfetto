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

#ifndef SRC_PROFILING_PERF_UNWIND_BACKEND_H_
#define SRC_PROFILING_PERF_UNWIND_BACKEND_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/common/regs_common.h"
#include "src/profiling/perf/unwind_types.h"

namespace perfetto {
namespace profiling {

// Opaque per-process unwinding state, owned by the backend.
class ProcessUnwindState {
 public:
  virtual ~ProcessUnwindState();
};

// Abstract interface for an unwinding backend.
// Allows swapping between libunwindstack and libunwind at build time.
class UnwindBackend {
 public:
  virtual ~UnwindBackend();

  struct UnwindResult {
    UnwindErrorCode error_code = UnwindErrorCode::kNone;
    uint64_t warnings = 0;
    std::vector<UnwindFrame> frames;
  };

  // Create per-process unwinding state from /proc/pid/{maps,mem} fds.
  virtual std::unique_ptr<ProcessUnwindState> CreateProcessState(
      base::ScopedFile maps_fd,
      base::ScopedFile mem_fd) = 0;

  // Perform DWARF-based stack unwinding.
  // |stack_base| is the address in the target process corresponding to the
  // start of |stack|. For perf samples this is regs.sp; for heapprofd it's
  // the stack_pointer from the wire protocol metadata.
  virtual UnwindResult Unwind(ProcessUnwindState* state,
                              const RegisterData& regs,
                              uint64_t stack_base,
                              const char* stack,
                              size_t stack_size,
                              size_t max_frames) = 0;

  // Perform frame-pointer-based stack unwinding.
  // Backends that don't support this should return kUnsupported.
  virtual UnwindResult UnwindFramePointers(ProcessUnwindState* state,
                                           const RegisterData& regs,
                                           uint64_t stack_base,
                                           const char* stack,
                                           size_t stack_size,
                                           size_t max_frames) = 0;

  // Reparse /proc/pid/maps (e.g. after kInvalidMap error).
  virtual void ReparseMaps(ProcessUnwindState* state) = 0;

  // Reset/clear internal caches (e.g. ELF cache).
  virtual void ResetCache() = 0;

  // Get the perf register bitmask for the host architecture.
  virtual uint64_t PerfUserRegsMask() = 0;

  // Parse raw perf sample register data into RegisterData.
  // Returns std::nullopt for kernel threads (no userspace regs).
  // Advances |data| past the consumed bytes.
  virtual std::optional<RegisterData> ParsePerfRegs(const char** data) = 0;

  // Create RegisterData from a raw register byte dump produced by the
  // heapprofd client (via AsmGetRegs). The register layout in |raw_data|
  // matches the libunwindstack register format for the given arch.
  virtual std::optional<RegisterData> ParseClientRegs(ArchEnum arch,
                                                      void* raw_data) = 0;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_UNWIND_BACKEND_H_
