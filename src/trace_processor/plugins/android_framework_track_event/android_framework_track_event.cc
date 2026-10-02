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
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/stats_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_extension_parser.h"
#include "src/trace_processor/plugins/android_framework_track_event/tables_py.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto::trace_processor::android_framework_track_event {
namespace {

using FBTE = ::com::android::internal::pbzero::FrameworksBaseTrackEvent;
using FBTP = ::com::android::internal::pbzero::FrameworksBaseTracePacket;
using AndroidProcessStartEvent =
    ::com::android::internal::pbzero::AndroidProcessStartEvent;
using AndroidBinderDiedEvent =
    ::com::android::internal::pbzero::AndroidBinderDiedEvent;
using AndroidProcessDiedEvent =
    ::com::android::internal::pbzero::AndroidProcessDiedEvent;
using AndroidProcessStateSnapshot =
    ::com::android::internal::pbzero::AndroidProcessStateSnapshot;
using AndroidTrackEventProcessTable = tables::AndroidTrackEventProcessTable;

template <typename Decoder>
std::optional<uint32_t> Uid(const Decoder& d) {
  return d.has_uid() ? std::make_optional(static_cast<uint32_t>(d.uid()))
                     : std::nullopt;
}

template <typename Decoder>
std::optional<int64_t> Seq(const Decoder& d) {
  return d.has_start_seq_id() ? std::make_optional(d.start_seq_id())
                              : std::nullopt;
}

// __intrinsic_android_track_event_process, with a single row per process.
// Shared by Parser and StartDumpModule.
//
// App processes are identified by start_seq_id, which AMS assigns to every
// process it starts and which every lifecycle event carries. This keeps app
// identity correct across pid reuse and late death events.
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

  // Returns the process for a started app (process_start, process_bound or a
  // START dump record), creating it if this launch is new.
  UniquePid GetOrCreateApp(TraceProcessorContext* context,
                           int64_t ts,
                           uint32_t pid,
                           std::optional<uint32_t> uid,
                           std::optional<int64_t> seq) {
    if (seq) {
      if (UniquePid* upid = seq_to_upid_.Find(*seq)) {
        return *upid;
      }
    }
    auto* tracker = context->process_tracker.get();
    std::optional<UniquePid> upid = tracker->GetProcessOrNull(pid);
    if (upid && !IsSameApp(context, *upid, uid, seq)) {
      // The process on this pid died unseen (e.g. a native process listed by
      // linux.process_stats): this is a new process.
      tracker->EndThread(ts, pid);
      upid = std::nullopt;
    }
    if (!upid) {
      upid =
          tracker->StartNewProcess(std::nullopt, std::nullopt, pid,
                                   kNullStringId, ThreadNamePriority::kOther);
    }
    if (uid) {
      tracker->SetProcessUid(*upid, *uid);
    }
    if (seq) {
      SetStartSeqId(*upid, *seq);
    }
    return *upid;
  }

  // Returns the process a death event refers to, or nullopt if unknown. Death
  // events can arrive after the pid was ended or reused, so match by seq first.
  std::optional<UniquePid> FindDyingApp(TraceProcessorContext* context,
                                        uint32_t pid,
                                        std::optional<uint32_t> uid,
                                        std::optional<int64_t> seq) {
    if (seq) {
      if (UniquePid* upid = seq_to_upid_.Find(*seq)) {
        return *upid;
      }
    }
    CheckStartSeen(context, seq);
    std::optional<UniquePid> upid =
        context->process_tracker->GetProcessOrNull(pid);
    if (!upid || !IsSameApp(context, *upid, uid, seq)) {
      return std::nullopt;
    }
    if (seq) {
      SetStartSeqId(*upid, *seq);
    }
    return upid;
  }

  void OnStartDump() { start_dump_seen_ = true; }

  // Counts |seq| as a missed app start if neither the START dump nor a
  // process_start had it. Without a START dump, apps that were already running
  // are expected to be unknown, so nothing is counted.
  void CheckStartSeen(TraceProcessorContext* context,
                      std::optional<int64_t> seq) {
    if (!start_dump_seen_ || !seq || seq_to_upid_.Find(*seq)) {
      return;
    }
    if (missed_seqs_.Insert(*seq, true).second) {
      context->stats_tracker->IncrementStats(stats::android_app_start_missed);
    }
  }

 private:
  // Whether the live process |upid| can be the app with |uid| and |seq|. If
  // both sides have a seq it decides; otherwise fall back to the uid, as
  // processes created before the app attached (by ftrace or
  // linux.process_stats) have no seq.
  bool IsSameApp(TraceProcessorContext* context,
                 UniquePid upid,
                 std::optional<uint32_t> uid,
                 std::optional<int64_t> seq) {
    if (auto* row = upid_to_row_.Find(upid); row && seq) {
      std::optional<int64_t> known = table_[*row].start_seq_id();
      if (known) {
        return *known == *seq;
      }
    }
    std::optional<uint32_t> known_uid =
        context->storage->process_table()[upid].uid();
    return !uid || !known_uid || *uid == *known_uid;
  }

  void SetStartSeqId(UniquePid upid, int64_t seq) {
    GetOrInsertRow(upid).set_start_seq_id(seq);
    seq_to_upid_[seq] = upid;
  }

  AndroidTrackEventProcessTable table_;
  base::FlatHashMap<UniquePid, AndroidTrackEventProcessTable::Id> upid_to_row_;
  base::FlatHashMap<int64_t, UniquePid> seq_to_upid_;
  base::FlatHashMap<int64_t, bool> missed_seqs_;
  bool start_dump_seen_ = false;
};

// Records AndroidProcessStartEvent, AndroidBinderDiedEvent and
// AndroidProcessDiedEvent into __intrinsic_android_track_event_process.
class Parser : public TrackEventExtensionParser {
 public:
  Parser(TrackEventExtensionParserContext* extension_parser_context,
         TraceProcessorContext* context,
         AndroidTrackEventProcessTableHolder* table)
      : TrackEventExtensionParser(extension_parser_context),
        trace_context_(context),
        table_(table) {
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
    switch (field.id()) {
      case FBTE::kProcessStartEventFieldNumber:
        HandleProcessStart(field.Cast<FBTE::kProcessStartEvent>(), ts);
        break;
      case FBTE::kBinderDiedEventFieldNumber:
        HandleBinderDied(field.Cast<FBTE::kBinderDiedEvent>(), ts);
        break;
      case FBTE::kProcessDiedEventFieldNumber:
        HandleProcessDied(field.Cast<FBTE::kProcessDiedEvent>());
        // Still reflect it into the slice args, as before.
        return Result::kIgnored;
      default:
        break;
    }
    return Result::kHandled;
  }

 private:
  void HandleProcessStart(protozero::ConstBytes data, int64_t ts) {
    AndroidProcessStartEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }
    UniquePid upid = table_->GetOrCreateApp(trace_context_, ts,
                                            static_cast<uint32_t>(evt.pid()),
                                            Uid(evt), Seq(evt));
    if (evt.has_process_name()) {
      trace_context_->process_tracker->UpdateProcessName(
          upid, trace_context_->storage->InternString(evt.process_name()),
          ProcessNamePriority::kOther);
    }

    auto row = table_->GetOrInsertRow(upid);
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

  void HandleBinderDied(protozero::ConstBytes data, int64_t ts) {
    AndroidBinderDiedEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }
    auto pid = static_cast<uint32_t>(evt.pid());
    std::optional<UniquePid> upid =
        table_->FindDyingApp(trace_context_, pid, Uid(evt), Seq(evt));
    if (!upid) {
      return;
    }
    table_->GetOrInsertRow(*upid).set_fw_end_ts(ts);
    // Only end the pid if it still belongs to this process.
    if (trace_context_->process_tracker->GetProcessOrNull(pid) == upid) {
      trace_context_->process_tracker->EndThread(ts, pid);
    }
  }

  // Records why a process died. Ending it is left to binder_died, which
  // arrives earlier.
  void HandleProcessDied(protozero::ConstBytes data) {
    AndroidProcessDiedEvent::Decoder evt(data);
    if (!evt.has_pid() || evt.pid() <= 0) {
      return;
    }
    std::optional<UniquePid> upid = table_->FindDyingApp(
        trace_context_, static_cast<uint32_t>(evt.pid()), Uid(evt), Seq(evt));
    if (!upid) {
      return;
    }
    auto row = table_->GetOrInsertRow(*upid);
    if (evt.has_reason()) {
      row.set_exit_reason(InternEnum(exit_reason_cache_,
                                     ".com.android.internal.AppExitReasonCode",
                                     static_cast<int32_t>(evt.reason())));
    }
    if (evt.has_sub_reason()) {
      row.set_exit_sub_reason(InternEnum(
          exit_sub_reason_cache_, ".com.android.internal.AppExitSubReasonCode",
          static_cast<int32_t>(evt.sub_reason())));
    }
  }

  StringId InternEnum(DescriptorPool::CachedDescriptor& cache,
                      const char* enum_name,
                      int32_t value) {
    auto name = trace_context_->descriptor_pool_->FindEnumString(
        cache, enum_name, value);
    return trace_context_->storage->InternString(
        base::StringView(name ? *name : std::to_string(value)));
  }

  TraceProcessorContext* trace_context_;
  DescriptorPool::CachedDescriptor trigger_type_cache_;
  DescriptorPool::CachedDescriptor hosting_type_cache_;
  DescriptorPool::CachedDescriptor exit_reason_cache_;
  DescriptorPool::CachedDescriptor exit_sub_reason_cache_;
  AndroidTrackEventProcessTableHolder* table_;
};

// Handles AndroidProcessStateSnapshot. The START dump lists the apps alive when
// the trace started; later dumps are only used to detect missed app starts.
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
      // Later dumps (END, CLONE) only check for apps whose start was missed.
      for (auto it = dump.record(); it; ++it) {
        AndroidProcessStateSnapshot::Record::Decoder rec(*it);
        table_->CheckStartSeen(trace_context_, Seq(rec));
      }
      return;
    }
    table_->OnStartDump();
    for (auto it = dump.record(); it; ++it) {
      AndroidProcessStateSnapshot::Record::Decoder rec(*it);
      if (!rec.has_pid() || rec.pid() <= 0) {
        continue;
      }
      UniquePid upid = table_->GetOrCreateApp(trace_context_, args.ts,
                                              static_cast<uint32_t>(rec.pid()),
                                              Uid(rec), Seq(rec));
      if (rec.has_process_name()) {
        trace_context_->process_tracker->UpdateProcessName(
            upid, trace_context_->storage->InternString(rec.process_name()),
            ProcessNamePriority::kOther);
      }
    }
  }

 private:
  TraceProcessorContext* trace_context_;
  AndroidTrackEventProcessTableHolder* table_;
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
  }

  void RegisterTrackEventExtensions(
      TrackEventExtensionParserContext* ctx,
      TraceProcessorContext* trace_context) override {
    EnsureTable();
    ctx->parsers.emplace_back(
        std::make_unique<Parser>(ctx, trace_context, table_.get()));
  }

 private:
  void EnsureTable() {
    if (!table_) {
      table_ = std::make_unique<AndroidTrackEventProcessTableHolder>(
          trace_context_->storage->mutable_string_pool());
    }
  }

  std::unique_ptr<AndroidTrackEventProcessTableHolder> table_;
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
