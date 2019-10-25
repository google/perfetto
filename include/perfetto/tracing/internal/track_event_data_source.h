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

#ifndef INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
#define INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

#include <unordered_map>

namespace perfetto {
namespace internal {

struct TrackEventDataSourceTraits : public perfetto::DefaultDataSourceTraits {
  using IncrementalStateType = TrackEventIncrementalState;

  // Use a one shared TLS slot so that all track event data sources write into
  // the same sequence and share interning dictionaries.
  static DataSourceThreadLocalState* GetDataSourceTLS(DataSourceStaticState*,
                                                      TracingTLS* root_tls) {
    return &root_tls->track_event_tls;
  }
};

// A generic track event data source which is instantiated once per track event
// category namespace.
template <typename DataSourceType, const TrackEventCategoryRegistry* Registry>
class TrackEventDataSource
    : public DataSource<DataSourceType, TrackEventDataSourceTraits> {
  using Base = DataSource<DataSourceType, TrackEventDataSourceTraits>;

 public:
  // DataSource implementation.
  void OnSetup(const DataSourceBase::SetupArgs& args) override {
    TrackEventInternal::EnableTracing(*Registry, *args.config,
                                      args.internal_instance_index);
  }

  void OnStart(const DataSourceBase::StartArgs&) override {}

  void OnStop(const DataSourceBase::StopArgs& args) override {
    TrackEventInternal::DisableTracing(*Registry, args.internal_instance_index);
  }

  static void Flush() {
    Base::template Trace([](typename Base::TraceContext ctx) { ctx.Flush(); });
  }

  // This is the inlined entrypoint for all track event trace points. It tries
  // to be as lightweight as possible in terms of instructions and aims to
  // compile down to an unlikely conditional jump to the actual trace writing
  // function.
  template <size_t CategoryIndex, typename Callback>
  static void CallIfCategoryEnabled(Callback callback) PERFETTO_ALWAYS_INLINE {
    Base::template CallIfEnabled<CategoryTracePointTraits<CategoryIndex>>(
        [&callback](uint32_t instances) { callback(instances); });
  }

  // Once we've determined tracing to be enabled for this category, actually
  // write a trace event. Outlined to avoid bloating code at the actual trace
  // point.
  // TODO(skyostil): Investigate whether this should be fully outlined to reduce
  // binary size.
  template <size_t CategoryIndex>
  static void TraceForCategory(uint32_t instances,
                               const char* event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type)
      PERFETTO_NO_INLINE {
    Base::template TraceWithInstances<CategoryTracePointTraits<CategoryIndex>>(
        instances, [&](typename Base::TraceContext ctx) {
          TrackEventTraceContext track_event_ctx(
              ctx.GetIncrementalState(),
              [&ctx]() { return ctx.NewTracePacket(); });
          // TODO(skyostil): Intern categories at compile time.
          TrackEventInternal::WriteEvent(
              &track_event_ctx, Registry->GetCategory(CategoryIndex)->name,
              event_name, type);
        });
  }

  static bool Register() {
    TrackEventInternal::Initialize();
    perfetto::DataSourceDescriptor dsd;
    // TODO(skyostil): Advertise the known categories.
    dsd.set_name("track_event");
    return Base::Register(dsd);
  }

 private:
  // Each category has its own enabled/disabled state, stored in the category
  // registry.
  template <size_t CategoryIndex>
  struct CategoryTracePointTraits {
    static constexpr std::atomic<uint8_t>* GetActiveInstances() {
      return Registry->GetCategoryState(CategoryIndex);
    }
  };
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
