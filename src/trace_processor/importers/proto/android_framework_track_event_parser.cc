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

#include "src/trace_processor/importers/proto/android_framework_track_event_parser.h"

#include <memory>

#include "perfetto/protozero/field.h"
#include "protos/third_party/android/frameworks/base/proto/tracing/frameworks_base_track_event.pbzero.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto::trace_processor {

namespace {
using FBTE = ::com::android::internal::pbzero::FrameworksBaseTrackEvent;
using AndroidProcessStartEvent =
    ::com::android::internal::pbzero::AndroidProcessStartEvent;
using AndroidBinderDiedEvent =
    ::com::android::internal::pbzero::AndroidBinderDiedEvent;
}  // namespace

// static
void AndroidFrameworkTrackEventParser::Register(TraceProcessorContext* context,
                                                TrackEventParser* parser) {
  auto plugin = std::make_unique<AndroidFrameworkTrackEventParser>(context);
  auto* p = plugin.get();
  auto& registry = parser->mutable_plugins();
  registry.RegisterFieldHandler(FBTE::kProcessStartEventFieldNumber,
                                [p](protozero::ConstBytes data, int64_t ts) {
                                  p->HandleProcessStart(data, ts);
                                });
  registry.RegisterFieldHandler(FBTE::kBinderDiedEventFieldNumber,
                                [p](protozero::ConstBytes data, int64_t ts) {
                                  p->HandleBinderDied(data, ts);
                                });
  registry.RegisterPlugin(std::move(plugin));
}

AndroidFrameworkTrackEventParser::AndroidFrameworkTrackEventParser(
    TraceProcessorContext* context)
    : context_(context) {}

AndroidFrameworkTrackEventParser::~AndroidFrameworkTrackEventParser() = default;

tables::AndroidTrackEventProcessTable::RowReference
AndroidFrameworkTrackEventParser::RowFor(int64_t pid) {
  UniquePid upid =
      context_->process_tracker->GetOrCreateProcessWithoutMainThread(pid);
  auto* table = context_->storage->mutable_android_track_event_process_table();
  auto it_and_ins =
      upid_to_row_.Insert(upid, tables::AndroidTrackEventProcessTable::Id{0});
  if (it_and_ins.second) {
    tables::AndroidTrackEventProcessTable::Row row;
    row.upid = upid;
    *it_and_ins.first = table->Insert(row).id;
  }
  return (*table)[*it_and_ins.first];
}

void AndroidFrameworkTrackEventParser::HandleProcessStart(
    protozero::ConstBytes data,
    int64_t ts) {
  AndroidProcessStartEvent::Decoder evt(data);
  if (!evt.has_pid())
    return;
  auto row = RowFor(evt.pid());
  if (!row.start_ts().has_value())
    row.set_start_ts(ts);
}

void AndroidFrameworkTrackEventParser::HandleBinderDied(
    protozero::ConstBytes data,
    int64_t ts) {
  AndroidBinderDiedEvent::Decoder evt(data);
  if (!evt.has_pid())
    return;
  auto row = RowFor(evt.pid());
  if (!row.end_ts().has_value())
    row.set_end_ts(ts);
}

}  // namespace perfetto::trace_processor
