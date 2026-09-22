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

#include "src/trace_processor/core/exec/interval_join.h"

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/batch_order.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/pipeline.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Intervals shaped like the slices of one thread: nested runs of depth up to
// four, so the operand overlaps itself, spread over `count` starts.
struct Intervals {
  std::vector<int64_t> ts;
  std::vector<int64_t> dur;
};

Intervals SliceLike(uint32_t count, uint32_t seed) {
  std::minstd_rand rng(seed);
  Intervals out;
  int64_t now = 0;
  while (out.ts.size() < count) {
    now += static_cast<int64_t>(rng() % 1000);
    int64_t outer = static_cast<int64_t>(100 + rng() % 5000);
    out.ts.push_back(now);
    out.dur.push_back(outer);
    int64_t inner_start = now;
    for (uint32_t depth = 1; depth < 4 && out.ts.size() < count; ++depth) {
      int64_t inner = outer / (depth + 1);
      inner_start += static_cast<int64_t>(rng() % 50);
      out.ts.push_back(inner_start);
      out.dur.push_back(inner);
    }
    now += outer;
  }
  return out;
}

// Emits ts and dur as two Int64 columns.
class IntervalSource final : public Source {
 public:
  explicit IntervalSource(const Intervals& intervals) : intervals_(intervals) {}

  std::unique_ptr<OperatorState> MakeState() const override {
    return std::make_unique<State>();
  }
  void Rewind(OperatorState& state) const override {
    state.Cast<State>().offset = 0;
  }
  bool GetData(RowBatch& out, OperatorState& state) const override {
    State& s = state.Cast<State>();
    auto total = static_cast<uint32_t>(intervals_.ts.size());
    if (s.offset == total) {
      return false;
    }
    uint32_t count = std::min(kMaxBatchRows, total - s.offset);
    out.Reset();
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, intervals_.ts.data()));
    out.AddColumn(
        ColumnView::Reference(StorageType{Int64{}}, intervals_.dur.data()));
    out.Compose(RowSelection::Range(s.offset), count);
    out.SetCardinality(count);
    s.offset += count;
    return true;
  }

 private:
  struct State : OperatorState {
    uint32_t offset = 0;
  };
  const Intervals& intervals_;
};

// Every (query, operand) overlap pair through the join operator: the build of
// the operand's lane and one probe per query, plus the batch plumbing.
void BM_IntervalJoin_OverlappingOperand(benchmark::State& state) {
  Intervals operand = SliceLike(static_cast<uint32_t>(state.range(0)), 1);
  Intervals queries = SliceLike(static_cast<uint32_t>(state.range(0)), 2);
  IntervalSource operand_source(operand);
  IntervalSource query_source(queries);
  IntervalJoinSpec spec;
  spec.input.ts_column = spec.operand.ts_column = 0;
  spec.input.dur_column = spec.operand.dur_column = 1;
  spec.operand_column_count = 2;
  std::vector<std::unique_ptr<Operator>> operators;
  operators.push_back(std::make_unique<BatchOrder>(std::vector<uint32_t>{0}));
  operators.push_back(std::make_unique<IntervalJoin>(operand_source, spec));
  Pipeline pipeline(query_source, std::move(operators));

  uint64_t pairs = 0;
  for (auto _ : state) {
    std::unique_ptr<OperatorState> run = pipeline.MakeState();
    RowBatch batch;
    while (pipeline.GetData(batch, *run)) {
      pairs += batch.size();
    }
  }
  state.counters["pairs"] =
      static_cast<double>(pairs) / static_cast<double>(state.iterations());
}

// The same pairs through IntervalTree: build, then FindOverlaps per query.
void BM_IntervalTree_OverlappingOperand(benchmark::State& state) {
  Intervals operand = SliceLike(static_cast<uint32_t>(state.range(0)), 1);
  Intervals queries = SliceLike(static_cast<uint32_t>(state.range(0)), 2);
  std::vector<perfetto::trace_processor::Interval> sorted;
  for (uint32_t i = 0; i < operand.ts.size(); ++i) {
    sorted.push_back({static_cast<perfetto::trace_processor::Ts>(operand.ts[i]),
                      static_cast<perfetto::trace_processor::Ts>(
                          operand.ts[i] + operand.dur[i]),
                      i});
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const perfetto::trace_processor::Interval& a,
               const perfetto::trace_processor::Interval& b) {
              return a.start < b.start;
            });

  uint64_t pairs = 0;
  std::vector<perfetto::trace_processor::Id> found;
  for (auto _ : state) {
    IntervalTree tree(sorted);
    for (uint32_t i = 0; i < queries.ts.size(); ++i) {
      found.clear();
      tree.FindOverlaps(
          static_cast<perfetto::trace_processor::Ts>(queries.ts[i]),
          static_cast<perfetto::trace_processor::Ts>(queries.ts[i] +
                                                     queries.dur[i]),
          found);
      pairs += found.size();
    }
  }
  state.counters["pairs"] =
      static_cast<double>(pairs) / static_cast<double>(state.iterations());
}

BENCHMARK(BM_IntervalJoin_OverlappingOperand)->Arg(10000)->Arg(100000);
BENCHMARK(BM_IntervalTree_OverlappingOperand)->Arg(10000)->Arg(100000);

}  // namespace
}  // namespace perfetto::trace_processor::core::exec
