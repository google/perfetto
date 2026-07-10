/*
 * Copyright (C) 2024 The Android Open Source Project
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

// Minimal end-to-end example of the Perfetto C SDK using the in-process
// backend: it emits a few track events, records them into an in-process
// buffer, and writes the resulting trace to "example.pftrace", which can be
// opened directly in https://ui.perfetto.dev.
//
// This is the companion code for docs/getting-started/c-sdk.md.

#include "perfetto/public/abi/heap_buffer.h"
#include "perfetto/public/pb_msg.h"
#include "perfetto/public/producer.h"
#include "perfetto/public/protos/config/data_source_config.pzc.h"
#include "perfetto/public/protos/config/trace_config.pzc.h"
#include "perfetto/public/protos/config/track_event/track_event_config.pzc.h"
#include "perfetto/public/stream_writer.h"
#include "perfetto/public/te_category_macros.h"
#include "perfetto/public/te_macros.h"
#include "perfetto/public/tracing_session.h"
#include "perfetto/public/track_event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Declare the tracing categories this program uses.
#define EXAMPLE_CATEGORIES(C) C(rendering, "rendering", "Rendering events")

PERFETTO_TE_CATEGORIES_DEFINE(EXAMPLE_CATEGORIES)

// Builds a serialized TraceConfig proto that records the "track_event" data
// source into a 1 MiB buffer, with the "rendering" category enabled. The
// caller owns the returned buffer and must free() it.
static void* BuildTraceConfig(size_t* size) {
  struct PerfettoPbMsgWriter writer;
  struct PerfettoHeapBuffer* hb;
  struct perfetto_protos_TraceConfig cfg;
  size_t sz;
  void* buf;

  hb = PerfettoHeapBufferCreate(&writer.writer);
  PerfettoPbMsgInit(&cfg.msg, &writer);

  {
    struct perfetto_protos_TraceConfig_BufferConfig buffers;
    perfetto_protos_TraceConfig_begin_buffers(&cfg, &buffers);
    perfetto_protos_TraceConfig_BufferConfig_set_size_kb(&buffers, 1024);
    perfetto_protos_TraceConfig_end_buffers(&cfg, &buffers);
  }

  {
    struct perfetto_protos_TraceConfig_DataSource data_sources;
    perfetto_protos_TraceConfig_begin_data_sources(&cfg, &data_sources);
    {
      struct perfetto_protos_DataSourceConfig ds_cfg;
      perfetto_protos_TraceConfig_DataSource_begin_config(&data_sources,
                                                          &ds_cfg);
      perfetto_protos_DataSourceConfig_set_cstr_name(&ds_cfg, "track_event");
      {
        struct perfetto_protos_TrackEventConfig te_cfg;
        const char kCat[] = "rendering";
        perfetto_protos_DataSourceConfig_begin_track_event_config(&ds_cfg,
                                                                  &te_cfg);
        perfetto_protos_TrackEventConfig_set_enabled_categories(&te_cfg, kCat,
                                                                strlen(kCat));
        perfetto_protos_DataSourceConfig_end_track_event_config(&ds_cfg,
                                                                &te_cfg);
      }
      perfetto_protos_TraceConfig_DataSource_end_config(&data_sources, &ds_cfg);
    }
    perfetto_protos_TraceConfig_end_data_sources(&cfg, &data_sources);
  }

  sz = PerfettoStreamWriterGetWrittenSize(&writer.writer);
  buf = malloc(sz);
  PerfettoHeapBufferCopyInto(hb, &writer.writer, buf, sz);
  PerfettoHeapBufferDestroy(hb, &writer.writer);
  *size = sz;
  return buf;
}

// Invoked with successive chunks of trace data; appends them to the FILE*.
static void ReadTraceCb(struct PerfettoTracingSessionImpl* session,
                        const void* data,
                        size_t size,
                        bool has_more,
                        void* user_arg) {
  FILE* f = (FILE*)user_arg;
  size_t written = fwrite(data, 1, size, f);
  (void)session;
  (void)has_more;
  (void)written;
}

static void DrawPlayer(int player_number) {
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("DrawPlayer"),
              PERFETTO_TE_ARG_INT64("player_number", player_number));
  // ... draw the player ...
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END());
}

int main(void) {
  struct PerfettoProducerInitArgs args = PERFETTO_PRODUCER_INIT_ARGS_INIT();
  size_t cfg_size = 0;
  void* cfg;
  struct PerfettoTracingSessionImpl* session;
  FILE* f;

  // 1. Initialize the SDK with the in-process backend and register categories.
  args.backends = PERFETTO_BACKEND_IN_PROCESS;
  PerfettoProducerInit(args);
  PerfettoTeInit();
  PERFETTO_TE_REGISTER_CATEGORIES(EXAMPLE_CATEGORIES);

  // 2. Start an in-process tracing session from a serialized TraceConfig.
  cfg = BuildTraceConfig(&cfg_size);
  session = PerfettoTracingSessionCreate(PERFETTO_BACKEND_IN_PROCESS);
  PerfettoTracingSessionSetup(session, cfg, cfg_size);
  free(cfg);
  PerfettoTracingSessionStartBlocking(session);

  // 3. Run the instrumented workload.
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_BEGIN("DrawGame"));
  DrawPlayer(1);
  DrawPlayer(2);
  PERFETTO_TE(rendering, PERFETTO_TE_SLICE_END());

  // 4. Stop tracing and write the trace out to a file.
  PerfettoTracingSessionStopBlocking(session);
  f = fopen("example.pftrace", "wb");
  PerfettoTracingSessionReadTraceBlocking(session, ReadTraceCb, f);
  fclose(f);
  PerfettoTracingSessionDestroy(session);

  printf("Wrote example.pftrace\n");
  return 0;
}
