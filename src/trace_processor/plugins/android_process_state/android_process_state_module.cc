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
#include <map>
#include <optional>
#include <string>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor::android_process_state {

namespace fb = ::com::android::internal::pbzero;

AndroidProcessStateTracker::AndroidProcessStateTracker(
    TraceProcessorContext* context,
    tables::AndroidProcessStateTable* process_state_table)
    : context_(context), process_state_table_(process_state_table) {}

StringId AndroidProcessStateTracker::InternEnum(
    DescriptorPool::CachedDescriptor& cache,
    const char* enum_name,
    int32_t value) {
  auto name =
      context_->descriptor_pool_->FindEnumString(cache, enum_name, value);
  return context_->storage->InternString(
      base::StringView(name ? *name : std::to_string(value)));
}

void AndroidProcessStateTracker::ParseChange(int64_t ts,
                                             protozero::ConstBytes bytes) {
  fb::AndroidProcessStateChangedEvent::Decoder p(bytes);
  if (!p.has_pid()) {
    return;
  }
  int32_t pid = p.pid();
  UniquePid upid =
      context_->process_tracker->GetOrCreateProcess(static_cast<uint32_t>(pid));
  if (p.has_uid()) {
    context_->process_tracker->SetProcessUid(upid,
                                             static_cast<uint32_t>(p.uid()));
  }

  // Record the prev_* state of this process's earliest delta.
  EarliestDelta& earliest = earliest_prev_[upid];
  if (ts < earliest.ts) {
    earliest.ts = ts;
    earliest.values.upid = upid;
    earliest.values.pid = pid;
    if (p.has_uid()) {
      earliest.values.uid = p.uid();
    }
    if (p.has_prev_proc_state()) {
      earliest.values.proc_state = static_cast<int32_t>(p.prev_proc_state());
    }
    if (p.has_prev_oom_score()) {
      earliest.values.oom_score = p.prev_oom_score();
    }
    if (p.has_prev_capability_flags()) {
      earliest.values.capability_flags = p.prev_capability_flags();
    }
  }

  // Insert the change row.
  tables::AndroidProcessStateTable::Row row;
  row.upid = upid;
  row.pid = pid;
  if (p.has_uid()) {
    row.uid = p.uid();
  }
  row.ts = ts;
  row.is_initial = 0;
  if (p.has_cur_proc_state()) {
    row.proc_state =
        InternEnum(proc_state_cache_, ".com.android.internal.ProcessStateEnum",
                   static_cast<int32_t>(p.cur_proc_state()));
  }
  if (p.has_cur_oom_score()) {
    row.oom_score = p.cur_oom_score();
  }
  if (p.has_cur_capability_flags()) {
    row.capability_flags = p.cur_capability_flags();
  }
  if (p.has_reason()) {
    row.reason =
        InternEnum(reason_cache_, ".com.android.internal.OomChangeReasonEnum",
                   static_cast<int32_t>(p.reason()));
  }
  if (p.has_seq_id()) {
    row.seq_id = p.seq_id();
  }
  process_state_table_->Insert(row);
}

void AndroidProcessStateTracker::ParseDump(protozero::ConstBytes blob) {
  fb::AndroidProcessStateSnapshot::Decoder dump(blob);
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    ProcessStateValues v;
    v.pid = rec.pid();
    v.upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(v.pid));
    if (rec.has_uid()) {
      v.uid = rec.uid();
      context_->process_tracker->SetProcessUid(v.upid,
                                               static_cast<uint32_t>(*v.uid));
    }
    if (rec.has_proc_state()) {
      v.proc_state = rec.proc_state();
    }
    if (rec.has_oom_score()) {
      v.oom_score = rec.oom_score();
    }
    if (rec.has_capability_flags()) {
      v.capability_flags = rec.capability_flags();
    }
    dump_[v.upid] = v;
  }
}

void AndroidProcessStateTracker::Finalize() {
  // Start from the trace-stop dump (the initial state of every process that did
  // not change), then let a changed process's earliest-delta prev_* override
  // it.
  std::map<UniquePid, ProcessStateValues> initial = dump_;
  for (const auto& [upid, earliest] : earliest_prev_) {
    initial[upid] = earliest.values;
  }
  for (const auto& [upid, v] : initial) {
    EmitInitialRow(v);
  }
}

void AndroidProcessStateTracker::EmitInitialRow(const ProcessStateValues& v) {
  tables::AndroidProcessStateTable::Row row;
  row.upid = v.upid;
  row.pid = v.pid;
  if (v.uid.has_value()) {
    row.uid = *v.uid;
  }
  row.ts = std::nullopt;
  row.is_initial = 1;
  if (v.proc_state.has_value()) {
    row.proc_state =
        InternEnum(proc_state_cache_, ".com.android.internal.ProcessStateEnum",
                   *v.proc_state);
  }
  if (v.oom_score.has_value()) {
    row.oom_score = *v.oom_score;
  }
  if (v.capability_flags.has_value()) {
    row.capability_flags = *v.capability_flags;
  }
  process_state_table_->Insert(row);
}

AndroidProcessStateModule::AndroidProcessStateModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : ProtoImporterModule(module_context),
      context_(trace_context),
      tracker_(tracker),
      freezer_state_table_(freezer_state_table) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
}

AndroidProcessStateModule::~AndroidProcessStateModule() = default;

void AndroidProcessStateModule::ParseField(const ParseFieldArgs& args) {
  for (const protozero::Field& f : args.decoder.unknown_fields()) {
    switch (f.id()) {
      case fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber:
        tracker_->ParseDump(f.as_bytes());
        break;
      case fb::FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber:
        ParseAndroidFreezerState(args.ts, f.as_bytes());
        break;
      default:
        break;
    }
  }
}

void AndroidProcessStateModule::OnEventsFullyExtracted() {
  tracker_->Finalize();
}

void AndroidProcessStateModule::ParseAndroidFreezerState(
    int64_t ts,
    protozero::ConstBytes blob) {
  fb::AndroidFreezerStateSnapshot::Decoder evt(blob);
  for (auto it = evt.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));

    tables::AndroidFreezerStateTable::Row row;
    row.ts = ts;
    row.upid = upid;
    row.pid = rec.pid();
    if (rec.has_uid()) {
      row.uid = rec.uid();
    }
    row.unfrozen_dur_ms = rec.has_unfrozen_dur_ms() ? rec.unfrozen_dur_ms() : 0;
    row.frozen_dur_ms = rec.has_frozen_dur_ms() ? rec.frozen_dur_ms() : 0;
    row.unfreeze_reason = InternEnum(
        unfreeze_reason_cache_, ".com.android.internal.UnfreezeReason",
        static_cast<int32_t>(
            rec.has_unfreeze_reason() ? rec.has_unfreeze_reason() : 0));
    row.is_initial = 1;
    freezer_state_table_->Insert(row);
  }
}

StringId AndroidProcessStateModule::InternEnum(
    DescriptorPool::CachedDescriptor& cache,
    const char* enum_name,
    int32_t value) {
  auto name =
      context_->descriptor_pool_->FindEnumString(cache, enum_name, value);
  return context_->storage->InternString(
      base::StringView(name ? *name : std::to_string(value)));
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker)
    : TrackEventExtensionParser(context),
      trace_context_(trace_context),
      tracker_(tracker) {
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
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
    default:
      break;
  }
  return Result::kHandled;
}

}  // namespace perfetto::trace_processor::android_process_state
