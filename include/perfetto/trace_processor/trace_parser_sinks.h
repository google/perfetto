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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_SINKS_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_SINKS_H_

#include <cstdint>
#include <optional>
#include <string_view>
#include "perfetto/base/status.h"

namespace perfetto::trace_processor {
// Parser-local string identity plus contents. Copy the view to keep it beyond
// its callback. ID zero denotes the null string, distinct from an empty string.
struct TraceParserString {
  uint32_t id = 0;
  std::string_view value;
};

class TraceParserTableSink {
 public:
  virtual ~TraceParserTableSink() = default;
  // Eviction permits later patches; it is not a completion promise.
  virtual base::Status OnStorageFrontierAdvance(uint32_t) {
    return base::OkStatus();
  }
  // Earlier rows cannot change but may still be referenced by later records.
  virtual base::Status OnCompletionFrontierAdvance(uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnReset() { return base::OkStatus(); }
  virtual base::Status OnEndOfTrace() { return base::OkStatus(); }
};

// Hand-written initial interfaces. Add further tables here as consumers need
// them; these public row types do not expose dataframe or generated-table
// types.
class SliceSink : public TraceParserTableSink {
 public:
  struct Row {
    int64_t ts{};
    int64_t dur{};
    uint32_t track_id{};
    std::optional<TraceParserString> category{};
    std::optional<TraceParserString> name{};
    uint32_t depth{};
    std::optional<uint32_t> parent_id{};
    std::optional<uint32_t> arg_set_id{};
    std::optional<int64_t> thread_ts{};
    std::optional<int64_t> thread_dur{};
    std::optional<int64_t> thread_instruction_count{};
    std::optional<int64_t> thread_instruction_delta{};
  };
  virtual base::Status OnInsert(uint32_t id, const Row&) = 0;
  virtual base::Status OnTsUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnDurUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnTrackIdUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnCategoryUpdate(uint32_t,
                                        std::optional<TraceParserString>) {
    return base::OkStatus();
  }
  virtual base::Status OnNameUpdate(uint32_t,
                                    std::optional<TraceParserString>) {
    return base::OkStatus();
  }
  virtual base::Status OnDepthUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnParentIdUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnArgSetIdUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnThreadTsUpdate(uint32_t, std::optional<int64_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnThreadDurUpdate(uint32_t, std::optional<int64_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnThreadInstructionCountUpdate(uint32_t,
                                                      std::optional<int64_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnThreadInstructionDeltaUpdate(uint32_t,
                                                      std::optional<int64_t>) {
    return base::OkStatus();
  }
};

class ArgSink : public TraceParserTableSink {
 public:
  struct Row {
    uint32_t arg_set_id{};
    TraceParserString flat_key{};
    TraceParserString key{};
    std::optional<int64_t> int_value{};
    std::optional<TraceParserString> string_value{};
    std::optional<double> real_value{};
    TraceParserString value_type{};
  };
  virtual base::Status OnInsert(uint32_t id, const Row&) = 0;
  virtual base::Status OnArgSetIdUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnFlatKeyUpdate(uint32_t, TraceParserString) {
    return base::OkStatus();
  }
  virtual base::Status OnKeyUpdate(uint32_t, TraceParserString) {
    return base::OkStatus();
  }
  virtual base::Status OnIntValueUpdate(uint32_t, std::optional<int64_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnStringValueUpdate(uint32_t,
                                           std::optional<TraceParserString>) {
    return base::OkStatus();
  }
  virtual base::Status OnRealValueUpdate(uint32_t, std::optional<double>) {
    return base::OkStatus();
  }
  virtual base::Status OnValueTypeUpdate(uint32_t, TraceParserString) {
    return base::OkStatus();
  }
};

class SchedSink : public TraceParserTableSink {
 public:
  struct Row {
    int64_t ts{};
    int64_t dur{};
    uint32_t utid{};
    TraceParserString end_state{};
    int32_t priority{};
    uint32_t ucpu{};
  };
  virtual base::Status OnInsert(uint32_t id, const Row&) = 0;
  virtual base::Status OnTsUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnDurUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnUtidUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnEndStateUpdate(uint32_t, TraceParserString) {
    return base::OkStatus();
  }
  virtual base::Status OnPriorityUpdate(uint32_t, int32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnUcpuUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
};

class ThreadStateSink : public TraceParserTableSink {
 public:
  struct Row {
    int64_t ts{};
    int64_t dur{};
    uint32_t utid{};
    TraceParserString state{};
    std::optional<uint32_t> io_wait{};
    std::optional<TraceParserString> blocked_function{};
    std::optional<uint32_t> waker_utid{};
    std::optional<uint32_t> waker_id{};
    std::optional<uint32_t> irq_context{};
    std::optional<uint32_t> ucpu{};
  };
  virtual base::Status OnInsert(uint32_t id, const Row&) = 0;
  virtual base::Status OnTsUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnDurUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnUtidUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnStateUpdate(uint32_t, TraceParserString) {
    return base::OkStatus();
  }
  virtual base::Status OnIoWaitUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnBlockedFunctionUpdate(
      uint32_t,
      std::optional<TraceParserString>) {
    return base::OkStatus();
  }
  virtual base::Status OnWakerUtidUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnWakerIdUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnIrqContextUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
  virtual base::Status OnUcpuUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
};

class CounterSink : public TraceParserTableSink {
 public:
  struct Row {
    int64_t ts = 0;
    uint32_t track_id = 0;
    double value = 0;
    std::optional<uint32_t> arg_set_id;
  };
  virtual base::Status OnInsert(uint32_t id, const Row&) = 0;
  virtual base::Status OnTsUpdate(uint32_t, int64_t) {
    return base::OkStatus();
  }
  virtual base::Status OnTrackIdUpdate(uint32_t, uint32_t) {
    return base::OkStatus();
  }
  virtual base::Status OnValueUpdate(uint32_t, double) {
    return base::OkStatus();
  }
  virtual base::Status OnArgSetIdUpdate(uint32_t, std::optional<uint32_t>) {
    return base::OkStatus();
  }
};

// Borrowed until parser destruction. A null pointer discards that table's
// output; storage retention and frontier advancement remain parser decisions.
struct TraceParserSinks {
  SliceSink* slice = nullptr;
  ArgSink* args = nullptr;
  SchedSink* sched = nullptr;
  ThreadStateSink* thread_states = nullptr;
  CounterSink* counters = nullptr;
};
}  // namespace perfetto::trace_processor
#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PARSER_SINKS_H_
