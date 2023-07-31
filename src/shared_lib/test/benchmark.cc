/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <memory>

#include <benchmark/benchmark.h>

#include "perfetto/public/data_source.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"

#include "src/shared_lib/test/utils.h"

static struct PerfettoDs custom = PERFETTO_DS_INIT();

namespace {

using ::perfetto::shlib::test_utils::FieldView;
using ::perfetto::shlib::test_utils::IdFieldView;
using ::perfetto::shlib::test_utils::TracingSession;

constexpr char kDataSourceName[] = "com.example.custom_data_source";

bool Initialize() {
  struct PerfettoProducerInitArgs args = {0};
  args.backends = PERFETTO_BACKEND_IN_PROCESS;
  PerfettoProducerInit(args);
  PerfettoDsRegister(&custom, kDataSourceName, PerfettoDsParamsDefault());
  return true;
}

void EnsureInitialized() {
  static bool initialized = Initialize();
  (void)initialized;
}

size_t DecodePacketSizes(const std::vector<uint8_t>& data) {
  for (struct PerfettoPbDecoderField field :
       IdFieldView(data, perfetto_protos_Trace_packet_field_number)) {
    if (field.status != PERFETTO_PB_DECODER_OK ||
        field.wire_type != PERFETTO_PB_WIRE_TYPE_DELIMITED) {
      abort();
    }
    IdFieldView for_testing_fields(
        field, perfetto_protos_TracePacket_for_testing_field_number);
    if (!for_testing_fields.ok()) {
      abort();
    }
    if (for_testing_fields.size() == 0) {
      continue;
    }
    if (for_testing_fields.size() > 1 || for_testing_fields.front().wire_type !=
                                             PERFETTO_PB_WIRE_TYPE_DELIMITED) {
      abort();
    }
    return field.value.delimited.len;
  }

  return 0;
}

void BM_Shlib_DataSource_Disabled(benchmark::State& state) {
  EnsureInitialized();
  for (auto _ : state) {
    PERFETTO_DS_TRACE(custom, ctx) {}
    benchmark::ClobberMemory();
  }
}

void BM_Shlib_DataSource_DifferentPacketSize(benchmark::State& state) {
  EnsureInitialized();
  TracingSession tracing_session =
      TracingSession::Builder().set_data_source_name(kDataSourceName).Build();

  // This controls the number of times a field is added in the trace packet.
  // It controls the size of the trace packet. The PacketSize counter reports
  // the exact number.
  const size_t kNumFields = static_cast<size_t>(state.range(0));

  for (auto _ : state) {
    PERFETTO_DS_TRACE(custom, ctx) {
      struct PerfettoDsRootTracePacket trace_packet;
      PerfettoDsTracerPacketBegin(&ctx, &trace_packet);

      {
        struct perfetto_protos_TestEvent for_testing;
        perfetto_protos_TracePacket_begin_for_testing(&trace_packet.msg,
                                                      &for_testing);
        {
          struct perfetto_protos_TestEvent_TestPayload payload;
          perfetto_protos_TestEvent_begin_payload(&for_testing, &payload);
          for (size_t i = 0; i < kNumFields; i++) {
            perfetto_protos_TestEvent_TestPayload_set_cstr_str(&payload,
                                                               "ABCDEFGH");
          }
          perfetto_protos_TestEvent_end_payload(&for_testing, &payload);
        }
        perfetto_protos_TracePacket_end_for_testing(&trace_packet.msg,
                                                    &for_testing);
      }
      PerfettoDsTracerPacketEnd(&ctx, &trace_packet);
    }
    benchmark::ClobberMemory();
  }

  tracing_session.StopBlocking();
  std::vector<uint8_t> data = tracing_session.ReadBlocking();

  // Just compute the PacketSize counter.
  state.counters["PacketSize"] = static_cast<double>(DecodePacketSizes(data));
}

}  // namespace

BENCHMARK(BM_Shlib_DataSource_Disabled);
BENCHMARK(BM_Shlib_DataSource_DifferentPacketSize)->Range(1, 1000);
