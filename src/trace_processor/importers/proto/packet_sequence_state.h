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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_H_

#include <stdint.h>

#include <unordered_map>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/stack_profile_tracker.h"
#include "src/trace_processor/trace_blob_view.h"
#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

#if PERFETTO_DCHECK_IS_ON()
// When called from GetOrCreateDecoder(), should include the stringified name of
// the MessageType.
#define PERFETTO_TYPE_IDENTIFIER PERFETTO_DEBUG_FUNCTION_IDENTIFIER()
#else  // PERFETTO_DCHECK_IS_ON()
#define PERFETTO_TYPE_IDENTIFIER nullptr
#endif  // PERFETTO_DCHECK_IS_ON()

class PacketSequenceState {
 public:
  // Entry in an interning index, refers to the interned message.
  struct InternedMessageView {
    InternedMessageView(TraceBlobView msg) : message(std::move(msg)) {}

    template <typename MessageType>
    typename MessageType::Decoder* GetOrCreateDecoder() {
      if (!decoder) {
        // Lazy init the decoder and save it away, so that we don't have to
        // reparse the message every time we access the interning entry.
        decoder = std::unique_ptr<void, std::function<void(void*)>>(
            new typename MessageType::Decoder(message.data(), message.length()),
            [](void* obj) {
              delete reinterpret_cast<typename MessageType::Decoder*>(obj);
            });
        decoder_type = PERFETTO_TYPE_IDENTIFIER;
      }
      // Verify that the type of the decoder didn't change.
      if (PERFETTO_TYPE_IDENTIFIER &&
          strcmp(decoder_type,
                 // GCC complains if this arg can be null.
                 PERFETTO_TYPE_IDENTIFIER ? PERFETTO_TYPE_IDENTIFIER : "") !=
              0) {
        PERFETTO_FATAL(
            "Interning entry accessed under different types! previous type: "
            "%s. new type: %s.",
            decoder_type, __PRETTY_FUNCTION__);
      }
      return reinterpret_cast<typename MessageType::Decoder*>(decoder.get());
    }

    TraceBlobView message;
    std::unique_ptr<void, std::function<void(void*)>> decoder;

   private:
    const char* decoder_type = nullptr;
  };

  using InternedMessageMap =
      std::unordered_map<uint64_t /*iid*/, InternedMessageView>;
  using InternedFieldMap =
      std::unordered_map<uint32_t /*field_id*/, InternedMessageMap>;
  using InternedDataGenerationList = std::vector<InternedFieldMap>;

  PacketSequenceState(TraceProcessorContext* context)
      : context_(context), stack_profile_tracker_(context) {
    interned_data_.emplace_back();
  }

  int64_t IncrementAndGetTrackEventTimeNs(int64_t delta_ns) {
    PERFETTO_DCHECK(track_event_timestamps_valid());
    track_event_timestamp_ns_ += delta_ns;
    return track_event_timestamp_ns_;
  }

  int64_t IncrementAndGetTrackEventThreadTimeNs(int64_t delta_ns) {
    PERFETTO_DCHECK(track_event_timestamps_valid());
    track_event_thread_timestamp_ns_ += delta_ns;
    return track_event_thread_timestamp_ns_;
  }

  int64_t IncrementAndGetTrackEventThreadInstructionCount(int64_t delta) {
    PERFETTO_DCHECK(track_event_timestamps_valid());
    track_event_thread_instruction_count_ += delta;
    return track_event_thread_instruction_count_;
  }

  void OnPacketLoss() {
    packet_loss_ = true;
    track_event_timestamps_valid_ = false;
  }

  void OnIncrementalStateCleared() {
    packet_loss_ = false;
    interned_data_.emplace_back();  // Bump generation number
  }

  void SetThreadDescriptor(int32_t pid,
                           int32_t tid,
                           int64_t timestamp_ns,
                           int64_t thread_timestamp_ns,
                           int64_t thread_instruction_count) {
    track_event_timestamps_valid_ = true;
    pid_and_tid_valid_ = true;
    pid_ = pid;
    tid_ = tid;
    track_event_timestamp_ns_ = timestamp_ns;
    track_event_thread_timestamp_ns_ = thread_timestamp_ns;
    track_event_thread_instruction_count_ = thread_instruction_count;
  }

  bool IsIncrementalStateValid() const { return !packet_loss_; }

  StackProfileTracker& stack_profile_tracker() {
    return stack_profile_tracker_;
  }

  // Returns the index of the current generation in the
  // InternedDataGenerationList.
  size_t current_generation() const { return interned_data_.size() - 1; }

  bool track_event_timestamps_valid() const {
    return track_event_timestamps_valid_;
  }

  bool pid_and_tid_valid() const { return pid_and_tid_valid_; }

  int32_t pid() const { return pid_; }
  int32_t tid() const { return tid_; }

  void InternMessage(uint32_t field_id, TraceBlobView message) {
    constexpr auto kIidFieldNumber = 1;

    uint64_t iid = 0;
    auto message_start = message.data();
    auto message_size = message.length();
    protozero::ProtoDecoder decoder(message_start, message_size);

    auto field = decoder.FindField(kIidFieldNumber);
    if (PERFETTO_UNLIKELY(!field)) {
      PERFETTO_DLOG("Interned message without interning_id");
      context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
      return;
    }
    iid = field.as_uint64();

    auto* map = &interned_data_.back()[field_id];
    auto res = map->emplace(iid, InternedMessageView(std::move(message)));

    // If a message with this ID is already interned in the same generation,
    // its data should not have changed (this is forbidden by the InternedData
    // proto).
    // TODO(eseckler): This DCHECK assumes that the message is encoded the
    // same way if it is re-emitted.
    PERFETTO_DCHECK(res.second ||
                    (res.first->second.message.length() == message_size &&
                     memcmp(res.first->second.message.data(), message_start,
                            message_size) == 0));
  }

  template <uint32_t FieldId, typename MessageType>
  typename MessageType::Decoder* LookupInternedMessage(size_t generation,
                                                       uint64_t iid) {
    PERFETTO_CHECK(generation <= interned_data_.size());
    auto* field_map = &interned_data_[generation];
    auto field_it = field_map->find(FieldId);
    if (field_it != field_map->end()) {
      auto* message_map = &field_it->second;
      auto it = message_map->find(iid);
      if (it != message_map->end()) {
        return it->second.GetOrCreateDecoder<MessageType>();
      }
    }
    context_->storage->IncrementStats(stats::interned_data_tokenizer_errors);
    PERFETTO_DLOG("Could not find interning entry for field ID %" PRIu32
                  ", generation %zu, and IID %" PRIu64,
                  FieldId, generation, iid);
    return nullptr;
  }

 private:
  TraceProcessorContext* context_;

  // If true, incremental state on the sequence is considered invalid until we
  // see the next packet with incremental_state_cleared. We assume that we
  // missed some packets at the beginning of the trace.
  bool packet_loss_ = true;

  // We can only consider TrackEvent delta timestamps to be correct after we
  // have observed a thread descriptor (since the last packet loss).
  bool track_event_timestamps_valid_ = false;

  // |pid_| and |tid_| are only valid after we parsed at least one
  // ThreadDescriptor packet on the sequence.
  bool pid_and_tid_valid_ = false;

  // Process/thread ID of the packet sequence set by a ThreadDescriptor
  // packet. Used as default values for TrackEvents that don't specify a
  // pid/tid override. Only valid after |pid_and_tid_valid_| is set to true.
  int32_t pid_ = 0;
  int32_t tid_ = 0;

  // Current wall/thread timestamps/counters used as reference for the next
  // TrackEvent delta timestamp.
  int64_t track_event_timestamp_ns_ = 0;
  int64_t track_event_thread_timestamp_ns_ = 0;
  int64_t track_event_thread_instruction_count_ = 0;

  InternedDataGenerationList interned_data_;
  StackProfileTracker stack_profile_tracker_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_H_
