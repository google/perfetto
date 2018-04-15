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

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <utility>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/temp_file.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace perfetto {

// static
std::unique_ptr<PosixSharedMemory> PosixSharedMemory::Create(size_t size) {
  base::ScopedFile fd;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  bool is_memfd = false;
  fd.reset(static_cast<int>(syscall(__NR_memfd_create, "perfetto_shmem",
                                    MFD_CLOEXEC | MFD_ALLOW_SEALING)));
  is_memfd = !!fd;

  if (!fd) {
    // TODO: if this fails on Android we should fall back on ashmem.
    PERFETTO_DPLOG("memfd_create() failed");
  }
#endif

  if (!fd)
    fd = base::TempFile::CreateUnlinked().ReleaseFD();

  PERFETTO_CHECK(fd);
  int res = ftruncate(fd.get(), static_cast<off_t>(size));
  PERFETTO_CHECK(res == 0);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  if (is_memfd) {
    res = fcntl(*fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    PERFETTO_DCHECK(res == 0);
  }
#endif
  return MapFD(std::move(fd), size);
}

// static
std::unique_ptr<PosixSharedMemory> PosixSharedMemory::AttachToFd(
    base::ScopedFile fd) {
  struct stat stat_buf = {};
  int res = fstat(fd.get(), &stat_buf);
  PERFETTO_CHECK(res == 0 && stat_buf.st_size > 0);
  return MapFD(std::move(fd), static_cast<size_t>(stat_buf.st_size));
}

// static
std::unique_ptr<PosixSharedMemory> PosixSharedMemory::MapFD(base::ScopedFile fd,
                                                            size_t size) {
  PERFETTO_DCHECK(fd);
  PERFETTO_DCHECK(size > 0);
  void* start =
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
  PERFETTO_CHECK(start != MAP_FAILED);
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
  return PosixSharedMemory::Create(size);
}

}  // namespace perfetto
