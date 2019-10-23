/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_TOKENIZER_H_

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/thread_descriptor.pbzero.h"

namespace perfetto {
namespace trace_processor {

class PacketSequenceState;
class TraceProcessorContext;
class TraceBlobView;

class TrackEventTokenizer {
 public:
  explicit TrackEventTokenizer(TraceProcessorContext* context)
      : context_(context) {}

  void TokenizeTrackDescriptorPacket(
      const protos::pbzero::TracePacket::Decoder&);
  void TokenizeProcessDescriptorPacket(
      const protos::pbzero::TracePacket::Decoder&);
  void TokenizeThreadDescriptorPacket(
      PacketSequenceState* state,
      const protos::pbzero::TracePacket::Decoder&);
  void TokenizeThreadDescriptor(
      const protos::pbzero::ThreadDescriptor::Decoder&);
  void TokenizeTrackEventPacket(PacketSequenceState* state,
                                const protos::pbzero::TracePacket::Decoder&,
                                TraceBlobView* packet,
                                int64_t packet_timestamp);

 private:
  TraceProcessorContext* context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_TOKENIZER_H_
