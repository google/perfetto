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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_

#include <optional>
#include <unordered_map>

#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/util/interned_message_view.h"

#include "protos/perfetto/trace/trace_packet_defaults.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"

namespace perfetto {
namespace trace_processor {

using InternedMessageMap =
    std::unordered_map<uint64_t /*iid*/, InternedMessageView>;
using InternedFieldMap =
    std::unordered_map<uint32_t /*field_id*/, InternedMessageMap>;

class PacketSequenceState;

class PacketSequenceStateGeneration : public RefCounted {
 public:
  // Returns |nullptr| if the message with the given |iid| was not found (also
  // records a stat in this case).
  template <uint32_t FieldId, typename MessageType>
  typename MessageType::Decoder* LookupInternedMessage(uint64_t iid);

  InternedMessageView* GetInternedMessageView(uint32_t field_id, uint64_t iid);
  // Returns |nullptr| if no defaults were set.
  InternedMessageView* GetTracePacketDefaultsView() {
    if (!trace_packet_defaults_)
      return nullptr;
    return &trace_packet_defaults_.value();
  }

  // Returns |nullptr| if no defaults were set.
  protos::pbzero::TracePacketDefaults::Decoder* GetTracePacketDefaults() {
    InternedMessageView* view = GetTracePacketDefaultsView();
    if (!view)
      return nullptr;
    return view->GetOrCreateDecoder<protos::pbzero::TracePacketDefaults>();
  }

  // Returns |nullptr| if no TrackEventDefaults were set.
  protos::pbzero::TrackEventDefaults::Decoder* GetTrackEventDefaults() {
    auto* packet_defaults_view = GetTracePacketDefaultsView();
    if (packet_defaults_view) {
      auto* track_event_defaults_view =
          packet_defaults_view
              ->GetOrCreateSubmessageView<protos::pbzero::TracePacketDefaults,
                                          protos::pbzero::TracePacketDefaults::
                                              kTrackEventDefaultsFieldNumber>();
      if (track_event_defaults_view) {
        return track_event_defaults_view
            ->GetOrCreateDecoder<protos::pbzero::TrackEventDefaults>();
      }
    }
    return nullptr;
  }

  PacketSequenceState* state() const { return state_; }
  size_t generation_index() const { return generation_index_; }

 private:
  friend class PacketSequenceState;

  PacketSequenceStateGeneration(PacketSequenceState* state,
                                size_t generation_index)
      : state_(state), generation_index_(generation_index) {}

  PacketSequenceStateGeneration(PacketSequenceState* state,
                                size_t generation_index,
                                InternedFieldMap interned_data,
                                TraceBlobView defaults)
      : state_(state),
        generation_index_(generation_index),
        interned_data_(interned_data),
        trace_packet_defaults_(InternedMessageView(std::move(defaults))) {}

  void InternMessage(uint32_t field_id, TraceBlobView message);

  void SetTracePacketDefaults(TraceBlobView defaults) {
    // Defaults should only be set once per generation.
    PERFETTO_DCHECK(!trace_packet_defaults_);
    trace_packet_defaults_ = InternedMessageView(std::move(defaults));
  }

  PacketSequenceState* state_;
  size_t generation_index_;
  InternedFieldMap interned_data_;
  std::optional<InternedMessageView> trace_packet_defaults_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_GENERATION_H_
