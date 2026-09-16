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

#include "src/trace_processor/plugins/android_process_state/android_process_state_tracker.h"

#include <string>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_common.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto::trace_processor::android_process_state {
namespace {
namespace fb = com::android::internal::pbzero;
}  // namespace

StringId InternEnum(TraceProcessorContext* context,
                    DescriptorPool::CachedDescriptor& cache,
                    const char* enum_name,
                    int32_t value) {
  std::optional<std::string> name =
      context->descriptor_pool_->FindEnumString(cache, enum_name, value);
  return context->storage->InternString(
      base::StringView(name ? *name : std::to_string(value)));
}

AndroidProcessStateTracker::AndroidProcessStateTracker(
    TraceProcessorContext* context,
    tables::AndroidProcessStateTable* process_state_table,
    tables::AndroidFreezerStateTable* freezer_state_table)
    : context_(context),
      process_state_table_(process_state_table),
      freezer_state_table_(freezer_state_table) {}

// The track-event stream only emits delta transitions (prev -> cur). To
// reconstruct each process's state at the start of the trace, we record the
// `prev_*` values from its earliest observed delta event.
void AndroidProcessStateTracker::UpdateInitialStateFromDelta(
    int64_t ts,
    const ProcessStateValues& prev_state) {
  EarliestDelta& earliest = earliest_prev_[prev_state.upid];
  if (ts >= earliest.ts) {
    return;
  }
  earliest.ts = ts;
  earliest.values = prev_state;
}

void AndroidProcessStateTracker::ParseProcessStateChange(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessStateChangedEvent::Decoder p(bytes);
  if (!p.has_pid()) {
    return;
  }
  int32_t pid = p.pid();
  UniquePid upid =
      context_->process_tracker->GetOrCreateProcess(static_cast<uint32_t>(pid));

  ProcessStateValues prev;
  prev.upid = upid;
  if (p.has_prev_proc_state()) {
    prev.proc_state = static_cast<int32_t>(p.prev_proc_state());
  }
  if (p.has_prev_oom_score()) {
    prev.oom_score = p.prev_oom_score();
  }
  if (p.has_prev_capability_flags()) {
    prev.capability_flags = p.prev_capability_flags();
  }
  UpdateInitialStateFromDelta(ts, prev);

  // Insert the change row.
  tables::AndroidProcessStateTable::Row row;
  row.upid = upid;
  row.ts = ts;
  row.is_initial = 0;
  if (p.has_cur_proc_state()) {
    row.proc_state = InternEnum(context_, proc_state_cache_,
                                ".com.android.internal.ProcessStateEnum",
                                static_cast<int32_t>(p.cur_proc_state()));
  }
  if (p.has_cur_oom_score()) {
    row.oom_score = p.cur_oom_score();
  }
  if (p.has_cur_capability_flags()) {
    row.capability_flags = p.cur_capability_flags();
  }
  if (p.has_reason()) {
    row.reason = InternEnum(context_, reason_cache_,
                            ".com.android.internal.OomChangeReasonEnum",
                            static_cast<int32_t>(p.reason()));
  }
  process_state_table_->Insert(row);
}

void AndroidProcessStateTracker::ParseProcessStateDump(
    protozero::ConstBytes blob) {
  fb::AndroidProcessStateSnapshot::Decoder dump(blob);
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    ProcessStateValues v;
    v.upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));

    // Note: android.util.proto.ProtoOutputStream ignores/omits 0 data points
    // during serialization on Android, so unset fields in the dump snapshot
    // represent 0.
    //
    // TODO: Consider setting process_name and uid on the core `process` table
    // from dump records in a future update.
    v.proc_state = rec.has_proc_state()
                       ? static_cast<int32_t>(rec.proc_state())
                       : static_cast<int32_t>(
                             fb::ProcessStateEnum::PROCESS_STATE_UNSPECIFIED);
    v.oom_score = rec.has_oom_score() ? rec.oom_score() : 0;
    v.capability_flags =
        rec.has_capability_flags() ? rec.capability_flags() : 0;
    process_dump_[v.upid] = v;
  }
}

void AndroidProcessStateTracker::ParseFreezerEvent(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidFreezerEvent::Decoder evt(bytes);
  if (!evt.has_pid()) {
    return;
  }
  tables::AndroidFreezerStateTable::Row row;
  row.ts = ts;
  row.upid = context_->process_tracker->GetOrCreateProcess(
      static_cast<uint32_t>(evt.pid()));
  if (evt.has_unfrozen_dur_ms()) {
    row.unfrozen_dur_ms = evt.unfrozen_dur_ms();
  }
  if (evt.has_frozen_dur_ms()) {
    row.frozen_dur_ms = evt.frozen_dur_ms();
  }
  if (evt.has_unfreeze_reason()) {
    row.unfreeze_reason =
        InternEnum(context_, unfreeze_reason_cache_,
                   ".com.android.internal.UnfreezeReason",
                   static_cast<int32_t>(evt.unfreeze_reason()));
  }
  row.is_initial = 0;
  freezer_state_table_->Insert(row);
}

void AndroidProcessStateTracker::ParseFreezerDump(protozero::ConstBytes blob) {
  fb::AndroidFreezerStateSnapshot::Decoder dump(blob);
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    FreezerStateValues v;
    v.upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));
    // Note: android.util.proto.ProtoOutputStream ignores/omits 0 data points
    // during serialization on Android, so unset fields represent UFR_NONE (0).
    v.unfreeze_reason =
        rec.has_unfreeze_reason()
            ? static_cast<int32_t>(rec.unfreeze_reason())
            : static_cast<int32_t>(fb::UnfreezeReason::UFR_NONE);
    freezer_dump_[v.upid] = v;
  }
}

// Combines the trace-stop snapshot dump with earliest observed delta events:
// 1. Processes that never changed state during the trace have their initial
//    state recovered directly from `process_dump_` (the trace-stop snapshot).
// 2. Processes that changed state during the trace have their initial state
//    recovered from the `prev_*` fields of their earliest delta in
//    `earliest_prev_`.
std::map<UniquePid, AndroidProcessStateTracker::ProcessStateValues>
AndroidProcessStateTracker::ComputeInitialProcessStates() const {
  std::map<UniquePid, ProcessStateValues> initial = process_dump_;

  for (const auto& [upid, earliest] : earliest_prev_) {
    auto& v = initial[upid];
    v.upid = upid;
    if (earliest.values.proc_state.has_value()) {
      v.proc_state = earliest.values.proc_state;
    }
    if (earliest.values.oom_score.has_value()) {
      v.oom_score = earliest.values.oom_score;
    }
    if (earliest.values.capability_flags.has_value()) {
      v.capability_flags = earliest.values.capability_flags;
    }
  }

  return initial;
}

void AndroidProcessStateTracker::Finalize() {
  for (const auto& [upid, v] : ComputeInitialProcessStates()) {
    EmitInitialProcessStateRow(v);
  }
  for (const auto& [upid, v] : freezer_dump_) {
    EmitInitialFreezerRow(v);
  }
}

void AndroidProcessStateTracker::EmitInitialProcessStateRow(
    const ProcessStateValues& v) {
  tables::AndroidProcessStateTable::Row row;
  row.upid = v.upid;
  row.ts = std::nullopt;
  row.is_initial = 1;
  if (v.proc_state.has_value()) {
    row.proc_state =
        InternEnum(context_, proc_state_cache_,
                   ".com.android.internal.ProcessStateEnum", *v.proc_state);
  }
  if (v.oom_score.has_value()) {
    row.oom_score = *v.oom_score;
  }
  if (v.capability_flags.has_value()) {
    row.capability_flags = *v.capability_flags;
  }
  process_state_table_->Insert(row);
}

void AndroidProcessStateTracker::EmitInitialFreezerRow(
    const FreezerStateValues& v) {
  tables::AndroidFreezerStateTable::Row row;
  row.upid = v.upid;
  row.ts = std::nullopt;
  row.unfrozen_dur_ms = std::nullopt;
  row.frozen_dur_ms = std::nullopt;
  row.is_initial = 1;
  if (v.unfreeze_reason.has_value()) {
    row.unfreeze_reason =
        InternEnum(context_, unfreeze_reason_cache_,
                   ".com.android.internal.UnfreezeReason", *v.unfreeze_reason);
  }
  freezer_state_table_->Insert(row);
}

}  // namespace perfetto::trace_processor::android_process_state
