/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_redaction/scrub_process_trees.h"

#include <cstdint>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {
namespace {

constexpr auto kThreadsFieldNumber =
    protos::pbzero::ProcessTree::kThreadsFieldNumber;
constexpr auto kTimestampFieldNumber =
    protos::pbzero::TracePacket::kTimestampFieldNumber;
constexpr auto kProcessTreeFieldNumber =
    protos::pbzero::TracePacket::kProcessTreeFieldNumber;
constexpr auto kProcessesFieldNumber =
    protos::pbzero::ProcessTree::kProcessesFieldNumber;

// Skips the cmdline fields.
void ClearProcessName(protozero::ConstBytes bytes,
                      protos::pbzero::ProcessTree::Process* message) {
  protozero::ProtoDecoder decoder(bytes);

  for (auto field = decoder.ReadField(); field; field = decoder.ReadField()) {
    if (field.id() !=
        protos::pbzero::ProcessTree::Process::kCmdlineFieldNumber) {
      proto_util::AppendField(field, message);
    }
  }
}

void ScrubProcess(protozero::Field field,
                  const ProcessThreadTimeline& timeline,
                  uint64_t now,
                  uint64_t uid,
                  protos::pbzero::ProcessTree* message) {
  if (field.id() != kProcessesFieldNumber) {
    PERFETTO_FATAL(
        "ScrubProcess() should only be called with a ProcessTree::Processes");
  }

  protos::pbzero::ProcessTree::Process::Decoder decoder(field.as_bytes());
  auto slice = timeline.Search(now, decoder.pid());

  if (NormalizeUid(slice.uid) == NormalizeUid(uid)) {
    proto_util::AppendField(field, message);
  } else {
    ClearProcessName(field.as_bytes(), message->add_processes());
  }
}

// The thread name is unused, but it's safer to remove it.
void ClearThreadName(protozero::ConstBytes bytes,
                     protos::pbzero::ProcessTree::Thread* message) {
  protozero::ProtoDecoder decoder(bytes);

  for (auto field = decoder.ReadField(); field; field = decoder.ReadField()) {
    if (field.id() != protos::pbzero::ProcessTree::Thread::kNameFieldNumber) {
      proto_util::AppendField(field, message);
    }
  }
}

void ScrubThread(protozero::Field field,
                 const ProcessThreadTimeline& timeline,
                 uint64_t now,
                 uint64_t uid,
                 protos::pbzero::ProcessTree* message) {
  if (field.id() != kThreadsFieldNumber) {
    PERFETTO_FATAL(
        "ScrubThread() should only be called with a ProcessTree::Threads");
  }

  protos::pbzero::ProcessTree::Thread::Decoder thread_decoder(field.as_bytes());
  auto slice = timeline.Search(now, thread_decoder.tid());

  if (NormalizeUid(slice.uid) == NormalizeUid(uid)) {
    proto_util::AppendField(field, message);
  } else {
    ClearThreadName(field.as_bytes(), message->add_threads());
  }
}

}  // namespace

base::Status ScrubProcessTrees::Transform(const Context& context,
                                          std::string* packet) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("ScrubProcessTrees: missing package uid.");
  }

  if (context.timeline == nullptr) {
    return base::ErrStatus("ScrubProcessTrees: missing timeline.");
  }

  protozero::ProtoDecoder decoder(*packet);

  if (!decoder.FindField(kProcessTreeFieldNumber).valid()) {
    return base::OkStatus();
  }

  auto timestamp_field = decoder.FindField(kTimestampFieldNumber);

  if (!timestamp_field.valid()) {
    return base::ErrStatus("ScrubProcessTrees: trace packet missing timestamp");
  }

  auto timestamp = timestamp_field.as_uint64();

  auto uid = context.package_uid.value();

  const auto& timeline = *context.timeline.get();

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  for (auto packet_field = decoder.ReadField(); packet_field.valid();
       packet_field = decoder.ReadField()) {
    if (packet_field.id() != kProcessTreeFieldNumber) {
      proto_util::AppendField(packet_field, message.get());
      continue;
    }

    auto* process_tree_message = message->set_process_tree();

    protozero::ProtoDecoder process_tree_decoder(packet_field.as_bytes());

    for (auto process_tree_field = process_tree_decoder.ReadField();
         process_tree_field.valid();
         process_tree_field = process_tree_decoder.ReadField()) {
      switch (process_tree_field.id()) {
        case kProcessesFieldNumber:
          ScrubProcess(process_tree_field, timeline, timestamp, uid,
                       process_tree_message);
          break;

        case kThreadsFieldNumber:
          ScrubThread(process_tree_field, timeline, timestamp, uid,
                      process_tree_message);
          break;

        default:
          proto_util::AppendField(process_tree_field, process_tree_message);
          break;
      }
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
