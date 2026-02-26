/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/trace_processor/importers/arrow/arrow_ipc_trace_reader.h"

#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "src/trace_processor/core/dataframe/arrow_ipc.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

ArrowIpcTraceReader::ArrowIpcTraceReader(TraceProcessorContext* context)
    : context_(context) {}

ArrowIpcTraceReader::~ArrowIpcTraceReader() = default;

base::Status ArrowIpcTraceReader::Parse(TraceBlobView blob) {
  buffer_.PushBack(std::move(blob));
  return base::OkStatus();
}

base::Status ArrowIpcTraceReader::OnPushDataToSorter() {
  if (buffer_.empty()) {
    return base::OkStatus();
  }

  auto* storage = context_->storage.get();

  // Look up the filename from the TraceFile table using our trace_id.
  auto row = storage->trace_file_table().FindById(context_->trace_id());
  if (!row) {
    return base::ErrStatus("ArrowIpcTraceReader: trace file entry not found");
  }
  auto name_id = row->name();
  if (!name_id) {
    return base::ErrStatus("ArrowIpcTraceReader: trace file has no name");
  }
  std::string filename(storage->string_pool().Get(*name_id).c_str());

  // Strip the ".arrow" suffix to get the table name.
  std::string table_name = base::StripSuffix(filename, ".arrow");

  // Find the matching static dataframe.
  auto dataframes = storage->GetStaticDataframes();
  TraceStorage::DataframeWithName* match = nullptr;
  for (auto& entry : dataframes) {
    if (entry.name == table_name) {
      match = &entry;
      break;
    }
  }
  if (!match) {
    // Silently skip tables that don't exist (e.g. session-specific metadata).
    buffer_ = util::TraceBlobViewReader();
    return base::OkStatus();
  }

  RETURN_IF_ERROR(core::dataframe::DeserializeFromArrowIpc(
      *match->dataframe, storage->mutable_string_pool(), buffer_));

  // Clear the buffer so this is idempotent.
  buffer_ = util::TraceBlobViewReader();
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
