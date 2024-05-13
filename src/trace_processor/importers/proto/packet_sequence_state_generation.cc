/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include <cstddef>

#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

PacketSequenceStateGeneration::PacketSequenceStateGeneration(
    PacketSequenceState* state,
    PacketSequenceStateGeneration* prev_gen,
    TraceBlobView defaults)
    : state_(state),
      interned_data_(prev_gen->interned_data_),
      trace_packet_defaults_(InternedMessageView(std::move(defaults))),
      trackers_(prev_gen->trackers_) {
  for (auto& t : trackers_) {
    if (t.get() != nullptr) {
      t->set_generation(this);
    }
  }
}

PacketSequenceStateGeneration::InternedDataTracker::~InternedDataTracker() =
    default;

bool PacketSequenceStateGeneration::pid_and_tid_valid() const {
  return state_->pid_and_tid_valid();
}
int32_t PacketSequenceStateGeneration::pid() const {
  return state_->pid();
}
int32_t PacketSequenceStateGeneration::tid() const {
  return state_->tid();
}

TraceProcessorContext* PacketSequenceStateGeneration::GetContext() const {
  return state_->context();
}

void PacketSequenceStateGeneration::InternMessage(uint32_t field_id,
                                                  TraceBlobView message) {
  constexpr auto kIidFieldNumber = 1;

  uint64_t iid = 0;
  auto message_start = message.data();
  auto message_size = message.length();
  protozero::ProtoDecoder decoder(message_start, message_size);

  auto field = decoder.FindField(kIidFieldNumber);
  if (PERFETTO_UNLIKELY(!field)) {
    PERFETTO_DLOG("Interned message without interning_id");
    state_->context()->storage->IncrementStats(
        stats::interned_data_tokenizer_errors);
    return;
  }
  iid = field.as_uint64();

  auto res = interned_data_[field_id].emplace(
      iid, InternedMessageView(std::move(message)));

  // If a message with this ID is already interned in the same generation,
  // its data should not have changed (this is forbidden by the InternedData
  // proto).
  // TODO(eseckler): This DCHECK assumes that the message is encoded the
  // same way if it is re-emitted.
  PERFETTO_DCHECK(res.second ||
                  (res.first->second.message().length() == message_size &&
                   memcmp(res.first->second.message().data(), message_start,
                          message_size) == 0));
}

InternedMessageView* PacketSequenceStateGeneration::GetInternedMessageView(
    uint32_t field_id,
    uint64_t iid) {
  auto field_it = interned_data_.find(field_id);
  if (field_it != interned_data_.end()) {
    auto* message_map = &field_it->second;
    auto it = message_map->find(iid);
    if (it != message_map->end()) {
      return &it->second;
    }
  }
  state_->context()->storage->IncrementStats(
      stats::interned_data_tokenizer_errors);
  return nullptr;
}

int64_t PacketSequenceStateGeneration::IncrementAndGetTrackEventTimeNs(
    int64_t delta_ns) {
  return state_->IncrementAndGetTrackEventTimeNs(delta_ns);
}
int64_t PacketSequenceStateGeneration::IncrementAndGetTrackEventThreadTimeNs(
    int64_t delta_ns) {
  return state_->IncrementAndGetTrackEventThreadTimeNs(delta_ns);
}
int64_t
PacketSequenceStateGeneration::IncrementAndGetTrackEventThreadInstructionCount(
    int64_t delta) {
  return state_->IncrementAndGetTrackEventThreadInstructionCount(delta);
}
bool PacketSequenceStateGeneration::track_event_timestamps_valid() const {
  return state_->track_event_timestamps_valid();
}
void PacketSequenceStateGeneration::SetThreadDescriptor(
    int32_t pid,
    int32_t tid,
    int64_t timestamp_ns,
    int64_t thread_timestamp_ns,
    int64_t thread_instruction_count) {
  state_->SetThreadDescriptor(pid, tid, timestamp_ns, thread_timestamp_ns,
                              thread_instruction_count);
}

bool PacketSequenceStateGeneration::IsIncrementalStateValid() const {
  return state_->IsIncrementalStateValid();
}

}  // namespace trace_processor
}  // namespace perfetto
