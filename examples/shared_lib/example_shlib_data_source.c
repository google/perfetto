/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <unistd.h>

#include "perfetto/public/data_source.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"

static struct PerfettoDs custom = PERFETTO_DS_INIT();

int main(void) {
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  args.backends = PERFETTO_BACKEND_SYSTEM;
  PerfettoProducerInit(args);

  PerfettoDsRegister(&custom, "com.example.custom_data_source",
                     PerfettoDsParamsDefault());

  for (;;) {
    PERFETTO_DS_TRACE(custom, ctx) {
      struct PerfettoDsRootTracePacket root;
      PerfettoDsTracerPacketBegin(&ctx, &root);

      perfetto_protos_TracePacket_set_timestamp(&root.msg, 42);
      {
        struct perfetto_protos_TestEvent for_testing;
        perfetto_protos_TracePacket_begin_for_testing(&root.msg, &for_testing);

        perfetto_protos_TestEvent_set_cstr_str(&for_testing,
                                               "This is a long string");
        {
          struct perfetto_protos_TestEvent_TestPayload payload;
          perfetto_protos_TestEvent_begin_payload(&for_testing, &payload);

          for (int i = 0; i < 1000; i++) {
            perfetto_protos_TestEvent_TestPayload_set_cstr_str(&payload,
                                                               "nested");
          }
          perfetto_protos_TestEvent_end_payload(&for_testing, &payload);
        }
        perfetto_protos_TracePacket_end_for_testing(&root.msg, &for_testing);
      }
      PerfettoDsTracerPacketEnd(&ctx, &root);
    }
    sleep(1);
  }
}

// These headers are not needed but are pulled in to check that we don't
// accidentally slip in regressions of C++ code in public/ headers.
#include "perfetto/public/abi/atomic.h"
#include "perfetto/public/abi/backend_type.h"
#include "perfetto/public/abi/data_source_abi.h"
#include "perfetto/public/abi/export.h"
#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/abi/pb_decoder_abi.h"
#include "perfetto/public/abi/producer_abi.h"
#include "perfetto/public/abi/stream_writer_abi.h"
#include "perfetto/public/abi/thread_utils_abi.h"
#include "perfetto/public/abi/tracing_session_abi.h"
#include "perfetto/public/abi/track_event_abi.h"
#include "perfetto/public/abi/track_event_hl_abi.h"
#include "perfetto/public/abi/track_event_ll_abi.h"
#include "perfetto/public/compiler.h"
#include "perfetto/public/data_source.h"
#include "perfetto/public/fnv1a.h"
#include "perfetto/public/pb_decoder.h"
#include "perfetto/public/pb_macros.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/pb_packed.h"
#include "perfetto/public/pb_utils.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/common/builtin_clock.pzc.h"
#include "perfetto/public/protos/common/data_source_descriptor.pzc.h"
#include "perfetto/public/protos/config/data_source_config.pzc.h"
#include "perfetto/public/protos/config/trace_config.pzc.h"
#include "perfetto/public/protos/config/track_event/track_event_config.pzc.h"
#include "perfetto/public/protos/trace/android/android_track_event.pzc.h"
#include "perfetto/public/protos/trace/clock_snapshot.pzc.h"
#include "perfetto/public/protos/trace/interned_data/interned_data.pzc.h"
#include "perfetto/public/protos/trace/test_event.pzc.h"
#include "perfetto/public/protos/trace/trace.pzc.h"
#include "perfetto/public/protos/trace/trace_packet.pzc.h"
#include "perfetto/public/protos/trace/track_event/counter_descriptor.pzc.h"
#include "perfetto/public/protos/trace/track_event/debug_annotation.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_descriptor.pzc.h"
#include "perfetto/public/protos/trace/track_event/track_event.pzc.h"
#include "perfetto/public/protos/trace/trigger.pzc.h"
#include "perfetto/public/stream_writer.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/thread_utils.h"
#include "perfetto/public/tracing_session.h"
#include "perfetto/public/track_event.h"
