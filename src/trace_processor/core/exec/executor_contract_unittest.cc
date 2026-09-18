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
#include <utility>
#include <vector>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/batch_buffer.h"
#include "src/trace_processor/core/exec/buffer_pool.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_cursor.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/test_utils.h"
#include "src/trace_processor/core/exec/tree_accumulate.h"
#include "src/trace_processor/core/exec/tree_number_nodes.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/span.h"
#include "test/gtest_and_gmock.h"

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
      auto selection =
          Span<const uint32_t>(physical.data(), physical.data() + size);
      ids.SetBorrowedRows(selection);
      data.SetBorrowedRows(selection);
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

TEST(ExecutorContractTest, MaterializationPreservesFloatingPointBits) {
  const uint64_t bits[] = {0x8000000000000000ull, 0x7ff8000000000123ull,
                           0x7ff0000000000000ull, 0xfff0000000000000ull};
  std::vector<double> values(4);
  std::memcpy(values.data(), bits, sizeof(bits));
  std::vector<uint32_t> rows{1, 0, 1, 3};
  auto view = ColumnView::Reference(StorageType{Double{}}, values.data());
  view.SetBorrowedRows(
      Span<const uint32_t>(rows.data(), rows.data() + rows.size()));
  RowBatch input, output;
  input.AddColumn(view);
  input.SetCardinality(4);
  RowStore store;
  ASSERT_TRUE(store.Append(input).ok());
  ASSERT_EQ(store.View(&output, 0, 4), 4u);
  for (uint32_t i = 0; i < 4; ++i) {
    double value = output.column(0).Value<double>(i);
    uint64_t actual;
    std::memcpy(&actual, &value, sizeof(actual));
    EXPECT_EQ(actual, bits[rows[i]]);
  }
}

TEST(ExecutorContractTest, FragmentedGatherPreservesRowSpacesAndMixedValidity) {
  RowStore store;
  auto stable = std::make_shared<std::vector<int64_t>>(test::Sequence(256));
  for (uint32_t batch = 0; batch < 2; ++batch) {
    RowBatch in;
    auto original = ColumnView::Reference(StorageType{Int64{}}, stable->data());
    original.SetRange(batch * 128);
    in.AddColumn(original, stable);
    in.AddColumn(original, stable);
    auto local = std::make_shared<ColumnChunk>();
    local->validity = BitVector::CreateWithSize(129);
    for (uint32_t r = 0; r < 129; ++r) {
      local->Values<int64_t>()[r] = batch * 1000 + r;
      if (r % 3)
        local->validity.set(r);
    }
    // One column mixes nullable and non-null batches; the next is always
    // nullable and uses a different physical row space.
    in.AddColumn(ColumnView::Reference(StorageType{Int64{}},
                                       local->Values<int64_t>().data(),
                                       batch ? nullptr : &local->validity),
                 local);
    auto shifted = ColumnView::Reference(StorageType{Int64{}},
                                         local->Values<int64_t>().data(),
                                         &local->validity);
    shifted.SetRange(1);
    in.AddColumn(shifted, local);
    in.SetCardinality(128);
    ASSERT_TRUE(store.Append(in).ok());
  }
  RowBatch retained;
  for (uint32_t count : {63u, 64u, 65u, kMaxBatchRows}) {
    SCOPED_TRACE(count);
    std::vector<uint32_t> order;
    std::vector<int64_t> expected_stable;
    std::vector<std::optional<int64_t>> expected_mixed, expected_shifted;
    for (uint32_t r = 0; r < count; ++r) {
      uint32_t batch = r % 2, row = r * 7 % 128;
      order.push_back(batch * 128 + row);
      expected_stable.push_back(batch * 128 + row);
      expected_mixed.emplace_back(
          batch || row % 3 ? std::optional<int64_t>(batch * 1000 + row)
                           : std::nullopt);
      expected_shifted.emplace_back(
          (row + 1) % 3 ? std::optional<int64_t>(batch * 1000 + row + 1)
                        : std::nullopt);
    }
    RowBatch out;
    ASSERT_EQ(store.View(&out, Span<const uint32_t>(order.data(),
                                                    order.data() + count)),
              count);
    EXPECT_NE(out.column(0).data(), stable->data());
    EXPECT_TRUE(out.column(0).selection().is_range());
    EXPECT_EQ(out.column(0).selection().data(),
              out.column(1).selection().data());
    EXPECT_EQ(test::ReadColumn<int64_t>(out, 0), expected_stable);
    EXPECT_EQ(test::ReadColumn<int64_t>(out, 1), expected_stable);
    EXPECT_EQ(test::ReadNullableColumn<int64_t>(out, 2), expected_mixed);
    EXPECT_EQ(test::ReadNullableColumn<int64_t>(out, 3), expected_shifted);
    if (count == 63)
      retained.CopyFrom(out);
  }
  store.Clear();
  // Later gathers and destruction of the input batches cannot change output.
  EXPECT_EQ(retained.size(), 63u);
  EXPECT_EQ(retained.column(2).Value<int64_t>(1), 1007);
  EXPECT_FALSE(retained.column(2).validity()->is_set(0));
}

// Scripted source: empty chunks are genuine intermediate batches, not EOF.
// The fixture does not implement batching policy. It records what the real
// Pipeline/RowCursor asks for and can fail after delivering a prefix.
class ContractSource final : public Source {
 public:
  ContractSource(std::vector<std::vector<int64_t>> chunks, bool fail = false)
      : chunks_(std::move(chunks)), fail_(fail) {}
  struct State : OperatorState {
    size_t next = 0;
    bool failed = false;
  };
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().next = 0;
    state.Cast<State>().failed = false;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    ++pulls;
    auto& s = state.Cast<State>();
    out.Reset();
    if (s.next == chunks_.size()) {
      s.failed = fail_;
      return false;
    }
    const auto& values = chunks_[s.next++];
    out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, values.data()));
    out.SetCardinality(static_cast<uint32_t>(values.size()));
    return true;
  }
  base::Status status(const OperatorState& state) const override {
    return state.Cast<const State>().failed ? base::ErrStatus("source failed")
                                            : base::OkStatus();
  }
  mutable uint32_t pulls = 0;

 private:
  std::vector<std::vector<int64_t>> chunks_;
  bool fail_;
};

class ContractFilter final : public Operator {
 public:
  struct State : OperatorState {
    std::vector<uint32_t> selected;
  };
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override {
    auto& rows = state.Cast<State>().selected;
    rows.clear();
    for (uint32_t i = 0; i < in.size(); ++i) {
      ++evaluated;
      if (in.column(0).Value<int64_t>(i) >= 0)
        rows.push_back(i);
    }
    out.CopyFrom(in);
    out.Slice(RowSelection::Indices(
                  Span<const uint32_t>(rows.data(), rows.data() + rows.size())),
              static_cast<uint32_t>(rows.size()));
    return OpResult::kNeedMoreInput;
  }
  mutable uint32_t evaluated = 0;
};

class FinishProbe final : public Operator {
 public:
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState&) const override {
    out.CopyFrom(in);
    return OpResult::kNeedMoreInput;
  }
  OpResult Finish(RowBatch& out, OperatorState&) const override {
    ++finishes;
    out.Reset();
    return OpResult::kNeedMoreInput;
  }
  mutable uint32_t finishes = 0;
};

class ExecutorBoundaryContractTest : public testing::TestWithParam<uint32_t> {};

TEST_P(ExecutorBoundaryContractTest, FilteringIsIndependentOfBatchBoundaries) {
  std::vector<int64_t> expected;
  std::vector<std::vector<int64_t>> chunks(2);  // initial empty output
  for (uint32_t i = 0; i < 4103; ++i) {
    if (chunks.back().size() == GetParam()) {
      chunks.emplace_back();
      chunks.emplace_back();  // empty between nonempty batches
    }
    int64_t value = i % 7 == 0 ? static_cast<int64_t>(i) : -1;
    chunks.back().push_back(value);
    if (value >= 0)
      expected.push_back(value);
  }
  chunks.emplace_back();
  ContractSource source(std::move(chunks));
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::make_unique<ContractFilter>());
  Pipeline pipeline(source, std::move(ops));
  RowCursor cursor(pipeline);
  for (uint32_t run = 0; run < 2; ++run) {
    std::vector<int64_t> actual;
    for (bool more = cursor.Open(); more && !cursor.eof(); more = cursor.Next())
      actual.push_back(cursor.Value<int64_t>(0));
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(cursor.status().ok());
  }
}

INSTANTIATE_TEST_SUITE_P(BatchSizes,
                         ExecutorBoundaryContractTest,
                         testing::Values(1u, 2u, 7u, 127u, 2048u));

TEST(ExecutorContractTest, EmptySourceBatchesAreNotEofWithoutOperators) {
  ContractSource source({{}, {11}, {}, {22}, {}});
  Pipeline pipeline(source, {});
  RowCursor cursor(pipeline);
  std::vector<int64_t> actual;
  for (bool more = cursor.Open(); more && !cursor.eof(); more = cursor.Next())
    actual.push_back(cursor.Value<int64_t>(0));
  EXPECT_THAT(actual, ElementsAre(11, 22));
  EXPECT_TRUE(cursor.status().ok());
}

TEST(ExecutorContractTest, FirstSurvivorDoesNotTriggerLookahead) {
  ContractSource source({{-1, -2}, {9, 10}, {11}});
  auto filter = std::make_unique<ContractFilter>();
  auto* observed = filter.get();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::move(filter));
  Pipeline pipeline(source, std::move(ops));
  {
    RowCursor cursor(pipeline);
    ASSERT_TRUE(cursor.Open());
    ASSERT_FALSE(cursor.eof());
    EXPECT_EQ(cursor.Value<int64_t>(0), 9);
    EXPECT_EQ(source.pulls, 2u);
    EXPECT_EQ(observed->evaluated, 4u);
    // The consumer stops after one row without requesting another batch.
  }
  EXPECT_EQ(source.pulls, 2u);
}

TEST(ExecutorContractTest, SourceFailureDoesNotFinalizeBufferedOperators) {
  ContractSource source({{11}}, true);
  auto probe = std::make_unique<FinishProbe>();
  auto* observed = probe.get();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::move(probe));
  Pipeline pipeline(source, std::move(ops));
  RowCursor cursor(pipeline);
  ASSERT_TRUE(cursor.Open());
  EXPECT_EQ(cursor.Value<int64_t>(0), 11);
  EXPECT_FALSE(cursor.Next());
  EXPECT_FALSE(cursor.status().ok());
  EXPECT_EQ(observed->finishes, 0u);
  uint32_t pulls = source.pulls;
  EXPECT_FALSE(cursor.Next());
  EXPECT_EQ(source.pulls, pulls);
}

class ContractFailure final : public Operator {
 public:
  struct State : OperatorState {
    bool failed = false;
  };
  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  OpResult Execute(const RowBatch&,
                   RowBatch& out,
                   OperatorState& state) const override {
    out.Reset();
    state.Cast<State>().failed = true;
    return OpResult::kError;
  }
  base::Status status(const OperatorState& state) const override {
    return state.Cast<const State>().failed ? base::ErrStatus("operator failed")
                                            : base::OkStatus();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().failed = false;
  }
};

TEST(ExecutorContractTest, OperatorFailureDoesNotFinalizeOrPullFollowingBatch) {
  ContractSource source({{1}, {2}});
  auto probe = std::make_unique<FinishProbe>();
  auto* observed = probe.get();
  std::vector<std::unique_ptr<Operator>> ops;
  ops.push_back(std::move(probe));
  ops.push_back(std::make_unique<ContractFailure>());
  Pipeline pipeline(source, std::move(ops));
  RowCursor cursor(pipeline);
  EXPECT_FALSE(cursor.Open());
  EXPECT_FALSE(cursor.status().ok());
  EXPECT_FALSE(cursor.Next());
  EXPECT_EQ(observed->finishes, 0u);
  EXPECT_EQ(source.pulls, 1u);
}

TEST(ExecutorContractTest, ThroughputCombinesButFiniteDemandNeverLooksAhead) {
  for (uint32_t limit : {0u, 1u, UINT32_MAX}) {
    ContractSource source({{}, {1}, {2}, {3}, {4}, {5}});
    ExecutionOptions options;
    options.preference = BatchPreference::kThroughput;
    options.target_batch_rows = 4;
    options.small_batch_rows = 1;
    if (limit != UINT32_MAX)
      options.limit = limit;
    Pipeline pipeline(source, {}, options);
    auto state = pipeline.MakeState();
    RowBatch out;
    if (!limit) {
      EXPECT_FALSE(pipeline.GetData(out, *state));
      EXPECT_EQ(source.pulls, 0u);
    } else if (limit == 1) {
      ASSERT_TRUE(pipeline.GetData(out, *state));
      EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1));
      EXPECT_EQ(source.pulls, 2u);
      EXPECT_FALSE(pipeline.GetData(out, *state));
      EXPECT_EQ(source.pulls, 2u);
    } else {
      ASSERT_TRUE(pipeline.GetData(out, *state));
      EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 2, 3, 4));
      ASSERT_TRUE(pipeline.GetData(out, *state));
      EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(5));
      EXPECT_FALSE(pipeline.GetData(out, *state));
    }
    EXPECT_TRUE(pipeline.status(*state).ok());
  }
}

TEST(ExecutorContractTest,
     CompactionPreservesOrderAndDiscardsPendingOnFailure) {
  for (bool fail : {false, true}) {
    ContractSource source({{9}, {1, 2, 3, 4}, {7}}, fail);
    ExecutionOptions options;
    options.preference = BatchPreference::kThroughput;
    options.target_batch_rows = 4;
    options.small_batch_rows = 1;
    Pipeline pipeline(source, {}, options);
    auto state = pipeline.MakeState();
    RowBatch out;
    std::vector<int64_t> actual;
    while (pipeline.GetData(out, *state)) {
      auto rows = test::ReadColumn<int64_t>(out, 0);
      actual.insert(actual.end(), rows.begin(), rows.end());
    }
    if (fail)
      EXPECT_THAT(actual, ElementsAre(9, 1, 2, 3, 4));
    else
      EXPECT_THAT(actual, ElementsAre(9, 1, 2, 3, 4, 7));
    EXPECT_EQ(pipeline.status(*state).ok(), !fail);
  }
}

TEST(ExecutorContractTest, CancellationDropsPendingRowsAndRewindStartsFresh) {
  ContractSource source({{1}, {}, {}, {2}});
  bool cancel = true;
  ExecutionOptions options;
  options.preference = BatchPreference::kThroughput;
  options.target_batch_rows = 4;
  options.small_batch_rows = 1;
  options.cancelled = [&] { return cancel && source.pulls >= 2; };
  Pipeline pipeline(source, {}, options);
  auto state = pipeline.MakeState();
  RowBatch out;
  EXPECT_FALSE(pipeline.GetData(out, *state));
  EXPECT_EQ(out.size(), 0u);
  EXPECT_FALSE(pipeline.status(*state).ok());
  EXPECT_EQ(source.pulls, 2u);
  cancel = false;
  pipeline.Rewind(*state);
  ASSERT_TRUE(pipeline.GetData(out, *state));
  EXPECT_THAT(test::ReadColumn<int64_t>(out, 0), ElementsAre(1, 2));
  EXPECT_FALSE(pipeline.GetData(out, *state));
  EXPECT_TRUE(pipeline.status(*state).ok());
}

TEST(ExecutorContractTest, CombiningMixedBackingOnlyPacksComputedValues) {
  auto source = std::make_shared<std::vector<int64_t>>(
      std::initializer_list<int64_t>{10, 20, 30, 40});
  BatchBuffer buffer;
  RowStore store;
  for (uint32_t i : {3u, 1u, 3u, 0u}) {
    auto computed = std::make_shared<std::vector<int64_t>>(1, 100 + i);
    RowBatch in;
    auto original = ColumnView::Reference(StorageType{Int64{}}, source->data());
    original.SetRange(i);
    in.AddColumn(original, source);
    in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, computed->data()),
                 computed);
    in.SetCardinality(1);
    ASSERT_TRUE(buffer.Append(in).ok());
    ASSERT_TRUE(store.Append(in).ok());
  }
  RowBatch output, retained;
  buffer.Take(output);
  EXPECT_EQ(output.column(0).data(), source->data());
  EXPECT_THAT(test::ReadColumn<int64_t>(output, 0),
              ElementsAre(40, 20, 40, 10));
  EXPECT_THAT(test::ReadColumn<int64_t>(output, 1),
              ElementsAre(103, 101, 103, 100));
  // Reordering crosses computed buffers while retaining the stable column.
  std::vector<uint32_t> order = {3, 0, 3, 1};
  RowBatch reordered, second_view;
  auto rows = Span<const uint32_t>(order.data(), order.data() + order.size());
  ASSERT_EQ(store.View(&reordered, rows), 4u);
  EXPECT_EQ(reordered.column(0).data(), source->data());
  ASSERT_EQ(store.View(&second_view, rows), 4u);
  store.Clear();
  EXPECT_THAT(test::ReadColumn<int64_t>(reordered, 0),
              ElementsAre(10, 40, 10, 20));
  EXPECT_THAT(test::ReadColumn<int64_t>(reordered, 1),
              ElementsAre(100, 103, 100, 101));
  retained.CopyFrom(output);
  output.Reset();
  // A later use must not overwrite the packed column retained above.
  for (int64_t value : {7, 8}) {
    auto data = std::make_shared<std::vector<int64_t>>(1, value);
    RowBatch in;
    in.AddColumn(ColumnView::Reference(StorageType{Int64{}}, data->data()),
                 data);
    in.SetCardinality(1);
    ASSERT_TRUE(buffer.Append(in).ok());
  }
  buffer.Take(output);
  EXPECT_THAT(test::ReadColumn<int64_t>(retained, 1),
              ElementsAre(103, 101, 103, 100));
}

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
