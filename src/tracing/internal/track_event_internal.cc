/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "perfetto/tracing/internal/track_event_internal.h"

#include "perfetto/base/time.h"
#include "perfetto/ext/base/proc_utils.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"

namespace perfetto {
namespace internal {
namespace {

std::atomic<perfetto::base::PlatformThreadID> g_main_thread;

uint64_t GetTimeNs() {
  // TODO(skyostil): Consider using boot time where available.
  return static_cast<uint64_t>(perfetto::base::GetWallTimeNs().count());
}

uint64_t GetNameIidOrZero(std::unordered_map<const char*, uint64_t>& name_map,
                          const char* name) {
  auto it = name_map.find(name);
  if (it == name_map.end())
    return 0;
  return it->second;
}

// static
void WriteSequenceDescriptors(TrackEventTraceContext* ctx, uint64_t timestamp) {
  if (perfetto::base::GetThreadId() == g_main_thread) {
    auto packet = ctx->NewTracePacket();
    packet->set_timestamp(timestamp);
    packet->set_incremental_state_cleared(true);
    auto pd = packet->set_process_descriptor();
    pd->set_pid(static_cast<int32_t>(base::GetProcessId()));
    // TODO(skyostil): Record command line.
  }
  {
    auto packet = ctx->NewTracePacket();
    packet->set_timestamp(timestamp);
    auto td = packet->set_thread_descriptor();
    td->set_pid(static_cast<int32_t>(base::GetProcessId()));
    td->set_tid(static_cast<int32_t>(perfetto::base::GetThreadId()));
  }
}

}  // namespace

TrackEventTraceContext::TrackEventTraceContext(
    TrackEventIncrementalState* incremental_state,
    TracePacketCreator new_trace_packet)
    : incremental_state_(incremental_state),
      new_trace_packet_(std::move(new_trace_packet)) {}

TrackEventTraceContext::TracePacketHandle
TrackEventTraceContext::NewTracePacket() {
  return new_trace_packet_();
}

// static
void TrackEventInternal::Initialize() {
  if (!g_main_thread)
    g_main_thread = perfetto::base::GetThreadId();
}

// static
void TrackEventInternal::EnableTracing(
    const TrackEventCategoryRegistry& registry,
    const DataSourceConfig& config,
    uint32_t instance_index) {
  for (size_t i = 0; i < registry.category_count(); i++) {
    // TODO(skyostil): Support the full category config syntax instead of
    // just strict matching.
    // TODO(skyostil): Support comma-separated categories.
    if (config.legacy_config().empty() ||
        config.legacy_config() == registry.GetCategory(i)->name) {
      registry.EnableCategoryForInstance(i, instance_index);
    }
  }
}

// static
void TrackEventInternal::DisableTracing(
    const TrackEventCategoryRegistry& registry,
    uint32_t instance_index) {
  for (size_t i = 0; i < registry.category_count(); i++)
    registry.DisableCategoryForInstance(i, instance_index);
}

// static
void TrackEventInternal::WriteEvent(
    TrackEventTraceContext* ctx,
    const char* category,
    const char* name,
    perfetto::protos::pbzero::TrackEvent::Type type) {
  PERFETTO_DCHECK(category);
  PERFETTO_DCHECK(g_main_thread);
  auto timestamp = GetTimeNs();

  auto* incr_state = ctx->incremental_state();
  if (incr_state->was_cleared) {
    incr_state->was_cleared = false;
    WriteSequenceDescriptors(ctx, timestamp);
  }
  auto packet = ctx->NewTracePacket();
  packet->set_timestamp(timestamp);

  // We assume that |category| and |name| point to strings with static lifetime.
  // This means we can use their addresses as interning keys.
  uint64_t name_iid = GetNameIidOrZero(incr_state->event_names, name);
  uint64_t category_iid = GetNameIidOrZero(incr_state->categories, category);
  if (PERFETTO_UNLIKELY((name && !name_iid) || !category_iid)) {
    auto id = packet->set_interned_data();
    if (name && !name_iid) {
      auto event_name = id->add_event_names();
      name_iid = incr_state->event_names.size() + 1;
      event_name->set_name(name, strlen(name));
      event_name->set_iid(name_iid);
      incr_state->event_names[name] = name_iid;
    }
    if (!category_iid) {
      auto category_name = id->add_event_categories();
      category_iid = incr_state->categories.size() + 1;
      category_name->set_name(category, strlen(category));
      category_name->set_iid(category_iid);
      incr_state->categories[category] = category_iid;
    }
  }

  auto track_event = packet->set_track_event();
  track_event->set_type(type);
  // TODO(skyostil): Handle multiple categories.
  track_event->add_category_iids(category_iid);
  if (name) {
    auto legacy_event = track_event->set_legacy_event();
    legacy_event->set_name_iid(name_iid);
  }
}

}  // namespace internal
}  // namespace perfetto
