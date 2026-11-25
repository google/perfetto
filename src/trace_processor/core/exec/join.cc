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

#include "src/trace_processor/core/exec/join.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/run_held.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/exec/sink.h"
#include "src/trace_processor/core/util/bit_vector.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// A flat column's values, starting at the column's first row.
template <typename T>
const T* Values(const ColumnView& column) {
  return static_cast<const T*>(column.data()) + column.start();
}

// The build row of an input row that has no match.
constexpr uint32_t kNoBuildRow = UINT32_MAX;

// For each of the first `rows` keys, writes its build row to `paired`. The
// build keys run from `first` to `first + count - 1`, so a key's build row is
// `key - first`, or kNoBuildRow if that's out of range. This is pipitdb's
// Direct::pair. It covers every row, selected or not, so the loop vectorizes.
// Returns true if any key has no match.
template <typename T>
PERFETTO_NO_INLINE bool Pair(const T* keys,
                             uint32_t rows,
                             uint64_t first,
                             uint64_t count,
                             uint32_t* paired) {
  uint32_t missing = 0;
  PERFETTO_VECTORIZE_LOOP_ONCE
  for (uint32_t row = 0; row < rows; ++row) {
    uint64_t above =
        static_cast<uint64_t>(static_cast<int64_t>(keys[row])) - first;
    bool found = above < count;
    paired[row] = found ? static_cast<uint32_t>(above) : kNoBuildRow;
    missing += !found;
  }
  return missing != 0;
}

// Sets kNoBuildRow for each of the first `rows` rows whose key is null.
void MarkNullKeys(const ColumnView& key, uint32_t rows, uint32_t* paired) {
  const BitVector& validity = *key.validity();
  for (uint32_t row = 0; row < rows; ++row) {
    if (!validity.is_set(key.start() + row)) {
      paired[row] = kNoBuildRow;
    }
  }
}

// Runs Pair() on every row of `in`, reading column `key_column` as whichever
// integer type it's stored as. Null keys never match. Returns true if any row
// has no match.
bool PairKey(const RowBatch& in,
             uint32_t key_column,
             uint64_t first,
             uint64_t count,
             uint32_t* paired) {
  const ColumnView& key = in.column(key_column);
  uint32_t rows = in.row_count();
  bool missing;
  if (key.kind() == ColumnView::Kind::kSequence) {
    uint32_t start = key.start();
    uint32_t missed = 0;
    PERFETTO_VECTORIZE_LOOP_ONCE
    for (uint32_t row = 0; row < rows; ++row) {
      uint64_t above = uint64_t{start + row} - first;
      bool found = above < count;
      paired[row] = found ? static_cast<uint32_t>(above) : kNoBuildRow;
      missed += !found;
    }
    missing = missed != 0;
  } else if (key.type().Is<Int64>()) {
    missing = Pair(Values<int64_t>(key), rows, first, count, paired);
  } else if (key.type().Is<Int32>()) {
    missing = Pair(Values<int32_t>(key), rows, first, count, paired);
  } else {
    PERFETTO_DCHECK((key.type().IsAnyOf<base::TypeSet<Id, Uint32>>()));
    missing = Pair(Values<uint32_t>(key), rows, first, count, paired);
  }
  if (key.validity()) {
    MarkNullKeys(key, rows, paired);
    return true;
  }
  return missing;
}

// Adds an all-null column for each build-side output. Each column has the
// kind and type of the build rows, when there are any.
void AddNullColumns(const JoinSpec& spec,
                    const RowStore& built,
                    Context& context,
                    RowBatch& out) {
  const RowBatch* shape = built.shape();
  for (const JoinSpec::Output& output : spec.outputs) {
    if (shape) {
      ColumnShape like = ColumnShape::Of(shape->column(output.column));
      out.AddNullColumnLike(&like, output.fallback, context);
    } else {
      out.AddNullColumnLike(nullptr, output.fallback, context);
    }
  }
}

}  // namespace

// JoinBuild

struct JoinBuild::Built : OperatorState {
  ~Built() override;

  Context* context = nullptr;
  Held<RowStore> rows{this};
  // The first build key, and whether each key so far is one more than the
  // one before it.
  RunValue<int64_t> first{this, 0};
  RunValue<bool> sequence{this, true};
};

JoinBuild::Built::~Built() = default;

JoinBuild::JoinBuild(uint32_t key, core::ArenaSpan<bool> unread)
    : key_(key), unread_(unread) {}

JoinBuild::~JoinBuild() = default;

std::unique_ptr<OperatorState> JoinBuild::MakeSinkState(
    Context& context) const {
  auto built = std::make_unique<Built>();
  built->context = &context;
  built->rows.SetUnread(unread_.span());
  return built;
}

base::Status JoinBuild::ConsumeBatch(const RowBatch& in,
                                     OperatorState& state) const {
  Built& b = state.Cast<Built>();
  if (b.sequence && in.size()) {
    // Like pipitdb's consecutive(), but not vectorized: the build side is
    // read only once, so smaller code matters more than speed here.
    const ColumnView& key = in.column(key_);
    const int64_t* keys = Values<int64_t>(key);
    const Selection& selection = in.selection();
    uint32_t count = b.rows.size();
    if (!count) {
      b.first = keys[selection[0]];
    }
    auto expected = static_cast<uint64_t>(*b.first) + count;
    bool all = true;
    PERFETTO_NO_VECTORIZE_LOOP
    for (uint32_t i = 0; i < in.size(); ++i) {
      uint32_t row = selection[i];
      all &=
          key.IsValid(row) && static_cast<uint64_t>(keys[row]) == expected + i;
    }
    b.sequence = all;
  }
  return b.rows.Append(in, *b.context);
}

base::Status JoinBuild::FinalizeSink(OperatorState& state) const {
  if (!state.Cast<Built>().sequence) {
    return base::ErrStatus(
        "JOIN: only a right side whose keys are a sequence, such as a "
        "table's ids, is supported yet");
  }
  return base::OkStatus();
}

// Join

struct Join::State : OperatorState {
  State(Context& c, uint32_t build) : context(&c), build_use(this, build) {}
  ~State() override;

  Context* context;
  PipelineUse build_use;
  Held<base::Status> status{this};
  Held<RowBatch> gathered{this};
  // Set for a left join when some rows didn't match. The next call outputs
  // those rows, reading `paired` from this call.
  RunValue<bool> unmatched{this, false};
  // For each input row, its build row or kNoBuildRow.
  uint32_t paired[kMaxBatchRows];
};

Join::State::~State() = default;

Join::Join(JoinSpec spec) : spec_(spec) {}

Join::~Join() = default;

void Join::DescribeColumns(StepColumns* out) const {
  out->keeps = StepColumns::Keeps::kAll;
  out->added = static_cast<uint32_t>(spec_.outputs.size());
  out->reads = {spec_.probe_key};
}

std::unique_ptr<OperatorState> Join::MakeState(Context& context) const {
  return std::make_unique<State>(context, spec_.build);
}

base::Status Join::status(const OperatorState& state) const {
  return state.Cast<const State>().status;
}

OpResult Join::Execute(const RowBatch& in,
                       RowBatch& out,
                       OperatorState& state) const {
  State& s = state.Cast<State>();
  base::StatusOr<OperatorState*> built = s.build_use.Built(*s.context);
  if (!built.ok()) {
    s.status = std::move(built).status();
    return OpResult::kError;
  }
  auto& b = (*built)->Cast<JoinBuild::Built>();
  out.CopyFrom(in);
  if (s.unmatched) {
    OutputMissed(b, out, s);
    return OpResult::kNeedMoreInput;
  }
  const ColumnView& key = in.column(spec_.probe_key);
  if (key.kind() != ColumnView::Kind::kSequence &&
      !key.type().IsAnyOf<IntegerType>()) {
    s.status = base::ErrStatus("JOIN: the key is not an integer");
    return OpResult::kError;
  }
  OutputMatched(in, b, out, s);
  return s.unmatched ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
}

void Join::OutputMatched(const RowBatch& in,
                         JoinBuild::Built& b,
                         RowBatch& out,
                         State& s) const {
  if (PairKey(in, spec_.probe_key, static_cast<uint64_t>(*b.first),
              b.rows.size(), s.paired)) {
    out.mutable_selection().KeepMatching(s.paired, kNoBuildRow, false);
  }
  if (out.size() && !spec_.outputs.empty()) {
    b.rows.Gather(&s.gathered, {s.paired, s.paired + in.row_count()},
                  &out.selection(), *s.context);
    for (const JoinSpec::Output& output : spec_.outputs) {
      out.AddColumn(s.gathered.column(output.column),
                    s.gathered.buffer(output.column));
    }
  } else {
    AddNullColumns(spec_, b.rows, *s.context, out);
  }
  s.unmatched = spec_.kind == JoinSpec::Kind::kLeft && out.size() != in.size();
}

void Join::OutputMissed(const JoinBuild::Built& b,
                        RowBatch& out,
                        State& s) const {
  s.unmatched = false;
  out.mutable_selection().KeepMatching(s.paired, kNoBuildRow, true);
  AddNullColumns(spec_, b.rows, *s.context, out);
}

}  // namespace perfetto::trace_processor::core::exec
