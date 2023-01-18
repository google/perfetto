/*
 * Copyright (C) 2020 The Android Open Source Project
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

#ifndef INCLUDE_PERFETTO_TRACING_TRACK_EVENT_LEGACY_H_
#define INCLUDE_PERFETTO_TRACING_TRACK_EVENT_LEGACY_H_

// This file defines a compatibility shim between legacy (Chrome, V8) trace
// event macros and track events. To avoid accidentally introducing legacy
// events in new code, the PERFETTO_ENABLE_LEGACY_TRACE_EVENTS macro must be set
// to 1 activate the compatibility layer.

#include "perfetto/base/compiler.h"
#include "perfetto/tracing/track_event.h"

#include <stdint.h>

#ifndef PERFETTO_ENABLE_LEGACY_TRACE_EVENTS
#define PERFETTO_ENABLE_LEGACY_TRACE_EVENTS 0
#endif

// Ignore GCC warning about a missing argument for a variadic macro parameter.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC system_header
#endif

// ----------------------------------------------------------------------------
// Constants.
// ----------------------------------------------------------------------------

namespace perfetto {
namespace legacy {

enum TraceEventFlag {
  kTraceEventFlagNone = 0,
  kTraceEventFlagCopy = 1u << 0,
  kTraceEventFlagHasId = 1u << 1,
  kTraceEventFlagScopeOffset = 1u << 2,
  kTraceEventFlagScopeExtra = 1u << 3,
  kTraceEventFlagExplicitTimestamp = 1u << 4,
  kTraceEventFlagAsyncTTS = 1u << 5,
  kTraceEventFlagBindToEnclosing = 1u << 6,
  kTraceEventFlagFlowIn = 1u << 7,
  kTraceEventFlagFlowOut = 1u << 8,
  kTraceEventFlagHasContextId = 1u << 9,
  kTraceEventFlagHasProcessId = 1u << 10,
  kTraceEventFlagHasLocalId = 1u << 11,
  kTraceEventFlagHasGlobalId = 1u << 12,
  // TODO(eseckler): Remove once we have native support for typed proto events
  // in TRACE_EVENT macros.
  kTraceEventFlagTypedProtoArgs = 1u << 15,
  kTraceEventFlagJavaStringLiterals = 1u << 16,
};

enum PerfettoLegacyCurrentThreadId { kCurrentThreadId };

}  // namespace legacy
}  // namespace perfetto

#if PERFETTO_ENABLE_LEGACY_TRACE_EVENTS
// The following constants are defined in the global namespace, since they were
// originally implemented as macros.

// Event phases.
static constexpr char TRACE_EVENT_PHASE_BEGIN = 'B';
static constexpr char TRACE_EVENT_PHASE_END = 'E';
static constexpr char TRACE_EVENT_PHASE_COMPLETE = 'X';
static constexpr char TRACE_EVENT_PHASE_INSTANT = 'I';
static constexpr char TRACE_EVENT_PHASE_ASYNC_BEGIN = 'S';
static constexpr char TRACE_EVENT_PHASE_ASYNC_STEP_INTO = 'T';
static constexpr char TRACE_EVENT_PHASE_ASYNC_STEP_PAST = 'p';
static constexpr char TRACE_EVENT_PHASE_ASYNC_END = 'F';
static constexpr char TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN = 'b';
static constexpr char TRACE_EVENT_PHASE_NESTABLE_ASYNC_END = 'e';
static constexpr char TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT = 'n';
static constexpr char TRACE_EVENT_PHASE_FLOW_BEGIN = 's';
static constexpr char TRACE_EVENT_PHASE_FLOW_STEP = 't';
static constexpr char TRACE_EVENT_PHASE_FLOW_END = 'f';
static constexpr char TRACE_EVENT_PHASE_METADATA = 'M';
static constexpr char TRACE_EVENT_PHASE_COUNTER = 'C';
static constexpr char TRACE_EVENT_PHASE_SAMPLE = 'P';
static constexpr char TRACE_EVENT_PHASE_CREATE_OBJECT = 'N';
static constexpr char TRACE_EVENT_PHASE_SNAPSHOT_OBJECT = 'O';
static constexpr char TRACE_EVENT_PHASE_DELETE_OBJECT = 'D';
static constexpr char TRACE_EVENT_PHASE_MEMORY_DUMP = 'v';
static constexpr char TRACE_EVENT_PHASE_MARK = 'R';
static constexpr char TRACE_EVENT_PHASE_CLOCK_SYNC = 'c';

// Flags for changing the behavior of TRACE_EVENT_API_ADD_TRACE_EVENT.
static constexpr uint32_t TRACE_EVENT_FLAG_NONE =
    perfetto::legacy::kTraceEventFlagNone;
static constexpr uint32_t TRACE_EVENT_FLAG_COPY =
    perfetto::legacy::kTraceEventFlagCopy;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_ID =
    perfetto::legacy::kTraceEventFlagHasId;
static constexpr uint32_t TRACE_EVENT_FLAG_SCOPE_OFFSET =
    perfetto::legacy::kTraceEventFlagScopeOffset;
static constexpr uint32_t TRACE_EVENT_FLAG_SCOPE_EXTRA =
    perfetto::legacy::kTraceEventFlagScopeExtra;
static constexpr uint32_t TRACE_EVENT_FLAG_EXPLICIT_TIMESTAMP =
    perfetto::legacy::kTraceEventFlagExplicitTimestamp;
static constexpr uint32_t TRACE_EVENT_FLAG_ASYNC_TTS =
    perfetto::legacy::kTraceEventFlagAsyncTTS;
static constexpr uint32_t TRACE_EVENT_FLAG_BIND_TO_ENCLOSING =
    perfetto::legacy::kTraceEventFlagBindToEnclosing;
static constexpr uint32_t TRACE_EVENT_FLAG_FLOW_IN =
    perfetto::legacy::kTraceEventFlagFlowIn;
static constexpr uint32_t TRACE_EVENT_FLAG_FLOW_OUT =
    perfetto::legacy::kTraceEventFlagFlowOut;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_CONTEXT_ID =
    perfetto::legacy::kTraceEventFlagHasContextId;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_PROCESS_ID =
    perfetto::legacy::kTraceEventFlagHasProcessId;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_LOCAL_ID =
    perfetto::legacy::kTraceEventFlagHasLocalId;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_GLOBAL_ID =
    perfetto::legacy::kTraceEventFlagHasGlobalId;
static constexpr uint32_t TRACE_EVENT_FLAG_TYPED_PROTO_ARGS =
    perfetto::legacy::kTraceEventFlagTypedProtoArgs;
static constexpr uint32_t TRACE_EVENT_FLAG_JAVA_STRING_LITERALS =
    perfetto::legacy::kTraceEventFlagJavaStringLiterals;

static constexpr uint32_t TRACE_EVENT_FLAG_SCOPE_MASK =
    TRACE_EVENT_FLAG_SCOPE_OFFSET | TRACE_EVENT_FLAG_SCOPE_EXTRA;

// Type values for identifying types in the TraceValue union.
static constexpr uint8_t TRACE_VALUE_TYPE_BOOL = 1;
static constexpr uint8_t TRACE_VALUE_TYPE_UINT = 2;
static constexpr uint8_t TRACE_VALUE_TYPE_INT = 3;
static constexpr uint8_t TRACE_VALUE_TYPE_DOUBLE = 4;
static constexpr uint8_t TRACE_VALUE_TYPE_POINTER = 5;
static constexpr uint8_t TRACE_VALUE_TYPE_STRING = 6;
static constexpr uint8_t TRACE_VALUE_TYPE_COPY_STRING = 7;
static constexpr uint8_t TRACE_VALUE_TYPE_CONVERTABLE = 8;
static constexpr uint8_t TRACE_VALUE_TYPE_PROTO = 9;

// Enum reflecting the scope of an INSTANT event. Must fit within
// TRACE_EVENT_FLAG_SCOPE_MASK.
static constexpr uint8_t TRACE_EVENT_SCOPE_GLOBAL = 0u << 2;
static constexpr uint8_t TRACE_EVENT_SCOPE_PROCESS = 1u << 2;
static constexpr uint8_t TRACE_EVENT_SCOPE_THREAD = 2u << 2;

static constexpr char TRACE_EVENT_SCOPE_NAME_GLOBAL = 'g';
static constexpr char TRACE_EVENT_SCOPE_NAME_PROCESS = 'p';
static constexpr char TRACE_EVENT_SCOPE_NAME_THREAD = 't';

#define TRACE_EVENT_API_CURRENT_THREAD_ID ::perfetto::legacy::kCurrentThreadId

#endif  // PERFETTO_ENABLE_LEGACY_TRACE_EVENTS

// ----------------------------------------------------------------------------
// Internal legacy trace point implementation.
// ----------------------------------------------------------------------------

namespace perfetto {
namespace legacy {

// The following user-provided adaptors are used to serialize user-defined
// thread id and time types into track events. For full compatibility, the user
// should also define the following macros appropriately:
//
//   #define TRACE_TIME_TICKS_NOW() ...
//   #define TRACE_TIME_NOW() ...

// User-provided function to convert an abstract thread id into a thread track.
template <typename T>
ThreadTrack ConvertThreadId(const T&);

// Built-in implementation for events referring to the current thread.
template <>
ThreadTrack PERFETTO_EXPORT_COMPONENT
ConvertThreadId(const PerfettoLegacyCurrentThreadId&);

}  // namespace legacy

namespace internal {

// LegacyTraceId encapsulates an ID that can either be an integer or pointer.
class PERFETTO_EXPORT_COMPONENT LegacyTraceId {
 public:
  // Can be combined with WithScope.
  class LocalId {
   public:
    explicit LocalId(const void* raw_id)
        : raw_id_(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw_id))) {}
    explicit LocalId(uint64_t raw_id) : raw_id_(raw_id) {}
    uint64_t raw_id() const { return raw_id_; }

   private:
    uint64_t raw_id_;
  };

  // Can be combined with WithScope.
  class GlobalId {
   public:
    explicit GlobalId(uint64_t raw_id) : raw_id_(raw_id) {}
    uint64_t raw_id() const { return raw_id_; }

   private:
    uint64_t raw_id_;
  };

  class WithScope {
   public:
    WithScope(const char* scope, uint64_t raw_id)
        : scope_(scope), raw_id_(raw_id) {}
    WithScope(const char* scope, LocalId local_id)
        : scope_(scope), raw_id_(local_id.raw_id()) {
      id_flags_ = legacy::kTraceEventFlagHasLocalId;
    }
    WithScope(const char* scope, GlobalId global_id)
        : scope_(scope), raw_id_(global_id.raw_id()) {
      id_flags_ = legacy::kTraceEventFlagHasGlobalId;
    }
    WithScope(const char* scope, uint64_t prefix, uint64_t raw_id)
        : scope_(scope), has_prefix_(true), prefix_(prefix), raw_id_(raw_id) {}
    WithScope(const char* scope, uint64_t prefix, GlobalId global_id)
        : scope_(scope),
          has_prefix_(true),
          prefix_(prefix),
          raw_id_(global_id.raw_id()) {
      id_flags_ = legacy::kTraceEventFlagHasGlobalId;
    }
    uint64_t raw_id() const { return raw_id_; }
    const char* scope() const { return scope_; }
    bool has_prefix() const { return has_prefix_; }
    uint64_t prefix() const { return prefix_; }
    uint32_t id_flags() const { return id_flags_; }

   private:
    const char* scope_ = nullptr;
    bool has_prefix_ = false;
    uint64_t prefix_;
    uint64_t raw_id_;
    uint32_t id_flags_ = legacy::kTraceEventFlagHasId;
  };

  explicit LegacyTraceId(const void* raw_id)
      : raw_id_(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw_id))) {
    id_flags_ = legacy::kTraceEventFlagHasLocalId;
  }
  explicit LegacyTraceId(uint64_t raw_id) : raw_id_(raw_id) {}
  explicit LegacyTraceId(uint32_t raw_id) : raw_id_(raw_id) {}
  explicit LegacyTraceId(uint16_t raw_id) : raw_id_(raw_id) {}
  explicit LegacyTraceId(uint8_t raw_id) : raw_id_(raw_id) {}
  explicit LegacyTraceId(int64_t raw_id)
      : raw_id_(static_cast<uint64_t>(raw_id)) {}
  explicit LegacyTraceId(int32_t raw_id)
      : raw_id_(static_cast<uint64_t>(raw_id)) {}
  explicit LegacyTraceId(int16_t raw_id)
      : raw_id_(static_cast<uint64_t>(raw_id)) {}
  explicit LegacyTraceId(int8_t raw_id)
      : raw_id_(static_cast<uint64_t>(raw_id)) {}
// Different platforms disagree on which integer types are same and which
// are different. E.g. on Mac size_t is considered a different type from
// uint64_t even though it has the same size and signedness.
// Below we add overloads for those types that are known to cause ambiguity.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  explicit LegacyTraceId(size_t raw_id) : raw_id_(raw_id) {}
  explicit LegacyTraceId(intptr_t raw_id)
      : raw_id_(static_cast<uint64_t>(raw_id)) {}
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  explicit LegacyTraceId(unsigned long raw_id) : raw_id_(raw_id) {}
#endif
  explicit LegacyTraceId(LocalId raw_id) : raw_id_(raw_id.raw_id()) {
    id_flags_ = legacy::kTraceEventFlagHasLocalId;
  }
  explicit LegacyTraceId(GlobalId raw_id) : raw_id_(raw_id.raw_id()) {
    id_flags_ = legacy::kTraceEventFlagHasGlobalId;
  }
  explicit LegacyTraceId(WithScope scoped_id)
      : scope_(scoped_id.scope()),
        has_prefix_(scoped_id.has_prefix()),
        prefix_(scoped_id.prefix()),
        raw_id_(scoped_id.raw_id()),
        id_flags_(scoped_id.id_flags()) {}

  uint64_t raw_id() const { return raw_id_; }
  const char* scope() const { return scope_; }
  bool has_prefix() const { return has_prefix_; }
  uint64_t prefix() const { return prefix_; }
  uint32_t id_flags() const { return id_flags_; }

  void Write(protos::pbzero::TrackEvent::LegacyEvent*,
             uint32_t event_flags) const;

 private:
  const char* scope_ = nullptr;
  bool has_prefix_ = false;
  uint64_t prefix_;
  uint64_t raw_id_;
  uint32_t id_flags_ = legacy::kTraceEventFlagHasId;
};

}  // namespace internal
}  // namespace perfetto

#if PERFETTO_ENABLE_LEGACY_TRACE_EVENTS

namespace perfetto {
namespace internal {

template <typename T>
bool IsEqual(T x, T y) {
  return x == y;
}

template <typename T, typename U>
bool IsEqual(T x, U y) {
  return false;
}

class PERFETTO_EXPORT_COMPONENT TrackEventLegacy {
 public:
  static constexpr protos::pbzero::TrackEvent::Type PhaseToType(char phase) {
    // clang-format off
    return (phase == TRACE_EVENT_PHASE_BEGIN) ?
               protos::pbzero::TrackEvent::TYPE_SLICE_BEGIN :
           (phase == TRACE_EVENT_PHASE_END) ?
               protos::pbzero::TrackEvent::TYPE_SLICE_END :
           (phase == TRACE_EVENT_PHASE_INSTANT) ?
               protos::pbzero::TrackEvent::TYPE_INSTANT :
           protos::pbzero::TrackEvent::TYPE_UNSPECIFIED;
    // clang-format on
  }

  // Reduce binary size overhead by outlining most of the code for writing a
  // legacy trace event.
  template <typename... Args>
  static void WriteLegacyEvent(EventContext ctx,
                               char phase,
                               uint32_t flags,
                               Args&&... args) PERFETTO_NO_INLINE {
    PERFETTO_DCHECK(!(flags & TRACE_EVENT_FLAG_HAS_PROCESS_ID));
    AddDebugAnnotations(&ctx, std::forward<Args>(args)...);
    if (NeedLegacyFlags(phase, flags)) {
      auto legacy_event = ctx.event()->set_legacy_event();
      SetLegacyFlags(legacy_event, phase, flags);
    }
  }

  template <typename ThreadIdType, typename... Args>
  static void WriteLegacyEventWithIdAndTid(EventContext ctx,
                                           char phase,
                                           uint32_t flags,
                                           const LegacyTraceId& id,
                                           const ThreadIdType& thread_id,
                                           Args&&... args) PERFETTO_NO_INLINE {
    //
    // Overrides to consider:
    //
    // 1. If we have an id, we need to write {unscoped,local,global}_id and/or
    //    bind_id.
    // 2. If we have a thread id, we need to write track_uuid() or
    //    {pid,tid}_override if the id represents another process.  The
    //    conversion from |thread_id| happens in embedder code since the type is
    //    embedder-specified.
    // 3. If we have a timestamp, we need to write a different timestamp in the
    //    trace packet itself and make sure TrackEvent won't write one
    //    internally. This is already done at the call site.
    //
    PERFETTO_DCHECK(PhaseToType(phase) ==
                        protos::pbzero::TrackEvent::TYPE_UNSPECIFIED ||
                    !(flags & TRACE_EVENT_FLAG_HAS_PROCESS_ID));
    flags |= id.id_flags();
    AddDebugAnnotations(&ctx, std::forward<Args>(args)...);
    if (NeedLegacyFlags(phase, flags)) {
      auto legacy_event = ctx.event()->set_legacy_event();
      SetLegacyFlags(legacy_event, phase, flags);
      if (id.id_flags())
        id.Write(legacy_event, flags);
      if (flags & TRACE_EVENT_FLAG_HAS_PROCESS_ID) {
        // The thread identifier actually represents a process id. Let's set an
        // override for it.
        int32_t pid_override =
            static_cast<int32_t>(legacy::ConvertThreadId(thread_id).tid);
        legacy_event->set_pid_override(pid_override);
        legacy_event->set_tid_override(-1);
      } else {
        // Only synchronous phases are supported for other threads. These phases
        // are supported in TrackEvent types and receive a track_uuid
        // association via TrackEventDataSource::TraceForCategoryImpl().
        PERFETTO_DCHECK(PhaseToType(phase) !=
                            protos::pbzero::TrackEvent::TYPE_UNSPECIFIED ||
                        IsEqual(thread_id, TRACE_EVENT_API_CURRENT_THREAD_ID) ||
                        legacy::ConvertThreadId(thread_id).tid ==
                            ThreadTrack::Current().tid);
      }
    }
  }

  // No arguments.
  static void AddDebugAnnotations(EventContext*) {}

  // N number of debug arguments.
  template <typename ArgNameType, typename ArgType, typename... OtherArgs>
  static void AddDebugAnnotations(EventContext* ctx,
                                  ArgNameType&& arg_name,
                                  ArgType&& arg_value,
                                  OtherArgs&&... more_args) {
    TrackEventInternal::AddDebugAnnotation(ctx,
                                           std::forward<ArgNameType>(arg_name),
                                           std::forward<ArgType>(arg_value));
    AddDebugAnnotations(ctx, std::forward<OtherArgs>(more_args)...);
  }

 private:
  static bool NeedLegacyFlags(char phase, uint32_t flags) {
    if (PhaseToType(phase) == protos::pbzero::TrackEvent::TYPE_UNSPECIFIED)
      return true;
    // TODO(skyostil): Implement/deprecate:
    // - TRACE_EVENT_FLAG_EXPLICIT_TIMESTAMP
    // - TRACE_EVENT_FLAG_HAS_CONTEXT_ID
    // - TRACE_EVENT_FLAG_TYPED_PROTO_ARGS
    // - TRACE_EVENT_FLAG_JAVA_STRING_LITERALS
    return flags &
           (TRACE_EVENT_FLAG_HAS_ID | TRACE_EVENT_FLAG_HAS_LOCAL_ID |
            TRACE_EVENT_FLAG_HAS_GLOBAL_ID | TRACE_EVENT_FLAG_ASYNC_TTS |
            TRACE_EVENT_FLAG_BIND_TO_ENCLOSING | TRACE_EVENT_FLAG_FLOW_IN |
            TRACE_EVENT_FLAG_FLOW_OUT | TRACE_EVENT_FLAG_HAS_PROCESS_ID);
  }

  static void SetLegacyFlags(
      protos::pbzero::TrackEvent::LegacyEvent* legacy_event,
      char phase,
      uint32_t flags) {
    if (PhaseToType(phase) == protos::pbzero::TrackEvent::TYPE_UNSPECIFIED)
      legacy_event->set_phase(phase);
    if (flags & TRACE_EVENT_FLAG_ASYNC_TTS)
      legacy_event->set_use_async_tts(true);
    if (flags & TRACE_EVENT_FLAG_BIND_TO_ENCLOSING)
      legacy_event->set_bind_to_enclosing(true);

    const auto kFlowIn = TRACE_EVENT_FLAG_FLOW_IN;
    const auto kFlowOut = TRACE_EVENT_FLAG_FLOW_OUT;
    const auto kFlowInOut = kFlowIn | kFlowOut;
    if ((flags & kFlowInOut) == kFlowInOut) {
      legacy_event->set_flow_direction(
          protos::pbzero::TrackEvent::LegacyEvent::FLOW_INOUT);
    } else if (flags & kFlowIn) {
      legacy_event->set_flow_direction(
          protos::pbzero::TrackEvent::LegacyEvent::FLOW_IN);
    } else if (flags & kFlowOut) {
      legacy_event->set_flow_direction(
          protos::pbzero::TrackEvent::LegacyEvent::FLOW_OUT);
    }
  }
};

inline ::perfetto::DynamicString GetEventNameTypeForLegacyEvents(
    ::perfetto::DynamicString name) {
  return name;
}

inline ::perfetto::StaticString GetEventNameTypeForLegacyEvents(
    ::perfetto::StaticString name) {
  return name;
}

// In legacy macro, `const char*` is considered static by default, unless it's
// wrapped in `TRACE_STR_COPY`.
inline ::perfetto::StaticString GetEventNameTypeForLegacyEvents(
    const char* name) {
  return ::perfetto::StaticString{name};
}

}  // namespace internal
}  // namespace perfetto

// Implementations for the INTERNAL_* adapter macros used by the trace points
// below.
#define PERFETTO_INTERNAL_LEGACY_EVENT_ON_TRACK(phase, category, name, track, \
                                                ...)                          \
  PERFETTO_INTERNAL_TRACK_EVENT(                                              \
      category, ::perfetto::internal::GetEventNameTypeForLegacyEvents(name),  \
      ::perfetto::internal::TrackEventLegacy::PhaseToType(phase), track,      \
      ##__VA_ARGS__);

// The main entrypoint for writing unscoped legacy events.  This macro
// determines the right track to write the event on based on |flags| and
// |thread_id|.
#define PERFETTO_INTERNAL_LEGACY_EVENT(phase, category, name, flags,       \
                                       thread_id, ...)                     \
  [&]() {                                                                  \
    using ::perfetto::internal::TrackEventInternal;                        \
    PERFETTO_DCHECK(!(flags & TRACE_EVENT_FLAG_COPY));                     \
    /* First check the scope for instant events. */                        \
    if ((phase) == TRACE_EVENT_PHASE_INSTANT) {                            \
      /* Note: Avoids the need to set LegacyEvent::instant_event_scope. */ \
      auto scope = (flags)&TRACE_EVENT_FLAG_SCOPE_MASK;                    \
      switch (scope) {                                                     \
        case TRACE_EVENT_SCOPE_GLOBAL:                                     \
          PERFETTO_INTERNAL_LEGACY_EVENT_ON_TRACK(                         \
              phase, category, name, ::perfetto::Track::Global(0),         \
              ##__VA_ARGS__);                                              \
          return;                                                          \
        case TRACE_EVENT_SCOPE_PROCESS:                                    \
          PERFETTO_INTERNAL_LEGACY_EVENT_ON_TRACK(                         \
              phase, category, name, ::perfetto::ProcessTrack::Current(),  \
              ##__VA_ARGS__);                                              \
          return;                                                          \
        default:                                                           \
        case TRACE_EVENT_SCOPE_THREAD:                                     \
          /* Fallthrough. */                                               \
          break;                                                           \
      }                                                                    \
    }                                                                      \
    /* If an event targets the current thread or another process, write    \
     * it on the current thread's track. The process override case is      \
     * handled through |pid_override| in WriteLegacyEvent. */              \
    if (std::is_same<                                                      \
            decltype(thread_id),                                           \
            ::perfetto::legacy::PerfettoLegacyCurrentThreadId>::value ||   \
        ((flags)&TRACE_EVENT_FLAG_HAS_PROCESS_ID)) {                       \
      PERFETTO_INTERNAL_LEGACY_EVENT_ON_TRACK(                             \
          phase, category, name, TrackEventInternal::kDefaultTrack,        \
          ##__VA_ARGS__);                                                  \
    } else {                                                               \
      PERFETTO_INTERNAL_LEGACY_EVENT_ON_TRACK(                             \
          phase, category, name,                                           \
          ::perfetto::legacy::ConvertThreadId(thread_id), ##__VA_ARGS__);  \
    }                                                                      \
  }()

#define INTERNAL_TRACE_EVENT_ADD(phase, category, name, flags, ...)        \
  PERFETTO_INTERNAL_LEGACY_EVENT(                                          \
      phase, category, name, flags, ::perfetto::legacy::kCurrentThreadId,  \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS { \
        using ::perfetto::internal::TrackEventLegacy;                      \
        TrackEventLegacy::WriteLegacyEvent(std::move(ctx), phase, flags,   \
                                           ##__VA_ARGS__);                 \
      })

// PERFETTO_INTERNAL_SCOPED_TRACK_EVENT does not require GetStaticString, as it
// uses TRACE_EVENT_BEGIN/END internally, which already have this call.
#define INTERNAL_TRACE_EVENT_ADD_SCOPED(category, name, ...)                 \
  PERFETTO_INTERNAL_SCOPED_TRACK_EVENT(                                      \
      category, ::perfetto::internal::GetEventNameTypeForLegacyEvents(name), \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS {   \
        using ::perfetto::internal::TrackEventLegacy;                        \
        TrackEventLegacy::AddDebugAnnotations(&ctx, ##__VA_ARGS__);          \
      })

// PERFETTO_INTERNAL_SCOPED_TRACK_EVENT does not require GetStaticString, as it
// uses TRACE_EVENT_BEGIN/END internally, which already have this call.
#define INTERNAL_TRACE_EVENT_ADD_SCOPED_WITH_FLOW(category, name, bind_id,   \
                                                  flags, ...)                \
  PERFETTO_INTERNAL_SCOPED_TRACK_EVENT(                                      \
      category, ::perfetto::internal::GetEventNameTypeForLegacyEvents(name), \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS {   \
        using ::perfetto::internal::TrackEventLegacy;                        \
        ::perfetto::internal::LegacyTraceId PERFETTO_UID(trace_id){bind_id}; \
        TrackEventLegacy::WriteLegacyEventWithIdAndTid(                      \
            std::move(ctx), TRACE_EVENT_PHASE_BEGIN, flags,                  \
            PERFETTO_UID(trace_id), TRACE_EVENT_API_CURRENT_THREAD_ID,       \
            ##__VA_ARGS__);                                                  \
      })

#define INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(phase, category, name,     \
                                                timestamp, flags, ...)     \
  PERFETTO_INTERNAL_LEGACY_EVENT(                                          \
      phase, category, name, flags, ::perfetto::legacy::kCurrentThreadId,  \
      timestamp,                                                           \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS { \
        using ::perfetto::internal::TrackEventLegacy;                      \
        TrackEventLegacy::WriteLegacyEvent(std::move(ctx), phase, flags,   \
                                           ##__VA_ARGS__);                 \
      })

#define INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                  \
    phase, category, name, id, thread_id, timestamp, flags, ...)             \
  PERFETTO_INTERNAL_LEGACY_EVENT(                                            \
      phase, category, name, flags, thread_id, timestamp,                    \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS {   \
        using ::perfetto::internal::TrackEventLegacy;                        \
        ::perfetto::internal::LegacyTraceId PERFETTO_UID(trace_id){id};      \
        TrackEventLegacy::WriteLegacyEventWithIdAndTid(                      \
            std::move(ctx), phase, flags, PERFETTO_UID(trace_id), thread_id, \
            ##__VA_ARGS__);                                                  \
      })

#define INTERNAL_TRACE_EVENT_ADD_WITH_ID(phase, category, name, id, flags, \
                                         ...)                              \
  PERFETTO_INTERNAL_LEGACY_EVENT(                                          \
      phase, category, name, flags, ::perfetto::legacy::kCurrentThreadId,  \
      [&](perfetto::EventContext ctx) PERFETTO_NO_THREAD_SAFETY_ANALYSIS { \
        using ::perfetto::internal::TrackEventLegacy;                      \
        ::perfetto::internal::LegacyTraceId PERFETTO_UID(trace_id){id};    \
        TrackEventLegacy::WriteLegacyEventWithIdAndTid(                    \
            std::move(ctx), phase, flags, PERFETTO_UID(trace_id),          \
            TRACE_EVENT_API_CURRENT_THREAD_ID, ##__VA_ARGS__);             \
      })

#define INTERNAL_TRACE_EVENT_METADATA_ADD(category, name, ...)         \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_METADATA, category, name, \
                           TRACE_EVENT_FLAG_NONE)

// ----------------------------------------------------------------------------
// Legacy tracing common API (adapted from trace_event_common.h).
// ----------------------------------------------------------------------------

#define TRACE_DISABLED_BY_DEFAULT(name) "disabled-by-default-" name

// Scoped events.
#define TRACE_EVENT0(category_group, name) \
  INTERNAL_TRACE_EVENT_ADD_SCOPED(category_group, name)
#define TRACE_EVENT_WITH_FLOW0(category_group, name, bind_id, flow_flags)  \
  INTERNAL_TRACE_EVENT_ADD_SCOPED_WITH_FLOW(category_group, name, bind_id, \
                                            flow_flags)
#define TRACE_EVENT1(category_group, name, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_SCOPED(category_group, name, arg1_name, arg1_val)
#define TRACE_EVENT_WITH_FLOW1(category_group, name, bind_id, flow_flags,  \
                               arg1_name, arg1_val)                        \
  INTERNAL_TRACE_EVENT_ADD_SCOPED_WITH_FLOW(category_group, name, bind_id, \
                                            flow_flags, arg1_name, arg1_val)
#define TRACE_EVENT2(category_group, name, arg1_name, arg1_val, arg2_name,   \
                     arg2_val)                                               \
  INTERNAL_TRACE_EVENT_ADD_SCOPED(category_group, name, arg1_name, arg1_val, \
                                  arg2_name, arg2_val)
#define TRACE_EVENT_WITH_FLOW2(category_group, name, bind_id, flow_flags,    \
                               arg1_name, arg1_val, arg2_name, arg2_val)     \
  INTERNAL_TRACE_EVENT_ADD_SCOPED_WITH_FLOW(category_group, name, bind_id,   \
                                            flow_flags, arg1_name, arg1_val, \
                                            arg2_name, arg2_val)

// Instant events.
#define TRACE_EVENT_INSTANT0(category_group, name, scope)                   \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name, \
                           TRACE_EVENT_FLAG_NONE | scope)
#define TRACE_EVENT_INSTANT1(category_group, name, scope, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name,    \
                           TRACE_EVENT_FLAG_NONE | scope, arg1_name, arg1_val)
#define TRACE_EVENT_INSTANT2(category_group, name, scope, arg1_name, arg1_val, \
                             arg2_name, arg2_val)                              \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name,    \
                           TRACE_EVENT_FLAG_NONE | scope, arg1_name, arg1_val, \
                           arg2_name, arg2_val)
#define TRACE_EVENT_COPY_INSTANT0(category_group, name, scope)        \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, \
                           ::perfetto::DynamicString{name}, scope)
#define TRACE_EVENT_COPY_INSTANT1(category_group, name, scope, arg1_name, \
                                  arg1_val)                               \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group,     \
                           ::perfetto::DynamicString{name}, scope,        \
                           ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_INSTANT2(category_group, name, scope, arg1_name,  \
                                  arg1_val, arg2_name, arg2_val)           \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group,      \
                           ::perfetto::DynamicString{name}, scope,         \
                           ::perfetto::DynamicString{arg1_name}, arg1_val, \
                           ::perfetto::DynamicString{arg2_name}, arg2_val)
#define TRACE_EVENT_INSTANT_WITH_FLAGS0(category_group, name, scope_and_flags) \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name,    \
                           scope_and_flags)
#define TRACE_EVENT_INSTANT_WITH_FLAGS1(category_group, name, scope_and_flags, \
                                        arg1_name, arg1_val)                   \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name,    \
                           scope_and_flags, arg1_name, arg1_val)

// Instant events with explicit timestamps.
#define TRACE_EVENT_INSTANT_WITH_TIMESTAMP0(category_group, name, scope,   \
                                            timestamp)                     \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(TRACE_EVENT_PHASE_INSTANT,       \
                                          category_group, name, timestamp, \
                                          TRACE_EVENT_FLAG_NONE | scope)

#define TRACE_EVENT_INSTANT_WITH_TIMESTAMP1(category_group, name, scope,  \
                                            timestamp, arg_name, arg_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                \
      TRACE_EVENT_PHASE_INSTANT, category_group, name, timestamp,         \
      TRACE_EVENT_FLAG_NONE | scope, arg_name, arg_val)

// Begin events.
#define TRACE_EVENT_BEGIN0(category_group, name)                          \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name, \
                           TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_BEGIN1(category_group, name, arg1_name, arg1_val)     \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_BEGIN2(category_group, name, arg1_name, arg1_val,     \
                           arg2_name, arg2_val)                           \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val,    \
                           arg2_name, arg2_val)
#define TRACE_EVENT_BEGIN_WITH_FLAGS0(category_group, name, flags) \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name, flags)
#define TRACE_EVENT_BEGIN_WITH_FLAGS1(category_group, name, flags, arg1_name, \
                                      arg1_val)                               \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name,     \
                           flags, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_BEGIN2(category_group, name, arg1_name, arg1_val, \
                                arg2_name, arg2_val)                       \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group,        \
                           ::perfetto::DynamicString{name},                \
                           TRACE_EVENT_FLAG_NONE,                          \
                           ::perfetto::DynamicString{arg1_name}, arg1_val, \
                           ::perfetto::DynamicString{arg2_name}, arg2_val)

// Begin events with explicit timestamps.
#define TRACE_EVENT_BEGIN_WITH_ID_TID_AND_TIMESTAMP0(category_group, name, id, \
                                                     thread_id, timestamp)     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                          \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, thread_id,      \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP0(       \
    category_group, name, id, thread_id, timestamp)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(            \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,             \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp, \
      TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP1(               \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                    \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,                     \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp,         \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP2(                   \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val,     \
    arg2_name, arg2_val)                                                     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,                         \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp,             \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val, \
      ::perfetto::DynamicString{arg2_name}, arg2_val)

// End events.
#define TRACE_EVENT_END0(category_group, name)                          \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name, \
                           TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_END1(category_group, name, arg1_name, arg1_val)     \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_END2(category_group, name, arg1_name, arg1_val, arg2_name, \
                         arg2_val)                                             \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name,        \
                           TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val,         \
                           arg2_name, arg2_val)
#define TRACE_EVENT_END_WITH_FLAGS0(category_group, name, flags) \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name, flags)
#define TRACE_EVENT_END_WITH_FLAGS1(category_group, name, flags, arg1_name,    \
                                    arg1_val)                                  \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name, flags, \
                           arg1_name, arg1_val)
#define TRACE_EVENT_COPY_END2(category_group, name, arg1_name, arg1_val,      \
                              arg2_name, arg2_val)                            \
  INTERNAL_TRACE_EVENT_ADD(                                                   \
      TRACE_EVENT_PHASE_END, category_group, ::perfetto::DynamicString{name}, \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val,  \
      ::perfetto::DynamicString{arg2_name}, arg2_val)

// Mark events.
#define TRACE_EVENT_MARK_WITH_TIMESTAMP0(category_group, name, timestamp)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(TRACE_EVENT_PHASE_MARK,          \
                                          category_group, name, timestamp, \
                                          TRACE_EVENT_FLAG_NONE)

#define TRACE_EVENT_MARK_WITH_TIMESTAMP1(category_group, name, timestamp, \
                                         arg1_name, arg1_val)             \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                \
      TRACE_EVENT_PHASE_MARK, category_group, name, timestamp,            \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)

#define TRACE_EVENT_MARK_WITH_TIMESTAMP2(                                      \
    category_group, name, timestamp, arg1_name, arg1_val, arg2_name, arg2_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                     \
      TRACE_EVENT_PHASE_MARK, category_group, name, timestamp,                 \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)

#define TRACE_EVENT_COPY_MARK(category_group, name)                \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_MARK, category_group, \
                           ::perfetto::DynamicString{name},        \
                           TRACE_EVENT_FLAG_NONE)

#define TRACE_EVENT_COPY_MARK1(category_group, name, arg1_name, arg1_val)      \
  INTERNAL_TRACE_EVENT_ADD(                                                    \
      TRACE_EVENT_PHASE_MARK, category_group, ::perfetto::DynamicString{name}, \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val)

#define TRACE_EVENT_COPY_MARK_WITH_TIMESTAMP(category_group, name, timestamp)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                     \
      TRACE_EVENT_PHASE_MARK, category_group, ::perfetto::DynamicString{name}, \
      timestamp, TRACE_EVENT_FLAG_NONE)

// End events with explicit thread and timestamp.
#define TRACE_EVENT_END_WITH_ID_TID_AND_TIMESTAMP0(category_group, name, id, \
                                                   thread_id, timestamp)     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id, thread_id,      \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP0(         \
    category_group, name, id, thread_id, timestamp)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(            \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,               \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp, \
      TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP1(                 \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                    \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,                       \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp,         \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP2(                     \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val,     \
    arg2_name, arg2_val)                                                     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,                           \
      ::perfetto::DynamicString{name}, id, thread_id, timestamp,             \
      TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, arg1_val, \
      ::perfetto::DynamicString{arg2_name}, arg2_val)

// Counters.
#define TRACE_COUNTER1(category_group, name, value)                         \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, "value",                  \
                           static_cast<int>(value))
#define TRACE_COUNTER_WITH_FLAG1(category_group, name, flag, value)         \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           flag, "value", static_cast<int>(value))
#define TRACE_COPY_COUNTER1(category_group, name, value)              \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, \
                           ::perfetto::DynamicString{name},           \
                           TRACE_EVENT_FLAG_NONE, "value",            \
                           static_cast<int>(value))
#define TRACE_COUNTER2(category_group, name, value1_name, value1_val,       \
                       value2_name, value2_val)                             \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, value1_name,              \
                           static_cast<int>(value1_val), value2_name,       \
                           static_cast<int>(value2_val))
#define TRACE_COPY_COUNTER2(category_group, name, value1_name, value1_val, \
                            value2_name, value2_val)                       \
  INTERNAL_TRACE_EVENT_ADD(                                                \
      TRACE_EVENT_PHASE_COUNTER, category_group,                           \
      ::perfetto::DynamicString{name}, TRACE_EVENT_FLAG_NONE, value1_name, \
      static_cast<int>(value1_val), value2_name, static_cast<int>(value2_val))

// Counters with explicit timestamps.
#define TRACE_COUNTER_WITH_TIMESTAMP1(category_group, name, timestamp, value) \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                    \
      TRACE_EVENT_PHASE_COUNTER, category_group, name, timestamp,             \
      TRACE_EVENT_FLAG_NONE, "value", static_cast<int>(value))

#define TRACE_COUNTER_WITH_TIMESTAMP2(category_group, name, timestamp,      \
                                      value1_name, value1_val, value2_name, \
                                      value2_val)                           \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                  \
      TRACE_EVENT_PHASE_COUNTER, category_group, name, timestamp,           \
      TRACE_EVENT_FLAG_NONE, value1_name, static_cast<int>(value1_val),     \
      value2_name, static_cast<int>(value2_val))

// Counters with ids.
#define TRACE_COUNTER_ID1(category_group, name, id, value)                    \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_COUNTER, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE, "value",  \
                                   static_cast<int>(value))
#define TRACE_COPY_COUNTER_ID1(category_group, name, id, value)               \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_COUNTER, category_group, \
                                   ::perfetto::DynamicString{name}, id,       \
                                   TRACE_EVENT_FLAG_NONE, "value",            \
                                   static_cast<int>(value))
#define TRACE_COUNTER_ID2(category_group, name, id, value1_name, value1_val,  \
                          value2_name, value2_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_COUNTER, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE,           \
                                   value1_name, static_cast<int>(value1_val), \
                                   value2_name, static_cast<int>(value2_val))
#define TRACE_COPY_COUNTER_ID2(category_group, name, id, value1_name,          \
                               value1_val, value2_name, value2_val)            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_COUNTER, category_group,                               \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE, value1_name, \
      static_cast<int>(value1_val), value2_name, static_cast<int>(value2_val))

// Sampling profiler events.
#define TRACE_EVENT_SAMPLE_WITH_ID1(category_group, name, id, arg1_name,       \
                                    arg1_val)                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_SAMPLE, category_group,   \
                                   name, id, TRACE_EVENT_FLAG_NONE, arg1_name, \
                                   arg1_val)

// Legacy async events.
#define TRACE_EVENT_ASYNC_BEGIN0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_BEGIN, \
                                   category_group, name, id,      \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_BEGIN1(category_group, name, id, arg1_name, \
                                 arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_BEGIN,     \
                                   category_group, name, id,          \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_ASYNC_BEGIN2(category_group, name, id, arg1_name, \
                                 arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                   \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,        \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_ASYNC_BEGIN0(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                             \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_ASYNC_BEGIN1(category_group, name, id, arg1_name, \
                                      arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                        \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,                       \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,          \
      ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_ASYNC_BEGIN2(category_group, name, id, arg1_name, \
                                      arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                        \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,                       \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,          \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                      \
      ::perfetto::DynamicString{arg2_name}, arg2_val)
#define TRACE_EVENT_ASYNC_BEGIN_WITH_FLAGS0(category_group, name, id, flags) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_BEGIN,            \
                                   category_group, name, id, flags)

// Legacy async events with explicit timestamps.
#define TRACE_EVENT_ASYNC_BEGIN_WITH_TIMESTAMP0(category_group, name, id, \
                                                timestamp)                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,            \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_BEGIN_WITH_TIMESTAMP1(                           \
    category_group, name, id, timestamp, arg1_name, arg1_val)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,             \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      arg1_name, arg1_val)
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP_AND_FLAGS0(     \
    category_group, name, id, timestamp, flags)                         \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                   \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, flags)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(category_group, name, \
                                                       id, timestamp)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,        \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_BEGIN_WITH_TIMESTAMP2(category_group, name, id,      \
                                                timestamp, arg1_name,          \
                                                arg1_val, arg2_name, arg2_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                          \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,                 \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE,     \
      arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_ASYNC_BEGIN_WITH_TIMESTAMP0(category_group, name, id, \
                                                     timestamp)                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                          \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group,                           \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_API_CURRENT_THREAD_ID,  \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_BEGIN_WITH_TIMESTAMP_AND_FLAGS0(     \
    category_group, name, id, timestamp, flags)                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(          \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, flags)

// Legacy async step into events.
#define TRACE_EVENT_ASYNC_STEP_INTO0(category_group, name, id, step)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_STEP_INTO, \
                                   category_group, name, id,          \
                                   TRACE_EVENT_FLAG_NONE, "step", step)
#define TRACE_EVENT_ASYNC_STEP_INTO1(category_group, name, id, step, \
                                     arg1_name, arg1_val)            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                  \
      TRACE_EVENT_PHASE_ASYNC_STEP_INTO, category_group, name, id,   \
      TRACE_EVENT_FLAG_NONE, "step", step, arg1_name, arg1_val)

// Legacy async step into events with timestamps.
#define TRACE_EVENT_ASYNC_STEP_INTO_WITH_TIMESTAMP0(category_group, name, id, \
                                                    step, timestamp)          \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_ASYNC_STEP_INTO, category_group, name, id,            \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE,    \
      "step", step)

// Legacy async step past events.
#define TRACE_EVENT_ASYNC_STEP_PAST0(category_group, name, id, step)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_STEP_PAST, \
                                   category_group, name, id,          \
                                   TRACE_EVENT_FLAG_NONE, "step", step)
#define TRACE_EVENT_ASYNC_STEP_PAST1(category_group, name, id, step, \
                                     arg1_name, arg1_val)            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                  \
      TRACE_EVENT_PHASE_ASYNC_STEP_PAST, category_group, name, id,   \
      TRACE_EVENT_FLAG_NONE, "step", step, arg1_name, arg1_val)

// Legacy async end events.
#define TRACE_EVENT_ASYNC_END0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_END, \
                                   category_group, name, id,    \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_END1(category_group, name, id, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_END,               \
                                   category_group, name, id,                  \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_ASYNC_END2(category_group, name, id, arg1_name, arg1_val, \
                               arg2_name, arg2_val)                           \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                           \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,                  \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_ASYNC_END0(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                           \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_ASYNC_END1(category_group, name, id, arg1_name, \
                                    arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                      \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,                       \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,        \
      ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_ASYNC_END2(category_group, name, id, arg1_name, \
                                    arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                      \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,                       \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,        \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                    \
      ::perfetto::DynamicString{arg2_name}, arg2_val)
#define TRACE_EVENT_ASYNC_END_WITH_FLAGS0(category_group, name, id, flags) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_END,            \
                                   category_group, name, id, flags)

// Legacy async end events with explicit timestamps.
#define TRACE_EVENT_ASYNC_END_WITH_TIMESTAMP0(category_group, name, id, \
                                              timestamp)                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                   \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,            \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_END_WITH_TIMESTAMP1(category_group, name, id,       \
                                              timestamp, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,                  \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE,    \
      arg1_name, arg1_val)
#define TRACE_EVENT_ASYNC_END_WITH_TIMESTAMP2(category_group, name, id,       \
                                              timestamp, arg1_name, arg1_val, \
                                              arg2_name, arg2_val)            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,                  \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE,    \
      arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_ASYNC_END_WITH_TIMESTAMP0(category_group, name, id,  \
                                                   timestamp)                 \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_ASYNC_END, category_group,                            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_API_CURRENT_THREAD_ID, \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_ASYNC_END_WITH_TIMESTAMP_AND_FLAGS0(category_group, name, \
                                                        id, timestamp, flags) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,                  \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, flags)

// Async events.
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, \
                                   category_group, name, id,               \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN1(category_group, name, id, arg1_name, \
                                          arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN,     \
                                   category_group, name, id,                   \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN2(category_group, name, id, arg1_name, \
                                          arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id,        \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_FLAGS0(category_group, name, id, \
                                                     flags)                    \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN,     \
                                   category_group, name, id, flags)
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP1(                  \
    category_group, name, id, timestamp, arg1_name, arg1_val)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id,    \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      arg1_name, arg1_val)

// Async end events.
#define TRACE_EVENT_NESTABLE_ASYNC_END0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, \
                                   category_group, name, id,             \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_NESTABLE_ASYNC_END1(category_group, name, id, arg1_name, \
                                        arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_END,     \
                                   category_group, name, id,                 \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_NESTABLE_ASYNC_END2(category_group, name, id, arg1_name, \
                                        arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                          \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,        \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_FLAGS0(category_group, name, id, \
                                                   flags)                    \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_END,     \
                                   category_group, name, id, flags)

// Async instant events.
#define TRACE_EVENT_NESTABLE_ASYNC_INSTANT0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT, \
                                   category_group, name, id,                 \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_NESTABLE_ASYNC_INSTANT1(category_group, name, id,        \
                                            arg1_name, arg1_val)             \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT, \
                                   category_group, name, id,                 \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_NESTABLE_ASYNC_INSTANT2(                              \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val)   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                       \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT, category_group, name, id, \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TTS2(                \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                     \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,           \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_ASYNC_TTS,  \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                   \
      ::perfetto::DynamicString{arg2_name}, arg2_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TTS2(                  \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                     \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group,             \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_ASYNC_TTS,  \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                   \
      ::perfetto::DynamicString{arg2_name}, arg2_val)

// Async events with explicit timestamps.
#define TRACE_EVENT_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(category_group, name, \
                                                         id, timestamp)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                          \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id,        \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(category_group, name, \
                                                       id, timestamp)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,        \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP1(                    \
    category_group, name, id, timestamp, arg1_name, arg1_val)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,      \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      arg1_name, arg1_val)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP2(                    \
    category_group, name, id, timestamp, arg1_name, arg1_val, arg2_name,   \
    arg2_val)                                                              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,      \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_NESTABLE_ASYNC_END_WITH_TIMESTAMP_AND_FLAGS0(     \
    category_group, name, id, timestamp, flags)                       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                 \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, flags)
#define TRACE_EVENT_NESTABLE_ASYNC_INSTANT_WITH_TIMESTAMP0(               \
    category_group, name, id, timestamp)                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN0(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN1(category_group, name, id, \
                                               arg1_name, arg1_val)      \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,        \
      ::perfetto::DynamicString{arg1_name}, arg1_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN2(                         \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                     \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,           \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE,       \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                   \
      ::perfetto::DynamicString{arg2_name}, arg2_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END0(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                    \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group,            \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(                \
    category_group, name, id, timestamp)                                      \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,                 \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_API_CURRENT_THREAD_ID, \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP1(                \
    category_group, name, id, timestamp, arg1_name, arg1_val)                 \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group,                 \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_API_CURRENT_THREAD_ID, \
      timestamp, TRACE_EVENT_FLAG_NONE, ::perfetto::DynamicString{arg1_name}, \
      arg1_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(                  \
    category_group, name, id, timestamp)                                      \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                         \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group,                   \
      ::perfetto::DynamicString{name}, id, TRACE_EVENT_API_CURRENT_THREAD_ID, \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP2(               \
    category_group, name, id, timestamp, arg1_name, arg1_val, arg2_name,   \
    arg2_val)                                                              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,      \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      ::perfetto::DynamicString{arg1_name}, arg1_val,                      \
      ::perfetto::DynamicString{arg2_name}, arg2_val)

// Metadata events.
#define TRACE_EVENT_METADATA1(category_group, name, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_METADATA_ADD(category_group, name, arg1_name, arg1_val)

// Clock sync events.
#define TRACE_EVENT_CLOCK_SYNC_RECEIVER(sync_id)                           \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_CLOCK_SYNC, "__metadata",     \
                           "clock_sync", TRACE_EVENT_FLAG_NONE, "sync_id", \
                           sync_id)
#define TRACE_EVENT_CLOCK_SYNC_ISSUER(sync_id, issue_ts, issue_end_ts)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(                                    \
      TRACE_EVENT_PHASE_CLOCK_SYNC, "__metadata", "clock_sync", issue_end_ts, \
      TRACE_EVENT_FLAG_NONE, "sync_id", sync_id, "issue_ts", issue_ts)

// Object events.
#define TRACE_EVENT_OBJECT_CREATED_WITH_ID(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_CREATE_OBJECT,  \
                                   category_group, name, id,         \
                                   TRACE_EVENT_FLAG_NONE)

#define TRACE_EVENT_OBJECT_SNAPSHOT_WITH_ID(category_group, name, id, \
                                            snapshot)                 \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                   \
      TRACE_EVENT_PHASE_SNAPSHOT_OBJECT, category_group, name, id,    \
      TRACE_EVENT_FLAG_NONE, "snapshot", snapshot)

#define TRACE_EVENT_OBJECT_SNAPSHOT_WITH_ID_AND_TIMESTAMP(                 \
    category_group, name, id, timestamp, snapshot)                         \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                      \
      TRACE_EVENT_PHASE_SNAPSHOT_OBJECT, category_group, name, id,         \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE, \
      "snapshot", snapshot)

#define TRACE_EVENT_OBJECT_DELETED_WITH_ID(category_group, name, id) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_DELETE_OBJECT,  \
                                   category_group, name, id,         \
                                   TRACE_EVENT_FLAG_NONE)

// TODO(skyostil): Implement binary-efficient trace events.
#define TRACE_EVENT_BINARY_EFFICIENT0 TRACE_EVENT0
#define TRACE_EVENT_BINARY_EFFICIENT1 TRACE_EVENT1
#define TRACE_EVENT_BINARY_EFFICIENT2 TRACE_EVENT2

// Macro to efficiently determine if a given category group is enabled.
#define TRACE_EVENT_CATEGORY_GROUP_ENABLED(category, ret) \
  do {                                                    \
    *ret = TRACE_EVENT_CATEGORY_ENABLED(category);        \
  } while (0)

// Macro to efficiently determine, through polling, if a new trace has begun.
#define TRACE_EVENT_IS_NEW_TRACE(ret)                                \
  do {                                                               \
    static int PERFETTO_UID(prev) = -1;                              \
    int PERFETTO_UID(curr) =                                         \
        ::perfetto::internal::TrackEventInternal::GetSessionCount(); \
    if (PERFETTO_TRACK_EVENT_NAMESPACE::TrackEvent::IsEnabled() &&   \
        (PERFETTO_UID(prev) != PERFETTO_UID(curr))) {                \
      *(ret) = true;                                                 \
      PERFETTO_UID(prev) = PERFETTO_UID(curr);                       \
    } else {                                                         \
      *(ret) = false;                                                \
    }                                                                \
  } while (0)

// ----------------------------------------------------------------------------
// Legacy tracing API (adapted from trace_event.h).
// ----------------------------------------------------------------------------

// We can implement the following subset of the legacy tracing API without
// involvement from the embedder. APIs such as TRACE_EVENT_API_ADD_TRACE_EVENT
// are still up to the embedder to define.

#define TRACE_STR_COPY(str) (::perfetto::DynamicString{str})

#define TRACE_ID_WITH_SCOPE(scope, ...) \
  ::perfetto::internal::LegacyTraceId::WithScope(scope, ##__VA_ARGS__)

// Use this for ids that are unique across processes. This allows different
// processes to use the same id to refer to the same event.
#define TRACE_ID_GLOBAL(id) ::perfetto::internal::LegacyTraceId::GlobalId(id)

// Use this for ids that are unique within a single process. This allows
// different processes to use the same id to refer to different events.
#define TRACE_ID_LOCAL(id) ::perfetto::internal::LegacyTraceId::LocalId(id)

// Returns a pointer to a uint8_t which indicates whether tracing is enabled for
// the given category or not. A zero value means tracing is disabled and
// non-zero indicates at least one tracing session for this category is active.
// Note that callers should not make any assumptions at what each bit represents
// in the status byte. Does not support dynamic categories.
#define TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED(category)              \
  reinterpret_cast<const uint8_t*>(                                       \
      [&] {                                                               \
        static_assert(                                                    \
            !std::is_same<::perfetto::DynamicCategory,                    \
                          decltype(category)>::value,                     \
            "Enabled flag pointers are not supported for dynamic trace "  \
            "categories.");                                               \
      },                                                                  \
      PERFETTO_TRACK_EVENT_NAMESPACE::internal::kCategoryRegistry         \
          .GetCategoryState(                                              \
              PERFETTO_TRACK_EVENT_NAMESPACE::internal::kCategoryRegistry \
                  .Find(category, /*is_dynamic=*/false)))

// Given a pointer returned by TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED,
// yields a pointer to the name of the corresponding category group.
#define TRACE_EVENT_API_GET_CATEGORY_GROUP_NAME(category_enabled_ptr)     \
  PERFETTO_TRACK_EVENT_NAMESPACE::internal::kCategoryRegistry             \
      .GetCategory(                                                       \
          category_enabled_ptr -                                          \
          reinterpret_cast<const uint8_t*>(                               \
              PERFETTO_TRACK_EVENT_NAMESPACE::internal::kCategoryRegistry \
                  .GetCategoryState(0u)))                                 \
      ->name

#endif  // PERFETTO_ENABLE_LEGACY_TRACE_EVENTS

#endif  // INCLUDE_PERFETTO_TRACING_TRACK_EVENT_LEGACY_H_
