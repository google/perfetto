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

#include "src/trace_processor/core/exec/dataframe_scan.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Optional;

// Bigger than an int32, so a column holding it is stored as an int64.
constexpr int64_t kBig = int64_t{1} << 40;

class DataframeScanTest : public ::testing::Test {
 protected:
  // A dataframe of one column, nullable where `present` says so. The values
  // are big so that the builder picks int64 and the test can read it as one.
  dataframe::Dataframe Build(const std::vector<int64_t>& values,
                             const std::vector<bool>& present) {
    dataframe::AdhocDataframeBuilder builder({"v"}, &pool_);
    for (uint32_t i = 0; i < values.size(); ++i) {
      if (present[i]) {
        EXPECT_TRUE(builder.PushNonNull(0, values[i]));
      } else {
        builder.PushNull(0);
      }
    }
    auto df = std::move(builder).Build();
    EXPECT_TRUE(df.ok()) << df.status().c_message();
    return std::move(*df);
  }

  std::vector<int64_t> Drain(const DataframeScan& scan, uint32_t column) {
    std::unique_ptr<OperatorState> state = scan.MakeState();
    RowBatch batch;
    std::vector<int64_t> out;
    while (scan.GetData(batch, *state)) {
      std::vector<int64_t> values = test::ReadColumn<int64_t>(batch, column);
      out.insert(out.end(), values.begin(), values.end());
    }
    return out;
  }

  StringPool pool_;
};

inline constexpr auto kAllTypes = dataframe::CreateTypedDataframeSpec(
    {"id", "u32", "i32", "i64", "double", "string"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     NonNull{},
                                     IdSorted{},
                                     NoDuplicates{}),
    dataframe::CreateTypedColumnSpec(Uint32{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Int32{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Int64{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(Double{}, NonNull{}, Unsorted{}),
    dataframe::CreateTypedColumnSpec(String{}, NonNull{}, Unsorted{}));

inline constexpr auto kSparseId = dataframe::CreateTypedDataframeSpec(
    {"id"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     SparseNull{},
                                     Unsorted{},
                                     NoDuplicates{}));

inline constexpr auto kDenseId = dataframe::CreateTypedDataframeSpec(
    {"id"},
    dataframe::CreateTypedColumnSpec(Id{},
                                     DenseNull{},
                                     Unsorted{},
                                     NoDuplicates{}));

TEST_F(DataframeScanTest, ReadsEveryStorageType) {
  StringPool::Id first = pool_.InternString("first");
  StringPool::Id second = pool_.InternString("second");
  dataframe::Dataframe df =
      dataframe::Dataframe::CreateFromTypedSpec(kAllTypes, &pool_);
  df.InsertUnchecked(kAllTypes, std::monostate{}, uint32_t{7}, int32_t{-3},
                     kBig, 1.5, first);
  df.InsertUnchecked(kAllTypes, std::monostate{}, uint32_t{9}, int32_t{4},
                     kBig + 1, 2.5, second);
  df.Finalize();

  DataframeScan scan(df, {0, 1, 2, 3, 4, 5});
  std::unique_ptr<OperatorState> state = scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(scan.GetData(batch, *state));
  EXPECT_TRUE(batch.column(0).type().Is<Id>());
  EXPECT_TRUE(batch.column(1).type().Is<Uint32>());
  EXPECT_TRUE(batch.column(2).type().Is<Int32>());
  EXPECT_TRUE(batch.column(3).type().Is<Int64>());
  EXPECT_TRUE(batch.column(4).type().Is<Double>());
  EXPECT_TRUE(batch.column(5).type().Is<String>());
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 0), ElementsAre(0u, 1u));
  EXPECT_THAT(test::ReadColumn<uint32_t>(batch, 1), ElementsAre(7u, 9u));
  EXPECT_THAT(test::ReadColumn<int32_t>(batch, 2), ElementsAre(-3, 4));
  EXPECT_THAT(test::ReadColumn<int64_t>(batch, 3), ElementsAre(kBig, kBig + 1));
  EXPECT_THAT(test::ReadColumn<double>(batch, 4), ElementsAre(1.5, 2.5));
  EXPECT_THAT(test::ReadColumn<StringPool::Id>(batch, 5),
              ElementsAre(first, second));
}

TEST_F(DataframeScanTest, PreservesNullableIdSemantics) {
  dataframe::Dataframe sparse =
      dataframe::Dataframe::CreateFromTypedSpec(kSparseId, &pool_);
  sparse.InsertUnchecked(kSparseId,
                         std::optional<std::monostate>{std::monostate{}});
  sparse.InsertUnchecked(kSparseId, std::optional<std::monostate>{});
  sparse.InsertUnchecked(kSparseId,
                         std::optional<std::monostate>{std::monostate{}});
  sparse.Finalize();

  DataframeScan sparse_scan(sparse, {0});
  std::unique_ptr<OperatorState> sparse_state = sparse_scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(sparse_scan.GetData(batch, *sparse_state));
  EXPECT_TRUE(batch.column(0).type().Is<Uint32>());
  EXPECT_THAT(test::ReadNullableColumn<uint32_t>(batch, 0),
              ElementsAre(Optional(0u), Eq(std::nullopt), Optional(1u)));

  dataframe::Dataframe dense =
      dataframe::Dataframe::CreateFromTypedSpec(kDenseId, &pool_);
  dense.InsertUnchecked(kDenseId,
                        std::optional<std::monostate>{std::monostate{}});
  dense.InsertUnchecked(kDenseId, std::optional<std::monostate>{});
  dense.InsertUnchecked(kDenseId,
                        std::optional<std::monostate>{std::monostate{}});
  dense.Finalize();

  DataframeScan dense_scan(dense, {0});
  std::unique_ptr<OperatorState> dense_state = dense_scan.MakeState();
  ASSERT_TRUE(dense_scan.GetData(batch, *dense_state));
  EXPECT_TRUE(batch.column(0).type().Is<Id>());
  EXPECT_THAT(test::ReadNullableColumn<uint32_t>(batch, 0),
              ElementsAre(Optional(0u), Eq(std::nullopt), Optional(2u)));
}

// The point of the operator: the batch reads the dataframe's own storage.
TEST_F(DataframeScanTest, ReadsTheDataframesOwnStorage) {
  dataframe::Dataframe df =
      Build({kBig + 10, kBig + 20, kBig + 30}, {true, true, true});
  DataframeScan scan(df, {0});

  std::unique_ptr<OperatorState> state = scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(scan.GetData(batch, *state));
  EXPECT_EQ(batch.size(), 3u);
  EXPECT_EQ(batch.column(0).data(),
            df.column(0).storage.unchecked_data<Int64>());
}

TEST_F(DataframeScanTest, HandsBackEveryRow) {
  dataframe::Dataframe df =
      Build({kBig + 10, kBig + 20, kBig + 30}, {true, true, true});
  DataframeScan scan(df, {0});
  EXPECT_THAT(Drain(scan, 0), ElementsAre(kBig + 10, kBig + 20, kBig + 30));
}

TEST_F(DataframeScanTest, SplitsIntoBatches) {
  std::vector<int64_t> values(kMaxBatchRows * 2 + 5);
  std::vector<bool> present(values.size(), true);
  for (uint32_t i = 0; i < values.size(); ++i) {
    values[i] = kBig + i;
  }
  dataframe::Dataframe df = Build(values, present);
  DataframeScan scan(df, {0});
  EXPECT_EQ(Drain(scan, 0).size(), values.size());
}

// A column which does not store one value per row is laid back out once, so
// the rest of the pipeline never has to know the difference.
TEST_F(DataframeScanTest, AColumnWithoutASlotPerRowIsExpanded) {
  dataframe::Dataframe df =
      Build({kBig + 10, 0, kBig + 30}, {true, false, true});
  DataframeScan scan(df, {0});

  std::unique_ptr<OperatorState> state = scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(scan.GetData(batch, *state));
  ASSERT_EQ(batch.size(), 3u);

  const ColumnView& view = batch.column(0);
  const auto* data = static_cast<const int64_t*>(view.data());
  EXPECT_EQ(data[0], kBig + 10);
  EXPECT_EQ(data[2], kBig + 30);
  // Readable at every row, whether or not the row is null.
  EXPECT_EQ(data[1], 0);
  ASSERT_NE(view.validity(), nullptr);
  EXPECT_TRUE(view.validity()->is_set(0));
  EXPECT_FALSE(view.validity()->is_set(1));
  EXPECT_TRUE(view.validity()->is_set(2));
}

// Expanding a batch at a time means picking up in the packed values where the
// previous batch left off, which every batch after the first depends on.
TEST_F(DataframeScanTest, AColumnWithoutASlotPerRowSpansBatches) {
  std::vector<int64_t> values(kMaxBatchRows * 2 + 5);
  std::vector<bool> present(values.size());
  std::vector<int64_t> expected;
  for (uint32_t i = 0; i < values.size(); ++i) {
    present[i] = i % 3 != 0;
    values[i] = present[i] ? kBig + i : 0;
    expected.push_back(values[i]);
  }
  dataframe::Dataframe df = Build(values, present);
  DataframeScan scan(df, {0});
  EXPECT_EQ(Drain(scan, 0), expected);
}

// Replaying has to wind the packed values back too, not just the row counter.
TEST_F(DataframeScanTest, AColumnWithoutASlotPerRowIsReplayable) {
  dataframe::Dataframe df =
      Build({kBig + 10, 0, kBig + 30}, {true, false, true});
  DataframeScan scan(df, {0});

  std::unique_ptr<OperatorState> state = scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(scan.GetData(batch, *state));
  ASSERT_FALSE(scan.GetData(batch, *state));

  scan.Rewind(*state);
  ASSERT_TRUE(scan.GetData(batch, *state));
  const auto* data = static_cast<const int64_t*>(batch.column(0).data());
  EXPECT_EQ(data[0], kBig + 10);
  EXPECT_EQ(data[1], 0);
  EXPECT_EQ(data[2], kBig + 30);
}

TEST_F(DataframeScanTest, IsReplayable) {
  dataframe::Dataframe df = Build({kBig + 1, kBig + 2}, {true, true});
  DataframeScan scan(df, {0});

  std::unique_ptr<OperatorState> state = scan.MakeState();
  RowBatch batch;
  ASSERT_TRUE(scan.GetData(batch, *state));
  EXPECT_FALSE(scan.GetData(batch, *state));
  scan.Rewind(*state);
  EXPECT_TRUE(scan.GetData(batch, *state));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
