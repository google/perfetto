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

#include "perfetto/tracing/track_event.h"

#include "perfetto/ext/base/proc_utils.h"
#include "perfetto/ext/base/thread_utils.h"
#include "perfetto/trace/interned_data/interned_data.pbzero.h"
#include "perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "perfetto/trace/track_event/thread_descriptor.pbzero.h"
#include "perfetto/trace/track_event/track_event.pbzero.h"
#include "perfetto/tracing/core/data_source_descriptor.h"
#include "perfetto/tracing/data_source.h"

namespace perfetto {
namespace internal {
namespace {

std::atomic<perfetto::base::PlatformThreadID> g_main_thread;

uint64_t GetNameIidOrZero(std::unordered_map<const char*, uint64_t>& name_map,
                          const char* name) {
  auto it = name_map.find(name);
  if (it == name_map.end())
    return 0;
  return it->second;
}

}  // namespace

void TrackEventDataSource::OnSetup(const SetupArgs&) {}
void TrackEventDataSource::OnStart(const StartArgs&) {}
void TrackEventDataSource::OnStop(const StopArgs&) {}

// static
void TrackEventDataSource::WriteEventImpl(
    internal::TrackEventDataSource::TraceContext ctx,
    const char* category,
    const char* name,
    perfetto::protos::pbzero::TrackEvent::Type type) {
  PERFETTO_DCHECK(category);
  auto timestamp = TrackEvent::GetTimeNs();

  auto* incr_state = ctx.GetIncrementalState();
  if (incr_state->was_cleared) {
    incr_state->was_cleared = false;
    WriteSequenceDescriptors(&ctx, timestamp);
  }
  auto packet = ctx.NewTracePacket();
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

// static
void TrackEventDataSource::WriteSequenceDescriptors(
    internal::TrackEventDataSource::TraceContext* ctx,
    uint64_t timestamp) {
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

}  // namespace internal

// static
void TrackEvent::Initialize() {
  internal::g_main_thread = perfetto::base::GetThreadId();
  DataSourceDescriptor dsd;
  dsd.set_name("track_event");
  internal::TrackEventDataSource::Register(dsd);
}

}  // namespace perfetto

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(
    perfetto::internal::TrackEventDataSource,
    perfetto::internal::TrackEventIncrementalState);
