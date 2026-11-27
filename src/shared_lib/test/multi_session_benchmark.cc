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
#include <string.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
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

#define MULTI_SESSION_CATEGORIES(C)                       \
  C(multi_session_cat, "sdk_multi_session", "")           \
  C(multi_session_marker, "sdk_multi_session_marker", "") \
  C(multi_session_disabled, "sdk_multi_session_disabled", "")

PERFETTO_TE_CATEGORIES_DEFINE(MULTI_SESSION_CATEGORIES)

namespace {

constexpr size_t kSessionCount = 8;
constexpr char kMarker[] = "sdk_multi_session_verified";

// Distinct consumer UIDs let us open eight sessions without hitting the
// service's five-sessions-per-UID limit.
// All endpoints share the SDK task runner.
class MultiSessionBackend : public perfetto::TracingBackend {
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

void InitializeMultiSession() {
  static const bool initialized = [] {
    // The SDK retains the backend for the process lifetime.
    static auto* backend = new MultiSessionBackend();
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kCustomBackend;
    args.custom_backend = backend;
    args.shmem_size_hint_kb = 8192;
    perfetto::Tracing::Initialize(args);
    PerfettoTeInit();
    PERFETTO_TE_REGISTER_CATEGORIES(MULTI_SESSION_CATEGORIES);
    return true;
  }();
  (void)initialized;
}

#define MULTI_SESSION_CASES(C) \
  C(Instant)                   \
  C(DisabledCategory)          \
  C(NamedTrack)                \
  C(ManyDebugArgs)             \
  C(ProtoNested)               \
  C(DynamicDisabled)           \
  C(MixedFields)

enum class EventCase {
#define ENUM_CASE(name) name,
  MULTI_SESSION_CASES(ENUM_CASE)
#undef ENUM_CASE
};

// Keep the public macros inside the timed loop so the full caller emit path is
// measured.
void EmitMultiSessionEvent(EventCase which, uint64_t i) {
  switch (which) {
    case EventCase::Instant:
      PERFETTO_TE(multi_session_cat, PERFETTO_TE_INSTANT("Event"));
      break;
    case EventCase::DisabledCategory:
      PERFETTO_TE(multi_session_disabled, PERFETTO_TE_INSTANT("Event"));
      break;
    case EventCase::NamedTrack:
      PERFETTO_TE(
          multi_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_NAMED_TRACK("named", 1, PerfettoTeProcessTrackUuid()));
      break;
    case EventCase::ManyDebugArgs:
      PERFETTO_TE(
          multi_session_cat, PERFETTO_TE_INSTANT("Event"),
          PERFETTO_TE_ARG_UINT64("a0", i), PERFETTO_TE_ARG_UINT64("a1", i),
          PERFETTO_TE_ARG_UINT64("a2", i), PERFETTO_TE_ARG_UINT64("a3", i),
          PERFETTO_TE_ARG_UINT64("a4", i), PERFETTO_TE_ARG_UINT64("a5", i),
          PERFETTO_TE_ARG_UINT64("a6", i), PERFETTO_TE_ARG_UINT64("a7", i),
          PERFETTO_TE_ARG_UINT64("a8", i), PERFETTO_TE_ARG_UINT64("a9", i),
          PERFETTO_TE_ARG_UINT64("a10", i), PERFETTO_TE_ARG_UINT64("a11", i),
          PERFETTO_TE_ARG_UINT64("a12", i), PERFETTO_TE_ARG_UINT64("a13", i),
          PERFETTO_TE_ARG_UINT64("a14", i), PERFETTO_TE_ARG_UINT64("a15", i));
      break;
    // Use private field numbers so synthetic fields do not collide with
    // TrackEvent schema fields.
    case EventCase::ProtoNested:
      PERFETTO_TE(multi_session_cat, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
                      1000, PERFETTO_TE_PROTO_FIELD_CSTR(1, "nested"),
                      PERFETTO_TE_PROTO_FIELD_NESTED(
                          2, PERFETTO_TE_PROTO_FIELD_VARINT(1, i)))));
      break;
    case EventCase::DynamicDisabled:
      PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT("Event"),
                  PERFETTO_TE_DYNAMIC_CATEGORY_STRING("sdk_dynamic"));
      break;
    case EventCase::MixedFields:
      PERFETTO_TE(
          multi_session_cat, PERFETTO_TE_INSTANT("Mixed"),
          PERFETTO_TE_NAMED_TRACK("mixed", 7, PerfettoTeProcessTrackUuid()),
          PERFETTO_TE_NO_INTERN(), PERFETTO_TE_ARG_BOOL("bool", true),
          PERFETTO_TE_ARG_UINT64("uint", i), PERFETTO_TE_ARG_INT64("int", -42),
          PERFETTO_TE_ARG_DOUBLE("double", 1.5),
          PERFETTO_TE_ARG_STRING("string", "value"),
          PERFETTO_TE_ARG_POINTER("pointer", reinterpret_cast<uintptr_t>(&i)),
          PERFETTO_TE_FLOW(PerfettoTeFlow{i}),
          PERFETTO_TE_TERMINATING_FLOW(PerfettoTeFlow{i + 1}),
          PERFETTO_TE_CORRELATION_ID_STR("request"),
          PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
              1000, PERFETTO_TE_PROTO_FIELD_CSTR(1, "value"),
              PERFETTO_TE_PROTO_FIELD_VARINT(2, i))));
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

void BM_Shlib_MultiSession(benchmark::State& state,
                           EventCase which,
                           bool writer_v2) {
  InitializeMultiSession();
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
    track_event_config.add_enabled_categories("sdk_multi_session");
    track_event_config.add_enabled_categories("sdk_multi_session_marker");
    track_event_config.add_disabled_categories("*");
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

  // Warm sequence metadata and intern tables before timing.
  EmitMultiSessionEvent(EventCase::Instant, 0);
  for (uint64_t i = 0; i < 32; ++i) {
    EmitMultiSessionEvent(which, i);
  }

  // Keep each batch small enough that the producer does not fill shared memory
  // and drop events.
  // Drain and verify every session outside the timed region so we measure emit
  // cost rather than consumer throughput.
  const int64_t batch_size =
      which == EventCase::DisabledCategory ? 65536 : 1024;
  const bool filtered = which == EventCase::DisabledCategory ||
                        which == EventCase::DynamicDisabled;

  auto drain = [&](bool verify) {
    PERFETTO_TE(multi_session_marker, PERFETTO_TE_INSTANT(kMarker),
                PERFETTO_TE_NO_INTERN(), PERFETTO_TE_FLUSH());
    size_t verified = 0;
    for (auto& session : sessions) {
      const bool flushed = session->FlushBlocking();
      EventCounts counts = CountEvents(session->ReadTraceBlocking());
      const size_t expected = filtered ? 0 : static_cast<size_t>(batch_size);
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
      EmitMultiSessionEvent(which, iteration++);
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

  for (auto& session : sessions) {
    session->StopBlocking();
  }
  state.counters["VerifiedSessions"] = static_cast<double>(verified);
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

bool RegisterMultiSessionBenchmarks() {
#define REGISTER_CASE(name)                                                   \
  for (bool v2 : {false, true}) {                                             \
    std::string label =                                                       \
        std::string("BM_Shlib_MultiSession/") + #name + (v2 ? "/v2" : "/v1"); \
    auto* b = benchmark::RegisterBenchmark(                                   \
        label.c_str(), BM_Shlib_MultiSession, EventCase::name, v2);           \
    b->MeasureProcessCPUTime()->UseRealTime();                                \
  }
  MULTI_SESSION_CASES(REGISTER_CASE)
#undef REGISTER_CASE
  return true;
}

const bool registered_multi_session_benchmarks =
    RegisterMultiSessionBenchmarks();

}  // namespace
