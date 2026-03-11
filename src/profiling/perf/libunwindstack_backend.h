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

#ifndef SRC_PROFILING_PERF_LIBUNWINDSTACK_BACKEND_H_
#define SRC_PROFILING_PERF_LIBUNWINDSTACK_BACKEND_H_

#include <memory>
#include <optional>

#include "src/profiling/common/unwind_support.h"
#include "src/profiling/perf/unwind_backend.h"

namespace perfetto {
namespace profiling {

class LibunwindstackProcessState : public ProcessUnwindState {
 public:
  explicit LibunwindstackProcessState(base::ScopedFile maps_fd,
                                      base::ScopedFile mem_fd);
  ~LibunwindstackProcessState() override;

  UnwindingMetadata& metadata() { return metadata_; }

 private:
  UnwindingMetadata metadata_;
};

class LibunwindstackBackend : public UnwindBackend {
 public:
  LibunwindstackBackend();
  ~LibunwindstackBackend() override;

  std::unique_ptr<ProcessUnwindState> CreateProcessState(
      base::ScopedFile maps_fd,
      base::ScopedFile mem_fd) override;

  UnwindResult Unwind(ProcessUnwindState* state,
                      const RegisterData& regs,
                      uint64_t stack_base,
                      const char* stack,
                      size_t stack_size,
                      size_t max_frames) override;

  UnwindResult UnwindFramePointers(ProcessUnwindState* state,
                                   const RegisterData& regs,
                                   uint64_t stack_base,
                                   const char* stack,
                                   size_t stack_size,
                                   size_t max_frames) override;

  void ReparseMaps(ProcessUnwindState* state) override;

  void ResetCache() override;

  uint64_t PerfUserRegsMask() override;

  std::optional<RegisterData> ParsePerfRegs(const char** data) override;

  std::optional<RegisterData> ParseClientRegs(ArchEnum arch,
                                              void* raw_data) override;

 private:
  // Converts libunwindstack FrameData + build_id to our UnwindFrame.
  static UnwindFrame ConvertFrame(const unwindstack::FrameData& frame,
                                  const std::string& build_id);

  // Converts libunwindstack ErrorCode to our UnwindErrorCode.
  static UnwindErrorCode ConvertErrorCode(unwindstack::ErrorCode code);

  // Reconstructs unwindstack::Regs from our RegisterData.
  static std::unique_ptr<unwindstack::Regs> ToUnwindstackRegs(
      const RegisterData& regs);
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_LIBUNWINDSTACK_BACKEND_H_
