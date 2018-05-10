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

#include "perfetto/base/page_allocator.h"

#include "perfetto/base/build_config.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#else
#include <sys/mman.h>
#endif

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

namespace perfetto {
namespace base {

namespace {

constexpr size_t kGuardSize = kPageSize;

// static
PageAllocator::UniquePtr AllocateInternal(size_t size, bool unchecked) {
  PERFETTO_DCHECK(size % kPageSize == 0);
  size_t outer_size = size + kGuardSize * 2;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  void* ptr = VirtualAlloc(nullptr, outer_size, MEM_RESERVE, PAGE_NOACCESS);
  if (!ptr && unchecked)
    return nullptr;
  PERFETTO_CHECK(ptr);
  char* usable_region = reinterpret_cast<char*>(ptr) + kGuardSize;
  void* res = VirtualAlloc(usable_region, size, MEM_COMMIT, PAGE_READWRITE);
  PERFETTO_CHECK(res);
#else
  void* ptr = mmap(nullptr, outer_size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
  if (ptr == MAP_FAILED && unchecked)
    return nullptr;
  PERFETTO_CHECK(ptr && ptr != MAP_FAILED);
  char* usable_region = reinterpret_cast<char*>(ptr) + kGuardSize;
  int res = mprotect(ptr, kGuardSize, PROT_NONE);
  res |= mprotect(usable_region + size, kGuardSize, PROT_NONE);
  PERFETTO_CHECK(res == 0);
#endif
  return PageAllocator::UniquePtr(usable_region, PageAllocator::Deleter(size));
}

}  // namespace

PageAllocator::Deleter::Deleter() : Deleter(0) {}
PageAllocator::Deleter::Deleter(size_t size) : size_(size) {}

void PageAllocator::Deleter::operator()(void* ptr) const {
  if (!ptr)
    return;
  PERFETTO_CHECK(size_);
  char* start = reinterpret_cast<char*>(ptr) - kGuardSize;
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  BOOL res = VirtualFree(start, 0, MEM_RELEASE);
  PERFETTO_CHECK(res != 0);
#else
  const size_t outer_size = size_ + kGuardSize * 2;
  int res = munmap(start, outer_size);
  PERFETTO_CHECK(res == 0);
#endif
}

// static
PageAllocator::UniquePtr PageAllocator::Allocate(size_t size) {
  return AllocateInternal(size, false /*unchecked*/);
}

// static
PageAllocator::UniquePtr PageAllocator::AllocateMayFail(size_t size) {
  return AllocateInternal(size, true /*unchecked*/);
}

// static
bool PageAllocator::AdviseDontNeed(void* p, size_t size) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Discarding pages on Windows has more CPU cost than is justified for the
  // possible memory savings.
  return false;
#else
  // http://man7.org/linux/man-pages/man2/madvise.2.html
  int res = madvise(p, size, MADV_DONTNEED);
  PERFETTO_DCHECK(res == 0);
  return true;
#endif
}

}  // namespace base
}  // namespace perfetto
