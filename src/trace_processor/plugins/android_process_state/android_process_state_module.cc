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

#include "src/trace_processor/plugins/android_process_state/android_process_state_module.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>

#include "perfetto/ext/base/string_view.h"
#include "perfetto/protozero/field.h"

#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/plugins/android_process_state/tables_py.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_trace_packet.pbzero.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"

namespace perfetto::trace_processor {

namespace fb = ::com::android::internal::pbzero;

namespace {

// The generated <Enum>_Name() returns this for a value outside the enum.
constexpr char kUnknownEnum[] = "PBZERO_UNKNOWN_ENUM_VALUE";

StringId InternEnum(TraceStorage* storage, const char* name, int32_t raw) {
  base::StringView sv(name);
  if (sv == base::StringView(kUnknownEnum)) {
    return storage->InternString(base::StringView(std::to_string(raw)));
  }
  return storage->InternString(sv);
}

}  // namespace

AndroidProcessStateTracker::AndroidProcessStateTracker(
    TraceProcessorContext* context,
    tables::AndroidProcessStateChangeTable* change_table)
    : context_(context), change_table_(change_table) {}

StringId AndroidProcessStateTracker::ProcStateName(int32_t value) {
  return InternEnum(
      context_->storage.get(),
      fb::ProcessStateEnum_Name(static_cast<fb::ProcessStateEnum>(value)),
      value);
}

StringId AndroidProcessStateTracker::ReasonName(int32_t value) {
  return InternEnum(
      context_->storage.get(),
      fb::OomChangeReasonEnum_Name(static_cast<fb::OomChangeReasonEnum>(value)),
      value);
}

void AndroidProcessStateTracker::ParseChange(int64_t ts,
                                             protozero::ConstBytes bytes) {
  fb::AndroidProcessStateChangedEvent::Decoder p(bytes);
  int32_t pid = p.pid();
  UniquePid upid = context_->process_tracker->GetOrCreateProcess(pid);
  // Record the prev_* state of this process's earliest delta: that is its state
  // at the start of the trace, used below to emit its initial-state row.
  EarliestDelta& earliest = earliest_prev_[upid];
  if (ts < earliest.ts) {
    earliest.ts = ts;
    earliest.values.upid = upid;
    earliest.values.pid = pid;
    if (p.has_uid()) {
      earliest.values.uid = p.uid();
    }
    if (p.has_prev_proc_state()) {
      earliest.values.proc_state = static_cast<int32_t>(p.prev_proc_state());
    }
    if (p.has_prev_oom_score()) {
      earliest.values.oom_score = p.prev_oom_score();
    }
    if (p.has_prev_capability_flags()) {
      earliest.values.capability_flags = p.prev_capability_flags();
    }
  }

  tables::AndroidProcessStateChangeTable::Row row;
  row.upid = upid;
  row.pid = pid;
  if (p.has_uid()) {
    row.uid = p.uid();
  }
  row.ts = ts;
  row.is_initial = 0;
  if (p.has_cur_proc_state()) {
    row.proc_state = ProcStateName(static_cast<int32_t>(p.cur_proc_state()));
  }
  row.oom_score = p.cur_oom_score();
  if (p.has_cur_capability_flags()) {
    row.capability_flags = p.cur_capability_flags();
  }
  if (p.has_reason()) {
    row.reason = ReasonName(static_cast<int32_t>(p.reason()));
  }
  if (p.has_seq_id()) {
    row.seq_id = p.seq_id();
  }
  change_table_->Insert(row);
}

void AndroidProcessStateTracker::ParseDump(protozero::ConstBytes bytes) {
  fb::AndroidProcessState::Decoder dump(bytes);
  for (auto it = dump.process(); it; ++it) {
    fb::AndroidProcessStateChangedEvent::Decoder pr(*it);
    ProcessStateValues d;
    d.pid = pr.pid();
    d.upid = context_->process_tracker->GetOrCreateProcess(d.pid);
    if (pr.has_uid()) {
      d.uid = pr.uid();
    }
    if (pr.has_cur_oom_score()) {
      d.oom_score = pr.cur_oom_score();
    }
    if (pr.has_cur_proc_state()) {
      d.proc_state = static_cast<int32_t>(pr.cur_proc_state());
    }
    if (pr.has_cur_capability_flags()) {
      d.capability_flags = pr.cur_capability_flags();
    }
    dump_[d.upid] = d;
  }
}

void AndroidProcessStateTracker::Finalize() {
  // Start from the trace-stop dump (the initial state of every process that did
  // not change), then let a changed process's earliest-delta prev_* override
  // it.
  std::map<UniquePid, ProcessStateValues> initial = dump_;
  for (const auto& [upid, earliest] : earliest_prev_) {
    initial[upid] = earliest.values;
  }
  for (const auto& [upid, v] : initial) {
    EmitInitialRow(v);
  }
}

void AndroidProcessStateTracker::EmitInitialRow(const ProcessStateValues& v) {
  tables::AndroidProcessStateChangeTable::Row row;
  row.upid = v.upid;
  row.pid = v.pid;
  row.uid = v.uid;
  // ts stays NULL: this is the state at trace start, not an observed change.
  row.is_initial = 1;
  if (v.proc_state.has_value()) {
    row.proc_state = ProcStateName(*v.proc_state);
  }
  if (v.oom_score.has_value()) {
    row.oom_score = *v.oom_score;
  }
  row.capability_flags = v.capability_flags;
  change_table_->Insert(row);
}

AndroidProcessStateModule::AndroidProcessStateModule(
    ProtoImporterModuleContext* mc,
    AndroidProcessStateTracker* tracker)
    : ProtoImporterModule(mc), tracker_(tracker) {
  RegisterForField(
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber);
}

AndroidProcessStateModule::~AndroidProcessStateModule() = default;

void AndroidProcessStateModule::ParseField(const ParseFieldArgs& args) {
  if (args.field.id() !=
      fb::FrameworksBaseTracePacket::kAndroidProcessStateFieldNumber) {
    return;
  }
  tracker_->ParseDump(
      args.field.Cast<fb::FrameworksBaseTracePacket::kAndroidProcessState>());
}

void AndroidProcessStateModule::OnEventsFullyExtracted() {
  tracker_->Finalize();
}

AndroidProcessStateExtensionParser::AndroidProcessStateExtensionParser(
    TrackEventExtensionParserContext* context,
    TraceProcessorContext* trace_context,
    AndroidProcessStateTracker* tracker)
    : TrackEventExtensionParser(context),
      trace_context_(trace_context),
      tracker_(tracker) {
  RegisterTrackEventExtension(
      fb::FrameworksBaseTrackEvent::kProcessStateChangedEventFieldNumber);
}

AndroidProcessStateExtensionParser::~AndroidProcessStateExtensionParser() =
    default;

TrackEventExtensionParser::Result
AndroidProcessStateExtensionParser::OnTrackEventSliceExtension(
    const TrackEventExtensionField& field,
    SliceId id) {
  int64_t ts = trace_context_->storage->slice_table()[id].ts();
  tracker_->ParseChange(
      ts,
      field.Cast<fb::FrameworksBaseTrackEvent::kProcessStateChangedEvent>());
  return Result::kIgnored;
}

}  // namespace perfetto::trace_processor
