/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_PROFILING_PERF_FTRACE_EVENT_MANAGER_H_
#define SRC_PROFILING_PERF_FTRACE_EVENT_MANAGER_H_

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/profiling/perf/common_types.h"
#include "src/profiling/perf/event_config.h"
#include "src/traced/probes/ftrace/compact_sched.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/ftrace_metadata.h"
#include "src/traced/probes/ftrace/proto_translation_table.h"

namespace perfetto {
namespace profiling {
namespace {
constexpr size_t kCompactSchedInternerThreshold = 64;
using protos::pbzero::FtraceParseStatus;

}  // namespace

class FtraceEventManager {
 public:
  class Bundler {
   public:
    Bundler(std::unique_ptr<TraceWriter> trace_writer,
            uint32_t cpu,
            int32_t clock_id,
            FtraceMetadata* metadata,
            const GenericEventProtoDescriptors* generic_pb_descriptors,
            bool comp_sched_enabled);

    perfetto::CompactSchedBuffer* CompactSchedBuf() { return &comp_sched_buf_; }

    void SetLastEventTimestamp(uint64_t ts) { last_event_ts_ = ts; }

    base::FlatSet<uint32_t>* generic_descriptors_to_write() {
      return cpu_bundler_.generic_descriptors_to_write();
    }

    perfetto::protos::pbzero::FtraceEvent* AddEvent() {
      return cpu_bundler_.GetOrCreateBundle()->add_event();
    }

    perfetto::protos::pbzero::FtraceEventBundle_FtraceError* AddError() {
      return cpu_bundler_.GetOrCreateBundle()->add_error();
    }

    void SetLostEvents(bool value) { events_lost_ = value; }

    // finalizes the current bundle and start new.
    void Finalize(bool close) {
      auto bundle = cpu_bundler_.GetOrCreateBundle();
      bundle->set_ftrace_clock(ftrace_clock_);
      cpu_bundler_.FinalizeAndRunSymbolizer();
      if (!close) {
        cpu_bundler_.StartNewPacket(events_lost_, last_event_ts_);
      }
      events_lost_ = false;
    }

    void MaybeFinalize() {
      if (comp_sched_buf_.interner().interned_comms_size() >
          kCompactSchedInternerThreshold) {
        Finalize(false /*close*/);
      }
    }

   private:
    std::unique_ptr<TraceWriter> trace_writer_ = nullptr;
    perfetto::protos::pbzero::FtraceClock ftrace_clock_;
    CompactSchedBuffer comp_sched_buf_;
    CpuReader::Bundler cpu_bundler_;

    // last timestamp appended in this (unfinalized) bundle
    uint64_t last_event_ts_ = 0;
    // mark that next bundle must set lost_events
    bool events_lost_ = false;
  };

  FtraceEventManager(perfetto::ProtoTranslationTable* table,
                     const EventConfig* event_config,
                     TraceWriter* trace_writer,
                     bool compact_sched_enabled,
                     perfetto::TracingService::ProducerEndpoint* endpoint,
                     perfetto::BufferID buffer_id);

  FtraceParseStatus ProcessSample(const ParsedSample& sample);
  void Flush();
  void Flush(uint32_t cpu, bool events_lost = false);
  void OnEventsLost(uint32_t cpu) { Flush(cpu, true /*events lost*/); }

 private:
  FtraceParseStatus ParseSchedSwitchCompact(const ParsedSample& sample);
  FtraceParseStatus ParseSchedWakingCompact(const ParsedSample& sample);
  FtraceParseStatus ParseFtraceEvent(const ParsedSample& sample,
                                     uint16_t event_id);
  Bundler* GetOrCreateBundler(uint32_t cpu);
  void WriteAndSetParseError(Bundler* bundler,
                             uint64_t timestamp,
                             FtraceParseStatus status);
  void EmitAndClearParseErrors();

  FtraceMetadata metadata_ = {};
  bool compact_sched_enabled_;
  const EventConfig* event_config_;  // Never nullptr
  // event format translation
  perfetto::ProtoTranslationTable* translation_table_;    // Never nullptr
  perfetto::TracingService::ProducerEndpoint* endpoint_;  // Never nullptr
  perfetto::BufferID buffer_id_;
  TraceWriter* trace_writer_;
  FtraceDataSourceConfig ds_config_;
  std::unordered_map<uint32_t, std::unique_ptr<Bundler>> cpu_bundler_map_;
  base::FlatSet<FtraceParseStatus> parse_errors_;
};

}  // namespace profiling
}  // namespace perfetto
#endif  // SRC_PROFILING_PERF_FTRACE_EVENT_MANAGER_H_
