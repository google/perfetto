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

#include "src/trace_processor/importers/common/scoped_active_trace_file.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "perfetto/ext/base/string_view.h"
#include "src/trace_processor/importers/common/trace_file_tracker.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
ScopedActiveTraceFile::~ScopedActiveTraceFile() {
  if (is_valid_) {
    context_->trace_file_tracker->EndFile(row_);
  }
}

void ScopedActiveTraceFile::SetName(const std::string& name) {
  row_.set_name(context_->storage->InternString(base::StringView(name)));
}

void ScopedActiveTraceFile::SetTraceType(TraceType type) {
  row_.set_trace_type(context_->storage->InternString(TraceTypeToString(type)));
}

void ScopedActiveTraceFile::SetSize(size_t size) {
  row_.set_size(static_cast<int64_t>(size));
}

void ScopedActiveTraceFile::AddSize(size_t size) {
  row_.set_size(static_cast<int64_t>(size) + row_.size());
}

}  // namespace perfetto::trace_processor
