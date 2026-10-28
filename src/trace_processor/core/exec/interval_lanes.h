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

#ifndef SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_LANES_H_
#define SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_LANES_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/utils.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/context.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/exec/selection.h"
#include "src/trace_processor/core/util/span.h"

// The input stage shared by the interval operators. Each reads one or more
// interval inputs: rows with a ts and dur, grouped into lanes by PER columns
// and sorted by ts within each lane.

namespace perfetto::trace_processor::core::exec {

class GroupTable;
class OperatorState;
class Source;

// Where an interval input's columns are. ts and dur are flat Int64. Rows are
// grouped by `key_columns`, numbered by `group_column` if there are any, and
// sorted by ts within each group.
struct IntervalLaneSpec {
  uint32_t ts_column = 0;
  uint32_t dur_column = 0;
  std::vector<uint32_t> key_columns;
  uint32_t group_column = 0;
  // Whether a dur of -1 is an interval which never ends.
  bool forever = false;
};

// Adds the columns an interval input reads to `out`.
inline void DescribeLaneReads(const IntervalLaneSpec& lanes, StepColumns* out) {
  out->reads.push_back(lanes.ts_column);
  out->reads.push_back(lanes.dur_column);
  out->reads.insert(out->reads.end(), lanes.key_columns.begin(),
                    lanes.key_columns.end());
  if (!lanes.key_columns.empty()) {
    out->reads.push_back(lanes.group_column);
  }
}

// An interval input read whole from its own source, keeping
// `retained_columns` of each row, in this order. Any other column is read
// only for the intervals and is never buffered.
struct CollectedInputSpec {
  const Source* source = nullptr;
  IntervalLaneSpec lanes;
  std::vector<uint32_t> retained_columns;
};

// An interval input, as named in error messages: "<op>: <input> has ...".
// A non-zero `number` follows the input's name, as in "operand 2".
struct IntervalInputName {
  std::string_view op;
  std::string_view input;
  uint32_t number = 0;
};

// One batch of an interval input: each row's lane and interval. Small, so
// callers keep it in a local while they loop over the rows. Without a
// LaneReader, lanes are numbered by `group_column`.
class LaneBatch {
 public:
  enum class Result { kOk, kSkip, kNegative, kOverflow };

  LaneBatch(const RowBatch& batch, const IntervalLaneSpec& spec)
      : ts_(batch, spec.ts_column),
        dur_(batch, spec.dur_column),
        batch_(&batch),
        group_column_(spec.group_column),
        keyed_(!spec.key_columns.empty()),
        forever_(spec.forever) {}

  // The lane of row `row`. Rows without keys are all in lane 0.
  PERFETTO_ALWAYS_INLINE uint32_t lane(uint32_t row) const {
    if (!keyed_) {
      return 0;
    }
    return lanes_ ? lanes_[row] : batch_->Value<uint32_t>(group_column_, row);
  }

  // Reads row `row`'s interval [start, end). A row without a ts or dur is
  // kSkip. Error() describes the other failures.
  PERFETTO_ALWAYS_INLINE Result Read(uint32_t row,
                                     int64_t* start,
                                     int64_t* end) const {
    int64_t length;
    if (!ts_.Read(row, start) || !dur_.Read(row, &length)) {
      return Result::kSkip;
    }
    if (PERFETTO_UNLIKELY(*start < 0 || length < 0)) {
      if (forever_ && length == -1 && *start >= 0) {
        *end = std::numeric_limits<int64_t>::max();
        return Result::kOk;
      }
      return Result::kNegative;
    }
    if (PERFETTO_UNLIKELY(!base::CheckedAdd(*start, length, end))) {
      return Result::kOverflow;
    }
    return Result::kOk;
  }

  // Why a row of input `name` could not be read.
  base::Status Error(Result, const IntervalInputName& name) const;

 private:
  friend class LaneReader;

  FlatColumnReader<int64_t> ts_;
  FlatColumnReader<int64_t> dur_;
  const RowBatch* batch_;
  // The shared number of each row's lane, if a LaneReader numbered them.
  const uint32_t* lanes_ = nullptr;
  uint32_t group_column_;
  bool keyed_;
  bool forever_;
};

// Numbers the lanes of an interval input's batches in a GroupTable, which
// several inputs can share so that they agree on each key's number.
class LaneReader {
 public:
  LaneReader(const IntervalLaneSpec& spec, IntervalInputName name)
      : spec_(&spec), name_(name) {}

  // Reads `in`, numbering its lanes in `shared` if given, and by group number
  // otherwise.
  base::Status Start(const RowBatch& in, GroupTable* shared);

  // The batch Start() was last given.
  LaneBatch batch() const {
    LaneBatch batch(*batch_, *spec_);
    batch.lanes_ = shared_ ? lanes_.data() : nullptr;
    return batch;
  }

  const IntervalInputName& name() const { return name_; }

  // Forgets the last lane read, for a new run.
  void Reset() { last_group_ = kNoGroup; }
  // Ends a run (see RunHeld).
  void EndRun() { Reset(); }

 private:
  const IntervalLaneSpec* spec_;
  IntervalInputName name_;

  const RowBatch* batch_ = nullptr;
  bool shared_ = false;
  std::vector<uint32_t> lanes_;
  // Group numbers count rows, so none is kNoGroup.
  static constexpr uint32_t kNoGroup = std::numeric_limits<uint32_t>::max();
  // The input's group number of the last row read, and its shared number.
  uint32_t last_group_ = kNoGroup;
  uint32_t last_lane_ = 0;

  // Scratch: the first row of each group in the batch, and their numbers.
  Selection firsts_;
  std::vector<uint32_t> first_rows_;
  std::vector<uint32_t> first_lanes_;
};

// An interval input read whole: its intervals in one vector, grouped by lane
// and sorted by start within each, and the rows they came from. Each
// interval's id is its row in rows(). Kept across runs to reuse memory.
class CollectedLanes {
 public:
  struct Lane {
    uint32_t number;
    Span<const Interval> intervals;
    // Whether no two intervals overlap. If so, they can be searched without
    // building a tree.
    bool nonoverlapping;
  };

  CollectedLanes(const CollectedInputSpec& input, IntervalInputName name)
      : input_(&input), reader_(input.lanes, name) {}

  // Reads every batch of the input's source. Lanes are numbered in `shared`
  // if given, as LaneReader::Start() does, and by group number otherwise.
  // Lanes numbered at or above `limit` are dropped. The rows are held in
  // buffers from `context` where they are packed (see RowStore).
  base::Status Collect(OperatorState& state,
                       Context& context,
                       GroupTable* shared,
                       uint32_t limit = std::numeric_limits<uint32_t>::max());

  uint32_t size() const { return static_cast<uint32_t>(ranges_.size()); }
  Lane lane(uint32_t i) const {
    const Range& range = ranges_[i];
    const Interval* data = intervals_.data();
    return {range.number,
            {data + range.begin, data + range.end},
            range.nonoverlapping};
  }
  // The index of the lane numbered `number`, if there is one.
  std::optional<uint32_t> Find(uint32_t number) const {
    auto it = std::lower_bound(by_number_.begin(), by_number_.end(), number,
                               [](const std::pair<uint32_t, uint32_t>& p,
                                  uint32_t n) { return p.first < n; });
    if (it == by_number_.end() || it->first != number) {
      return std::nullopt;
    }
    return it->second;
  }

  RowStore& rows() { return rows_; }
  const LaneReader& reader() const { return reader_; }

  void Clear();
  // Clear(), keeping only small allocations for the next run (see RunHeld).
  void EndRun();

 private:
  struct Range {
    uint32_t number;
    uint32_t begin;
    uint32_t end;
    bool nonoverlapping;
  };

  void Add(uint32_t number, const Interval& interval);

  const CollectedInputSpec* input_;
  LaneReader reader_;
  std::vector<Interval> intervals_;
  std::vector<Range> ranges_;
  // Each range's number and index, sorted by number.
  std::vector<std::pair<uint32_t, uint32_t>> by_number_;
  RowStore rows_;

  // Scratch: the current batch, and the part of it kept.
  RowBatch batch_;
  RowBatch retained_;
};

// The PER values of the lanes of one input batch, for rows an operator makes
// for those lanes. Only one batch's keys are held, plus those of a lane still
// open from the batch before, so memory does not grow with the input.
class LaneKeys {
 public:
  // The handle of the open lane's keys after Hold().
  static constexpr uint32_t kCarried = 0;

  // `key_columns` must outlive this.
  explicit LaneKeys(const std::vector<uint32_t>& key_columns)
      : key_columns_(&key_columns) {}

  // Holds the keys of every row of `in`, after those carried by Carry().
  // Handles from before are invalid. Keys packed are held in buffers from
  // `context` (see RowStore).
  base::Status Hold(const RowBatch& in, Context& context);

  // The handle of the keys of row `row` of the batch held.
  uint32_t Of(uint32_t row) const { return first_ + row; }

  // Keeps the keys of `handle` through the next Hold(), as kCarried: a lane
  // still open at the end of a batch continues into the next.
  void Carry(uint32_t handle);

  // Copies the keys of `handles`, in order, into `out`.
  void Gather(RowBatch* out, Span<const uint32_t> handles, Context&);

  void Clear();
  // Clear(), keeping only small allocations for the next run (see RunHeld).
  void EndRun();

 private:
  const std::vector<uint32_t>* key_columns_;
  RowStore rows_;
  RowBatch retained_;
  RowBatch carried_;
  uint32_t first_ = 0;
};

// The ts and dur columns of rows an interval operator makes, written straight
// into buffers taken from a Context.
class BoundsColumns {
 public:
  bool taken() const { return ts != nullptr; }
  void Take(Context&);
  // Adds the columns to `out` and gives it the buffers.
  void AddTo(RowBatch* out);
  // Adds one column to `out`, sharing its buffer, so it can be added again.
  void AddTsTo(RowBatch* out) const;
  void AddDurTo(RowBatch* out) const;
  void Clear();
  // Clear(), keeping only small allocations for the next run (see RunHeld).
  void EndRun();

  int64_t* ts = nullptr;
  int64_t* dur = nullptr;

 private:
  ColumnBuffer ts_buffer_;
  ColumnBuffer dur_buffer_;
};

}  // namespace perfetto::trace_processor::core::exec

#endif  // SRC_TRACE_PROCESSOR_CORE_EXEC_INTERVAL_LANES_H_
