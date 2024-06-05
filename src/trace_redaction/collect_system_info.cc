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

#include "src/trace_redaction/collect_system_info.h"

#include "perfetto/protozero/field.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"

namespace perfetto::trace_redaction {

base::Status CollectSystemInfo::Begin(Context* context) const {
  // Other primitives are allows to push more data into the system info (e.g.
  // another source of pids).
  if (!context->system_info.has_value()) {
    context->system_info.emplace();
  }

  return base::OkStatus();
}

base::Status CollectSystemInfo::Collect(
    const protos::pbzero::TracePacket::Decoder& packet,
    Context* context) const {
  auto* system_info = &context->system_info.value();

  if (!packet.has_ftrace_events()) {
    return base::OkStatus();
  }

  protozero::ProtoDecoder decoder(packet.ftrace_events());

  auto field =
      decoder.FindField(protos::pbzero::FtraceEventBundle::kCpuFieldNumber);

  if (field.valid()) {
    system_info->ReserveCpu(field.as_uint32());
  }

  return base::OkStatus();
}

base::Status BuildSyntheticThreads::Build(Context* context) const {
  if (!context->system_info.has_value()) {
    return base::ErrStatus("BuildThreadMap: missing system info.");
  }

  if (context->synthetic_threads.has_value()) {
    return base::ErrStatus(
        "BuildThreadMap: synthetic threads were already initialized.");
  }

  auto& system_info = context->system_info.value();
  auto& synthetic_threads = context->synthetic_threads.emplace();

  auto cpu_count = system_info.last_cpu() + 1;

  synthetic_threads.tgid = system_info.AllocateSynthThread();
  synthetic_threads.tids.resize(cpu_count);

  for (uint32_t cpu = 0; cpu < cpu_count; ++cpu) {
    synthetic_threads.tids[cpu] = system_info.AllocateSynthThread();
  }

  return base::OkStatus();
}

}  // namespace perfetto::trace_redaction
