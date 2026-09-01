/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_redaction/merge_process_tree.h"

#include "perfetto/base/compiler.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

using protos::pbzero::ProcessTree;
using protos::pbzero::TracePacket;

base::Status CollectProcessTrees::Collect(const TracePacket::Decoder& packet,
                                          Context* context) const {
  if (!packet.has_process_tree()) {
    return base::OkStatus();
  }

  if (!context->merged_process_tree.has_value()) {
    context->merged_process_tree = Context::MergedProcessTree();
  }
  auto& merged_tree = context->merged_process_tree.value();

  // Collect unique processes
  ProcessTree::Decoder process_tree_decoder(packet.process_tree());
  for (auto it = process_tree_decoder.processes(); it; ++it) {
    if (PERFETTO_UNLIKELY(!merged_tree.collected_global_packet_fields)) {
      // Collect global packet fields only from the first process_tree packet.
      merged_tree.collected_global_packet_fields = true;
      merged_tree.timestamp = packet.timestamp();
      merged_tree.trusted_uid = packet.trusted_uid();
    }

    ProcessTree::Process::Decoder process_decoder(*it);
    Context::ProcessTreeProcess process;
    process.pid = process_decoder.pid();

    if (merged_tree.processes_by_pid.find(process.pid) !=
        merged_tree.processes_by_pid.end()) {
      continue;
    }

    for (auto cmdline_it = process_decoder.cmdline(); cmdline_it;
         ++cmdline_it) {
      process.cmdline.push_back(cmdline_it->as_std_string());
    }

    process.is_kthread = process_decoder.is_kthread();
    process.ppid = process_decoder.ppid();
    process.uid = process_decoder.uid();
    merged_tree.processes_by_pid[process.pid] = process;
  }

  // Collect unique threads
  for (auto it = process_tree_decoder.threads(); it; ++it) {
    ProcessTree::Thread::Decoder thread_decoder(*it);
    if (merged_tree.threads_by_tid.find(thread_decoder.tid()) !=
        merged_tree.threads_by_tid.end()) {
      continue;
    }
    Context::ProcessTreeThread thread;
    thread.tgid = thread_decoder.tgid();
    thread.tid = thread_decoder.tid();
    thread.name = thread_decoder.name().ToStdString();
    merged_tree.threads_by_tid[thread.tid] = thread;
  }

  return base::OkStatus();
}

base::Status ReduceProcessTrees::Transform(const Context&,
                                           std::string* packet) const {
  TracePacket::Decoder packet_decoder(*packet);
  if (packet_decoder.has_process_tree()) {
    // Drop every process tree, it will be added back during Augment phase.
    packet->clear();
  }
  return base::OkStatus();
}

base::Status AugmentProcessTrees::Augment(const Context& context,
                                          std::string* packet) {
  PERFETTO_DCHECK(packet);
  if (emitted_ || !context.merged_process_tree.has_value()) {
    return base::OkStatus();
  }
  const auto& merged_tree = context.merged_process_tree.value();
  if (merged_tree.processes_by_pid.empty() &&
      merged_tree.threads_by_tid.empty()) {
    return base::OkStatus();
  }

  emitted_ = true;

  protozero::HeapBuffered<protos::pbzero::TracePacket> message;
  message->set_timestamp(merged_tree.timestamp);
  message->set_trusted_uid(merged_tree.trusted_uid);

  // Add the merged process tree (processes and threads).
  auto* tree = message->set_process_tree();
  for (const auto& it : merged_tree.processes_by_pid) {
    const Context::ProcessTreeProcess& process = it.second;
    auto* process_field = tree->add_processes();
    process_field->set_pid(process.pid);
    process_field->set_ppid(process.ppid);
    process_field->set_uid(process.uid);
    process_field->set_is_kthread(process.is_kthread);
    for (const std::string& cmdline : process.cmdline) {
      process_field->add_cmdline(cmdline);
    }
  }
  for (const auto& it : merged_tree.threads_by_tid) {
    const Context::ProcessTreeThread& thread = it.second;
    auto* thread_field = tree->add_threads();
    thread_field->set_tgid(thread.tgid);
    thread_field->set_tid(thread.tid);
    if (!thread.name.empty()) {
      thread_field->set_name(thread.name);
    }
  }
  proto_util::AppendMessage(message, packet);
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
