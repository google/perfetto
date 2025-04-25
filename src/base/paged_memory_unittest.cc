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

#include "perfetto/ext/base/paged_memory.h"

#include <stdint.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/utils.h"
#include "src/base/test/vm_test_utils.h"
#include "test/gtest_and_gmock.h"

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) && \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) &&   \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
#include <sys/resource.h>
#endif

namespace perfetto {
namespace base {
namespace {

TEST(PagedMemoryTest, Basic) {
  const size_t kNumPages = 10;
  const size_t kSize = GetSysPageSize() * kNumPages;
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  void* ptr_raw = nullptr;
#endif
  {
    PagedMemory mem = PagedMemory::Allocate(kSize);
    ASSERT_TRUE(mem.IsValid());
    ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(mem.Get()) % GetSysPageSize());
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    ptr_raw = mem.Get();
#endif
    for (size_t i = 0; i < kSize / sizeof(uint64_t); i++)
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(mem.Get()) + i));

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    ASSERT_TRUE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
    ASSERT_TRUE(mem.AdviseDontNeed(ptr_raw, kSize));

    // Make sure the pages were removed from the working set.
    ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  // Freed memory is necessarily not mapped in to the process.
  ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif
}

TEST(PagedMemoryTest, SubPageGranularity) {
  const size_t kSize = GetSysPageSize() + 1024;
  PagedMemory mem = PagedMemory::Allocate(kSize);
  ASSERT_TRUE(mem.IsValid());
  ASSERT_EQ(0u, reinterpret_cast<uintptr_t>(mem.Get()) % GetSysPageSize());
  void* ptr_raw = mem.Get();
  for (size_t i = 0; i < kSize / sizeof(uint64_t); i++) {
    auto* ptr64 = reinterpret_cast<volatile uint64_t*>(ptr_raw) + i;
    ASSERT_EQ(0u, *ptr64);
    *ptr64 = i;
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  // Do an AdviseDontNeed on the whole range, which is NOT an integer multiple
  // of the page size. The initial page must be cleared. The remaining 1024
  // might or might not be cleared depending on the OS implementation.
  ASSERT_TRUE(mem.AdviseDontNeed(ptr_raw, kSize));
  ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, GetSysPageSize()));
  for (size_t i = 0; i < GetSysPageSize() / sizeof(uint64_t); i++) {
    auto* ptr64 = reinterpret_cast<volatile uint64_t*>(ptr_raw) + i;
    ASSERT_EQ(0u, *ptr64);
  }
#endif
}

TEST(PagedMemoryTest, Uncommitted) {
  const size_t kNumPages = 4096;
  const size_t kSize = GetSysPageSize() * kNumPages;
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  char* ptr_raw = nullptr;
#endif
  {
    PagedMemory mem = PagedMemory::Allocate(kSize, PagedMemory::kDontCommit);
    ASSERT_TRUE(mem.IsValid());
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    ptr_raw = reinterpret_cast<char*>(mem.Get());
#endif

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    // Windows only commits the first 1024 pages.
    constexpr size_t kMappedSize = 4096 * 1024;

    for (size_t i = 0; i < kMappedSize / sizeof(uint64_t); i++)
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(mem.Get()) + i));

    ASSERT_TRUE(vm_test_utils::IsMapped(ptr_raw, kMappedSize));

    // Next page shouldn't be mapped.
    ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw + kMappedSize, 4096));
    EXPECT_DEATH_IF_SUPPORTED({ ptr_raw[kMappedSize] = 'x'; }, ".*");

    // Commit the remaining pages.
    mem.EnsureCommitted(kSize);

    for (size_t i = kMappedSize / sizeof(uint64_t);
         i < kSize / sizeof(uint64_t); i++) {
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(mem.Get()) + i));
    }
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
    // Fuchsia doesn't yet support paging. So this should be a no-op.
    mem.EnsureCommitted(kSize);
    for (size_t i = 0; i < kSize / sizeof(uint64_t); i++)
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(mem.Get()) + i));
#else
    // Linux only maps on access.
    ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));

    // This should not have any effect.
    mem.EnsureCommitted(kSize);
    ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));

    for (size_t i = 0; i < kSize / sizeof(uint64_t); i++)
      ASSERT_EQ(0u, *(reinterpret_cast<uint64_t*>(mem.Get()) + i));
    ASSERT_TRUE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif
  }

#if !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA)
  // Freed memory is necessarily not mapped in to the process.
  ASSERT_FALSE(vm_test_utils::IsMapped(ptr_raw, kSize));
#endif
}

#if defined(ADDRESS_SANITIZER)
TEST(PagedMemoryTest, AccessUncommittedMemoryTriggersASAN) {
  EXPECT_DEATH_IF_SUPPORTED(
      {
        const size_t kNumPages = 2000;
        const size_t kSize = GetSysPageSize() * kNumPages;
        PagedMemory mem =
            PagedMemory::Allocate(kSize, PagedMemory::kDontCommit);
        ASSERT_TRUE(mem.IsValid());
        char* ptr_raw = reinterpret_cast<char*>(mem.Get());
        // Only the first 1024 pages are mapped.
        const size_t kMappedSize = GetSysPageSize() * 1024;
        ptr_raw[kMappedSize] = 'x';
        abort();
      },
      "AddressSanitizer: .*");
}
#endif  // ADDRESS_SANITIZER

TEST(PagedMemoryTest, GuardRegions) {
  const size_t kSize = GetSysPageSize();
  PagedMemory mem = PagedMemory::Allocate(kSize);
  ASSERT_TRUE(mem.IsValid());
  volatile char* raw = reinterpret_cast<char*>(mem.Get());
  EXPECT_DEATH_IF_SUPPORTED({ raw[-1] = 'x'; }, ".*");
  EXPECT_DEATH_IF_SUPPORTED({ raw[kSize] = 'x'; }, ".*");
}

// Disable this on:
// MacOS: because it doesn't seem to have an equivalent rlimit to bound mmap().
// Fuchsia: doesn't support rlimit.
// Sanitizers: they seem to try to shadow mmaped memory and fail due to OOMs.
#if !PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE) &&                                  \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_WIN) &&                                    \
    !PERFETTO_BUILDFLAG(PERFETTO_OS_FUCHSIA) && !defined(ADDRESS_SANITIZER) && \
    !defined(LEAK_SANITIZER) && !defined(THREAD_SANITIZER) &&                  \
    !defined(MEMORY_SANITIZER)
// Glibc headers hit this on RLIMIT_ macros.
#pragma GCC diagnostic push
#if defined(__clang__)
#pragma GCC diagnostic ignored "-Wdisabled-macro-expansion"
#endif
TEST(PagedMemoryTest, Unchecked) {
  const size_t kMemLimit = 256 * 1024 * 1024l;
  struct rlimit limit{kMemLimit, kMemLimit};
  // ASSERT_EXIT here is to spawn the test in a sub-process and avoid
  // propagating the setrlimit() to other test units in case of failure.
  ASSERT_EXIT(
      {
        ASSERT_EQ(0, setrlimit(RLIMIT_AS, &limit));
        auto mem = PagedMemory::Allocate(kMemLimit * 2, PagedMemory::kMayFail);
        ASSERT_FALSE(mem.IsValid());
        // Use _exit() instead of exit() to avoid calling destructors on child
        // process death, which may interfere with the parent process's test
        // launcher expectations.
        _exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#pragma GCC diagnostic pop
#endif

}  // namespace
}  // namespace base
}  // namespace perfetto
