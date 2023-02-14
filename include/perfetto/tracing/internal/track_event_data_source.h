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
#include "perfetto/base/template_util.h"
#include "perfetto/protozero/message_handle.h"
#include "perfetto/tracing/core/data_source_config.h"
#include "perfetto/tracing/data_source.h"
#include "perfetto/tracing/event_context.h"
#include "perfetto/tracing/internal/track_event_internal.h"
#include "perfetto/tracing/internal/write_track_event_args.h"
#include "perfetto/tracing/track.h"
#include "perfetto/tracing/track_event_category_registry.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/config/track_event/track_event_config.gen.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

#include <type_traits>

namespace perfetto {

namespace {

class StopArgsImpl : public DataSourceBase::StopArgs {
 public:
  // HandleAsynchronously() can optionally be called to defer the tracing
  // session stop and write track events just before stopping. This function
  // returns a closure that must be invoked after the last track events have
  // been emitted. The caller also needs to explicitly call
  // TrackEvent::Flush() because no other implicit flushes will happen after
  // the stop signal.
  // See the comment in include/perfetto/tracing/data_source.h for more info.
  std::function<void()> HandleStopAsynchronously() const override {
    auto closure = std::move(async_stop_closure);
    async_stop_closure = std::function<void()>();
    return closure;
  }

  mutable std::function<void()> async_stop_closure;
};

}  // namespace

// A function for converting an abstract timestamp into a
// perfetto::TraceTimestamp struct. By specialising this template and defining
// static ConvertTimestampToTraceTimeNs function in it the user can register
// additional timestamp types. The return value should specify the
// clock domain used by the timestamp as well as its value.
//
// The supported clock domains are the ones described in
// perfetto.protos.ClockSnapshot. However, custom clock IDs (>=64) are
// reserved for internal use by the SDK for the time being.
// The timestamp value should be in nanoseconds regardless of the clock domain.
template <typename T>
struct TraceTimestampTraits;

// A pass-through implementation for raw uint64_t nanosecond timestamps.
template <>
struct TraceTimestampTraits<uint64_t> {
  static inline TraceTimestamp ConvertTimestampToTraceTimeNs(
      const uint64_t& timestamp) {
    return {static_cast<uint32_t>(internal::TrackEventInternal::GetClockId()), timestamp};
  }
};

// A pass-through implementation for the trace timestamp structure.
template <>
struct TraceTimestampTraits<TraceTimestamp> {
  static inline TraceTimestamp ConvertTimestampToTraceTimeNs(
      const TraceTimestamp& timestamp) {
    return timestamp;
  }
};

namespace internal {
namespace {

// Checks if |T| is a valid track.
template <typename T>
static constexpr bool IsValidTrack() {
  return std::is_convertible<T, Track>::value;
}

// Checks if |T| is a valid non-counter track.
template <typename T>
static constexpr bool IsValidNormalTrack() {
  return std::is_convertible<T, Track>::value &&
         !std::is_convertible<T, CounterTrack>::value;
}

// Because the user can use arbitrary timestamp types, we can't compare against
// any known base type here. Instead, we check that a track or a trace lambda
// isn't being interpreted as a timestamp.
template <typename T,
          typename CanBeConvertedToNsCheck = decltype(
              ::perfetto::TraceTimestampTraits<typename base::remove_cvref_t<
                  T>>::ConvertTimestampToTraceTimeNs(std::declval<T>())),
          typename NotTrackCheck =
              typename std::enable_if<!IsValidNormalTrack<T>()>::type,
          typename NotLambdaCheck =
              typename std::enable_if<!IsValidTraceLambda<T>()>::type>
static constexpr bool IsValidTimestamp() {
  return true;
}

// Taken from C++17
template <typename...>
using void_t = void;

// Returns true iff `GetStaticString(T)` is defined OR T == DynamicString.
template <typename T, typename = void>
struct IsValidEventNameType
    : std::is_same<perfetto::DynamicString, typename std::decay<T>::type> {};

template <typename T>
struct IsValidEventNameType<
    T,
    void_t<decltype(GetStaticString(std::declval<T>()))>> : std::true_type {};

template <typename T>
inline void ValidateEventNameType() {
  static_assert(
      IsValidEventNameType<T>::value,
      "Event names must be static strings. To use dynamic event names, see "
      "https://perfetto.dev/docs/instrumentation/"
      "track-events#dynamic-event-names");
}

}  // namespace

// Traits for dynamic categories.
template <typename CategoryType>
struct CategoryTraits {
  static constexpr bool kIsDynamic = true;
  static constexpr const Category* GetStaticCategory(
      const TrackEventCategoryRegistry*,
      const CategoryType&) {
    return nullptr;
  }
  static size_t GetStaticIndex(const CategoryType&) {
    PERFETTO_DCHECK(false);  // Not reached.
    return TrackEventCategoryRegistry::kDynamicCategoryIndex;
  }
  static DynamicCategory GetDynamicCategory(const CategoryType& category) {
    return DynamicCategory{category};
  }
};

// Traits for static categories.
template <>
struct CategoryTraits<size_t> {
  static constexpr bool kIsDynamic = false;
  static const Category* GetStaticCategory(
      const TrackEventCategoryRegistry* registry,
      size_t category_index) {
    return registry->GetCategory(category_index);
  }
  static constexpr size_t GetStaticIndex(size_t category_index) {
    return category_index;
  }
  static DynamicCategory GetDynamicCategory(size_t) {
    PERFETTO_DCHECK(false);  // Not reached.
    return DynamicCategory();
  }
};

struct TrackEventDataSourceTraits : public perfetto::DefaultDataSourceTraits {
  using IncrementalStateType = TrackEventIncrementalState;
  using TlsStateType = TrackEventTlsState;

  // Use a one shared TLS slot so that all track event data sources write into
  // the same sequence and share interning dictionaries.
  static DataSourceThreadLocalState* GetDataSourceTLS(DataSourceStaticState*,
                                                      TracingTLS* root_tls) {
    return &root_tls->track_event_tls;
  }
};

// A generic track event data source which is instantiated once per track event
// category namespace.
template <typename DerivedDataSource,
          const TrackEventCategoryRegistry* Registry>
class TrackEventDataSource
    : public DataSource<DerivedDataSource, TrackEventDataSourceTraits> {
  using Base = DataSource<DerivedDataSource, TrackEventDataSourceTraits>;

 public:
  static constexpr bool kRequiresCallbacksUnderLock = false;

  // Add or remove a session observer for this track event data source. The
  // observer will be notified about started and stopped tracing sessions.
  // Returns |true| if the observer was successfully added (i.e., the maximum
  // number of observers wasn't exceeded).
  static bool AddSessionObserver(TrackEventSessionObserver* observer) {
    return TrackEventInternal::AddSessionObserver(*Registry, observer);
  }

  static void RemoveSessionObserver(TrackEventSessionObserver* observer) {
    TrackEventInternal::RemoveSessionObserver(*Registry, observer);
  }

  // DataSource implementation.
  void OnSetup(const DataSourceBase::SetupArgs& args) override {
    auto config_raw = args.config->track_event_config_raw();
    bool ok = config_.ParseFromArray(config_raw.data(), config_raw.size());
    PERFETTO_DCHECK(ok);
    TrackEventInternal::EnableTracing(*Registry, config_, args);
  }

  void OnStart(const DataSourceBase::StartArgs& args) override {
    TrackEventInternal::OnStart(*Registry, args);
  }

  void OnStop(const DataSourceBase::StopArgs& args) override {
    auto outer_stop_closure = args.HandleStopAsynchronously();
    StopArgsImpl inner_stop_args{};
    uint32_t internal_instance_index = args.internal_instance_index;
    inner_stop_args.internal_instance_index = internal_instance_index;
    inner_stop_args.async_stop_closure = [internal_instance_index,
                                          outer_stop_closure] {
      TrackEventInternal::DisableTracing(*Registry, internal_instance_index);
      outer_stop_closure();
    };

    TrackEventInternal::OnStop(*Registry, inner_stop_args);

    // If inner_stop_args.HandleStopAsynchronously() hasn't been called,
    // run the async closure here.
    if (inner_stop_args.async_stop_closure)
      std::move(inner_stop_args.async_stop_closure)();
  }

  void WillClearIncrementalState(
      const DataSourceBase::ClearIncrementalStateArgs& args) override {
    TrackEventInternal::WillClearIncrementalState(*Registry, args);
  }

  static void Flush() {
    Base::template Trace([](typename Base::TraceContext ctx) { ctx.Flush(); });
  }

  // Determine if *any* tracing category is enabled.
  static bool IsEnabled() {
    bool enabled = false;
    Base::template CallIfEnabled(
        [&](uint32_t /*instances*/) { enabled = true; });
    return enabled;
  }

  // Determine if tracing for the given static category is enabled.
  static bool IsCategoryEnabled(size_t category_index) {
    return Registry->GetCategoryState(category_index)
        ->load(std::memory_order_relaxed);
  }

  // Determine if tracing for the given dynamic category is enabled.
  static bool IsDynamicCategoryEnabled(
      const DynamicCategory& dynamic_category) {
    bool enabled = false;
    Base::template Trace([&](typename Base::TraceContext ctx) {
      enabled = IsDynamicCategoryEnabled(&ctx, dynamic_category);
    });
    return enabled;
  }

  // This is the inlined entrypoint for all track event trace points. It tries
  // to be as lightweight as possible in terms of instructions and aims to
  // compile down to an unlikely conditional jump to the actual trace writing
  // function.
  template <typename Callback>
  static void CallIfCategoryEnabled(size_t category_index,
                                    Callback callback) PERFETTO_ALWAYS_INLINE {
    Base::template CallIfEnabled<CategoryTracePointTraits>(
        [&callback](uint32_t instances) { callback(instances); },
        {category_index});
  }

  // Once we've determined tracing to be enabled for this category, actually
  // write a trace event onto this thread's default track. Outlined to avoid
  // bloating code (mostly stack depth) at the actual trace point.
  //
  // The following combination of parameters is supported (in the given order):
  // - Zero or one track,
  // - Zero or one custom timestamp,
  // - Arbitrary number of debug annotations.
  // - Zero or one lambda.

  // Trace point which does not take a track or timestamp.
  template <typename CategoryType,
            typename EventNameType,
            typename... Arguments>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType& event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               Arguments&&... args) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type,
                         TrackEventInternal::kDefaultTrack,
                         TrackEventInternal::GetTraceTime(),
                         std::forward<Arguments>(args)...);
  }

  // Trace point which takes a track, but not timestamp.
  // NOTE: Here track should be captured using universal reference (TrackType&&)
  // instead of const TrackType& to ensure that the proper overload is selected
  // (otherwise the compiler will fail to disambiguate between adding const& and
  // parsing track as a part of Arguments...).
  template <typename TrackType,
            typename CategoryType,
            typename EventNameType,
            typename... Arguments,
            typename TrackTypeCheck = typename std::enable_if<
                std::is_convertible<TrackType, Track>::value>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType& event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               TrackType&& track,
                               Arguments&&... args) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(
        instances, category, event_name, type, std::forward<TrackType>(track),
        TrackEventInternal::GetTraceTime(), std::forward<Arguments>(args)...);
  }

  // Trace point which takes a timestamp, but not track.
  template <typename CategoryType,
            typename EventNameType,
            typename TimestampType = uint64_t,
            typename... Arguments,
            typename TimestampTypeCheck = typename std::enable_if<
                IsValidTimestamp<TimestampType>()>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType& event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               TimestampType&& timestamp,
                               Arguments&&... args) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type,
                         TrackEventInternal::kDefaultTrack,
                         std::forward<TimestampType>(timestamp),
                         std::forward<Arguments>(args)...);
  }

  // Trace point which takes a timestamp and a track.
  template <typename TrackType,
            typename CategoryType,
            typename EventNameType,
            typename TimestampType = uint64_t,
            typename... Arguments,
            typename TrackTypeCheck = typename std::enable_if<
                std::is_convertible<TrackType, Track>::value>::type,
            typename TimestampTypeCheck = typename std::enable_if<
                IsValidTimestamp<TimestampType>()>::type>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType& event_name,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               TrackType&& track,
                               TimestampType&& timestamp,
                               Arguments&&... args) PERFETTO_NO_INLINE {
    TraceForCategoryImpl(instances, category, event_name, type,
                         std::forward<TrackType>(track),
                         std::forward<TimestampType>(timestamp),
                         std::forward<Arguments>(args)...);
  }

  // Trace point with with a counter sample.
  template <typename CategoryType, typename EventNameType, typename ValueType>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType&,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               CounterTrack track,
                               ValueType value) PERFETTO_ALWAYS_INLINE {
    PERFETTO_DCHECK(type == perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
    TraceForCategory(instances, category, /*name=*/nullptr, type, track,
                     TrackEventInternal::GetTraceTime(), value);
  }

  // Trace point with with a timestamp and a counter sample.
  template <typename CategoryType,
            typename EventNameType,
            typename TimestampType = uint64_t,
            typename TimestampTypeCheck = typename std::enable_if<
                IsValidTimestamp<TimestampType>()>::type,
            typename ValueType>
  static void TraceForCategory(uint32_t instances,
                               const CategoryType& category,
                               const EventNameType&,
                               perfetto::protos::pbzero::TrackEvent::Type type,
                               CounterTrack track,
                               TimestampType timestamp,
                               ValueType value) PERFETTO_ALWAYS_INLINE {
    PERFETTO_DCHECK(type == perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER);
    TraceForCategoryImpl(
        instances, category, /*name=*/nullptr, type, track, timestamp,
        [&](EventContext event_ctx) {
          if (std::is_integral<ValueType>::value) {
            int64_t value_int64 = static_cast<int64_t>(value);
            if (track.is_incremental()) {
              TrackEventIncrementalState* incr_state =
                  event_ctx.GetIncrementalState();
              PERFETTO_DCHECK(incr_state != nullptr);
              auto prv_value =
                  incr_state->last_counter_value_per_track[track.uuid];
              event_ctx.event()->set_counter_value(value_int64 - prv_value);
              prv_value = value_int64;
              incr_state->last_counter_value_per_track[track.uuid] = prv_value;
            } else {
              event_ctx.event()->set_counter_value(value_int64);
            }
          } else {
            event_ctx.event()->set_double_counter_value(
                static_cast<double>(value));
          }
        });
  }

  // Initialize the track event library. Should be called before tracing is
  // enabled.
  static bool Register() {
    // Registration is performed out-of-line so users don't need to depend on
    // DataSourceDescriptor C++ bindings.
    return TrackEventInternal::Initialize(
        *Registry,
        [](const DataSourceDescriptor& dsd) { return Base::Register(dsd); });
  }

  // Record metadata about different types of timeline tracks. See Track.
  static void SetTrackDescriptor(const Track& track,
                                 const protos::gen::TrackDescriptor& desc) {
    PERFETTO_DCHECK(track.uuid == desc.uuid());
    TrackRegistry::Get()->UpdateTrack(track, desc.SerializeAsString());
    Base::template Trace([&](typename Base::TraceContext ctx) {
      TrackEventInternal::WriteTrackDescriptor(
          track, ctx.tls_inst_->trace_writer.get(), ctx.GetIncrementalState(),
          *ctx.GetCustomTlsState(), TrackEventInternal::GetTraceTime());
    });
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetTrackDescriptor(
      const Track& track,
      std::function<void(protos::pbzero::TrackDescriptor*)> callback) {
    SetTrackDescriptorImpl(track, std::move(callback));
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetProcessDescriptor(
      std::function<void(protos::pbzero::TrackDescriptor*)> callback,
      const ProcessTrack& track = ProcessTrack::Current()) {
    SetTrackDescriptorImpl(std::move(track), std::move(callback));
  }

  // DEPRECATED. Only kept for backwards compatibility.
  static void SetThreadDescriptor(
      std::function<void(protos::pbzero::TrackDescriptor*)> callback,
      const ThreadTrack& track = ThreadTrack::Current()) {
    SetTrackDescriptorImpl(std::move(track), std::move(callback));
  }

  static void EraseTrackDescriptor(const Track& track) {
    TrackRegistry::Get()->EraseTrack(track);
  }

  // Returns the current trace timestamp in nanoseconds. Note the returned
  // timebase may vary depending on the platform, but will always match the
  // timestamps recorded by track events (see GetTraceClockId).
  static uint64_t GetTraceTimeNs() { return TrackEventInternal::GetTimeNs(); }

  // Returns the type of clock used by GetTraceTimeNs().
  static constexpr protos::pbzero::BuiltinClock GetTraceClockId() {
    return TrackEventInternal::GetClockId();
  }

  const protos::gen::TrackEventConfig& GetConfig() const { return config_; }

 private:
  // Each category has its own enabled/disabled state, stored in the category
  // registry.
  struct CategoryTracePointTraits {
    // Each trace point with a static category has an associated category index.
    struct TracePointData {
      size_t category_index;
    };
    // Called to get the enabled state bitmap of a given category.
    // |data| is the trace point data structure given to
    // DataSource::TraceWithInstances.
    static constexpr std::atomic<uint8_t>* GetActiveInstances(
        TracePointData data) {
      return Registry->GetCategoryState(data.category_index);
    }
  };

  template <typename CategoryType,
            typename EventNameType,
            typename TrackType = Track,
            typename TimestampType = uint64_t,
            typename TimestampTypeCheck = typename std::enable_if<
                IsValidTimestamp<TimestampType>()>::type,
            typename TrackTypeCheck =
                typename std::enable_if<IsValidTrack<TrackType>()>::type,
            typename... Arguments>
  static void TraceForCategoryImpl(
      uint32_t instances,
      const CategoryType& category,
      const EventNameType& event_name,
      perfetto::protos::pbzero::TrackEvent::Type type,
      const TrackType& track,
      const TimestampType& timestamp,
      Arguments&&... args) PERFETTO_ALWAYS_INLINE {
    using CatTraits = CategoryTraits<CategoryType>;
    const Category* static_category =
        CatTraits::GetStaticCategory(Registry, category);
    TraceWithInstances(
        instances, category, [&](typename Base::TraceContext ctx) {
          // If this category is dynamic, first check whether it's enabled.
          if (CatTraits::kIsDynamic &&
              !IsDynamicCategoryEnabled(
                  &ctx, CatTraits::GetDynamicCategory(category))) {
            return;
          }

          const TrackEventTlsState& tls_state = *ctx.GetCustomTlsState();
          TraceTimestamp trace_timestamp = ::perfetto::TraceTimestampTraits<
              TimestampType>::ConvertTimestampToTraceTimeNs(timestamp);

          TraceWriterBase* trace_writer = ctx.tls_inst_->trace_writer.get();
          // Make sure incremental state is valid.
          TrackEventIncrementalState* incr_state = ctx.GetIncrementalState();
          TrackEventInternal::ResetIncrementalStateIfRequired(
              trace_writer, incr_state, tls_state, trace_timestamp);

          // Write the track descriptor before any event on the track.
          if (track) {
            TrackEventInternal::WriteTrackDescriptorIfNeeded(
                track, trace_writer, incr_state, tls_state, trace_timestamp);
          }

          // Write the event itself.
          {
            bool on_current_thread_track =
                (&track == &TrackEventInternal::kDefaultTrack);
            auto event_ctx = TrackEventInternal::WriteEvent(
                trace_writer, incr_state, tls_state, static_category, type,
                trace_timestamp, on_current_thread_track);
            // event name should be emitted with `TRACE_EVENT_BEGIN` macros
            // but not with `TRACE_EVENT_END`.
            if (type != protos::pbzero::TrackEvent::TYPE_SLICE_END) {
              TrackEventInternal::WriteEventName(event_name, event_ctx,
                                                 tls_state);
            }
            // Write dynamic categories (except for events that don't require
            // categories). For counter events, the counter name (and optional
            // category) is stored as part of the track descriptor instead being
            // recorded with individual events.
            if (CatTraits::kIsDynamic &&
                type != protos::pbzero::TrackEvent::TYPE_SLICE_END &&
                type != protos::pbzero::TrackEvent::TYPE_COUNTER) {
              DynamicCategory dynamic_category =
                  CatTraits::GetDynamicCategory(category);
              Category cat = Category::FromDynamicCategory(dynamic_category);
              cat.ForEachGroupMember(
                  [&](const char* member_name, size_t name_size) {
                    event_ctx.event()->add_categories(member_name, name_size);
                    return true;
                  });
            }
            if (type == protos::pbzero::TrackEvent::TYPE_UNSPECIFIED) {
              // Explicitly clear the track, so that the event is not associated
              // with the default track, but instead uses the legacy mechanism
              // based on the phase and pid/tid override.
              event_ctx.event()->set_track_uuid(0);
            } else if (!on_current_thread_track) {
              // We emit these events using TrackDescriptors, and we cannot emit
              // events on behalf of other processes using the TrackDescriptor
              // format. Chrome is the only user of events with explicit process
              // ids and currently only Chrome emits PHASE_MEMORY_DUMP events
              // with an explicit process id, so we should be fine here.
              // TODO(mohitms): Get rid of events with explicit process ids
              // entirely.
              event_ctx.event()->set_track_uuid(track.uuid);
            }
            WriteTrackEventArgs(std::move(event_ctx),
                                std::forward<Arguments>(args)...);
          }  // event_ctx
        });
  }

  template <typename CategoryType, typename Lambda>
  static void TraceWithInstances(uint32_t instances,
                                 const CategoryType& category,
                                 Lambda lambda) PERFETTO_ALWAYS_INLINE {
    using CatTraits = CategoryTraits<CategoryType>;
    if (CatTraits::kIsDynamic) {
      Base::template TraceWithInstances(instances, std::move(lambda));
    } else {
      Base::template TraceWithInstances<CategoryTracePointTraits>(
          instances, std::move(lambda), {CatTraits::GetStaticIndex(category)});
    }
  }

  // Records a track descriptor into the track descriptor registry and, if we
  // are tracing, also mirrors the descriptor into the trace.
  template <typename TrackType>
  static void SetTrackDescriptorImpl(
      const TrackType& track,
      std::function<void(protos::pbzero::TrackDescriptor*)> callback) {
    TrackRegistry::Get()->UpdateTrack(track, std::move(callback));
    Base::template Trace([&](typename Base::TraceContext ctx) {
      TrackEventInternal::WriteTrackDescriptor(
          track, ctx.tls_inst_->trace_writer.get(), ctx.GetIncrementalState(),
          *ctx.GetCustomTlsState(), TrackEventInternal::GetTraceTime());
    });
  }

  // Determines if the given dynamic category is enabled, first by checking the
  // per-trace writer cache or by falling back to computing it based on the
  // trace config for the given session.
  static bool IsDynamicCategoryEnabled(
      typename Base::TraceContext* ctx,
      const DynamicCategory& dynamic_category) {
    auto incr_state = ctx->GetIncrementalState();
    auto it = incr_state->dynamic_categories.find(dynamic_category.name);
    if (it == incr_state->dynamic_categories.end()) {
      // We haven't seen this category before. Let's figure out if it's enabled.
      // This requires grabbing a lock to read the session's trace config.
      auto ds = ctx->GetDataSourceLocked();
      Category category{Category::FromDynamicCategory(dynamic_category)};
      bool enabled = TrackEventInternal::IsCategoryEnabled(
          *Registry, ds->config_, category);
      // TODO(skyostil): Cap the size of |dynamic_categories|.
      incr_state->dynamic_categories[dynamic_category.name] = enabled;
      return enabled;
    }
    return it->second;
  }

  // Config for the current tracing session.
  protos::gen::TrackEventConfig config_;
};

}  // namespace internal
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_INTERNAL_TRACK_EVENT_DATA_SOURCE_H_
