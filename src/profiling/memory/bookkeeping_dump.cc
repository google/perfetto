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

#include "src/profiling/memory/bookkeeping_dump.h"

namespace perfetto {
namespace profiling {
namespace {
using ::perfetto::protos::pbzero::Callstack;
using ::perfetto::protos::pbzero::ProfilePacket;
// This needs to be lower than the maximum acceptable chunk size, because this
// is checked *before* writing another submessage. We conservatively assume
// submessages can be up to 100k here for a 500k chunk size.
// DropBox has a 500k chunk limit, and each chunk needs to parse as a proto.
uint32_t kPacketSizeThreshold = 400000;
}  // namespace

void WriteFixedInternings(TraceWriter* trace_writer) {
  constexpr const uint8_t kEmptyString[] = "";
  // Explicitly reserve intern ID 0 for the empty string, so unset string
  // fields get mapped to this.
  auto packet = trace_writer->NewTracePacket();
  auto* interned_data = packet->set_interned_data();
  auto interned_string = interned_data->add_build_ids();
  interned_string->set_iid(0);
  interned_string->set_str(kEmptyString, 0);

  interned_string = interned_data->add_mapping_paths();
  interned_string->set_iid(0);
  interned_string->set_str(kEmptyString, 0);

  interned_string = interned_data->add_function_names();
  interned_string->set_iid(0);
  interned_string->set_str(kEmptyString, 0);
}

void DumpState::WriteMap(const Interned<Mapping> map) {
  auto map_it_and_inserted = intern_state_->dumped_mappings_.emplace(map.id());
  if (map_it_and_inserted.second) {
    for (const Interned<std::string>& str : map->path_components)
      WriteMappingPathString(str);

    WriteBuildIDString(map->build_id);

    auto mapping = GetCurrentInternedData()->add_mappings();
    mapping->set_iid(map.id());
    mapping->set_exact_offset(map->exact_offset);
    mapping->set_start_offset(map->start_offset);
    mapping->set_start(map->start);
    mapping->set_end(map->end);
    mapping->set_load_bias(map->load_bias);
    mapping->set_build_id(map->build_id.id());
    for (const Interned<std::string>& str : map->path_components)
      mapping->add_path_string_ids(str.id());
  }
}

void DumpState::WriteFrame(Interned<Frame> frame) {
  WriteMap(frame->mapping);
  WriteFunctionNameString(frame->function_name);
  bool inserted;
  std::tie(std::ignore, inserted) =
      intern_state_->dumped_frames_.emplace(frame.id());
  if (inserted) {
    auto frame_proto = GetCurrentInternedData()->add_frames();
    frame_proto->set_iid(frame.id());
    frame_proto->set_function_name_id(frame->function_name.id());
    frame_proto->set_mapping_id(frame->mapping.id());
    frame_proto->set_rel_pc(frame->rel_pc);
  }
}

void DumpState::WriteBuildIDString(const Interned<std::string>& str) {
  bool inserted;
  std::tie(std::ignore, inserted) =
      intern_state_->dumped_strings_.emplace(str.id());
  if (inserted) {
    auto interned_string = GetCurrentInternedData()->add_build_ids();
    interned_string->set_iid(str.id());
    interned_string->set_str(reinterpret_cast<const uint8_t*>(str->c_str()),
                             str->size());
  }
}

void DumpState::WriteMappingPathString(const Interned<std::string>& str) {
  bool inserted;
  std::tie(std::ignore, inserted) =
      intern_state_->dumped_strings_.emplace(str.id());
  if (inserted) {
    auto interned_string = GetCurrentInternedData()->add_mapping_paths();
    interned_string->set_iid(str.id());
    interned_string->set_str(reinterpret_cast<const uint8_t*>(str->c_str()),
                             str->size());
  }
}

void DumpState::WriteFunctionNameString(const Interned<std::string>& str) {
  bool inserted;
  std::tie(std::ignore, inserted) =
      intern_state_->dumped_strings_.emplace(str.id());
  if (inserted) {
    auto interned_string = GetCurrentInternedData()->add_function_names();
    interned_string->set_iid(str.id());
    interned_string->set_str(reinterpret_cast<const uint8_t*>(str->c_str()),
                             str->size());
  }
}

void DumpState::WriteAllocation(
    const HeapTracker::CallstackAllocations& alloc) {
  if (intern_state_->dumped_callstacks_.find(alloc.node->id()) ==
      intern_state_->dumped_callstacks_.end())
    callstacks_to_dump_.emplace(alloc.node);

  auto* heap_samples = GetCurrentProcessHeapSamples();
  ProfilePacket::HeapSample* sample = heap_samples->add_samples();
  sample->set_callstack_id(alloc.node->id());
  sample->set_self_allocated(alloc.allocated);
  sample->set_self_freed(alloc.freed);
  sample->set_alloc_count(alloc.allocation_count);
  sample->set_free_count(alloc.free_count);

  auto it = current_process_idle_allocs_.find(alloc.node->id());
  if (it != current_process_idle_allocs_.end())
    sample->set_self_idle(it->second);
}

void DumpState::DumpCallstacks(GlobalCallstackTrie* callsites) {
  // We need a way to signal to consumers when they have fully consumed the
  // InternedData they need to understand the sequence of continued
  // ProfilePackets. The way we do that is to mark the last ProfilePacket as
  // continued, then emit the InternedData, and then an empty ProfilePacket
  // to terminate the sequence.
  //
  // This is why we set_continued at the beginning of this function, and
  // MakeProfilePacket at the end.
  if (current_trace_packet_)
    current_profile_packet_->set_continued(true);
  for (GlobalCallstackTrie::Node* node : callstacks_to_dump_) {
    // There need to be two separate loops over built_callstack because
    // protozero cannot interleave different messages.
    auto built_callstack = callsites->BuildCallstack(node);
    for (const Interned<Frame>& frame : built_callstack)
      WriteFrame(frame);
    Callstack* callstack = GetCurrentInternedData()->add_callstacks();
    callstack->set_iid(node->id());
    for (const Interned<Frame>& frame : built_callstack)
      callstack->add_frame_ids(frame.id());

    intern_state_->dumped_callstacks_.emplace(node->id());
  }
  MakeProfilePacket();
}

void DumpState::AddIdleBytes(uintptr_t callstack_id, uint64_t bytes) {
  current_process_idle_allocs_[callstack_id] += bytes;
}

ProfilePacket::ProcessHeapSamples* DumpState::GetCurrentProcessHeapSamples() {
  if (currently_written() > kPacketSizeThreshold) {
    if (current_profile_packet_)
      current_profile_packet_->set_continued(true);
    MakeProfilePacket();
  }

  if (current_process_heap_samples_ == nullptr) {
    current_process_heap_samples_ =
        current_profile_packet_->add_process_dumps();
    current_process_fill_header_(current_process_heap_samples_);
  }

  return current_process_heap_samples_;
}

protos::pbzero::InternedData* DumpState::GetCurrentInternedData() {
  if (currently_written() > kPacketSizeThreshold)
    MakeTracePacket();

  if (current_interned_data_ == nullptr)
    current_interned_data_ = current_trace_packet_->set_interned_data();

  return current_interned_data_;
}

}  // namespace profiling
}  // namespace perfetto
