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

#include "src/trace_processor/importers/ftrace/generic_ftrace_tracker.h"

#include "perfetto/base/logging.h"
#include "perfetto/protozero/proto_utils.h"
#include "protos/perfetto/common/descriptor.pbzero.h"

#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/common/tracks.h"
#include "src/trace_processor/importers/common/tracks_common.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

using protozero::proto_utils::ProtoSchemaType;

// We do not expect tracepoints with over 32 fields. It's more likely that the
// trace is corrupted. See also |kMaxFtraceEventFields| in ftrace_descriptors.h.
static constexpr uint32_t kMaxAllowedFields = 32;

GenericFtraceTracker::GenericFtraceTracker(TraceProcessorContext* context)
    : context_(context) {}

GenericFtraceTracker::~GenericFtraceTracker() = default;

void GenericFtraceTracker::AddDescriptor(uint32_t pb_field_id,
                                         protozero::ConstBytes pb_descriptor) {
  if (events_.Find(pb_field_id))
    return;  // already added

  protos::pbzero::DescriptorProto::Decoder decoder(pb_descriptor);

  GenericEvent event;
  event.name = context_->storage->InternString(decoder.name());
  for (auto it = decoder.field(); it; ++it) {
    protos::pbzero::FieldDescriptorProto::Decoder field_decoder(it->data(),
                                                                it->size());

    uint32_t field_id = static_cast<uint32_t>(field_decoder.number());
    if (field_id >= kMaxAllowedFields) {
      PERFETTO_DLOG("Skipping generic descriptor with >32 fields.");
      context_->storage->IncrementStats(
          stats::ftrace_generic_descriptor_errors);
      return;
    }
    if (field_decoder.type() > static_cast<int32_t>(ProtoSchemaType::kSint64)) {
      PERFETTO_DLOG("Skipping generic descriptor with invalid field type.");
      context_->storage->IncrementStats(
          stats::ftrace_generic_descriptor_errors);
      return;
    }

    if (field_id >= event.fields.size()) {
      event.fields.resize(field_id + 1);
    }
    GenericField& field = event.fields[field_id];

    field.name = context_->storage->InternString(field_decoder.name());
    field.type = static_cast<ProtoSchemaType>(field_decoder.type());
  }
  MatchTrackEventTemplate(pb_field_id, event);
  events_.Insert(pb_field_id, std::move(event));
}

GenericFtraceTracker::GenericEvent* GenericFtraceTracker::GetEvent(
    uint32_t pb_field_id) {
  return events_.Find(pb_field_id);
}

void GenericFtraceTracker::MatchTrackEventTemplate(uint32_t pb_field_id,
                                                   const GenericEvent& event) {
  KernelTrackEvent info = {};
  info.event_name = event.name;
  for (uint32_t field_id = 1; field_id < event.fields.size(); field_id++) {
    const GenericField& field = event.fields[field_id];

    // TODO(prototype): intern strings in constructor, consider allowing more
    // integral types.
    if (field.name == context_->storage->InternString("track_event_type") &&
        (field.type == ProtoSchemaType::kInt64 ||
         field.type == ProtoSchemaType::kUint64)) {
      info.slice_type_field_id = field_id;
    } else if (field.name == context_->storage->InternString("slice_name") &&
               field.type == ProtoSchemaType::kString) {
      info.slice_name_field_id = field_id;
    } else if (field.name == context_->storage->InternString("track_name") &&
               field.type == ProtoSchemaType::kString) {
      info.track_name_field_id = field_id;
    } else if (field.name == context_->storage->InternString("value") &&
               (field.type == ProtoSchemaType::kInt64 ||
                field.type == ProtoSchemaType::kUint64)) {
      info.value_field_id = field_id;
    }
    // context fields: well-known names or a prefix.
    if (field.name == context_->storage->InternString("context_tgid") &&
        (field.type == ProtoSchemaType::kInt64 ||
         field.type == ProtoSchemaType::kUint64)) {
      info.context_field_id = field_id;
      info.context_type = KernelTrackEvent::kTgid;
    } else if (field.name == context_->storage->InternString("context_cpu") &&
               (field.type == ProtoSchemaType::kInt64 ||
                field.type == ProtoSchemaType::kUint64)) {
      info.context_field_id = field_id;
      info.context_type = KernelTrackEvent::kCpu;
    } else if (context_->storage->GetString(field.name)
                   .StartsWith("context_")) {
      info.context_field_id = field_id;
      info.context_type = KernelTrackEvent::kCustom;
    }
  }

  if (info.slice_type_field_id && info.slice_name_field_id) {
    info.kind = KernelTrackEvent::EventKind::kSlice;
  } else if (info.track_name_field_id && info.value_field_id) {
    info.kind = KernelTrackEvent::EventKind::kCounter;
  } else {
    // common case: tracepoint doesn't look like a kernel track event
    return;
  }

  track_event_info_.Insert(pb_field_id, info);
}

// TODO(prototype): short ver:
// * BEI events as macro#1
// * C events as macro#2
// * No support for single macro for both (i.e. no track_event_type = 'C')
//
// * No async events (have to strictly nest slices within track_name + context_)
// * All counters have to have an explicit name in every payload, no defaulting.
// * No system track merging, even for thread-scoped track events.
//
// Not yet implemented / unsure:
// * Could put trailing fields into the args table automatically.
// * Decide on how to group & surface custom-scoped tracks that aren't bound to
//   a pre-existing tid/tgid/cpu.
// * Whether we should offer a simple migration path / parsing hook for
//   checked-in versions of the events, instead of hooking onto only generic
//   events. There are legitimate use-cases where checked-in protos are
//   necessary (e.g. on-device bytecode filtering).
// * probably keeping existing "tracing_mark_write" events on the
// systrace_parser.
//
// Example macros for same-thread scoped track events and a tgid-scoped counter:
//  TRACE_EVENT(trk_example,
//      TP_PROTO(
//          char track_event_type,
//          const char *slice_name
//      ),
//      TP_ARGS(
//          track_event_type,
//          slice_name
//      ),
//      TP_STRUCT__entry(
//          __field(char, track_event_type)
//          __string(slice_name, slice_name)
//      ),
//      TP_fast_assign(
//          __entry->track_event_type = track_event_type;
//          __assign_str(slice_name);
//      ),
//      TP_printk(
//          "type=%c slice_name=%s",
//          __entry->track_event_type,
//          __get_str(slice_name)
//      )
//  );
//
//  TRACE_EVENT(tgid_cntr_example,
//      TP_PROTO(
//          const char *track_name,
//          u64 value,
//          int32_t context_tgid
//      ),
//      TP_ARGS(
//          track_name,
//          value,
//          context_tgid
//      ),
//      TP_STRUCT__entry(
//          __string(track_name, track_name)
//          __field(u64, value)
//          __field(int32_t, context_tgid)
//      ),
//      TP_fast_assign(
//          __assign_str(track_name);
//          __entry->value = value;
//          __entry->context_tgid = context_tgid;
//      ),
//      TP_printk(
//          "track_name=%s value=%llu tgid=%d",
//          __get_str(track_name),
//          (unsigned long long)__entry->value,
//          __entry->context_tgid
//      )
//  );
void GenericFtraceTracker::MaybeParseAsTrackEvent(
    uint32_t pb_field_id,
    int64_t ts,
    uint32_t tid,
    protozero::ProtoDecoder& decoder) {
  constexpr auto kThreadSliceTrackBp = tracks::SliceBlueprint(
      "kernel_trackevent_thread_slice",
      tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());
  constexpr auto kThreadCounterTrackBp = tracks::CounterBlueprint(
      "kernel_trackevent_thread_counter", tracks::UnknownUnitBlueprint(),
      tracks::DimensionBlueprints(tracks::kThreadDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());

  constexpr auto kProcessSliceTrackBp = tracks::SliceBlueprint(
      "kernel_trackevent_process_slice",
      tracks::DimensionBlueprints(tracks::kProcessDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());
  constexpr auto kProcessCounterTrackBp = tracks::CounterBlueprint(
      "kernel_trackevent_process_counter", tracks::UnknownUnitBlueprint(),
      tracks::DimensionBlueprints(tracks::kProcessDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());

  constexpr auto kCpuSliceTrackBp = tracks::SliceBlueprint(
      "kernel_trackevent_cpu_slice",
      tracks::DimensionBlueprints(tracks::kCpuDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());
  constexpr auto kCpuCounterTrackBp = tracks::CounterBlueprint(
      "kernel_trackevent_cpu_counter", tracks::UnknownUnitBlueprint(),
      tracks::DimensionBlueprints(tracks::kCpuDimensionBlueprint,
                                  tracks::StringIdDimensionBlueprint("name")),
      tracks::DynamicNameBlueprint());

  auto* maybe_info = track_event_info_.Find(pb_field_id);
  if (!maybe_info)
    return;

  const KernelTrackEvent& info = *maybe_info;

  // Track name: default = tracepoint's name. Or taken from payload.
  StringId track_name = info.event_name;
  if (info.track_name_field_id) {
    protozero::Field track_name_fld =
        decoder.FindField(info.track_name_field_id);
    if (!track_name_fld.valid()) {
      return context_->storage->IncrementStats(
          stats::kernel_trackevent_format_error);
    }
    track_name = context_->storage->InternString(track_name_fld.as_string());
  }

  // Track lookup: default to thread-scoped events (scoped by track name). With
  // an optional context field that overrides the scoping.
  TrackId track_id = kInvalidTrackId;
  switch (info.context_type) {
    case KernelTrackEvent::kTid: {
      UniqueTid utid = context_->process_tracker->GetOrCreateThread(tid);

      const auto& track_kind = (info.kind == KernelTrackEvent::kSlice)
                                   ? kThreadSliceTrackBp
                                   : kThreadCounterTrackBp;
      track_id = context_->track_tracker->InternTrack(
          track_kind, tracks::Dimensions(utid, track_name),
          tracks::DynamicName(track_name));
      break;
    }
    case KernelTrackEvent::kTgid: {
      protozero::Field context_tgid = decoder.FindField(info.context_field_id);
      if (!context_tgid.valid()) {
        return context_->storage->IncrementStats(
            stats::kernel_trackevent_format_error);
      }

      // NB: trusting that this is a real tgid, but *not* assuming that the
      // emitting thread is inside the tgid.
      UniquePid upid = context_->process_tracker->GetOrCreateProcess(
          context_tgid.as_int64());

      const auto& track_kind = (info.kind == KernelTrackEvent::kSlice)
                                   ? kProcessSliceTrackBp
                                   : kProcessCounterTrackBp;
      track_id = context_->track_tracker->InternTrack(
          track_kind, tracks::Dimensions(upid, track_name),
          tracks::DynamicName(track_name));
      break;
    }
    case KernelTrackEvent::kCpu: {
      protozero::Field context_cpu = decoder.FindField(info.context_field_id);
      if (!context_cpu.valid()) {
        return context_->storage->IncrementStats(
            stats::kernel_trackevent_format_error);
      }

      // Trusting that this is a real cpu number.
      const auto& track_kind = (info.kind == KernelTrackEvent::kSlice)
                                   ? kCpuSliceTrackBp
                                   : kCpuCounterTrackBp;
      track_id = context_->track_tracker->InternTrack(
          track_kind, tracks::Dimensions(context_cpu.as_uint32(), track_name),
          tracks::DynamicName(track_name));
      break;
    }
    case KernelTrackEvent::kCustom:
      // TODO(prototype): create a new top-level track group? Would like to
      // avoid an entire UI plugin to propagate the group to the UI though.
      break;
  }
  PERFETTO_DCHECK(track_id != kInvalidTrackId);

  // Insert the slice/counter data.
  if (info.kind == KernelTrackEvent::kSlice) {
    protozero::Field slice_type = decoder.FindField(info.slice_type_field_id);
    protozero::Field slice_name = decoder.FindField(info.slice_name_field_id);
    if (!slice_type.valid() || !slice_name.valid()) {
      return context_->storage->IncrementStats(
          stats::kernel_trackevent_format_error);
    }

    switch (static_cast<char>(slice_type.as_int64())) {
      case 'B': {  // begin
        context_->slice_tracker->Begin(
            ts, track_id, kNullStringId,
            context_->storage->InternString(slice_name.as_string()));
        break;
      }
      case 'E': {  // end
        context_->slice_tracker->End(ts, track_id);
        break;
      }
      case 'I': {  // instant
        context_->slice_tracker->Scoped(
            ts, track_id, kNullStringId,
            context_->storage->InternString(slice_name.as_string()),
            /*duration=*/0);
        break;
      }
      default: {
        return context_->storage->IncrementStats(
            stats::kernel_trackevent_format_error);
      }
    }
  } else if (info.kind == KernelTrackEvent::kCounter) {
    protozero::Field value = decoder.FindField(info.value_field_id);
    if (!value.valid()) {
      return context_->storage->IncrementStats(
          stats::kernel_trackevent_format_error);
    }

    context_->event_tracker->PushCounter(
        ts, static_cast<double>(value.as_int64()), track_id);
  }
}

}  // namespace perfetto::trace_processor
