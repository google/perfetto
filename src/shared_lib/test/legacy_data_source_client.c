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

// Frozen legacy call sequence: no append-only capability setter or new packet
// begin symbol. This translation unit is compiled as C against the stable ABI.
#include <stdlib.h>
#include "perfetto/public/data_source.h"

bool PerfettoLegacyClientRegister(struct PerfettoDs*,
                                  const char*,
                                  PerfettoDsOnStartCb,
                                  void*);
void PerfettoLegacyClientBegin(struct PerfettoDsTracerIterator*,
                               struct PerfettoDsRootTracePacket*);

bool PerfettoLegacyClientRegister(struct PerfettoDs* ds,
                                  const char* name,
                                  PerfettoDsOnStartCb started,
                                  void* arg) {
  struct PerfettoPbMsgWriter writer;
  struct PerfettoHeapBuffer* heap = PerfettoHeapBufferCreate(&writer.writer);
  struct perfetto_protos_DataSourceDescriptor descriptor;
  PerfettoPbMsgInit(&descriptor.msg, &writer);
  perfetto_protos_DataSourceDescriptor_set_cstr_name(&descriptor, name);
  perfetto_protos_DataSourceDescriptor_set_will_notify_on_stop(&descriptor,
                                                               true);
  size_t size = PerfettoStreamWriterGetWrittenSize(&writer.writer);
  void* bytes = malloc(size);
  if (!bytes)
    abort();
  PerfettoHeapBufferCopyInto(heap, &writer.writer, bytes, size);
  PerfettoHeapBufferDestroy(heap, &writer.writer);
  ds->impl = PerfettoDsImplCreate();
  PerfettoDsSetOnStartCallback(ds->impl, started);
  PerfettoDsSetCbUserArg(ds->impl, arg);
  bool registered = PerfettoDsImplRegister(ds->impl, &ds->enabled, bytes, size);
  free(bytes);
  return registered;
}

void PerfettoLegacyClientBegin(struct PerfettoDsTracerIterator* iterator,
                               struct PerfettoDsRootTracePacket* root) {
  root->writer.writer = PerfettoDsTracerImplPacketBegin(iterator->impl.tracer);
  PerfettoPbMsgInit(&root->msg.msg, &root->writer);
}
