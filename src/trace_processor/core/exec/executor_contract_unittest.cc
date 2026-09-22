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

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

#include <utility>
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"

namespace perfetto::trace_processor::core::exec {
namespace {

using testing::ElementsAre;

struct CountedBuffer {
  CountedBuffer() { ++allocations; }
  static uint32_t allocations;
  uint32_t value = 0;
};
uint32_t CountedBuffer::allocations = 0;

TEST(ExecutorContractTest, RetentionSharesValuesAndReleasesLastOwner) {
  auto values = std::make_shared<std::vector<int64_t>>(
      std::initializer_list<int64_t>{10, 20, 30});
  std::weak_ptr<std::vector<int64_t>> lifetime = values;
  const void* backing = values->data();
  RowBatch first, second;
  {
    RowBatch published;
    published.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, values->data()), values);
    published.SetCardinality(3);
    first.CopyFrom(published);
    second.CopyFrom(published);
    values.reset();
  }
  EXPECT_FALSE(lifetime.expired());
  EXPECT_EQ(first.column(0).data(), backing);
  EXPECT_EQ(second.column(0).data(), backing);
  first.Reset();
  EXPECT_FALSE(lifetime.expired());
  EXPECT_THAT(test::ReadColumn<int64_t>(second, 0), ElementsAre(10, 20, 30));
  second.Reset();
  EXPECT_TRUE(lifetime.expired());
}

TEST(ExecutorContractTest, RetainedSelectionSurvivesProducerReuse) {
  std::vector<int64_t> values{0, 10, 20, 30, 40, 50};
  std::vector<uint32_t> selected{0, 2};
  RowBatch producer, retained;
  auto publish = [&](uint32_t offset) {
    producer.Reset();
    auto view = ColumnView::Reference(StorageType{Int64{}}, values.data());
    view.SetRange(offset);
    producer.AddColumn(view);
    producer.SetCardinality(3);
    producer.Slice(RowSelection::Indices(Span<const uint32_t>(
                       selected.data(), selected.data() + selected.size())),
                   2);
  };
  publish(1);
  retained.CopyFrom(producer);
  EXPECT_THAT(test::ReadColumn<int64_t>(retained, 0), ElementsAre(10, 30));
  publish(2);
  EXPECT_THAT(test::ReadColumn<int64_t>(producer, 0), ElementsAre(20, 40));
  EXPECT_THAT(test::ReadColumn<int64_t>(retained, 0), ElementsAre(10, 30));
  EXPECT_EQ(retained.column(0).data(), values.data());
}

TEST(ExecutorContractTest,
     RetainedComputedValuesSurviveNextExecutionAndRewind) {
  TreeAccumulateDown op({0, 1, 2});
  auto state = op.MakeState();
  std::vector<uint32_t> nodes{0, 1}, parents{kNoNode, 0};
  std::vector<int64_t> values{10, 20};
  RowBatch input, output, retained;
  input.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, nodes.data()));
  input.AddColumn(ColumnView::Reference(StorageType{Uint32{}}, parents.data()));
  input.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
  input.SetCardinality(2);
  ASSERT_EQ(op.Execute(input, output, *state), OpResult::kNeedMoreInput);
  retained.CopyFrom(output);
  EXPECT_THAT(test::ReadColumn<int64_t>(retained, 3), ElementsAre(10, 30));
  values = {100, 200};
  op.Rewind(*state);
  ASSERT_EQ(op.Execute(input, output, *state), OpResult::kNeedMoreInput);
  EXPECT_THAT(test::ReadColumn<int64_t>(output, 3), ElementsAre(100, 300));
  EXPECT_THAT(test::ReadColumn<int64_t>(retained, 3), ElementsAre(10, 30));
}

TEST(ExecutorContractTest, ComposedSelectionsMatchScalarRows) {
  std::mt19937 random(1729);
  for (uint32_t size : {1u, 2u, 17u, 2048u}) {
    for (uint32_t trial = 0; trial < 32; ++trial) {
      SCOPED_TRACE(testing::Message() << "size=" << size << " trial=" << trial);
      std::vector<int64_t> values(size), computed(size);
      std::vector<uint32_t> physical(size), logical;
      auto valid = BitVector::CreateWithSize(size);
      for (uint32_t i = 0; i < size; ++i) {
        values[i] = 100 + i;
        computed[i] = 10000 + i;
        physical[i] = random() % size;
        if (random() % 3)
          valid.set(i);
        if (random() % 2)
          logical.push_back(i);
      }
      RowBatch batch;
      auto ids = ColumnView::Reference(StorageType{Id{}}, nullptr);
      auto data =
          ColumnView::Reference(StorageType{Int64{}}, values.data(), &valid);
      auto selection = test::OwnedRows(physical);
      ids.SetOwnedRows(selection, size);
      data.SetOwnedRows(selection, size);
      batch.AddColumn(ids);
      batch.AddColumn(data);
      batch.AddColumn(
          ColumnView::Reference(StorageType{Int64{}}, computed.data()));
      batch.SetCardinality(size);
      std::vector<uint32_t> expected_ids;
      std::vector<std::optional<int64_t>> expected_values;
      std::vector<int64_t> expected_computed;
      for (uint32_t i : logical) {
        uint32_t row = physical[i];
        expected_ids.push_back(row);
        expected_values.push_back(valid.is_set(row)
                                      ? std::optional<int64_t>(values[row])
                                      : std::nullopt);
        expected_computed.push_back(computed[i]);
      }
      if (logical.empty()) {
        batch.Slice(RowSelection::Range(0), 0);
      } else {
        batch.Slice(RowSelection::Indices(Span<const uint32_t>(
                        logical.data(), logical.data() + logical.size())),
                    static_cast<uint32_t>(logical.size()));
      }
      EXPECT_EQ(test::ReadColumn<uint32_t>(batch, 0), expected_ids);
      EXPECT_EQ(test::ReadNullableColumn<int64_t>(batch, 1), expected_values);
      EXPECT_EQ(test::ReadColumn<int64_t>(batch, 2), expected_computed);
      EXPECT_EQ(batch.column(1).data(), values.data());
      EXPECT_EQ(batch.column(2).data(), computed.data());
    }
  }
}

TEST(ExecutorContractTest, PublishedValidityRetainsItsOwner) {
  struct Storage {
    std::vector<int64_t> values{10, 20, 30};
    BitVector validity = BitVector::CreateWithSize(3, true);
  };
  auto storage = std::make_shared<Storage>();
  storage->validity.clear(1);
  RowBatch retained;
  {
    RowBatch output;
    output.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, storage->values.data(),
                              &storage->validity),
        storage);
    output.SetCardinality(3);
    retained.CopyFrom(output);
    storage.reset();
  }
  EXPECT_THAT(test::ReadNullableColumn<int64_t>(retained, 0),
              ElementsAre(10, std::nullopt, 30));
}

TEST(ExecutorContractTest, PoolReusesOnlyReleasedBuffers) {
  BufferPool<CountedBuffer> pool;
  auto first = pool.Acquire();
  first->value = 42;
  auto retained = first;
  first.reset();
  auto next = pool.Acquire();
  next->value = 7;
  EXPECT_EQ(retained->value, 42u);
  next.reset();
  uint32_t allocations = CountedBuffer::allocations;
  for (uint32_t i = 0; i < 100; ++i) {
    auto reusable = pool.Acquire();
  }
  EXPECT_EQ(CountedBuffer::allocations, allocations);
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
