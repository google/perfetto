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

#include "src/trace_redaction/add_synth_threads_to_process_trees.h"

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

void AddProcessToProcessTree(const Context& context,
                             protos::pbzero::ProcessTree* process_tree) {
  PERFETTO_DCHECK(context.synthetic_process);
  PERFETTO_DCHECK(context.synthetic_process->tids().size() >= 2);

  auto* process = process_tree->add_processes();
  process->set_uid(context.synthetic_process->uid());
  process->set_ppid(context.synthetic_process->ppid());
  process->set_pid(context.synthetic_process->tids().front());
  process->add_cmdline("Other-Processes");
}

void AddThreadsToProcessTree(const Context& context,
                             protos::pbzero::ProcessTree* process_tree) {
  PERFETTO_DCHECK(context.synthetic_process);

  auto& tids = context.synthetic_process->tids();
  PERFETTO_DCHECK(tids.size() >= 2);

  auto it = tids.begin();
  ++it;

  for (; it != tids.end(); ++it) {
    auto name = std::to_string(*it);
    name.insert(0, "cpu-");

    auto* thread = process_tree->add_threads();
    thread->set_tgid(context.synthetic_process->tgid());
    thread->set_tid(*it);
    thread->set_name(name);
  }
}

// Copy fields from one process tree to another process tree.
void CopyProcessTreeEntries(protozero::Field src,
                            protos::pbzero::ProcessTree* dest) {
  PERFETTO_DCHECK(src.valid());
  PERFETTO_DCHECK(src.id() ==
                  protos::pbzero::TracePacket::kProcessTreeFieldNumber);

  protozero::ProtoDecoder decoder(src.as_bytes());

  for (auto it = decoder.ReadField(); it.valid(); it = decoder.ReadField()) {
    proto_util::AppendField(it, dest);
  }
}

}  // namespace

base::Status AddSythThreadsToProcessTrees::Transform(
    const Context& context,
    std::string* packet) const {
  PERFETTO_DCHECK(packet);

  if (!context.synthetic_process) {
    return base::ErrStatus(
        "AddSythThreadsToProcessTrees: missing synthentic threads.");
  }

  if (context.synthetic_process->tids().size() <= 2) {
    return base::ErrStatus(
        "AddSythThreadsToProcessTrees: no synthentic threads in synthentic "
        "process.");
  }

  protozero::ProtoDecoder decoder(*packet);
  protozero::HeapBuffered<protos::pbzero::TracePacket> message;

  for (auto it = decoder.ReadField(); it.valid(); it = decoder.ReadField()) {
    if (it.id() == protos::pbzero::TracePacket::kProcessTreeFieldNumber) {
      auto* process_tree = message->set_process_tree();

      CopyProcessTreeEntries(it, process_tree);

      AddProcessToProcessTree(context, process_tree);
      AddThreadsToProcessTree(context, process_tree);
    } else {
      proto_util::AppendField(it, message.get());
    }
  }

  packet->assign(message.SerializeAsString());

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
