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

#include "perfetto/public/abi/track_event_abi.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/trace/track_event/track_event.pzc.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/track_event.h"

#include <stdio.h>
#include <unistd.h>

#define EXAMPLE_CATEGORIES(C)                                   \
  C(rendering, "rendering", "Rendering events", "tag1", "tag2") \
  C(physics, "physics", "Physics events", "tag1")               \
  C(cat, "cat", "Sample category")                              \
  C(c3, "c3", "c3", "tag1", "tag2", "tag3")                     \
  C(c4, "c4", "c4", "tag1", "tag2", "tag3", "tag4")

PERFETTO_TE_CATEGORIES_DEFINE(EXAMPLE_CATEGORIES)

static struct PerfettoTeRegisteredTrack mytrack;
static struct PerfettoTeRegisteredTrack mycounter;

static void EnabledCb(struct PerfettoTeCategoryImpl* c,
                      PerfettoDsInstanceIndex inst_id,
                      bool enabled,
                      bool global_state_changed,
                      void* user_arg) {
  printf("Callback: %p id: %u on: %d, global_state_changed: %d, user_arg:%p\n",
         PERFETTO_STATIC_CAST(void*, c), inst_id,
         PERFETTO_STATIC_CAST(int, enabled),
         PERFETTO_STATIC_CAST(int, global_state_changed), user_arg);
  if (enabled) {
    PERFETTO_TE(physics, PERFETTO_TE_INSTANT("callback"), PERFETTO_TE_FLUSH());
  }
}

int main(void) {
  uint64_t flow_counter = 0;
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  args.backends = PERFETTO_BACKEND_SYSTEM;
  PerfettoProducerInit(args);
  PerfettoTeInit();
  PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);
  PerfettoTeNamedTrackRegister(&mytrack, "mytrack", 0,
                               PerfettoTeProcessTrackUuid());
  PerfettoTeCounterTrackRegister(&mycounter, "mycounter",
                                 PerfettoTeProcessTrackUuid());
  PerfettoTeCategorySetCallback(&physics, EnabledCb, PERFETTO_NULL);
  for (;;) {
    PERFETTO_TE(rendering, PERFETTO_TE_INSTANT("name1"));
    PERFETTO_TE(physics, PERFETTO_TE_INSTANT("name2"),
                PERFETTO_TE_ARG_BOOL("dbg_arg", false),
                PERFETTO_TE_ARG_STRING("dbg_arg2", "mystring"));
    PERFETTO_TE(cat, PERFETTO_TE_SLICE_BEGIN("name"));
    PERFETTO_TE(cat, PERFETTO_TE_SLICE_END());
    flow_counter++;
    PERFETTO_TE(physics, PERFETTO_TE_SLICE_BEGIN("name4"),
                PERFETTO_TE_REGISTERED_TRACK(&mytrack),
                PERFETTO_TE_FLOW(PerfettoTeProcessScopedFlow(flow_counter)));
    PERFETTO_TE(physics, PERFETTO_TE_SLICE_END(),
                PERFETTO_TE_REGISTERED_TRACK(&mytrack));
    PERFETTO_TE(cat, PERFETTO_TE_INSTANT("name5"),
                PERFETTO_TE_TIMESTAMP(PerfettoTeGetTimestamp()));
    PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_INSTANT("name6"),
                PERFETTO_TE_DYNAMIC_CATEGORY_STRING("physics"),
                PERFETTO_TE_TERMINATING_FLOW(
                    PerfettoTeProcessScopedFlow(flow_counter)));
    PERFETTO_TE(physics, PERFETTO_TE_COUNTER(),
                PERFETTO_TE_REGISTERED_TRACK(&mycounter),
                PERFETTO_TE_INT_COUNTER(79));
    PERFETTO_TE(physics, PERFETTO_TE_INSTANT("name8"),
                PERFETTO_TE_NAMED_TRACK("dynamictrack", 2,
                                        PerfettoTeProcessTrackUuid()),
                PERFETTO_TE_TIMESTAMP(PerfettoTeGetTimestamp()));
    PERFETTO_TE(physics, PERFETTO_TE_INSTANT("name9"),
                PERFETTO_TE_PROTO_FIELDS(PERFETTO_TE_PROTO_FIELD_NESTED(
                    perfetto_protos_TrackEvent_source_location_field_number,
                    PERFETTO_TE_PROTO_FIELD_CSTR(2, __FILE__),
                    PERFETTO_TE_PROTO_FIELD_VARINT(4, __LINE__))));
    PERFETTO_TE(
        physics, PERFETTO_TE_COUNTER(),
        PERFETTO_TE_COUNTER_TRACK("mycounter", PerfettoTeProcessTrackUuid()),
        PERFETTO_TE_INT_COUNTER(89));
    PERFETTO_TE(PERFETTO_TE_DYNAMIC_CATEGORY, PERFETTO_TE_COUNTER(),
                PERFETTO_TE_DOUBLE_COUNTER(3.14),
                PERFETTO_TE_REGISTERED_TRACK(&mycounter),
                PERFETTO_TE_DYNAMIC_CATEGORY_STRING("physics"));
    sleep(1);
  }
}
