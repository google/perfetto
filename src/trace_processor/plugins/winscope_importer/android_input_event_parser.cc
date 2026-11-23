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

#include "src/trace_processor/plugins/winscope_importer/android_input_event_parser.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/base64.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/common/builtin_clock.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/native/tracing/winscope/android_input_event.pbzero.h"
#include "src/trace_processor/importers/common/arg_sink.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_to_args_parser.h"
#include "src/trace_processor/plugins/winscope_importer/winscope_proto_mapping.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/clock_synchronizer.h"

namespace perfetto::trace_processor {

using com::android::internal::pbzero::AndroidInputEvent;
using com::android::internal::pbzero::AndroidKeyEvent;
using com::android::internal::pbzero::AndroidMotionEvent;
using com::android::internal::pbzero::AndroidWindowInputDispatchEvent;
using perfetto::protos::pbzero::TracePacket;

namespace {
// android.view.MotionEvent.AXIS_X / AXIS_Y.
constexpr int32_t kAxisX = 0;
constexpr int32_t kAxisY = 1;
}  // namespace

AndroidInputEventParser::AndroidInputEventParser(TraceProcessorContext* context)
    : context_(*context),
      args_parser_{*context->descriptor_pool_,
                   *context->storage->mutable_string_pool()} {}

void AndroidInputEventParser::ParseAndroidInputEvent(
    int64_t packet_ts,
    const protozero::ConstBytes& bytes) {
  auto input_event = AndroidInputEvent::Decoder(bytes);

  constexpr static auto supported_fields = std::array{
      AndroidInputEvent::kDispatcherMotionEventFieldNumber,
      AndroidInputEvent::kDispatcherMotionEventRedactedFieldNumber,
      AndroidInputEvent::kDispatcherKeyEventFieldNumber,
      AndroidInputEvent::kDispatcherKeyEventRedactedFieldNumber,
      AndroidInputEvent::kDispatcherWindowDispatchEventFieldNumber,
      AndroidInputEvent::kDispatcherWindowDispatchEventRedactedFieldNumber};

  for (auto sub_field_id : supported_fields) {
    auto sub_field = input_event.Get(static_cast<uint32_t>(sub_field_id));
    if (!sub_field.valid())
      continue;

    switch (sub_field_id) {
      case AndroidInputEvent::kDispatcherMotionEventFieldNumber:
        ParseMotionEvent(packet_ts, sub_field.as_bytes(), /*redacted=*/false);
        return;
      case AndroidInputEvent::kDispatcherMotionEventRedactedFieldNumber:
        ParseMotionEvent(packet_ts, sub_field.as_bytes(), /*redacted=*/true);
        return;
      case AndroidInputEvent::kDispatcherKeyEventFieldNumber:
      case AndroidInputEvent::kDispatcherKeyEventRedactedFieldNumber:
        ParseKeyEvent(packet_ts, sub_field.as_bytes());
        return;
      case AndroidInputEvent::kDispatcherWindowDispatchEventFieldNumber:
      case AndroidInputEvent::kDispatcherWindowDispatchEventRedactedFieldNumber:
        ParseWindowDispatchEvent(packet_ts, sub_field.as_bytes());
        return;
    }
  }
}

void AndroidInputEventParser::ParseMotionEvent(
    int64_t packet_ts,
    const protozero::ConstBytes& bytes,
    bool redacted) {
  AndroidMotionEvent::Decoder event_proto(bytes);
  tables::AndroidMotionEventsTable::Row event_row;
  event_row.event_id = event_proto.event_id();
  event_row.ts = packet_ts;
  event_row.base64_proto_id =
      context_.storage->mutable_string_pool()
          ->InternString(
              base::StringView(base::Base64Encode(bytes.data, bytes.size)))
          .raw_id();
  event_row.source = event_proto.source();
  event_row.action = event_proto.action();
  event_row.device_id = event_proto.device_id();
  event_row.display_id = event_proto.display_id();
  if (event_proto.has_event_time_nanos()) {
    event_row.event_ts = MonotonicToTraceTime(event_proto.event_time_nanos());
  }
  event_row.is_redacted = redacted ? 1 : 0;

  auto event_row_id = context_.storage->mutable_android_motion_events_table()
                          ->Insert(event_row)
                          .id;

  // Decode the pointer coordinates here so consumers don't need to pick them
  // out of the args. A redacted event has the axes but no values, so its
  // coordinates stay NULL.
  auto* pointer_table =
      context_.storage->mutable_android_motion_event_pointer_table();
  uint32_t pointer_index = 0;
  for (auto it = event_proto.pointer(); it; ++it, ++pointer_index) {
    AndroidMotionEvent::Pointer::Decoder pointer(*it);
    tables::AndroidMotionEventPointerTable::Row pointer_row;
    pointer_row.motion_event_id = event_row_id;
    pointer_row.pointer_index = pointer_index;
    if (pointer.has_pointer_id()) {
      pointer_row.pointer_id = pointer.pointer_id();
    }
    if (pointer.has_tool_type()) {
      pointer_row.tool_type = pointer.tool_type();
    }
    for (auto axis_it = pointer.axis_value(); axis_it; ++axis_it) {
      AndroidMotionEvent::Pointer::AxisValue::Decoder axis_value(*axis_it);
      if (!axis_value.has_value()) {
        continue;
      }
      switch (axis_value.axis()) {
        case kAxisX:
          pointer_row.x = static_cast<double>(axis_value.value());
          break;
        case kAxisY:
          pointer_row.y = static_cast<double>(axis_value.value());
          break;
        default:
          break;
      }
    }
    pointer_table->Insert(pointer_row);
  }
  auto inserter = context_.args_tracker->AddArgsTo(event_row_id);
  util::ProtoArgsEnv env(packet_ts, context_.process_tracker.get());
  ArgsInserterSink writer(inserter);

  base::Status status =
      args_parser_.ParseMessage(bytes,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::AndroidMotionEventsTable::Name()),
                                nullptr /*parse all fields*/, env, writer);
  if (!status.ok()) {
    context_.stats_tracker->IncrementStats(
        stats::android_input_event_parse_errors);
  }

  TryConvertMonotonicTimestampFields(event_proto.event_time_nanos(),
                                     event_proto.down_time_nanos(), writer);
}

void AndroidInputEventParser::ParseKeyEvent(
    int64_t packet_ts,
    const protozero::ConstBytes& bytes) {
  AndroidKeyEvent::Decoder event_proto(bytes);
  tables::AndroidKeyEventsTable::Row event_row;
  event_row.event_id = event_proto.event_id();
  event_row.ts = packet_ts;
  event_row.base64_proto_id =
      context_.storage->mutable_string_pool()
          ->InternString(
              base::StringView(base::Base64Encode(bytes.data, bytes.size)))
          .raw_id();
  event_row.source = event_proto.source();
  event_row.action = event_proto.action();
  event_row.device_id = event_proto.device_id();
  event_row.display_id = event_proto.display_id();
  event_row.key_code = event_proto.key_code();

  auto event_row_id = context_.storage->mutable_android_key_events_table()
                          ->Insert(event_row)
                          .id;
  auto inserter = context_.args_tracker->AddArgsTo(event_row_id);
  util::ProtoArgsEnv env(packet_ts, context_.process_tracker.get());
  ArgsInserterSink writer(inserter);

  base::Status status =
      args_parser_.ParseMessage(bytes,
                                *util::winscope_proto_mapping::GetProtoName(
                                    tables::AndroidKeyEventsTable::Name()),
                                nullptr /*parse all fields*/, env, writer);
  if (!status.ok()) {
    context_.stats_tracker->IncrementStats(
        stats::android_input_event_parse_errors);
  }

  TryConvertMonotonicTimestampFields(event_proto.event_time_nanos(),
                                     event_proto.down_time_nanos(), writer);
}

void AndroidInputEventParser::ParseWindowDispatchEvent(
    int64_t packet_ts,
    const protozero::ConstBytes& bytes) {
  AndroidWindowInputDispatchEvent::Decoder event_proto(bytes);
  tables::AndroidInputEventDispatchTable::Row event_row;
  event_row.event_id = event_proto.event_id();
  event_row.vsync_id = event_proto.vsync_id();
  event_row.window_id = event_proto.window_id();
  event_row.base64_proto_id =
      context_.storage->mutable_string_pool()
          ->InternString(
              base::StringView(base::Base64Encode(bytes.data, bytes.size)))
          .raw_id();

  auto event_row_id =
      context_.storage->mutable_android_input_event_dispatch_table()
          ->Insert(event_row)
          .id;

  // The producer writes x/y_in_display only when the display's raw transform
  // changes them, so absent values mean "same as the motion event's".
  auto* pointer_table =
      context_.storage->mutable_android_input_event_dispatch_pointer_table();
  uint32_t pointer_index = 0;
  for (auto it = event_proto.dispatched_pointer(); it; ++it, ++pointer_index) {
    AndroidWindowInputDispatchEvent::DispatchedPointer::Decoder pointer(*it);
    tables::AndroidInputEventDispatchPointerTable::Row pointer_row;
    pointer_row.dispatch_id = event_row_id;
    pointer_row.pointer_index = pointer_index;
    if (pointer.has_pointer_id()) {
      pointer_row.pointer_id = pointer.pointer_id();
    }
    if (pointer.has_x_in_display()) {
      pointer_row.x_in_display = static_cast<double>(pointer.x_in_display());
    }
    if (pointer.has_y_in_display()) {
      pointer_row.y_in_display = static_cast<double>(pointer.y_in_display());
    }
    pointer_table->Insert(pointer_row);
  }

  auto inserter = context_.args_tracker->AddArgsTo(event_row_id);
  util::ProtoArgsEnv env(packet_ts, context_.process_tracker.get());
  ArgsInserterSink writer(inserter);

  base::Status status = args_parser_.ParseMessage(
      bytes,
      *util::winscope_proto_mapping::GetProtoName(
          tables::AndroidInputEventDispatchTable::Name()),
      nullptr /*parse all fields*/, env, writer);
  if (!status.ok())
    context_.stats_tracker->IncrementStats(
        stats::android_input_event_parse_errors);
}

void AndroidInputEventParser::TryConvertMonotonicTimestampFields(
    std::optional<int64_t> event_time_nanos,
    std::optional<int64_t> down_time_nanos,
    ArgSink& writer) {
  if (event_time_nanos.has_value()) {
    ConvertMonotonicTimestampField(event_time_nanos.value(), KERNEL_TIME_KEY,
                                   writer);
  }
  if (down_time_nanos.has_value()) {
    ConvertMonotonicTimestampField(down_time_nanos.value(), DOWN_TIME_KEY,
                                   writer);
  }
}

std::optional<int64_t> AndroidInputEventParser::MonotonicToTraceTime(
    int64_t monotonic_time) {
  return context_.clock_tracker->ToTraceTime(
      ClockId::Machine(perfetto::protos::pbzero::BUILTIN_CLOCK_MONOTONIC),
      monotonic_time);
}

void AndroidInputEventParser::ConvertMonotonicTimestampField(
    int64_t monotonic_time,
    const std::string& key_string,
    ArgSink& writer) {
  auto boottime = MonotonicToTraceTime(monotonic_time);
  if (boottime.has_value()) {
    auto key = context_.storage->InternString(base::StringView(key_string));
    writer.Add({key, key}, Variadic::Integer(boottime.value()));
  }
}

}  // namespace perfetto::trace_processor
