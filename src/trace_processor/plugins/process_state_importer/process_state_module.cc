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

#include "src/trace_processor/plugins/process_state_importer/process_state_module.h"

#include <cstdint>
#include <string>

#include "perfetto/ext/base/string_view.h"

#include "src/trace_processor/plugins/process_state_importer/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/track_event.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto::trace_processor {

namespace fb = ::com::android::internal::pbzero;
using ::perfetto::protos::pbzero::TracePacket;
using ::perfetto::protos::pbzero::TrackEvent;

namespace {

// "PBZERO_UNKNOWN_ENUM_VALUE" is what the generated <Enum>_Name() returns for a
// value not in the enum.
constexpr char kUnknownEnum[] = "PBZERO_UNKNOWN_ENUM_VALUE";

// Intern an enum name with its common prefix stripped (e.g.
// "PROCESS_STATE_TOP" -> "TOP"); falls back to the raw number if unknown.
StringId InternStripped(TraceStorage* storage,
                        const char* name,
                        size_t prefix_len,
                        int32_t raw) {
  base::StringView sv(name);
  if (sv == base::StringView(kUnknownEnum)) {
    return storage->InternString(base::StringView(std::to_string(raw)));
  }
  if (sv.size() > prefix_len) {
    sv = base::StringView(name + prefix_len, sv.size() - prefix_len);
  }
  return storage->InternString(sv);
}

}  // namespace

ProcessStateModule::ProcessStateModule(
    ProtoImporterModuleContext* mc,
    TraceProcessorContext* context,
    tables::AndroidProcessStateSnapshotTable* snapshot_table,
    tables::AndroidProcessStateProcessTable* process_table,
    tables::AndroidProcessStateServiceTable* service_table,
    tables::AndroidProcessStateServiceBindingTable* service_binding_table,
    tables::AndroidProcessStateProviderTable* provider_table,
    tables::AndroidProcessStateProviderBindingTable* provider_binding_table)
    : ProtoImporterModule(mc),
      context_(context),
      snapshot_table_(snapshot_table),
      process_table_(process_table),
      service_table_(service_table),
      service_binding_table_(service_binding_table),
      provider_table_(provider_table),
      provider_binding_table_(provider_binding_table) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kProcessStateSnapshotFieldNumber);
  // Also receive process / service state-change track events, to rebuild the
  // graph from the event stream (dispatched from ParseTrackEvent).
  RegisterForField(TracePacket::kTrackEventFieldNumber);
}

ProcessStateModule::~ProcessStateModule() = default;

StringId ProcessStateModule::ProcStateName(int32_t value) {
  return InternStripped(
      context_->storage.get(),
      fb::ProcessStateEnum_Name(static_cast<fb::ProcessStateEnum>(value)),
      sizeof("PROCESS_STATE_") - 1, value);
}

StringId ProcessStateModule::ReasonName(int32_t value) {
  return InternStripped(
      context_->storage.get(),
      fb::OomChangeReasonEnum_Name(static_cast<fb::OomChangeReasonEnum>(value)),
      sizeof("OOM_ADJ_REASON_") - 1, value);
}

StringId ProcessStateModule::CapabilityNames(int32_t flags) {
  auto* storage = context_->storage.get();
  if (flags == 0) {
    return storage->InternString("none");
  }
  // Capability flag bit i corresponds to ProcessCapabilityEnum value i + 1.
  std::string out;
  for (int i = 0; i < 31; i++) {
    if ((flags & (1 << i)) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " | ";
    }
    const char* name = fb::ProcessCapabilityEnum_Name(
        static_cast<fb::ProcessCapabilityEnum>(i + 1));
    if (base::StringView(name) == base::StringView(kUnknownEnum)) {
      out += "0x" + std::to_string(1 << i);
    } else {
      out += (name + (sizeof("PROCESS_CAPABILITY_") - 1));
    }
  }
  return storage->InternString(base::StringView(out));
}

// Writes the current running model as one snapshot, stamped at the START of the
// oom-adj pass that produced it (its first event). The slice extends to the
// next snapshot on the UI side, so the state stays "current" until it changes.
void ProcessStateModule::EmitSnapshot() {
  if (current_ts_ == -1) {
    return;
  }
  tables::AndroidProcessStateSnapshotTable::Row snap_row;
  snap_row.ts = seq_start_ts_ != -1 ? seq_start_ts_ : current_ts_;
  if (current_reason_.has_value()) {
    snap_row.reason = ReasonName(*current_reason_);
  }
  uint32_t snapshot_id = snapshot_table_->Insert(snap_row).id.value;

  for (const auto& [pid, row] : processes_) {
    auto r = row;
    r.snapshot_id = snapshot_id;
    process_table_->Insert(r);
  }
  for (const auto& [svc_id, row] : services_) {
    auto r = row;
    r.snapshot_id = snapshot_id;
    service_table_->Insert(r);
  }
  for (const auto& [bind_id, row] : service_bindings_) {
    auto r = row;
    r.snapshot_id = snapshot_id;
    service_binding_table_->Insert(r);
  }
  for (const auto& [provider_id, row] : providers_) {
    auto r = row;
    r.snapshot_id = snapshot_id;
    provider_table_->Insert(r);
  }
  for (const auto& [bind_id, row] : provider_bindings_) {
    auto r = row;
    r.snapshot_id = snapshot_id;
    provider_binding_table_->Insert(r);
  }
  graph_changed_ = false;
}

void ProcessStateModule::OnEventsFullyExtracted() {
  if (graph_changed_) {
    EmitSnapshot();
  }
}

void ProcessStateModule::ParseField(const ParseFieldArgs& args) {
  const int64_t ts = args.ts;
  auto* storage = context_->storage.get();

  // Path 2: reconstruct the graph from process / service state-change track
  // events. We keep a running model and emit a snapshot whenever the oom-adj
  // sequence id advances.
  if (args.field.id() == TracePacket::kTrackEventFieldNumber) {
    TrackEvent::Decoder track_event(
        args.field.Cast<TracePacket::kTrackEvent>());

    auto process_state_field = track_event.FindField(
        fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
    if (process_state_field.valid()) {
      fb::AndroidProcessStateChangedEvent::Decoder p(
          process_state_field.as_bytes());
      // A new oom-adj sequence id ends the previous adjuster pass: flush the
      // graph as it stood at that point before applying this pass's changes.
      if (p.has_seq_id() && p.seq_id() != current_seq_id_) {
        if (current_seq_id_ != -1 && graph_changed_) {
          EmitSnapshot();
        }
        current_seq_id_ = p.seq_id();
        seq_start_ts_ = ts;  // first event of the new oom-adj pass
      }
      current_ts_ = ts;
      // Capture the pass reason (after any flush above, so the just-emitted
      // snapshot kept the previous pass's reason). All events in a pass share
      // it.
      if (p.has_reason()) {
        current_reason_ = static_cast<int32_t>(p.reason());
      }

      int32_t pid = p.pid();
      auto& row = processes_[pid];
      row.pid = pid;
      if (p.has_uid()) {
        row.uid = p.uid();
      }
      row.oom_score = p.cur_oom_score();
      if (p.has_cur_proc_state()) {
        row.proc_state =
            ProcStateName(static_cast<int32_t>(p.cur_proc_state()));
      }
      if (p.has_cur_capability_flags()) {
        row.capabilities = CapabilityNames(p.cur_capability_flags());
      }
      graph_changed_ = true;
    }

    auto service_state_field = track_event.FindField(
        fb::FrameworksBaseTrackEvent::kServiceStateChangedEventFieldNumber);
    if (service_state_field.valid()) {
      fb::AndroidServiceStateChangedEvent::Decoder s(
          service_state_field.as_bytes());
      current_ts_ = ts;
      if (s.has_action()) {
        if (s.action() == fb::AndroidServiceStateChangedEvent::ACTION_BIND) {
          // Intern the service by (host uid, component) so repeated binds to
          // the same service reuse one synthetic id.
          auto key = std::make_pair(
              s.uid(),
              storage->InternString(s.component_name().ToStdStringView()));
          int32_t svc_id;
          auto found = service_to_id_.find(key);
          if (found == service_to_id_.end()) {
            svc_id = next_svc_id_++;
            service_to_id_[key] = svc_id;
            tables::AndroidProcessStateServiceTable::Row s_row;
            s_row.svc_id = svc_id;
            s_row.owning_pid = s.pid();
            s_row.name = key.second;
            services_[svc_id] = s_row;
          } else {
            svc_id = found->second;
          }

          tables::AndroidProcessStateServiceBindingTable::Row b_row;
          b_row.client_pid = s.caller_pid();
          b_row.service_id = svc_id;
          if (s.has_flag_foreground_service()) {
            b_row.foreground = s.flag_foreground_service() ? 1 : 0;
          }
          service_bindings_[s.bind_id()] = b_row;
          graph_changed_ = true;
        } else if (s.action() ==
                       fb::AndroidServiceStateChangedEvent::ACTION_UNBIND ||
                   s.action() ==
                       fb::AndroidServiceStateChangedEvent::ACTION_DISCONNECT) {
          // The connection is gone (explicit unbind or teardown), drop the
          // edge.
          service_bindings_.erase(s.bind_id());
          graph_changed_ = true;
        }
      }
    }

    auto provider_state_field = track_event.FindField(
        fb::FrameworksBaseTrackEvent::kProviderStateChangedEventFieldNumber);
    if (provider_state_field.valid()) {
      fb::AndroidProviderStateChangedEvent::Decoder pv(
          provider_state_field.as_bytes());
      current_ts_ = ts;
      if (pv.has_action()) {
        if (pv.action() == fb::AndroidProviderStateChangedEvent::ACTION_BIND) {
          // Intern the provider by (host uid, authority) so repeated binds to
          // the same provider reuse one synthetic id.
          auto key = std::make_pair(
              pv.uid(),
              storage->InternString(pv.authority().ToStdStringView()));
          int32_t provider_id;
          auto found = provider_to_id_.find(key);
          if (found == provider_to_id_.end()) {
            provider_id = next_provider_id_++;
            provider_to_id_[key] = provider_id;
            tables::AndroidProcessStateProviderTable::Row p_row;
            p_row.provider_id = provider_id;
            p_row.owning_pid = pv.pid();
            p_row.authority = key.second;
            providers_[provider_id] = p_row;
          } else {
            provider_id = found->second;
          }

          tables::AndroidProcessStateProviderBindingTable::Row b_row;
          b_row.client_pid = pv.caller_pid();
          b_row.provider_id = provider_id;
          if (pv.has_is_stable()) {
            b_row.stable = pv.is_stable() ? 1 : 0;
          }
          provider_bindings_[pv.bind_id()] = b_row;
          graph_changed_ = true;
        } else if (pv.action() ==
                       fb::AndroidProviderStateChangedEvent::ACTION_UNBIND ||
                   pv.action() == fb::AndroidProviderStateChangedEvent::
                                      ACTION_DISCONNECT) {
          // The connection is gone (explicit unbind or teardown), drop the
          // edge.
          provider_bindings_.erase(pv.bind_id());
          graph_changed_ = true;
        }
      }
    }
    return;
  }

  // Path 1: a one-shot ProcessStateSnapshot (dumpsys activity). Seed the
  // running model from it and emit immediately, sharing EmitSnapshot.
  if (args.field.id() !=
      fb::FrameworksBaseTracePacket::kProcessStateSnapshotFieldNumber) {
    return;
  }
  fb::ProcessStateSnapshot::Decoder snapshot(
      args.field.Cast<fb::FrameworksBaseTracePacket::kProcessStateSnapshot>());

  current_ts_ = ts;
  seq_start_ts_ = ts;  // a one-shot snapshot is a point (dur 0)
  current_seq_id_ = -1;
  current_reason_.reset();  // a one-shot snapshot has no oom-adj pass reason
  processes_.clear();
  services_.clear();
  service_to_id_.clear();
  service_bindings_.clear();
  providers_.clear();
  provider_to_id_.clear();
  provider_bindings_.clear();

  for (auto it = snapshot.processes(); it; ++it) {
    fb::ProcessStateSnapshot::Process::Decoder p(*it);
    tables::AndroidProcessStateProcessTable::Row row;
    row.pid = p.pid();
    row.uid = p.uid();
    row.name = storage->InternString(p.name().ToStdStringView());
    // oom_score 0 (FOREGROUND_APP_ADJ) is meaningful; ProtoOutputStream omits a
    // 0-valued singleton, so populate it unconditionally rather than NULL.
    row.oom_score = p.oom_score();
    if (p.has_proc_state()) {
      row.proc_state = ProcStateName(p.proc_state());
    }
    if (p.has_capability_flags()) {
      row.capabilities = CapabilityNames(p.capability_flags());
    }
    if (p.has_persistent()) {
      row.persistent = p.persistent() ? 1 : 0;
    }
    processes_[row.pid] = row;
  }

  for (auto it = snapshot.services(); it; ++it) {
    fb::ProcessStateSnapshot::Service::Decoder s(*it);
    tables::AndroidProcessStateServiceTable::Row row;
    row.svc_id = s.id();
    if (s.has_owning_pid()) {
      row.owning_pid = s.owning_pid();
    }
    if (s.has_name()) {
      row.name = storage->InternString(s.name().ToStdStringView());
    }
    services_[row.svc_id] = row;
  }

  int32_t synth = 0;
  for (auto it = snapshot.service_bindings(); it; ++it) {
    fb::ProcessStateSnapshot::ServiceBinding::Decoder b(*it);
    tables::AndroidProcessStateServiceBindingTable::Row row;
    row.client_pid = b.client_pid();
    row.service_id = b.service_id();
    if (b.has_foreground()) {
      row.foreground = b.foreground() ? 1 : 0;
    }
    service_bindings_[synth++] = row;
  }

  for (auto it = snapshot.providers(); it; ++it) {
    fb::ProcessStateSnapshot::Provider::Decoder p(*it);
    tables::AndroidProcessStateProviderTable::Row row;
    row.provider_id = p.id();
    if (p.has_owning_pid()) {
      row.owning_pid = p.owning_pid();
    }
    if (p.has_authority()) {
      row.authority = storage->InternString(p.authority().ToStdStringView());
    }
    providers_[row.provider_id] = row;
  }

  synth = 0;
  for (auto it = snapshot.provider_bindings(); it; ++it) {
    fb::ProcessStateSnapshot::ProviderBinding::Decoder b(*it);
    tables::AndroidProcessStateProviderBindingTable::Row row;
    row.client_pid = b.client_pid();
    row.provider_id = b.provider_id();
    if (b.has_stable()) {
      row.stable = b.stable() ? 1 : 0;
    }
    provider_bindings_[synth++] = row;
  }

  graph_changed_ = true;
  EmitSnapshot();
}

}  // namespace perfetto::trace_processor
