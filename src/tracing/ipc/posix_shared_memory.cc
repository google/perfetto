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

#include "src/tracing/ipc/posix_shared_memory.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_FREEBSD) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <memory>
#include <utility>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/temp_file.h"
#include "src/tracing/ipc/memfd.h"

namespace perfetto {

namespace {
int kFileSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;
}  // namespace

// static
base::StatusOr<std::unique_ptr<PosixSharedMemory>> PosixSharedMemory::Create(
    size_t size) {
  if (size == 0)
    return base::ErrStatus("Invalid shmem size 0");

  base::ScopedFile fd =
      CreateMemfd("perfetto_shmem", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  const bool is_memfd = !!fd;
  if (!is_memfd) {
#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
    // In-tree builds only allow memfd, so the service can verify the seals.
    return base::ErrStatus("memfd_create() failed: %s", strerror(errno));
#else
    ASSIGN_OR_RETURN(
        base::TempFile temp_file,
        base::TempFile::Create(base::TempFile::LinkMode::kUnlinked));
    fd = temp_file.ReleaseFD();
#endif
  }

  if (ftruncate(fd.get(), static_cast<off_t>(size)) != 0)
    return base::ErrStatus("Couldn't resize shmem FD: %s", strerror(errno));

  if (is_memfd && fcntl(fd.get(), F_ADD_SEALS, kFileSeals) != 0)
    return base::ErrStatus("Couldn't seal shmem FD: %s", strerror(errno));

  return MapFD(std::move(fd), size);
}

// static
std::unique_ptr<PosixSharedMemory> PosixSharedMemory::AttachToFd(
    base::ScopedFile fd,
    bool require_seals_if_supported,
    size_t max_size) {
  if (!fd)
    return nullptr;
  bool requires_seals = require_seals_if_supported;

#if PERFETTO_BUILDFLAG(PERFETTO_ANDROID_BUILD)
  // In-tree kernels all support memfd.
  PERFETTO_CHECK(HasMemfdSupport());
#else
  // In out-of-tree builds, we only require seals if the kernel supports memfd.
  if (requires_seals)
    requires_seals = HasMemfdSupport();
#endif

  if (requires_seals) {
    // If the system supports memfd, we require a sealed memfd.
    int res = fcntl(*fd, F_GET_SEALS);
    if (res == -1 || (res & kFileSeals) != kFileSeals) {
      PERFETTO_PLOG("Couldn't verify file seals on shmem FD");
      return nullptr;
    }
  }

  struct stat stat_buf = {};
  if (fstat(fd.get(), &stat_buf) != 0) {
    PERFETTO_PLOG("Couldn't stat shmem FD");
    return nullptr;
  }
  if (stat_buf.st_size <= 0 ||
      static_cast<uint64_t>(stat_buf.st_size) > max_size) {
    PERFETTO_ELOG("Invalid shmem FD size %" PRId64 " (max %zu)",
                  static_cast<int64_t>(stat_buf.st_size), max_size);
    return nullptr;
  }
  auto memory = MapFD(std::move(fd), static_cast<size_t>(stat_buf.st_size));
  if (!memory.ok()) {
    PERFETTO_ELOG("%s", memory.status().c_message());
    return nullptr;
  }
  return std::move(*memory);
}

// static
base::StatusOr<std::unique_ptr<PosixSharedMemory>> PosixSharedMemory::MapFD(
    base::ScopedFile fd,
    size_t size) {
  PERFETTO_DCHECK(fd);
  PERFETTO_DCHECK(size > 0);
  void* start =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
  if (start == MAP_FAILED)
    return base::ErrStatus("Couldn't map shmem FD: %s", strerror(errno));
  return std::unique_ptr<PosixSharedMemory>(
      new PosixSharedMemory(start, size, std::move(fd)));
}

PosixSharedMemory::PosixSharedMemory(void* start,
                                     size_t size,
                                     base::ScopedFile fd)
    : start_(start), size_(size), fd_(std::move(fd)) {}

PosixSharedMemory::~PosixSharedMemory() {
  munmap(start(), size());
}

PosixSharedMemory::Factory::~Factory() {}

std::unique_ptr<SharedMemory> PosixSharedMemory::Factory::CreateSharedMemory(
    size_t size) {
  return PosixSharedMemory::Create(size).ValueOrDie();
}

}  // namespace perfetto

#endif  // OS_LINUX || OS_ANDROID || OS_APPLE
