/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "src/trace_processor/importers/common/v8_cpu_profile_tracker.h"

#include <utility>

#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

V8CpuProfileTracker::V8CpuProfileTracker(TraceProcessorContext* context)
    : context_(context) {}

void V8CpuProfileTracker::OnSessionStart(uint32_t sequence_id,
                                         int64_t ts,
                                         std::optional<StringId> source,
                                         std::optional<int64_t> wall_time_us,
                                         std::optional<int64_t> thread_time_us,
                                         std::optional<int32_t> pid,
                                         std::optional<int32_t> tid) {
  Metadata metadata{ts,
                    source,
                    pid,
                    tid,
                    wall_time_us,
                    thread_time_us
                        ? std::optional<int64_t>(*thread_time_us * 1000)
                        : std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt};
  metadata_.Insert(sequence_id, std::move(metadata));
  if (!profiler_sessions_.Find(sequence_id)) {
    tables::ProfilerSessionTable::Row row;
    row.source = context_->storage->InternString("v8.cpu_profiler");
    row.timebase_unit = context_->storage->InternString("ns");
    auto session_id =
        context_->storage->mutable_profiler_session_table()->Insert(row).id;
    profiler_sessions_.Insert(sequence_id, session_id);
  }
  MaybeCreateSession(sequence_id);
}

void V8CpuProfileTracker::OnSessionEnd(uint32_t sequence_id,
                                       int64_t ts,
                                       std::optional<int64_t> wall_time_us,
                                       std::optional<int64_t> thread_time_us) {
  auto* metadata = metadata_.Find(sequence_id);
  if (!metadata)
    return;
  metadata->end_ts = ts;
  metadata->end_time_us = wall_time_us;
  if (thread_time_us)
    metadata->end_thread_ts = *thread_time_us * 1000;
  MaybeCreateSession(sequence_id);
  if (auto* id = v8_sessions_.Find(sequence_id)) {
    auto row =
        (*context_->storage->mutable_v8_cpu_profile_session_table())[*id];
    row.set_end_ts(ts);
    if (wall_time_us)
      row.set_end_time_us(*wall_time_us);
    if (thread_time_us)
      row.set_end_thread_ts(*thread_time_us * 1000);
  }
}

void V8CpuProfileTracker::OnProfilerSession(
    uint32_t sequence_id,
    tables::ProfilerSessionTable::Id session_id) {
  if (auto* existing = profiler_sessions_.Find(sequence_id)) {
    PERFETTO_DCHECK(*existing == session_id);
  } else {
    profiler_sessions_.Insert(sequence_id, session_id);
  }
  MaybeCreateSession(sequence_id);
}

std::optional<tables::ProfilerSessionTable::Id>
V8CpuProfileTracker::GetProfilerSession(uint32_t sequence_id) const {
  if (const auto* id = profiler_sessions_.Find(sequence_id))
    return *id;
  return std::nullopt;
}

void V8CpuProfileTracker::OnProfilerSample(
    uint32_t /*sequence_id*/,
    tables::ProfilerSampleTable::Id sample_id,
    std::optional<int32_t> sample_kind,
    std::optional<uint32_t> leaf_line,
    std::optional<uint32_t> leaf_column) {
  if (!sample_kind && !leaf_line && !leaf_column)
    return;
  tables::V8CpuProfileSampleTable::Row row;
  row.profiler_sample_id = sample_id;
  if (sample_kind) {
    row.sample_kind =
        context_->storage->InternString(*sample_kind == 2   ? "PROGRAM"
                                        : *sample_kind == 3 ? "GC"
                                        : *sample_kind == 4 ? "IDLE"
                                        : *sample_kind == 5 ? "OTHER"
                                                            : "NORMAL");
  }
  row.leaf_line = leaf_line;
  row.leaf_column = leaf_column;
  context_->storage->mutable_v8_cpu_profile_sample_table()->Insert(row);
}

void V8CpuProfileTracker::MaybeCreateSession(uint32_t sequence_id) {
  if (v8_sessions_.Find(sequence_id))
    return;
  auto* metadata = metadata_.Find(sequence_id);
  auto* profiler_session = profiler_sessions_.Find(sequence_id);
  if (!metadata || !profiler_session)
    return;
  tables::V8CpuProfileSessionTable::Row row;
  row.profiler_session_id = *profiler_session;
  row.source = metadata->source;
  row.pid = metadata->pid;
  row.tid = metadata->tid;
  row.start_ts = metadata->start_ts;
  row.start_time_us = metadata->start_time_us;
  row.start_thread_ts = metadata->start_thread_ts;
  row.end_ts = metadata->end_ts;
  row.end_time_us = metadata->end_time_us;
  row.end_thread_ts = metadata->end_thread_ts;
  auto id =
      context_->storage->mutable_v8_cpu_profile_session_table()->Insert(row).id;
  v8_sessions_.Insert(sequence_id, id);
}

}  // namespace perfetto::trace_processor
