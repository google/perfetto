
/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <cstdint>
#include <functional>
#include <memory>

#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/ftrace/virtio_video_tracker.h"

#include "protos/perfetto/trace/ftrace/ftrace_event.pbzero.h"
#include "protos/perfetto/trace/ftrace/virtio_video.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

namespace {
using protos::pbzero::FtraceEvent;
using protos::pbzero::VirtioVideoCmdDoneFtraceEvent;
using protos::pbzero::VirtioVideoCmdFtraceEvent;
using protos::pbzero::VirtioVideoResourceQueueDoneFtraceEvent;
using protos::pbzero::VirtioVideoResourceQueueFtraceEvent;
using protozero::ConstBytes;
using TrackSetId = AsyncTrackSetTracker::TrackSetId;

/* VIRTIO_VIDEO_QUEUE_TYPE_INPUT */
constexpr uint64_t kVirtioVideoQueueTypeInput = 0x100;

/* VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT */
constexpr uint64_t kVirtioVideoQueueTypeOutput = 0x101;

constexpr int64_t kVirtioVideoCmdDuration = 100000;
}  // namespace

VirtioVideoTracker::VirtioVideoTracker(TraceProcessorContext* context)
    : context_(context),
      unknown_id_(context->storage->InternString("Unknown")),
      input_queue_id_(context->storage->InternString("INPUT")),
      output_queue_id_(context->storage->InternString("OUTPUT")),
      fields_string_ids_(*context->storage) {
  TraceStorage& storage = *context_->storage;

  command_names_.Insert(0x100, storage.InternString("QUERY_CAPABILITY"));
  command_names_.Insert(0x101, storage.InternString("STREAM_CREATE"));
  command_names_.Insert(0x102, storage.InternString("STREAM_DESTROY"));
  command_names_.Insert(0x103, storage.InternString("STREAM_DRAIN"));
  command_names_.Insert(0x104, storage.InternString("RESOURCE_CREATE"));
  command_names_.Insert(0x105, storage.InternString("RESOURCE_QUEUE"));
  command_names_.Insert(0x106, storage.InternString("RESOURCE_DESTROY_ALL"));
  command_names_.Insert(0x107, storage.InternString("QUEUE_CLEAR"));
  command_names_.Insert(0x108, storage.InternString("GET_PARAMS"));
  command_names_.Insert(0x109, storage.InternString("SET_PARAMS"));
  command_names_.Insert(0x10a, storage.InternString("QUERY_CONTROL"));
  command_names_.Insert(0x10b, storage.InternString("GET_CONTROL"));
  command_names_.Insert(0x10c, storage.InternString("SET_CONTROL"));
  command_names_.Insert(0x10d, storage.InternString("GET_PARAMS_EXT"));
  command_names_.Insert(0x10e, storage.InternString("SET_PARAMS_EXT"));
}

VirtioVideoTracker::~VirtioVideoTracker() = default;

void VirtioVideoTracker::ParseVirtioVideoEvent(uint64_t fld_id,
                                               int64_t timestamp,
                                               const ConstBytes& blob) {
  switch (fld_id) {
    case FtraceEvent::kVirtioVideoResourceQueueFieldNumber: {
      VirtioVideoResourceQueueFtraceEvent::Decoder pb_evt(blob.data, blob.size);

      uint64_t hash = base::Hasher::Combine(
          pb_evt.stream_id(), pb_evt.resource_id(), pb_evt.queue_type());

      base::StackString<64> name("Resource #%" PRIu32, pb_evt.resource_id());
      StringId name_id = context_->storage->InternString(name.string_view());

      TrackSetId track_set_id =
          InternOrCreateBufferTrack(pb_evt.stream_id(), pb_evt.queue_type());
      TrackId begin_id = context_->async_track_set_tracker->Begin(
          track_set_id, static_cast<int64_t>(hash));
      context_->slice_tracker->Begin(timestamp, begin_id, kNullStringId,
                                     name_id);
      break;
    }
    case FtraceEvent::kVirtioVideoResourceQueueDoneFieldNumber: {
      VirtioVideoResourceQueueDoneFtraceEvent::Decoder pb_evt(blob.data,
                                                              blob.size);

      uint64_t hash = base::Hasher::Combine(
          pb_evt.stream_id(), pb_evt.resource_id(), pb_evt.queue_type());

      TrackSetId track_set_id =
          InternOrCreateBufferTrack(pb_evt.stream_id(), pb_evt.queue_type());

      TrackId end_id = context_->async_track_set_tracker->End(
          track_set_id, static_cast<int64_t>(hash));
      context_->slice_tracker->End(
          timestamp, end_id, {}, {},
          [this, &pb_evt](ArgsTracker::BoundInserter* args) {
            this->AddCommandSliceArgs(&pb_evt, args);
          });
      break;
    }
    case FtraceEvent::kVirtioVideoCmdFieldNumber: {
      VirtioVideoCmdFtraceEvent::Decoder pb_evt(blob.data, blob.size);
      AddCommandSlice(timestamp, pb_evt.stream_id(), pb_evt.type(), false);
      break;
    }
    case FtraceEvent::kVirtioVideoCmdDoneFieldNumber: {
      VirtioVideoCmdDoneFtraceEvent::Decoder pb_evt(blob.data, blob.size);
      AddCommandSlice(timestamp, pb_evt.stream_id(), pb_evt.type(), true);
      break;
    }
  }
}

VirtioVideoTracker::FieldsStringIds::FieldsStringIds(TraceStorage& storage)
    : stream_id(storage.InternString("stream_id")),
      resource_id(storage.InternString("resource_id")),
      queue_type(storage.InternString("queue_type")),
      data_size0(storage.InternString("data_size0")),
      data_size1(storage.InternString("data_size1")),
      data_size2(storage.InternString("data_size2")),
      data_size3(storage.InternString("data_size3")),
      timestamp(storage.InternString("timestamp")) {}

TrackSetId VirtioVideoTracker::InternOrCreateBufferTrack(int32_t stream_id,
                                                         uint32_t queue_type) {
  const char* queue_name;

  switch (queue_type) {
    case kVirtioVideoQueueTypeInput: {
      queue_name = "INPUT";
      break;
    }
    case kVirtioVideoQueueTypeOutput: {
      queue_name = "OUTPUT";
      break;
    }
    default: {
      queue_name = "Unknown";
      break;
    }
  }

  base::StackString<64> track_name("virtio_video stream #%" PRId32 " %s",
                                   stream_id, queue_name);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());
  return context_->async_track_set_tracker->InternGlobalTrackSet(track_name_id);
}

void VirtioVideoTracker::AddCommandSlice(int64_t timestamp,
                                         uint32_t stream_id,
                                         uint64_t type,
                                         bool response) {
  const StringId* cmd_name_id = command_names_.Find(type);
  if (!cmd_name_id) {
    cmd_name_id = &unknown_id_;
  }

  const char* suffix = response ? "Responses" : "Requests";

  base::StackString<64> track_name("virtio_video stream #%" PRId32 " %s",
                                   stream_id, suffix);
  StringId track_name_id =
      context_->storage->InternString(track_name.string_view());

  TrackSetId track_set_id =
      context_->async_track_set_tracker->InternGlobalTrackSet(track_name_id);

  TrackId track_id = context_->async_track_set_tracker->Scoped(
      track_set_id, timestamp, kVirtioVideoCmdDuration);

  context_->slice_tracker->Scoped(timestamp, track_id, kNullStringId,
                                  *cmd_name_id, kVirtioVideoCmdDuration);
}

void VirtioVideoTracker::AddCommandSliceArgs(
    protos::pbzero::VirtioVideoResourceQueueDoneFtraceEvent::Decoder* pb_evt,
    ArgsTracker::BoundInserter* args) {
  StringId queue_type_id;
  switch (pb_evt->queue_type()) {
    case kVirtioVideoQueueTypeInput: {
      queue_type_id = input_queue_id_;
      break;
    }
    case kVirtioVideoQueueTypeOutput: {
      queue_type_id = output_queue_id_;
      break;
    }
    default: {
      queue_type_id = unknown_id_;
      break;
    }
  }

  args->AddArg(fields_string_ids_.stream_id,
               Variadic::Integer(pb_evt->stream_id()));
  args->AddArg(fields_string_ids_.resource_id,
               Variadic::Integer(pb_evt->resource_id()));
  args->AddArg(fields_string_ids_.queue_type, Variadic::String(queue_type_id));
  args->AddArg(fields_string_ids_.data_size0,
               Variadic::Integer(pb_evt->data_size0()));
  args->AddArg(fields_string_ids_.data_size1,
               Variadic::Integer(pb_evt->data_size1()));
  args->AddArg(fields_string_ids_.data_size2,
               Variadic::Integer(pb_evt->data_size2()));
  args->AddArg(fields_string_ids_.data_size3,
               Variadic::Integer(pb_evt->data_size3()));
  args->AddArg(fields_string_ids_.timestamp,
               Variadic::UnsignedInteger(pb_evt->timestamp()));
}

}  // namespace trace_processor
}  // namespace perfetto
