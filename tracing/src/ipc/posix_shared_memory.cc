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

#include "tracing/src/ipc/posix_shared_memory.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <utility>

#include "perfetto_base/logging.h"

namespace perfetto {

// static
std::unique_ptr<PosixSharedMemory> PosixSharedMemory::Create(size_t size) {
  // TODO: use memfd_create on Linux/Android if the kernel supports it (needs
  // syscall.h, there is no glibc wrapper). If not, on Android fallback on
  // ashmem and on Linux fallback on /dev/shm/perfetto-whatever.
  FILE* tmp_file = tmpfile();
  PERFETTO_CHECK(tmp_file);
  base::ScopedFile fd(fileno(tmp_file));
  PERFETTO_CHECK(fd);
  int res = ftruncate(fd.get(), static_cast<off_t>(size));
  PERFETTO_CHECK(res == 0);
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
  void* start = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
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
