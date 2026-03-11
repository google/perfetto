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

#ifndef SRC_PROFILING_PERF_LIBUNWIND_BACKEND_H_
#define SRC_PROFILING_PERF_LIBUNWIND_BACKEND_H_

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/perf/unwind_backend.h"

namespace perfetto {
namespace profiling {

// Per-process state for libunwind-based unwinding.
class LibunwindProcessState : public ProcessUnwindState {
 public:
  struct MapEntry {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t offset = 0;
    std::string name;
  };

  LibunwindProcessState(base::ScopedFile maps_fd, base::ScopedFile mem_fd);
  ~LibunwindProcessState() override;

  int mem_fd() const { return *mem_fd_; }
  const std::vector<MapEntry>& maps() const { return maps_; }

  void ReparseMaps();

 private:
  void ParseMaps();

  base::ScopedFile maps_fd_;
  base::ScopedFile mem_fd_;
  std::vector<MapEntry> maps_;
};

// UnwindBackend implementation using nongnu.org libunwind for DWARF-based
// stack unwinding on Linux. Uses the remote unwinding API
// (unw_create_addr_space, unw_init_remote, unw_step) with custom accessors
// that read from the sampled stack and /proc/pid/mem.
class LibunwindBackend : public UnwindBackend {
 public:
  LibunwindBackend();
  ~LibunwindBackend() override;

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
  // Looks up the MapEntry for the given PC.
  static const LibunwindProcessState::MapEntry* FindMap(
      const LibunwindProcessState* state,
      uint64_t pc);

  // Reads the ELF build ID from a mapped file.
  static std::string ReadBuildId(const std::string& path);

  // Gets the ELF load bias for a given mapping.
  static uint64_t GetLoadBias(const std::string& path);
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PERF_LIBUNWIND_BACKEND_H_
