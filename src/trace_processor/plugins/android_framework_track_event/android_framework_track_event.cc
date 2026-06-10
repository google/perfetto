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

#include "perfetto/base/compiler.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/core/plugin/plugin.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_plugin.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor::android_framework_track_event {
namespace {

using FBTE = ::com::android::internal::pbzero::FrameworksBaseTrackEvent;
using AndroidProcessStartEvent =
    ::com::android::internal::pbzero::AndroidProcessStartEvent;
using AndroidBinderDiedEvent =
    ::com::android::internal::pbzero::AndroidBinderDiedEvent;

// Records AndroidProcessStartEvent and AndroidBinderDiedEvent into
// __intrinsic_android_track_event_process (upid, fw_start_ts, fw_end_ts).
class Parser : public TrackEventPlugin {
 public:
  explicit Parser(TraceProcessorContext* context) : context_(context) {}
  ~Parser() override = default;

  void ParseField(uint32_t field_id,
                  protozero::ConstBytes data,
                  int64_t ts) override {
    switch (field_id) {
      case FBTE::kProcessStartEventFieldNumber:
        HandleProcessStart(data, ts);
        break;
      case FBTE::kBinderDiedEventFieldNumber:
        HandleBinderDied(data, ts);
        break;
    }
  }

 private:
  void SetProcessMetadata(UniquePid upid,
                          const AndroidProcessStartEvent::Decoder& evt) {
    if (evt.has_uid())
      context_->process_tracker->SetProcessUid(
          upid, static_cast<uint32_t>(evt.uid()));
    if (evt.has_process_name())
      context_->process_tracker->UpdateProcessName(
          upid, context_->storage->InternString(evt.process_name()),
          ProcessNamePriority::kOther);
  }

  tables::AndroidTrackEventProcessTable::RowReference GetOrInsertRow(
      UniquePid upid) {
    auto* table =
        context_->storage->mutable_android_track_event_process_table();
    auto it_and_ins =
        upid_to_row_.Insert(upid, tables::AndroidTrackEventProcessTable::Id{0});
    if (it_and_ins.second) {
      tables::AndroidTrackEventProcessTable::Row row;
      row.upid = upid;
      *it_and_ins.first = table->Insert(row).id;
    }
    return (*table)[*it_and_ins.first];
  }

  void HandleProcessStart(protozero::ConstBytes data, int64_t ts) {
    AndroidProcessStartEvent::Decoder evt(data);
    if (!evt.has_pid())
      return;
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(evt.pid());
    SetProcessMetadata(upid, evt);
    // Keep the earliest start to capture the process_bound event as opposed to
    // process_start event as the first event even though they share the same
    // proto.
    auto row = GetOrInsertRow(upid);
    if (!row.fw_start_ts().has_value())
      row.set_fw_start_ts(ts);
  }

  void HandleBinderDied(protozero::ConstBytes data, int64_t ts) {
    AndroidBinderDiedEvent::Decoder evt(data);
    if (!evt.has_pid())
      return;
    // Resolve the process without creating one. If ftrace sched already ended
    // it (freeing the pid), GetOrCreateProcess would resurrect a phantom
    // process, so look it up via its still-tracked main thread instead and
    // bail if the process is already gone.
    std::optional<UniqueTid> utid =
        context_->process_tracker->GetThreadOrNull(evt.pid());
    if (!utid)
      return;
    std::optional<UniquePid> upid =
        context_->storage->thread_table()[*utid].upid();
    if (!upid)
      return;
    GetOrInsertRow(*upid).set_fw_end_ts(ts);
    // End the process so its pid is freed for reuse. With only an initial
    // ftrace snapshot (no ongoing sched) this is the sole signal that ends it.
    context_->process_tracker->EndThread(ts, evt.pid());
  }

  TraceProcessorContext* context_;
  base::FlatHashMap<UniquePid, tables::AndroidTrackEventProcessTable::Id>
      upid_to_row_;
};

class AndroidFrameworkTrackEventPlugin
    : public Plugin<AndroidFrameworkTrackEventPlugin> {
 public:
  ~AndroidFrameworkTrackEventPlugin() override;

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    module_context->track_event_plugins.Register(
        std::make_unique<Parser>(trace_context),
        {FBTE::kProcessStartEventFieldNumber,
         FBTE::kBinderDiedEventFieldNumber});
  }
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
