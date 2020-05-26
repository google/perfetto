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

#include <stdint.h>

#include "src/trace_processor/trace_storage.h"

namespace perfetto {

namespace protos {
namespace pbzero {
class ThreadDescriptor_Decoder;
class TracePacket_Decoder;
}  // namespace pbzero
}  // namespace protos

namespace trace_processor {

class PacketSequenceState;
class TraceProcessorContext;
class TraceBlobView;

class TrackEventTokenizer {
 public:
  explicit TrackEventTokenizer(TraceProcessorContext* context);

  void TokenizeTrackDescriptorPacket(
      const protos::pbzero::TracePacket_Decoder&);
  void TokenizeProcessDescriptorPacket(
      const protos::pbzero::TracePacket_Decoder&);
  void TokenizeThreadDescriptorPacket(
      PacketSequenceState* state,
      const protos::pbzero::TracePacket_Decoder&);
  void TokenizeThreadDescriptor(
      const protos::pbzero::ThreadDescriptor_Decoder&);
  void TokenizeTrackEventPacket(PacketSequenceState* state,
                                const protos::pbzero::TracePacket_Decoder&,
                                TraceBlobView* packet,
                                int64_t packet_timestamp);

 private:
  TraceProcessorContext* context_;

  std::array<StringId, 9> process_name_ids_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_TRACK_EVENT_TOKENIZER_H_
