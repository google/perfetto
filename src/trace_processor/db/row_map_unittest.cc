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

#include "src/trace_processor/db/row_map.h"

#include <memory>

#include "src/base/test/gtest_test_suite.h"
#include "test/gtest_and_gmock.h"

namespace perfetto {
namespace trace_processor {
namespace {

std::shared_ptr<RowMap> BitVectorRowMap() {
  BitVector bv;
  bv.Append(true);
  bv.Append(false);
  bv.Append(true);
  bv.Append(true);
  bv.Append(false);
  bv.Append(true);
  return std::shared_ptr<RowMap>(new RowMap(std::move(bv)));
}

std::shared_ptr<RowMap> RowVectorRowMap() {
  return std::shared_ptr<RowMap>(
      new RowMap(std::vector<uint32_t>{0u, 2u, 3u, 5u}));
}

// We use a shared_ptr here because value-parameterized gtests need to be
// copyable but RowMap has a deleted copy constructor. Wrapping with a
// shared_ptr works around this restriction.
class RowMapUnittest
    : public ::testing::TestWithParam<std::shared_ptr<RowMap>> {};

TEST_P(RowMapUnittest, Add) {
  RowMap row_map = GetParam()->Copy();
  ASSERT_EQ(row_map.size(), 4u);

  row_map.Add(10u);

  ASSERT_EQ(row_map.size(), 5u);
  ASSERT_EQ(row_map.Get(0u), 0u);
  ASSERT_EQ(row_map.Get(1u), 2u);
  ASSERT_EQ(row_map.Get(2u), 3u);
  ASSERT_EQ(row_map.Get(3u), 5u);
  ASSERT_EQ(row_map.Get(4u), 10u);
  ASSERT_EQ(row_map.IndexOf(0u), 0u);
  ASSERT_EQ(row_map.IndexOf(2u), 1u);
  ASSERT_EQ(row_map.IndexOf(3u), 2u);
  ASSERT_EQ(row_map.IndexOf(5u), 3u);
  ASSERT_EQ(row_map.IndexOf(10u), 4u);
}

TEST_P(RowMapUnittest, SelectRowsBitVector) {
  RowMap row_map = GetParam()->Copy();

  BitVector picker_bv;
  picker_bv.Append(true);
  picker_bv.Append(false);
  picker_bv.Append(false);
  picker_bv.Append(true);
  RowMap picker(std::move(picker_bv));

  row_map.SelectRows(picker);

  ASSERT_EQ(row_map.size(), 2u);
  ASSERT_EQ(row_map.Get(0u), 0u);
  ASSERT_EQ(row_map.Get(1u), 5u);
}

TEST_P(RowMapUnittest, SelectRowsRowVector) {
  RowMap row_map = GetParam()->Copy();
  RowMap picker(std::vector<uint32_t>{1u, 0u, 3u, 0u, 0u});

  row_map.SelectRows(picker);

  ASSERT_EQ(row_map.size(), 5u);
  ASSERT_EQ(row_map.Get(0u), 2u);
  ASSERT_EQ(row_map.Get(1u), 0u);
  ASSERT_EQ(row_map.Get(2u), 5u);
  ASSERT_EQ(row_map.Get(3u), 0u);
  ASSERT_EQ(row_map.Get(4u), 0u);
}

TEST_P(RowMapUnittest, RemoveIf) {
  RowMap row_map = GetParam()->Copy();

  row_map.RemoveIf([](uint32_t row) { return row == 2u || row == 5u; });

  ASSERT_EQ(row_map.size(), 2u);
  ASSERT_EQ(row_map.Get(0), 0u);
  ASSERT_EQ(row_map.Get(1), 3u);
}

INSTANTIATE_TEST_SUITE_P(RowMapUnittestInstatition,
                         RowMapUnittest,
                         testing::Values(BitVectorRowMap(), RowVectorRowMap()));

}  // namespace
}  // namespace trace_processor
}  // namespace perfetto
