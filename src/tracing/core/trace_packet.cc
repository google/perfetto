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

#include "src/tracing/core/sliced_protobuf_input_stream.h"

#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {

TracePacket::TracePacket() = default;
TracePacket::~TracePacket() = default;

TracePacket::TracePacket(TracePacket&&) noexcept = default;
TracePacket& TracePacket::operator=(TracePacket&&) = default;

bool TracePacket::Decode() {
  if (decoded_packet_)
    return true;
  decoded_packet_.reset(new DecodedTracePacket());
  SlicedProtobufInputStream istr(&slices_);
  if (!decoded_packet_->ParseFromZeroCopyStream(&istr)) {
    decoded_packet_.reset();
    return false;
  }
  return true;
}

void TracePacket::AddSlice(Slice slice) {
  size_ += slice.size;
  slices_.push_back(std::move(slice));
}

void TracePacket::AddSlice(const void* start, size_t size) {
  size_ += size;
  slices_.emplace_back(start, size);
}

}  // namespace perfetto
