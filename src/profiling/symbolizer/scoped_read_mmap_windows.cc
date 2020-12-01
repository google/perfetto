
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

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)

#include <Windows.h>

namespace perfetto {
namespace profiling {

ScopedReadMmap::ScopedReadMmap(const char* fName, size_t length)
    : length_(length), ptr_(nullptr) {
  file_ = CreateFileA(fName, GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file_ == INVALID_HANDLE_VALUE) {
    PERFETTO_DLOG("Failed to open file: %s", fName);
    return;
  }
  map_ = CreateFileMapping(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (map_ == INVALID_HANDLE_VALUE) {
    PERFETTO_DLOG("Failed to mmap file");
    return;
  }
  ptr_ = MapViewOfFile(map_, FILE_MAP_READ, 0, 0, length_);
  if (ptr_ == nullptr) {
    PERFETTO_DLOG("Failed to map view of file");
  }
}

ScopedReadMmap::~ScopedReadMmap() {
  if (ptr_ != nullptr) {
    UnmapViewOfFile(ptr_);
  }
  if (map_ != nullptr && map_ != INVALID_HANDLE_VALUE) {
    CloseHandle(map_);
  }
  if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) {
    CloseHandle(file_);
  }
}

bool ScopedReadMmap::IsValid() {
  return ptr_ != nullptr;
}

}  // namespace profiling
}  // namespace perfetto

#endif  // PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
