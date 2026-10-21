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

#ifndef SRC_PROFILING_UNWIND_UNWIND_CONTEXT_H_
#define SRC_PROFILING_UNWIND_UNWIND_CONTEXT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
  bool stack_maxed = false;
};

struct UnwindOptions {
  size_t max_frames = kUnwindingMaxFrames;
  // if true will resovle function names online.
  // if set to false will do offline symbolization. which may help to reduce
  // unwinding time.
  bool resolve_names = true;
  std::vector<std::string> skip_initial_maps;
};

class UnwindContext {
 public:
  static std::unique_ptr<UnwindContext> Create(base::ScopedFile maps_fd,
                                               base::ScopedFile mem_fd);

  virtual ~UnwindContext();

  virtual void ReparseMaps() = 0;
  virtual void ResetMaps() = 0;

  virtual FrameData BuildFrameFromPc(uint64_t /* pc */,
                                     bool resolve_names = true) = 0;
  // Backend-specific DWARF unwinding for this process
  virtual UnwindResult Unwind(const UnwindInputSample& /* sample */,
                              const UnwindOptions& /*options*/ = {}) = 0;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_UNWIND_CONTEXT_H_
