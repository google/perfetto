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
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
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

// Intervals are held as the unsigned timestamps `IntervalIntersector` takes.
// Flipping the sign bit maps int64 onto uint64 keeping the order, so a
// negative timestamp needs no rejecting and a search still works: the least
// int64 becomes zero and the greatest becomes the greatest uint64.
PERFETTO_ALWAYS_INLINE Ts ToTs(int64_t v) {
  return static_cast<Ts>(v) ^ (uint64_t{1} << 63);
}

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

// The operand rows sharing one key: their intervals sorted by start, the row
// each came from, and the intersector which searches them. `Interval::id` is
// the position in `intervals`, so a result leads back to both.
struct Lane {
  std::vector<Interval> intervals;
  std::vector<uint32_t> rows;
  std::unique_ptr<IntervalIntersector> intersector;
};

class JoinState : public OperatorState {
 public:
  ~JoinState() override;

  std::unique_ptr<OperatorState> operand;
  bool built = false;
  RowStore store;
  std::vector<Lane> lanes;
  // Which lane a key is in. `lane_by_key` serves a key of one column, which
  // is the common case; `lane_by_keys` holds the bytes of a wider one.
  base::FlatHashMap<int64_t, uint32_t> lane_by_key;
  base::FlatHashMap<std::string, uint32_t> lane_by_keys;
  // The all-null operand row unmatched input rows are joined with.
  uint32_t null_row = 0;

  // The joined pairs of the current input batch, as its input row and the
  // stored operand row, filled a batch's worth of input rows at a time and
  // served a batch's worth of pairs at a time. `input_row` is the next input
  // row to join; `served` how many pairs have already gone out.
  std::vector<uint32_t> input_rows;
  std::vector<uint32_t> operand_rows;
  uint32_t input_row = 0;
  uint32_t served = 0;
  RowBatch operand_batch;

  std::vector<int64_t> key_scratch;
  // Where a search leaves the positions it found, reused by every row.
  std::vector<uint32_t> found;
  base::Status status = base::OkStatus();
};

JoinState::~JoinState() = default;

std::string KeyBytes(const std::vector<int64_t>& key) {
  return {reinterpret_cast<const char*>(key.data()),
          key.size() * sizeof(int64_t)};
}

// Whether an operand interval [a, b) relates to an input interval [s, e).
// Written out from the definitions so the searches below only have to find a
// superset of what relates.
PERFETTO_ALWAYS_INLINE bool Relates(IntervalRelationship relationship,
                                    int64_t s,
                                    int64_t e,
                                    int64_t a,
                                    int64_t b) {
  using R = IntervalRelationship;
  switch (relationship) {
    case R::kOverlappingBounds:
    case R::kCoveringBegin:
      // Both are exactly what the search asked for.
      return true;
    case R::kCoveringEnd:
      return a < e && e <= b;
    case R::kCoveringBounds:
      return a <= s && e <= b;
    case R::kWithinBounds:
      return s <= a && b <= e && (a < e || s == e);
  }
  PERFETTO_FATAL("For GCC");
}

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
  return state;
}

void IntervalJoin::Rewind(OperatorState& state) const {
  JoinState& s = state.Cast<JoinState>();
  s.operand.reset();
  s.built = false;
  s.store.Clear();
  s.lanes.clear();
  s.lane_by_key.Clear();
  s.lane_by_keys.Clear();
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

// Reads the whole operand into `s.store` and lays its intervals out by key.
base::Status Build(const Source& operand,
                   const IntervalJoinSpec& spec,
                   uint32_t queries,
                   JoinState& s) {
  size_t key_size = spec.operand.key_columns.size();
  if (key_size == 0) {
    s.lanes.emplace_back();
  }
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
      if (!reader.ReadBounds(row, &start, &end) ||
          !reader.ReadKey(row, s.key_scratch.data())) {
        continue;
      }
      uint32_t lane = 0;
      if (key_size == 1) {
        auto [it, inserted] = s.lane_by_key.Insert(
            s.key_scratch[0], static_cast<uint32_t>(s.lanes.size()));
        lane = *it;
      } else if (key_size > 1) {
        auto [it, inserted] = s.lane_by_keys.Insert(
            KeyBytes(s.key_scratch), static_cast<uint32_t>(s.lanes.size()));
        lane = *it;
      }
      if (lane == s.lanes.size()) {
        s.lanes.emplace_back();
      }
      s.lanes[lane].intervals.push_back({ToTs(start), ToTs(end), 0});
      s.lanes[lane].rows.push_back(s.store.size() + row);
    }
    RETURN_IF_ERROR(s.store.Append(batch));
  }
  RETURN_IF_ERROR(operand.status(*s.operand));

  for (Lane& lane : s.lanes) {
    // The search wants them by start; the row each came from follows along,
    // and the id left behind leads back to both.
    std::vector<uint32_t> order(lane.intervals.size());
    for (uint32_t i = 0; i < order.size(); ++i) {
      order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
      const Interval& x = lane.intervals[a];
      const Interval& y = lane.intervals[b];
      return x.start != y.start ? x.start < y.start : a < b;
    });
    std::vector<Interval> sorted(order.size());
    std::vector<uint32_t> rows(order.size());
    bool nonoverlapping = true;
    for (uint32_t i = 0; i < order.size(); ++i) {
      sorted[i] = lane.intervals[order[i]];
      sorted[i].id = i;
      rows[i] = lane.rows[order[i]];
      nonoverlapping =
          nonoverlapping && (i == 0 || sorted[i - 1].end <= sorted[i].start);
    }
    lane.intervals = std::move(sorted);
    lane.rows = std::move(rows);
    // The intersector keeps a reference to the intervals, which the lane owns
    // and does not touch again.
    lane.intersector = std::make_unique<IntervalIntersector>(
        lane.intervals,
        IntervalIntersector::DecideMode(nonoverlapping, queries));
  }
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

// Appends a pair for each operand row of `lane` which `[start, end)` relates
// to, in the order the lane holds them.
void FindMatches(const Lane& lane,
                 IntervalRelationship relationship,
                 uint32_t row,
                 int64_t start,
                 int64_t end,
                 JoinState& s) {
  using R = IntervalRelationship;
  // An interval covering the whole of [s, e) covers s, and a point relates
  // only where it lies, so those ask about the one instant and the rest about
  // the whole span. Either way the search returns every interval which could
  // relate, and `Relates` settles the ones it cannot tell apart.
  bool at_start = start == end || relationship == R::kCoveringBegin ||
                  relationship == R::kCoveringBounds;
  Ts s_ts = ToTs(start);
  Ts e_ts = at_start ? s_ts : ToTs(end);

  s.found.clear();
  lane.intersector->FindOverlaps(s_ts, e_ts, s.found);
  // A search may return them in whatever order suits it, a tree by its own
  // shape; an id is the position in the lane, so sorting puts the matches of
  // one row back in the order the lane holds them.
  std::sort(s.found.begin(), s.found.end());
  // A point relates to whatever holds it, whichever relationship was asked
  // for, except that lying within a span still asks about the operand's own.
  bool written_out = start != end || relationship == R::kWithinBounds;
  for (uint32_t id : s.found) {
    if (written_out) {
      const Interval& interval = lane.intervals[id];
      // Back to signed to compare as the definitions are written.
      int64_t a = static_cast<int64_t>(interval.start ^ (uint64_t{1} << 63));
      int64_t b = static_cast<int64_t>(interval.end ^ (uint64_t{1} << 63));
      if (!Relates(relationship, start, end, a, b)) {
        continue;
      }
    }
    s.input_rows.push_back(row);
    s.operand_rows.push_back(lane.rows[id]);
  }
}

}  // namespace

OpResult IntervalJoin::Execute(const RowBatch& in,
                               RowBatch& out,
                               OperatorState& state) const {
  JoinState& s = state.Cast<JoinState>();
  if (!s.built) {
    // The first batch is a lower bound on how often the operand is searched,
    // which is what tells the intersector whether a tree is worth building.
    s.status = Build(operand_, spec_, in.size(), s);
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
    // Enough input rows to fill a batch of pairs, however many that is: a
    // row can have more matches than a batch holds, and then they are served
    // over several calls.
    SideReader reader(in, spec_.input);
    size_t key_size = spec_.input.key_columns.size();
    for (; s.input_row < in.size() && s.input_rows.size() < kMaxBatchRows;
         ++s.input_row) {
      int64_t start;
      int64_t end;
      if (!reader.ReadBounds(s.input_row, &start, &end) ||
          !reader.ReadKey(s.input_row, s.key_scratch.data())) {
        // A null never joins.
      } else if (key_size == 0) {
        FindMatches(s.lanes[0], spec_.relationship, s.input_row, start, end, s);
      } else {
        const uint32_t* lane =
            key_size == 1 ? s.lane_by_key.Find(s.key_scratch[0])
                          : s.lane_by_keys.Find(KeyBytes(s.key_scratch));
        if (lane) {
          FindMatches(s.lanes[*lane], spec_.relationship, s.input_row, start,
                      end, s);
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
