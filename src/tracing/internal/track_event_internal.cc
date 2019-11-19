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
#include "perfetto/tracing/track_event.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "perfetto/tracing/track_event_interned_data_index.h"
#include "protos/perfetto/common/data_source_descriptor.gen.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"

namespace perfetto {
namespace internal {

BaseTrackEventInternedDataIndex::~BaseTrackEventInternedDataIndex() = default;

namespace {

std::atomic<perfetto::base::PlatformThreadID> g_main_thread;

struct InternedEventCategory
    : public TrackEventInternedDataIndex<
          InternedEventCategory,
          perfetto::protos::pbzero::InternedData::kEventCategoriesFieldNumber,
          const char*,
          SmallInternedDataTraits> {
  static void Add(protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const char* value) {
    auto category = interned_data->add_event_categories();
    category->set_iid(iid);
    category->set_name(value);
  }
};

struct InternedEventName
    : public TrackEventInternedDataIndex<
          InternedEventName,
          perfetto::protos::pbzero::InternedData::kEventNamesFieldNumber,
          const char*,
          SmallInternedDataTraits> {
  static void Add(protos::pbzero::InternedData* interned_data,
                  size_t iid,
                  const char* value) {
    auto name = interned_data->add_event_names();
    name->set_iid(iid);
    name->set_name(value);
  }
};

uint64_t GetTimeNs() {
  // TODO(skyostil): Consider using boot time where available.
  return static_cast<uint64_t>(perfetto::base::GetWallTimeNs().count());
}

// static
void WriteSequenceDescriptors(TraceWriterBase* trace_writer,
                              uint64_t timestamp) {
  if (perfetto::base::GetThreadId() == g_main_thread) {
    auto packet = trace_writer->NewTracePacket();
    packet->set_timestamp(timestamp);
    packet->set_incremental_state_cleared(true);
    auto pd = packet->set_process_descriptor();
    pd->set_pid(static_cast<int32_t>(base::GetProcessId()));
    // TODO(skyostil): Record command line.
  }
  {
    auto packet = trace_writer->NewTracePacket();
    packet->set_timestamp(timestamp);
    auto td = packet->set_thread_descriptor();
    td->set_pid(static_cast<int32_t>(base::GetProcessId()));
    td->set_tid(static_cast<int32_t>(perfetto::base::GetThreadId()));
  }
}

}  // namespace

// static
bool TrackEventInternal::Initialize(
    bool (*register_data_source)(const DataSourceDescriptor&)) {
  if (!g_main_thread)
    g_main_thread = perfetto::base::GetThreadId();

  perfetto::DataSourceDescriptor dsd;
  // TODO(skyostil): Advertise the known categories.
  dsd.set_name("track_event");
  return register_data_source(dsd);
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
EventContext TrackEventInternal::WriteEvent(
    TraceWriterBase* trace_writer,
    TrackEventIncrementalState* incr_state,
    const char* category,
    const char* name,
    perfetto::protos::pbzero::TrackEvent::Type type) {
  PERFETTO_DCHECK(category);
  PERFETTO_DCHECK(g_main_thread);
  auto timestamp = GetTimeNs();

  if (incr_state->was_cleared) {
    incr_state->was_cleared = false;
    WriteSequenceDescriptors(trace_writer, timestamp);
  }
  auto packet = trace_writer->NewTracePacket();
  packet->set_timestamp(timestamp);

  // We assume that |category| and |name| point to strings with static lifetime.
  // This means we can use their addresses as interning keys.
  EventContext ctx(std::move(packet), incr_state);
  size_t category_iid = InternedEventCategory::Get(&ctx, category);

  auto track_event = ctx.event();
  track_event->set_type(type);
  // TODO(skyostil): Handle multiple categories.
  track_event->add_category_iids(category_iid);
  if (name) {
    size_t name_iid = InternedEventName::Get(&ctx, name);
    track_event->set_name_iid(name_iid);
  }
  return ctx;
}

}  // namespace internal
}  // namespace perfetto
