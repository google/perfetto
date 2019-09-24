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

#include <stdint.h>
#include <string>
#include <vector>

#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/protozero/proto_utils.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/sched.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

// Re-encodes the given trace, converting sched events to their compact
// representation.
// Note: doesn't do bundle splitting/merging, the original trace must already
// have multi-page bundles for the re-encoding to be realistic.

namespace perfetto {
namespace compact_reencode {
namespace {

void WriteToFile(const std::string& out, const char* path) {
  PERFETTO_CHECK(!unlink(path) || errno == ENOENT);
  auto out_fd = base::OpenFile(path, O_RDWR | O_CREAT, 0666);
  if (!out_fd || base::WriteAll(out_fd.get(), out.data(), out.size()) !=
                     static_cast<ssize_t>(out.size())) {
    PERFETTO_FATAL("WriteToFile");
  }
}

static void CopyField(protozero::Message* out, const protozero::Field& field) {
  using protozero::proto_utils::ProtoWireType;
  if (field.type() == ProtoWireType::kVarInt) {
    out->AppendVarInt(field.id(), field.as_uint64());
  } else if (field.type() == ProtoWireType::kLengthDelimited) {
    out->AppendBytes(field.id(), field.as_bytes().data, field.as_bytes().size);
  } else if (field.type() == ProtoWireType::kFixed32) {
    out->AppendFixed(field.id(), field.as_uint32());
  } else if (field.type() == ProtoWireType::kFixed64) {
    out->AppendFixed(field.id(), field.as_uint64());
  } else {
    PERFETTO_FATAL("unexpected wire type");
  }
}

void ReEncodeBundle(protos::pbzero::TracePacket* packet_out,
                    const uint8_t* data,
                    size_t size) {
  protos::pbzero::FtraceEventBundle::Decoder bundle(data, size);
  auto* bundle_out = packet_out->set_ftrace_events();

  if (bundle.has_lost_events())
    bundle_out->set_lost_events(bundle.lost_events());
  if (bundle.has_cpu())
    bundle_out->set_cpu(bundle.cpu());

  static constexpr size_t kMaxElements = 2560;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_timestamp;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_prev_state;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_pid;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_prio;
  protozero::StackAllocated<protozero::PackedVarIntBuffer, kMaxElements>
      switch_next_comm_index;

  uint64_t last_switch_timestamp = 0;

  std::vector<std::string> string_table;
  auto intern = [&string_table](std::string str) {
    for (size_t i = 0; i < string_table.size(); i++) {
      if (str == string_table[i])
        return static_cast<uint32_t>(i);
    }
    size_t new_idx = string_table.size();
    string_table.push_back(str);
    return static_cast<uint32_t>(new_idx);
  };

  for (auto event_it = bundle.event(); event_it; ++event_it) {
    protos::pbzero::FtraceEvent::Decoder event(event_it->data(),
                                               event_it->size());
    if (!event.has_sched_switch()) {
      CopyField(bundle_out, *event_it);
    } else {
      switch_timestamp.Append(event.timestamp() - last_switch_timestamp);
      last_switch_timestamp = event.timestamp();

      protos::pbzero::SchedSwitchFtraceEvent::Decoder sswitch(
          event.sched_switch());

      auto iid = intern(sswitch.next_comm().ToStdString());
      switch_next_comm_index.Append(iid);

      switch_next_pid.Append(sswitch.next_pid());
      switch_next_prio.Append(sswitch.next_prio());
      switch_prev_state.Append(sswitch.prev_state());
    }
  }
  auto* compact_sched = bundle_out->set_compact_sched();

  for (const auto& s : string_table)
    compact_sched->add_switch_next_comm_table(s.data(), s.size());

  compact_sched->set_switch_timestamp(switch_timestamp);
  compact_sched->set_switch_next_comm_index(switch_next_comm_index);
  compact_sched->set_switch_next_pid(switch_next_pid);
  compact_sched->set_switch_next_prio(switch_next_prio);
  compact_sched->set_switch_prev_state(switch_prev_state);
}

std::string ReEncode(const std::string& raw) {
  protos::pbzero::Trace::Decoder trace(raw);
  protozero::HeapBuffered<protos::pbzero::Trace> output;

  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    const auto* data = packet_it->data();
    size_t size = packet_it->size();
    protozero::ProtoDecoder packet(data, size);
    protos::pbzero::TracePacket* packet_out = output->add_packet();

    for (auto field = packet.ReadField(); field.valid();
         field = packet.ReadField()) {
      if (field.id() == protos::pbzero::TracePacket::kFtraceEventsFieldNumber) {
        ReEncodeBundle(packet_out, field.data(), field.size());
      } else {
        CopyField(packet_out, field);
      }
    }
  }
  // Minor technicality: we will be a tiny bit off the real encoding since
  // we've encoded the top-level Trace & TracePacket sizes redundantly, while
  // the tracing service writes them as a minimal varint (so only a few bytes
  // off per trace packet).
  return output.SerializeAsString();
}

int Main(int argc, const char** argv) {
  if (argc < 3) {
    PERFETTO_LOG("Usage: %s input output", argv[0]);
    return 1;
  }
  const char* in_path = argv[1];
  const char* out_path = argv[2];

  std::string raw;
  if (!base::ReadFile(in_path, &raw)) {
    PERFETTO_PLOG("ReadFile");
    return 1;
  }

  std::string raw_out = ReEncode(raw);
  WriteToFile(raw_out, out_path);
  return 0;
}

}  // namespace
}  // namespace compact_reencode
}  // namespace perfetto

int main(int argc, const char** argv) {
  return perfetto::compact_reencode::Main(argc, argv);
}
