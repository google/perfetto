/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/dynamic/thread_state_generator.h"

#include <memory>
#include <set>

#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

ThreadStateGenerator::ThreadStateGenerator(TraceProcessorContext* context)
    : running_string_id_(context->storage->InternString("Running")),
      runnable_string_id_(context->storage->InternString("R")),
      context_(context) {}

ThreadStateGenerator::~ThreadStateGenerator() = default;

base::Status ThreadStateGenerator::ValidateConstraints(
    const QueryConstraints&) {
  return base::OkStatus();
}

base::Status ThreadStateGenerator::ComputeTable(
    const std::vector<Constraint>&,
    const std::vector<Order>&,
    const BitVector&,
    std::unique_ptr<Table>& table_return) {
  if (!unsorted_thread_state_table_) {
    int64_t trace_end_ts =
        context_->storage->GetTraceTimestampBoundsNs().second;

    unsorted_thread_state_table_ = ComputeThreadStateTable(trace_end_ts);

    // We explicitly sort by ts here as ComputeThreadStateTable does not insert
    // rows in sorted order but we expect our clients to always want to sort
    // on ts. Writing ComputeThreadStateTable to insert in sorted order is
    // more trouble than its worth.
    sorted_thread_state_table_ = unsorted_thread_state_table_->Sort(
        {unsorted_thread_state_table_->ts().ascending()});
  }
  // TODO(rsavitski): return base::ErrStatus instead?
  PERFETTO_CHECK(sorted_thread_state_table_);
  table_return =
      std::unique_ptr<Table>(new Table(sorted_thread_state_table_->Copy()));
  return base::OkStatus();
}

std::unique_ptr<tables::ThreadStateTable>
ThreadStateGenerator::ComputeThreadStateTable(int64_t trace_end_ts) {
  std::unique_ptr<tables::ThreadStateTable> table(new tables::ThreadStateTable(
      context_->storage->mutable_string_pool(), nullptr));

  const auto& raw_sched = context_->storage->sched_slice_table();
  const auto& instants = context_->storage->legacy_instant_table();

  // In both tables, exclude utid == 0 which represents the idle thread.
  Table sched = raw_sched.Filter({raw_sched.utid().ne(0)},
                                 RowMap::OptimizeFor::kLookupSpeed);
  Table waking = instants.Filter(
      {instants.name().eq("sched_waking"), instants.utid().ne(0)},
      RowMap::OptimizeFor::kLookupSpeed);

  // We prefer to use waking if at all possible and fall back to wakeup if not
  // available.
  if (waking.row_count() == 0) {
    waking = instants.Filter(
        {instants.name().eq("sched_wakeup"), instants.utid().ne(0)},
        RowMap::OptimizeFor::kLookupSpeed);
  }

  Table sched_blocked_reason = instants.Filter(
      {instants.name().eq("sched_blocked_reason"), instants.utid().ne(0)},
      RowMap::OptimizeFor::kLookupSpeed);

  const auto& sched_ts_col = sched.GetTypedColumnByName<int64_t>("ts");
  const auto& waking_ts_col = waking.GetTypedColumnByName<int64_t>("ts");
  const auto& blocked_ts_col =
      sched_blocked_reason.GetTypedColumnByName<int64_t>("ts");

  uint32_t sched_idx = 0;
  uint32_t waking_idx = 0;
  uint32_t blocked_idx = 0;
  TidInfoMap state_map(/*initial_capacity=*/1024);
  while (sched_idx < sched.row_count() || waking_idx < waking.row_count() ||
         blocked_idx < sched_blocked_reason.row_count()) {
    int64_t sched_ts = sched_idx < sched.row_count()
                           ? sched_ts_col[sched_idx]
                           : std::numeric_limits<int64_t>::max();
    int64_t waking_ts = waking_idx < waking.row_count()
                            ? waking_ts_col[waking_idx]
                            : std::numeric_limits<int64_t>::max();
    int64_t blocked_ts = blocked_idx < sched_blocked_reason.row_count()
                             ? blocked_ts_col[blocked_idx]
                             : std::numeric_limits<int64_t>::max();

    // We go through all tables, picking the earliest timestamp from any
    // to process that event.
    int64_t min_ts = std::min({sched_ts, waking_ts, blocked_ts});
    if (min_ts == sched_ts) {
      AddSchedEvent(sched, sched_idx++, state_map, trace_end_ts, table.get());
    } else if (min_ts == waking_ts) {
      AddWakingEvent(waking, waking_idx++, state_map);
    } else /* (min_ts == blocked_ts) */ {
      AddBlockedReasonEvent(sched_blocked_reason, blocked_idx++, state_map);
    }
  }

  // At the end, go through and flush any remaining pending events.
  for (auto it = state_map.GetIterator(); it; ++it) {
    // for (const auto& utid_to_pending_info : state_map) {
    UniqueTid utid = it.key();
    const ThreadSchedInfo& pending_info = it.value();
    FlushPendingEventsForThread(utid, pending_info, table.get(), base::nullopt);
  }

  return table;
}

void ThreadStateGenerator::AddSchedEvent(const Table& sched,
                                         uint32_t sched_idx,
                                         TidInfoMap& state_map,
                                         int64_t trace_end_ts,
                                         tables::ThreadStateTable* table) {
  int64_t ts = sched.GetTypedColumnByName<int64_t>("ts")[sched_idx];
  UniqueTid utid = sched.GetTypedColumnByName<uint32_t>("utid")[sched_idx];
  ThreadSchedInfo* info = &state_map[utid];

  // Due to races in the kernel, it is possible for the same thread to be
  // scheduled on different CPUs at the same time. This will manifest itself
  // here by having |info->desched_ts| in the future of this scheduling slice
  // (i.e. there was a scheduling slice in the past which ended after the start
  // of the current scheduling slice).
  //
  // We work around this problem by truncating the previous slice to the start
  // of this slice and not adding the descheduled slice (i.e. we don't call
  // |FlushPendingEventsForThread| which adds this slice).
  //
  // See b/186509316 for details and an example on when this happens.
  if (info->desched_ts && info->desched_ts.value() > ts) {
    uint32_t prev_sched_row = info->scheduled_row.value();
    int64_t prev_sched_start = table->ts()[prev_sched_row];

    // Just a double check that descheduling slice would have started at the
    // same time the scheduling slice would have ended.
    PERFETTO_DCHECK(prev_sched_start + table->dur()[prev_sched_row] ==
                    info->desched_ts.value());

    // Truncate the duration of the old slice to end at the start of this
    // scheduling slice.
    table->mutable_dur()->Set(prev_sched_row, ts - prev_sched_start);
  } else {
    FlushPendingEventsForThread(utid, *info, table, ts);
  }

  // Reset so we don't have any leftover data on the next round.
  *info = {};

  // Undo the expansion of the final sched slice for each CPU to the end of the
  // trace by setting the duration back to -1. This counteracts the code in
  // SchedEventTracker::FlushPendingEvents
  // TODO(lalitm): remove this hack when we stop expanding the last slice to the
  // end of the trace.
  int64_t dur = sched.GetTypedColumnByName<int64_t>("dur")[sched_idx];
  if (ts + dur == trace_end_ts) {
    dur = -1;
  }

  // Now add the sched slice itself as "Running" with the other fields
  // unchanged.
  tables::ThreadStateTable::Row sched_row;
  sched_row.ts = ts;
  sched_row.dur = dur;
  sched_row.cpu = sched.GetTypedColumnByName<uint32_t>("cpu")[sched_idx];
  sched_row.state = running_string_id_;
  sched_row.utid = utid;

  auto id_and_row = table->Insert(sched_row);

  // If the sched row had a negative duration, don't add any descheduled slice
  // because it would be meaningless.
  if (sched_row.dur == -1) {
    return;
  }

  // This will be flushed to the table on the next sched slice (or the very end
  // of the big loop).
  info->desched_ts = ts + dur;
  info->desched_end_state =
      sched.GetTypedColumnByName<StringId>("end_state")[sched_idx];
  info->scheduled_row = id_and_row.row;
}

void ThreadStateGenerator::AddWakingEvent(const Table& waking,
                                          uint32_t waking_idx,
                                          TidInfoMap& state_map) {
  int64_t ts = waking.GetTypedColumnByName<int64_t>("ts")[waking_idx];
  UniqueTid utid = waking.GetTypedColumnByName<uint32_t>("utid")[waking_idx];
  ThreadSchedInfo* info = &state_map[utid];

  // Occasionally, it is possible to get a waking event for a thread
  // which is already in a runnable state. When this happens, we just
  // ignore the waking event.
  // See b/186509316 for details and an example on when this happens.
  if (info->desched_end_state &&
      *info->desched_end_state == runnable_string_id_) {
    return;
  }

  // As counter-intuitive as it seems, occasionally we can get a waking
  // event for a thread which is currently running.
  //
  // There are two cases when this can happen:
  // 1. The kernel legitimately send a waking event for a "running" thread
  //    because the thread was woken up before the kernel switched away
  //    from it. In this case, the waking timestamp will be in the past
  //    because we added the descheduled slice when we processed the sched
  //    event.
  // 2. We're close to the end of the trace or had data-loss and we missed
  //    the switch out event for a thread but we see a waking after.

  // Case 1 described above. In this situation, we should drop the waking
  // entirely.
  if (info->desched_ts && *info->desched_ts > ts) {
    return;
  }

  // For case 2 and otherwise, we should just note the fact that the thread
  // became runnable at this time. Note that we cannot check if runnable is
  // already not set because we could have data-loss which leads to us getting
  // back to back waking for a single thread.
  info->runnable_ts = ts;
}

Table::Schema ThreadStateGenerator::CreateSchema() {
  auto schema = tables::ThreadStateTable::Schema();

  // Because we expect our users to generally want ordered by ts, we set the
  // ordering for the schema to match our forced sort pass in ComputeTable.
  auto ts_it = std::find_if(
      schema.columns.begin(), schema.columns.end(),
      [](const Table::Schema::Column& col) { return col.name == "ts"; });
  ts_it->is_sorted = true;
  auto id_it = std::find_if(
      schema.columns.begin(), schema.columns.end(),
      [](const Table::Schema::Column& col) { return col.name == "id"; });
  id_it->is_sorted = false;

  return schema;
}

void ThreadStateGenerator::FlushPendingEventsForThread(
    UniqueTid utid,
    const ThreadSchedInfo& info,
    tables::ThreadStateTable* table,
    base::Optional<int64_t> end_ts) {
  // First, let's flush the descheduled period (if any) to the table.
  if (info.desched_ts) {
    PERFETTO_DCHECK(info.desched_end_state);

    int64_t dur;
    if (end_ts) {
      int64_t desched_end_ts = info.runnable_ts ? *info.runnable_ts : *end_ts;
      dur = desched_end_ts - *info.desched_ts;
    } else {
      dur = -1;
    }

    tables::ThreadStateTable::Row row;
    row.ts = *info.desched_ts;
    row.dur = dur;
    row.state = *info.desched_end_state;
    row.utid = utid;
    row.io_wait = info.io_wait;
    row.blocked_function = info.blocked_function;
    table->Insert(row);
  }

  // Next, flush the runnable period (if any) to the table.
  if (info.runnable_ts) {
    tables::ThreadStateTable::Row row;
    row.ts = *info.runnable_ts;
    row.dur = end_ts ? *end_ts - row.ts : -1;
    row.state = runnable_string_id_;
    row.utid = utid;
    table->Insert(row);
  }
}

void ThreadStateGenerator::AddBlockedReasonEvent(const Table& blocked_reason,
                                                 uint32_t blocked_idx,
                                                 TidInfoMap& state_map) {
  const auto& utid_col = blocked_reason.GetTypedColumnByName<uint32_t>("utid");
  const auto& arg_set_id_col =
      blocked_reason.GetTypedColumnByName<uint32_t>("arg_set_id");

  UniqueTid utid = static_cast<UniqueTid>(utid_col[blocked_idx]);
  uint32_t arg_set_id = arg_set_id_col[blocked_idx];
  ThreadSchedInfo& info = state_map[utid];

  base::Optional<Variadic> opt_value;
  base::Status status =
      context_->storage->ExtractArg(arg_set_id, "io_wait", &opt_value);

  // We can't do anything better than ignoring any errors here.
  // TODO(lalitm): see if there's a better way to handle this.
  if (status.ok() && opt_value) {
    PERFETTO_CHECK(opt_value->type == Variadic::Type::kBool);
    info.io_wait = opt_value->bool_value;
  }

  status = context_->storage->ExtractArg(arg_set_id, "function", &opt_value);
  if (status.ok() && opt_value) {
    PERFETTO_CHECK(opt_value->type == Variadic::Type::kString);
    info.blocked_function = opt_value->string_value;
  }
}

std::string ThreadStateGenerator::TableName() {
  return "thread_state";
}

uint32_t ThreadStateGenerator::EstimateRowCount() {
  return context_->storage->sched_slice_table().row_count();
}

}  // namespace trace_processor
}  // namespace perfetto
