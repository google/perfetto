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

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

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
  if (name) {
    return context->storage->InternString(*name);
  }
  return context->storage->InternString(std::to_string(value));
}

AndroidProcessStateTracker::AndroidProcessStateTracker(
    TraceProcessorContext* context,
    tables::AndroidProcessStateTable* process_state_table,
    tables::AndroidFreezerStateTable* freezer_state_table,
    tables::AndroidProcessGroupTable* process_group_table)
    : context_(context),
      process_state_table_(process_state_table),
      freezer_state_table_(freezer_state_table),
      process_group_table_(process_group_table),
      capability_flags_id_(context->storage->InternString("capability_flags")),
      cpu_time_reasons_id_(context->storage->InternString("cpu_time_reasons")),
      implicit_cpu_time_reasons_id_(
          context->storage->InternString("implicit_cpu_time_reasons")),
      freeze_policy_flags_id_(
          context->storage->InternString("freeze_policy_flags")),
      unknown_process_logger_(context) {}

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

std::optional<UniquePid> AndroidProcessStateTracker::ResolveLiveProcess(
    int64_t ts,
    const char* source,
    int64_t pid,
    protozero::ConstChars process_name,
    std::optional<int64_t> start_seq_id) {
  // A missing pid decodes as 0. pids <= 0 don't identify a target process.
  if (pid <= 0) {
    return std::nullopt;
  }
  std::optional<UniquePid> upid =
      context_->process_tracker->GetProcessOrNull(static_cast<uint32_t>(pid));
  if (!upid) {
    unknown_process_logger_.Record(ts, source, pid, process_name, start_seq_id);
  }
  return upid;
}

void AndroidProcessStateTracker::ParseProcessStateChange(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessStateChangedEvent::Decoder p(bytes);
  std::optional<UniquePid> opt_upid =
      ResolveLiveProcess(ts, "process_state_changed", p.pid());
  if (!opt_upid) {
    return;
  }
  UniquePid upid = *opt_upid;

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
  if (p.has_prev_process_group()) {
    prev.process_group = static_cast<int32_t>(p.prev_process_group());
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
  if (p.has_cur_process_group()) {
    row.process_group = InternEnum(context_, process_group_cache_,
                                   ".com.android.internal.ProcessGroup",
                                   static_cast<int32_t>(p.cur_process_group()));
  }
  if (p.has_reason()) {
    row.reason = InternEnum(context_, reason_cache_,
                            ".com.android.internal.OomChangeReasonEnum",
                            static_cast<int32_t>(p.reason()));
  }
  if (p.has_seq_id()) {
    row.seq_id = p.seq_id();
  }
  if (p.has_cpu_time_reasons()) {
    row.cpu_time_reasons = p.cpu_time_reasons();
  }
  if (p.has_implicit_cpu_time_reasons()) {
    row.implicit_cpu_time_reasons = p.implicit_cpu_time_reasons();
  }
  if (p.has_freeze_policy_flags()) {
    row.freeze_policy_flags = p.freeze_policy_flags();
  }
  auto id = process_state_table_->Insert(row).id;
  AddFlagsArgs(id, row);
}

void AndroidProcessStateTracker::ParseProcessStateDump(
    int64_t ts,
    protozero::ConstBytes blob) {
  fb::AndroidProcessStateSnapshot::Decoder dump(blob);
  // The trace-start dump carries no state. It is handled by
  // android_framework_track_event, which creates the processes it lists.
  if (dump.dump_reason() ==
      fb::AndroidProcessStateSnapshot::DUMP_REASON_START) {
    return;
  }
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
    std::optional<UniquePid> opt_upid = ResolveLiveProcess(
        ts, "process_dump", rec.pid(), rec.process_name(),
        rec.has_start_seq_id() ? std::make_optional(rec.start_seq_id())
                               : std::nullopt);
    if (!opt_upid) {
      continue;
    }
    ProcessStateValues v;
    v.upid = *opt_upid;

    // Note: android.util.proto.ProtoOutputStream ignores/omits 0 data points
    // during serialization on Android, so unset fields in the dump snapshot
    // represent 0.
    v.proc_state = rec.has_proc_state()
                       ? static_cast<int32_t>(rec.proc_state())
                       : static_cast<int32_t>(
                             fb::ProcessStateEnum::PROCESS_STATE_UNSPECIFIED);
    v.oom_score = rec.has_oom_score() ? rec.oom_score() : 0;
    v.capability_flags =
        rec.has_capability_flags() ? rec.capability_flags() : 0;
    v.process_group =
        rec.has_process_group()
            ? static_cast<int32_t>(rec.process_group())
            : static_cast<int32_t>(fb::ProcessGroup::PROCESS_GROUP_UNKNOWN);
    process_dump_[v.upid] = v;
  }
}

void AndroidProcessStateTracker::ParseFreezerEvent(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidFreezerEvent::Decoder evt(bytes);
  std::optional<UniquePid> opt_upid =
      ResolveLiveProcess(ts, "freezer_event", evt.pid());
  if (!opt_upid) {
    return;
  }
  tables::AndroidFreezerStateTable::Row row;
  row.ts = ts;
  row.upid = *opt_upid;
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

void AndroidProcessStateTracker::ParseFreezerDump(int64_t ts,
                                                  protozero::ConstBytes blob) {
  fb::AndroidFreezerStateSnapshot::Decoder dump(blob);
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    std::optional<UniquePid> opt_upid =
        ResolveLiveProcess(ts, "freezer_dump", rec.pid());
    if (!opt_upid) {
      continue;
    }
    FreezerStateValues v;
    v.upid = *opt_upid;
    // If the process is unfrozen in the dump, has_unfrozen_dur_ms() is true.
    // Note: android.util.proto.ProtoOutputStream omits 0 values during
    // serialization, so an unset unfreeze_reason on an unfrozen process
    // represents UFR_NONE (0). When the process is frozen, unfreeze_reason is
    // left as std::nullopt so the initial row has NULL.
    if (rec.has_unfrozen_dur_ms()) {
      v.unfreeze_reason =
          rec.has_unfreeze_reason()
              ? static_cast<int32_t>(rec.unfreeze_reason())
              : static_cast<int32_t>(fb::UnfreezeReason::UFR_NONE);
    }
    freezer_dump_[v.upid] = v;
  }
}

void AndroidProcessStateTracker::ParseProcessGroupChange(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessGroupChangedEvent::Decoder evt(bytes);
  std::optional<UniquePid> opt_upid =
      ResolveLiveProcess(ts, "process_group_changed", evt.pid());
  if (!opt_upid) {
    return;
  }
  tables::AndroidProcessGroupTable::Row row;
  row.ts = ts;
  row.upid = *opt_upid;
  if (evt.has_uid()) {
    row.uid = evt.uid();
  }
  if (evt.has_cur_process_group()) {
    row.process_group = InternEnum(
        context_, process_group_cache_, ".com.android.internal.ProcessGroup",
        static_cast<int32_t>(evt.cur_process_group()));
  }
  process_group_table_->Insert(row);
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
    if (earliest.values.process_group.has_value()) {
      v.process_group = earliest.values.process_group;
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
  if (v.process_group.has_value()) {
    row.process_group =
        InternEnum(context_, process_group_cache_,
                   ".com.android.internal.ProcessGroup", *v.process_group);
  }
  auto id = process_state_table_->Insert(row).id;
  AddFlagsArgs(id, row);
}

void AndroidProcessStateTracker::AddFlagsArgs(
    tables::AndroidProcessStateTable::Id id,
    const tables::AndroidProcessStateTable::Row& row) {
  auto has_non_zero = [](std::optional<int32_t> mask) {
    return mask.has_value() && *mask != 0;
  };
  if (!has_non_zero(row.capability_flags) &&
      !has_non_zero(row.cpu_time_reasons) &&
      !has_non_zero(row.implicit_cpu_time_reasons) &&
      !has_non_zero(row.freeze_policy_flags)) {
    return;
  }

  auto inserter = context_->args_tracker->AddArgsTo(process_state_table_, id);
  if (has_non_zero(row.capability_flags)) {
    EmitDecodedFlags(inserter, capability_flags_id_, "capability_flags",
                     capability_flags_cache_,
                     ".com.android.internal.ProcessCapabilityEnum",
                     *row.capability_flags);
  }
  if (has_non_zero(row.cpu_time_reasons)) {
    EmitDecodedFlags(inserter, cpu_time_reasons_id_, "cpu_time_reasons",
                     cpu_time_reasons_cache_,
                     ".com.android.internal.CpuTimeReason",
                     *row.cpu_time_reasons);
  }
  if (has_non_zero(row.implicit_cpu_time_reasons)) {
    EmitDecodedFlags(inserter, implicit_cpu_time_reasons_id_,
                     "implicit_cpu_time_reasons", cpu_time_reasons_cache_,
                     ".com.android.internal.CpuTimeReason",
                     *row.implicit_cpu_time_reasons);
  }
  if (has_non_zero(row.freeze_policy_flags)) {
    EmitDecodedFlags(inserter, freeze_policy_flags_id_, "freeze_policy_flags",
                     freeze_policy_flags_cache_,
                     ".com.android.internal.FreezerPolicyFlag",
                     *row.freeze_policy_flags);
  }
}

void AndroidProcessStateTracker::EmitDecodedFlags(
    ArgsInserter& inserter,
    StringId flat_key_id,
    const char* key_prefix,
    DescriptorPool::CachedDescriptor& cache,
    const char* enum_name,
    int32_t mask) {
  flag_views_.clear();
  int64_t unmatched = context_->descriptor_pool_->FlagSetToViews(
      cache, enum_name, static_cast<int64_t>(static_cast<uint32_t>(mask)),
      &flag_views_);

  auto emit = [&](size_t i, base::StringView flag) {
    base::StackString<64> key("%s[%zu]", key_prefix, i);
    StringId key_id = context_->storage->InternString(key.string_view());
    inserter.AddArg(flat_key_id, key_id,
                    Variadic::String(context_->storage->InternString(flag)));
  };
  size_t i = 0;
  for (; i < flag_views_.size(); ++i) {
    emit(i, base::StringView(flag_views_[i].data(), flag_views_[i].size()));
  }
  if (unmatched != 0) {
    base::StackString<20> hex("0x%" PRIx64, static_cast<uint64_t>(unmatched));
    emit(i, hex.string_view());
  }
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
