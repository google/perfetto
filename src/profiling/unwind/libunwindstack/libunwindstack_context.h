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

#ifndef SRC_PROFILING_UNWIND_LIBUNWINDSTACK_LIBUNWINDSTACK_CONTEXT_H_
#define SRC_PROFILING_UNWIND_LIBUNWINDSTACK_LIBUNWINDSTACK_CONTEXT_H_

#include <unwindstack/DexFiles.h>
#include <unwindstack/JitDebug.h>
#include <unwindstack/Maps.h>
#include <unwindstack/Memory.h>
#include <unwindstack/Unwinder.h>
#include <cstdint>

#include "perfetto/base/time.h"
#include "perfetto/ext/base/scoped_file.h"
#include "src/profiling/unwind/unwind_types.h"
#include "src/profiling/unwind/unwind_context.h"

namespace perfetto {
namespace profiling {

class FDMaps : public unwindstack::Maps {
 public:
  explicit FDMaps(base::ScopedFile fd);

  FDMaps(const FDMaps&) = delete;
  FDMaps& operator=(const FDMaps&) = delete;

  FDMaps(FDMaps&& m) : Maps(std::move(m)) { fd_ = std::move(m.fd_); }

  FDMaps& operator=(FDMaps&& m) {
    if (&m != this)
      fd_ = std::move(m.fd_);
    Maps::operator=(std::move(m));
    return *this;
  }

  virtual ~FDMaps() override = default;

  bool Parse() override;
  void Reset();

 private:
  base::ScopedFile fd_;
};

class FDMemory : public unwindstack::Memory {
 public:
  explicit FDMemory(base::ScopedFile mem_fd);
  size_t Read(uint64_t addr, void* dst, size_t size) override;

 private:
  base::ScopedFile mem_fd_;
};

// Overlays size bytes pointed to by stack for addresses in [sp, sp + size).
// Addresses outside of that range are read from mem_fd, which should be an fd
// that opened /proc/$pid/mem.
class StackOverlayMemory : public unwindstack::Memory {
 public:
  StackOverlayMemory(std::shared_ptr<unwindstack::Memory> mem,
                     uint64_t sp,
                     const uint8_t* stack,
                     size_t size);
  size_t Read(uint64_t addr, void* dst, size_t size) override;

 private:
  std::shared_ptr<unwindstack::Memory> mem_;
  const uint64_t sp_;
  const uint64_t stack_end_;
  const uint8_t* const stack_;
};

struct UnwindingMetadata {
  UnwindingMetadata(base::ScopedFile maps_fd, base::ScopedFile mem_fd);

  // move-only
  UnwindingMetadata(const UnwindingMetadata&) = delete;
  UnwindingMetadata& operator=(const UnwindingMetadata&) = delete;

  UnwindingMetadata(UnwindingMetadata&&) = default;
  UnwindingMetadata& operator=(UnwindingMetadata&&) = default;

  void ReparseMaps();

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  unwindstack::JitDebug* GetJitDebug(unwindstack::ArchEnum arch);
  unwindstack::DexFiles* GetDexFiles(unwindstack::ArchEnum arch);
#endif

  FDMaps fd_maps;
  // The API of libunwindstack expects shared_ptr for Memory.
  std::shared_ptr<unwindstack::Memory> fd_mem;
  uint64_t reparses = 0;
  base::TimeMillis last_maps_reparse_time{0};
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  std::unique_ptr<unwindstack::JitDebug> jit_debug;
  std::unique_ptr<unwindstack::DexFiles> dex_files;
#endif
};

class LibunwindstackContext : public UnwindContext {
 public:
  LibunwindstackContext(base::ScopedFile maps_fd, base::ScopedFile mem_fd);
  ~LibunwindstackContext() override;

  // TODO: could these be done on the main unwinder?
  // but that is abstract....
  uint64_t reparses() const override { return metadata_.reparses; }
  void ReparseMaps() override;
  void ResetMaps() override;

  std::optional<FrameData> BuildFrameFromPc(uint64_t pc,
                                            bool resolve_names = true) override;

  UnwindResult Unwind(const UnwindInputSample& /* sample */,
                      const UnwindOptions& /*options*/ = {}) override;
  // UnwindingMetadata

 private:
  UnwindingMetadata metadata_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_UNWIND_LIBUNWINDSTACK_LIBUNWINDSTACK_CONTEXT_H_
