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

#include <string>

#include "perfetto/base/logging.h"
#include "perfetto/base/string_view.h"
#include "perfetto/base/utils.h"
#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/blob_reader.h"
#include "src/trace_processor/process_tracker.h"
#include "src/trace_processor/sched_tracker.h"
#include "src/trace_processor/trace_processor_context.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {
namespace trace_processor {

using protozero::ProtoDecoder;
using protozero::proto_utils::kFieldTypeLengthDelimited;
using protozero::proto_utils::ParseVarInt;
using protozero::proto_utils::MakeTagVarInt;

namespace {

template <int field_id>
inline bool FindIntField(ProtoDecoder* decoder, uint64_t* field_value) {
  bool res = false;
  for (auto f = decoder->ReadField(); f.id != 0; f = decoder->ReadField()) {
    if (f.id == field_id) {
      *field_value = f.int_value;
      res = true;
      break;
    }
  }
  decoder->Reset();
  return res;
}

}  // namespace

ProtoTraceParser::ProtoTraceParser(BlobReader* reader,
                                   TraceProcessorContext* context)
    : reader_(reader), context_(context) {}

ProtoTraceParser::~ProtoTraceParser() = default;

bool ProtoTraceParser::ParseNextChunk() {
  if (!buffer_)
    buffer_.reset(new uint8_t[chunk_size_]);

  uint32_t read = reader_->Read(offset_, chunk_size_, buffer_.get());
  if (read == 0)
    return false;

  ProtoDecoder decoder(buffer_.get(), read);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    if (fld.id != protos::Trace::kPacketFieldNumber) {
      PERFETTO_ELOG("Non-trace packet field found in root Trace proto");
      continue;
    }
    ParsePacket(fld.data(), fld.size());
  }

  if (decoder.offset() == 0) {
    PERFETTO_ELOG("The trace file seems truncated, interrupting parsing");
    return false;
  }
  offset_ += decoder.offset();
  return true;
}

void ProtoTraceParser::ParsePacket(const uint8_t* data, size_t length) {
  ProtoDecoder decoder(data, length);
  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::TracePacket::kFtraceEventsFieldNumber:
        ParseFtraceEventBundle(fld.data(), fld.size());
        break;
      case protos::TracePacket::kProcessTreeFieldNumber:
        ParseProcessTree(fld.data(), fld.size());
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseProcessTree(const uint8_t* data, size_t length) {
  ProtoDecoder decoder(data, length);

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::ProcessTree::kProcessesFieldNumber:
        ParseProcess(fld.data(), fld.size());
        break;
      case protos::ProcessTree::kThreadsFieldNumber:
        ParseThread(fld.data(), fld.size());
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

void ProtoTraceParser::ParseThread(const uint8_t* data, size_t length) {
  ProtoDecoder decoder(data, length);
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

void ProtoTraceParser::ParseProcess(const uint8_t* data, size_t length) {
  ProtoDecoder decoder(data, length);
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

void ProtoTraceParser::ParseFtraceEventBundle(const uint8_t* data,
                                              size_t length) {
  ProtoDecoder decoder(data, length);
  uint64_t cpu = 0;
  constexpr auto kCpuFieldNumber = protos::FtraceEventBundle::kCpuFieldNumber;
  constexpr auto kCpuFieldTag = MakeTagVarInt(kCpuFieldNumber);

  // For speed we speculate on the location and size (<128) of the cpu field.
  // In P+ cpu is pushed as the first field.
  // In P cpu is pushed as the 2nd last field.
  if (PERFETTO_LIKELY(length > 2 && data[0] == kCpuFieldTag &&
                      data[1] < 0x80)) {
    cpu = data[1];
  } else if (PERFETTO_LIKELY(length > 4 && data[length - 4] == kCpuFieldTag) &&
             data[length - 3] < 0x80) {
    cpu = data[length - 3];
  } else {
    if (!PERFETTO_LIKELY((FindIntField<kCpuFieldNumber>(&decoder, &cpu)))) {
      PERFETTO_ELOG("CPU field not found in FtraceEventBundle");
      return;
    }
  }

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceEventBundle::kEventFieldNumber:
        ParseFtraceEvent(static_cast<uint32_t>(cpu), fld.data(), fld.size());
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

PERFETTO_ALWAYS_INLINE void ProtoTraceParser::ParseFtraceEvent(
    uint32_t cpu,
    const uint8_t* data,
    size_t length) {
  ProtoDecoder decoder(data, length);
  constexpr auto kTimestampFieldNumber =
      protos::FtraceEvent::kTimestampFieldNumber;
  uint64_t timestamp;
  bool timestamp_found = false;

  // Speculate on the fact that the timestamp is often the 1st field of the
  // event.
  constexpr auto timestampFieldTag = MakeTagVarInt(kTimestampFieldNumber);
  if (PERFETTO_LIKELY(length > 10 && data[0] == timestampFieldTag)) {
    // Fastpath.
    const uint8_t* next = ParseVarInt(data + 1, data + 10, &timestamp);
    timestamp_found = next != data + 1;
    decoder.Reset(next);
  } else {
    // Slowpath.
    timestamp_found = FindIntField<kTimestampFieldNumber>(&decoder, &timestamp);
  }

  if (PERFETTO_UNLIKELY(!timestamp_found)) {
    PERFETTO_ELOG("Timestamp field not found in FtraceEvent");
    return;
  }

  for (auto fld = decoder.ReadField(); fld.id != 0; fld = decoder.ReadField()) {
    switch (fld.id) {
      case protos::FtraceEvent::kSchedSwitchFieldNumber:
        PERFETTO_DCHECK(timestamp > 0);
        ParseSchedSwitch(cpu, timestamp, fld.data(), fld.size());
        break;
      default:
        break;
    }
  }
  PERFETTO_DCHECK(decoder.IsEndOfBuffer());
}

PERFETTO_ALWAYS_INLINE void ProtoTraceParser::ParseSchedSwitch(
    uint32_t cpu,
    uint64_t timestamp,
    const uint8_t* data,
    size_t length) {
  ProtoDecoder decoder(data, length);
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

}  // namespace trace_processor
}  // namespace perfetto
