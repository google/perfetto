/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/tracing/core/trace_writer_for_testing.h"

#include "perfetto/base/logging.h"
#include "perfetto/base/utils.h"

#include "perfetto/protozero/message.h"

#include "perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {

TraceWriterForTesting::TraceWriterForTesting()
    : delegate_(static_cast<size_t>(base::kPageSize)), stream_(&delegate_) {
  delegate_.set_writer(&stream_);
  cur_packet_.reset(new protos::pbzero::TracePacket());
  cur_packet_->Finalize();  // To avoid the DCHECK in NewTracePacket().
}

TraceWriterForTesting::~TraceWriterForTesting() {}

void TraceWriterForTesting::Flush(std::function<void()> callback) {
  // Flush() cannot be called in the middle of a TracePacket.
  PERFETTO_CHECK(cur_packet_->is_finalized());

  if (callback)
    callback();
}

std::unique_ptr<protos::TracePacket> TraceWriterForTesting::ParseProto() {
  PERFETTO_CHECK(cur_packet_->is_finalized());
  size_t chunk_size_ = base::kPageSize;
  auto packet = std::unique_ptr<protos::TracePacket>(new protos::TracePacket());
  size_t msg_size =
      delegate_.chunks().size() * chunk_size_ - stream_.bytes_available();
  std::unique_ptr<uint8_t[]> buffer = delegate_.StitchChunks(msg_size);
  if (!packet->ParseFromArray(buffer.get(), static_cast<int>(msg_size)))
    return nullptr;
  return packet;
}

TraceWriterForTesting::TracePacketHandle
TraceWriterForTesting::NewTracePacket() {
  // If we hit this, the caller is calling NewTracePacket() without having
  // finalized the previous packet.
  PERFETTO_DCHECK(cur_packet_->is_finalized());
  cur_packet_->Reset(&stream_);
  return TraceWriter::TracePacketHandle(cur_packet_.get());
}

WriterID TraceWriterForTesting::writer_id() const {
  return 0;
}

}  // namespace perfetto
