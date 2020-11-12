/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/profiling/symbolizer/scoped_read_mmap.h"

#include "perfetto/base/logging.h"

#include <sys/mman.h>

namespace perfetto {
namespace profiling {

ScopedReadMmap::ScopedReadMmap(const char* fname, size_t length)
    : length_(length), fd_(base::OpenFile(fname, O_RDONLY)) {
  if (!fd_) {
    PERFETTO_PLOG("Failed to open %s", fname);
    return;
  }
  ptr_ = mmap(nullptr, length, PROT_READ, MAP_PRIVATE, *fd_, 0);
}

ScopedReadMmap::~ScopedReadMmap() {
  if (ptr_ != MAP_FAILED)
    munmap(ptr_, length_);
}

bool ScopedReadMmap::IsValid() {
  return ptr_ != MAP_FAILED;
}

}  // namespace profiling
}  // namespace perfetto
