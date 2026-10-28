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

#ifndef SRC_SHARED_LIB_TRACK_EVENT_DS_H_
#define SRC_SHARED_LIB_TRACK_EVENT_DS_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/base/flat_set.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/public/abi/track_event_abi.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/data_source_internal.h"
#include "perfetto/tracing/internal/data_source_type.h"
#include "src/shared_lib/track_event/category_impl.h"
#include "src/shared_lib/track_event/global_state.h"
#include "src/shared_lib/track_event/intern_map.h"

#include "protos/perfetto/common/data_source_descriptor.gen.h"  // IWYU pragma: keep
#include "protos/perfetto/config/track_event/track_event_config.gen.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"

namespace perfetto::shlib {

// The encoded TracePacket, except the trailing timestamp, of an event without
// extra data. Valid until the incremental state is cleared.
struct EventPrefix {
  static constexpr size_t kMaxSize = 48;
  static constexpr size_t kMaxNameSize = 32;
  bool valid = false;
  int32_t type = 0;
  uint32_t size = 0;
  const void* cat = nullptr;
  const char* name_ptr = nullptr;
  // NUL-terminated copy of the name, to detect a reused name buffer.
  char name[kMaxNameSize];
  uint8_t bytes[kMaxSize];
};

struct TrackEventIncrementalState {
  static constexpr size_t kNumEventPrefixes = 16;
  // A heap-allocated message for storing newly seen interned data while we are
  // in the middle of writing a track event. When a track event wants to write
  // new interned data into the trace, it is first serialized into this message
  // and then flushed to the real trace in EventContext when the packet ends.
  // The message is cached here as a part of incremental state so that we can
  // reuse the underlying buffer allocation for subsequently written interned
  // data.
  protozero::HeapBuffered<protos::pbzero::InternedData>
      serialized_interned_data;
  uint64_t last_timestamp_ns = 0;
  // Cached uuid of the current thread's track, also used by nested tracks.
  uint64_t thread_track_uuid = 0;
  bool was_cleared = true;
  base::FlatSet<uint64_t> seen_track_uuids;
  std::vector<bool> seen_category_iids;
  // Map from serialized representation of a dynamic category to its enabled
  // state.
  base::FlatHashMap<std::string, bool> dynamic_categories;
  InternMap iids;
  EventPrefix event_prefixes[kNumEventPrefixes];

  // Marks cat_iid as seen in the current incremental state.
  // Returns true only on the first call for this IID since creation or Clear().
  bool MarkCategorySeen(uint64_t cat_iid) {
    if (cat_iid >= seen_category_iids.size())
      seen_category_iids.resize((static_cast<size_t>(cat_iid) + 1) * 2);
    if (seen_category_iids[cat_iid])
      return false;
    seen_category_iids[cat_iid] = true;
    return true;
  }

  // Clears the incremental state without destroying and recreating this object.
  // This allows reusing allocated memory in hash maps and other data structures
  // instead of deallocating and reallocating them.
  void Clear() {
    was_cleared = true;
    serialized_interned_data.Reset();
    last_timestamp_ns = 0;
    seen_track_uuids.clear();
    seen_category_iids.assign(seen_category_iids.size(), false);
    dynamic_categories.Clear();
    iids.Clear();
    for (EventPrefix& prefix : event_prefixes)
      prefix.valid = false;
  }
};

struct TrackEventTlsState {
  template <typename TraceContext>
  explicit TrackEventTlsState(const TraceContext& trace_context) {
    auto locked_ds = trace_context.GetDataSourceLocked();
    bool disable_incremental_timestamps = false;
    timestamp_unit_multiplier = 1;
    if (locked_ds.valid()) {
      const auto& config = locked_ds->GetConfig();
      disable_incremental_timestamps = config.disable_incremental_timestamps();
      if (config.has_timestamp_unit_multiplier() &&
          config.timestamp_unit_multiplier() != 0) {
        timestamp_unit_multiplier = config.timestamp_unit_multiplier();
      }
    }
    if (disable_incremental_timestamps) {
      if (timestamp_unit_multiplier == 1) {
        default_clock_id = PerfettoDsGetDefaultClockId();
      } else {
        default_clock_id = PERFETTO_TE_TIMESTAMP_TYPE_ABSOLUTE;
      }
    } else {
      default_clock_id = PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL;
    }
  }
  uint32_t default_clock_id;
  uint64_t timestamp_unit_multiplier;
};

struct TrackEventDataSourceTraits : public perfetto::DefaultDataSourceTraits {
  using IncrementalStateType = TrackEventIncrementalState;
  using TlsStateType = TrackEventTlsState;

  // Clear the incremental state without destroying and recreating it. This
  // allows reusing allocated memory in hash maps.
  static bool ClearIncrementalState(TrackEventIncrementalState* incr_state) {
    incr_state->Clear();
    return true;
  }
};

class TrackEvent
    : public perfetto::DataSource<TrackEvent, TrackEventDataSourceTraits> {
 public:
  ~TrackEvent() override;
  void OnSetup(const DataSourceBase::SetupArgs& args) override;
  void OnStart(const DataSourceBase::StartArgs&) override;
  void OnStop(const DataSourceBase::StopArgs&) override;

  const perfetto::protos::gen::TrackEventConfig& GetConfig() const {
    return config_;
  }

  static void Init(DataSourceDescriptor dsd) {
    dsd.set_name("track_event");
    Register(dsd);
  }

  static void UpdateDescriptorFromCategories(DataSourceDescriptor dsd) {
    dsd.set_name("track_event");
    UpdateDescriptor(dsd);
  }

  static void CategorySetCallback(struct PerfettoTeCategoryImpl* cat,
                                  PerfettoTeCategoryImplCallback cb,
                                  void* user_arg) {
    GlobalState::Instance().CategorySetCallback(cat, cb, user_arg);
  }

  static internal::DataSourceType* GetType();

  static internal::DataSourceThreadLocalState** GetTlsState() {
    return &tls_state_;
  }

  static bool IsDynamicCategoryEnabled(
      uint32_t inst_idx,
      perfetto::shlib::TrackEventIncrementalState* incr_state,
      const struct PerfettoTeCategoryDescriptor& desc);

 private:
  uint32_t inst_id_;
  perfetto::protos::gen::TrackEventConfig config_;
};

struct TracePointTraits {
  struct TracePointData {
    struct PerfettoTeCategoryImpl* enabled;
  };
  static constexpr std::atomic<uint8_t>* GetActiveInstances(
      TracePointData data) {
    return &data.enabled->instances;
  }
};

}  // namespace perfetto::shlib

PERFETTO_DECLARE_DATA_SOURCE_STATIC_MEMBERS(
    perfetto::shlib::TrackEvent,
    perfetto::shlib::TrackEventDataSourceTraits);

inline perfetto::internal::DataSourceType*
perfetto::shlib::TrackEvent::GetType() {
  return &perfetto::shlib::TrackEvent::Helper::type();
}

#endif  // SRC_SHARED_LIB_TRACK_EVENT_DS_H_
