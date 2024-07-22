/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include "src/trace_processor/importers/common/trace_file_tracker.h"

#include <cstddef>
#include <string>
#include <vector>

#include "src/trace_processor/importers/common/metadata_tracker.h"
#include "src/trace_processor/storage/metadata.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

ScopedActiveTraceFile TraceFileTracker::StartNewFile() {
  tables::TraceFileTable::Row row;
  if (!ancestors_.empty()) {
    row.parent_id = ancestors_.back();
  }

  row.size = 0;
  row.trace_type =
      context_->storage->InternString(TraceTypeToString(kUnknownTraceType));

  auto ref =
      context_->storage->mutable_trace_file_table()->Insert(row).row_reference;

  ancestors_.push_back(ref.id());
  return ScopedActiveTraceFile(context_, std::move(ref));
}

ScopedActiveTraceFile TraceFileTracker::StartNewFile(const std::string& name,
                                                     TraceType type,
                                                     size_t size) {
  auto file = StartNewFile();
  file.SetName(name);
  file.SetTraceType(type);
  file.SetSize(size);
  return file;
}

void TraceFileTracker::EndFile(
    const tables::TraceFileTable::ConstRowReference& row) {
  PERFETTO_CHECK(!ancestors_.empty());
  PERFETTO_CHECK(ancestors_.back() == row.id());

  // First file (root)
  if (row.id().value == 0) {
    context_->metadata_tracker->SetMetadata(metadata::trace_size_bytes,
                                            Variadic::Integer(row.size()));
    context_->metadata_tracker->SetMetadata(metadata::trace_type,
                                            Variadic::String(row.trace_type()));
  }
  ancestors_.pop_back();
}

}  // namespace perfetto::trace_processor
