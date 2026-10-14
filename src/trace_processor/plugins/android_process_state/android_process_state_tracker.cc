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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

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

// ProcessStateEnum.PROCESS_STATE_NONEXISTENT: the process record does not exist
// (yet, or anymore). A transition from/to it is a process birth/death.
constexpr int32_t kProcStateNonexistent = 1019;

bool IsNonexistent(const std::optional<int32_t>& proc_state) {
  return proc_state.has_value() && *proc_state == kProcStateNonexistent;
}

StringId InternEnumShort(TraceProcessorContext* context,
                         DescriptorPool::CachedDescriptor& cache,
                         const char* enum_name,
                         const char* strip_prefix,
                         int32_t value) {
  std::optional<std::string> name =
      context->descriptor_pool_->FindEnumString(cache, enum_name, value);
  if (!name) {
    return context->storage->InternString(
        base::StringView(std::to_string(value)));
  }
  base::StringView sv(*name);
  size_t prefix_len = std::strlen(strip_prefix);
  if (sv.size() > prefix_len && sv.substr(0, prefix_len) == strip_prefix) {
    sv = sv.substr(prefix_len);
  }
  return context->storage->InternString(sv);
}

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
    tables::AndroidFreezerStateTable* freezer_state_table,
    tables::AndroidProcessStateSnapshotTable* snapshot_table,
    tables::AndroidProcessStateProcessTable* process_table,
    tables::AndroidProcessStateServiceTable* service_table,
    tables::AndroidProcessStateServiceBindingTable* service_binding_table,
    tables::AndroidProcessStateProviderTable* provider_table,
    tables::AndroidProcessStateProviderBindingTable* provider_binding_table,
    tables::AndroidProcessStateTriggerEventTable* trigger_event_table)
    : context_(context),
      process_state_table_(process_state_table),
      freezer_state_table_(freezer_state_table),
      snapshot_table_(snapshot_table),
      process_table_(process_table),
      service_table_(service_table),
      service_binding_table_(service_binding_table),
      provider_table_(provider_table),
      provider_binding_table_(provider_binding_table),
      trigger_event_table_(trigger_event_table) {}

AndroidProcessStateTracker::PassInfo& AndroidProcessStateTracker::EnsurePass(
    int64_t seq_id,
    int64_t ts) {
  auto& pass = passes_[seq_id];
  pass.seq_id = seq_id;
  if (ts < pass.ts) {
    pass.ts = ts;
  }
  return pass;
}

int32_t AndroidProcessStateTracker::ResolveServiceId(
    std::optional<int32_t> raw_svc_id,
    std::optional<int32_t> pid,
    StringId name_id) {
  if (raw_svc_id && *raw_svc_id != 0) {
    if (pid && !name_id.is_null()) {
      svc_by_key_[{*pid, name_id.raw_id()}] = *raw_svc_id;
    }
    return *raw_svc_id;
  }
  int32_t p = pid.value_or(0);
  auto key = std::make_pair(p, name_id.raw_id());
  auto it = svc_by_key_.find(key);
  if (it != svc_by_key_.end()) {
    return it->second;
  }
  int32_t syn = next_synthetic_id_--;
  svc_by_key_[key] = syn;
  return syn;
}

int32_t AndroidProcessStateTracker::ResolveProviderId(
    std::optional<int32_t> raw_prov_id,
    std::optional<int32_t> pid,
    StringId authority_id,
    StringId comp_id) {
  if (raw_prov_id && *raw_prov_id != 0) {
    StringId k = !authority_id.is_null() ? authority_id : comp_id;
    if (pid && !k.is_null()) {
      prov_by_key_[{*pid, k.raw_id()}] = *raw_prov_id;
    }
    return *raw_prov_id;
  }
  int32_t p = pid.value_or(0);
  StringId k = !authority_id.is_null() ? authority_id : comp_id;
  auto key = std::make_pair(p, k.raw_id());
  auto it = prov_by_key_.find(key);
  if (it != prov_by_key_.end()) {
    return it->second;
  }
  int32_t syn = next_synthetic_id_--;
  prov_by_key_[key] = syn;
  return syn;
}

StringId AndroidProcessStateTracker::InternCapabilities(int32_t mask) {
  if (mask == 0) {
    return context_->storage->InternString("none");
  }
  std::string out;
  for (int bit = 0; bit < 31; ++bit) {
    if ((mask & (1 << bit)) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " | ";
    }
    std::optional<std::string> name =
        context_->descriptor_pool_->FindEnumString(
            capability_cache_, ".com.android.internal.ProcessCapabilityEnum",
            bit + 1);
    if (name) {
      base::StringView sv(*name);
      constexpr char kPrefix[] = "PROCESS_CAPABILITY_";
      if (sv.size() > sizeof(kPrefix) - 1 &&
          sv.substr(0, sizeof(kPrefix) - 1) == kPrefix) {
        sv = sv.substr(sizeof(kPrefix) - 1);
      }
      out.append(sv.data(), sv.size());
    } else {
      out += "0x" + std::to_string(1 << bit);
    }
  }
  return context_->storage->InternString(base::StringView(out));
}

StringId AndroidProcessStateTracker::InternBindFlags(uint64_t flags) {
  if (flags == 0) {
    return kNullStringId;
  }
  std::string out;
  for (int bit = 0; bit < 64; ++bit) {
    uint64_t mask = 1ULL << bit;
    if ((flags & mask) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " | ";
    }
    std::optional<std::string> name;
    if (bit < 32) {
      name = context_->descriptor_pool_->FindEnumString(
          bind_flag_cache_, ".com.android.internal.ServiceBindFlag",
          static_cast<int32_t>(mask));
    } else {
      name = context_->descriptor_pool_->FindEnumString(
          bind_flag64_cache_, ".com.android.internal.ServiceBindFlagLongBits",
          static_cast<int32_t>(1ULL << (bit - 32)));
    }
    if (name) {
      base::StringView sv(*name);
      constexpr char kPrefix[] = "SERVICE_BIND_FLAG_";
      if (sv.size() > sizeof(kPrefix) - 1 &&
          sv.substr(0, sizeof(kPrefix) - 1) == kPrefix) {
        sv = sv.substr(sizeof(kPrefix) - 1);
      }
      out.append(sv.data(), sv.size());
    } else {
      out += "0x" + std::to_string(mask);
    }
  }
  return context_->storage->InternString(base::StringView(out));
}

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
    std::optional<UniqueTid> utid,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessStateChangedEvent::Decoder p(bytes);
  if (!p.has_pid()) {
    return;
  }
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  int32_t pid = p.pid();

  // Record in graph reconstruction model even if process_tracker hasn't seen
  // the PID yet, or look up name/uid from process_tracker if available.
  std::optional<UniquePid> opt_upid =
      context_->process_tracker->GetProcessOrNull(static_cast<uint32_t>(pid));
  if (opt_upid) {
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

    // Insert the change row into __intrinsic_android_process_state.
    tables::AndroidProcessStateTable::Row row;
    row.upid = upid;
    row.ts = ts;
    row.utid = utid;
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
      row.process_group = InternEnum(
          context_, process_group_cache_, ".com.android.internal.ProcessGroup",
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
    process_state_table_->Insert(row);
  }

  // Record graph mutation when process is known or graph data is present.
  if (opt_upid || procs_.count(pid) > 0) {
    has_graph_data_ = true;
    auto& proc = procs_[pid];
    proc.pid = pid;
    if (p.has_uid()) {
      proc.uid = p.uid();
    }
    ProcMutation mut;
    mut.ts = ts;
    mut.kind = ProcMutation::kStateChange;
    mut.seq_id = p.has_seq_id() ? p.seq_id() : -1;
    if (p.has_prev_proc_state()) {
      mut.prev.proc_state = static_cast<int32_t>(p.prev_proc_state());
    }
    if (p.has_prev_oom_score()) {
      mut.prev.oom_score = p.prev_oom_score();
    }
    if (p.has_prev_capability_flags()) {
      mut.prev.capability_flags = p.prev_capability_flags();
    }
    if (p.has_cur_proc_state()) {
      mut.cur.proc_state = static_cast<int32_t>(p.cur_proc_state());
    }
    if (p.has_cur_oom_score()) {
      mut.cur.oom_score = p.cur_oom_score();
    }
    if (p.has_cur_capability_flags()) {
      mut.cur.capability_flags = p.cur_capability_flags();
    }
    proc.mutations.push_back(mut);

    if (p.has_seq_id()) {
      auto& pass = EnsurePass(p.seq_id(), ts);
      if (p.has_reason() && !pass.reason.has_value()) {
        pass.reason = static_cast<int32_t>(p.reason());
      }
    }
  }
}

void AndroidProcessStateTracker::ParseProcessStateDied(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessStateDiedEvent::Decoder p(bytes);
  if (!p.has_pid() || p.pid() <= 0) {
    return;
  }
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  std::optional<UniquePid> opt_upid =
      context_->process_tracker->GetProcessOrNull(
          static_cast<uint32_t>(p.pid()));
  if (!opt_upid) {
    // GetProcessOrNull skips processes that already ended; fall back to the
    // most recent process row with this pid.
    const auto& pt = context_->storage->process_table();
    for (uint32_t i = pt.row_count(); i > 0; --i) {
      if (pt[i - 1].pid() == static_cast<uint32_t>(p.pid())) {
        opt_upid = i - 1;
        break;
      }
    }
  }
  if (opt_upid) {
    ProcessStateValues prev;
    prev.upid = *opt_upid;
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
  }

  // The kernel exit usually precedes this event, so the pid may already be
  // ended in the process tracker; still record the death so a process alive
  // at trace start (with no state changes) is reconstructed until here.
  has_graph_data_ = true;
  auto& proc = procs_[p.pid()];
  proc.pid = p.pid();
  if (p.has_uid()) {
    proc.uid = p.uid();
  }
  ProcMutation mut;
  mut.ts = ts;
  mut.seq_id = p.has_seq_id() ? p.seq_id() : -1;
  mut.kind = ProcMutation::kDeath;
  if (p.has_prev_proc_state()) {
    mut.prev.proc_state = static_cast<int32_t>(p.prev_proc_state());
  }
  if (p.has_prev_oom_score()) {
    mut.prev.oom_score = p.prev_oom_score();
  }
  if (p.has_prev_capability_flags()) {
    mut.prev.capability_flags = p.prev_capability_flags();
  }
  proc.mutations.push_back(mut);

  // Record the death as a trigger so the pass it causes links back to it
  // (the kernel exit may only be seen seconds later).
  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (mut.seq_id >= 0) {
    tr.seq_id = mut.seq_id;
  }
  tr.kind = context_->storage->InternString("process_state_died");
  tr.target_pid = p.pid();
  if (p.has_uid()) {
    tr.target_uid = p.uid();
  }
  if (!proc.name_id.is_null()) {
    tr.component_name = proc.name_id;
  } else if (opt_upid) {
    auto name = context_->storage->process_table()[*opt_upid].name();
    if (name) {
      tr.component_name = *name;
    }
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseOomAdjusterPass(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidOomAdjusterPassEvent::Decoder e(bytes);
  if (!e.has_seq_id()) {
    return;
  }
  has_graph_data_ = true;
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  auto& pass = EnsurePass(e.seq_id(), ts);
  if (e.has_reason()) {
    pass.reason = static_cast<int32_t>(e.reason());
  }
  if (e.has_is_full_update()) {
    pass.is_full_update = e.is_full_update() ? 1 : 0;
  }
  if (e.has_top_pid()) {
    pass.top_pid = e.top_pid();
  }
  if (e.has_top_proc_state()) {
    pass.top_proc_state = static_cast<int32_t>(e.top_proc_state());
  }
  std::string targets;
  for (auto it = e.target_pid(); it; ++it) {
    if (!targets.empty()) {
      targets += ",";
    }
    targets += std::to_string(*it);
  }
  if (!targets.empty()) {
    pass.target_pids =
        context_->storage->InternString(base::StringView(targets));
  }
}

void AndroidProcessStateTracker::ParseServiceStateChange(
    int64_t ts,
    base::StringView slice_name,
    protozero::ConstBytes bytes) {
  fb::AndroidServiceStateChangedEvent::Decoder s(bytes);
  has_graph_data_ = true;
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  StringId comp_id = s.has_component_name()
                         ? context_->storage->InternString(s.component_name())
                         : kNullStringId;
  std::optional<int32_t> pid =
      s.has_pid() ? std::make_optional(s.pid()) : std::nullopt;
  std::optional<int32_t> raw_svc_id =
      s.has_service_id() ? std::make_optional(s.service_id()) : std::nullopt;
  int32_t svc_id = ResolveServiceId(raw_svc_id, pid, comp_id);

  auto& svc = svcs_[svc_id];
  svc.svc_id = svc_id;
  if (pid && *pid > 0) {
    svc.owning_pid = *pid;
  }
  if (s.has_uid()) {
    svc.uid = s.uid();
  }
  if (!comp_id.is_null()) {
    svc.name_id = comp_id;
  }

  int64_t seq_id = s.has_seq_id() ? s.seq_id() : -1;
  uint64_t flags = 0;
  if (s.has_bind_flags_32()) {
    flags |= static_cast<uint32_t>(s.bind_flags_32());
  }
  if (s.has_bind_flags_32_to_64()) {
    flags |=
        (static_cast<uint64_t>(static_cast<uint32_t>(s.bind_flags_32_to_64()))
         << 32);
  }
  // BIND_FOREGROUND_SERVICE is bit 26 (0x04000000).
  bool is_fg_bind = (flags & 0x04000000ULL) != 0;
  StringId action_id = s.has_intent_action() && s.intent_action().size > 0
                           ? context_->storage->InternString(s.intent_action())
                           : kNullStringId;

  SvcMutation sm;
  sm.ts = ts;
  sm.seq_id = seq_id;
  sm.kind = SvcMutation::kOwner;
  sm.pid = pid;

  std::optional<int32_t> trigger_bind_id;
  if (slice_name == "service_binding" || slice_name == "service_unbinding" ||
      slice_name == "service_connection_added" ||
      slice_name == "service_connection_removed" ||
      slice_name == "service_binding_flags_updated") {
    bool is_flags_update = (slice_name == "service_binding_flags_updated");
    bool is_bind =
        (slice_name == "service_binding" ||
         slice_name == "service_connection_added" || is_flags_update);
    int32_t caller_pid = s.has_caller_pid() ? s.caller_pid() : 0;
    int32_t bind_id = 0;
    if (s.has_bind_id() && s.bind_id() != 0) {
      bind_id = s.bind_id();
      bind_by_pair_[{caller_pid, svc_id}] = bind_id;
    } else {
      auto key = std::make_pair(caller_pid, svc_id);
      auto it = bind_by_pair_.find(key);
      if (it != bind_by_pair_.end()) {
        bind_id = it->second;
      } else {
        bind_id = next_synthetic_id_--;
        bind_by_pair_[key] = bind_id;
      }
    }
    auto& b = binds_[bind_id];
    b.bind_id = bind_id;
    trigger_bind_id = bind_id;
    if (is_bind || b.client_pid == 0) {
      b.client_pid = caller_pid;
    }
    if (s.has_caller_uid()) {
      b.client_uid = s.caller_uid();
    }
    b.service_id = svc_id;
    if (s.has_intent_bind_id()) {
      b.intent_bind_id = s.intent_bind_id();
    }
    BindMutation bm;
    bm.ts = ts;
    bm.seq_id = seq_id;
    bm.bound = is_bind;
    bm.flags = flags;
    bm.fg = is_fg_bind;
    bm.action_id = action_id;
    bm.flags_update = is_flags_update;
    b.mutations.push_back(bm);
  } else if (slice_name == "service_create") {
    sm.kind = SvcMutation::kCreate;
  } else if (slice_name == "service_published") {
    sm.kind = SvcMutation::kPublish;
  } else if (slice_name == "service_start") {
    sm.kind = SvcMutation::kStart;
    sm.start_id = s.has_start_id() ? s.start_id() : 0;
  } else if (slice_name == "service_stop") {
    sm.kind = SvcMutation::kStop;
  } else if (slice_name == "service_destroy") {
    sm.kind = SvcMutation::kDestroy;
  }
  if (sm.kind != SvcMutation::kOwner || pid) {
    svc.mutations.push_back(sm);
  }

  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (seq_id >= 0) {
    tr.seq_id = seq_id;
  }
  tr.kind = context_->storage->InternString(
      slice_name.empty() ? base::StringView("service_state_changed")
                         : slice_name);
  tr.action = tr.kind;
  if (!comp_id.is_null()) {
    tr.component_name = comp_id;
  }
  if (pid) {
    tr.target_pid = *pid;
  }
  if (s.has_uid()) {
    tr.target_uid = s.uid();
  }
  if (s.has_caller_pid()) {
    tr.caller_pid = s.caller_pid();
  }
  if (s.has_caller_uid()) {
    tr.caller_uid = s.caller_uid();
  }
  StringId flags_str = InternBindFlags(flags);
  if (!flags_str.is_null()) {
    tr.detail = flags_str;
  } else if (!action_id.is_null()) {
    tr.detail = action_id;
  }
  tr.service_id = svc_id;
  if (trigger_bind_id) {
    tr.bind_id = *trigger_bind_id;
  }
  if (s.has_intent_bind_id() && s.intent_bind_id() != 0) {
    tr.intent_bind_id = s.intent_bind_id();
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseFgServiceStateChange(
    int64_t ts,
    base::StringView slice_name,
    protozero::ConstBytes bytes) {
  fb::AndroidServiceStateChangedEvent::Decoder s(bytes);
  has_graph_data_ = true;
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  StringId comp_id = s.has_component_name()
                         ? context_->storage->InternString(s.component_name())
                         : kNullStringId;
  std::optional<int32_t> pid =
      s.has_pid() ? std::make_optional(s.pid()) : std::nullopt;
  std::optional<int32_t> raw_svc_id =
      s.has_service_id() ? std::make_optional(s.service_id()) : std::nullopt;
  int32_t svc_id = ResolveServiceId(raw_svc_id, pid, comp_id);

  auto& svc = svcs_[svc_id];
  svc.svc_id = svc_id;
  if (pid && *pid > 0) {
    svc.owning_pid = *pid;
  }
  if (s.has_uid()) {
    svc.uid = s.uid();
  }
  if (!comp_id.is_null()) {
    svc.name_id = comp_id;
  }

  int64_t seq_id = s.has_seq_id() ? s.seq_id() : -1;
  SvcMutation sm;
  sm.ts = ts;
  sm.seq_id = seq_id;
  sm.kind = (slice_name == "fgs_stop")           ? SvcMutation::kFgsStop
            : (slice_name == "fgs_type_changed") ? SvcMutation::kFgsTypeChange
                                                 : SvcMutation::kFgsStart;
  sm.fg_type =
      s.has_foreground_service_type() ? s.foreground_service_type() : 0;
  sm.pid = pid;
  svc.mutations.push_back(sm);

  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (seq_id >= 0) {
    tr.seq_id = seq_id;
  }
  tr.kind = context_->storage->InternString(
      slice_name.empty() ? base::StringView("fgs_state_changed") : slice_name);
  tr.action = tr.kind;
  if (!comp_id.is_null()) {
    tr.component_name = comp_id;
  }
  if (pid) {
    tr.target_pid = *pid;
  }
  if (s.has_uid()) {
    tr.target_uid = s.uid();
  }
  if (s.has_caller_pid()) {
    tr.caller_pid = s.caller_pid();
  }
  if (s.has_caller_uid()) {
    tr.caller_uid = s.caller_uid();
  }
  if (s.has_foreground_service_type()) {
    char buf[32];
    snprintf(buf, sizeof(buf), "fgs_type=0x%x",
             static_cast<uint32_t>(s.foreground_service_type()));
    tr.detail = context_->storage->InternString(base::StringView(buf));
  }
  tr.service_id = svc_id;
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseProviderStateChange(
    int64_t ts,
    base::StringView slice_name,
    protozero::ConstBytes bytes) {
  fb::AndroidProviderStateChangedEvent::Decoder pv(bytes);
  has_graph_data_ = true;
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  StringId auth_id = pv.has_authority()
                         ? context_->storage->InternString(pv.authority())
                         : kNullStringId;
  StringId comp_id = pv.has_component_name()
                         ? context_->storage->InternString(pv.component_name())
                         : kNullStringId;
  std::optional<int32_t> pid =
      pv.has_pid() ? std::make_optional(pv.pid()) : std::nullopt;
  std::optional<int32_t> raw_prov_id =
      pv.has_provider_id() ? std::make_optional(pv.provider_id())
                           : std::nullopt;
  int32_t prov_id = ResolveProviderId(raw_prov_id, pid, auth_id, comp_id);

  auto& prov = provs_[prov_id];
  prov.provider_id = prov_id;
  if (pid && *pid > 0) {
    prov.owning_pid = *pid;
  }
  if (pv.has_uid()) {
    prov.uid = pv.uid();
  }
  if (!auth_id.is_null()) {
    prov.authority_id = auth_id;
  }
  if (!comp_id.is_null()) {
    prov.component_name_id = comp_id;
  }

  int64_t seq_id = pv.has_seq_id() ? pv.seq_id() : -1;
  ProvMutation pm;
  pm.ts = ts;
  pm.seq_id = seq_id;
  pm.pid = pid;
  pm.owner_only = true;
  std::optional<int32_t> trigger_bind_id;
  if (slice_name == "provider_published" || slice_name == "provider_died") {
    pm.owner_only = false;
    pm.published = (slice_name == "provider_published");
  } else if (slice_name == "provider_acquired" ||
             slice_name == "provider_released" ||
             slice_name == "provider_connection_updated") {
    bool is_update = (slice_name == "provider_connection_updated");
    bool is_acq = (slice_name == "provider_acquired") || is_update;
    int32_t caller_pid = pv.has_caller_pid() ? pv.caller_pid() : 0;
    int32_t bind_id = 0;
    if (pv.has_bind_id() && pv.bind_id() != 0) {
      bind_id = pv.bind_id();
      prov_bind_by_pair_[{caller_pid, prov_id}] = bind_id;
    } else {
      auto key = std::make_pair(caller_pid, prov_id);
      auto it = prov_bind_by_pair_.find(key);
      if (it != prov_bind_by_pair_.end()) {
        bind_id = it->second;
      } else {
        bind_id = next_synthetic_id_--;
        prov_bind_by_pair_[key] = bind_id;
      }
    }
    auto& pb = prov_binds_[bind_id];
    pb.bind_id = bind_id;
    trigger_bind_id = bind_id;
    if (is_acq || pb.client_pid == 0) {
      pb.client_pid = caller_pid;
    }
    if (pv.has_caller_uid()) {
      pb.client_uid = pv.caller_uid();
    }
    pb.provider_id = prov_id;
    ProvBindMutation pbm;
    pbm.ts = ts;
    pbm.seq_id = seq_id;
    pbm.acquired = is_acq;
    pbm.update_only = is_update;
    pbm.stable = pv.has_is_stable() ? (pv.is_stable() != 0) : true;
    pb.mutations.push_back(pbm);
  }
  if (!pm.owner_only || pid) {
    prov.mutations.push_back(pm);
  }

  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (seq_id >= 0) {
    tr.seq_id = seq_id;
  }
  tr.kind = context_->storage->InternString(
      slice_name.empty() ? base::StringView("provider_state_changed")
                         : slice_name);
  tr.action = tr.kind;
  tr.component_name = !auth_id.is_null() ? auth_id : comp_id;
  if (pid) {
    tr.target_pid = *pid;
  }
  if (pv.has_uid()) {
    tr.target_uid = pv.uid();
  }
  if (pv.has_caller_pid()) {
    tr.caller_pid = pv.caller_pid();
  }
  if (pv.has_caller_uid()) {
    tr.caller_uid = pv.caller_uid();
  }
  if (!auth_id.is_null() && !comp_id.is_null()) {
    if (pv.has_is_stable()) {
      std::string d = pv.component_name().ToStdString();
      d += pv.is_stable() != 0 ? " (stable)" : " (unstable)";
      tr.detail = context_->storage->InternString(base::StringView(d));
    } else {
      tr.detail = comp_id;
    }
  } else if (pv.has_is_stable()) {
    tr.detail = context_->storage->InternString(
        pv.is_stable() != 0 ? "stable" : "unstable");
  }
  tr.provider_id = prov_id;
  if (trigger_bind_id) {
    tr.bind_id = *trigger_bind_id;
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseBroadcastEvent(
    int64_t ts,
    base::StringView slice_name,
    protozero::ConstBytes bytes) {
  fb::AndroidBroadcastEvent::Decoder b(bytes);
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (b.has_seq_id()) {
    tr.seq_id = b.seq_id();
  }
  tr.kind = context_->storage->InternString(
      slice_name.empty() ? base::StringView("broadcast") : slice_name);
  if (b.has_action_name()) {
    tr.action = context_->storage->InternString(b.action_name());
  }
  if (b.has_receiver_component_name()) {
    tr.component_name =
        context_->storage->InternString(b.receiver_component_name());
  } else if (b.has_receiver_process_name()) {
    tr.component_name =
        context_->storage->InternString(b.receiver_process_name());
  }
  if (b.has_receiver_pid()) {
    tr.target_pid = b.receiver_pid();
  }
  if (b.has_receiver_uid()) {
    tr.target_uid = b.receiver_uid();
  }
  if (b.has_sender_pid()) {
    tr.caller_pid = b.sender_pid();
  }
  if (b.has_sender_uid()) {
    tr.caller_uid = b.sender_uid();
  }
  if (b.has_broadcast_type()) {
    tr.detail =
        InternEnum(context_, broadcast_type_cache_,
                   ".com.android.internal.BroadcastType", b.broadcast_type());
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseSelfBroadcastEvent(
    int64_t ts,
    base::StringView slice_name,
    protozero::ConstBytes bytes) {
  fb::AndroidSelfBroadcastEvent::Decoder b(bytes);
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (b.has_seq_id()) {
    tr.seq_id = b.seq_id();
  }
  tr.kind = context_->storage->InternString(
      slice_name.empty() ? base::StringView("self_broadcast_delivered")
                         : slice_name);
  if (b.has_action_name()) {
    tr.action = context_->storage->InternString(b.action_name());
  }
  if (b.has_pid()) {
    tr.target_pid = b.pid();
    tr.caller_pid = b.pid();
  }
  if (b.has_uid()) {
    tr.target_uid = b.uid();
    tr.caller_uid = b.uid();
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseActivityStateChange(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidActivityStateChangedEvent::Decoder a(bytes);
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (a.has_seq_id()) {
    tr.seq_id = a.seq_id();
  }
  tr.kind = context_->storage->InternString("activity_state_changed");
  if (a.has_component_name()) {
    tr.component_name = context_->storage->InternString(a.component_name());
  }
  if (a.has_pid()) {
    tr.target_pid = a.pid();
  }
  if (a.has_uid()) {
    tr.target_uid = a.uid();
  }
  std::string trans;
  if (a.has_prev_state()) {
    trans.append(a.prev_state().data, a.prev_state().size);
  }
  trans += " -> ";
  if (a.has_cur_state()) {
    trans.append(a.cur_state().data, a.cur_state().size);
  }
  tr.action = context_->storage->InternString(base::StringView(trans));
  if (a.has_reason()) {
    tr.detail = context_->storage->InternString(a.reason());
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseProcessStateTrigger(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidProcessStateTriggerEvent::Decoder t(bytes);
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  tables::AndroidProcessStateTriggerEventTable::Row tr;
  tr.ts = ts;
  if (t.has_seq_id()) {
    tr.seq_id = t.seq_id();
  }
  tr.kind = context_->storage->InternString("psc_trigger");
  if (t.has_trigger_name()) {
    tr.action = context_->storage->InternString(t.trigger_name());
  }
  if (t.has_process_name()) {
    tr.component_name = context_->storage->InternString(t.process_name());
  }
  if (t.has_pid()) {
    tr.target_pid = t.pid();
  }
  if (t.has_uid()) {
    tr.target_uid = t.uid();
  }
  if (t.has_caller_pid()) {
    tr.caller_pid = t.caller_pid();
  }
  if (t.has_caller_uid()) {
    tr.caller_uid = t.caller_uid();
  }
  if (t.has_detail()) {
    tr.detail = context_->storage->InternString(t.detail());
  } else if (t.has_bool_value()) {
    tr.detail =
        context_->storage->InternString(t.bool_value() ? "true" : "false");
  } else if (t.has_int_value()) {
    tr.detail = context_->storage->InternString(
        base::StringView(std::to_string(t.int_value())));
  }
  trigger_event_table_->Insert(tr);
}

void AndroidProcessStateTracker::ParseProcessStateDump(
    protozero::ConstBytes blob) {
  fb::AndroidProcessStateSnapshot::Decoder dump(blob);
  // The trace-start dump carries no proc_state/oom_score; record process name
  // and uid metadata for graph reconstruction, and return before touching
  // process_dump_.
  if (dump.dump_reason() ==
      fb::AndroidProcessStateSnapshot::DUMP_REASON_START) {
    for (auto it = dump.record(); it; ++it) {
      fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
      if (!rec.has_pid() || rec.pid() <= 0) {
        continue;
      }
      auto& proc = procs_[rec.pid()];
      proc.pid = rec.pid();
      if (rec.has_uid()) {
        proc.uid = rec.uid();
      }
      if (rec.has_process_name()) {
        proc.name_id = context_->storage->InternString(rec.process_name());
      }
    }
    return;
  }

  for (auto it = dump.record(); it; ++it) {
    fb::AndroidProcessStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    std::optional<UniquePid> opt_upid =
        context_->process_tracker->GetProcessOrNull(
            static_cast<uint32_t>(rec.pid()));
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

    auto& proc = procs_[rec.pid()];
    proc.pid = rec.pid();
    if (rec.has_uid()) {
      proc.uid = rec.uid();
    }
    if (rec.has_process_name()) {
      proc.name_id = context_->storage->InternString(rec.process_name());
    }
    proc.alive_at_end = true;
    proc.persistent = rec.has_persistent() && rec.persistent();
    proc.end_state.proc_state = v.proc_state;
    proc.end_state.oom_score = v.oom_score;
    proc.end_state.capability_flags = v.capability_flags;
    proc.end_state.persistent = proc.persistent;
  }

  for (auto it = dump.service(); it; ++it) {
    has_graph_data_ = true;
    fb::AndroidProcessStateSnapshot::Service::Decoder s(*it);
    StringId comp_id = s.has_component_name()
                           ? context_->storage->InternString(s.component_name())
                           : kNullStringId;
    std::optional<int32_t> pid =
        s.has_owning_pid() ? std::make_optional(s.owning_pid()) : std::nullopt;
    std::optional<int32_t> raw_id =
        s.has_service_id() ? std::make_optional(s.service_id()) : std::nullopt;
    int32_t svc_id = ResolveServiceId(raw_id, pid, comp_id);
    auto& svc = svcs_[svc_id];
    svc.svc_id = svc_id;
    if (pid && *pid > 0) {
      svc.owning_pid = *pid;
    }
    if (s.has_uid()) {
      svc.uid = s.uid();
    }
    if (!comp_id.is_null()) {
      svc.name_id = comp_id;
    }
    svc.alive_at_end = true;
    svc.end_state.is_foreground =
        s.has_is_foreground() ? s.is_foreground() : false;
    svc.end_state.fg_type =
        s.has_foreground_service_type() ? s.foreground_service_type() : 0;
    svc.end_state.start_requested =
        s.has_start_requested() ? s.start_requested() : false;
  }

  for (auto it = dump.service_binding(); it; ++it) {
    has_graph_data_ = true;
    fb::AndroidProcessStateSnapshot::ServiceBinding::Decoder b(*it);
    int32_t client_pid = b.has_client_pid() ? b.client_pid() : 0;
    StringId comp_id = b.has_component_name()
                           ? context_->storage->InternString(b.component_name())
                           : kNullStringId;
    std::optional<int32_t> host_pid =
        b.has_host_pid() ? std::make_optional(b.host_pid()) : std::nullopt;
    std::optional<int32_t> raw_svc_id =
        b.has_service_id() ? std::make_optional(b.service_id()) : std::nullopt;
    int32_t svc_id = ResolveServiceId(raw_svc_id, host_pid, comp_id);
    if (svc_id != 0) {
      auto& svc = svcs_[svc_id];
      svc.svc_id = svc_id;
      if (host_pid && *host_pid > 0 && !svc.owning_pid) {
        svc.owning_pid = *host_pid;
      }
      if (b.has_host_uid() && !svc.uid) {
        svc.uid = b.host_uid();
      }
      if (!comp_id.is_null() && svc.name_id.is_null()) {
        svc.name_id = comp_id;
      }
    }
    int32_t bind_id = (b.has_bind_id() && b.bind_id() != 0) ? b.bind_id() : 0;
    if (bind_id == 0) {
      auto key = std::make_pair(client_pid, svc_id);
      auto found = bind_by_pair_.find(key);
      bind_id =
          (found != bind_by_pair_.end()) ? found->second : next_synthetic_id_--;
      bind_by_pair_[key] = bind_id;
    }
    auto& rec = binds_[bind_id];
    rec.bind_id = bind_id;
    rec.client_pid = client_pid;
    if (b.has_client_uid()) {
      rec.client_uid = b.client_uid();
    }
    rec.service_id = svc_id;
    if (b.has_intent_bind_id()) {
      rec.intent_bind_id = b.intent_bind_id();
    }
    rec.alive_at_end = true;
    uint64_t flags = 0;
    if (b.has_bind_flags_32()) {
      flags |= static_cast<uint32_t>(b.bind_flags_32());
    }
    if (b.has_bind_flags_32_to_64()) {
      flags |=
          (static_cast<uint64_t>(static_cast<uint32_t>(b.bind_flags_32_to_64()))
           << 32);
    }
    rec.end_flags = flags;
    rec.end_fg = (flags & 0x04000000ULL) != 0;
    if (b.has_intent_action()) {
      rec.end_action_id = context_->storage->InternString(b.intent_action());
    }
  }

  for (auto it = dump.provider(); it; ++it) {
    has_graph_data_ = true;
    fb::AndroidProcessStateSnapshot::Provider::Decoder pv(*it);
    StringId auth_id = pv.has_authority()
                           ? context_->storage->InternString(pv.authority())
                           : kNullStringId;
    StringId comp_id =
        pv.has_component_name()
            ? context_->storage->InternString(pv.component_name())
            : kNullStringId;
    std::optional<int32_t> pid = pv.has_owning_pid()
                                     ? std::make_optional(pv.owning_pid())
                                     : std::nullopt;
    std::optional<int32_t> raw_id = pv.has_provider_id()
                                        ? std::make_optional(pv.provider_id())
                                        : std::nullopt;
    int32_t prov_id = ResolveProviderId(raw_id, pid, auth_id, comp_id);
    auto& prov = provs_[prov_id];
    prov.provider_id = prov_id;
    if (pid && *pid > 0) {
      prov.owning_pid = *pid;
    }
    if (pv.has_uid()) {
      prov.uid = pv.uid();
    }
    if (!auth_id.is_null()) {
      prov.authority_id = auth_id;
    }
    if (!comp_id.is_null()) {
      prov.component_name_id = comp_id;
    }
    prov.alive_at_end = true;
  }

  for (auto it = dump.provider_binding(); it; ++it) {
    has_graph_data_ = true;
    fb::AndroidProcessStateSnapshot::ProviderBinding::Decoder pb(*it);
    int32_t client_pid = pb.has_client_pid() ? pb.client_pid() : 0;
    StringId auth_id = pb.has_authority()
                           ? context_->storage->InternString(pb.authority())
                           : kNullStringId;
    StringId comp_id =
        pb.has_component_name()
            ? context_->storage->InternString(pb.component_name())
            : kNullStringId;
    std::optional<int32_t> host_pid =
        pb.has_host_pid() ? std::make_optional(pb.host_pid()) : std::nullopt;
    std::optional<int32_t> raw_prov_id =
        pb.has_provider_id() ? std::make_optional(pb.provider_id())
                             : std::nullopt;
    int32_t prov_id =
        ResolveProviderId(raw_prov_id, host_pid, auth_id, comp_id);
    if (prov_id != 0) {
      auto& prov = provs_[prov_id];
      prov.provider_id = prov_id;
      if (host_pid && *host_pid > 0 && !prov.owning_pid) {
        prov.owning_pid = *host_pid;
      }
      if (pb.has_host_uid() && !prov.uid) {
        prov.uid = pb.host_uid();
      }
      if (!auth_id.is_null() && prov.authority_id.is_null()) {
        prov.authority_id = auth_id;
      }
      if (!comp_id.is_null() && prov.component_name_id.is_null()) {
        prov.component_name_id = comp_id;
      }
    }
    int32_t bind_id =
        (pb.has_bind_id() && pb.bind_id() != 0) ? pb.bind_id() : 0;
    if (bind_id == 0) {
      auto key = std::make_pair(client_pid, prov_id);
      auto found = prov_bind_by_pair_.find(key);
      bind_id = (found != prov_bind_by_pair_.end()) ? found->second
                                                    : next_synthetic_id_--;
      prov_bind_by_pair_[key] = bind_id;
    }
    auto& rec = prov_binds_[bind_id];
    rec.bind_id = bind_id;
    rec.client_pid = client_pid;
    if (pb.has_client_uid()) {
      rec.client_uid = pb.client_uid();
    }
    rec.provider_id = prov_id;
    rec.alive_at_end = true;
    rec.end_stable = pb.has_is_stable() ? pb.is_stable() : true;
  }
}

void AndroidProcessStateTracker::ParseFreezerEvent(
    int64_t ts,
    protozero::ConstBytes bytes) {
  fb::AndroidFreezerEvent::Decoder evt(bytes);
  if (!evt.has_pid()) {
    return;
  }
  min_ts_ = std::min(min_ts_, ts);
  max_ts_ = std::max(max_ts_, ts);
  std::optional<UniquePid> opt_upid =
      context_->process_tracker->GetProcessOrNull(
          static_cast<uint32_t>(evt.pid()));
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

void AndroidProcessStateTracker::ParseFreezerDump(protozero::ConstBytes blob) {
  fb::AndroidFreezerStateSnapshot::Decoder dump(blob);
  for (auto it = dump.record(); it; ++it) {
    fb::AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    std::optional<UniquePid> opt_upid =
        context_->process_tracker->GetProcessOrNull(
            static_cast<uint32_t>(rec.pid()));
    if (!opt_upid) {
      continue;
    }
    FreezerStateValues v;
    v.upid = *opt_upid;
    // Note: android.util.proto.ProtoOutputStream ignores/omits 0 data points
    // during serialization on Android, so unset fields represent UFR_NONE (0).
    v.unfreeze_reason =
        rec.has_unfreeze_reason()
            ? static_cast<int32_t>(rec.unfreeze_reason())
            : static_cast<int32_t>(fb::UnfreezeReason::UFR_NONE);
    freezer_dump_[v.upid] = v;
  }
}

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

  if (!has_graph_data_) {
    return;
  }

  // Enrich ProcRecords with process name / uid / start_ts / end_ts from
  // TraceStorage's process table if not already populated.
  // GetProcessOrNull() skips processes that already ended, so also index the
  // process table by pid (latest upid wins) for processes that died in-trace.
  std::map<int32_t, UniquePid> upid_by_pid;
  {
    const auto& pt = context_->storage->process_table();
    for (uint32_t i = 0; i < pt.row_count(); ++i) {
      upid_by_pid[static_cast<int32_t>(pt[i].pid())] = i;
    }
  }
  for (auto& [pid, proc] : procs_) {
    if (pid <= 0) {
      continue;
    }
    std::optional<UniquePid> opt_upid =
        context_->process_tracker->GetProcessOrNull(static_cast<uint32_t>(pid));
    if (!opt_upid) {
      auto it = upid_by_pid.find(pid);
      if (it == upid_by_pid.end()) {
        continue;
      }
      opt_upid = it->second;
    }
    auto p_row = context_->storage->process_table()[*opt_upid];
    if (proc.name_id.is_null() && p_row.name().has_value()) {
      proc.name_id = *p_row.name();
    }
    if (proc.uid == 0 && p_row.uid().has_value()) {
      proc.uid = static_cast<int32_t>(*p_row.uid());
    }
    if (p_row.start_ts().has_value()) {
      ProcMutation birth;
      birth.ts = *p_row.start_ts();
      birth.kind = ProcMutation::kBirth;
      proc.mutations.push_back(birth);
    }
    if (p_row.end_ts().has_value()) {
      ProcMutation death;
      death.ts = *p_row.end_ts();
      death.kind = ProcMutation::kDeath;
      death.non_ams = true;
      proc.mutations.push_back(death);
    }
  }

  // Step 1: Sort mutations chronologically per entity and walk backwards from
  // end-of-trace state to determine initial state at the start of the trace.
  struct ActiveProc {
    bool alive = false;
    ProcState state;
  };
  std::map<int32_t, ActiveProc> cur_procs;
  for (auto& [pid, proc] : procs_) {
    // The kernel/binder-death process end can be seen a pass
    // before AMS drops the process record (crash) or seconds after (kill).
    // When AMS's own process_state_died is present for the same incarnation,
    // it matches the graph AMS sees; keep the others only as a fallback.
    constexpr int64_t kSameDeathWindowNs = 60ll * 1000 * 1000 * 1000;
    std::vector<int64_t> ams_deaths;
    for (const auto& m : proc.mutations) {
      if (m.kind == ProcMutation::kDeath && !m.non_ams) {
        ams_deaths.push_back(m.ts);
      }
    }
    if (!ams_deaths.empty()) {
      proc.mutations.erase(
          std::remove_if(proc.mutations.begin(), proc.mutations.end(),
                         [&](const ProcMutation& m) {
                           if (!m.non_ams) {
                             return false;
                           }
                           for (int64_t ts : ams_deaths) {
                             if (std::abs(ts - m.ts) < kSameDeathWindowNs) {
                               return true;
                             }
                           }
                           return false;
                         }),
          proc.mutations.end());
    }
    std::sort(proc.mutations.begin(), proc.mutations.end(),
              [](const ProcMutation& a, const ProcMutation& b) {
                return a.ts < b.ts;
              });
    bool alive = proc.alive_at_end;
    ProcState st = proc.end_state;
    for (auto it = proc.mutations.rbegin(); it != proc.mutations.rend(); ++it) {
      if (it->kind == ProcMutation::kBirth) {
        alive = false;
      } else if (it->kind == ProcMutation::kDeath) {
        alive = true;
        if (it->prev.proc_state) {
          st.proc_state = it->prev.proc_state;
        }
        if (it->prev.oom_score) {
          st.oom_score = it->prev.oom_score;
        }
        if (it->prev.capability_flags) {
          st.capability_flags = it->prev.capability_flags;
        }
      } else if (it->kind == ProcMutation::kStateChange) {
        // Coming from NONEXISTENT means the process was born here.
        alive = !IsNonexistent(it->prev.proc_state);
        if (!alive) {
          continue;
        }
        if (it->prev.proc_state) {
          st.proc_state = it->prev.proc_state;
        }
        if (it->prev.oom_score) {
          st.oom_score = it->prev.oom_score;
        }
        if (it->prev.capability_flags) {
          st.capability_flags = it->prev.capability_flags;
        }
      }
    }
    cur_procs[pid] = {alive, st};
  }

  struct ActiveSvc {
    bool alive = false;
    SvcState state;
    std::optional<int32_t> owner;
  };
  auto to_owner = [](std::optional<int32_t> pid) -> std::optional<int32_t> {
    return (pid && *pid > 0) ? pid : std::nullopt;
  };
  std::map<int32_t, ActiveSvc> cur_svcs;
  for (auto& [svc_id, svc] : svcs_) {
    std::sort(
        svc.mutations.begin(), svc.mutations.end(),
        [](const SvcMutation& a, const SvcMutation& b) { return a.ts < b.ts; });
    bool alive = svc.alive_at_end;
    SvcState st = svc.end_state;
    std::optional<int32_t> owner = svc.owning_pid;
    for (auto it = svc.mutations.rbegin(); it != svc.mutations.rend(); ++it) {
      if (it->pid) {
        owner = to_owner(it->pid);
      }
      switch (it->kind) {
        case SvcMutation::kOwner:
        case SvcMutation::kCreate:
          break;
        case SvcMutation::kPublish:
          alive = false;
          break;
        case SvcMutation::kDestroy:
          alive = true;
          break;
        case SvcMutation::kStart:
          // A first start (start_id 1) creates/starts the record; before it the
          // service only exists if bound (handled at emission time).
          alive = it->start_id > 1;
          st.start_requested = false;
          break;
        case SvcMutation::kStop:
          alive = true;
          st.start_requested = true;
          break;
        case SvcMutation::kFgsStart:
          alive = true;
          st.is_foreground = false;
          st.fg_type = 0;
          break;
        case SvcMutation::kFgsTypeChange:
          // Was already foreground before; previous type is unknown here and
          // is restored by the forward replay of the earlier fgs event.
          alive = true;
          st.is_foreground = true;
          break;
        case SvcMutation::kFgsStop:
          alive = true;
          st.is_foreground = true;
          if (it->fg_type != 0) {
            st.fg_type = it->fg_type;
          }
          break;
      }
    }
    cur_svcs[svc_id] = {alive, st, owner};
  }

  struct ActiveBind {
    bool alive = false;
    uint64_t flags = 0;
    bool fg = false;
    StringId action_id = kNullStringId;
  };
  std::map<int32_t, ActiveBind> cur_binds;
  for (auto& [bind_id, b] : binds_) {
    std::sort(b.mutations.begin(), b.mutations.end(),
              [](const BindMutation& a, const BindMutation& b) {
                return a.ts < b.ts;
              });
    bool alive = b.alive_at_end;
    uint64_t flags = b.end_flags;
    bool fg = b.end_fg;
    StringId act = b.end_action_id;
    for (auto it = b.mutations.rbegin(); it != b.mutations.rend(); ++it) {
      if (it->flags_update) {
        alive = true;
      } else if (it->bound) {
        alive = false;
      } else {
        alive = true;
        if (it->flags != 0) {
          flags = it->flags;
          fg = it->fg;
        }
        if (!it->action_id.is_null()) {
          act = it->action_id;
        }
      }
    }
    cur_binds[bind_id] = {alive, flags, fg, act};
  }
  // A binding alive at trace start implies its service existed then too, even
  // if the service's own first event (e.g. a first start) says otherwise. Later
  // a service_destroy hides it again while its (serviceDead) connections
  // linger until the client unbinds.
  for (const auto& [bind_id, ab] : cur_binds) {
    if (ab.alive) {
      cur_svcs[binds_[bind_id].service_id].alive = true;
    }
  }

  struct ActiveProv {
    bool alive = false;
    std::optional<int32_t> owner;
  };
  std::map<int32_t, ActiveProv> cur_provs;
  for (auto& [prov_id, pv] : provs_) {
    std::sort(pv.mutations.begin(), pv.mutations.end(),
              [](const ProvMutation& a, const ProvMutation& b) {
                return a.ts < b.ts;
              });
    bool alive = pv.alive_at_end;
    std::optional<int32_t> owner = pv.owning_pid;
    for (auto it = pv.mutations.rbegin(); it != pv.mutations.rend(); ++it) {
      if (it->pid) {
        owner = to_owner(it->pid);
      }
      if (!it->owner_only) {
        alive = !it->published;
      }
    }
    cur_provs[prov_id] = {alive, owner};
  }

  struct ActiveProvBind {
    bool alive = false;
    bool stable = true;
  };
  std::map<int32_t, ActiveProvBind> cur_prov_binds;
  for (auto& [bind_id, pb] : prov_binds_) {
    std::sort(pb.mutations.begin(), pb.mutations.end(),
              [](const ProvBindMutation& a, const ProvBindMutation& b) {
                return a.ts < b.ts;
              });
    bool alive = pb.alive_at_end;
    bool stable = pb.end_stable;
    for (auto it = pb.mutations.rbegin(); it != pb.mutations.rend(); ++it) {
      if (it->update_only) {
        // Before a stability flip the connection was alive with the opposite
        // stability.
        alive = true;
        stable = !it->stable;
      } else {
        alive = !it->acquired;
        stable = it->stable;
      }
    }
    cur_prov_binds[bind_id] = {alive, stable};
  }

  auto emit_snapshot = [&](int64_t ts, std::optional<int64_t> seq_id,
                           const PassInfo* pass) {
    tables::AndroidProcessStateSnapshotTable::Row snap_row;
    snap_row.ts = ts;
    snap_row.seq_id = seq_id;
    if (pass) {
      if (pass->reason) {
        snap_row.reason =
            InternEnumShort(context_, reason_cache_,
                            ".com.android.internal.OomChangeReasonEnum",
                            "OOM_ADJ_REASON_", *pass->reason);
      }
      snap_row.is_full_update = pass->is_full_update;
      snap_row.top_pid = pass->top_pid;
      if (pass->top_proc_state) {
        snap_row.top_proc_state =
            InternEnumShort(context_, proc_state_cache_,
                            ".com.android.internal.ProcessStateEnum",
                            "PROCESS_STATE_", *pass->top_proc_state);
      }
      if (!pass->target_pids.is_null()) {
        snap_row.target_pids = pass->target_pids;
      }
    }
    auto snap_id = snapshot_table_->Insert(snap_row).id;
    uint32_t sid = snap_id.value;

    for (const auto& [pid, ap] : cur_procs) {
      if (!ap.alive) {
        continue;
      }
      const auto& proc = procs_[pid];
      tables::AndroidProcessStateProcessTable::Row pr;
      pr.snapshot_id = sid;
      pr.pid = pid;
      pr.uid = proc.uid;
      if (!proc.name_id.is_null()) {
        pr.name = proc.name_id;
      }
      pr.oom_score = ap.state.oom_score;
      if (ap.state.proc_state) {
        pr.proc_state =
            InternEnumShort(context_, proc_state_cache_,
                            ".com.android.internal.ProcessStateEnum",
                            "PROCESS_STATE_", *ap.state.proc_state);
      }
      if (ap.state.capability_flags) {
        pr.capabilities = InternCapabilities(*ap.state.capability_flags);
      }
      pr.persistent = (proc.persistent || ap.state.persistent) ? 1 : 0;
      process_table_->Insert(pr);
    }

    // Services with a live connection exist in AMS even when not running
    // (bound without AUTO_CREATE before creation, or after being destroyed).
    std::set<int32_t> bound_svcs;
    for (const auto& [bind_id, ab] : cur_binds) {
      if (!ab.alive) {
        continue;
      }
      const auto& b = binds_[bind_id];
      if (b.client_pid > 0 && !cur_procs[b.client_pid].alive) {
        continue;
      }
      bound_svcs.insert(b.service_id);
    }

    std::set<int32_t> emitted_svcs;
    for (const auto& [svc_id, as] : cur_svcs) {
      if (!as.alive && !bound_svcs.count(svc_id)) {
        continue;
      }
      const auto& svc = svcs_[svc_id];
      if (as.owner && !cur_procs[*as.owner].alive) {
        continue;
      }
      emitted_svcs.insert(svc_id);
      tables::AndroidProcessStateServiceTable::Row sr;
      sr.snapshot_id = sid;
      sr.svc_id = svc_id;
      sr.owning_pid = as.owner;
      sr.uid = svc.uid;
      if (!svc.name_id.is_null()) {
        sr.name = svc.name_id;
      }
      sr.is_foreground = as.state.is_foreground ? 1 : 0;
      sr.foreground_service_type = as.state.fg_type;
      sr.start_requested = as.state.start_requested ? 1 : 0;
      service_table_->Insert(sr);
    }

    for (const auto& [bind_id, ab] : cur_binds) {
      if (!ab.alive) {
        continue;
      }
      const auto& b = binds_[bind_id];
      if (b.client_pid > 0 && !cur_procs[b.client_pid].alive) {
        continue;
      }
      if (!emitted_svcs.count(b.service_id)) {
        continue;
      }
      tables::AndroidProcessStateServiceBindingTable::Row br;
      br.snapshot_id = sid;
      br.bind_id = bind_id;
      br.client_pid = b.client_pid;
      br.client_uid = b.client_uid;
      br.service_id = b.service_id;
      br.intent_bind_id = b.intent_bind_id;
      br.foreground = ab.fg ? 1 : 0;
      StringId fl = InternBindFlags(ab.flags);
      if (!fl.is_null()) {
        br.flags = fl;
      }
      if (!ab.action_id.is_null()) {
        br.intent_action = ab.action_id;
      }
      service_binding_table_->Insert(br);
    }

    std::set<int32_t> connected_provs;
    for (const auto& [bind_id, apb] : cur_prov_binds) {
      if (!apb.alive) {
        continue;
      }
      const auto& pb = prov_binds_[bind_id];
      if (pb.client_pid > 0 && !cur_procs[pb.client_pid].alive) {
        continue;
      }
      connected_provs.insert(pb.provider_id);
    }

    std::set<int32_t> emitted_provs;
    for (const auto& [prov_id, ap] : cur_provs) {
      if (!ap.alive && !connected_provs.count(prov_id)) {
        continue;
      }
      const auto& pv = provs_[prov_id];
      if (ap.owner && !cur_procs[*ap.owner].alive) {
        continue;
      }
      emitted_provs.insert(prov_id);
      tables::AndroidProcessStateProviderTable::Row pvr;
      pvr.snapshot_id = sid;
      pvr.provider_id = prov_id;
      pvr.owning_pid = ap.owner;
      pvr.uid = pv.uid;
      if (!pv.authority_id.is_null()) {
        pvr.authority = pv.authority_id;
      }
      if (!pv.component_name_id.is_null()) {
        pvr.component_name = pv.component_name_id;
      }
      provider_table_->Insert(pvr);
    }

    for (const auto& [bind_id, apb] : cur_prov_binds) {
      if (!apb.alive) {
        continue;
      }
      const auto& pb = prov_binds_[bind_id];
      if (pb.client_pid > 0 && !cur_procs[pb.client_pid].alive) {
        continue;
      }
      if (!emitted_provs.count(pb.provider_id)) {
        continue;
      }
      tables::AndroidProcessStateProviderBindingTable::Row pbr;
      pbr.snapshot_id = sid;
      pbr.bind_id = bind_id;
      pbr.client_pid = pb.client_pid;
      pbr.client_uid = pb.client_uid;
      pbr.provider_id = pb.provider_id;
      pbr.stable = apb.stable ? 1 : 0;
      provider_binding_table_->Insert(pbr);
    }
  };

  // Step 2: Emit initial snapshot (seq_id = 0) if any entity is alive at trace
  // start, or if there are no incremental passes (dump-only trace).
  int64_t init_ts =
      (min_ts_ < std::numeric_limits<int64_t>::max()) ? min_ts_ : 0;
  bool any_initial = false;
  for (const auto& [pid, ap] : cur_procs) {
    if (ap.alive) {
      any_initial = true;
      break;
    }
  }
  if (any_initial || passes_.empty()) {
    PassInfo init_pass;
    init_pass.seq_id = 0;
    init_pass.ts = init_ts;
    init_pass.is_full_update = 1;
    emit_snapshot(init_ts, 0, &init_pass);
  }

  // Step 3: Replay mutations forward across each oom_adjuster pass (ordered by
  // seq_id) and emit a snapshot after each pass.
  std::map<int32_t, size_t> proc_idx;
  std::map<int32_t, size_t> svc_idx;
  std::map<int32_t, size_t> bind_idx;
  std::map<int32_t, size_t> prov_idx;
  std::map<int32_t, size_t> prov_bind_idx;

  for (const auto& [seq_id, pass] : passes_) {
    int64_t pass_ts = pass.ts;
    for (const auto& [pid, proc] : procs_) {
      for (const auto& m : proc.mutations) {
        if (m.seq_id == seq_id) {
          pass_ts = std::max(pass_ts, m.ts);
        }
      }
    }

    for (auto& [pid, proc] : procs_) {
      size_t& idx = proc_idx[pid];
      while (idx < proc.mutations.size()) {
        const auto& m = proc.mutations[idx];
        if (m.seq_id > seq_id || (m.seq_id < 0 && m.ts > pass_ts)) {
          break;
        }
        auto& ap = cur_procs[pid];
        if (m.kind == ProcMutation::kBirth) {
          ap.alive = true;
        } else if (m.kind == ProcMutation::kDeath) {
          ap.alive = false;
        } else if (IsNonexistent(m.cur.proc_state)) {
          ap.alive = false;
        } else {
          ap.alive = true;
          if (m.cur.proc_state) {
            ap.state.proc_state = m.cur.proc_state;
          }
          if (m.cur.oom_score) {
            ap.state.oom_score = m.cur.oom_score;
          }
          if (m.cur.capability_flags) {
            ap.state.capability_flags = m.cur.capability_flags;
          }
        }
        ++idx;
      }
    }

    for (auto& [svc_id, svc] : svcs_) {
      size_t& idx = svc_idx[svc_id];
      while (idx < svc.mutations.size()) {
        const auto& m = svc.mutations[idx];
        if (m.seq_id > seq_id || (m.seq_id < 0 && m.ts > pass_ts)) {
          break;
        }
        auto& as = cur_svcs[svc_id];
        if (m.pid) {
          as.owner = to_owner(m.pid);
        }
        switch (m.kind) {
          case SvcMutation::kOwner:
            break;
          case SvcMutation::kCreate:
            as.alive = true;
            break;
          case SvcMutation::kPublish:
            as.alive = true;
            break;
          case SvcMutation::kDestroy:
            // The record survives (unhosted) while connections remain bound;
            // emission keeps it visible in that case.
            as.alive = false;
            as.owner = std::nullopt;
            as.state.is_foreground = false;
            as.state.fg_type = 0;
            break;
          case SvcMutation::kStart:
            as.alive = true;
            as.state.start_requested = true;
            break;
          case SvcMutation::kStop:
            as.state.start_requested = false;
            break;
          case SvcMutation::kFgsStart:
          case SvcMutation::kFgsTypeChange:
            as.alive = true;
            as.state.is_foreground = true;
            as.state.fg_type = m.fg_type;
            break;
          case SvcMutation::kFgsStop:
            as.state.is_foreground = false;
            as.state.fg_type = 0;
            break;
        }
        ++idx;
      }
    }

    for (auto& [bind_id, b] : binds_) {
      size_t& idx = bind_idx[bind_id];
      while (idx < b.mutations.size()) {
        const auto& m = b.mutations[idx];
        if (m.seq_id > seq_id || (m.seq_id < 0 && m.ts > pass_ts)) {
          break;
        }
        auto& ab = cur_binds[bind_id];
        ab.alive = m.bound;
        if (m.bound) {
          ab.flags = m.flags;
          ab.fg = m.fg;
          if (!m.action_id.is_null()) {
            ab.action_id = m.action_id;
          }
        }
        ++idx;
      }
    }

    for (auto& [prov_id, pv] : provs_) {
      size_t& idx = prov_idx[prov_id];
      while (idx < pv.mutations.size()) {
        const auto& m = pv.mutations[idx];
        if (m.seq_id > seq_id || (m.seq_id < 0 && m.ts > pass_ts)) {
          break;
        }
        auto& ap = cur_provs[prov_id];
        if (m.pid) {
          ap.owner = to_owner(m.pid);
        }
        if (!m.owner_only) {
          ap.alive = m.published;
        }
        ++idx;
      }
    }

    for (auto& [bind_id, pb] : prov_binds_) {
      size_t& idx = prov_bind_idx[bind_id];
      while (idx < pb.mutations.size()) {
        const auto& m = pb.mutations[idx];
        if (m.seq_id > seq_id || (m.seq_id < 0 && m.ts > pass_ts)) {
          break;
        }
        auto& apb = cur_prov_binds[bind_id];
        apb.alive = m.acquired;
        apb.stable = m.stable;
        ++idx;
      }
    }

    emit_snapshot(pass.ts, seq_id, &pass);
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
