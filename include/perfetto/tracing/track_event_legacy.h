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

#if PERFETTO_ENABLE_LEGACY_TRACE_EVENTS

// Ignore GCC warning about a missing argument for a variadic macro parameter.
#pragma GCC system_header

// ----------------------------------------------------------------------------
// Constants.
// ----------------------------------------------------------------------------

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
static constexpr char TRACE_EVENT_PHASE_ENTER_CONTEXT = '(';
static constexpr char TRACE_EVENT_PHASE_LEAVE_CONTEXT = ')';

// Flags for changing the behavior of TRACE_EVENT_API_ADD_TRACE_EVENT.
static constexpr uint32_t TRACE_EVENT_FLAG_NONE = 0;
static constexpr uint32_t TRACE_EVENT_FLAG_COPY = 1u << 0;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_ID = 1u << 1;
// TODO(crbug.com/639003): Free this bit after ID mangling is deprecated.
static constexpr uint32_t TRACE_EVENT_FLAG_MANGLE_ID = 1u << 2;
static constexpr uint32_t TRACE_EVENT_FLAG_SCOPE_OFFSET = 1u << 3;
static constexpr uint32_t TRACE_EVENT_FLAG_SCOPE_EXTRA = 1u << 4;
static constexpr uint32_t TRACE_EVENT_FLAG_EXPLICIT_TIMESTAMP = 1u << 5;
static constexpr uint32_t TRACE_EVENT_FLAG_ASYNC_TTS = 1u << 6;
static constexpr uint32_t TRACE_EVENT_FLAG_BIND_TO_ENCLOSING = 1u << 7;
static constexpr uint32_t TRACE_EVENT_FLAG_FLOW_IN = 1u << 8;
static constexpr uint32_t TRACE_EVENT_FLAG_FLOW_OUT = 1u << 9;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_CONTEXT_ID = 1u << 10;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_PROCESS_ID = 1u << 11;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_LOCAL_ID = 1u << 12;
static constexpr uint32_t TRACE_EVENT_FLAG_HAS_GLOBAL_ID = 1u << 13;
// TODO(eseckler): Remove once we have native support for typed proto events in
// TRACE_EVENT macros.
static constexpr uint32_t TRACE_EVENT_FLAG_TYPED_PROTO_ARGS = 1u << 15;
static constexpr uint32_t TRACE_EVENT_FLAG_JAVA_STRING_LITERALS = 1u << 16;

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

// Enum reflecting the scope of an INSTANT event. Must fit within
// TRACE_EVENT_FLAG_SCOPE_MASK.
static constexpr uint8_t TRACE_EVENT_SCOPE_GLOBAL = 0u << 3;
static constexpr uint8_t TRACE_EVENT_SCOPE_PROCESS = 1u << 3;
static constexpr uint8_t TRACE_EVENT_SCOPE_THREAD = 2u << 3;

static constexpr char TRACE_EVENT_SCOPE_NAME_GLOBAL = 'g';
static constexpr char TRACE_EVENT_SCOPE_NAME_PROCESS = 'p';
static constexpr char TRACE_EVENT_SCOPE_NAME_THREAD = 't';

// ----------------------------------------------------------------------------
// Internal legacy trace point implementation.
// ----------------------------------------------------------------------------

namespace perfetto {
namespace internal {

class TrackEventLegacy {
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
    AddDebugAnnotations(&ctx, std::forward<Args>(args)...);
    SetTrackIfNeeded(&ctx, flags);
    if (PhaseToType(phase) == protos::pbzero::TrackEvent::TYPE_UNSPECIFIED) {
      auto legacy_event = ctx.event()->set_legacy_event();
      legacy_event->set_phase(phase);
    }
  }

  // No arguments.
  static void AddDebugAnnotations(EventContext*) {}

  // One argument.
  template <typename ArgType>
  static void AddDebugAnnotations(EventContext* ctx,
                                  const char* arg_name,
                                  ArgType&& arg_value) {
    TrackEventInternal::AddDebugAnnotation(ctx, arg_name, arg_value);
  }

  // Two arguments.
  template <typename ArgType, typename ArgType2>
  static void AddDebugAnnotations(EventContext* ctx,
                                  const char* arg_name,
                                  ArgType&& arg_value,
                                  const char* arg_name2,
                                  ArgType2&& arg_value2) {
    TrackEventInternal::AddDebugAnnotation(ctx, arg_name, arg_value);
    TrackEventInternal::AddDebugAnnotation(ctx, arg_name2, arg_value2);
  }

 private:
  static void SetTrackIfNeeded(EventContext* ctx, uint32_t flags) {
    auto scope = flags & TRACE_EVENT_FLAG_SCOPE_MASK;
    switch (scope) {
      case TRACE_EVENT_SCOPE_GLOBAL:
        ctx->event()->set_track_uuid(0);
        break;
      case TRACE_EVENT_SCOPE_PROCESS:
        ctx->event()->set_track_uuid(ProcessTrack::Current().uuid);
        break;
      default:
      case TRACE_EVENT_SCOPE_THREAD:
        // Thread scope is already the default.
        break;
    }
  }
};

}  // namespace internal
}  // namespace perfetto

// A black hole trace point where unsupported trace events are routed.
#define PERFETTO_INTERNAL_EVENT_NOOP(cat, name, ...) \
  do {                                               \
    if (false) {                                     \
      ::perfetto::base::ignore_result(cat);          \
      ::perfetto::base::ignore_result(name);         \
    }                                                \
  } while (false)

// Implementations for the INTERNAL_* adapter macros used by the trace points
// below.
#define INTERNAL_TRACE_EVENT_ADD(phase, category, name, flags, ...)      \
  PERFETTO_INTERNAL_TRACK_EVENT(                                         \
      category, name,                                                    \
      ::perfetto::internal::TrackEventLegacy::PhaseToType(phase),        \
      [&](perfetto::EventContext ctx) {                                  \
        using ::perfetto::internal::TrackEventLegacy;                    \
        TrackEventLegacy::WriteLegacyEvent(std::move(ctx), phase, flags, \
                                           ##__VA_ARGS__);               \
      })

#define INTERNAL_TRACE_EVENT_ADD_SCOPED(category, name, ...)        \
  PERFETTO_INTERNAL_SCOPED_TRACK_EVENT(                             \
      category, name, [&](perfetto::EventContext ctx) {             \
        using ::perfetto::internal::TrackEventLegacy;               \
        TrackEventLegacy::AddDebugAnnotations(&ctx, ##__VA_ARGS__); \
      })

#define INTERNAL_TRACE_EVENT_ADD_SCOPED_WITH_FLOW(...) \
  PERFETTO_INTERNAL_EVENT_NOOP(__VA_ARGS__)
#define INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(...) \
  PERFETTO_INTERNAL_EVENT_NOOP(__VA_ARGS__)
#define INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(...) \
  PERFETTO_INTERNAL_EVENT_NOOP(__VA_ARGS__)
#define INTERNAL_TRACE_EVENT_ADD_WITH_ID(...) \
  PERFETTO_INTERNAL_EVENT_NOOP(__VA_ARGS__)
#define INTERNAL_TRACE_EVENT_METADATA_ADD(...) \
  PERFETTO_INTERNAL_EVENT_NOOP(__VA_ARGS__)

#define INTERNAL_TRACE_TIME_TICKS_NOW() 0
#define INTERNAL_TRACE_TIME_NOW() 0

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
#define TRACE_EVENT_COPY_INSTANT0(category_group, name, scope)              \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name, \
                           TRACE_EVENT_FLAG_COPY | scope)
#define TRACE_EVENT_COPY_INSTANT1(category_group, name, scope, arg1_name,   \
                                  arg1_val)                                 \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name, \
                           TRACE_EVENT_FLAG_COPY | scope, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_INSTANT2(category_group, name, scope, arg1_name,      \
                                  arg1_val, arg2_name, arg2_val)               \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_INSTANT, category_group, name,    \
                           TRACE_EVENT_FLAG_COPY | scope, arg1_name, arg1_val, \
                           arg2_name, arg2_val)
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
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_BEGIN, category_group, name,  \
                           TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val,     \
                           arg2_name, arg2_val)

// Begin events with explicit timestamps.
#define TRACE_EVENT_BEGIN_WITH_ID_TID_AND_TIMESTAMP0(category_group, name, id, \
                                                     thread_id, timestamp)     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                          \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, thread_id,      \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP0(                \
    category_group, name, id, thread_id, timestamp)                       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, thread_id, \
      timestamp, TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP1(                \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, thread_id, \
      timestamp, TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_BEGIN_WITH_ID_TID_AND_TIMESTAMP2(                \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val,  \
    arg2_name, arg2_val)                                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id, thread_id, \
      timestamp, TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, arg2_name,   \
      arg2_val)

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
#define TRACE_EVENT_COPY_END2(category_group, name, arg1_name, arg1_val, \
                              arg2_name, arg2_val)                       \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_END, category_group, name,  \
                           TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val,   \
                           arg2_name, arg2_val)

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

#define TRACE_EVENT_COPY_MARK(category_group, name)                      \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_MARK, category_group, name, \
                           TRACE_EVENT_FLAG_COPY)

#define TRACE_EVENT_COPY_MARK1(category_group, name, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_MARK, category_group, name,  \
                           TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)

#define TRACE_EVENT_COPY_MARK_WITH_TIMESTAMP(category_group, name, timestamp) \
  INTERNAL_TRACE_EVENT_ADD_WITH_TIMESTAMP(TRACE_EVENT_PHASE_MARK,             \
                                          category_group, name, timestamp,    \
                                          TRACE_EVENT_FLAG_COPY)

// End events with explicit thread and timestamp.
#define TRACE_EVENT_END_WITH_ID_TID_AND_TIMESTAMP0(category_group, name, id, \
                                                   thread_id, timestamp)     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id, thread_id,      \
      timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP0(                \
    category_group, name, id, thread_id, timestamp)                     \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                   \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id, thread_id, \
      timestamp, TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP1(                 \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                    \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id, thread_id,  \
      timestamp, TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_END_WITH_ID_TID_AND_TIMESTAMP2(                 \
    category_group, name, id, thread_id, timestamp, arg1_name, arg1_val, \
    arg2_name, arg2_val)                                                 \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                    \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id, thread_id,  \
      timestamp, TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, arg2_name,  \
      arg2_val)

// Counters.
#define TRACE_COUNTER1(category_group, name, value)                         \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, "value",                  \
                           static_cast<int>(value))
#define TRACE_COUNTER_WITH_FLAG1(category_group, name, flag, value)         \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           flag, "value", static_cast<int>(value))
#define TRACE_COPY_COUNTER1(category_group, name, value)                    \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_COPY, "value",                  \
                           static_cast<int>(value))
#define TRACE_COUNTER2(category_group, name, value1_name, value1_val,       \
                       value2_name, value2_val)                             \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_NONE, value1_name,              \
                           static_cast<int>(value1_val), value2_name,       \
                           static_cast<int>(value2_val))
#define TRACE_COPY_COUNTER2(category_group, name, value1_name, value1_val,  \
                            value2_name, value2_val)                        \
  INTERNAL_TRACE_EVENT_ADD(TRACE_EVENT_PHASE_COUNTER, category_group, name, \
                           TRACE_EVENT_FLAG_COPY, value1_name,              \
                           static_cast<int>(value1_val), value2_name,       \
                           static_cast<int>(value2_val))

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
                                   name, id, TRACE_EVENT_FLAG_COPY, "value",  \
                                   static_cast<int>(value))
#define TRACE_COUNTER_ID2(category_group, name, id, value1_name, value1_val,  \
                          value2_name, value2_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_COUNTER, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE,           \
                                   value1_name, static_cast<int>(value1_val), \
                                   value2_name, static_cast<int>(value2_val))
#define TRACE_COPY_COUNTER_ID2(category_group, name, id, value1_name,         \
                               value1_val, value2_name, value2_val)           \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_COUNTER, category_group, \
                                   name, id, TRACE_EVENT_FLAG_COPY,           \
                                   value1_name, static_cast<int>(value1_val), \
                                   value2_name, static_cast<int>(value2_val))

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
#define TRACE_EVENT_COPY_ASYNC_BEGIN0(category_group, name, id)   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_BEGIN, \
                                   category_group, name, id,      \
                                   TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_ASYNC_BEGIN1(category_group, name, id, arg1_name, \
                                      arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_BEGIN,          \
                                   category_group, name, id,               \
                                   TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_ASYNC_BEGIN2(category_group, name, id, arg1_name, \
                                      arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                        \
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,             \
      TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, arg2_name, arg2_val)
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
      TRACE_EVENT_PHASE_ASYNC_BEGIN, category_group, name, id,                 \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_COPY)
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
#define TRACE_EVENT_COPY_ASYNC_END0(category_group, name, id)   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_END, \
                                   category_group, name, id,    \
                                   TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_ASYNC_END1(category_group, name, id, arg1_name, \
                                    arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ASYNC_END,          \
                                   category_group, name, id,             \
                                   TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_ASYNC_END2(category_group, name, id, arg1_name, \
                                    arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                      \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,             \
      TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, arg2_name, arg2_val)
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
#define TRACE_EVENT_COPY_ASYNC_END_WITH_TIMESTAMP0(category_group, name, id, \
                                                   timestamp)                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                        \
      TRACE_EVENT_PHASE_ASYNC_END, category_group, name, id,                 \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_COPY)
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
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TTS2(                       \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id,        \
      TRACE_EVENT_FLAG_ASYNC_TTS | TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, \
      arg2_name, arg2_val)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TTS2(                         \
    category_group, name, id, arg1_name, arg1_val, arg2_name, arg2_val)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id,          \
      TRACE_EVENT_FLAG_ASYNC_TTS | TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, \
      arg2_name, arg2_val)

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
#define TRACE_EVENT_NESTABLE_ASYNC_INSTANT_WITH_TIMESTAMP0(               \
    category_group, name, id, timestamp)                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                     \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_INSTANT, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_BEGIN_WITH_TIMESTAMP0(          \
    category_group, name, id, timestamp)                                \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                   \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_BEGIN, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_NESTABLE_ASYNC_END_WITH_TIMESTAMP0(          \
    category_group, name, id, timestamp)                              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID_TID_AND_TIMESTAMP(                 \
      TRACE_EVENT_PHASE_NESTABLE_ASYNC_END, category_group, name, id, \
      TRACE_EVENT_API_CURRENT_THREAD_ID, timestamp, TRACE_EVENT_FLAG_COPY)

// Legacy flow events.
#define TRACE_EVENT_FLOW_BEGIN0(category_group, name, id)        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_BEGIN, \
                                   category_group, name, id,     \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_FLOW_BEGIN1(category_group, name, id, arg1_name, arg1_val) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_BEGIN,               \
                                   category_group, name, id,                   \
                                   TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val)
#define TRACE_EVENT_FLOW_BEGIN2(category_group, name, id, arg1_name, arg1_val, \
                                arg2_name, arg2_val)                           \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_FLOW_BEGIN, category_group, name, id,                  \
      TRACE_EVENT_FLAG_NONE, arg1_name, arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_FLOW_BEGIN0(category_group, name, id)   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_BEGIN, \
                                   category_group, name, id,     \
                                   TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_FLOW_BEGIN1(category_group, name, id, arg1_name, \
                                     arg1_val)                            \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_BEGIN,          \
                                   category_group, name, id,              \
                                   TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_FLOW_BEGIN2(category_group, name, id, arg1_name, \
                                     arg1_val, arg2_name, arg2_val)       \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                       \
      TRACE_EVENT_PHASE_FLOW_BEGIN, category_group, name, id,             \
      TRACE_EVENT_FLAG_COPY, arg1_name, arg1_val, arg2_name, arg2_val)

// Legacy flow step events.
#define TRACE_EVENT_FLOW_STEP0(category_group, name, id, step)  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_STEP, \
                                   category_group, name, id,    \
                                   TRACE_EVENT_FLAG_NONE, "step", step)
#define TRACE_EVENT_FLOW_STEP1(category_group, name, id, step, arg1_name, \
                               arg1_val)                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                       \
      TRACE_EVENT_PHASE_FLOW_STEP, category_group, name, id,              \
      TRACE_EVENT_FLAG_NONE, "step", step, arg1_name, arg1_val)
#define TRACE_EVENT_COPY_FLOW_STEP0(category_group, name, id, step) \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_STEP,     \
                                   category_group, name, id,        \
                                   TRACE_EVENT_FLAG_COPY, "step", step)
#define TRACE_EVENT_COPY_FLOW_STEP1(category_group, name, id, step, arg1_name, \
                                    arg1_val)                                  \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(                                            \
      TRACE_EVENT_PHASE_FLOW_STEP, category_group, name, id,                   \
      TRACE_EVENT_FLAG_COPY, "step", step, arg1_name, arg1_val)

// Legacy flow end events.
#define TRACE_EVENT_FLOW_END0(category_group, name, id)                        \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_FLOW_END_BIND_TO_ENCLOSING0(category_group, name, id)      \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id,                                   \
                                   TRACE_EVENT_FLAG_BIND_TO_ENCLOSING)
#define TRACE_EVENT_FLOW_END1(category_group, name, id, arg1_name, arg1_val)   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE, arg1_name, \
                                   arg1_val)
#define TRACE_EVENT_FLOW_END2(category_group, name, id, arg1_name, arg1_val,   \
                              arg2_name, arg2_val)                             \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_NONE, arg1_name, \
                                   arg1_val, arg2_name, arg2_val)
#define TRACE_EVENT_COPY_FLOW_END0(category_group, name, id)                   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_COPY)
#define TRACE_EVENT_COPY_FLOW_END1(category_group, name, id, arg1_name,        \
                                   arg1_val)                                   \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_COPY, arg1_name, \
                                   arg1_val)
#define TRACE_EVENT_COPY_FLOW_END2(category_group, name, id, arg1_name,        \
                                   arg1_val, arg2_name, arg2_val)              \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_FLOW_END, category_group, \
                                   name, id, TRACE_EVENT_FLAG_COPY, arg1_name, \
                                   arg1_val, arg2_name, arg2_val)

// Special strongly typed trace events.
// TODO(skyostil): Migrate these to regular track event trace points.
#define TRACE_TASK_EXECUTION(run_function, task) \
  if (false) {                                   \
    base::ignore_result(run_function);           \
    base::ignore_result(task);                   \
  }

#define TRACE_LOG_MESSAGE(file, message, line) \
  if (false) {                                 \
    base::ignore_result(file);                 \
    base::ignore_result(message);              \
    base::ignore_result(line);                 \
  }

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

// Context events.
#define TRACE_EVENT_ENTER_CONTEXT(category_group, name, context)    \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_ENTER_CONTEXT, \
                                   category_group, name, context,   \
                                   TRACE_EVENT_FLAG_NONE)
#define TRACE_EVENT_LEAVE_CONTEXT(category_group, name, context)    \
  INTERNAL_TRACE_EVENT_ADD_WITH_ID(TRACE_EVENT_PHASE_LEAVE_CONTEXT, \
                                   category_group, name, context,   \
                                   TRACE_EVENT_FLAG_NONE)

// Macro to efficiently determine if a given category group is enabled.
// TODO(skyostil): Implement.
#define TRACE_EVENT_CATEGORY_GROUP_ENABLED(category_group, ret) \
  do {                                                          \
    *ret = false;                                               \
  } while (0)

// Macro to efficiently determine, through polling, if a new trace has begun.
// TODO(skyostil): Implement.
#define TRACE_EVENT_IS_NEW_TRACE(ret) \
  do {                                \
    *ret = false;                     \
  } while (0)

// Time queries.
#define TRACE_TIME_TICKS_NOW() INTERNAL_TRACE_TIME_TICKS_NOW()
#define TRACE_TIME_NOW() INTERNAL_TRACE_TIME_NOW()

// ----------------------------------------------------------------------------
// Legacy tracing API (adapted from trace_event.h).
// ----------------------------------------------------------------------------

// We can implement the following subset of the legacy tracing API without
// involvement from the embedder. APIs such as TraceId and
// TRACE_EVENT_API_ADD_TRACE_EVENT are still up to the embedder to define.

#define TRACE_STR_COPY(str) (str)

// TODO(skyostil): Implement properly using CategoryRegistry.
#define TRACE_EVENT_API_GET_CATEGORY_GROUP_ENABLED(category) \
  [&] {                                                      \
    static uint8_t enabled;                                  \
    TRACE_EVENT_CATEGORY_GROUP_ENABLED(category, &enabled);  \
    return &enabled;                                         \
  }()

#endif  // PERFETTO_ENABLE_LEGACY_TRACE_EVENTS

#endif  // INCLUDE_PERFETTO_TRACING_TRACK_EVENT_LEGACY_H_
