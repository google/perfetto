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

#include "perfetto/protozero/scattered_heap_buffer.h"
#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "src/trace_redaction/trace_redaction_framework.h"

namespace perfetto::trace_redaction {

using protos::pbzero::ProcessTree;
using protos::pbzero::TracePacket;

base::Status MergeProcessTree::Collect(const TracePacket::Decoder& packet,
                                       const Context*) {
  if (packet.has_process_tree()) {
    ProcessTree::Decoder process_tree_decoder(packet.process_tree());
    for (auto it = process_tree_decoder.processes(); it; ++it) {
      if (PERFETTO_UNLIKELY(!collected_global_packet_fields_)) {
        collected_global_packet_fields_ = true;
        timestamp_ = packet.timestamp();
        trusted_uid_ = packet.trusted_uid();
      }
      ProcessTree::Process::Decoder process_decoder(*it);
      ProcessTreeProcess process;
      process.pid = process_decoder.pid();
      if (processes_by_pid_.find(process.pid) != processes_by_pid_.end()) {
        continue;
      }
      for (auto cmdline_it = process_decoder.cmdline(); cmdline_it;
           ++cmdline_it) {
        process.cmdline.push_back(cmdline_it->as_std_string());
      }
      process.is_kthread = process_decoder.is_kthread();
      process.ppid = process_decoder.ppid();
      process.uid = process_decoder.uid();
      processes_by_pid_[process.pid] = process;
    }
    for (auto it = process_tree_decoder.threads(); it; ++it) {
      ProcessTree::Thread::Decoder thread_decoder(*it);
      if (threads_by_tid_.find(thread_decoder.tid()) != threads_by_tid_.end()) {
        continue;
      }
      ProcessTreeThread thread;
      thread.tgid = thread_decoder.tgid();
      thread.tid = thread_decoder.tid();
      thread.name = thread_decoder.name().ToStdString();
      threads_by_tid_[thread.tid] = thread;
    }
  }
  return base::OkStatus();
}

std::string MergeProcessTree::Augment(const Context&) {
  if (processes_by_pid_.empty() || threads_by_tid_.empty()) {
    return "";
  }
  protozero::HeapBuffered<protos::pbzero::TracePacket> message;
  message->set_timestamp(timestamp_);
  message->set_trusted_uid(trusted_uid_);

  // Add the merged process tree (processes and threads).
  auto* tree = message->set_process_tree();
  while (!processes_by_pid_.empty()) {
    auto it = processes_by_pid_.begin();
    ProcessTreeProcess process = it->second;
    auto* process_field = tree->add_processes();
    process_field->set_pid(process.pid);
    process_field->set_ppid(process.ppid);
    process_field->set_uid(process.uid);
    process_field->set_is_kthread(process.is_kthread);
    for (const std::string& cmdline : process.cmdline) {
      process_field->add_cmdline(cmdline);
    }
    processes_by_pid_.erase(it);
  }
  while (!threads_by_tid_.empty()) {
    auto it = threads_by_tid_.begin();
    ProcessTreeThread thread = it->second;
    auto* thread_field = tree->add_threads();
    thread_field->set_tgid(thread.tgid);
    thread_field->set_tid(thread.tid);
    if (!thread.name.empty()) {
      thread_field->set_name(thread.name);
    }
    threads_by_tid_.erase(it);
  }
  return message.SerializeAsString();
}

base::Status MergeProcessTree::Reduce(const Context* /*context*/,
                                      std::string* packet) const {
  TracePacket::Decoder packet_decoder(*packet);
  if (packet_decoder.has_process_tree()) {
    packet->clear();
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
