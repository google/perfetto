/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "perfetto/tracing/core/trace_packet.h"

#include "src/tracing/core/chunked_protobuf_input_stream.h"

#include "protos/trace_packet.pb.h"

namespace perfetto {

TracePacket::TracePacket() = default;
TracePacket::~TracePacket() = default;

TracePacket::TracePacket(TracePacket&&) noexcept = default;
TracePacket& TracePacket::operator=(TracePacket&&) = default;

bool TracePacket::Decode() {
  if (decoded_packet_)
    return true;
  decoded_packet_.reset(new DecodedTracePacket());
  ChunkedProtobufInputStream istr(&chunks_);
  if (!decoded_packet_->ParseFromZeroCopyStream(&istr)) {
    decoded_packet_.reset();
    return false;
  }
  return true;
}

void TracePacket::AddChunk(Chunk chunk) {
  chunks_.push_back(chunk);
}

}  // namespace perfetto
