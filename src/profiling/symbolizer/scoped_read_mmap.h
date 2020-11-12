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

#ifndef SRC_PROFILING_SYMBOLIZER_SCOPED_READ_MMAP_H_
#define SRC_PROFILING_SYMBOLIZER_SCOPED_READ_MMAP_H_

#include "perfetto/ext/base/scoped_file.h"

namespace perfetto {
namespace profiling {

class ScopedReadMmap {
 public:
  ScopedReadMmap(const char* fname, size_t length);
  virtual ~ScopedReadMmap();

  void* operator*() { return ptr_; }

  bool IsValid();

 private:
  size_t length_;
  void* ptr_;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  void* file_ = nullptr;
  void* map_ = nullptr;
#else
  base::ScopedFile fd_;
#endif
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_SYMBOLIZER_SCOPED_READ_MMAP_H_
