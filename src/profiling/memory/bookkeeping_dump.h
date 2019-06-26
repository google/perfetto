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

#ifndef SRC_PROFILING_MEMORY_BOOKKEEPING_DUMP_H_
#define SRC_PROFILING_MEMORY_BOOKKEEPING_DUMP_H_

#include <functional>
#include <set>

#include <inttypes.h>

#include "perfetto/trace/interned_data/interned_data.pbzero.h"
#include "perfetto/trace/profiling/profile_common.pbzero.h"
#include "perfetto/trace/profiling/profile_packet.pbzero.h"
#include "perfetto/trace/trace_packet.pbzero.h"

#include "perfetto/ext/tracing/core/trace_writer.h"

#include "src/profiling/memory/bookkeeping.h"
#include "src/profiling/memory/interner.h"

namespace perfetto {
namespace profiling {

class DumpState {
 public:
  DumpState(TraceWriter* trace_writer) : trace_writer_(trace_writer) {
    MakeTracePacket();

    constexpr const uint8_t kEmptyString[] = "";
    // Explicitly reserve intern ID 0 for the empty string, so unset string
    // fields get mapped to this.
    auto interned_string = GetCurrentInternedData()->add_build_ids();
    interned_string->set_iid(0);
    interned_string->set_str(kEmptyString, 0);

    interned_string = GetCurrentInternedData()->add_mapping_paths();
    interned_string->set_iid(0);
    interned_string->set_str(kEmptyString, 0);

    interned_string = GetCurrentInternedData()->add_function_names();
    interned_string->set_iid(0);
    interned_string->set_str(kEmptyString, 0);
  }

  void StartDump() { MakeProfilePacket(); }

  void StartProcessDump(
      std::function<void(protos::pbzero::ProfilePacket::ProcessHeapSamples*)>
          fill_process_header);

  void AddIdleBytes(uintptr_t callstack_id, uint64_t bytes);

  void WriteAllocation(const HeapTracker::CallstackAllocations& alloc);
  void DumpCallstacks(GlobalCallstackTrie* callsites);
  void RejectConcurrent(pid_t pid);
  void Finalize() { current_trace_packet_ = TraceWriter::TracePacketHandle(); }

 private:
  void WriteMap(const Interned<Mapping> map);
  void WriteFrame(const Interned<Frame> frame);
  void WriteBuildIDString(const Interned<std::string>& str);
  void WriteMappingPathString(const Interned<std::string>& str);
  void WriteFunctionNameString(const Interned<std::string>& str);

  void MakeTracePacket() {
    last_written_ = trace_writer_->written();

    if (current_trace_packet_)
      current_trace_packet_->Finalize();
    current_trace_packet_ = trace_writer_->NewTracePacket();
    current_trace_packet_->set_timestamp(
        static_cast<uint64_t>(base::GetBootTimeNs().count()));
    current_profile_packet_ = nullptr;
    current_interned_data_ = nullptr;
    current_process_heap_samples_ = nullptr;
  }

  void MakeProfilePacket() {
    MakeTracePacket();

    current_profile_packet_ = current_trace_packet_->set_profile_packet();
    current_profile_packet_->set_index(next_index_++);
  }

  uint64_t currently_written() {
    return trace_writer_->written() - last_written_;
  }

  protos::pbzero::ProfilePacket::ProcessHeapSamples*
  GetCurrentProcessHeapSamples();
  protos::pbzero::InternedData* GetCurrentInternedData();

  std::set<InternID> dumped_strings_;
  std::set<InternID> dumped_frames_;
  std::set<InternID> dumped_mappings_;
  std::set<uint64_t> dumped_callstacks_;

  std::set<GlobalCallstackTrie::Node*> callstacks_to_dump_;

  TraceWriter* trace_writer_;

  protos::pbzero::ProfilePacket* current_profile_packet_ = nullptr;
  protos::pbzero::InternedData* current_interned_data_ = nullptr;
  TraceWriter::TracePacketHandle current_trace_packet_;
  protos::pbzero::ProfilePacket::ProcessHeapSamples*
      current_process_heap_samples_ = nullptr;

  std::function<void(protos::pbzero::ProfilePacket::ProcessHeapSamples*)>
      current_process_fill_header_;

  std::map<uintptr_t /* callstack_id */, uint64_t> current_process_idle_allocs_;

  uint64_t next_index_ = 0;
  uint64_t last_written_ = 0;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_MEMORY_BOOKKEEPING_DUMP_H_
