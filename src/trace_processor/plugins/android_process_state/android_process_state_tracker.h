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

#ifndef SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_
#define SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_

#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor {
class TraceProcessorContext;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::android_process_state {

StringId InternEnum(TraceProcessorContext* context,
                    DescriptorPool::CachedDescriptor& cache,
                    const char* enum_name,
                    int32_t value);

// Tracks process states, freezer states, binding graph snapshots, and causal
// trigger events from incremental track events and trace-stop dump snapshots.
class AndroidProcessStateTracker {
 public:
  AndroidProcessStateTracker(
      TraceProcessorContext* context,
      tables::AndroidProcessStateTable* process_state_table,
      tables::AndroidFreezerStateTable* freezer_state_table,
      tables::AndroidProcessStateSnapshotTable* snapshot_table,
      tables::AndroidProcessStateProcessTable* process_table,
      tables::AndroidProcessStateServiceTable* service_table,
      tables::AndroidProcessStateServiceBindingTable* service_binding_table,
      tables::AndroidProcessStateProviderTable* provider_table,
      tables::AndroidProcessStateProviderBindingTable* provider_binding_table,
      tables::AndroidProcessStateTriggerEventTable* trigger_event_table);

  // TrackEvent extensions:
  void ParseProcessStateChange(int64_t ts,
                               std::optional<UniqueTid> utid,
                               protozero::ConstBytes bytes);
  void ParseProcessStateDied(int64_t ts, protozero::ConstBytes bytes);
  void ParseFreezerEvent(int64_t ts, protozero::ConstBytes bytes);
  void ParseOomAdjusterPass(int64_t ts, protozero::ConstBytes bytes);
  void ParseServiceStateChange(int64_t ts,
                               base::StringView slice_name,
                               protozero::ConstBytes bytes);
  void ParseFgServiceStateChange(int64_t ts,
                                 base::StringView slice_name,
                                 protozero::ConstBytes bytes);
  void ParseProviderStateChange(int64_t ts,
                                base::StringView slice_name,
                                protozero::ConstBytes bytes);
  void ParseBroadcastEvent(int64_t ts,
                           base::StringView slice_name,
                           protozero::ConstBytes bytes);
  void ParseSelfBroadcastEvent(int64_t ts,
                               base::StringView slice_name,
                               protozero::ConstBytes bytes);
  void ParseActivityStateChange(int64_t ts, protozero::ConstBytes bytes);
  void ParseProcessStateTrigger(int64_t ts, protozero::ConstBytes bytes);

  // TracePacket fields:
  void ParseProcessStateDump(protozero::ConstBytes blob);
  void ParseFreezerDump(protozero::ConstBytes blob);

  // Synthesizes the per-process initial-state rows and flushes the
  // reconstructed binding graph snapshots.
  void Finalize();

 private:
  struct ProcessStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> oom_score;
    std::optional<int32_t> proc_state;
    std::optional<int32_t> capability_flags;
    std::optional<int32_t> process_group;
  };

  struct FreezerStateValues {
    UniquePid upid = 0;
    std::optional<int32_t> unfreeze_reason;
  };

  struct EarliestDelta {
    int64_t ts = std::numeric_limits<int64_t>::max();
    ProcessStateValues values;
  };

  struct ProcState {
    std::optional<int32_t> oom_score;
    std::optional<int32_t> proc_state;
    std::optional<int32_t> capability_flags;
    bool persistent = false;
  };

  struct ProcMutation {
    int64_t ts = 0;
    int64_t seq_id = -1;
    enum Kind { kBirth, kDeath, kStateChange } kind = kStateChange;
    ProcState prev;
    ProcState cur;
    // kDeath from the process table end (kernel exit or binder death) rather
    // than AMS's process_state_died.
    bool non_ams = false;
  };

  struct ProcRecord {
    int32_t pid = 0;
    int32_t uid = 0;
    StringId name_id = kNullStringId;
    bool persistent = false;
    bool alive_at_end = false;
    ProcState end_state;
    std::vector<ProcMutation> mutations;
  };

  struct SvcState {
    bool is_foreground = false;
    int32_t fg_type = 0;
    bool start_requested = false;
  };

  struct SvcMutation {
    int64_t ts = 0;
    int64_t seq_id = -1;
    enum Kind {
      kPublish,
      kDestroy,
      kStart,
      kStop,
      kFgsStart,
      kFgsStop,
      // Already-foreground service changed its foreground_service_type.
      kFgsTypeChange,
      // No liveness change; only carries the hosting pid reported by the
      // event (e.g. bind/unbind/restart_scheduled).
      kOwner,
      // Service (re)created in a host process (realStartServiceLocked).
      kCreate,
    } kind = kStart;
    int32_t fg_type = 0;
    // ServiceRecord start id for kStart (1 == first start of this record).
    int32_t start_id = 0;
    // Hosting pid reported by the event. A ServiceRecord outlives its host
    // process (it is restarted in a new pid), so the owner varies over time.
    // <= 0 means the service currently has no host process.
    std::optional<int32_t> pid;
  };

  struct SvcRecord {
    int32_t svc_id = 0;
    std::optional<int32_t> owning_pid;
    std::optional<int32_t> uid;
    StringId name_id = kNullStringId;
    bool alive_at_end = false;
    SvcState end_state;
    std::vector<SvcMutation> mutations;
  };

  struct BindMutation {
    int64_t ts = 0;
    int64_t seq_id = -1;
    bool bound = true;
    uint64_t flags = 0;
    bool fg = false;
    StringId action_id = kNullStringId;
    // service_binding_flags_updated: the binding stays alive, only its flags
    // change.
    bool flags_update = false;
  };

  struct BindRecord {
    int32_t bind_id = 0;
    int32_t client_pid = 0;
    std::optional<int32_t> client_uid;
    int32_t service_id = 0;
    std::optional<int32_t> intent_bind_id;
    bool alive_at_end = false;
    uint64_t end_flags = 0;
    bool end_fg = false;
    StringId end_action_id = kNullStringId;
    std::vector<BindMutation> mutations;
  };

  struct ProvMutation {
    int64_t ts = 0;
    int64_t seq_id = -1;
    bool published = true;
    // If true, this mutation only carries the hosting pid (no liveness
    // change), e.g. from acquire/release events.
    bool owner_only = false;
    // Hosting pid reported by the event; <= 0 means no host process yet.
    std::optional<int32_t> pid;
  };

  struct ProvRecord {
    int32_t provider_id = 0;
    std::optional<int32_t> owning_pid;
    std::optional<int32_t> uid;
    StringId authority_id = kNullStringId;
    StringId component_name_id = kNullStringId;
    bool alive_at_end = false;
    std::vector<ProvMutation> mutations;
  };

  struct ProvBindMutation {
    int64_t ts = 0;
    int64_t seq_id = -1;
    bool acquired = true;
    bool stable = true;
    // provider_connection_updated: stable <-> unstable transition of a live
    // connection (it stays acquired).
    bool update_only = false;
  };

  struct ProvBindRecord {
    int32_t bind_id = 0;
    int32_t client_pid = 0;
    std::optional<int32_t> client_uid;
    int32_t provider_id = 0;
    bool alive_at_end = false;
    bool end_stable = true;
    std::vector<ProvBindMutation> mutations;
  };

  struct PassInfo {
    int64_t seq_id = 0;
    int64_t ts = std::numeric_limits<int64_t>::max();
    std::optional<int32_t> reason;
    std::optional<int32_t> is_full_update;
    std::optional<int32_t> top_pid;
    std::optional<int32_t> top_proc_state;
    StringId target_pids = kNullStringId;
  };

  void UpdateInitialStateFromDelta(int64_t ts,
                                   const ProcessStateValues& prev_state);
  std::map<UniquePid, ProcessStateValues> ComputeInitialProcessStates() const;
  void EmitInitialProcessStateRow(const ProcessStateValues& v);
  void EmitInitialFreezerRow(const FreezerStateValues& v);

  PassInfo& EnsurePass(int64_t seq_id, int64_t ts);
  int32_t ResolveServiceId(std::optional<int32_t> raw_svc_id,
                           std::optional<int32_t> pid,
                           StringId name_id);
  int32_t ResolveProviderId(std::optional<int32_t> raw_prov_id,
                            std::optional<int32_t> pid,
                            StringId authority_id,
                            StringId comp_id);
  StringId InternCapabilities(int32_t mask);
  StringId InternBindFlags(uint64_t flags);

  TraceProcessorContext* const context_;
  tables::AndroidProcessStateTable* const process_state_table_;
  tables::AndroidFreezerStateTable* const freezer_state_table_;
  tables::AndroidProcessStateSnapshotTable* const snapshot_table_;
  tables::AndroidProcessStateProcessTable* const process_table_;
  tables::AndroidProcessStateServiceTable* const service_table_;
  tables::AndroidProcessStateServiceBindingTable* const service_binding_table_;
  tables::AndroidProcessStateProviderTable* const provider_table_;
  tables::AndroidProcessStateProviderBindingTable* const
      provider_binding_table_;
  tables::AndroidProcessStateTriggerEventTable* const trigger_event_table_;

  DescriptorPool::CachedDescriptor proc_state_cache_;
  DescriptorPool::CachedDescriptor process_group_cache_;
  DescriptorPool::CachedDescriptor reason_cache_;
  DescriptorPool::CachedDescriptor unfreeze_reason_cache_;
  DescriptorPool::CachedDescriptor capability_cache_;
  DescriptorPool::CachedDescriptor bind_flag_cache_;
  DescriptorPool::CachedDescriptor bind_flag64_cache_;
  DescriptorPool::CachedDescriptor broadcast_type_cache_;

  // Map of upid -> earliest observed delta transition during the trace.
  std::map<UniquePid, EarliestDelta> earliest_prev_;
  // Map of upid -> final process state from the trace-stop dump snapshot.
  std::map<UniquePid, ProcessStateValues> process_dump_;
  // Map of upid -> final freezer state from the trace-stop dump snapshot.
  std::map<UniquePid, FreezerStateValues> freezer_dump_;

  // Graph reconstruction state:
  std::map<int32_t, ProcRecord> procs_;
  std::map<int32_t, SvcRecord> svcs_;
  std::map<int32_t, BindRecord> binds_;
  std::map<int32_t, ProvRecord> provs_;
  std::map<int32_t, ProvBindRecord> prov_binds_;
  std::map<int64_t, PassInfo> passes_;

  std::map<std::pair<int32_t, uint32_t>, int32_t> svc_by_key_;
  std::map<std::pair<int32_t, uint32_t>, int32_t> prov_by_key_;
  std::map<std::pair<int32_t, int32_t>, int32_t> bind_by_pair_;
  std::map<std::pair<int32_t, int32_t>, int32_t> prov_bind_by_pair_;
  int32_t next_synthetic_id_ = -1;
  int64_t min_ts_ = std::numeric_limits<int64_t>::max();
  int64_t max_ts_ = 0;
  bool has_graph_data_ = false;
};

}  // namespace perfetto::trace_processor::android_process_state

#endif  // SRC_TRACE_PROCESSOR_PLUGINS_ANDROID_PROCESS_STATE_ANDROID_PROCESS_STATE_TRACKER_H_
