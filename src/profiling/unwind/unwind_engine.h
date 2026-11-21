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

#ifndef SRC_PROFILING_UNWIND_UNWIND_ENGINE_H_
#define SRC_PROFILING_UNWIND_UNWIND_ENGINE_H_

#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/cpu_registers.h"
#include "src/profiling/unwind/unwind_types.h"

namespace perfetto {
namespace profiling {

constexpr size_t kUnwindingMaxFrames = 1000;

struct UnwindInputSample {
  const CpuRegisters* regs = nullptr;
  const uint8_t* stack_data = nullptr;
  size_t stack_size = 0;
};

struct UnwindOptions {
  size_t max_frames = kUnwindingMaxFrames;
  // If true, will resolve function names online.
  // Can be set to false to do offline symbolization, which reduces
  // unwinding time.
  bool resolve_names = true;
  const std::vector<std::string>* skip_initial_maps = nullptr;
};

// Per-process unwinding state (parsed /proc/<pid>/maps, /proc/<pid>/mem fd,
// backend-specific caches). Owned by the client; opaque to callers and
// created by the active unwinder backend.
class ProcessUnwindState {
 public:
  static std::unique_ptr<ProcessUnwindState> Create(base::ScopedFile maps_fd,
                                                    base::ScopedFile mem_fd);

  virtual ~ProcessUnwindState();

  virtual void ReparseMaps() = 0;
  virtual void ResetMaps() = 0;
};

// Stateless unwinding engine. All per-process mutable state lives in
// ProcessUnwindState, so a single instance can be shared across threads.
class UnwindEngine {
 public:
  // Returns the process-wide engine for the active unwinder backend.
  static UnwindEngine* Get();

  virtual ~UnwindEngine();

  // Constructs a single FrameData for a raw program counter |pc| without
  // performing stack unwinding.
  // - Looks up the containing memory mapping for |pc|.
  // - Computes the ELF-relative PC and return-address adjustment.
  // - Optionally resolves the symbol name.
  // If |pc| does not fall within any known mapping, returns a FrameData with
  // an empty |map_info|.
  virtual FrameData BuildFrameFromPc(ProcessUnwindState* state,
                                     uint64_t pc,
                                     bool resolve_names = true) = 0;

  // Backend-specific DWARF unwinding for the process described by |state|.
  virtual UnwindResult Unwind(ProcessUnwindState* state,
                              const UnwindInputSample& sample,
                              const UnwindOptions& options = {}) = 0;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_UNWIND_ENGINE_H_
