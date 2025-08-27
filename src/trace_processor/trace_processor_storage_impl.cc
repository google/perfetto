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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/ext/base/uuid.h"
#include "perfetto/trace_processor/basic_types.h"
#include "src/trace_processor/forwarding_trace_parser.h"
#include "src/trace_processor/importers/common/args_tracker.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/common/event_tracker.h"
#include "src/trace_processor/importers/common/process_tracker.h"
#include "src/trace_processor/importers/common/slice_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/importers/proto/packet_analyzer.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/importers/proto/proto_trace_parser_impl.h"
#include "src/trace_processor/importers/proto/proto_trace_reader.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/trace_reader_registry.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

TraceProcessorStorageImpl::TraceProcessorStorageImpl(const Config& cfg)
    : context_(TraceProcessorContext::CreateRootContext(cfg)) {
  context()->reader_registry->RegisterTraceReader<ProtoTraceReader>(
      kProtoTraceType);
  context()->reader_registry->RegisterTraceReader<ProtoTraceReader>(
      kSymbolsTraceType);
}

TraceProcessorStorageImpl::~TraceProcessorStorageImpl() {}

base::Status TraceProcessorStorageImpl::Parse(TraceBlobView blob) {
  if (blob.size() == 0)
    return base::OkStatus();
  if (unrecoverable_parse_error_)
    return base::ErrStatus(
        "Failed unrecoverably while parsing in a previous Parse call");
  if (eof_) {
    return base::ErrStatus("Parse() called after NotifyEndOfFile()");
  }

  if (!parser_) {
    parser_ = std::make_unique<ForwardingTraceParser>(
        &context_, context()->trace_file_tracker->AddFile());
  }

  auto scoped_trace = context()->storage->TraceExecutionTimeIntoStats(
      stats::parse_trace_duration_ns);

  if (hash_input_size_remaining_ > 0 &&
      !context()->uuid_state->uuid_found_in_trace) {
    const size_t hash_size = std::min(hash_input_size_remaining_, blob.size());
    hash_input_size_remaining_ -= hash_size;

    trace_hash_.Update(reinterpret_cast<const char*>(blob.data()), hash_size);
    base::Uuid uuid(static_cast<int64_t>(trace_hash_.digest()), 0);
    const StringId id_for_uuid = context()->storage->InternString(
        base::StringView(uuid.ToPrettyString()));
    context()->metadata_tracker->SetMetadata(metadata::trace_uuid,
                                             Variadic::String(id_for_uuid));
  }

  base::Status status = parser_->Parse(std::move(blob));
  unrecoverable_parse_error_ |= !status.ok();
  return status;
}

void TraceProcessorStorageImpl::Flush() {
  if (unrecoverable_parse_error_) {
    return;
  }
  if (context()->sorter) {
    context()->sorter->ExtractEventsForced();
  }
}

base::Status TraceProcessorStorageImpl::NotifyEndOfFile() {
  if (!parser_) {
    return base::OkStatus();
  }
  if (unrecoverable_parse_error_) {
    return base::ErrStatus("Unrecoverable parsing error already occurred");
  }
  eof_ = true;
  Flush();
  RETURN_IF_ERROR(parser_->NotifyEndOfFile());
  // NotifyEndOfFile might have pushed packets to the sorter.
  Flush();

  auto& traces = context()->forked_context_state->trace_to_context;
  for (auto it = traces.GetIterator(); it; ++it) {
    if (it.value()->content_analyzer) {
      PacketAnalyzer::Get(it.value())->NotifyEndOfFile();
    }
  }
  auto& all = context()->forked_context_state->trace_and_machine_to_context;
  for (auto it = all.GetIterator(); it; ++it) {
    it.value()->event_tracker->FlushPendingEvents();
    it.value()->slice_tracker->FlushPendingSlices();
    it.value()->process_tracker->NotifyEndOfFile();
  }
  return base::OkStatus();
}

void TraceProcessorStorageImpl::DestroyContext() {
  context_.DestroyParsingState();
  parser_.reset();
}

}  // namespace perfetto::trace_processor
