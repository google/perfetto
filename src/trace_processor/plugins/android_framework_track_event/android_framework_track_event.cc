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

#include "src/trace_processor/plugins/android_framework_track_event/android_framework_track_event.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "protos/perfetto/trace/track_event/process_descriptor.pbzero.h"
#include "protos/perfetto/trace/track_event/track_descriptor.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/import_logs_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_framework_track_event/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor::android_framework_track_event {
namespace {

using FBTE = ::com::android::internal::pbzero::FrameworksBaseTrackEvent;
using FBTP = ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using AndroidProcessStartEvent =
    ::com::android::internal::pbzero::AndroidProcessStartEvent;
using AndroidProcessDiedEvent =
    ::com::android::internal::pbzero::AndroidProcessDiedEvent;
using AndroidBinderDiedEvent =
    ::com::android::internal::pbzero::AndroidBinderDiedEvent;
using AndroidProcessStateSnapshot =
    ::com::android::internal::pbzero::AndroidProcessStateSnapshot;
using AndroidTrackEventProcessTable = tables::AndroidTrackEventProcessTable;
using TracePacket = ::perfetto::protos::pbzero::TracePacket;
using TrackDescriptor = ::perfetto::protos::pbzero::TrackDescriptor;

// __intrinsic_android_track_event_process, with a single row per process.
// Shared by Parser and StartDumpModule.
class AndroidTrackEventProcessTableHolder {
 public:
  explicit AndroidTrackEventProcessTableHolder(StringPool* pool)
      : table_(pool) {}

  AndroidTrackEventProcessTable& table() { return table_; }

  // Returns the row for |upid|, inserting one if needed.
  AndroidTrackEventProcessTable::RowReference GetOrInsertRow(UniquePid upid) {
    auto it_and_ins =
        upid_to_row_.Insert(upid, AndroidTrackEventProcessTable::Id{0});
    if (it_and_ins.second) {
      AndroidTrackEventProcessTable::Row row;
      row.upid = upid;
      *it_and_ins.first = table_.Insert(row).id;
    }
    return table_[*it_and_ins.first];
  }

  // Returns the row for |upid|, if any.
  std::optional<AndroidTrackEventProcessTable::RowReference> FindRow(
      UniquePid upid) {
    auto* id = upid_to_row_.Find(upid);
    if (!id) {
      return std::nullopt;
    }
    return table_[*id];
  }

  // Sets |start_seq_id| on |row| and indexes |row| by it. All writes of
  // start_seq_id must go through here to keep the index in sync.
  void SetStartSeqId(AndroidTrackEventProcessTable::RowReference row,
                     int64_t start_seq_id) {
    if (row.start_seq_id().has_value() && *row.start_seq_id() != start_seq_id) {
      start_seq_id_to_row_.Erase(*row.start_seq_id());
    }
    row.set_start_seq_id(start_seq_id);
    start_seq_id_to_row_[start_seq_id] = row.id();
  }

  // Returns the row currently holding |start_seq_id|, if any.
  std::optional<AndroidTrackEventProcessTable::RowReference>
  FindRowByStartSeqId(std::optional<int64_t> start_seq_id) {
    if (!start_seq_id) {
      return std::nullopt;
    }
    auto* id = start_seq_id_to_row_.Find(*start_seq_id);
    if (!id) {
      return std::nullopt;
    }
    auto row = table_[*id];
    // The row may since have been given a newer start_seq_id (same upid).
    if (row.start_seq_id() != *start_seq_id) {
      return std::nullopt;
    }
    return row;
  }

  // Drops the start_seq_id index. start_seq_ids are only unique within one
  // system_server instance: they restart from scratch when it restarts.
  void ClearStartSeqIdIndex() { start_seq_id_to_row_.Clear(); }

  // Returns the upid of every process with a row.
  std::vector<UniquePid> AllUpids() {
    std::vector<UniquePid> upids;
    for (auto it = upid_to_row_.GetIterator(); it; ++it) {
      upids.push_back(it.key());
    }
    return upids;
  }

 private:
  AndroidTrackEventProcessTable table_;
  base::FlatHashMap<UniquePid, AndroidTrackEventProcessTable::Id> upid_to_row_;
  // start_seq_id is unique per process start, so unlike the pid it still
  // identifies the process after it has died and its pid has been reused.
  base::FlatHashMap<int64_t, AndroidTrackEventProcessTable::Id>
      start_seq_id_to_row_;
};

// Tracks the current system_server instance and handles its restarts (e.g.
// after a crash): the previous system_server and every app process it started
// die with it without any death event. They are ended when a new instance is
// reported, otherwise a later process reusing one of their pids would be
// merged into the stale process. Their start_seq_ids are forgotten too, as
// they restart from scratch with system_server.
//
// This class only handles restarts: detecting them is left to the sources of
// system_server instances. Today the only one is Parser, which reports the
// process emitting framework process events (only system_server emits them)
// through OnInferredSystemServerInstance(). A restart detected this way is
// inferred, so it is reported as a data loss stat: an explicit signal emitted
// by system_server itself (e.g. at its start) should get its own entry point
// sharing HandleRestart(), without that stat.
class SystemServerTracker {
 public:
  SystemServerTracker(TraceProcessorContext* context,
                      AndroidTrackEventProcessTableHolder* table)
      : trace_context_(context), table_(table) {}

  // Records that |upid| was active at |ts| (e.g. it wrote a track
  // descriptor), so that a restart is placed at the first activity of the new
  // instance rather than at its first framework process event, which can be
  // seconds later.
  void OnProcessActive(UniquePid upid, int64_t ts) {
    auto [it, inserted] = first_active_ts_.Insert(upid, ts);
    if (!inserted && ts < *it) {
      *it = ts;
    }
  }

  // Reports that |upid| is the current system_server instance, inferred from
  // its framework events at |ts|. If it is a new instance, handles the
  // restart of system_server and records it as a data loss. Can be called
  // any number of times for the same instance.
  void OnInferredSystemServerInstance(UniquePid upid, int64_t ts) {
    if (upid_ && *upid_ != upid) {
      int64_t restart_ts = ts;
      if (auto* first_active = first_active_ts_.Find(upid);
          first_active && *first_active < restart_ts) {
        restart_ts = *first_active;
      }
      const auto& processes = trace_context_->storage->process_table();
      int64_t old_pid = processes[*upid_].pid();
      int64_t new_pid = processes[upid].pid();
      trace_context_->import_logs_tracker->RecordParserLog(
          stats::android_system_server_restart_inferred, restart_ts,
          [&](ArgsInserter& inserter) {
            inserter.AddArg(old_pid_key_, Variadic::Integer(old_pid));
            inserter.AddArg(new_pid_key_, Variadic::Integer(new_pid));
          });
      HandleRestart(upid, restart_ts);
    }
    upid_ = upid;
  }

 private:
  // Ends the previous system_server and every known process at |ts|, except
  // |new_system_server|, and forgets their start_seq_ids.
  void HandleRestart(UniquePid new_system_server, int64_t ts) {
    for (UniquePid upid : table_->AllUpids()) {
      if (upid != new_system_server) {
        EndProcessIfLive(upid, ts);
      }
    }
    // The previous system_server may no longer be the live process for its
    // pid, if the new instance reuses it: it is dead nevertheless.
    EndProcessIfLive(*upid_, ts);
    auto old = (*trace_context_->storage->mutable_process_table())[*upid_];
    if (!old.end_ts().has_value()) {
      old.set_end_ts(ts);
    }
    table_->ClearStartSeqIdIndex();
  }

  // Ends |upid| at |ts| if it is still the live process for its pid.
  void EndProcessIfLive(UniquePid upid, int64_t ts) {
    auto process = trace_context_->storage->process_table()[upid];
    if (process.end_ts().has_value()) {
      return;
    }
    int64_t pid = process.pid();
    if (trace_context_->process_tracker->GetProcessOrNull(pid) == upid) {
      trace_context_->process_tracker->EndThread(ts, pid);
    }
  }

  TraceProcessorContext* trace_context_;
  AndroidTrackEventProcessTableHolder* table_;
  const StringId old_pid_key_ =
      trace_context_->storage->InternString("old_system_server_pid");
  const StringId new_pid_key_ =
      trace_context_->storage->InternString("new_system_server_pid");
  // The current system_server, if known.
  std::optional<UniquePid> upid_;
  // The earliest activity of each process.
  base::FlatHashMap<UniquePid, int64_t> first_active_ts_;
};

// Records AndroidProcessStartEvent, AndroidBinderDiedEvent and
// AndroidProcessDiedEvent into __intrinsic_android_track_event_process.
class Parser : public TrackEventExtensionParser {
 public:
  Parser(TrackEventExtensionParserContext* extension_parser_context,
         TraceProcessorContext* context,
         AndroidTrackEventProcessTableHolder* table,
         SystemServerTracker* system_server)
      : TrackEventExtensionParser(extension_parser_context),
        trace_context_(context),
        table_(table),
        system_server_(system_server) {
    RegisterTrackEventExtension(FBTE::kProcessStartEventFieldNumber);
    RegisterTrackEventExtension(FBTE::kBinderDiedEventFieldNumber);
    RegisterTrackEventExtension(FBTE::kProcessDiedEventFieldNumber);
  }
  ~Parser() override = default;

  Result OnTrackEventField(const TrackEventExtensionField& field,
                           const TrackEventFieldContext& event) override {
    if (event.row_kind != TrackEventFieldContext::RowKind::kSlice) {
      return Result::kIgnored;
    }
    int64_t ts = event.ts;
    // These events are only emitted by system_server, on its process track:
    // the emitting process is the current system_server.
    if (event.has_upid()) {
      system_server_->OnInferredSystemServerInstance(event.upid, ts);
    }
    switch (field.id()) {
      case FBTE::kProcessStartEventFieldNumber:
        HandleProcessStart(field.Cast<FBTE::kProcessStartEvent>(), ts);
        break;
      case FBTE::kBinderDiedEventFieldNumber:
        HandleBinderDied(field.Cast<FBTE::kBinderDiedEvent>(), ts);
        break;
      case FBTE::kProcessDiedEventFieldNumber:
        HandleProcessDied(field.Cast<FBTE::kProcessDiedEvent>());
        break;
      default:
        break;
    }
    return Result::kHandled;
  }

 private:
  void SetProcessMetadata(UniquePid upid, protozero::ConstBytes process_start) {
    AndroidProcessStartEvent::Decoder evt(process_start);
    if (evt.has_uid()) {
      trace_context_->process_tracker->SetProcessUid(
          upid, static_cast<uint32_t>(evt.uid()));
    }
    if (evt.has_process_name()) {
      trace_context_->process_tracker->UpdateProcessName(
          upid, trace_context_->storage->InternString(evt.process_name()),
          ProcessNamePriority::kOther);
    }
  }

  void HandleProcessStart(protozero::ConstBytes data, int64_t ts) {
    AndroidProcessStartEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }
    auto pid = static_cast<uint32_t>(evt.pid());
    // The pid may still map to an older process whose death was missed (e.g.
    // a dropped AndroidBinderDiedEvent). A process with another start_seq_id
    // cannot be the one starting now: end it so that a new process is created
    // rather than merged into it.
    if (auto old = trace_context_->process_tracker->GetProcessOrNull(pid);
        old && evt.has_start_seq_id()) {
      auto old_row = table_->FindRow(*old);
      if (old_row && old_row->start_seq_id().has_value() &&
          *old_row->start_seq_id() != evt.start_seq_id()) {
        trace_context_->process_tracker->EndThread(ts, pid);
      }
    }
    UniquePid upid = trace_context_->process_tracker->GetOrCreateProcess(pid);
    SetProcessMetadata(upid, data);

    auto row = table_->GetOrInsertRow(upid);
    if (evt.has_start_seq_id()) {
      table_->SetStartSeqId(row, evt.start_seq_id());
    }
    if (evt.has_package_uid()) {
      row.set_package_uid(evt.package_uid());
    }
    if (evt.has_caller_uid()) {
      row.set_caller_uid(evt.caller_uid());
    }
    if (evt.has_defining_uid()) {
      row.set_defining_uid(evt.defining_uid());
    }
    if (!row.fw_start_ts().has_value()) {
      row.set_fw_start_ts(ts);
    }
    if (evt.has_trigger_type()) {
      row.set_trigger_type(
          InternEnum(trigger_type_cache_, ".com.android.internal.TriggerType",
                     static_cast<int32_t>(evt.trigger_type())));
    }
    if (evt.has_hosting_type()) {
      row.set_hosting_type(
          InternEnum(hosting_type_cache_, ".com.android.internal.HostingTypeId",
                     static_cast<int32_t>(evt.hosting_type())));
    }
    if (evt.has_hosting_name()) {
      row.set_hosting_name(
          trace_context_->storage->InternString(evt.hosting_name()));
    }
    if (evt.has_bind_application_delay_ms()) {
      row.set_bind_application_delay_ms(evt.bind_application_delay_ms());
    }
    if (evt.has_process_start_delay_ms()) {
      row.set_process_start_delay_ms(evt.process_start_delay_ms());
    }
  }

  // Returns the row of the process a death event refers to, identified by
  // |start_seq_id| and cross-checked against |pid|. Falls back to the live
  // process for |pid| when the event has no start_seq_id or it is unknown.
  // Returns nullopt if the process is unknown or the two values disagree.
  //
  // start_seq_id check comes first because AndroidProcessDiedEvent can arrive
  // up to ~15s after AndroidBinderDiedEvent, when the pid has already been
  // ended and possibly reused by another process.
  std::optional<AndroidTrackEventProcessTable::RowReference>
  ResolveDyingProcess(uint32_t pid, std::optional<int64_t> start_seq_id) {
    if (auto row = table_->FindRowByStartSeqId(start_seq_id)) {
      auto process = trace_context_->storage->process_table()[row->upid()];
      if (process.pid() != pid) {
        return std::nullopt;
      }
      return row;
    }
    std::optional<UniquePid> upid =
        trace_context_->process_tracker->GetProcessOrNull(pid);
    if (!upid) {
      return std::nullopt;
    }
    auto row = table_->GetOrInsertRow(*upid);
    if (start_seq_id) {
      std::optional<int64_t> known = row.start_seq_id();
      if (known && *known != *start_seq_id) {
        // The pid now belongs to another process.
        return std::nullopt;
      }
      table_->SetStartSeqId(row, *start_seq_id);
    }
    return row;
  }

  void HandleBinderDied(protozero::ConstBytes data, int64_t ts) {
    AndroidBinderDiedEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }

    auto pid = static_cast<uint32_t>(evt.pid());
    std::optional<int64_t> start_seq_id;
    if (evt.has_start_seq_id()) {
      start_seq_id = evt.start_seq_id();
    }
    // Ignores deaths for another process (e.g. an older process whose pid was
    // reused before it was ended) instead of ending the current one.
    auto row = ResolveDyingProcess(pid, start_seq_id);
    if (!row) {
      return;
    }
    // The first death wins: a duplicate or late event must not move it.
    if (row->fw_end_ts().has_value()) {
      return;
    }
    row->set_fw_end_ts(ts);
    // A row matched by start_seq_id may belong to a process that has already
    // ended some other way (e.g. sched_process_free) and whose pid has been
    // reused: only end the pid if it still refers to this process.
    if (trace_context_->process_tracker->GetProcessOrNull(pid) == row->upid()) {
      trace_context_->process_tracker->EndThread(ts, pid);
    }
  }

  // Records why a process died. Ending the process is left to
  // AndroidBinderDiedEvent, which arrives earlier.
  void HandleProcessDied(protozero::ConstBytes data) {
    AndroidProcessDiedEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }
    std::optional<int64_t> start_seq_id;
    if (evt.has_start_seq_id()) {
      start_seq_id = evt.start_seq_id();
    }
    auto row =
        ResolveDyingProcess(static_cast<uint32_t>(evt.pid()), start_seq_id);
    if (!row) {
      return;
    }
    if (!row->exit_reason().has_value() && evt.has_reason()) {
      row->set_exit_reason(InternEnum(exit_reason_cache_,
                                      ".com.android.internal.AppExitReasonCode",
                                      static_cast<int32_t>(evt.reason())));
    }
    if (!row->exit_sub_reason().has_value() && evt.has_sub_reason()) {
      row->set_exit_sub_reason(InternEnum(
          exit_sub_reason_cache_, ".com.android.internal.AppExitSubReasonCode",
          static_cast<int32_t>(evt.sub_reason())));
    }
  }

  StringId InternEnum(DescriptorPool::CachedDescriptor& cache,
                      const char* enum_name,
                      int32_t value) {
    if (auto name = trace_context_->descriptor_pool_->FindEnumString(
            cache, enum_name, value)) {
      return trace_context_->storage->InternString(base::StringView(*name));
    }
    return trace_context_->storage->InternString(
        base::StringView(std::to_string(value)));
  }

  TraceProcessorContext* trace_context_;
  DescriptorPool::CachedDescriptor trigger_type_cache_;
  DescriptorPool::CachedDescriptor hosting_type_cache_;
  DescriptorPool::CachedDescriptor exit_reason_cache_;
  DescriptorPool::CachedDescriptor exit_sub_reason_cache_;
  AndroidTrackEventProcessTableHolder* table_;
  SystemServerTracker* system_server_;
};

// Handles the AndroidProcessStateSnapshot emitted at trace start
// (DUMP_REASON_START). It lists the processes alive when the trace started, so
// it creates them and records their start_seq_id.
class StartDumpModule : public ProtoImporterModule {
 public:
  StartDumpModule(ProtoImporterModuleContext* module_context,
                  TraceProcessorContext* context,
                  AndroidTrackEventProcessTableHolder* table)
      : ProtoImporterModule(module_context),
        trace_context_(context),
        table_(table) {
    RegisterForField(FBTP::kAndroidProcessStateFieldNumber);
  }
  ~StartDumpModule() override = default;

  void ParseField(const ParseFieldArgs& args) override {
    AndroidProcessStateSnapshot::Decoder dump(
        args.field.Cast<FBTP::kAndroidProcessState>());
    if (dump.dump_reason() != AndroidProcessStateSnapshot::DUMP_REASON_START) {
      return;
    }
    for (auto it = dump.record(); it; ++it) {
      AndroidProcessStateSnapshot::Record::Decoder rec(*it);
      if (!rec.has_pid() || rec.pid() <= 0) {
        continue;
      }
      UniquePid upid = trace_context_->process_tracker->GetOrCreateProcess(
          static_cast<uint32_t>(rec.pid()));
      if (rec.has_process_name()) {
        trace_context_->process_tracker->UpdateProcessName(
            upid, trace_context_->storage->InternString(rec.process_name()),
            ProcessNamePriority::kOther);
      }
      if (rec.has_uid()) {
        trace_context_->process_tracker->SetProcessUid(
            upid, static_cast<uint32_t>(rec.uid()));
      }
      if (rec.has_start_seq_id()) {
        table_->SetStartSeqId(table_->GetOrInsertRow(upid), rec.start_seq_id());
      }
    }
  }

 private:
  TraceProcessorContext* trace_context_;
  AndroidTrackEventProcessTableHolder* table_;
};

// Reports the first activity of processes to SystemServerTracker: each thread
// writes its process track descriptor, with the timestamp of its first event,
// before any of its events.
class ProcessTrackDescriptorModule : public ProtoImporterModule {
 public:
  ProcessTrackDescriptorModule(ProtoImporterModuleContext* module_context,
                               TraceProcessorContext* context,
                               SystemServerTracker* system_server)
      : ProtoImporterModule(module_context),
        trace_context_(context),
        system_server_(system_server) {
    RegisterForField(TracePacket::kTrackDescriptorFieldNumber);
  }
  ~ProcessTrackDescriptorModule() override = default;

  void ParseField(const ParseFieldArgs& args) override {
    TrackDescriptor::Decoder desc(
        args.field.Cast<TracePacket::kTrackDescriptor>());
    if (!desc.has_process()) {
      return;
    }
    protos::pbzero::ProcessDescriptor::Decoder process(desc.process());
    if (!process.has_pid() || process.pid() <= 0) {
      return;
    }
    // The track event importer, registered first, has already resolved the
    // track to its process.
    std::optional<UniquePid> upid =
        trace_context_->process_tracker->GetProcessOrNull(
            static_cast<uint32_t>(process.pid()));
    if (upid) {
      system_server_->OnProcessActive(*upid, args.ts);
    }
  }

 private:
  TraceProcessorContext* trace_context_;
  SystemServerTracker* system_server_;
};

class AndroidFrameworkTrackEventPlugin
    : public Plugin<AndroidFrameworkTrackEventPlugin> {
 public:
  ~AndroidFrameworkTrackEventPlugin() override;

  void RegisterDataframes(std::vector<PluginDataframe>& out) override {
    EnsureTable();
    out.push_back({&table_->table().dataframe(),
                   AndroidTrackEventProcessTable::Name(),
                   {}});
  }

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    EnsureTable();
    module_context->modules.emplace_back(std::make_unique<StartDumpModule>(
        module_context, trace_context, table_.get()));
    module_context->modules.emplace_back(
        std::make_unique<ProcessTrackDescriptorModule>(
            module_context, trace_context,
            EnsureSystemServerTracker(trace_context)));
  }

  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* ctx,
      TraceProcessorContext* trace_context) override {
    EnsureTable();
    ctx->parsers.emplace_back(
        std::make_unique<Parser>(ctx, trace_context, table_.get(),
                                 EnsureSystemServerTracker(trace_context)));
  }

 private:
  void EnsureTable() {
    if (!table_) {
      table_ = std::make_unique<AndroidTrackEventProcessTableHolder>(
          trace_context_->storage->mutable_string_pool());
    }
  }

  SystemServerTracker* EnsureSystemServerTracker(
      TraceProcessorContext* trace_context) {
    if (!system_server_) {
      system_server_ =
          std::make_unique<SystemServerTracker>(trace_context, table_.get());
    }
    return system_server_.get();
  }

  std::unique_ptr<AndroidTrackEventProcessTableHolder> table_;
  std::unique_ptr<SystemServerTracker> system_server_;
};

AndroidFrameworkTrackEventPlugin::~AndroidFrameworkTrackEventPlugin() = default;

}  // namespace

void RegisterPlugin() {
  static PluginRegistration reg(
      []() -> std::unique_ptr<PluginBase> {
        return std::make_unique<AndroidFrameworkTrackEventPlugin>();
      },
      AndroidFrameworkTrackEventPlugin::kPluginId,
      AndroidFrameworkTrackEventPlugin::kDepIds.data(),
      AndroidFrameworkTrackEventPlugin::kDepIds.size());
  base::ignore_result(reg);
}

}  // namespace perfetto::trace_processor::android_framework_track_event
