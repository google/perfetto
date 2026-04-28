/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/core/util/paged_vector.h"

#include <cstddef>

#include "src/trace_processor/core/util/page_store.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

TEST(PagedVectorTest, DefaultConstructor) {
  PagedVector<int> vec;
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_EQ(vec.capacity(), 0u);
  EXPECT_TRUE(vec.empty());
  EXPECT_TRUE(vec.IsSinglePage());
}

TEST(PagedVectorTest, PushBack) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.push_back(42);
  vec.push_back(123);
  vec.push_back(7);

  EXPECT_EQ(vec.size(), 3u);
  EXPECT_FALSE(vec.empty());
  EXPECT_EQ(vec[0], 42);
  EXPECT_EQ(vec[1], 123);
  EXPECT_EQ(vec[2], 7);
}

TEST(PagedVectorTest, CapacityGrowth) {
  constexpr size_t kInitialCapacity = 64;
  auto vec = PagedVector<int>::CreateWithCapacity(kInitialCapacity);

  EXPECT_EQ(vec.capacity(), kInitialCapacity);
  for (size_t i = 0; i < kInitialCapacity; ++i) {
    vec.push_back(static_cast<int>(i));
  }
  EXPECT_EQ(vec.size(), kInitialCapacity);

  vec.push_back(100);
  EXPECT_GE(vec.capacity(), kInitialCapacity * 2);
  EXPECT_EQ(vec.size(), kInitialCapacity + 1);

  for (size_t i = 0; i < kInitialCapacity; ++i) {
    EXPECT_EQ(vec[i], static_cast<int>(i));
  }
  EXPECT_EQ(vec[kInitialCapacity], 100);
}

TEST(PagedVectorTest, MinimumCapacityGrowth) {
  auto vec = PagedVector<int>::CreateWithCapacity(1);
  vec.push_back(42);
  EXPECT_EQ(vec.size(), 1u);
  vec.push_back(43);
  EXPECT_GE(vec.capacity(), 64u);
  EXPECT_EQ(vec[0], 42);
  EXPECT_EQ(vec[1], 43);
}

TEST(PagedVectorTest, LargeGrowth) {
  auto vec = PagedVector<int>::CreateWithCapacity(2);
  constexpr size_t kNumElements = 1000;
  for (size_t i = 0; i < kNumElements; ++i) {
    vec.push_back(static_cast<int>(i));
  }
  EXPECT_EQ(vec.size(), kNumElements);
  for (size_t i = 0; i < kNumElements; ++i) {
    EXPECT_EQ(vec[i], static_cast<int>(i));
  }
}

TEST(PagedVectorTest, DifferentDataTypes) {
  {
    auto vec = PagedVector<double>::CreateWithCapacity(4);
    vec.push_back(3.14);
    vec.push_back(2.71);
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_DOUBLE_EQ(vec[0], 3.14);
    EXPECT_DOUBLE_EQ(vec[1], 2.71);
  }
  {
    struct Point {
      int x;
      int y;
      bool operator==(const Point& other) const {
        return x == other.x && y == other.y;
      }
    };
    auto vec = PagedVector<Point>::CreateWithCapacity(4);
    vec.push_back({1, 2});
    vec.push_back({3, 4});
    EXPECT_EQ(vec.size(), 2u);
    EXPECT_EQ(vec[0], (Point{1, 2}));
    EXPECT_EQ(vec[1], (Point{3, 4}));
  }
}

TEST(PagedVectorTest, RangeBasedForLoop) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.push_back(10);
  vec.push_back(20);
  vec.push_back(30);

  int sum = 0;
  for (int value : vec) {
    sum += value;
  }
  EXPECT_EQ(sum, 60);
}

TEST(PagedVectorTest, DataAccessor) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.push_back(1);
  vec.push_back(2);
  vec.push_back(3);

  int* data = vec.data();
  EXPECT_EQ(data[0], 1);
  EXPECT_EQ(data[1], 2);
  EXPECT_EQ(data[2], 3);

  data[1] = 42;
  EXPECT_EQ(vec[1], 42);
}

TEST(PagedVectorTest, ResizeShrink) {
  auto vec = PagedVector<int>::CreateWithCapacity(64);
  for (size_t i = 0; i < 10; ++i) {
    vec.push_back(static_cast<int>(i * 10));
  }
  EXPECT_EQ(vec.size(), 10u);

  vec.resize(5);
  EXPECT_EQ(vec.size(), 5u);
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_EQ(vec[i], static_cast<int>(i * 10));
  }
}

TEST(PagedVectorTest, ResizeGrow) {
  auto vec = PagedVector<int>::CreateWithCapacity(64);
  vec.push_back(1);
  vec.push_back(2);
  vec.push_back(3);

  vec.resize(100);
  EXPECT_EQ(vec.size(), 100u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
}

TEST(PagedVectorTest, ShrinkToFit) {
  auto vec = PagedVector<int>::CreateWithCapacity(1024);
  vec.push_back(1);
  vec.push_back(2);
  vec.push_back(3);
  EXPECT_GE(vec.capacity(), 1024u);

  vec.shrink_to_fit();
  EXPECT_EQ(vec.size(), 3u);
  EXPECT_EQ(vec[0], 1);
  EXPECT_EQ(vec[1], 2);
  EXPECT_EQ(vec[2], 3);
}

TEST(PagedVectorTest, Clear) {
  auto vec = PagedVector<int>::CreateWithCapacity(64);
  vec.push_back(1);
  vec.push_back(2);
  vec.clear();
  EXPECT_EQ(vec.size(), 0u);
  EXPECT_TRUE(vec.empty());
  EXPECT_TRUE(vec.IsSinglePage());
}

TEST(PagedVectorTest, PushBackMultiple) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.push_back_multiple(7, 100);
  EXPECT_EQ(vec.size(), 100u);
  for (size_t i = 0; i < 100; ++i) {
    EXPECT_EQ(vec[i], 7);
  }
}

// ---------------------------------------------------------------------------
// Multi-page tests
// ---------------------------------------------------------------------------

TEST(PagedVectorTest, AutoSealSplitsIntoPages) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.SetPageBytesForTesting(5 * sizeof(int));
  for (int i = 0; i < 4; ++i) {
    vec.push_back(i);
  }
  EXPECT_TRUE(vec.IsSinglePage());
  EXPECT_EQ(vec.num_pages(), 1u);

  // 5th push hits the threshold; tail auto-seals into a frozen page.
  vec.push_back(4);
  EXPECT_FALSE(vec.IsSinglePage());
  EXPECT_EQ(vec.num_pages(), 2u);  // 1 frozen + empty tail
  EXPECT_EQ(vec.size(), 5u);

  // Logical access still works.
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }

  // Append more; another auto-seal at i=9.
  for (int i = 5; i < 10; ++i) {
    vec.push_back(i);
  }
  EXPECT_EQ(vec.size(), 10u);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }
  EXPECT_EQ(vec.num_pages(), 3u);  // 2 frozen + empty tail
}

TEST(PagedVectorTest, AutoSealMultiplePages) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.SetPageBytesForTesting(3 * sizeof(int));
  // 9 pushes -> auto-seal at every 3rd (i=2, 5, 8) -> 3 frozen + empty tail.
  for (int i = 0; i < 9; ++i) {
    vec.push_back(i);
  }
  EXPECT_EQ(vec.size(), 9u);
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }
  EXPECT_EQ(vec.num_pages(), 4u);  // 3 frozen + empty tail
}

TEST(PagedVectorTest, PageViewIteration) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.SetPageBytesForTesting(7 * sizeof(int));
  for (int i = 0; i < 12; ++i) {
    vec.push_back(i);
  }
  // Auto-seal at i=6 (1 frozen page of 7) then 5 more in tail.
  EXPECT_EQ(vec.num_pages(), 2u);

  // Walk pages and reconstruct the logical sequence.
  std::vector<int> seen;
  for (uint32_t p = 0; p < vec.num_pages(); ++p) {
    auto view = vec.page(p);
    EXPECT_EQ(view.base_index, seen.size());
    for (uint64_t i = 0; i < view.length; ++i) {
      seen.push_back(view.data[i]);
    }
  }
  ASSERT_EQ(seen.size(), 12u);
  for (int i = 0; i < 12; ++i) {
    EXPECT_EQ(seen[static_cast<size_t>(i)], i);
  }
}

TEST(PagedVectorTest, ClearAfterAutoSealResetsToSinglePage) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.SetPageBytesForTesting(1 * sizeof(int));
  vec.push_back(1);  // immediately auto-seals.
  vec.push_back(2);  // immediately auto-seals.
  EXPECT_FALSE(vec.IsSinglePage());

  vec.clear();
  EXPECT_TRUE(vec.IsSinglePage());
  EXPECT_EQ(vec.size(), 0u);
}

// ---------------------------------------------------------------------------
// Eviction tests.
// ---------------------------------------------------------------------------

// Helper: creates a PageStore backed by InMemoryPageStorage and returns a
// borrowed pointer to that storage so tests can inspect entry_count etc.
struct InMemStore {
  PageStore store{std::make_unique<InMemoryPageStorage>()};
  InMemoryPageStorage* storage() {
    return static_cast<InMemoryPageStorage*>(store.storage());
  }
  // PagedVectors registered with this store must be destroyed before this
  // helper. Order test-local declarations accordingly.
};

TEST(PagedVectorTest, EvictAndRestoreInMemory) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(5 * sizeof(int));

  for (int i = 0; i < 13; ++i) {
    vec.push_back(i);
  }
  // Auto-seal at i=4, i=9 → 2 frozen pages of 5 + tail of 3.
  ASSERT_EQ(vec.num_pages(), 3u);
  EXPECT_TRUE(vec.IsPageResident(0));
  EXPECT_TRUE(vec.IsPageResident(1));

  vec.EvictPage(0);
  EXPECT_FALSE(vec.IsPageResident(0));
  EXPECT_TRUE(vec.IsPageResident(1));
  EXPECT_EQ(mem.storage()->entry_count(), 1u);

  // Random-access through operator[] auto-restores.
  EXPECT_EQ(vec[0], 0);
  EXPECT_TRUE(vec.IsPageResident(0));
  EXPECT_EQ(mem.storage()->entry_count(), 0u);  // handle released.

  // Re-evict and verify other elements still work.
  vec.EvictPage(0);
  vec.EvictPage(1);
  EXPECT_EQ(vec[7], 7);  // page 1 contains 5..9
  EXPECT_TRUE(vec.IsPageResident(1));
  EXPECT_FALSE(vec.IsPageResident(0));

  // Tail is always resident.
  EXPECT_EQ(vec[10], 10);
}

TEST(PagedVectorTest, EvictAndRestoreTempFile) {
  PageStore store{std::make_unique<TempFilePageStorage>()};
  auto vec = PagedVector<uint64_t>::CreateWithCapacity(8, &store);
  vec.SetPageBytesForTesting(100 * sizeof(uint64_t));

  constexpr uint64_t kCount = 200;
  for (uint64_t i = 0; i < kCount; ++i) {
    vec.push_back(i * 7u + 3u);
  }
  // Auto-seal at i=99, i=199 → 2 frozen pages + empty tail.
  ASSERT_EQ(vec.num_pages(), 3u);

  vec.EvictPage(0);
  EXPECT_FALSE(vec.IsPageResident(0));

  // Read elements; evicted page restores on demand.
  for (uint64_t i = 0; i < kCount; ++i) {
    EXPECT_EQ(vec[i], i * 7u + 3u) << "at i=" << i;
  }
  EXPECT_TRUE(vec.IsPageResident(0));
}

TEST(PagedVectorTest, ClearReleasesEvictedHandles) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(5 * sizeof(int));
  for (int i = 0; i < 5; ++i) {
    vec.push_back(i);
  }
  // Auto-sealed at i=4 → 1 frozen + empty tail.
  vec.EvictPage(0);
  EXPECT_EQ(mem.storage()->entry_count(), 1u);

  vec.clear();
  EXPECT_EQ(mem.storage()->entry_count(), 0u);
  EXPECT_TRUE(vec.IsSinglePage());
}

TEST(PagedVectorTest, EvictPageNoOpIfAlreadyEvicted) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(1 * sizeof(int));
  vec.push_back(7);  // immediately auto-seals.
  vec.EvictPage(0);
  EXPECT_EQ(mem.storage()->entry_count(), 1u);
  vec.EvictPage(0);  // no-op
  EXPECT_EQ(mem.storage()->entry_count(), 1u);
}

// ---------------------------------------------------------------------------
// Auto-seal tests.
// ---------------------------------------------------------------------------

TEST(PagedVectorTest, AutoSealEveryNElements) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  vec.SetPageBytesForTesting(10 * sizeof(int));

  for (int i = 0; i < 35; ++i) {
    vec.push_back(i);
  }
  // 35 elements, page-bytes = 10 elems -> auto-seal at i=9, 19, 29
  // -> 3 frozen pages + tail of 5.
  EXPECT_EQ(vec.num_pages(), 4u);
  EXPECT_EQ(vec.size(), 35u);
  for (int i = 0; i < 35; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }
}

TEST(PagedVectorTest, DefaultPageSizeKeepsSmallColumnSinglePage) {
  // With the production default of `kPageBytes` (4 MiB), a small column
  // never crosses the threshold and stays single-page.
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  for (int i = 0; i < 1000; ++i) {
    vec.push_back(i);
  }
  EXPECT_TRUE(vec.IsSinglePage());
  EXPECT_EQ(vec.num_pages(), 1u);
}

TEST(PagedVectorTest, AutoSealWithEviction) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(8 * sizeof(int));

  for (int i = 0; i < 30; ++i) {
    vec.push_back(i);
  }
  // 30 / 8 = 3 auto-seals (i=7, 15, 23) -> 3 frozen pages + tail of 6.
  ASSERT_EQ(vec.num_pages(), 4u);
  vec.EvictPage(0);
  vec.EvictPage(1);
  vec.EvictPage(2);
  EXPECT_EQ(mem.storage()->entry_count(), 3u);

  // Random-access reads transparently restore.
  for (int i = 0; i < 30; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }
}

// ---------------------------------------------------------------------------
// ScopedPage / WalkPages tests.
// ---------------------------------------------------------------------------

TEST(PagedVectorTest, ScopedPageRestoresEvicted) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(5 * sizeof(int));
  for (int i = 0; i < 5; ++i) {
    vec.push_back(i * 10);
  }
  // Auto-sealed at i=4 -> 1 frozen + empty tail.
  vec.EvictPage(0);
  EXPECT_FALSE(vec.IsPageResident(0));
  {
    ScopedPage<int> page(vec, 0);
    EXPECT_EQ(page.size(), 5u);
    EXPECT_EQ(page.base_index(), 0u);
    EXPECT_EQ(page.data()[3], 30);
  }
  EXPECT_TRUE(vec.IsPageResident(0));
}

TEST(PagedVectorTest, WalkPagesSinglePageZeroOverhead) {
  auto vec = PagedVector<int>::CreateWithCapacity(8);
  for (int i = 0; i < 5; ++i) {
    vec.push_back(i);
  }
  ASSERT_TRUE(vec.IsSinglePage());

  int sum = 0;
  uint32_t callback_count = 0;
  WalkPages(vec, 0, vec.size(), [&](const PagedVector<int>::PageView& view) {
    ++callback_count;
    for (uint64_t i = 0; i < view.length; ++i) {
      sum += view.data[i];
    }
  });
  EXPECT_EQ(callback_count, 1u);
  EXPECT_EQ(sum, 0 + 1 + 2 + 3 + 4);
}

TEST(PagedVectorTest, WalkPagesMultiPageWithEvictions) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(10 * sizeof(int));

  for (int i = 0; i < 30; ++i) {
    vec.push_back(i);
  }
  // Auto-seal at i=9, 19, 29 -> 3 frozen + empty tail.
  ASSERT_EQ(vec.num_pages(), 4u);

  vec.EvictPage(0);
  vec.EvictPage(1);
  EXPECT_FALSE(vec.IsPageResident(0));
  EXPECT_FALSE(vec.IsPageResident(1));

  // Walk full range — every non-empty page must be visited and restored.
  std::vector<int> seen;
  uint32_t cb = 0;
  WalkPages(vec, 0, vec.size(), [&](const PagedVector<int>::PageView& view) {
    ++cb;
    for (uint64_t i = 0; i < view.length; ++i) {
      seen.push_back(view.data[i]);
    }
  });
  // 3 frozen pages contain data; the empty tail is skipped (page_size==0).
  EXPECT_EQ(cb, 3u);
  ASSERT_EQ(seen.size(), 30u);
  for (int i = 0; i < 30; ++i) {
    EXPECT_EQ(seen[static_cast<size_t>(i)], i);
  }
  EXPECT_TRUE(vec.IsPageResident(0));
  EXPECT_TRUE(vec.IsPageResident(1));
}

TEST(PagedVectorTest, WalkPagesPartialRangeSkipsOuterPages) {
  InMemStore mem;
  auto vec = PagedVector<int>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(10 * sizeof(int));
  for (int i = 0; i < 30; ++i) {
    vec.push_back(i);
  }
  // Auto-seal at i=9, 19, 29 -> 3 frozen + empty tail.
  // Range covers only the middle frozen page (indices 10..19).
  vec.EvictPage(0);
  vec.EvictPage(1);

  uint32_t cb = 0;
  std::vector<int> seen;
  WalkPages(vec, 10, 20, [&](const PagedVector<int>::PageView& view) {
    ++cb;
    uint64_t lo = std::max<uint64_t>(view.base_index, 10);
    uint64_t hi = std::min<uint64_t>(view.base_index + view.length, 20);
    for (uint64_t i = lo; i < hi; ++i) {
      seen.push_back(view.data[i - view.base_index]);
    }
  });
  EXPECT_EQ(cb, 1u);
  ASSERT_EQ(seen.size(), 10u);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(seen[static_cast<size_t>(i)], i + 10);
  }
  // Page 0 stayed evicted because the walk skipped it.
  EXPECT_FALSE(vec.IsPageResident(0));
  // Page 1 was restored because the walk needed it.
  EXPECT_TRUE(vec.IsPageResident(1));
}

// ---------------------------------------------------------------------------
// Per-page (first, last) endpoint metadata tests.
// ---------------------------------------------------------------------------

TEST(PagedVectorTest, PageEndpointsSortedColumn) {
  auto vec = PagedVector<int64_t>::CreateWithCapacity(8);
  // 4-element pages → auto-seal at i=3, 7. Page 0: [10..13]. Page 1: [50..53].
  vec.SetPageBytesForTesting(4 * sizeof(int64_t));
  for (int64_t i = 10; i <= 13; ++i) {
    vec.push_back(i);
  }
  for (int64_t i = 50; i <= 53; ++i) {
    vec.push_back(i);
  }
  // Tail: [99, 100]
  vec.push_back(99);
  vec.push_back(100);

  ASSERT_EQ(vec.num_pages(), 3u);  // 2 frozen + tail.
  EXPECT_EQ(vec.page_first_value<int64_t>(0), 10);
  EXPECT_EQ(vec.page_last_value<int64_t>(0), 13);
  EXPECT_EQ(vec.page_first_value<int64_t>(1), 50);
  EXPECT_EQ(vec.page_last_value<int64_t>(1), 53);
}

TEST(PagedVectorTest, PageEndpointsSurviveEviction) {
  InMemStore mem;
  auto vec = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(4 * sizeof(int64_t));
  for (int64_t i = 0; i < 4; ++i) {
    vec.push_back(i * 100);
  }
  // Auto-sealed at i=3 -> 1 frozen + empty tail.
  ASSERT_EQ(vec.num_pages(), 2u);

  // Endpoints work both before and after eviction.
  EXPECT_EQ(vec.page_first_value<int64_t>(0), 0);
  EXPECT_EQ(vec.page_last_value<int64_t>(0), 300);
  vec.EvictPage(0);
  EXPECT_FALSE(vec.IsPageResident(0));
  EXPECT_EQ(vec.page_first_value<int64_t>(0), 0);
  EXPECT_EQ(vec.page_last_value<int64_t>(0), 300);
}

TEST(PagedVectorTest, PageEndpointsSinglePageColumnHasNone) {
  auto vec = PagedVector<int64_t>::CreateWithCapacity(8);
  for (int64_t i = 0; i < 5; ++i) {
    vec.push_back(i);
  }
  // Default page size keeps this single-page; endpoints are only populated
  // for *frozen* pages (the tail's last value is `back()`).
  EXPECT_EQ(vec.num_pages(), 1u);
  EXPECT_EQ(vec.back(), 4);
}

TEST(PagedVectorTest, PageEndpointsThroughTypeErasedReinterpret) {
  // Build an int64_t-typed view via a PagedVector<uint8_t>, mirroring how
  // tree storage stores typed data and the bytecode VM reinterprets it.
  PagedVector<uint8_t> bytes;
  // Auto-seal after each 24-byte chunk = one int64_t triple.
  bytes.SetPageBytesForTesting(24);
  // Page 0 stores int64_t values 7, 8, 9 — total 24 bytes.
  for (int64_t v : {int64_t{7}, int64_t{8}, int64_t{9}}) {
    for (size_t b = 0; b < sizeof(int64_t); ++b) {
      bytes.push_back(reinterpret_cast<const uint8_t*>(&v)[b]);
    }
  }
  // Auto-sealed at byte 23 -> 1 frozen + empty tail.
  ASSERT_EQ(bytes.num_pages(), 2u);

  // The endpoints are stored as raw bytes; through page_first_value<int64_t>
  // they reinterpret to the typed values.
  auto* as_int64 = reinterpret_cast<PagedVector<int64_t>*>(&bytes);
  EXPECT_EQ(as_int64->page_first_value<int64_t>(0), 7);
  EXPECT_EQ(as_int64->page_last_value<int64_t>(0), 9);
}

// ---------------------------------------------------------------------------
// PageStore budget enforcement tests.
// ---------------------------------------------------------------------------

TEST(PageStoreTest, ResidentBytesTracksAllocAndSeal) {
  InMemStore mem;
  EXPECT_EQ(mem.store.resident_bytes(), 0u);

  auto vec = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(8 * sizeof(int64_t));
  // Tail allocation is rounded up to kCapacityMultipleBytes (64 * 8 = 512).
  EXPECT_GT(mem.store.resident_bytes(), 0u);
  uint64_t after_alloc = mem.store.resident_bytes();

  for (int i = 0; i < 7; ++i) {
    vec.push_back(i);
  }
  // No reallocation while within tail capacity, no auto-seal yet.
  EXPECT_EQ(mem.store.resident_bytes(), after_alloc);

  // 8th push hits threshold -> auto-seal.
  vec.push_back(7);
  // After auto-seal: a new (empty) tail is allocated, the old data lives in
  // a shrunk-to-fit frozen page. Both contribute resident bytes.
  EXPECT_GT(mem.store.resident_bytes(), 0u);
}

TEST(PageStoreTest, OverBudgetTriggersEviction) {
  InMemStore mem;
  auto vec = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(8 * sizeof(int64_t));

  // Fill three frozen pages via auto-seal.
  for (int i = 0; i < 24; ++i) {
    vec.push_back(i);
  }
  // Auto-seal at i=7, 15, 23 -> 3 frozen + empty tail.
  ASSERT_EQ(vec.num_pages(), 4u);
  EXPECT_EQ(mem.storage()->entry_count(), 0u);

  // Drop the budget below current resident bytes — eviction must fire.
  uint64_t before = mem.store.resident_bytes();
  mem.store.SetBudgetBytes(before / 2);

  EXPECT_LE(mem.store.resident_bytes(), before / 2);
  EXPECT_GT(mem.storage()->entry_count(), 0u);

  // Reads still see the right data: evicted pages restore on demand.
  for (int i = 0; i < 24; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i);
  }
}

TEST(PageStoreTest, EvictionRoundRobinsAcrossPagedVectors) {
  InMemStore mem;
  auto a = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  auto b = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  a.SetPageBytesForTesting(8 * sizeof(int64_t));
  b.SetPageBytesForTesting(8 * sizeof(int64_t));

  // 16 elements at 8/page → auto-seal at i=7, 15: 2 frozen + empty tail each.
  for (int i = 0; i < 16; ++i) {
    a.push_back(i);
    b.push_back(i + 100);
  }
  ASSERT_EQ(a.num_pages(), 3u);
  ASSERT_EQ(b.num_pages(), 3u);

  // Tighten the budget enough that eviction has to draw from BOTH
  // PagedVectors — single-evictee mode would leave the other untouched.
  mem.store.SetBudgetBytes(0);

  // Both vectors should have at least one frozen page evicted (round-robin
  // hits each before re-visiting). With budget=0 every resident page is
  // pushed out, so both must end with no frozen page resident.
  EXPECT_FALSE(a.IsPageResident(0));
  EXPECT_FALSE(a.IsPageResident(1));
  EXPECT_FALSE(b.IsPageResident(0));
  EXPECT_FALSE(b.IsPageResident(1));
}

TEST(PageStoreTest, AutoSealPlusBudgetEvictsContinuously) {
  InMemStore mem;
  // Budget tight enough that only ~one page can stay resident; auto-seal
  // forces a new frozen page every 4 elements; resident bytes should
  // oscillate within budget rather than growing unbounded.
  auto vec = PagedVector<int64_t>::CreateWithCapacity(8, &mem.store);
  vec.SetPageBytesForTesting(4 * sizeof(int64_t));
  mem.store.SetBudgetBytes(64);  // smaller than two frozen pages.

  for (int i = 0; i < 200; ++i) {
    vec.push_back(i);
    EXPECT_LE(mem.store.resident_bytes(), 1024u)
        << "resident bytes exploded at i=" << i;
  }
  EXPECT_EQ(vec.size(), 200u);

  // Sanity: every element still readable (auto-restore on access).
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(vec[static_cast<uint64_t>(i)], i) << "at i=" << i;
  }
}

}  // namespace
}  // namespace perfetto::trace_processor::core
