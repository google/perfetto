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
#include "src/trace_processor/core/plugin/registration.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/track_event_module.h"
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
// __intrinsic_android_track_event_process (upid, fw_start_ts, fw_end_ts). Both
// events are emitted as instant TrackEvents, so they arrive via
// OnTrackEventSliceExtension.
class Parser : public TrackEventPlugin {
 public:
  Parser(TrackEventPluginContext* plugin_context,
         TraceProcessorContext* context)
      : TrackEventPlugin(plugin_context), trace_context_(context) {
    RegisterTrackEventExtension(FBTE::kProcessStartEventFieldNumber);
    RegisterTrackEventExtension(FBTE::kBinderDiedEventFieldNumber);
  }
  ~Parser() override = default;

  Result OnTrackEventSliceExtension(const TrackEventExtensionField& field,
                                    SliceId id) override {
    // The extension carries no timestamp of its own; the just-inserted slice
    // does (an instant event's ts is the event ts).
    int64_t ts = trace_context_->storage->slice_table()[id].ts();
    switch (field.id()) {
      case FBTE::kProcessStartEventFieldNumber:
        HandleProcessStart(field.Cast<FBTE::kProcessStartEvent>(), ts);
        break;
      case FBTE::kBinderDiedEventFieldNumber:
        HandleBinderDied(field.Cast<FBTE::kBinderDiedEvent>(), ts);
        break;
      default:
        break;
    }
    // Leave the event to be flattened into the args table: this plugin only
    // adds a side table, it doesn't take ownership of the event.
    return Result::kIgnored;
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

  tables::AndroidTrackEventProcessTable::RowReference GetOrInsertRow(
      UniquePid upid) {
    auto* table =
        trace_context_->storage->mutable_android_track_event_process_table();
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
    if (!evt.has_pid()) {
      return;
    }
    UniquePid upid = trace_context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(evt.pid()));
    SetProcessMetadata(upid, data);
    // Keep the earliest start so the process_bound event (which shares this
    // proto) is captured rather than a later process_start.
    auto row = GetOrInsertRow(upid);
    if (!row.fw_start_ts().has_value()) {
      row.set_fw_start_ts(ts);
    }
  }

  void HandleBinderDied(protozero::ConstBytes data, int64_t ts) {
    AndroidBinderDiedEvent::Decoder evt(data);
    if (!evt.has_pid()) {
      return;
    }
    // Resolve the process without creating one. If ftrace sched already ended
    // it (freeing the pid), GetOrCreateProcess would resurrect a phantom
    // process, so look it up via its still-tracked main thread instead and bail
    // if the process is already gone.
    std::optional<UniqueTid> utid =
        trace_context_->process_tracker->GetThreadOrNull(
            static_cast<uint32_t>(evt.pid()));
    if (!utid) {
      return;
    }
    std::optional<UniquePid> upid =
        trace_context_->storage->thread_table()[*utid].upid();
    if (!upid) {
      return;
    }
    GetOrInsertRow(*upid).set_fw_end_ts(ts);
    // End the process so its pid is freed for reuse. With only an initial
    // ftrace snapshot (no ongoing sched) this is the sole signal that ends it.
    trace_context_->process_tracker->EndThread(
        ts, static_cast<uint32_t>(evt.pid()));
  }

  TraceProcessorContext* trace_context_;
  base::FlatHashMap<UniquePid, tables::AndroidTrackEventProcessTable::Id>
      upid_to_row_;
};

// Core plugin: lives in `full`, so the frameworks proto never reaches
// `minimal`. It installs the Parser into the TrackEventModule's plugin context
// during proto-module registration (track_module is already published by
// RegisterDefaultModules at this point).
class AndroidFrameworkTrackEventPlugin
    : public Plugin<AndroidFrameworkTrackEventPlugin> {
 public:
  ~AndroidFrameworkTrackEventPlugin() override;

  void RegisterProtoImporterModules(
      ProtoImporterModuleContext* module_context,
      TraceProcessorContext* trace_context) override {
    TrackEventPluginContext* ctx =
        module_context->track_module->mutable_plugin_context();
    ctx->plugins.emplace_back(std::make_unique<Parser>(ctx, trace_context));
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
