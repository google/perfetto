/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"

#include <cstdint>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/base/test/status_matchers.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {

inline void PrintTo(const ColumnSpec& spec, std::ostream* os) {
  *os << "\n  ColumnSpec{\n"
      << "    type: " << spec.type.ToString() << ",\n"
      << "    nullability: " << spec.nullability.ToString() << ",\n"
      << "    sort_state: " << spec.sort_state.ToString() << ",\n"
      << "    duplicate_state: " << spec.duplicate_state.ToString() << "\n"
      << "  }";
}

inline bool operator==(const ColumnSpec& lhs, const ColumnSpec& rhs) {
  return lhs.type == rhs.type && lhs.nullability == rhs.nullability &&
         lhs.sort_state == rhs.sort_state &&
         lhs.duplicate_state == rhs.duplicate_state;
}

namespace {

using testing::ElementsAre;

class AdhocDataframeBuilderTest : public ::testing::Test {
 protected:
  StringPool pool_;
};

TEST_F(AdhocDataframeBuilderTest, StringColumnWithNullId) {
  AdhocDataframeBuilder builder({"str_col"}, &pool_);

  builder.PushNonNull(0, pool_.InternString("hello"));
  builder.PushNonNull(0, StringPool::Id::Null());
  builder.PushNonNull(0, pool_.InternString("world"));

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());

  auto spec = df.CreateSpec();
  ASSERT_THAT(spec.column_names, ElementsAre("str_col", "_auto_id"));
  ASSERT_THAT(
      spec.column_specs,
      ElementsAre(
          ColumnSpec{String{}, SparseNull{}, Unsorted{}, HasDuplicates{}},
          ColumnSpec{Id{}, NonNull{}, IdSorted{}, NoDuplicates{}}));
}

// Callback for reading cell values in tests.
struct TestCellCallback : CellCallback {
  void OnCell(int64_t v) {
    int_value = v;
    is_null = false;
  }
  void OnCell(uint32_t v) {
    int_value = static_cast<int64_t>(v);
    is_null = false;
  }
  void OnCell(int32_t v) {
    int_value = static_cast<int64_t>(v);
    is_null = false;
  }
  void OnCell(double v) {
    double_value = v;
    is_null = false;
  }
  void OnCell(NullTermStringView) { is_null = false; }
  void OnCell(std::nullptr_t) { is_null = true; }

  int64_t int_value = 0;
  double double_value = 0;
  bool is_null = false;
};

// Test that DenseNull correctly handles the case where PushNull is called
// before any non-null value (i.e., before storage type is known).
TEST_F(AdhocDataframeBuilderTest, DenseNullWithLeadingNulls) {
  AdhocDataframeBuilder builder(
      {"col"}, &pool_,
      AdhocDataframeBuilder::Options{{}, NullabilityType::kDenseNull});

  // Push null first - storage doesn't exist yet
  builder.PushNull(0);
  // Then push non-null values
  builder.PushNonNull(0, int64_t{10});
  builder.PushNonNull(0, int64_t{20});

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());

  ASSERT_EQ(df.row_count(), 3u);

  // Verify cell values
  TestCellCallback cb;

  df.GetCell(0, 0, cb);
  EXPECT_TRUE(cb.is_null);

  df.GetCell(1, 0, cb);
  EXPECT_FALSE(cb.is_null);
  EXPECT_EQ(cb.int_value, 10);

  df.GetCell(2, 0, cb);
  EXPECT_FALSE(cb.is_null);
  EXPECT_EQ(cb.int_value, 20);
}

// Test DenseNull with multiple leading nulls.
TEST_F(AdhocDataframeBuilderTest, DenseNullWithMultipleLeadingNulls) {
  AdhocDataframeBuilder builder(
      {"col"}, &pool_,
      AdhocDataframeBuilder::Options{{}, NullabilityType::kDenseNull});

  // Push multiple nulls first
  builder.PushNull(0);
  builder.PushNull(0);
  // Then push non-null value
  builder.PushNonNull(0, int64_t{42});

  base::StatusOr<Dataframe> df_status = std::move(builder).Build();
  ASSERT_OK(df_status.status());
  Dataframe df = std::move(df_status.value());

  ASSERT_EQ(df.row_count(), 3u);

  TestCellCallback cb;

  df.GetCell(0, 0, cb);
  EXPECT_TRUE(cb.is_null);

  df.GetCell(1, 0, cb);
  EXPECT_TRUE(cb.is_null);

  df.GetCell(2, 0, cb);
  EXPECT_FALSE(cb.is_null);
  EXPECT_EQ(cb.int_value, 42);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::dataframe
