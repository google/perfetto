/*
 * Copyright (C) 2024 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/v8_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

// Stores interned callsites for given pid for legacy v8 samples.
class V8CpuProfileTracker
    : public TraceSorter::Sink<LegacyV8CpuProfileEvent, V8CpuProfileTracker> {
 public:
  explicit V8CpuProfileTracker(TraceProcessorContext*);
  ~V8CpuProfileTracker() override;

  void Parse(int64_t ts, LegacyV8CpuProfileEvent);

  // Sets the start timestamp for the given pid.
  void SetStartTsForSessionAndPid(uint64_t session_id,
                                  uint32_t pid,
                                  int64_t ts);

  // Adds the callsite with for the given session and pid and given raw callsite
  // id.
  base::Status AddCallsite(
      uint64_t session_id,
      uint32_t pid,
      uint32_t raw_callsite_id,
      std::optional<uint32_t> parent_raw_callsite_id,
      base::StringView script_url,
      base::StringView function_name,
      const std::vector<uint32_t>& raw_children_callsite_ids);

  // Increments the current timestamp for the given session and pid by
  // |delta_ts| and returns the resulting full timestamp.
  base::StatusOr<int64_t> AddDeltaAndGetTs(uint64_t session_id,
                                           uint32_t pid,
                                           int64_t delta_ts);

  // Adds the sample with for the given session and pid/tid and given raw
  // callsite id.
  base::Status AddSample(int64_t ts,
                         uint64_t session_id,
                         uint32_t pid,
                         uint32_t tid,
                         uint32_t raw_callsite_id);

  // Creates a new session row in __intrinsic_v8_cpu_profile_session.
  void BeginSession(uint64_t session_id,
                    uint32_t pid,
                    uint32_t tid,
                    int64_t start_ts,
                    std::optional<int64_t> start_time_us,
                    std::optional<int64_t> start_thread_ts,
                    std::optional<base::StringView> source);

  // Updates the matching session row with end timestamps. No-op if BeginSession
  // was not called for this (session_id, pid).
  void EndSession(uint64_t session_id,
                  uint32_t pid,
                  int64_t end_ts,
                  std::optional<int64_t> end_time_us,
                  std::optional<int64_t> end_thread_ts);

  // Records a new V8CpuProfileChunk packet (data chunk) for the given session.
  // Subsequent AddNodeMetadata / AddSampleWithMeta calls are tagged with this
  // chunk until the next BeginChunk. No-op if BeginSession was not called.
  void BeginChunk(uint64_t session_id,
                  uint32_t pid,
                  int64_t ts,
                  std::optional<int64_t> thread_ts);

  // Records V8-specific metadata for the node identified by |raw_callsite_id|
  // in the (session_id, pid) profile. Must be called after AddCallsite for the
  // same id. No-op if BeginSession was not called.
  void AddNodeMetadata(uint64_t session_id,
                       uint32_t pid,
                       uint32_t raw_callsite_id,
                       std::optional<base::StringView> function_name,
                       std::optional<base::StringView> url,
                       std::optional<int32_t> script_id,
                       std::optional<int32_t> line,
                       std::optional<int32_t> column,
                       std::optional<base::StringView> code_type,
                       std::optional<base::StringView> deopt_reason);

  // Like AddSample but additionally records the V8 node id and per-sample
  // hit line/column in __intrinsic_v8_cpu_profile_sample. No-op on the v8
  // sample table if BeginSession was not called.
  base::Status AddSampleWithMeta(int64_t ts,
                                 uint64_t session_id,
                                 uint32_t pid,
                                 uint32_t tid,
                                 uint32_t raw_callsite_id,
                                 std::optional<int32_t> line,
                                 std::optional<int32_t> column);

  // Records a (trace_id -> node_id) mapping for the given session.
  // No-op if BeginSession was not called.
  void AddTraceIdMapping(uint64_t session_id,
                         uint32_t pid,
                         uint64_t trace_id,
                         uint32_t node_id);

 private:
  struct State {
    int64_t ts;
    base::FlatHashMap<uint32_t, CallsiteId> callsites;
    base::FlatHashMap<uint32_t, uint32_t> callsite_inferred_parents;
    DummyMemoryMapping* mapping;
    // Set when BeginSession is called.
    std::optional<tables::V8CpuProfileSessionTable::Id> session_row_id;
    // Most recent BeginChunk, used to tag nodes and
    // samples with the chunk that introduced them. Lazily materialized
    // from |pending_chunk| on the first content insert so that empty
    // chunks are not represented in the chunk table.
    std::optional<tables::V8CpuProfileChunkTable::Id> current_chunk_row_id;
    // Pending chunk metadata captured by BeginChunk and consumed by
    // EnsureChunkRow on the first node/sample/trace_id_mapping for the chunk.
    struct PendingChunk {
      int64_t ts;
      std::optional<int64_t> thread_ts;
    };
    std::optional<PendingChunk> pending_chunk;
  };
  // If the current chunk hasn't been materialized yet, create the row using
  // the metadata captured by the most recent BeginChunk. Returns true if a
  // chunk row is now available on |state|.
  bool EnsureChunkRow(State* state);

  base::FlatHashMap<std::pair<uint64_t, uint32_t>,
                    State,
                    base::MurmurHash<std::pair<uint64_t, uint32_t>>>
      state_by_session_and_pid_;

  TraceProcessorContext* const context_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_V8_CPU_PROFILE_TRACKER_H_
