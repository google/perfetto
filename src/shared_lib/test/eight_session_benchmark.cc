/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "perfetto/base/logging.h"
#include "perfetto/ext/tracing/core/client_identity.h"
#include "perfetto/ext/tracing/core/tracing_service.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_event.pzc.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/track_event.h"
#include "perfetto/tracing/tracing.h"
#include "perfetto/tracing/tracing_backend.h"
#include "protos/perfetto/config/data_source_config.gen.h"
#include "protos/perfetto/config/trace_config.gen.h"
#include "protos/perfetto/config/track_event/track_event_config.gen.h"
#include "src/shared_lib/test/utils.h"
#include "src/tracing/core/in_process_shared_memory.h"

#define EIGHT_SESSION_CATEGORIES(C)               \
  C(eight_session_cat, "sdk_eight_sessions", "")  \
  C(eight_session_marker, "sdk_eight_marker", "") \
  C(eight_session_disabled, "sdk_eight_disabled", "")

PERFETTO_TE_CATEGORIES_DEFINE(EIGHT_SESSION_CATEGORIES)

namespace {

constexpr size_t kSessionCount = 8;
constexpr char kMarker[] = "sdk_eight_sessions_verified";

// Distinct consumer identities allow eight real sessions without changing the
// service's five-sessions-per-UID limit. All endpoints use the SDK task runner.
class EightSessionBackend : public perfetto::TracingBackend {
 public:
  std::unique_ptr<perfetto::ProducerEndpoint> ConnectProducer(
      const ConnectProducerArgs& args) override {
    return Service(args.task_runner)
        ->ConnectProducer(
            args.producer, perfetto::ClientIdentity(0, 0), args.producer_name,
            args.shmem_size_hint_bytes, /*in_process=*/true,
            perfetto::TracingService::ProducerSMBScrapingMode::kEnabled,
            args.shmem_page_size_hint_bytes, nullptr, {}, {},
            perfetto::kProtocolAbiV1 | perfetto::kProtocolAbiV2);
  }

  std::unique_ptr<perfetto::ConsumerEndpoint> ConnectConsumer(
      const ConnectConsumerArgs& args) override {
    return Service(args.task_runner)
        ->ConnectConsumer(args.consumer, next_uid_++);
  }

 private:
  perfetto::TracingService* Service(perfetto::base::TaskRunner* task_runner) {
    if (!service_) {
      service_ = perfetto::TracingService::CreateInstance(
          std::make_unique<perfetto::InProcessSharedMemory::Factory>(),
          task_runner);
      service_->SetSMBScrapingEnabled(true);
    }
    return service_.get();
  }

  std::unique_ptr<perfetto::TracingService> service_;
  uid_t next_uid_ = 1;
};

void InitializeEightSessions() {
  static const bool initialized = [] {
    // The SDK retains the backend for the process lifetime.
    static auto* backend = new EightSessionBackend();
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kCustomBackend;
    args.custom_backend = backend;
    args.shmem_size_hint_kb = 8192;
    perfetto::Tracing::Initialize(args);
    PerfettoTeInit();
    PERFETTO_TE_REGISTER_CATEGORIES(EIGHT_SESSION_CATEGORIES);
    return true;
  }();
  (void)initialized;
}

#define EIGHT_SESSION_CASES(C) \
  C(Instant)                   \
  C(SliceBegin)                \
  C(SliceEnd)                  \
  C(BeginEnd)                  \
  C(State)                     \
  C(CounterInt)                \
  C(CounterDouble)             \
  C(IgnoredCounters)           \
  C(EmptyName)                 \
  C(LongName)                  \
  C(NoIntern)                  \
  C(LongNameNoIntern)          \
  C(NewNames)                  \
  C(DebugBool)                 \
  C(DebugUint)                 \
  C(DebugInt)                  \
  C(DebugDouble)               \
  C(DebugString)               \
  C(DebugPointer)              \
  C(AllDebugTypes)             \
  C(ManyDebugArgs)             \
  C(Flows)                     \
  C(TerminatingFlows)          \
  C(CorrelationInt)            \
  C(CorrelationString)         \
  C(NamedTrack)                \
  C(LongNamedTrack)            \
  C(NewNamedTracks)            \
  C(RegisteredTrack)           \
  C(ProtoTrack)                \
  C(NestedThread)              \
  C(NestedProcess)             \
  C(NestedNamed)               \
  C(NestedProto)               \
  C(NestedRegistered)          \
  C(NestedMixed)               \
  C(ExplicitIncremental)       \
  C(ExplicitAbsolute)          \
  C(ExplicitCustomClock)       \
  C(MixedSessionClocks)        \
  C(IncrementalResets)         \
  C(DynamicEnabled)            \
  C(DynamicDisabled)           \
  C(DynamicHalfEnabled)        \
  C(DynamicTagged)             \
  C(DynamicDisabledLongNames)  \
  C(DisabledCategory)          \
  C(ProtoString)               \
  C(ProtoBytes)                \
  C(ProtoVarint)               \
  C(ProtoFixed32)              \
  C(ProtoFixed64)              \
  C(ProtoDouble)               \
  C(ProtoFloat)                \
  C(ProtoNested)               \
  C(ProtoInternedString)       \
  C(AllProtoTypes)             \
  C(LargePayload)              \
  C(CustomOverrides)           \
  C(Flush)                     \
  C(MixedFields)               \
  C(MixedFieldsFlush)

enum class EventCase {
#define ENUM_CASE(name) name,
  EIGHT_SESSION_CASES(ENUM_CASE)
#undef ENUM_CASE
};

struct EventInputs {
  std::string long_name = std::string(512, 'n');
  std::string payload = std::string(4096, 'p');
  PerfettoTeRegisteredTrack registered{};
  PerfettoTeRegisteredTrack counter{};
  PerfettoTeTimestamp timestamp{};
  const char* tags[1] = {"sdk_tag"};
  PerfettoTeCategoryDescriptor dynamic{"sdk_dynamic", "", tags, 1};
  PerfettoTeHlExtraDynamicCategory dynamic_extra{
      {PERFETTO_TE_HL_EXTRA_TYPE_DYNAMIC_CATEGORY},
      &dynamic};
};

// The public macro work is inside the timed loop, as it is for SDK callers.
// Every case runs through the same dispatch in both compared implementations.
void EmitEightSessionEvent(EventCase which, uint64_t i, EventInputs& in) {
  switch (which) {
    case EventCase::Instant:
    case EventCase::MixedSessionClocks:
    case EventCase::IncrementalResets:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"));
      break;
    case EventCase::SliceBegin:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_SLICE_BEGIN("Event"));
      break;
    case EventCase::SliceEnd:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_SLICE_END());
      break;
    case EventCase::BeginEnd:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_SLICE_BEGIN("Event"));
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_SLICE_END());
      break;
    case EventCase::State:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_STATE("Busy"),
          PERFETTO_TE_STATE_TRACK("state", PerfettoTeProcessTrackUuid()));
      break;
    case EventCase::CounterInt:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_COUNTER(),
                  PERFETTO_TE_REGISTERED_TRACK(&in.counter),
                  PERFETTO_TE_INT_COUNTER(static_cast<int64_t>(i)));
      break;
    case EventCase::CounterDouble:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_COUNTER(),
                  PERFETTO_TE_REGISTERED_TRACK(&in.counter),
                  PERFETTO_TE_DOUBLE_COUNTER(static_cast<double>(i)));
      break;
    case EventCase::IgnoredCounters:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_INT_COUNTER(static_cast<int64_t>(i)),
                  PERFETTO_TE_DOUBLE_COUNTER(1.5));
      break;
    case EventCase::EmptyName:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT(""));
      break;
    case EventCase::LongName:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT(in.long_name.c_str()));
      break;
    case EventCase::NoIntern:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NO_INTERN());
      break;
    case EventCase::LongNameNoIntern:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT(in.long_name.c_str()),
                  PERFETTO_TE_NO_INTERN());
      break;
    case EventCase::NewNames: {
      char name[48];
      snprintf(name, sizeof(name), "event_%llu",
               static_cast<unsigned long long>(i));
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT(name));
      break;
    }
    case EventCase::DebugBool:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_ARG_BOOL("bool", (i & 1) != 0));
      break;
    case EventCase::DebugUint:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_ARG_UINT64("uint", i));
      break;
    case EventCase::DebugInt:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_ARG_INT64("int", -static_cast<int64_t>(i)));
      break;
    case EventCase::DebugDouble:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_ARG_DOUBLE("double", 1.5));
      break;
    case EventCase::DebugString:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_ARG_STRING("string", "example value"));
      break;
    case EventCase::DebugPointer:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_ARG_POINTER("pointer", reinterpret_cast<uintptr_t>(&in)));
      break;
    case EventCase::AllDebugTypes:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_ARG_BOOL("bool", true), PERFETTO_TE_ARG_UINT64("uint", i),
          PERFETTO_TE_ARG_INT64("int", -42),
          PERFETTO_TE_ARG_DOUBLE("double", 1.5),
          PERFETTO_TE_ARG_STRING("string", "value"),
          PERFETTO_TE_ARG_POINTER("pointer", reinterpret_cast<uintptr_t>(&in)));
      break;
    case EventCase::ManyDebugArgs:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_ARG_UINT64("a0", i), PERFETTO_TE_ARG_UINT64("a1", i),
          PERFETTO_TE_ARG_UINT64("a2", i), PERFETTO_TE_ARG_UINT64("a3", i),
          PERFETTO_TE_ARG_UINT64("a4", i), PERFETTO_TE_ARG_UINT64("a5", i),
          PERFETTO_TE_ARG_UINT64("a6", i), PERFETTO_TE_ARG_UINT64("a7", i),
          PERFETTO_TE_ARG_UINT64("a8", i), PERFETTO_TE_ARG_UINT64("a9", i),
          PERFETTO_TE_ARG_UINT64("a10", i), PERFETTO_TE_ARG_UINT64("a11", i),
          PERFETTO_TE_ARG_UINT64("a12", i), PERFETTO_TE_ARG_UINT64("a13", i),
          PERFETTO_TE_ARG_UINT64("a14", i), PERFETTO_TE_ARG_UINT64("a15", i));
      break;
    case EventCase::Flows:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_FLOW(PerfettoTeFlow{i}),
                  PERFETTO_TE_FLOW(PerfettoTeFlow{i + 1}));
      break;
    case EventCase::TerminatingFlows:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_TERMINATING_FLOW(PerfettoTeFlow{i}),
                  PERFETTO_TE_TERMINATING_FLOW(PerfettoTeFlow{i + 1}));
      break;
    case EventCase::CorrelationInt:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_CORRELATION_ID(i));
      break;
    case EventCase::CorrelationString:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_CORRELATION_ID_STR("request"));
      break;
    case EventCase::NamedTrack:
    case EventCase::NewNamedTracks:
      // Root tracks keep the miss case's UUID sequence identical across
      // processes; otherwise FlatSet insertion costs depend on the process
      // UUID.
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NAMED_TRACK(
                      "named", which == EventCase::NewNamedTracks ? i : 1,
                      which == EventCase::NewNamedTracks
                          ? 0
                          : PerfettoTeProcessTrackUuid()));
      break;
    case EventCase::LongNamedTrack:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NAMED_TRACK(in.long_name.c_str(), 1,
                                          PerfettoTeProcessTrackUuid()));
      break;
    case EventCase::RegisteredTrack:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_REGISTERED_TRACK(&in.registered));
      break;
    case EventCase::ProtoTrack:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_TRACK(
              123456,
              PERFETTO_TE_PROTO_FIELD_CSTR(
                  perfetto_protos_TrackDescriptor_name_field_number, "proto")));
      break;
    case EventCase::NestedThread:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NESTED_TRACKS(PERFETTO_TE_NESTED_TRACK_THREAD()));
      break;
    case EventCase::NestedProcess:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_NESTED_TRACKS(PERFETTO_TE_NESTED_TRACK_PROCESS()));
      break;
    case EventCase::NestedNamed:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NESTED_TRACKS(
                      PERFETTO_TE_NESTED_TRACK_PROCESS(),
                      PERFETTO_TE_NESTED_TRACK_NAMED("nested", 1)));
      break;
    case EventCase::NestedProto:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_NESTED_TRACKS(
              PERFETTO_TE_NESTED_TRACK_PROCESS(),
              PERFETTO_TE_NESTED_TRACK_PROTO(
                  123456, PERFETTO_TE_PROTO_FIELD_CSTR(
                              perfetto_protos_TrackDescriptor_name_field_number,
                              "proto"))));
      break;
    case EventCase::NestedRegistered:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_NESTED_TRACKS(
                      PERFETTO_TE_NESTED_TRACK_REGISTERED(&in.registered)));
      break;
    case EventCase::NestedMixed:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_NESTED_TRACKS(
              PERFETTO_TE_NESTED_TRACK_THREAD(),
              PERFETTO_TE_NESTED_TRACK_NAMED_ORDERED(
                  "parent", 1, 2, PERFETTO_TE_HL_CHILD_ORDERING_EXPLICIT),
              PERFETTO_TE_NESTED_TRACK_NAMED_MERGED(
                  "child", 2,
                  PERFETTO_TE_HL_SIBLING_MERGE_BEHAVIOR_BY_SIBLING_MERGE_KEY,
                  "key", 0),
              PERFETTO_TE_NESTED_TRACK_PROTO(
                  7, PERFETTO_TE_PROTO_FIELD_CSTR(
                         perfetto_protos_TrackDescriptor_name_field_number,
                         "leaf"))));
      break;
    case EventCase::ExplicitIncremental:
    case EventCase::ExplicitAbsolute:
    case EventCase::ExplicitCustomClock:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_TIMESTAMP(in.timestamp));
      break;
    case EventCase::DynamicEnabled:
    case EventCase::DynamicDisabled:
    case EventCase::DynamicHalfEnabled:
      PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_DYNAMIC_CATEGORY_STRING("sdk_dynamic"));
      break;
    case EventCase::DynamicTagged: {
      PerfettoTeHlExtra* extras[] = {&in.dynamic_extra.header, nullptr};
      PerfettoTeHlEmitImpl(PerfettoTeRegisteredDynamicCategory().impl,
                           PERFETTO_TE_TYPE_INSTANT, "Event", extras);
      break;
    }
    case EventCase::DynamicDisabledLongNames:
      PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY,
                  PERFETTO_TE_INSTANT(in.long_name.c_str()),
                  PERFETTO_TE_DYNAMIC_CATEGORY_STRING("sdk_dynamic"),
                  PERFETTO_TE_NAMED_TRACK(in.long_name.c_str(), 1,
                                          PerfettoTeProcessTrackUuid()));
      break;
    case EventCase::DisabledCategory:
      PERFETTO_TE(eight_session_disabled, PERFETTO_TE_INSTANT("Event"));
      break;
    // Private field numbers exercise the custom proto encoder without making
    // the synthetic fields look like existing TrackEvent schema fields.
    case EventCase::ProtoString:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(
                      PERFETTO_TE_PROTO_FIELD_CSTR(1000, "value")));
      break;
    case EventCase::ProtoBytes:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(
                      PERFETTO_TE_PROTO_FIELD_BYTES(1000, "bytes", 5)));
      break;
    case EventCase::ProtoVarint:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_VARINT(1000, i)));
      break;
    case EventCase::ProtoFixed32:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_FIXED32(1000, 42)));
      break;
    case EventCase::ProtoFixed64:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_FIXED64(1000, i)));
      break;
    case EventCase::ProtoDouble:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_DOUBLE(1000, 1.5)));
      break;
    case EventCase::ProtoFloat:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_FLOAT(1000, 1.5f)));
      break;
    case EventCase::ProtoNested:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
                      1000, PERFETTO_TE_PROTO_FIELD_CSTR(1, "nested"),
                      PERFETTO_TE_PROTO_FIELD_NESTED(
                          2, PERFETTO_TE_PROTO_FIELD_VARINT(1, i)))));
      break;
    case EventCase::ProtoInternedString: {
      PerfettoTeHlProtoFieldCstrInterned field{
          {PERFETTO_TE_HL_PROTO_TYPE_CSTR_INTERNED, 1000}, "interned", 2};
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(&field.header));
      break;
    }
    case EventCase::AllProtoTypes:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
                      1000, PERFETTO_TE_PROTO_FIELD_CSTR(1, "value"),
                      PERFETTO_TE_PROTO_FIELD_BYTES(2, "bytes", 5),
                      PERFETTO_TE_PROTO_FIELD_VARINT(3, i),
                      PERFETTO_TE_PROTO_FIELD_ZIGZAG(4, -42),
                      PERFETTO_TE_PROTO_FIELD_FIXED32(5, 42),
                      PERFETTO_TE_PROTO_FIELD_FIXED64(6, i),
                      PERFETTO_TE_PROTO_FIELD_FLOAT(7, 1.5f),
                      PERFETTO_TE_PROTO_FIELD_DOUBLE(8, 1.5))));
      break;
    case EventCase::LargePayload:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_BYTES(
                      1000, in.payload.data(), in.payload.size())));
      break;
    case EventCase::CustomOverrides:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_CORRELATION_ID(1),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_VARINT(
              perfetto_protos_TrackEvent_correlation_id_field_number, 2)));
      break;
    case EventCase::Flush:
      PERFETTO_TE(eight_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_FLUSH());
      break;
    case EventCase::MixedFields:
    case EventCase::MixedFieldsFlush:
      PERFETTO_TE(
          eight_session_cat, PERFETTO_TE_INSTANT("Mixed"),
          PERFETTO_TE_NAMED_TRACK("mixed", 7, PerfettoTeProcessTrackUuid()),
          PERFETTO_TE_NO_INTERN(), PERFETTO_TE_ARG_BOOL("bool", true),
          PERFETTO_TE_ARG_UINT64("uint", i), PERFETTO_TE_ARG_INT64("int", -42),
          PERFETTO_TE_ARG_DOUBLE("double", 1.5),
          PERFETTO_TE_ARG_STRING("string", "value"),
          PERFETTO_TE_ARG_POINTER("pointer", reinterpret_cast<uintptr_t>(&in)),
          PERFETTO_TE_FLOW(PerfettoTeFlow{i}),
          PERFETTO_TE_TERMINATING_FLOW(PerfettoTeFlow{i + 1}),
          PERFETTO_TE_CORRELATION_ID_STR("request"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
              1000, PERFETTO_TE_PROTO_FIELD_CSTR(1, "value"),
              PERFETTO_TE_PROTO_FIELD_VARINT(2, i))));
      if (which == EventCase::MixedFieldsFlush) {
        PERFETTO_TE(eight_session_cat, PERFETTO_TE_SLICE_END(),
                    PERFETTO_TE_FLUSH());
      }
      break;
  }
}

struct EventCounts {
  size_t events = 0;
  size_t markers = 0;
};

EventCounts CountEvents(const std::vector<char>& trace) {
  EventCounts counts;
  using perfetto::shlib::test_utils::IdFieldView;
  const auto* begin = reinterpret_cast<const uint8_t*>(trace.data());
  for (auto packet : IdFieldView(begin, begin + trace.size(),
                                 perfetto_protos_Trace_packet_field_number)) {
    for (auto event : IdFieldView(
             packet, perfetto_protos_TracePacket_track_event_field_number)) {
      bool is_marker = false;
      for (auto name :
           IdFieldView(event, perfetto_protos_TrackEvent_name_field_number)) {
        if (name.wire_type == PERFETTO_PB_WIRE_TYPE_DELIMITED &&
            name.value.delimited.len == sizeof(kMarker) - 1 &&
            memcmp(name.value.delimited.start, kMarker, sizeof(kMarker) - 1) ==
                0) {
          is_marker = true;
        }
      }
      if (is_marker)
        ++counts.markers;
      else
        ++counts.events;
    }
  }
  return counts;
}

void BM_Shlib_EightSessions(benchmark::State& state,
                            EventCase which,
                            bool writer_v2) {
  InitializeEightSessions();
  std::vector<std::unique_ptr<perfetto::TracingSession>> sessions;
  sessions.reserve(kSessionCount);
  for (size_t i = 0; i < kSessionCount; ++i) {
    perfetto::TraceConfig cfg;
    auto* buffer = cfg.add_buffers();
    buffer->set_size_kb(8192);
    if (writer_v2) {
      buffer->set_experimental_mode(
          perfetto::TraceConfig::BufferConfig::TRACE_BUFFER_V2);
    }
    auto* ds = cfg.add_data_sources()->mutable_config();
    ds->set_name("track_event");
    ds->mutable_experimental_tracing_v2()->set_use_v2_probability_percent(
        writer_v2 ? 100 : 0);
    perfetto::protos::gen::TrackEventConfig track_event_config;
    auto* te = &track_event_config;
    te->add_enabled_categories("sdk_eight_sessions");
    te->add_enabled_categories("sdk_eight_marker");
    te->add_disabled_categories("*");
    bool dynamic_enabled =
        which != EventCase::DynamicDisabled &&
        which != EventCase::DynamicDisabledLongNames &&
        which != EventCase::DynamicTagged &&
        (which != EventCase::DynamicHalfEnabled || i % 2 == 0);
    if (dynamic_enabled)
      te->add_enabled_categories("sdk_dynamic");
    if (which == EventCase::DynamicTagged)
      te->add_enabled_tags("sdk_tag");
    if (which == EventCase::MixedSessionClocks) {
      te->set_disable_incremental_timestamps(i % 3 == 1);
      te->set_timestamp_unit_multiplier(i % 3 == 2 ? 1000 : 1);
    }
    if (which == EventCase::IncrementalResets)
      cfg.mutable_incremental_state_config()->set_clear_period_ms(1);
    ds->set_track_event_config_raw(track_event_config.SerializeAsString());
    auto session = perfetto::Tracing::NewTrace(perfetto::kCustomBackend);
    session->SetOnErrorCallback([](perfetto::TracingError error) {
      if (error.code == perfetto::TracingError::kTracingFailed) {
        PERFETTO_FATAL("Eight-session benchmark setup failed: %s",
                       error.message.c_str());
      }
    });
    session->Setup(cfg);
    session->StartBlocking();
    sessions.push_back(std::move(session));
  }

  EventInputs in;
  PerfettoTeNamedTrackRegister(&in.registered, "registered", 1,
                               PerfettoTeProcessTrackUuid(), true);
  PerfettoTeCounterTrackRegister(&in.counter, "counter",
                                 PerfettoTeProcessTrackUuid(), true);
  in.timestamp.clock_id = which == EventCase::ExplicitAbsolute
                              ? PERFETTO_TE_TIMESTAMP_TYPE_BOOT
                          : which == EventCase::ExplicitCustomClock
                              ? 128
                              : PERFETTO_TE_TIMESTAMP_TYPE_INCREMENTAL;
  in.timestamp.value = 1000000000;

  // Warm common sequence metadata before timing. Miss cases keep every timed
  // name/track unique and use a fixed iteration count to bound retained maps.
  EmitEightSessionEvent(EventCase::Instant, 0, in);
  const bool cold =
      which == EventCase::NewNames || which == EventCase::NewNamedTracks;
  if (!cold) {
    for (uint64_t i = 0; i < 32; ++i)
      EmitEightSessionEvent(which, i, in);
  }

  const int events_per_iteration =
      which == EventCase::BeginEnd || which == EventCase::MixedFieldsFlush ? 2
                                                                           : 1;
  // Bound each burst so the producer cannot fill its shared memory and start
  // dropping events. Drain and verify every session outside the timed region.
  // This measures the caller's emission cost, not consumer throughput.
  int64_t batch_size = 1024;
  if (which == EventCase::LargePayload)
    batch_size = 64;
  if (which == EventCase::Flush || which == EventCase::MixedFieldsFlush)
    batch_size = 128;
  if (which == EventCase::DisabledCategory)
    batch_size = 65536;

  auto drain = [&](bool verify) {
    PERFETTO_TE(eight_session_marker, PERFETTO_TE_INSTANT(kMarker),
                PERFETTO_TE_NO_INTERN(), PERFETTO_TE_FLUSH());
    size_t verified = 0;
    for (size_t i = 0; i < sessions.size(); ++i) {
      const bool flushed = sessions[i]->FlushBlocking();
      EventCounts counts = CountEvents(sessions[i]->ReadTraceBlocking());
      const bool filtered =
          which == EventCase::DisabledCategory ||
          which == EventCase::DynamicDisabled ||
          which == EventCase::DynamicDisabledLongNames ||
          (which == EventCase::DynamicHalfEnabled && i % 2 != 0);
      const size_t expected =
          filtered ? 0 : static_cast<size_t>(batch_size * events_per_iteration);
      if (flushed && counts.markers == 1 &&
          (!verify || counts.events == expected)) {
        ++verified;
      }
    }
    return verified;
  };
  size_t verified = drain(false);
  uint64_t iteration = 0;
  while (state.KeepRunningBatch(static_cast<uint64_t>(batch_size))) {
    for (int64_t i = 0; i < batch_size; ++i) {
      EmitEightSessionEvent(which, iteration++, in);
      benchmark::ClobberMemory();
    }
    state.PauseTiming();
    verified = drain(true);
    state.ResumeTiming();
    if (verified != kSessionCount) {
      state.SkipWithError(
          "Event count or flush failed in an eight-session batch");
      break;
    }
  }

  for (auto& session : sessions)
    session->StopBlocking();
  state.counters["VerifiedSessions"] = static_cast<double>(verified);
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          events_per_iteration);
  PerfettoTeRegisteredTrackUnregister(&in.counter);
  PerfettoTeRegisteredTrackUnregister(&in.registered);
}

bool RegisterEightSessionBenchmarks() {
#define REGISTER_CASE(name)                                                    \
  for (bool v2 : {false, true}) {                                              \
    std::string label =                                                        \
        std::string("BM_Shlib_EightSessions/") + #name + (v2 ? "/v2" : "/v1"); \
    auto* b = benchmark::RegisterBenchmark(                                    \
        label.c_str(), BM_Shlib_EightSessions, EventCase::name, v2);           \
    b->MeasureProcessCPUTime()->UseRealTime();                                 \
    if (EventCase::name == EventCase::NewNames ||                              \
        EventCase::name == EventCase::NewNamedTracks)                          \
      b->Iterations(16384);                                                    \
  }
  EIGHT_SESSION_CASES(REGISTER_CASE)
#undef REGISTER_CASE
  return true;
}

const bool registered_eight_session_benchmarks =
    RegisterEightSessionBenchmarks();

}  // namespace
