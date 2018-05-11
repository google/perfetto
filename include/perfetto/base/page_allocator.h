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

#ifndef INCLUDE_PERFETTO_BASE_PAGE_ALLOCATOR_H_
#define INCLUDE_PERFETTO_BASE_PAGE_ALLOCATOR_H_

#include <memory>

namespace perfetto {
namespace base {

class PageAllocator {
 public:
  class Deleter {
   public:
    Deleter();
    explicit Deleter(size_t);
    void operator()(void*) const;

   private:
    size_t size_;
  };

  using UniquePtr = std::unique_ptr<void, Deleter>;

  // Allocates |size| bytes using mmap(MAP_ANONYMOUS). The returned pointer is
  // guaranteed to be page-aligned and the memory is guaranteed to be zeroed.
  // |size| must be a multiple of 4KB (a page size). Crashes if the underlying
  // mmap() fails.
  static UniquePtr Allocate(size_t size);

  // Like the above, but returns a nullptr if the mmap() fails (e.g., if out
  // of virtual address space).
  static UniquePtr AllocateMayFail(size_t size);

  // Hint to the OS that the memory range is not needed and can be discarded.
  // The memory remains accessible and its contents may be retained, or they
  // may be zeroed. This function may be a NOP on some platforms. Returns true
  // if implemented.
  static bool AdviseDontNeed(void* p, size_t size);
};

}  // namespace base
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_BASE_PAGE_ALLOCATOR_H_
