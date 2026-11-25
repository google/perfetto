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

#include "src/trace_processor/core/exec/selection.h"

#include <cstdint>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

std::vector<uint32_t> Kept(const Selection& selection) {
  std::vector<uint32_t> rows;
  for (uint32_t i = 0; i < selection.size(); ++i) {
    rows.push_back(selection[i]);
  }
  return rows;
}

// One pseudo-random value per row, where roughly one row in every `one_in`
// is 7.
std::vector<uint32_t> Values(uint32_t rows, uint32_t one_in) {
  std::vector<uint32_t> values(rows);
  uint32_t state = 12345;
  for (uint32_t& value : values) {
    state = state * 1103515245 + 12345;
    value = (state >> 16) % one_in == 0 ? 7 : state >> 16;
  }
  return values;
}

// Checks KeepMatching() on `selection` against a simple row-by-row loop.
void ExpectKeepMatching(Selection& selection,
                        const std::vector<uint32_t>& values,
                        bool equal) {
  std::vector<uint32_t> expected;
  for (uint32_t row : Kept(selection)) {
    if ((values[row] == 7) == equal) {
      expected.push_back(row);
    }
  }
  selection.KeepMatching(values.data(), 7, equal);
  EXPECT_EQ(Kept(selection), expected);
}

TEST(SelectionTest, KeepMatchingAPrefix) {
  for (uint32_t rows : {0u, 1u, 7u, 8u, 9u, 16u, 37u, kMaxBatchRows}) {
    for (uint32_t one_in : {1u, 2u, 10u, 1000u}) {
      for (bool equal : {true, false}) {
        std::vector<uint32_t> values = Values(rows, one_in);
        Selection selection;
        selection.Reset(rows);
        ExpectKeepMatching(selection, values, equal);
      }
    }
  }
}

TEST(SelectionTest, KeepMatchingIndices) {
  std::vector<uint32_t> values = Values(kMaxBatchRows, 3);
  Selection selection;
  selection.Reset(kMaxBatchRows);
  std::vector<uint32_t> odd;
  for (uint32_t row = 1; row < kMaxBatchRows; row += 2) {
    odd.push_back(row);
  }
  selection.KeepRows({odd.data(), odd.data() + odd.size()});
  ExpectKeepMatching(selection, values, false);
  ExpectKeepMatching(selection, values, true);
}

TEST(SelectionTest, KeepMatchingTheFirstRowsStaysAPrefix) {
  std::vector<uint32_t> values(20, 7);
  values[18] = 0;
  values[19] = 0;
  Selection selection;
  selection.Reset(20);
  selection.KeepMatching(values.data(), 7, true);
  EXPECT_TRUE(selection.prefix());
  EXPECT_EQ(selection.size(), 18u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
