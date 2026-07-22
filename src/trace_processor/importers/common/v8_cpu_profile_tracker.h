/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_

#include <cstdint>
#include <optional>

#include "perfetto/ext/base/flat_hash_map.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/tables/v8_tables_py.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class V8CpuProfileTracker {
 public:
  explicit V8CpuProfileTracker(TraceProcessorContext*);

  void OnSessionStart(uint32_t sequence_id,
                      int64_t ts,
                      std::optional<StringId> source,
                      std::optional<int64_t> wall_time_us,
                      std::optional<int64_t> thread_time_us,
                      std::optional<int32_t> pid,
                      std::optional<int32_t> tid);
  void OnSessionEnd(uint32_t sequence_id,
                    int64_t ts,
                    std::optional<int64_t> wall_time_us,
                    std::optional<int64_t> thread_time_us);
  void OnProfilerSession(uint32_t sequence_id,
                         tables::ProfilerSessionTable::Id);
  std::optional<tables::ProfilerSessionTable::Id> GetProfilerSession(
      uint32_t sequence_id) const;
  void OnProfilerSample(uint32_t sequence_id,
                        tables::ProfilerSampleTable::Id,
                        std::optional<int32_t> sample_kind,
                        std::optional<uint32_t> leaf_line,
                        std::optional<uint32_t> leaf_column);

 private:
  struct Metadata {
    int64_t start_ts;
    std::optional<StringId> source;
    std::optional<int64_t> pid;
    std::optional<int64_t> tid;
    std::optional<int64_t> start_time_us;
    std::optional<int64_t> start_thread_ts;
    std::optional<int64_t> end_ts;
    std::optional<int64_t> end_time_us;
    std::optional<int64_t> end_thread_ts;
  };

  void MaybeCreateSession(uint32_t sequence_id);

  TraceProcessorContext* const context_;
  base::FlatHashMap<uint32_t, Metadata> metadata_;
  base::FlatHashMap<uint32_t, tables::ProfilerSessionTable::Id>
      profiler_sessions_;
  base::FlatHashMap<uint32_t, tables::V8CpuProfileSessionTable::Id>
      v8_sessions_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_
