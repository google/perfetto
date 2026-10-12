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
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/small_vector.h"
#include "perfetto/ext/base/status_macros.h"
#include "src/trace_processor/containers/interval_intersector.h"
#include "src/trace_processor/containers/interval_tree.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/exec/column_chunk.h"
#include "src/trace_processor/core/exec/column_view.h"
#include "src/trace_processor/core/exec/key_encoder.h"
#include "src/trace_processor/core/exec/operator.h"
#include "src/trace_processor/core/exec/row_batch.h"
#include "src/trace_processor/core/exec/row_selection.h"
#include "src/trace_processor/core/exec/row_store.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/util/cold_sort.h"

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
  std::string key;
  std::vector<Interval> intervals;
  bool nonoverlapping = true;
};

// One operand's groups, kept across runs to reuse their memory: the first
// `used` are this run's. Held by pointer, so the keys the map views never
// move.
struct Groups {
  std::vector<std::unique_ptr<Group>> all;
  size_t used = 0;
  base::FlatHashMapV2<std::string_view, Group*> by_key;

  Group& Get(std::string_view key) {
    if (Group** found = by_key.Find(key)) {
      return **found;
    }
    if (used == all.size()) {
      all.emplace_back(std::make_unique<Group>());
    }
    Group& group = *all[used++];
    group.key.assign(key);
    group.intervals.clear();
    group.nonoverlapping = true;
    by_key.Insert(group.key, &group);
    return group;
  }
  const Group* Find(std::string_view key) const {
    Group* const* found = by_key.Find(key);
    return found ? *found : nullptr;
  }
  void Clear() {
    used = 0;
    by_key.Clear();
  }
};

base::Status ValidateOperand(const RowBatch& batch,
                             const IntervalIntersectOperand& operand,
                             uint32_t which) {
  auto is_int64 = [&](uint32_t index) {
    const ColumnView& column = batch.column(index);
    return column.kind() == ColumnView::Kind::kFlat &&
           column.type().Is<Int64>();
  };
  bool ok = is_int64(operand.ts_column) && is_int64(operand.dur_column);
  return ok ? base::OkStatus()
            : base::ErrStatus(
                  "INTERVAL INTERSECTION: operand %u's ts and dur columns "
                  "must be Int64",
                  which + 1);
}

class IntersectState : public OperatorState {
 public:
  ~IntersectState() override;

  std::vector<std::unique_ptr<OperatorState>> operand_states;
  std::vector<std::unique_ptr<RowStore>> stores;
  // Shared by the operands, so their keys' types must agree.
  KeyEncoder keys;
  // Per operand, its rows by key. A key with no entry in some operand covers
  // nothing, so only keys every operand has produce regions.
  std::vector<Groups> groups;

  bool computed = false;
  Regions regions;
  uint32_t served = 0;

  // The bounds of the regions being served, which no operand owns.
  ColumnChunk ts;
  ColumnChunk dur;
  // One gathered batch per operand.
  std::vector<RowBatch> gathered;
  base::Status status = base::OkStatus();

  // Buffers reused across calls to avoid allocating: nothing in them outlives
  // the call which fills them.
  struct Scratch {
    // The regions of one key, before and after narrowing by an operand.
    Regions running;
    Regions narrowed;
    // The order in which a key's operands narrow its regions.
    std::vector<uint32_t> order;
    // The intervals of one operand overlapping a region.
    std::vector<Interval> overlaps;
    // Per operand, the row indices a gather reads.
    std::vector<std::vector<uint32_t>> gather_rows;
    // An operand's batches as they are read, and the part of them kept.
    RowBatch batch;
    RowBatch retained;
    // Per operand, its group of the key being narrowed.
    std::vector<const Group*> key_groups;
  } scratch;
};

IntersectState::~IntersectState() = default;

// Reads every row of `operand` into `store` and groups them by key.
base::Status Collect(const IntervalIntersectOperand& operand,
                     uint32_t which,
                     OperatorState& state,
                     RowStore& store,
                     KeyEncoder& keys,
                     Groups& groups,
                     IntersectState::Scratch& scratch) {
  RowBatch& batch = scratch.batch;
  RowBatch& retained = scratch.retained;
  while (operand.source->GetData(batch, state)) {
    RETURN_IF_ERROR(ValidateOperand(batch, operand, which));
    FlatColumnReader<int64_t> ts(batch.column(operand.ts_column));
    FlatColumnReader<int64_t> dur(batch.column(operand.dur_column));
    if (std::optional<uint32_t> bad = keys.Encode(batch, operand.key_columns)) {
      return base::ErrStatus(
          "INTERVAL INTERSECTION: operand %u's PER column %u must hold one "
          "type, the same in every operand",
          which + 1, *bad + 1);
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
      std::string_view key = keys.Key(row);
      Group& group = groups.Get(key);
      group.intervals.push_back({static_cast<Ts>(start),
                                 static_cast<Ts>(start + length),
                                 store.size() + row});
    }
    retained.Reset();
    for (uint32_t column : operand.retained_columns) {
      retained.AddColumn(batch.column(column), batch.owner(column));
    }
    retained.SetCardinality(batch.size());
    RETURN_IF_ERROR(store.Append(retained));
  }
  RETURN_IF_ERROR(operand.source->status(state));

  // The intersector reads intervals in order of their start, and takes a set
  // which never overlaps itself down a cheaper path.
  for (size_t g = 0; g < groups.used; ++g) {
    Group& group = *groups.all[g];
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
          !IsOverlapping(group.intervals[i - 1], group.intervals[i]);
    }
  }
  return base::OkStatus();
}

// Narrows the regions of one key. If every operand is non-overlapping, runs a
// fast k-pointer sweep. Otherwise, narrows through operands pairwise, smallest
// first so the running set starts as small as it can.
void NarrowKey(const std::vector<const Group*>& groups,
               IntersectState::Scratch& scratch,
               Regions& out) {
  if (std::all_of(groups.begin(), groups.end(),
                  [](const Group* g) { return g->nonoverlapping; })) {
    base::SmallVector<const std::vector<Interval>*, 16> tables;
    for (const Group* group : groups) {
      tables.emplace_back(&group->intervals);
    }
    IntervalIntersector::IntersectNonOverlapping(
        tables, [&](uint64_t start, uint64_t end, const uint32_t* ids) {
          out.Push(start, end, ids);
        });
    return;
  }

  Regions& running = scratch.running;
  Regions& narrowed = scratch.narrowed;
  std::vector<uint32_t>& order = scratch.order;
  std::vector<Interval>& overlaps = scratch.overlaps;

  auto count = static_cast<uint32_t>(groups.size());
  order.resize(count);
  std::iota(order.begin(), order.end(), 0u);
  ColdSortByKey(order.begin(), order.end(),
                [&](uint32_t i) { return groups[i]->intervals.size(); });

  running.Clear();
  for (const Interval& interval : groups[order.front()]->intervals) {
    running.Push(interval.start, interval.end, nullptr)[order.front()] =
        interval.id;
  }

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
  state->scratch.gather_rows.resize(count);
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
    s.scratch.running.operands = count;
    s.scratch.narrowed.operands = count;
    for (uint32_t i = 0; i < count; ++i) {
      s.status = Collect(operands_[i], i, *s.operand_states[i], *s.stores[i],
                         s.keys, s.groups[i], s.scratch);
      if (!s.status.ok()) {
        return false;
      }
    }
    // A region needs every operand, so only the keys of the operand with the
    // fewest of them can start one.
    uint32_t fewest = 0;
    for (uint32_t i = 1; i < count; ++i) {
      if (s.groups[i].used < s.groups[fewest].used) {
        fewest = i;
      }
    }
    std::vector<const Group*>& groups = s.scratch.key_groups;
    groups.resize(count);
    for (size_t g = 0; g < s.groups[fewest].used; ++g) {
      const std::string& key = s.groups[fewest].all[g]->key;
      bool everywhere = true;
      for (uint32_t i = 0; i < count && everywhere; ++i) {
        const Group* group = s.groups[i].Find(key);
        groups[i] = group;
        everywhere = group != nullptr;
      }
      if (everywhere) {
        NarrowKey(groups, s.scratch, s.regions);
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
    s.scratch.gather_rows[i].resize(serving);
  }
  for (uint32_t row = 0; row < serving; ++row) {
    uint32_t at = s.served + row;
    ts[row] = static_cast<int64_t>(s.regions.starts[at]);
    dur[row] = static_cast<int64_t>(s.regions.ends[at] - s.regions.starts[at]);
    const uint32_t* from = s.regions.RowsOf(at);
    for (uint32_t i = 0; i < count; ++i) {
      s.scratch.gather_rows[i][row] = from[i];
    }
  }
  s.served += serving;

  out.Reset();
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, ts));
  out.AddColumn(ColumnView::Reference(StorageType{Int64{}}, dur));
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t* rows = s.scratch.gather_rows[i].data();
    s.stores[i]->View(&s.gathered[i], {rows, rows + serving});
    for (uint32_t c = 0; c < s.gathered[i].column_count(); ++c) {
      out.AddColumn(s.gathered[i].column(c));
    }
  }
  out.SetCardinality(serving);
  return true;
}

}  // namespace perfetto::trace_processor::core::exec
