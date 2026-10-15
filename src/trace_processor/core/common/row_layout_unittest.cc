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

#include "src/trace_processor/core/common/row_layout.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core {
namespace {

template <typename T>
std::vector<uint8_t> LayOut(RowLayout::Type type,
                            const std::vector<std::optional<T>>& values,
                            bool nullable,
                            bool descending) {
  RowLayout layout({{type, nullable, descending}});
  std::vector<uint8_t> rows(layout.stride() * values.size());
  RowLayout::Write<T>(
      layout.slot(0), static_cast<uint32_t>(values.size()),
      [&](uint32_t i, T* out) {
        if (!values[i]) {
          return false;
        }
        *out = *values[i];
        return true;
      },
      rows.data());
  return rows;
}

template <typename T>
void ExpectOrdered(RowLayout::Type type,
                   const std::vector<std::optional<T>>& ascending,
                   bool nullable) {
  for (bool descending : {false, true}) {
    std::vector<uint8_t> rows = LayOut(type, ascending, nullable, descending);
    size_t stride = rows.size() / ascending.size();
    for (size_t i = 0; i + 1 < ascending.size(); ++i) {
      int cmp = memcmp(&rows[i * stride], &rows[(i + 1) * stride], stride);
      if (descending) {
        EXPECT_GT(cmp, 0) << i;
      } else {
        EXPECT_LT(cmp, 0) << i;
      }
    }
  }
}

TEST(RowLayoutTest, BytesSortAsValues) {
  ExpectOrdered<uint32_t>(
      RowLayout::Type::kUint32,
      {0u, 1u, 255u, 256u, std::numeric_limits<uint32_t>::max()}, false);
  ExpectOrdered<int32_t>(RowLayout::Type::kInt32,
                         {std::numeric_limits<int32_t>::min(), -256, -1, 0, 1,
                          256, std::numeric_limits<int32_t>::max()},
                         false);
  ExpectOrdered<int64_t>(
      RowLayout::Type::kInt64,
      {std::numeric_limits<int64_t>::min(), -(int64_t{1} << 40), int64_t{-1},
       int64_t{0}, int64_t{1}, int64_t{1} << 40,
       std::numeric_limits<int64_t>::max()},
      false);
  ExpectOrdered<double>(
      RowLayout::Type::kDouble,
      {-std::numeric_limits<double>::infinity(), -1e300, -1.5, -0.0, 1e-300,
       1.5, 1e300, std::numeric_limits<double>::infinity()},
      false);
}

TEST(RowLayoutTest, NullsSortFirst) {
  ExpectOrdered<int64_t>(
      RowLayout::Type::kInt64,
      {std::nullopt, std::numeric_limits<int64_t>::min(), int64_t{0}}, true);
}

TEST(RowLayoutTest, SlotsSitOneAfterAnother) {
  RowLayout layout({{RowLayout::Type::kInt32, false},
                    {RowLayout::Type::kDouble, true},
                    {RowLayout::Type::kUint32, true}});
  EXPECT_EQ(layout.slot(0).offset, 0u);
  EXPECT_EQ(layout.slot(1).offset, 4u);
  EXPECT_EQ(layout.slot(2).offset, 13u);
  EXPECT_EQ(layout.stride(), 18u);
  EXPECT_EQ(layout.slot(1).stride, 18u);
}

}  // namespace
}  // namespace perfetto::trace_processor::core
