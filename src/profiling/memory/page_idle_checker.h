/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_PROFILING_MEMORY_PAGE_IDLE_CHECKER_H_
#define SRC_PROFILING_MEMORY_PAGE_IDLE_CHECKER_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/base/scoped_file.h"

namespace perfetto {
namespace profiling {

uint64_t GetFirstPageShare(uint64_t addr, size_t size);
uint64_t GetLastPageShare(uint64_t addr, size_t size);

class PageIdleChecker {
 public:
  PageIdleChecker(base::ScopedFile pagemap_fd, base::ScopedFile kpageflags_fd)
      : pagemap_fd_(std::move(pagemap_fd)),
        kpageflags_fd_(std::move(kpageflags_fd)) {}

  // Return number of bytes of allocation of size bytes starting at alloc that
  // are on unreferenced pages.
  // Return -1 on error.
  int64_t OnIdlePage(uint64_t addr, size_t size);

 private:
  base::ScopedFile pagemap_fd_;
  base::ScopedFile kpageflags_fd_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_PAGE_IDLE_CHECKER_H_
