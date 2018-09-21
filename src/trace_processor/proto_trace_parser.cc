/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "src/trace_processor/proto_trace_parser.h"

#include <string.h>

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/slice_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {

// We have to handle trace_marker events of a few different types:
// 1. some random text
// 2. B|1636|pokeUserActivity
// 3. E|1636
// 4. C|1636|wq:monitor|0
bool ParseSystraceTracePoint(base::StringView str, SystraceTracePoint* out) {
  // THIS char* IS NOT NULL TERMINATED.
  const char* s = str.data();
  size_t len = str.size();

  // If str matches '[BEC]\|[0-9]+[\|\n]' set tid_length to the length of
  // the number. Otherwise return false.
  if (len < 3 || s[1] != '|')
    return false;
  if (s[0] != 'B' && s[0] != 'E' && s[0] != 'C')
    return false;
  size_t tid_length;
  for (size_t i = 2;; i++) {
    if (i >= len)
      return false;
    if (s[i] == '|' || s[i] == '\n') {
      tid_length = i - 2;
      break;
    }
    if (s[i] < '0' || s[i] > '9')
      return false;
  }

  std::string tid_str(s + 2, tid_length);
  out->tid = static_cast<uint32_t>(std::stoi(tid_str.c_str()));

  out->phase = s[0];
  switch (s[0]) {
    case 'B': {
      size_t name_index = 2 + tid_length + 1;
      out->name = base::StringView(s + name_index, len - name_index);
      return true;
    }
    case 'E': {
      return true;
    }
    case 'C': {
      size_t name_index = 2 + tid_length + 1;
      size_t name_length = 0;
      for (size_t i = name_index; i < len; i++) {
        if (s[i] == '|' || s[i] == '\n') {
          name_length = i - name_index;
          break;
        }
      }
      out->name = base::StringView(s + name_index, name_length);
      size_t value_index = name_index + name_length + 1;
      char value_str[32];
      strcpy(value_str, s + value_index);
      out->value = std::stod(value_str);
      return true;
    }
    default:
      return false;
  }
}

using protozero::ProtoDecoder;
using protozero::proto_utils::kFieldTypeLengthDelimited;

ProtoTraceParser::ProtoTraceParser(TraceProcessorContext* context)
    : context_(context),
      cpu_freq_name_id_(context->storage->InternString("cpufreq")) {}

ProtoTraceParser::~ProtoTraceParser() = default;

void ProtoTraceParser::ParseTracePacket(TraceBlobView packet) {
  ProtoDecoder decoder(packet.data(), packet.length());

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TracePacket::kProcessTreeFieldNumber: {
        const size_t fld_off = packet.offset_of(fld.data());
        ParseProcessTree(packet.slice(fld_off, fld.size()));
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcessTree(TraceBlobView pstree) {
  ProtoDecoder decoder(pstree.data(), pstree.length());

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    const size_t fld_off = pstree.offset_of(fld.data());
    switch (fld.id) {
      case protos::ProcessTree::kProcessesFieldNumber: {
        ParseProcess(pstree.slice(fld_off, fld.size()));
        break;
      }
      case protos::ProcessTree::kThreadsFieldNumber: {
        ParseThread(pstree.slice(fld_off, fld.size()));
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseThread(TraceBlobView thread) {
  ProtoDecoder decoder(thread.data(), thread.length());
  uint32_t tid = 0;
  uint32_t tgid = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessTree::Thread::kTidFieldNumber:
        tid = fld.as_uint32();
        break;
      case protos::ProcessTree::Thread::kTgidFieldNumber:
        tgid = fld.as_uint32();
        break;
      default:
        break;
    }
  }
  context_->process_tracker->UpdateThread(tid, tgid);

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcess(TraceBlobView process) {
  ProtoDecoder decoder(process.data(), process.length());

  uint32_t pid = 0;
  base::StringView process_name;

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessTree::Process::kPidFieldNumber:
        pid = fld.as_uint32();
        break;
      case protos::ProcessTree::Process::kCmdlineFieldNumber:
        if (process_name.empty())
          process_name = fld.as_string();
        break;
      default:
        break;
    }
  }
  context_->process_tracker->UpdateProcess(pid, process_name);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseFtracePacket(uint32_t cpu,
                                         uint64_t timestamp,
                                         TraceBlobView ftrace) {
  ProtoDecoder decoder(ftrace.data(), ftrace.length());
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceEvent::kSchedSwitchFieldNumber: {
        PERFETTO_DCHECK(timestamp > 0);
        const size_t fld_off = ftrace.offset_of(fld.data());
        ParseSchedSwitch(cpu, timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kCpuFrequency: {
        PERFETTO_DCHECK(timestamp > 0);
        const size_t fld_off = ftrace.offset_of(fld.data());
        ParseCpuFreq(timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      case protos::FtraceEvent::kPrintFieldNumber: {
        PERFETTO_DCHECK(timestamp > 0);
        const size_t fld_off = ftrace.offset_of(fld.data());
        ParsePrint(cpu, timestamp, ftrace.slice(fld_off, fld.size()));
        break;
      }
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseCpuFreq(uint64_t timestamp, TraceBlobView view) {
  ProtoDecoder decoder(view.data(), view.length());

  uint32_t cpu_affected = 0;
  uint32_t new_freq = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::CpuFrequencyFtraceEvent::kCpuIdFieldNumber:
        cpu_affected = fld.as_uint32();
        break;
      case protos::CpuFrequencyFtraceEvent::kStateFieldNumber:
        new_freq = fld.as_uint32();
        break;
    }
  }
  context_->sched_tracker->PushCounter(timestamp, new_freq, cpu_freq_name_id_,
                                       cpu_affected, RefType::kCPU_ID);

  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseSchedSwitch(uint32_t cpu,
                                        uint64_t timestamp,
                                        TraceBlobView sswitch) {
  ProtoDecoder decoder(sswitch.data(), sswitch.length());

  uint32_t prev_pid = 0;
  uint32_t prev_state = 0;
  base::StringView prev_comm;
  uint32_t next_pid = 0;
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::SchedSwitchFtraceEvent::kPrevPidFieldNumber:
        prev_pid = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevStateFieldNumber:
        prev_state = fld.as_uint32();
        break;
      case protos::SchedSwitchFtraceEvent::kPrevCommFieldNumber:
        prev_comm = fld.as_string();
        break;
      case protos::SchedSwitchFtraceEvent::kNextPidFieldNumber:
        next_pid = fld.as_uint32();
        break;
      default:
        break;
    }
  }
  context_->sched_tracker->PushSchedSwitch(cpu, timestamp, prev_pid, prev_state,
                                           prev_comm, next_pid);
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParsePrint(uint32_t,
                                  uint64_t timestamp,
                                  TraceBlobView print) {
  ProtoDecoder decoder(print.data(), print.length());

  base::StringView buf{};
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    if (fld.id == protos::PrintFtraceEvent::kBufFieldNumber) {
      buf = fld.as_string();
      break;
    }
  }

  SystraceTracePoint point{};
  if (!ParseSystraceTracePoint(buf, &point))
    return;

  UniqueTid utid =
      context_->process_tracker->UpdateThread(timestamp, point.tid, 0);

  switch (point.phase) {
    case 'B': {
      StringId name_id = context_->storage->InternString(point.name);
      context_->slice_tracker->Begin(timestamp, utid, 0 /*cat_id*/, name_id);
      break;
    }

    case 'E': {
      context_->slice_tracker->End(timestamp, utid);
      break;
    }

    case 'C': {
      StringId name_id = context_->storage->InternString(point.name);
      context_->sched_tracker->PushCounter(timestamp, point.value, name_id,
                                           utid, RefType::kUTID);
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

}  // namespace trace_processor
}  // namespace perfetto
