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

#include "src/trace_processor/core/exec/interval_intersect.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"

namespace perfetto::trace_processor::core::exec {
namespace {

// Regions and the row each came from in every operand: the bounds in one
// array and the rows in another, `operands` to a region, so narrowing copies
// a run of indices rather than reaching for the heap once a region.
struct Regions {
  uint32_t operands = 0;
  std::vector<Ts> starts;
  std::vector<Ts> ends;
  std::vector<uint32_t> rows;

  uint32_t size() const { return static_cast<uint32_t>(starts.size()); }
  const uint32_t* RowsOf(uint32_t i) const {
    return rows.data() + static_cast<size_t>(i) * operands;
  }
  void Clear() {
    starts.clear();
    ends.clear();
    rows.clear();
  }
  // Adds a region covering [start, end) whose rows are `from`, which may be
  // null when none are known yet.
  uint32_t* Push(Ts start, Ts end, const uint32_t* from) {
    starts.push_back(start);
    ends.push_back(end);
    size_t at = rows.size();
    rows.resize(at + operands, 0);
    uint32_t* to = rows.data() + at;
    if (from) {
      std::copy(from, from + operands, to);
    }
    return to;
  }
};

// One operand's rows sharing a key, and whether they can be searched without
// a tree.
struct Group {
  std::vector<Interval> intervals;
  bool nonoverlapping = true;
};

// Reads one flat, non-null Int64 column of a batch.
struct Int64Reader {
  explicit Int64Reader(const ColumnView& column)
      : data(static_cast<const int64_t*>(column.data())),
        selection(column.selection()),
        validity(column.validity()) {}

  bool Read(uint32_t row, int64_t* out) const {
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

base::Status ValidateOperand(const RowBatch& batch,
                             const IntervalIntersectOperand& operand,
                             uint32_t which) {
  auto is_int64 = [&](uint32_t index) {
    const ColumnView& column = batch.column(index);
    return column.kind() == ColumnView::Kind::kFlat &&
           column.type().Is<Int64>();
  };
  bool ok = is_int64(operand.ts_column) && is_int64(operand.dur_column) &&
            std::all_of(operand.key_columns.begin(), operand.key_columns.end(),
                        is_int64);
  return ok ? base::OkStatus()
            : base::ErrStatus(
                  "INTERVAL INTERSECTION: operand %u's ts, dur and PER "
                  "columns must be Int64",
                  which + 1);
}

// A key column takes this many bytes: whether it holds a value, then the
// value itself. Two rows which hold no value there agree on it, as they
// would under GROUP BY.
constexpr size_t kKeyColumnBytes = 1 + sizeof(int64_t);

void WriteKeyColumn(std::string& key,
                    uint32_t at,
                    bool present,
                    int64_t value) {
  char* to = key.data() + at * kKeyColumnBytes;
  to[0] = present ? 1 : 0;
  memcpy(to + 1, &value, sizeof(value));
}

class IntersectState : public OperatorState {
 public:
  ~IntersectState() override;

  std::vector<std::unique_ptr<OperatorState>> operand_states;
  std::vector<std::unique_ptr<RowStore>> stores;
  // Per operand, its rows by key. A key with no entry in some operand covers
  // nothing, so only keys every operand has produce regions.
  std::vector<base::FlatHashMap<std::string, Group>> groups;

  bool computed = false;
  Regions regions;
  uint32_t served = 0;

  // The bounds of the regions being served, which no operand owns.
  ColumnChunk ts;
  ColumnChunk dur;
  // One gathered batch per operand, plus the row indices each gather reads.
  std::vector<RowBatch> gathered;
  std::vector<std::vector<uint32_t>> gather_rows;
  Regions running;
  Regions narrowed;
  base::Status status = base::OkStatus();
};

IntersectState::~IntersectState() = default;

// Reads every row of `operand` into `store` and groups them by key.
base::Status Collect(const IntervalIntersectOperand& operand,
                     uint32_t which,
                     OperatorState& state,
                     RowStore& store,
                     base::FlatHashMap<std::string, Group>& groups) {
  RowBatch batch;
  std::string key(operand.key_columns.size() * kKeyColumnBytes, '\0');
  while (operand.source->GetData(batch, state)) {
    RETURN_IF_ERROR(ValidateOperand(batch, operand, which));
    Int64Reader ts(batch.column(operand.ts_column));
    Int64Reader dur(batch.column(operand.dur_column));
    std::vector<Int64Reader> keys;
    for (uint32_t column : operand.key_columns) {
      keys.emplace_back(batch.column(column));
    }
    for (uint32_t row = 0; row < batch.size(); ++row) {
      int64_t start;
      int64_t length;
      if (!ts.Read(row, &start) || !dur.Read(row, &length)) {
        continue;
      }
      if (start < 0 || length < 0) {
        return base::ErrStatus(
            "INTERVAL INTERSECTION: operand %u has a row whose ts or dur is "
            "below zero",
            which + 1);
      }
      for (uint32_t i = 0; i < keys.size(); ++i) {
        int64_t value = 0;
        WriteKeyColumn(key, i, keys[i].Read(row, &value), value);
      }
      Group& group = groups[key];
      group.intervals.push_back({static_cast<Ts>(start),
                                 static_cast<Ts>(start + length),
                                 store.size() + row});
    }
    RETURN_IF_ERROR(store.Append(batch));
  }
  RETURN_IF_ERROR(operand.source->status(state));

  // The intersector reads intervals in order of their start, and takes a set
  // which never overlaps itself down a cheaper path.
  for (auto it = groups.GetIterator(); it; ++it) {
    Group& group = it.value();
    std::sort(group.intervals.begin(), group.intervals.end(),
              [](const Interval& a, const Interval& b) {
                if (a.start != b.start) {
                  return a.start < b.start;
                }
                return a.end != b.end ? a.end < b.end : a.id < b.id;
              });
    for (size_t i = 1; i < group.intervals.size() && group.nonoverlapping;
         ++i) {
      group.nonoverlapping =
          group.intervals[i - 1].end <= group.intervals[i].start;
    }
  }
  return base::OkStatus();
}

// Narrows the regions of one key down through every operand, smallest first
// so the running set starts as small as it can.
void NarrowKey(const std::vector<const Group*>& groups,
               Regions& running,
               Regions& narrowed,
               Regions& out) {
  auto count = static_cast<uint32_t>(groups.size());
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return groups[a]->intervals.size() < groups[b]->intervals.size();
  });

  running.Clear();
  for (const Interval& interval : groups[order.front()]->intervals) {
    running.Push(interval.start, interval.end, nullptr)[order.front()] =
        interval.id;
  }

  std::vector<Interval> overlaps;
  for (uint32_t i = 1; i < count && running.size() > 0; ++i) {
    uint32_t which = order[i];
    const Group& group = *groups[which];
    IntervalIntersector intersector(
        group.intervals,
        IntervalIntersector::DecideMode(group.nonoverlapping, running.size()));
    narrowed.Clear();
    for (uint32_t r = 0; r < running.size(); ++r) {
      overlaps.clear();
      intersector.FindOverlaps(running.starts[r], running.ends[r], overlaps);
      const uint32_t* from = running.RowsOf(r);
      for (const Interval& overlap : overlaps) {
        narrowed.Push(overlap.start, overlap.end, from)[which] = overlap.id;
      }
    }
    std::swap(running, narrowed);
  }
  for (uint32_t r = 0; r < running.size(); ++r) {
    out.Push(running.starts[r], running.ends[r], running.RowsOf(r));
  }
}

}  // namespace

IntervalIntersect::IntervalIntersect(
    std::vector<IntervalIntersectOperand> operands)
    : operands_(std::move(operands)) {
  PERFETTO_CHECK(operands_.size() >= 2);
}

IntervalIntersect::~IntervalIntersect() = default;

std::unique_ptr<OperatorState> IntervalIntersect::MakeState() const {
  auto state = std::make_unique<IntersectState>();
  auto count = static_cast<uint32_t>(operands_.size());
  for (uint32_t i = 0; i < count; ++i) {
    state->stores.push_back(std::make_unique<RowStore>());
  }
  state->groups.resize(count);
  state->gathered.resize(count);
  state->gather_rows.resize(count);
  for (const IntervalIntersectOperand& operand : operands_) {
    state->operand_states.push_back(operand.source->MakeState());
  }
  return state;
}

base::Status IntervalIntersect::status(const OperatorState& state) const {
  return state.Cast<const IntersectState>().status;
}

void IntervalIntersect::Rewind(OperatorState& state) const {
  IntersectState& s = state.Cast<IntersectState>();
  for (uint32_t i = 0; i < operands_.size(); ++i) {
    operands_[i].source->Rewind(*s.operand_states[i]);
    s.stores[i]->Clear();
    s.groups[i].Clear();
  }
  s.computed = false;
  s.regions.Clear();
  s.served = 0;
  s.status = base::OkStatus();
}

bool IntervalIntersect::GetData(RowBatch& out, OperatorState& state) const {
  IntersectState& s = state.Cast<IntersectState>();
  auto count = static_cast<uint32_t>(operands_.size());
  if (!s.computed) {
    s.regions.operands = count;
    s.running.operands = count;
    s.narrowed.operands = count;
    for (uint32_t i = 0; i < count; ++i) {
      s.status = Collect(operands_[i], i, *s.operand_states[i], *s.stores[i],
                         s.groups[i]);
      if (!s.status.ok()) {
        return false;
      }
    }
    // A region needs every operand, so only the keys of the operand with the
    // fewest of them can start one.
    uint32_t fewest = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (s.groups[i].size() < s.groups[fewest].size()) {
        fewest = i;
      }
    }
    std::vector<const Group*> groups(count);
    for (auto it = s.groups[fewest].GetIterator(); it; ++it) {
      bool everywhere = true;
      for (uint32_t i = 0; i < count && everywhere; ++i) {
        const Group* group = s.groups[i].Find(it.key());
        groups[i] = group;
        everywhere = group != nullptr;
      }
      if (everywhere) {
        NarrowKey(groups, s.running, s.narrowed, s.regions);
      }
    }
    s.computed = true;
  }

  uint32_t pending = s.regions.size() - s.served;
  uint32_t serving = std::min(pending, kMaxBatchRows);
  if (serving == 0) {
    out.Reset();
    return false;
  }

  int64_t* ts = s.ts.Values<int64_t>().data();
  int64_t* dur = s.dur.Values<int64_t>().data();
  for (uint32_t i = 0; i < count; ++i) {
    s.gather_rows[i].resize(serving);
  }
  for (uint32_t row = 0; row < serving; ++row) {
    uint32_t at = s.served + row;
    ts[row] = static_cast<int64_t>(s.regions.starts[at]);
    dur[row] = static_cast<int64_t>(s.regions.ends[at] - s.regions.starts[at]);
    const uint32_t* from = s.regions.RowsOf(at);
    for (uint32_t i = 0; i < count; ++i) {
      s.gather_rows[i][row] = from[i];
    }
  }
  s.served += serving;

  out.Reset();
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur));
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t* rows = s.gather_rows[i].data();
    s.stores[i]->View(&s.gathered[i], {rows, rows + serving});
    for (uint32_t c = 0; c < s.gathered[i].column_count(); ++c) {
      out.AddColumn(s.gathered[i].column(c));
    }
  }
  out.SetCardinality(serving);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
