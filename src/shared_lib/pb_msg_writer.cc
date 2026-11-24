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

#include "src/shared_lib/pb_msg_writer.h"

#include <algorithm>

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/contiguous_memory_range.h"
#include "perfetto/protozero/scattered_stream_writer.h"
#include "perfetto/public/abi/pb_msg_writer_abi.h"

void PerfettoPbMsgWriterUpdateWritePtr(struct PerfettoPbMsgWriter* w) {
  auto* sw = reinterpret_cast<protozero::ScatteredStreamWriter*>(w->impl);
  sw->set_write_ptr(w->write_ptr);
}

void PerfettoPbMsgWriterNewChunk(struct PerfettoPbMsgWriter* w) {
  auto* sw = reinterpret_cast<protozero::ScatteredStreamWriter*>(w->impl);
  sw->set_write_ptr(w->write_ptr);
  sw->Extend();
  perfetto::UpdatePbMsgWriter(*sw, w);
}

uint8_t* PerfettoPbMsgWriterAnnotatePatch(struct PerfettoPbMsgWriter* w,
                                          uint8_t* patch_addr) {
  auto* sw = reinterpret_cast<protozero::ScatteredStreamWriter*>(w->impl);
  static_assert(PERFETTO_PB_MSG_WRITER_PATCH_SIZE ==
                    protozero::ScatteredStreamWriter::Delegate::kPatchSize,
                "Size mismatch");
  memset(patch_addr, 0, PERFETTO_PB_MSG_WRITER_PATCH_SIZE);
  return sw->AnnotatePatch(patch_addr);
}

void PerfettoPbMsgWriterAppendBytesSlowpath(struct PerfettoPbMsgWriter* w,
                                            const uint8_t* src,
                                            size_t size) {
  auto* sw = reinterpret_cast<protozero::ScatteredStreamWriter*>(w->impl);
  sw->set_write_ptr(w->write_ptr);
  sw->WriteBytesSlowPath(src, size);
  perfetto::UpdatePbMsgWriter(*sw, w);
}

void PerfettoPbMsgWriterReserveBytesSlowpath(struct PerfettoPbMsgWriter* w,
                                             size_t size) {
  auto* sw = reinterpret_cast<protozero::ScatteredStreamWriter*>(w->impl);
  sw->set_write_ptr(w->write_ptr);
  sw->ReserveBytes(size);
  perfetto::UpdatePbMsgWriter(*sw, w);
}
