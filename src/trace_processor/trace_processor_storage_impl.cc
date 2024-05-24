/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/trace_processor_storage_impl.h"
#include <memory>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/uuid.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/args_translation_table.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/clock_converter.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/flow_tracker.h"
#include "src/trace_processor/importers/common/machine_tracker.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/importers/common/process_track_translation_table.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/sched_event_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/slice_translation_table.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/track_tracker.h"
#include "src/trace_processor/importers/perf/dso_tracker.h"
#include "src/trace_processor/importers/proto/chrome_track_event.descriptor.h"
#include "src/trace_processor/importers/proto/default_modules.h"
#include "src/trace_processor/importers/proto/packet_analyzer.h"
#include "src/trace_processor/importers/proto/perf_sample_tracker.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/importers/proto/track_event.descriptor.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/util/descriptors.h"

namespace perfetto {
namespace trace_processor {

TraceProcessorStorageImpl::TraceProcessorStorageImpl(const Config& cfg)
    : context_({cfg, std::make_shared<TraceStorage>(cfg)}) {
  context_.reader_registry->RegisterTraceReader<ProtoTraceReader>(
      kProtoTraceType);
  context_.proto_trace_parser =
      std::make_unique<ProtoTraceParserImpl>(&context_);
  RegisterDefaultModules(&context_);
}

TraceProcessorStorageImpl::~TraceProcessorStorageImpl() {}

util::Status TraceProcessorStorageImpl::Parse(TraceBlobView blob) {
  if (blob.size() == 0)
    return util::OkStatus();
  if (unrecoverable_parse_error_)
    return util::ErrStatus(
        "Failed unrecoverably while parsing in a previous Parse call");
  if (!context_.chunk_reader)
    context_.chunk_reader.reset(new ForwardingTraceParser(&context_));

  auto scoped_trace = context_.storage->TraceExecutionTimeIntoStats(
      stats::parse_trace_duration_ns);

  if (hash_input_size_remaining_ > 0 && !context_.uuid_found_in_trace) {
    const size_t hash_size = std::min(hash_input_size_remaining_, blob.size());
    hash_input_size_remaining_ -= hash_size;

    trace_hash_.Update(reinterpret_cast<const char*>(blob.data()), hash_size);
    base::Uuid uuid(static_cast<int64_t>(trace_hash_.digest()), 0);
    const StringId id_for_uuid =
        context_.storage->InternString(base::StringView(uuid.ToPrettyString()));
    context_.metadata_tracker->SetMetadata(metadata::trace_uuid,
                                           Variadic::String(id_for_uuid));
  }

  util::Status status = context_.chunk_reader->Parse(std::move(blob));
  unrecoverable_parse_error_ |= !status.ok();
  return status;
}

void TraceProcessorStorageImpl::Flush() {
  if (unrecoverable_parse_error_)
    return;

  if (context_.sorter)
    context_.sorter->ExtractEventsForced();
  context_.args_tracker->Flush();
}

void TraceProcessorStorageImpl::NotifyEndOfFile() {
  if (unrecoverable_parse_error_ || !context_.chunk_reader)
    return;
  Flush();
  context_.chunk_reader->NotifyEndOfFile();
  // NotifyEndOfFile might have pushed packets to the sorter.
  Flush();
  for (std::unique_ptr<ProtoImporterModule>& module : context_.modules) {
    module->NotifyEndOfFile();
  }
  if (context_.content_analyzer) {
    PacketAnalyzer::Get(&context_)->NotifyEndOfFile();
  }
  context_.event_tracker->FlushPendingEvents();
  context_.slice_tracker->FlushPendingSlices();
  context_.args_tracker->Flush();
  context_.process_tracker->NotifyEndOfFile();
  if (context_.perf_dso_tracker) {
    perf_importer::DsoTracker::GetOrCreate(&context_).SymbolizeFrames();
  }
}

void TraceProcessorStorageImpl::DestroyContext() {
  TraceProcessorContext context;
  context.storage = std::move(context_.storage);

  // TODO(b/309623584): Decouple from storage and remove from here. This
  // function should only move storage and delete everything else.
  context.heap_graph_tracker = std::move(context_.heap_graph_tracker);
  context.clock_converter = std::move(context_.clock_converter);
  // "to_ftrace" textual converter of the "raw" table requires remembering the
  // kernel version (inside system_info_tracker) to know how to textualise
  // sched_switch.prev_state bitflags.
  context.system_info_tracker = std::move(context_.system_info_tracker);

  context_ = std::move(context);

  // TODO(chinglinyu): also need to destroy secondary contextes.
}

}  // namespace trace_processor
}  // namespace perfetto
