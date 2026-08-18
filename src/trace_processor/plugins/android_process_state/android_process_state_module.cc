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

#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"

#include <cstdint>

#include "perfetto/base/compiler.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/blob_packet_writer.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor::android_process_state {
namespace {
namespace fb = com::android::internal::pbzero;
}  // namespace

AndroidProcessStateModule::AndroidProcessStateModule(
    ProtoImporterModuleContext* module_context,
    AndroidProcessStateTracker* tracker,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : ProtoImporterModule(module_context),
      context_(tracker->context()),
      tracker_(tracker),
      freezer_state_table_(freezer_state_table) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
}

AndroidProcessStateModule::~AndroidProcessStateModule() = default;

ModuleResult AndroidProcessStateModule::TokenizePacket(
    const TokenizePacketArgs& args) {
  if (args.field.id() !=
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber) {
    return ModuleResult::Ignored();
  }

  fb::AndroidProcessStateSnapshot::Decoder dump(
      args.field.Cast<fb::FrameworksBaseTracePacket::kAndroidProcessState>());
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
    int64_t real_ts = args.ts;
    if (rec.has_start_time_ms()) {
      std::optional<int64_t> start_ts = context_->clock_tracker->ToTraceTime(
          ClockTracker::ClockId::Machine(
              protos::pbzero::BUILTIN_CLOCK_BOOTTIME),
          static_cast<int64_t>(rec.start_time_ms()) * 1000000LL);
      if (start_ts.has_value()) {
        real_ts = *start_ts;
      }
    }

    TraceBlobView tbv = context_->blob_packet_writer->WritePacket(
        [&](protos::pbzero::TracePacket* pkt) {
          pkt->set_timestamp(static_cast<uint64_t>(real_ts));
          auto* snap = pkt->BeginNestedMessage<fb::AndroidProcessStateSnapshot>(
              fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
          auto* r = snap->add_record();
          r->set_pid(rec.pid());
          if (rec.has_uid()) {
            r->set_uid(rec.uid());
          }
          if (rec.has_proc_state()) {
            r->set_proc_state(rec.proc_state());
          }
          if (rec.has_oom_score()) {
            r->set_oom_score(rec.oom_score());
          }
          if (rec.has_capability_flags()) {
            r->set_capability_flags(rec.capability_flags());
          }
          if (rec.has_process_name()) {
            r->set_process_name(rec.process_name());
          }
          if (rec.has_start_seq_id()) {
            r->set_start_seq_id(rec.start_seq_id());
          }
          if (rec.has_start_time_ms()) {
            r->set_start_time_ms(rec.start_time_ms());
          }
        });

    RefPtr<PacketSequenceStateGeneration> state =
        args.state ? args.state
                   : PacketSequenceStateGeneration::CreateFirst(context_);
    module_context_->trace_packet_stream->Push(
        real_ts, TracePacketData{std::move(tbv), std::move(state)});
  }

  return ModuleResult::Handled();
}

void AndroidProcessStateModule::ParseField(const ParseFieldArgs& args) {
  switch (args.field.id()) {
    case fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber:
      tracker_->ParseDump(
          args.field
              .Cast<fb::FrameworksBaseTracePacket::kAndroidProcessState>());
      break;
    case fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber:
      ParseAndroidFreezerState(
          args.ts,
          args.field
              .Cast<fb::FrameworksBaseTracePacket::kAndroidFreezerState>());
      break;
    default:
      break;
  }
}

void AndroidProcessStateModule::OnEventsFullyExtracted() {
  tracker_->Finalize();
}

void AndroidProcessStateModule::ParseAndroidFreezerState(
    int64_t ts,
    protozero::ConstBytes blob) {
  base::ignore_result(ts);
  fb::AndroidFreezerStateSnapshot::Decoder evt(blob);
  for (auto it = evt.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));

    tables::AndroidFreezerStateTable::Row row;
    row.ts = std::nullopt;
    row.upid = upid;
    row.unfrozen_dur_ms = std::nullopt;
    row.frozen_dur_ms = std::nullopt;
    row.unfreeze_reason = std::nullopt;
    row.is_initial = 1;
    freezer_state_table_->Insert(row);
  }
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : TrackEventExtensionParser(context),
      trace_context_(trace_context),
      tracker_(tracker),
      freezer_state_table_(freezer_state_table) {
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber);
}

AndroidProcessStateExtensionParser::~AndroidProcessStateExtensionParser() =
    default;

TrackEventExtensionParser::Result
AndroidProcessStateExtensionParser::OnTrackEventSliceExtension(
    const TrackEventExtensionField& field,
    SliceId id) {
  int64_t ts = trace_context_->storage->slice_table()[id].ts();
  switch (field.id()) {
    case fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber:
      tracker_->ParseChange(
          ts,
          field
              .Cast<fb::FrameworksBaseTrackEvent::kProcessStateChangedEvent>());
      break;
    case fb::FrameworksBaseTrackEvent::kFreezerEventFieldNumber:
      ParseFreezerEvent(
          ts, field.Cast<fb::FrameworksBaseTrackEvent::kFreezerEvent>());
      break;
    default:
      break;
  }
  return Result::kHandled;
}

void AndroidProcessStateExtensionParser::ParseFreezerEvent(
    int64_t ts,
    protozero::ConstBytes data) {
  fb::AndroidFreezerEvent::Decoder evt(data);
  if (!evt.has_pid()) {
    return;
  }
  tables::AndroidFreezerStateTable::Row row;
  row.ts = ts;
  row.upid = trace_context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(evt.pid()));
  if (evt.has_unfrozen_dur_ms()) {
    row.unfrozen_dur_ms = evt.unfrozen_dur_ms();
  }
  if (evt.has_frozen_dur_ms()) {
    row.frozen_dur_ms = evt.frozen_dur_ms();
  }
  if (evt.has_unfreeze_reason()) {
    row.unfreeze_reason =
        InternEnum(trace_context_, unfreeze_reason_cache_,
                   ".com.android.internal.UnfreezeReason",
                   static_cast<int32_t>(evt.unfreeze_reason()));
  }
  row.is_initial = 0;
  freezer_state_table_->Insert(row);
}

}  // namespace perfetto::trace_processor::android_process_state
