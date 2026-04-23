/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/etw/disk_io_tracker.h"

#include <optional>

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/etw/etw.pbzero.h"
#include "protos/perfetto/trace/etw/etw_event.pbzero.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_compressor.h"

namespace perfetto::trace_processor {

namespace {

using protozero::ConstBytes;
using std::nullopt;
using std::optional;

enum EventType {
  kRead = 10,
  kWrite = 11,
  kReadInit = 12,
  kWriteInit = 13,
  kFlush = 14,
  kFlushInit = 15,
};

// Returns a readable description for a disk I/O event type.
const char* GetEventTypeString(EventType event_type) {
  switch (event_type) {
    case kRead:
      return "DiskRead";
    case kWrite:
      return "DiskWrite";
    case kReadInit:
      return "DiskReadInit";
    case kWriteInit:
      return "DiskWriteInit";
    case kFlush:
      return "DiskFlush";
    case kFlushInit:
      return "DiskFlushInit";
  }
  return nullptr;
}

}  // namespace

DiskIoTracker::DiskIoTracker(TraceProcessorContext* context)
    : context_(context),
      disk_number_arg_(context_->storage->InternString("Disk Number")),
      irp_flags_arg_(context_->storage->InternString("Irp Flags")),
      transfer_size_arg_(context_->storage->InternString("Transfer Size")),
      reserved_arg_(context_->storage->InternString("Reserved")),
      byte_offset_arg_(context_->storage->InternString("Byte Offset")),
      file_object_arg_(context_->storage->InternString("File Object")),
      irp_ptr_arg_(context_->storage->InternString("Irp Ptr")),
      high_res_response_time_arg_(
          context_->storage->InternString("High Res Response Time")),
      thread_id_arg_(context_->storage->InternString("Thread ID")) {}

void DiskIoTracker::ParseDiskIo(int64_t timestamp, ConstBytes blob) {
  protos::pbzero::DiskIoEtwEvent::Decoder decoder(blob);
  if (!decoder.has_opcode() || !decoder.has_irp_ptr()) {
    return;
  }
  const auto opcode = static_cast<EventType>(decoder.opcode());
  const auto irp = decoder.irp_ptr();
  UniqueTid utid =
      context_->process_tracker->GetOrCreateThread(decoder.issuing_thread_id());

  const auto disk_number =
      decoder.has_disk_number() ? optional(decoder.disk_number()) : nullopt;
  const auto irp_flags =
      decoder.has_irp_flags() ? optional(decoder.irp_flags()) : nullopt;
  const auto transfer_size =
      decoder.has_transfer_size() ? optional(decoder.transfer_size()) : nullopt;
  const auto reserved =
      decoder.has_reserved() ? optional(decoder.reserved()) : nullopt;
  const auto byte_offset =
      decoder.has_byte_offset() ? optional(decoder.byte_offset()) : nullopt;
  const auto file_object =
      decoder.has_file_object() ? optional(decoder.file_object()) : nullopt;
  const auto high_res_response_time =
      decoder.has_high_res_response_time()
          ? optional(decoder.high_res_response_time())
          : nullopt;

  SliceTracker::SetArgsCallback set_args =
      [this, disk_number, irp_flags, transfer_size, reserved, byte_offset,
       file_object, irp,
       high_res_response_time](ArgsTracker::BoundInserter* inserter) {
        inserter->AddArg(irp_ptr_arg_, Variadic::Pointer(irp));
        if (disk_number) {
          inserter->AddArg(disk_number_arg_,
                           Variadic::UnsignedInteger(*disk_number));
        }
        if (irp_flags) {
          inserter->AddArg(irp_flags_arg_, Variadic::Pointer(*irp_flags));
        }
        if (transfer_size) {
          inserter->AddArg(transfer_size_arg_,
                           Variadic::UnsignedInteger(*transfer_size));
        }
        if (reserved) {
          inserter->AddArg(reserved_arg_, Variadic::UnsignedInteger(*reserved));
        }
        if (byte_offset) {
          inserter->AddArg(byte_offset_arg_, Variadic::Integer(*byte_offset));
        }
        if (file_object) {
          inserter->AddArg(file_object_arg_, Variadic::Pointer(*file_object));
        }
        if (high_res_response_time) {
          inserter->AddArg(high_res_response_time_arg_,
                           Variadic::UnsignedInteger(*high_res_response_time));
        }
      };

  switch (opcode) {
    case kReadInit:
    case kWriteInit:
    case kFlushInit: {
      const char* event_type = GetEventTypeString(opcode);
      if (!event_type)
        return;
      StringId name = context_->storage->InternString(event_type);
      StartEvent(irp, name, timestamp, utid, std::move(set_args));
      break;
    }
    case kRead:
    case kWrite:
    case kFlush: {
      const char* end_event_type = GetEventTypeString(opcode);
      if (!end_event_type)
        return;
      StringId name = context_->storage->InternString(end_event_type);
      EndEvent(irp, name, timestamp, utid, std::move(set_args));
      break;
    }
  }
}

void DiskIoTracker::StartEvent(uint64_t irp,
                               StringId name,
                               int64_t timestamp,
                               UniqueTid utid,
                               SliceTracker::SetArgsCallback args) {
  // `track_id` controls the row the events appear in. This must be created via
  // `TrackCompressor` because slices may be partially overlapping, which is not
  // supported by the Perfetto data model as-is.
  static const auto kBlueprint = TrackCompressor::SliceBlueprint(
      "etw_diskio",
      tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint));

  const auto track_id = context_->track_compressor->InternBegin(
      kBlueprint, tracks::Dimensions(utid),
      /*cookie=*/static_cast<int64_t>(irp));

  // Begin a slice for the event.
  context_->slice_tracker->Begin(timestamp, track_id, kNullStringId, name,
                                 std::move(args));
  started_events_[irp] = {name, timestamp, utid, std::move(args)};
}

void DiskIoTracker::EndEvent(uint64_t irp,
                             StringId name,
                             int64_t end_timestamp,
                             UniqueTid utid,
                             SliceTracker::SetArgsCallback args) {
  auto started_event_it = started_events_.find(irp);
  if (!irp || started_event_it == started_events_.end()) {
    RecordEventWithoutIrp(name, end_timestamp, utid, std::move(args));
    return;
  }

  static const auto kBlueprint = TrackCompressor::SliceBlueprint(
      "etw_diskio",
      tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint));

  const auto event_name = started_event_it->second.name;

  // End the slice for this event.
  const auto track_id = context_->track_compressor->InternEnd(
      kBlueprint, tracks::Dimensions(utid),
      /*cookie=*/static_cast<int64_t>(irp));
  context_->slice_tracker->End(end_timestamp, track_id, kNullStringId,
                               event_name, std::move(args));
  started_events_.erase(started_event_it);
}

void DiskIoTracker::RecordEventWithoutIrp(StringId name,
                                          int64_t timestamp,
                                          UniqueTid utid,
                                          SliceTracker::SetArgsCallback args) {
  const int64_t duration = 0;
  const auto category = kNullStringId;

  static const auto kBlueprint = TrackCompressor::SliceBlueprint(
      "etw_diskio",
      tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint));

  const auto track_id = context_->track_compressor->InternScoped(
      kBlueprint, tracks::Dimensions(utid), timestamp, duration);

  context_->slice_tracker->Scoped(timestamp, track_id, category, name, duration,
                                  std::move(args));
}

void DiskIoTracker::OnEventsFullyExtracted() {
  for (auto& [irp, event] : started_events_) {
    RecordEventWithoutIrp(event.name, event.timestamp, event.utid,
                          std::move(event.set_args));
  }
  started_events_.clear();
}

}  // namespace perfetto::trace_processor
