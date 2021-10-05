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

#ifndef INCLUDE_PERFETTO_TRACING_TRACK_EVENT_ARGS_H_
#define INCLUDE_PERFETTO_TRACING_TRACK_EVENT_ARGS_H_

#include "perfetto/tracing/event_context.h"

#include <functional>

namespace perfetto {

// A helper to add |flow_id| as a non-terminating flow id to TRACE_EVENT
// inline: TRACE_EVENT(..., perfetto::Flow(42));
PERFETTO_ALWAYS_INLINE inline std::function<void(EventContext&)> Flow(
    uint64_t flow_id) {
  return [flow_id](perfetto::EventContext& ctx) {
    ctx.event()->add_flow_ids(flow_id);
  };
}

PERFETTO_ALWAYS_INLINE inline std::function<void(EventContext&)>
TerminatingFlow(uint64_t flow_id) {
  return [flow_id](perfetto::EventContext& ctx) {
    ctx.event()->add_terminating_flow_ids(flow_id);
  };
}

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_TRACK_EVENT_ARGS_H_
