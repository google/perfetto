/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/traced/probes/ftrace/page_pool.h"

#include <array>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

namespace perfetto {
namespace {

TEST(PagePoolTest, SingleThreaded) {
  PagePool pool;
  for (int i = 0; i < 2; i++)
    ASSERT_TRUE(pool.BeginRead().empty());

  for (int repeat = 0; repeat < 3; repeat++) {
    for (uint32_t seed = 0; seed < 6; seed++) {
      uint8_t* page = pool.BeginWrite();
      std::minstd_rand0 rnd_engine(seed);
      std::generate(page, page + base::kPageSize, rnd_engine);
      // Deliberately make it so pages 3 is overwritten, so we should see only
      // pages 0, 1, 2, 4, 5.
      if (seed != 3)
        pool.EndWrite();
    }

    // No write should be visible until the CommitWrittenPages() call.
    ASSERT_TRUE(pool.BeginRead().empty());

    pool.CommitWrittenPages();

    auto blocks = pool.BeginRead();
    ASSERT_EQ(blocks.size(), 1);
    ASSERT_EQ(blocks[0].size(), 5);
    for (uint32_t i = 0; i < blocks[0].size(); i++) {
      auto seed = std::array<uint32_t, 5>{{0, 1, 2, 4, 5}}[i];
      const char* page = reinterpret_cast<const char*>(blocks[0].At(i));
      char expected[base::kPageSize];
      std::minstd_rand0 rnd_engine(seed);
      std::generate(expected, expected + base::kPageSize, rnd_engine);
      EXPECT_STREQ(page, expected);
    }

    pool.EndRead(std::move(blocks));
    ASSERT_EQ(pool.freelist_size_for_testing(), 1);
  }
}

TEST(PagePoolTest, MultiThreaded) {
  PagePool pool;

  // Generate some random content.
  std::vector<std::string> expected_pages;
  std::minstd_rand0 rnd_engine(0);
  for (int i = 0; i < 1000; i++) {
    expected_pages.emplace_back();
    std::string& page = expected_pages.back();
    page.resize(base::kPageSize);
    std::generate(page.begin(), page.end(), rnd_engine);
  }

  auto writer_fn = [&pool, &expected_pages] {
    std::minstd_rand0 rnd(0);
    for (const std::string& expected_page : expected_pages) {
      uint8_t* dst = pool.BeginWrite();
      memcpy(dst, expected_page.data(), base::kPageSize);
      pool.EndWrite();
      if (rnd() % 16 == 0)
        pool.CommitWrittenPages();
    }
    pool.CommitWrittenPages();
  };

  auto reader_fn = [&pool, &expected_pages] {
    for (size_t page_idx = 0; page_idx < expected_pages.size();) {
      auto blocks = pool.BeginRead();
      for (const auto& block : blocks) {
        for (size_t i = 0; i < block.size(); i++) {
          const char* page = reinterpret_cast<const char*>(block.At(i));
          EXPECT_EQ(expected_pages[page_idx],
                    std::string(page, base::kPageSize));
          page_idx++;
        }
      }
      pool.EndRead(std::move(blocks));
    }
  };

  std::thread writer(writer_fn);
  std::thread reader(reader_fn);
  writer.join();
  reader.join();
}

}  // namespace
}  // namespace perfetto
