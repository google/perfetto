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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_JOIN_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_JOIN_H_

#include <cstdint>
#include <memory>

#include "perfetto/base/status.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/arena.h"

namespace perfetto::trace_processor::core::exec {

// The build (right) side of a join. It holds the right side's rows and checks
// that their keys are consecutive, as a table's ids are. That way the row for
// a key is found by subtracting the first key, with no hash table. Keys that
// aren't consecutive are not supported yet.
class JoinBuild final : public Sink {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  // `key` is the column holding the Int64 join keys. `unread[c]` is true for
  // each column the join doesn't output; those columns aren't stored.
  JoinBuild(uint32_t key, core::ArenaSpan<bool> unread);

  std::unique_ptr<OperatorState> MakeSinkState(Context&) const override;
  base::Status ConsumeBatch(const RowBatch& in,
                            OperatorState& state) const override;
  base::Status FinalizeSink(OperatorState& state) const override;

 private:
  friend class Join;
  struct Built;

  uint32_t key_;
  core::ArenaSpan<bool> unread_;
};

struct JoinSpec {
  enum class Kind : uint8_t { kInner, kLeft };
  // A build-side column to output. If the build side has no rows there is no
  // column to copy the type from, so its nulls get the type `fallback`.
  struct Output {
    uint32_t column;
    StorageType fallback{Int64{}};
  };

  Kind kind;
  // The input column holding the join key. It must be an integer.
  uint32_t probe_key;
  // Which of the plan's pipelines ends in the JoinBuild.
  uint32_t build;
  core::ArenaSpan<Output> outputs;
};

// Joins each input row to the build row with the same key (see JoinBuild) and
// appends that row's columns. An inner join drops input rows with no match. A
// left join keeps them, with nulls in the build columns, and outputs them
// after the matched rows of the same batch. Build keys are unique, so each
// input row is output at most once.
class Join final : public Operator {
 public:
  static constexpr char kType = 0;
  const void* type() const override { return &kType; }

  explicit Join(JoinSpec spec);

  void DescribeColumns(StepColumns* out) const override;
  std::unique_ptr<OperatorState> MakeState(Context&) const override;
  OpResult Execute(const RowBatch& in,
                   RowBatch& out,
                   OperatorState& state) const override;
  base::Status status(const OperatorState& state) const override;

 private:
  friend class PipelineFormatter;
  struct State;

  // Outputs the rows of `in` that have a match. For a left join where some
  // rows didn't match, it arranges for the next call to output those.
  void OutputMatched(const RowBatch& in,
                     JoinBuild::Built& b,
                     RowBatch& out,
                     State& s) const;
  // Outputs the rows that didn't match in the previous call, with nulls in
  // the build columns.
  void OutputMissed(const JoinBuild::Built& b, RowBatch& out, State& s) const;

  JoinSpec spec_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_JOIN_H_
