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

#include "src/trace_processor/importers/proto/android_process_dump_module.h"

#include <cstdint>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/android_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

using ::com::android::internal::pbzero::AndroidFreezerStateSnapshot;
using ::com::android::internal::pbzero::AndroidProcessStateSnapshot;
using ::com::android::internal::pbzero::FrameworksBaseTracePacket;

AndroidProcessDumpModule::AndroidProcessDumpModule(
    ProtoImporterModuleContext* module_context,
    TraceProcessorContext* context)
    : ProtoImporterModule(module_context), context_(context) {
  RegisterForField(FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
  RegisterForField(FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber);
}

AndroidProcessDumpModule::~AndroidProcessDumpModule() = default;

void AndroidProcessDumpModule::ParseField(const ParseFieldArgs& args) {
  switch (args.field.id()) {
    case FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber:
      ParseAndroidProcessState(
          args.ts,
          args.field.Cast<FrameworksBaseTracePacket::kAndroidProcessState>());
      return;
    case FrameworksBaseTracePacket::kAndroidFreezerStateFieldNumber:
      ParseAndroidFreezerState(
          args.ts,
          args.field.Cast<FrameworksBaseTracePacket::kAndroidFreezerState>());
      return;
  }
}

void AndroidProcessDumpModule::ParseAndroidProcessState(
    int64_t ts,
    protozero::ConstBytes blob) {
  AndroidProcessStateSnapshot::Decoder evt(blob);
  auto* table = context_->storage->mutable_android_process_state_dump_table();
  for (auto it = evt.record(); it; ++it) {
    AndroidProcessStateSnapshot::Record::Decoder proc(*it);
    if (!proc.has_pid()) {
      continue;
    }
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(proc.pid()));
    if (proc.has_uid()) {
      context_->process_tracker->SetProcessUid(
          upid, static_cast<uint32_t>(proc.uid()));
    }
    tables::AndroidProcessStateDumpTable::Row row;
    row.ts = ts;
    row.pid = static_cast<uint32_t>(proc.pid());
    if (proc.has_uid()) {
      row.uid = static_cast<uint32_t>(proc.uid());
    }
    row.proc_state = proc.has_proc_state() ? proc.proc_state() : 0;
    row.oom_score = proc.has_oom_score() ? proc.oom_score() : 0;
    row.capability_flags =
        proc.has_capability_flags() ? proc.capability_flags() : 0;
    table->Insert(row);
  }
}

void AndroidProcessDumpModule::ParseAndroidFreezerState(
    int64_t ts,
    protozero::ConstBytes blob) {
  AndroidFreezerStateSnapshot::Decoder evt(blob);
  auto* table = context_->storage->mutable_android_freezer_state_dump_table();
  for (auto it = evt.record(); it; ++it) {
    AndroidFreezerStateSnapshot::Record::Decoder rec(*it);
    if (!rec.has_pid()) {
      continue;
    }
    UniquePid upid = context_->process_tracker->GetOrCreateProcess(
        static_cast<uint32_t>(rec.pid()));
    if (rec.has_process_name()) {
      context_->process_tracker->UpdateProcessName(
          upid, context_->storage->InternString(rec.process_name()),
          ProcessNamePriority::kOther);
    }
    tables::AndroidFreezerStateDumpTable::Row row;
    row.ts = ts;
    row.pid = static_cast<uint32_t>(rec.pid());
    if (rec.has_process_name()) {
      row.process_name = context_->storage->InternString(rec.process_name());
    }
    if (rec.has_freeze_unfreeze_time()) {
      row.freeze_unfreeze_time = rec.freeze_unfreeze_time();
    }
    if (rec.has_isfreezesticky()) {
      row.is_freeze_sticky = rec.isfreezesticky() ? 1u : 0u;
    }
    table->Insert(row);
  }
}

}  // namespace trace_processor
}  // namespace perfetto
