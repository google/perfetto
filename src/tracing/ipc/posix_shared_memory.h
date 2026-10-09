/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef SRC_TRACING_IPC_POSIX_SHARED_MEMORY_H_
#define SRC_TRACING_IPC_POSIX_SHARED_MEMORY_H_

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)

#include <stddef.h>

#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/tracing/core/shared_memory.h"

namespace perfetto {

// Implements the SharedMemory and its factory for the posix-based transport.
class PosixSharedMemory : public SharedMemory {
 public:
  class Factory : public SharedMemory::Factory {
   public:
    ~Factory() override;
    std::unique_ptr<SharedMemory> CreateSharedMemory(size_t) override;
  };

  // Creates a new SHM region and returns nullptr on failure.
  // Seals each memfd, so its size cannot change and it takes no more seals.
  // If |require_sealed_memfd| is true, requires a sealed memfd, otherwise
  // permits a temporary file fallback.
  static std::unique_ptr<PosixSharedMemory> Create(size_t size,
                                                   bool require_sealed_memfd);

  // Mmaps an existing SHM region for shared read/write access.
  // - Takes ownership of the descriptor, also on failure.
  // - Returns nullptr if descriptor validation fails.
  // - Also returns nullptr if mmap() fails (e.g. OOM).
  // - Rejects an empty file, and a file larger than |max_size|, before mmap().
  // - If |require_seals_if_supported| is true and the system supports
  //   memfd_create, requires F_SEAL_SEAL, F_SEAL_GROW and F_SEAL_SHRINK
  //   before it reads the size.
  static std::unique_ptr<PosixSharedMemory> AttachToFd(
      base::ScopedFile,
      bool require_seals_if_supported,
      size_t max_size);

  ~PosixSharedMemory() override;

  int fd() const { return fd_.get(); }

  // SharedMemory implementation.
  using SharedMemory::start;  // Equal priority to const and non-const versions
  const void* start() const override { return start_; }
  size_t size() const override { return size_; }

 private:
  // Returns nullptr if mmap() fails.
  static std::unique_ptr<PosixSharedMemory> MapFD(base::ScopedFile, size_t);

  PosixSharedMemory(void* start, size_t size, base::ScopedFile);
  PosixSharedMemory(const PosixSharedMemory&) = delete;
  PosixSharedMemory& operator=(const PosixSharedMemory&) = delete;

  void* const start_;
  const size_t size_;
  base::ScopedFile fd_;
};

}  // namespace perfetto

#endif  // OS_LINUX || OS_ANDROID || OS_APPLE
#endif  // SRC_TRACING_IPC_POSIX_SHARED_MEMORY_H_
