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

#ifndef SRC_TOOLS_TRACE_REPLAY_TRACE_ANALYZER_H_
#define SRC_TOOLS_TRACE_REPLAY_TRACE_ANALYZER_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "protos/perfetto/config/trace_config.gen.h"

#include "src/tools/trace_replay/replay_file.h"

namespace perfetto {
namespace trace_replay {

// Stats about how the seq_id -> buffer mapping was recovered. Forward-declared
// here so TraceAnalysis can use it (definition follows).
struct MappingStats {
  uint64_t packets_resolved_by_stats = 0;
  uint64_t packets_resolved_by_content = 0;
  uint64_t packets_defaulted_to_buf0 = 0;
  uint64_t packets_dropped_orphan = 0;
};

struct TraceAnalysis {
  // The first TraceConfig packet seen in the input trace.
  protos::gen::TraceConfig original_config;

  // sequence_id -> buffer index (from trace_stats.writer_stats).
  std::map<uint64_t, uint32_t> seq_to_buf;

  // pid -> sorted-by-rel-ts records.
  std::map<int32_t, std::vector<ReplayRecord>> records_by_pid;

  // The minimum TracePacket.timestamp observed (anchor for rel_ts_ns).
  uint64_t min_ts_ns = 0;

  // The maximum (rel_ts_ns) across all records.
  uint64_t max_rel_ts_ns = 0;

  // Total packet count emitted (excluding seq_id==1 and ones with no pid).
  uint64_t total_packets = 0;

  // Skipped: had no trusted_pid; sequence_id==1 (kServicePacketSequenceID).
  uint64_t skipped_service_packets = 0;
  uint64_t skipped_no_pid_packets = 0;

  MappingStats mapping_stats;
};

struct AnalyzeOptions {
  // When true, sequence_ids whose target buffer cannot be recovered (neither
  // from trace_stats.writer_stats nor from content-based inference) are
  // dropped silently instead of triggering an error.
  bool ignore_orphan_writers = false;

  // When true:
  //  - sequences that emit packets in a non-default clock domain (i.e. set
  //    `timestamp_clock_id` per-packet, or via `trace_packet_defaults`) are
  //    still kept (their timestamps would be uninterpretable without a clock
  //    converter, so the replay loses real-time pacing);
  //  - every record's `rel_ts_ns` is forced to 0, so the producer emits the
  //    entire replay as fast as it can.
  // When false (default), the analyzer hard-fails on the first non-default
  // clock packet seen, instructing the user to opt into --zero-delay.
  bool zero_delay = false;
};

base::Status AnalyzeTraceFile(const std::string& path,
                              const AnalyzeOptions& opts,
                              TraceAnalysis* out);

}  // namespace trace_replay
}  // namespace perfetto

#endif  // SRC_TOOLS_TRACE_REPLAY_TRACE_ANALYZER_H_
