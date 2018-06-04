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

#include <stdint.h>

#include "gtest/gtest.h"
#include "perfetto/base/build_config.h"
#include "src/base/test/vm_test_utils.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <sys/resource.h>
#endif

namespace perfetto {
namespace base {
namespace {

TEST(PageAllocatorTest, Basic) {
  const size_t kNumPages = 10;
  const size_t kSize = 4096 * kNumPages;
  void* ptr_raw = nullptr;
  {
    PageAllocator::UniquePtr ptr = PageAllocator::Allocate(kSize);
    ASSERT_TRUE(ptr);
    ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(ptr.get()) % 4096);
    ptr_raw = ptr.get();
    for (size_t i = 0; i < kSize / sizeof(uint64_t); i++)
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(ptr.get()) + i));

    ASSERT_TRUE(vm_test_utils::IsMapped(ptr_raw, kSize));

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    ASSERT_TRUE(PageAllocator::AdviseDontNeed(ptr_raw, kSize));

    // Make sure the pages were removed from the working set.
    ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif
  }

  // Freed memory is necessarily not mapped in to the process.
  ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));
}

TEST(PageAllocatorTest, GuardRegions) {
  const size_t kSize = 4096;
  PageAllocator::UniquePtr ptr = PageAllocator::Allocate(kSize);
  ASSERT_TRUE(ptr);
  volatile char* raw = reinterpret_cast<char*>(ptr.get());
  EXPECT_DEATH({ raw[-1] = 'x'; }, ".*");
  EXPECT_DEATH({ raw[kSize] = 'x'; }, ".*");
}

// Disable this on:
// MacOS: because it doesn't seem to have an equivalent rlimit to bound mmap().
// Sanitizers: they seem to try to shadow mmaped memory and fail due to OOMs.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_MACOSX) &&                             \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) && !defined(ADDRESS_SANITIZER) && \
    !defined(LEAK_SANITIZER) && !defined(THREAD_SANITIZER) &&              \
    !defined(MEMORY_SANITIZER)
// Glibc headers hit this on RLIMIT_ macros.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
TEST(PageAllocatorTest, Unchecked) {
  const size_t kMemLimit = 256 * 1024 * 1024l;
  struct rlimit limit {
    kMemLimit, kMemLimit
  };
  // ASSERT_EXIT here is to spawn the test in a sub-process and avoid
  // propagating the setrlimit() to other test units in case of failure.
  ASSERT_EXIT(
      {
        ASSERT_EQ(0, setrlimit(RLIMIT_AS, &limit));
        auto ptr = PageAllocator::AllocateMayFail(kMemLimit * 2);
        ASSERT_FALSE(ptr);
        exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#pragma GCC diagnostic pop
#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
