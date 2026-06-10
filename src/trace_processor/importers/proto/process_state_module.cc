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

#include "src/trace_processor/importers/proto/process_state_module.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/android/process_state_data.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"

namespace perfetto::trace_processor {

namespace {
using ::perfetto::protos::pbzero::ProcessStateSnapshot;
using ::perfetto::protos::pbzero::TracePacket;
using ::protozero::ConstBytes;

// Look up an interned string by 1-based index. Returns kNullStringId if idx is
// 0 (proto-side sentinel for "absent") or out of range.
StringId LookupInterned(const std::vector<StringId>& interned, int32_t idx) {
  if (idx <= 0 || static_cast<size_t>(idx) >= interned.size()) {
    return kNullStringId;
  }
  return interned[static_cast<size_t>(idx)];
}
}  // namespace

ProcessStateModule::ProcessStateModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(TracePacket::kProcessStateSnapshotFieldNumber);
}

ProcessStateModule::~ProcessStateModule() = default;

void ProcessStateModule::ParseTracePacketData(
    const TracePacket::Decoder& decoder,
    int64_t ts,
    const TracePacketData&,
    uint32_t field_id) {
  if (field_id != TracePacket::kProcessStateSnapshotFieldNumber) {
    return;
  }
  ParseSnapshot(ts, decoder.process_state_snapshot());
}

void ProcessStateModule::ParseSnapshot(int64_t ts, ConstBytes blob) {
  ProcessStateSnapshot::Decoder snap(blob);

  // Index 0 is reserved for "absent". Build a 1-based table.
  std::vector<StringId> interned;
  interned.push_back(kNullStringId);
  for (auto it = snap.interned_strings(); it; ++it) {
    interned.push_back(
        context_->storage->InternString((*it).ToStdStringView()));
  }

  // Top-level snapshot row. Prefer the producer-supplied capture
  // timestamp (snapshot_boottime_ns) over the TracePacket emit time
  // when present — for batched / rate-limited configs, emit time can
  // be a measurable interval after the snapshot was actually captured,
  // which would misalign the snapshot against scheduling/binder
  // tracks in the UI.
  tables::ProcessStateSnapshotTable::Row snap_row;
  snap_row.ts =
      snap.has_snapshot_boottime_ns() ? snap.snapshot_boottime_ns() : ts;
  snap_row.oom_adj_reason = snap.oom_adj_reason();
  if (snap.has_global_state()) {
    protos::pbzero::GlobalState::Decoder global(snap.global_state());
    if (global.has_top_pid()) {
      snap_row.top_pid = global.top_pid();
    }
    // Device-wide context: capture every GlobalState scalar so the UI can show
    // exactly what the device saw at this snapshot (bools emit only when true,
    // so an absent flag means false / 0).
    snap_row.is_awake = global.is_awake() ? 1 : 0;
    snap_row.unlocking = global.unlocking() ? 1 : 0;
    snap_row.expanded_notification_shade =
        global.expanded_notification_shade() ? 1 : 0;
    snap_row.last_memory_level_normal =
        global.last_memory_level_normal() ? 1 : 0;
    if (global.has_top_process_state()) {
      snap_row.top_process_state = global.top_process_state();
    }
    if (global.has_home_pid()) {
      snap_row.home_pid = global.home_pid();
    }
    if (global.has_heavy_weight_pid()) {
      snap_row.heavy_weight_pid = global.heavy_weight_pid();
    }
    if (global.has_previous_pid()) {
      snap_row.previous_pid = global.previous_pid();
    }
    if (global.has_dozing_ui_pid()) {
      snap_row.dozing_ui_pid = global.dozing_ui_pid();
    }
    // Repeated allowlists: comma-join so nothing the device saw is dropped.
    auto join = [](auto it) {
      std::string out;
      for (; it; ++it) {
        if (!out.empty())
          out += ",";
        out += std::to_string(*it);
      }
      return out;
    };
    std::string backup = join(global.backup_pid());
    std::string idle = join(global.device_idle_allowlist_app_ids());
    std::string temp = join(global.temp_allowlist_app_ids());
    if (!backup.empty()) {
      snap_row.backup_pids =
          context_->storage->InternString(base::StringView(backup));
    }
    if (!idle.empty()) {
      snap_row.idle_allowlist_appids =
          context_->storage->InternString(base::StringView(idle));
    }
    if (!temp.empty()) {
      snap_row.temp_allowlist_appids =
          context_->storage->InternString(base::StringView(temp));
    }
  }
  snap_row.is_full =
      snap.kind() == protos::pbzero::ProcessStateSnapshot::KIND_ANCHOR ? 1 : 0;
  auto snap_id = context_->storage->mutable_process_state_snapshot_table()
                     ->Insert(snap_row)
                     .id;

  // Per-process rows.
  for (auto it = snap.process(); it; ++it) {
    protos::pbzero::ProcessRecord::Decoder pr(*it);

    tables::ProcessStateProcessTable::Row prow;
    prow.snapshot_id = snap_id;
    prow.pid = pr.pid();
    prow.uid = pr.uid();
    prow.user_id = pr.user_id();
    if (pr.has_process_name_idx()) {
      StringId s = LookupInterned(interned, pr.process_name_idx());
      if (!s.is_null())
        prow.process_name = s;
    }
    if (pr.has_package_name_idx()) {
      StringId s = LookupInterned(interned, pr.package_name_idx());
      if (!s.is_null())
        prow.package_name = s;
    }
    prow.lru_index = pr.lru_index();

    // ProtoOutputStream elides default values (0 / false) on the producer
    // side, so has_X() is unreliable as a "wasn't written" signal. Read
    // everything unconditionally and trust the proto3-style default semantics.
    prow.cur_adj = pr.cur_adj();
    prow.cur_raw_adj = pr.cur_raw_adj();
    prow.set_adj = pr.set_adj();
    prow.max_adj = pr.max_adj();
    prow.cur_proc_state = pr.cur_proc_state();
    prow.set_proc_state = pr.set_proc_state();
    prow.cur_raw_proc_state = pr.cur_raw_proc_state();
    prow.cur_capability = pr.cur_capability();
    prow.set_capability = pr.set_capability();
    prow.cur_sched_group = pr.cur_sched_group();
    prow.set_sched_group = pr.set_sched_group();
    prow.has_foreground_activities = pr.has_foreground_activities() ? 1 : 0;
    prow.has_top_ui = pr.has_top_ui() ? 1 : 0;
    prow.has_overlay_ui = pr.has_overlay_ui() ? 1 : 0;
    prow.has_shown_ui = pr.has_shown_ui() ? 1 : 0;
    prow.has_visible_activities = pr.has_visible_activities() ? 1 : 0;
    prow.has_started_services = pr.has_started_services() ? 1 : 0;
    prow.persistent = pr.persistent() ? 1 : 0;
    prow.isolated = pr.isolated() ? 1 : 0;
    prow.has_active_instrumentation = pr.has_active_instrumentation() ? 1 : 0;

    // The resolved oom-adj "why".
    if (pr.has_adj_type_idx()) {
      StringId s = LookupInterned(interned, pr.adj_type_idx());
      if (!s.is_null())
        prow.adj_type = s;
    }
    if (pr.has_adj_source_pid())
      prow.adj_source_pid = pr.adj_source_pid();
    if (pr.has_adj_target_pid())
      prow.adj_target_pid = pr.adj_target_pid();
    if (pr.has_adj_source_proc_state()) {
      prow.adj_source_proc_state = pr.adj_source_proc_state();
    }
    if (pr.has_rep_proc_state())
      prow.rep_proc_state = pr.rep_proc_state();
    if (pr.has_has_recent_tasks()) {
      prow.has_recent_tasks = pr.has_recent_tasks() ? 1 : 0;
    }
    if (pr.has_is_frozen())
      prow.is_frozen = pr.is_frozen() ? 1 : 0;
    if (pr.has_cached_adj())
      prow.cached_adj = pr.cached_adj();
    if (pr.has_cached_proc_state()) {
      prow.cached_proc_state = pr.cached_proc_state();
    }
    if (pr.has_cached_sched_group()) {
      prow.cached_sched_group = pr.cached_sched_group();
    }

    // --- additional process fields (materialise every proto field) ---
    prow.verified_adj = pr.verified_adj();
    prow.hosting_component_types = pr.hosting_component_types();
    prow.hosting_component_types_for_oom_adj =
        pr.hosting_component_types_for_oom_adj();
    prow.activity_state_flags = pr.activity_state_flags();
    prow.fg_service_types = pr.fg_service_types();
    prow.num_executing_services = pr.num_executing_services();
    prow.broadcast_receiver_sched_group = pr.broadcast_receiver_sched_group();
    prow.num_pending_receivers = pr.num_pending_receivers();
    prow.adj_type_code = pr.adj_type_code();
    prow.adj_seq = pr.adj_seq();
    prow.completed_adj_seq = pr.completed_adj_seq();
    prow.lru_seq = pr.lru_seq();
    prow.last_top_time_ms = pr.last_top_time_ms();
    prow.when_unimportant_ms = pr.when_unimportant_ms();
    prow.interaction_event_time_ms = pr.interaction_event_time_ms();
    prow.fg_interaction_time_ms = pr.fg_interaction_time_ms();
    prow.last_provider_time_ms = pr.last_provider_time_ms();
    prow.perceptible_task_stopped_time_ms =
        pr.perceptible_task_stopped_time_ms();
    prow.last_top_almost_perceptible_bind_request_uptime_ms =
        pr.last_top_almost_perceptible_bind_request_uptime_ms();
    prow.frozen_since_ms = pr.frozen_since_ms();
    prow.last_pss_kb = pr.last_pss_kb();
    prow.last_cached_pss_kb = pr.last_cached_pss_kb();
    prow.running_remote_animation = pr.running_remote_animation() ? 1 : 0;
    prow.forcing_to_important = pr.forcing_to_important() ? 1 : 0;
    prow.system_no_ui = pr.system_no_ui() ? 1 : 0;
    prow.has_foreground_services = pr.has_foreground_services() ? 1 : 0;
    prow.has_non_short_foreground_services =
        pr.has_non_short_foreground_services() ? 1 : 0;
    prow.has_executing_services = pr.has_executing_services() ? 1 : 0;
    prow.exec_services_fg = pr.exec_services_fg() ? 1 : 0;
    prow.has_client_activities = pr.has_client_activities() ? 1 : 0;
    prow.has_above_client = pr.has_above_client() ? 1 : 0;
    prow.has_top_started_almost_perceptible_services =
        pr.has_top_started_almost_perceptible_services() ? 1 : 0;
    prow.is_receiving_broadcast = pr.is_receiving_broadcast() ? 1 : 0;
    prow.background_restricted = pr.background_restricted() ? 1 : 0;
    prow.cur_bound_by_non_bg_restricted_app =
        pr.cur_bound_by_non_bg_restricted_app() ? 1 : 0;
    prow.is_sdk_sandbox = pr.is_sdk_sandbox() ? 1 : 0;
    prow.is_backup_target = pr.is_backup_target() ? 1 : 0;
    prow.cached_foreground_activities =
        pr.cached_foreground_activities() ? 1 : 0;
    if (pr.has_instrumentation_class_idx()) {
      StringId s = LookupInterned(interned, pr.instrumentation_class_idx());
      if (!s.is_null())
        prow.instrumentation_class = s;
    }
    if (pr.has_backup_agent_name_idx()) {
      StringId s = LookupInterned(interned, pr.backup_agent_name_idx());
      if (!s.is_null())
        prow.backup_agent_name = s;
    }
    if (pr.has_cached_adj_type_idx()) {
      StringId s = LookupInterned(interned, pr.cached_adj_type_idx());
      if (!s.is_null())
        prow.cached_adj_type = s;
    }
    context_->storage->mutable_process_state_process_table()->Insert(prow);
  }

  // Per-uid rows.
  for (auto it = snap.uid(); it; ++it) {
    protos::pbzero::UidRecord::Decoder ur(*it);

    tables::ProcessStateUidTable::Row urow;
    urow.snapshot_id = snap_id;
    urow.uid = ur.uid();
    urow.cur_proc_state = ur.cur_proc_state();
    urow.set_proc_state = ur.set_proc_state();
    urow.cur_capability = ur.cur_capability();
    urow.idle = ur.idle() ? 1 : 0;
    urow.ephemeral = ur.ephemeral() ? 1 : 0;

    // --- additional uid fields (materialise every proto field) ---
    urow.set_capability = ur.set_capability();
    urow.restriction_level = ur.restriction_level();
    urow.standby_bucket = ur.standby_bucket();
    urow.last_background_time_ms = ur.last_background_time_ms();
    urow.running = ur.running() ? 1 : 0;
    urow.device_idle_allowlisted = ur.device_idle_allowlisted() ? 1 : 0;
    urow.temp_allowlisted = ur.temp_allowlisted() ? 1 : 0;
    context_->storage->mutable_process_state_uid_table()->Insert(urow);
  }

  // Per-service rows.
  for (auto it = snap.service(); it; ++it) {
    protos::pbzero::ServiceRecord::Decoder sr(*it);

    tables::ProcessStateServiceTable::Row srow;
    srow.snapshot_id = snap_id;
    srow.service_id = sr.id();
    srow.owning_pid = sr.owning_pid();
    if (sr.has_short_name_idx()) {
      StringId s = LookupInterned(interned, sr.short_name_idx());
      if (!s.is_null())
        srow.short_name = s;
    }
    if (sr.has_package_name_idx()) {
      StringId s = LookupInterned(interned, sr.package_name_idx());
      if (!s.is_null())
        srow.package_name = s;
    }
    srow.is_foreground = sr.is_foreground() ? 1 : 0;
    srow.foreground_id = sr.foreground_id();
    srow.foreground_service_type = sr.foreground_service_type();
    srow.is_short_fgs = sr.is_short_fgs() ? 1 : 0;
    srow.start_requested = sr.start_requested() ? 1 : 0;
    srow.delayed = sr.delayed() ? 1 : 0;
    srow.delayed_stop = sr.delayed_stop() ? 1 : 0;
    srow.execute_nesting = sr.execute_nesting();
    srow.execute_fg = sr.execute_fg() ? 1 : 0;
    srow.restart_count = sr.restart_count();
    srow.crash_count = sr.crash_count();
    srow.is_isolated = sr.is_isolated() ? 1 : 0;

    // --- additional service fields (materialise every proto field) ---
    srow.restart_delay_ms = sr.restart_delay_ms();
    srow.short_fgs_start_uptime_ms = sr.short_fgs_start_uptime_ms();
    srow.short_fgs_timeout_uptime_ms = sr.short_fgs_timeout_uptime_ms();
    srow.short_fgs_proc_state_demote_uptime_ms =
        sr.short_fgs_proc_state_demote_uptime_ms();
    srow.short_fgs_anr_uptime_ms = sr.short_fgs_anr_uptime_ms();
    srow.executing_start_uptime_ms = sr.executing_start_uptime_ms();
    srow.last_activity_uptime_ms = sr.last_activity_uptime_ms();
    srow.restart_uptime_ms = sr.restart_uptime_ms();
    srow.next_restart_uptime_ms = sr.next_restart_uptime_ms();
    srow.created_from_fg = sr.created_from_fg() ? 1 : 0;
    if (sr.has_class_name_idx()) {
      StringId s = LookupInterned(interned, sr.class_name_idx());
      if (!s.is_null())
        srow.class_name = s;
    }
    context_->storage->mutable_process_state_service_table()->Insert(srow);
  }

  // Per-binding rows.
  for (auto it = snap.binding(); it; ++it) {
    protos::pbzero::ServiceBinding::Decoder b(*it);

    tables::ProcessStateBindingTable::Row brow;
    brow.snapshot_id = snap_id;
    brow.binding_id = b.id();
    brow.client_pid = b.client_pid();
    brow.client_uid = b.client_uid();
    if (b.has_client_process_name_idx()) {
      StringId s = LookupInterned(interned, b.client_process_name_idx());
      if (!s.is_null())
        brow.client_process_name = s;
    }
    brow.service_id = b.service_id();
    brow.flags = b.flags();
    brow.flag_auto_create = b.flag_auto_create() ? 1 : 0;
    brow.flag_foreground_service = b.flag_foreground_service() ? 1 : 0;
    brow.flag_not_foreground = b.flag_not_foreground() ? 1 : 0;
    brow.flag_above_client = b.flag_above_client() ? 1 : 0;
    brow.flag_allow_oom_management = b.flag_allow_oom_management() ? 1 : 0;
    brow.flag_waive_priority = b.flag_waive_priority() ? 1 : 0;
    brow.flag_important = b.flag_important() ? 1 : 0;
    brow.flag_adjust_with_activity = b.flag_adjust_with_activity() ? 1 : 0;
    brow.flag_include_capabilities = b.flag_include_capabilities() ? 1 : 0;
    brow.client_label = b.client_label();
    brow.service_dead = b.service_dead() ? 1 : 0;

    // --- additional binding fields (materialise every proto field) ---
    brow.ongoing_calls = b.ongoing_calls();
    brow.effective_proc_state = b.effective_proc_state();
    brow.effective_capability = b.effective_capability();
    brow.flag_not_perceptible = b.flag_not_perceptible() ? 1 : 0;
    brow.flag_almost_perceptible = b.flag_almost_perceptible() ? 1 : 0;
    brow.flag_treat_like_visible_fgs = b.flag_treat_like_visible_fgs() ? 1 : 0;
    brow.flag_treat_like_activity = b.flag_treat_like_activity() ? 1 : 0;
    brow.flag_schedule_like_top_app = b.flag_schedule_like_top_app() ? 1 : 0;
    brow.flag_bypass_power_network_restrictions =
        b.flag_bypass_power_network_restrictions() ? 1 : 0;
    brow.flag_allow_background_activity_starts =
        b.flag_allow_background_activity_starts() ? 1 : 0;
    brow.has_bound_service_session = b.has_bound_service_session() ? 1 : 0;
    context_->storage->mutable_process_state_binding_table()->Insert(brow);
  }

  // Per-provider rows.
  for (auto it = snap.provider(); it; ++it) {
    protos::pbzero::ContentProviderRecord::Decoder cpr(*it);

    tables::ProcessStateProviderTable::Row prow;
    prow.snapshot_id = snap_id;
    prow.provider_id = cpr.id();
    prow.owning_pid = cpr.owning_pid();
    if (cpr.has_authority_idx()) {
      StringId s = LookupInterned(interned, cpr.authority_idx());
      if (!s.is_null())
        prow.authority = s;
    }
    if (cpr.has_package_name_idx()) {
      StringId s = LookupInterned(interned, cpr.package_name_idx());
      if (!s.is_null())
        prow.package_name = s;
    }
    if (cpr.has_class_name_idx()) {
      StringId s = LookupInterned(interned, cpr.class_name_idx());
      if (!s.is_null())
        prow.class_name = s;
    }
    prow.external_handle_count = cpr.external_handle_count();
    prow.launched = cpr.launched() ? 1 : 0;

    context_->storage->mutable_process_state_provider_table()->Insert(prow);
  }

  // Per-provider-binding rows.
  for (auto it = snap.provider_binding(); it; ++it) {
    protos::pbzero::ContentProviderBinding::Decoder cpc(*it);

    tables::ProcessStateProviderBindingTable::Row brow;
    brow.snapshot_id = snap_id;
    brow.binding_id = cpc.id();
    brow.provider_id = cpc.provider_id();
    brow.client_pid = cpc.client_pid();
    brow.stable_count = cpc.stable_count();
    brow.unstable_count = cpc.unstable_count();
    brow.dead = cpc.dead() ? 1 : 0;
    brow.waiting = cpc.waiting() ? 1 : 0;

    // --- additional provider_binding fields (materialise every proto field)
    // ---
    brow.last_ref_uptime_ms = cpc.last_ref_uptime_ms();
    context_->storage->mutable_process_state_provider_binding_table()->Insert(
        brow);
  }
}

}  // namespace perfetto::trace_processor
