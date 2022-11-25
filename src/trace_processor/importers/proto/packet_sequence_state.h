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
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/stack_profile_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/interned_message_view.h"

namespace perfetto {
namespace trace_processor {

class PacketSequenceState {
 public:
  explicit PacketSequenceState(TraceProcessorContext* context)
      : context_(context), sequence_stack_profile_tracker_(context) {
    current_generation_.reset(
        new PacketSequenceStateGeneration(this, generation_index_++));
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

  // Intern a message into the current generation.
  void InternMessage(uint32_t field_id, TraceBlobView message) {
    current_generation_->InternMessage(field_id, std::move(message));
  }

  // Set the trace packet defaults for the current generation. If the current
  // generation already has defaults set, starts a new generation without
  // invalidating other incremental state (such as interned data).
  void UpdateTracePacketDefaults(TraceBlobView defaults) {
    if (!current_generation_->GetTracePacketDefaultsView()) {
      current_generation_->SetTracePacketDefaults(std::move(defaults));
      return;
    }

    // The new defaults should only apply to subsequent messages on the
    // sequence. Add a new generation with the updated defaults but the
    // current generation's interned data state.
    current_generation_.reset(new PacketSequenceStateGeneration(
        this, generation_index_++, current_generation_->interned_data_,
        std::move(defaults)));
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

  void OnPacketLoss() {
    packet_loss_ = true;
    track_event_timestamps_valid_ = false;
  }

  // Starts a new generation with clean-slate incremental state and defaults.
  void OnIncrementalStateCleared() {
    packet_loss_ = false;
    current_generation_.reset(
        new PacketSequenceStateGeneration(this, generation_index_++));
  }

  bool IsIncrementalStateValid() const { return !packet_loss_; }

  SequenceStackProfileTracker& sequence_stack_profile_tracker() {
    return sequence_stack_profile_tracker_;
  }

  // Returns a ref-counted ptr to the current generation.
  RefPtr<PacketSequenceStateGeneration> current_generation() const {
    return current_generation_;
  }

  bool track_event_timestamps_valid() const {
    return track_event_timestamps_valid_;
  }

  bool pid_and_tid_valid() const { return pid_and_tid_valid_; }

  int32_t pid() const { return pid_; }
  int32_t tid() const { return tid_; }

  TraceProcessorContext* context() const { return context_; }

 private:
  TraceProcessorContext* context_;

  size_t generation_index_ = 0;

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

  RefPtr<PacketSequenceStateGeneration> current_generation_;
  SequenceStackProfileTracker sequence_stack_profile_tracker_;
};

template <uint32_t FieldId, typename MessageType>
typename MessageType::Decoder*
PacketSequenceStateGeneration::LookupInternedMessage(uint64_t iid) {
  auto* interned_message_view = GetInternedMessageView(FieldId, iid);
  if (!interned_message_view)
    return nullptr;

  return interned_message_view->template GetOrCreateDecoder<MessageType>();
}

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_PACKET_SEQUENCE_STATE_H_
