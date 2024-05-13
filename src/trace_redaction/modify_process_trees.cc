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

#include "src/trace_redaction/modify_process_trees.h"

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

base::Status ModifyProcessTree::VerifyContext(const Context&) const {
  return base::OkStatus();
}

base::Status ModifyProcessTree::Transform(const Context& context,
                                          std::string* packet) const {
  protozero::ProtoDecoder decoder(*packet);

  auto process_tree =
      decoder.FindField(protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  if (!process_tree.valid()) {
    return base::OkStatus();
  }

  auto timestamp =
      decoder.FindField(protos::pbzero::TracePacket::kTimestampFieldNumber);

  protozero::HeapBuffered<protos::pbzero::TracePacket> packet_message;

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::TracePacket::kProcessTreeFieldNumber) {
      TransformProcessTree(context, timestamp, field,
                           packet_message->set_process_tree());
    } else {
      proto_util::AppendField(field, packet_message.get());
    }
  }

  packet->assign(packet_message.SerializeAsString());

  return base::OkStatus();
}

void ModifyProcessTree::TransformProcess(
    const Context&,
    const protozero::Field&,
    const protozero::Field& process,
    protos::pbzero::ProcessTree* process_tree) const {
  PERFETTO_DCHECK(process.id() ==
                  protos::pbzero::ProcessTree::kProcessesFieldNumber);
  proto_util::AppendField(process, process_tree);
}

void ModifyProcessTree::TransformThread(
    const Context&,
    const protozero::Field&,
    const protozero::Field& thread,
    protos::pbzero::ProcessTree* process_tree) const {
  PERFETTO_DCHECK(thread.id() ==
                  protos::pbzero::ProcessTree::kThreadsFieldNumber);
  proto_util::AppendField(thread, process_tree);
}

void ModifyProcessTree::TransformProcessTree(
    const Context& context,
    const protozero::Field& timestamp,
    const protozero::Field& process_tree,
    protos::pbzero::ProcessTree* message) const {
  protozero::ProtoDecoder decoder(process_tree.as_bytes());

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    switch (field.id()) {
      case protos::pbzero::ProcessTree::kProcessesFieldNumber:
        TransformProcess(context, timestamp, field, message);
        break;

      case protos::pbzero::ProcessTree::kThreadsFieldNumber:
        TransformThread(context, timestamp, field, message);
        break;

      default:
        proto_util::AppendField(field, message);
        break;
    }
  }

  // TODO(vaage): Call the handler to add extra fields to the process tree.
}

}  // namespace perfetto::trace_redaction
