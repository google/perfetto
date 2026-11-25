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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_TOP_K_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_TOP_K_H_

#include <cstdint>
#include <optional>
#include <utility>

#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/exec/breaker.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/run_vector.h"
#include "src/trace_processor/core/exec/sorted_rows.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

struct TopKSpec {
  core::ArenaSpan<SortKey> keys;
  // Holds the strings of the string keys, as for SortSpec.
  const StringPool* pool = nullptr;
  // How many rows to output.
  uint32_t count = 0;
};

// Outputs the first `count` rows in the order Sort would put them: ORDER BY
// with LIMIT.
//
// Once it holds `count` rows plus `count` or a batch's worth more, whichever is
// larger, it sorts them and keeps only the first `count`, by appending them in
// order to rows held afresh. The last of those becomes the cutoff: from then
// on, rows which do not come before it are dropped as they arrive, so on most
// inputs pruning soon becomes rare.
class TopK final : public Breaker {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }
  void DescribeColumns(StepColumns* out) const override {
    for (const SortKey& key : spec_.keys) {
      out->reads.push_back(key.column);
      out->held_reads.push_back(key.column);
    }
  }
  void SetUnreadColumns(core::ArenaSpan<bool> unread) override {
    unread_ = unread;
  }

  explicit TopK(TopKSpec);

 private:
  friend class PipelineFormatter;

  struct State : Breaker::State {
    // The rows held. Pruning appends the first of them to `spare`, which
    // then takes over.
    SortedRows rows;
    SortedRows spare;
    // The cutoff's keys, laid out, and the strings of its string keys, by
    // key. Set once the rows are first pruned.
    bool has_cutoff = false;
    RunVector<uint8_t> cutoff;
    RunVector<std::optional<StringPool::Id>> cutoff_strings;
    // The keys of a batch.
    RunVector<uint8_t> keys;
    uint32_t emitted = 0;
    // Scratch for one call: the rows of a batch which come before the
    // cutoff, and a reader of each string key.
    core::ArenaSpan<uint32_t> passing;
    core::ArenaSpan<FlatColumnReader<StringPool::Id>> strings;
    RowBatch* passed = nullptr;
  };

  OperatorState* NewSinkState(Context&) const override;
  bool Consume(const RowBatch& in, Breaker::State& state) const override;
  bool Finalize(Breaker::State& state) const override;
  bool Serve(RowBatch& out, Breaker::State& state) const override;

  base::Status Add(const RowBatch& in, State& s) const;
  // The rows of `in` which come before the cutoff, their keys laid out in
  // `s.keys`.
  const RowBatch& BeforeCutoff(const RowBatch& in, State& s) const;
  // Compares the row `row`, its keys laid out at `keys`, to the cutoff.
  int CompareToCutoff(uint32_t row, const uint8_t* keys, const State& s) const;
  base::Status Prune(State& s) const;

  TopKSpec spec_;
  // Input columns no later step reads (see Operator::SetUnreadColumns).
  core::ArenaSpan<bool> unread_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_TOP_K_H_
