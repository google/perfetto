/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "src/trace_processor/perfetto_sql/intrinsics/table_functions/experimental_flat_slice.h"

#include <memory>
#include <set>

#include "src/trace_processor/sqlite/sqlite_utils.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ExperimentalFlatSlice::ExperimentalFlatSlice(TraceProcessorContext* context)
    : context_(context) {}

base::Status ExperimentalFlatSlice::ValidateConstraints(
    const QueryConstraints& qc) {
  using CI = tables::ExperimentalFlatSliceTable::ColumnIndex;
  bool has_start_bound = false;
  bool has_end_bound = false;
  for (const auto& c : qc.constraints()) {
    has_start_bound |= c.column == static_cast<int>(CI::start_bound) &&
                       sqlite_utils::IsOpEq(c.op);
    has_end_bound |= c.column == static_cast<int>(CI::end_bound) &&
                     sqlite_utils::IsOpEq(c.op);
  }
  return has_start_bound && has_end_bound
             ? base::OkStatus()
             : base::ErrStatus("Failed to find required constraints");
}

base::Status ExperimentalFlatSlice::ComputeTable(
    const std::vector<Constraint>& cs,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  using CI = tables::ExperimentalFlatSliceTable::ColumnIndex;
  auto start_it = std::find_if(cs.begin(), cs.end(), [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(CI::start_bound) &&
           c.op == FilterOp::kEq;
  });
  auto end_it = std::find_if(cs.begin(), cs.end(), [](const Constraint& c) {
    return c.col_idx == static_cast<uint32_t>(CI::end_bound) &&
           c.op == FilterOp::kEq;
  });
  // TODO(rsavitski): consider checking the values' types (in case of erroneous
  // queries passing e.g. null).
  int64_t start_bound = start_it->value.AsLong();
  int64_t end_bound = end_it->value.AsLong();
  table_return = ComputeFlatSliceTable(context_->storage->slice_table(),
                                       context_->storage->mutable_string_pool(),
                                       start_bound, end_bound);
  return base::OkStatus();
}

std::unique_ptr<tables::ExperimentalFlatSliceTable>
ExperimentalFlatSlice::ComputeFlatSliceTable(const tables::SliceTable& slice,
                                             StringPool* pool,
                                             int64_t start_bound,
                                             int64_t end_bound) {
  std::unique_ptr<tables::ExperimentalFlatSliceTable> out(
      new tables::ExperimentalFlatSliceTable(pool));

  auto insert_slice = [&](uint32_t i, int64_t ts,
                          tables::TrackTable::Id track_id) {
    tables::ExperimentalFlatSliceTable::Row row;
    row.ts = ts;
    row.dur = -1;
    row.track_id = track_id;
    row.category = slice.category()[i];
    row.name = slice.name()[i];
    row.arg_set_id = slice.arg_set_id()[i];
    row.source_id = slice.id()[i];
    row.start_bound = start_bound;
    row.end_bound = end_bound;
    return out->Insert(row).row;
  };
  auto insert_sentinel = [&](int64_t ts, TrackId track_id) {
    tables::ExperimentalFlatSliceTable::Row row;
    row.ts = ts;
    row.dur = -1;
    row.track_id = track_id;
    row.category = kNullStringId;
    row.name = kNullStringId;
    row.arg_set_id = kInvalidArgSetId;
    row.source_id = std::nullopt;
    row.start_bound = start_bound;
    row.end_bound = end_bound;
    return out->Insert(row).row;
  };

  auto terminate_slice = [&](uint32_t out_row, int64_t end_ts) {
    PERFETTO_DCHECK(out->dur()[out_row] == -1);
    int64_t out_ts = out->ts()[out_row];
    out->mutable_dur()->Set(out_row, end_ts - out_ts);
  };

  struct ActiveSlice {
    std::optional<uint32_t> source_row;
    uint32_t out_row = std::numeric_limits<uint32_t>::max();

    bool is_sentinel() const { return !source_row; }
  };
  struct Track {
    std::vector<uint32_t> parents;
    ActiveSlice active;
    bool initialized = false;
  };
  std::unordered_map<TrackId, Track> tracks;

  auto maybe_terminate_active_slice = [&](const Track& t, int64_t fin_ts) {
    int64_t ts = slice.ts()[t.active.source_row.value()];
    int64_t dur = slice.dur()[t.active.source_row.value()];
    if (dur == -1 || ts + dur > fin_ts)
      return false;

    terminate_slice(t.active.out_row, ts + dur);
    return true;
  };

  // Post-condition: |tracks[track_id].active| will always point to
  // a slice which finishes after |fin_ts| and has a |dur| == -1 in
  // |out|.
  auto output_slices_before = [&](TrackId track_id, int64_t fin_ts) {
    auto& t = tracks[track_id];

    // A sentinel slice cannot have parents.
    PERFETTO_DCHECK(!t.active.is_sentinel() || t.parents.empty());

    // If we have a sentinel slice active, we have nothing to output.
    if (t.active.is_sentinel())
      return;

    // Try and terminate the current slice (if it ends before |fin_ts|)
    // If we cannot terminate it, then we leave it as pending for the caller
    // to terminate.
    if (!maybe_terminate_active_slice(t, fin_ts))
      return;

    // Next, add any parents as appropriate.
    for (int64_t i = static_cast<int64_t>(t.parents.size()) - 1; i >= 0; --i) {
      uint32_t source_row = t.parents[static_cast<size_t>(i)];
      t.parents.pop_back();

      int64_t active_ts = out->ts()[t.active.out_row];
      int64_t active_dur = out->dur()[t.active.out_row];
      PERFETTO_DCHECK(active_dur != -1);

      t.active.source_row = source_row;
      t.active.out_row =
          insert_slice(source_row, active_ts + active_dur, track_id);

      if (!maybe_terminate_active_slice(t, fin_ts))
        break;
    }

    if (!t.parents.empty())
      return;

    // If the active slice is a sentinel, the check at the top of this function
    // should have caught it; all code only adds slices from source.
    PERFETTO_DCHECK(!t.active.is_sentinel());

    int64_t ts = out->ts()[t.active.out_row];
    int64_t dur = out->dur()[t.active.out_row];

    // If the active slice is unfinshed, we return that for the caller to
    // terminate.
    if (dur == -1)
      return;

    // Otherwise, Add a sentinel slice after the end of the active slice.
    t.active.source_row = std::nullopt;
    t.active.out_row = insert_sentinel(ts + dur, track_id);
  };

  for (uint32_t i = 0; i < slice.row_count(); ++i) {
    // TODO(lalitm): this can be optimized using a O(logn) lower bound/filter.
    // Not adding for now as a premature optimization but may be needed down the
    // line.
    int64_t ts = slice.ts()[i];
    if (ts < start_bound)
      continue;

    if (ts >= end_bound)
      break;

    // Ignore instants as they don't factor into flat slice at all.
    if (slice.dur()[i] == 0)
      continue;

    TrackId track_id = slice.track_id()[i];
    Track& track = tracks[track_id];

    // Initalize the track (if needed) by adding a sentinel slice starting at
    // start_bound.
    bool is_root = slice.depth()[i] == 0;
    if (!track.initialized) {
      // If we are unintialized and our start box picks up slices mid way
      // through startup, wait until we reach a root slice.
      if (!is_root)
        continue;

      track.active.out_row = insert_sentinel(start_bound, track_id);
      track.initialized = true;
    }
    output_slices_before(track_id, ts);
    terminate_slice(track.active.out_row, ts);

    // We should have sentinel slices iff the slice is a root.
    PERFETTO_DCHECK(track.active.is_sentinel() == is_root);

    // If our current slice has a parent, that must be the current active slice.
    if (!is_root) {
      track.parents.push_back(*track.active.source_row);
    }

    // The depth of our slice should also match the depth of the parent stack
    // (after adding the previous slice).
    PERFETTO_DCHECK(track.parents.size() == slice.depth()[i]);

    track.active.source_row = i;
    track.active.out_row = insert_slice(i, ts, track_id);
  }

  for (const auto& track : tracks) {
    // If the track is not initialized, don't add anything.
    if (!track.second.initialized)
      continue;

    // First, terminate any hanging slices.
    output_slices_before(track.first, end_bound);

    // Second, force terminate the final slice to the end bound.
    terminate_slice(track.second.active.out_row, end_bound);
  }

  return out;
}

Table::Schema ExperimentalFlatSlice::CreateSchema() {
  return tables::ExperimentalFlatSliceTable::ComputeStaticSchema();
}

std::string ExperimentalFlatSlice::TableName() {
  return "experimental_flat_slice";
}

uint32_t ExperimentalFlatSlice::EstimateRowCount() {
  return context_->storage->slice_table().row_count();
}

}  // namespace trace_processor
}  // namespace perfetto
