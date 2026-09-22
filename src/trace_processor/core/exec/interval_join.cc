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

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/interval_index.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/variant.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

constexpr int64_t kNeverEnds = std::numeric_limits<int64_t>::max();

// Reads one nullable Int64 column of a batch.
struct Int64Reader {
  explicit Int64Reader(const ColumnView& column)
      : data(static_cast<const int64_t*>(column.data())),
        selection(column.selection()),
        validity(column.validity()) {}

  PERFETTO_ALWAYS_INLINE bool Read(uint32_t row, int64_t* out) const {
    uint32_t index = selection.GetIndex(row);
    if (validity && !validity->is_set(index)) {
      return false;
    }
    *out = data[index];
    return true;
  }

  const int64_t* data;
  RowSelection selection;
  const BitVector* validity;
};

// Reads the interval and key of each row of one side of the join.
class SideReader {
 public:
  SideReader(const RowBatch& batch, const IntervalJoinSpec::Side& side)
      : ts_(batch.column(side.ts_column)) {
    if (side.dur_column) {
      dur_.emplace(batch.column(*side.dur_column));
    }
    for (uint32_t column : side.key_columns) {
      keys_.emplace_back(batch.column(column));
    }
  }

  // Returns false if the row holds a null and so cannot join.
  PERFETTO_ALWAYS_INLINE bool ReadBounds(uint32_t row,
                                         int64_t* start,
                                         int64_t* end) const {
    if (!ts_.Read(row, start)) {
      return false;
    }
    int64_t dur = 0;
    if (dur_ && !dur_->Read(row, &dur)) {
      return false;
    }
    if (dur < 0 || *start > kNeverEnds - dur) {
      *end = kNeverEnds;
    } else {
      *end = *start + dur;
    }
    return true;
  }

  PERFETTO_ALWAYS_INLINE bool ReadKey(uint32_t row, int64_t* out) const {
    for (uint32_t i = 0; i < keys_.size(); ++i) {
      if (!keys_[i].Read(row, &out[i])) {
        return false;
      }
    }
    return true;
  }

 private:
  Int64Reader ts_;
  std::optional<Int64Reader> dur_;
  base::SmallVector<Int64Reader, 4> keys_;
};

base::Status ValidateSide(const RowBatch& batch,
                          const IntervalJoinSpec::Side& side,
                          const char* name) {
  auto is_int64 = [&](uint32_t index) {
    const ColumnView& column = batch.column(index);
    return column.kind() == ColumnView::Kind::kFlat &&
           column.type().Is<Int64>();
  };
  bool ok =
      is_int64(side.ts_column) &&
      (!side.dur_column || is_int64(*side.dur_column)) &&
      std::all_of(side.key_columns.begin(), side.key_columns.end(), is_int64);
  return ok ? base::OkStatus()
            : base::ErrStatus(
                  "INTERVAL JOIN: the %s ts, dur and PER columns must be Int64",
                  name);
}

class JoinState : public OperatorState {
 public:
  JoinState() : sweep(index) {}
  ~JoinState() override;

  std::unique_ptr<OperatorState> operand;
  bool built = false;
  RowStore store;
  IntervalIndex index;
  // The all-null operand row unmatched input rows are joined with.
  uint32_t null_row = 0;

  // The run of one key the input is in: the key itself, and the sweep down
  // its lane, which `in_lane` says the key has.
  IntervalIndex::Sweep sweep;
  bool in_lane = false;
  bool in_run = false;
  std::vector<int64_t> run_key;
  std::vector<int64_t> key_scratch;

  // The joined pairs of the current input batch, as its input row and the
  // stored operand row, filled a batch's worth of input rows at a time and
  // served a batch's worth of pairs at a time. `input_row` is the next input
  // row to join; `served` how many pairs have already gone out.
  std::vector<uint32_t> input_rows;
  std::vector<uint32_t> operand_rows;
  uint32_t input_row = 0;
  uint32_t served = 0;
  RowBatch operand_batch;

  base::Status status = base::OkStatus();
};

JoinState::~JoinState() = default;

}  // namespace

IntervalJoin::IntervalJoin(const Source& operand, IntervalJoinSpec spec)
    : operand_(operand), spec_(std::move(spec)) {
  PERFETTO_CHECK(spec_.input.key_columns.size() ==
                 spec_.operand.key_columns.size());
}

IntervalJoin::~IntervalJoin() = default;

std::unique_ptr<OperatorState> IntervalJoin::MakeState() const {
  auto state = std::make_unique<JoinState>();
  state->key_scratch.resize(spec_.input.key_columns.size());
  state->run_key.resize(spec_.input.key_columns.size());
  return state;
}

void IntervalJoin::Rewind(OperatorState& state) const {
  JoinState& s = state.Cast<JoinState>();
  s.operand.reset();
  s.built = false;
  s.store.Clear();
  s.index.Clear();
  s.in_lane = false;
  s.in_run = false;
  s.input_rows.clear();
  s.operand_rows.clear();
  s.input_row = 0;
  s.served = 0;
  s.status = base::OkStatus();
}

base::Status IntervalJoin::status(const OperatorState& state) const {
  return state.Cast<const JoinState>().status;
}

namespace {

// Reads the whole operand into `s.store` and indexes its intervals by key.
base::Status Build(const Source& operand,
                   const IntervalJoinSpec& spec,
                   JoinState& s) {
  size_t key_size = spec.operand.key_columns.size();
  // Made here rather than with the rest of the state: making it can be
  // costly, as preparing a SQL statement is, and an input without rows never
  // reads the operand at all.
  s.operand = operand.MakeState();
  RowBatch batch;
  while (operand.GetData(batch, *s.operand)) {
    RETURN_IF_ERROR(ValidateSide(batch, spec.operand, "operand"));
    SideReader reader(batch, spec.operand);
    for (uint32_t row = 0; row < batch.size(); ++row) {
      int64_t start;
      int64_t end;
      if (reader.ReadBounds(row, &start, &end) &&
          reader.ReadKey(row, s.key_scratch.data())) {
        s.index.Add(s.key_scratch.data(), key_size, start, end,
                    s.store.size() + row);
      }
    }
    RETURN_IF_ERROR(s.store.Append(batch));
  }
  RETURN_IF_ERROR(operand.status(*s.operand));
  s.index.Build();
  if (!spec.keep_unmatched) {
    return base::OkStatus();
  }

  // One more row, null in every column, for unmatched input rows to join with.
  static constexpr int64_t kZero = 0;
  static const Variant kNull = Variant::Null();
  BitVector invalid = BitVector::CreateWithSize(1);
  RowBatch null_batch;
  for (uint32_t i = 0; i < spec.operand_column_count; ++i) {
    // An operand without rows never said what its columns hold.
    bool variant = s.store.column_count() == 0 || s.store.is_variant(i);
    null_batch.AddColumn(
        variant ? ColumnView::Variants(&kNull)
                : ColumnView::Reference(s.store.type(i), &kZero, &invalid));
  }
  null_batch.SetCardinality(1);
  s.null_row = s.store.size();
  return s.store.Append(null_batch);
}

// Appends a pair for each operand interval `[start, end)` relates to, from a
// sweep advanced to `start`. Returns false if `start` is before the previous
// probe of the run, which the sweep cannot follow.
//
// Every relationship is some of the intervals containing `start`, which the
// sweep holds active, and some of those starting inside the probe, which
// follow next() in order; neither needs a search.
PERFETTO_ALWAYS_INLINE bool FindMatches(const IntervalIndex& index,
                                        IntervalIndex::Sweep& sweep,
                                        IntervalRelationship relationship,
                                        uint32_t row,
                                        int64_t start,
                                        int64_t end,
                                        JoinState& s) {
  using R = IntervalRelationship;
  bool point = start == end;
  if (point && relationship != R::kWithinBounds) {
    relationship = R::kCoveringBegin;
  }
  if (!sweep.Advance(start)) {
    return false;
  }
  auto emit = [&](uint32_t i) {
    s.input_rows.push_back(row);
    s.operand_rows.push_back(index.row(i));
  };
  // The intervals starting inside the probe, while `keep` holds.
  auto starting_inside = [&](int64_t before, auto keep) {
    for (uint32_t i = sweep.next();
         i < sweep.limit() && index.start(i) < before; ++i) {
      if (keep(i)) {
        emit(i);
      }
    }
  };
  switch (relationship) {
    case R::kCoveringBegin:
      for (uint32_t i : sweep.active()) {
        emit(i);
      }
      break;
    case R::kOverlappingBounds:
      for (uint32_t i : sweep.active()) {
        emit(i);
      }
      starting_inside(end, [](uint32_t) { return true; });
      break;
    case R::kCoveringBounds:
      for (uint32_t i : sweep.active()) {
        if (index.end(i) >= end) {
          emit(i);
        }
      }
      break;
    case R::kCoveringEnd:
      for (uint32_t i : sweep.active()) {
        if (index.end(i) >= end) {
          emit(i);
        }
      }
      starting_inside(end, [&](uint32_t i) { return index.end(i) >= end; });
      break;
    case R::kWithinBounds: {
      // `end` itself is outside the probe, but is all there is of a point.
      int64_t before =
          point && end != std::numeric_limits<int64_t>::max() ? end + 1 : end;
      for (uint32_t i : sweep.active()) {
        if (index.start(i) >= start && index.end(i) <= end) {
          emit(i);
        }
      }
      starting_inside(before, [&](uint32_t i) { return index.end(i) <= end; });
      break;
    }
  }
  return true;
}

}  // namespace

OpResult IntervalJoin::Execute(const RowBatch& in,
                               RowBatch& out,
                               OperatorState& state) const {
  JoinState& s = state.Cast<JoinState>();
  if (!s.built) {
    s.status = Build(operand_, spec_, s);
    if (!s.status.ok()) {
      return OpResult::kError;
    }
    s.built = true;
  }
  if (s.served == s.input_rows.size()) {
    s.status = ValidateSide(in, spec_.input, "input");
    if (!s.status.ok()) {
      return OpResult::kError;
    }
    s.input_rows.clear();
    s.operand_rows.clear();
    s.served = 0;
    // Rows are ordered within a batch, not across batches, so a new batch
    // starts a new run whatever key it begins with.
    if (s.input_row == 0) {
      s.in_run = false;
    }
    // Enough input rows to fill a batch of pairs, however many that is: a
    // row can have more matches than a batch holds, and then they are served
    // over several calls.
    SideReader reader(in, spec_.input);
    size_t key_size = spec_.input.key_columns.size();
    for (; s.input_row < in.size() && s.input_rows.size() < kMaxBatchRows;
         ++s.input_row) {
      int64_t start;
      int64_t end;
      if (reader.ReadBounds(s.input_row, &start, &end) &&
          reader.ReadKey(s.input_row, s.key_scratch.data())) {
        // A new key starts a new run down its lane.
        bool same_key = s.in_run;
        for (size_t i = 0; same_key && i < key_size; ++i) {
          same_key = s.key_scratch[i] == s.run_key[i];
        }
        if (!same_key) {
          s.in_run = true;
          for (size_t i = 0; i < key_size; ++i) {
            s.run_key[i] = s.key_scratch[i];
          }
          std::optional<uint32_t> lane =
              s.index.FindLane(s.run_key.data(), key_size);
          s.in_lane = lane.has_value();
          if (lane) {
            s.sweep.Start(*lane);
          }
        }
        if (s.in_lane && !FindMatches(s.index, s.sweep, spec_.relationship,
                                      s.input_row, start, end, s)) {
          s.status = base::ErrStatus(
              "INTERVAL JOIN: rows are not ordered by PER keys then ts");
          return OpResult::kError;
        }
      }
      if (spec_.keep_unmatched &&
          (s.input_rows.empty() || s.input_rows.back() != s.input_row)) {
        s.input_rows.push_back(s.input_row);
        s.operand_rows.push_back(s.null_row);
      }
    }
  }

  auto pending = static_cast<uint32_t>(s.input_rows.size()) - s.served;
  uint32_t count = std::min(pending, kMaxBatchRows);
  const uint32_t* input_rows = s.input_rows.data() + s.served;
  const uint32_t* operand_rows = s.operand_rows.data() + s.served;
  s.served += count;
  bool more = s.served < s.input_rows.size() || s.input_row < in.size();
  if (!more) {
    s.input_row = 0;
  }
  if (count == 0) {
    out.Reset();
    return more ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
  }
  out.CopyFrom(in);
  out.Compose(RowSelection::Indices({input_rows, input_rows + count}), count);
  out.SetCardinality(count);
  s.store.View(&s.operand_batch, {operand_rows, operand_rows + count});
  for (uint32_t i = 0; i < s.operand_batch.column_count(); ++i) {
    out.AddColumn(s.operand_batch.column(i));
  }
  return more ? OpResult::kHaveMoreOutput : OpResult::kNeedMoreInput;
}

}  // namespace perfetto::trace_processor::core::exec
