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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_GENERIC_KERNEL_GENERIC_KERNEL_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_GENERIC_KERNEL_GENERIC_KERNEL_PARSER_H_

#include <cstdint>

#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/sched_event_state.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

class GenericKernelParser {
 public:
  explicit GenericKernelParser(TraceProcessorContext* context);

  void ParseGenericTaskStateEvent(int64_t ts, protozero::ConstBytes data);

 private:
  enum SchedSwitchType {
    // No context switch event was handled.
    SCHED_SWITCH_NONE = 0,
    // A new context switch slice was opened
    // without any side effects.
    SCHED_SWITCH_START,
    // A new context switch slice was opened
    // and the previous running thread's slice
    // was closed without knowing the end_state.
    SCHED_SWITCH_START_WITH_PENDING,
    // The previously started context switch slice
    // was closed.
    SCHED_SWITCH_CLOSE,
    // A closed context switch with unknown end
    // state was updated with a new valid end
    // state. No new context switch slice was
    // opened/closed.
    SCHED_SWITCH_UPDATE_END_STATE,
  };

  SchedSwitchType PushSchedSwitch(int64_t ts,
                                  uint32_t cpu,
                                  uint32_t tid,
                                  UniqueTid utid,
                                  StringId state_string_id,
                                  int32_t prio);

  void InsertPendingStateInfoForTid(
      UniqueTid utid,
      SchedEventState::PendingSchedInfo sched_info);

  std::optional<SchedEventState::PendingSchedInfo> GetPendingStateInfoForTid(
      UniqueTid utid);

  void RemovePendingStateInfoForTid(UniqueTid utid);

  StringId TaskStateToStringId(size_t state);

  TraceProcessorContext* context_;
  // Keeps track of the latest context switches
  SchedEventState sched_event_state_;
  std::vector<std::optional<SchedEventState::PendingSchedInfo>>
      pending_state_per_utid_;

  StringId running_string_id_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_GENERIC_KERNEL_GENERIC_KERNEL_PARSER_H_
