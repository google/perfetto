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

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "src/trace_redaction/proto_util.h"
#include "src/trace_redaction/trace_redaction_framework.h"

#include "protos/perfetto/trace/ps/process_tree.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto::trace_redaction {

namespace {

// Appends a value to the message if (and only if) the pid belongs to the target
// package.
void TryAppendPid(const Context& context,
                  const protozero::Field& timestamp,
                  const protozero::Field& pid,
                  const protozero::Field& value,
                  protozero::Message* message) {
  // All valid processes with have a time and pid/tid values. However, if
  // they're missing values, the trace is corrupt. To avoid making this work by
  // dropping too much data, drop the cmdline for all processes.
  if (!timestamp.valid() || !pid.valid()) {
    return;
  }

  auto slice = context.timeline->Search(timestamp.as_uint64(), pid.as_int32());

  // Only keep the target process cmdline.
  if (NormalizeUid(slice.uid) != NormalizeUid(context.package_uid.value())) {
    return;
  }

  proto_util::AppendField(value, message);
}

}  // namespace

base::Status ScrubProcessTrees::VerifyContext(const Context& context) const {
  if (!context.package_uid.has_value()) {
    return base::ErrStatus("ScrubProcessTrees: missing package uid.");
  }

  if (!context.timeline) {
    return base::ErrStatus("ScrubProcessTrees: missing timeline.");
  }

  return base::OkStatus();
}

void ScrubProcessTrees::TransformProcess(
    const Context& context,
    const protozero::Field& timestamp,
    const protozero::Field& process,
    protos::pbzero::ProcessTree* process_tree) const {
  protozero::ProtoDecoder decoder(process.as_bytes());

  auto pid =
      decoder.FindField(protos::pbzero::ProcessTree::Process::kPidFieldNumber);

  auto* process_message = process_tree->add_processes();

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() ==
        protos::pbzero::ProcessTree::Process::kCmdlineFieldNumber) {
      TryAppendPid(context, timestamp, pid, field, process_message);
    } else {
      proto_util::AppendField(field, process_message);
    }
  }
}

void ScrubProcessTrees::TransformThread(
    const Context& context,
    const protozero::Field& timestamp,
    const protozero::Field& thread,
    protos::pbzero::ProcessTree* process_tree) const {
  protozero::ProtoDecoder decoder(thread.as_bytes());

  auto tid =
      decoder.FindField(protos::pbzero::ProcessTree::Thread::kTidFieldNumber);

  auto* thread_message = process_tree->add_threads();

  for (auto field = decoder.ReadField(); field.valid();
       field = decoder.ReadField()) {
    if (field.id() == protos::pbzero::ProcessTree::Thread::kNameFieldNumber) {
      TryAppendPid(context, timestamp, tid, field, thread_message);
    } else {
      proto_util::AppendField(field, thread_message);
    }
  }
}

}  // namespace perfetto::trace_redaction
