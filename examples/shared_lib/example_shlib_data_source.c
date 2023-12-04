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
