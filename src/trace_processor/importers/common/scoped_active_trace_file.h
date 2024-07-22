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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCOPED_ACTIVE_TRACE_FILE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCOPED_ACTIVE_TRACE_FILE_H_

#include <string>

#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/util/trace_type.h"

namespace perfetto::trace_processor {

class TraceProcessorContext;

// RAII like object that represents a file currently being parsed. When
// instances of this object go out of scope they will notify the
// TraceFileTracker that we are done parsing the file.
// This class also acts a handler for setting file related properties.
class ScopedActiveTraceFile {
 public:
  ~ScopedActiveTraceFile();

  ScopedActiveTraceFile(const ScopedActiveTraceFile&) = delete;
  ScopedActiveTraceFile& operator=(const ScopedActiveTraceFile&) = delete;

  ScopedActiveTraceFile(ScopedActiveTraceFile&& o)
      : context_(o.context_), row_(o.row_), is_valid_(o.is_valid_) {
    o.is_valid_ = false;
  }

  ScopedActiveTraceFile& operator=(ScopedActiveTraceFile&& o) {
    context_ = o.context_;
    row_ = o.row_;
    is_valid_ = o.is_valid_;
    o.is_valid_ = false;
    return *this;
  }

  void SetTraceType(TraceType type);

  // For streamed files this method can be called for each chunk to update the
  // file size incrementally.
  void AddSize(size_t delta);

 private:
  friend class TraceFileTracker;
  ScopedActiveTraceFile(TraceProcessorContext* context,
                        tables::TraceFileTable::RowReference row)
      : context_(context), row_(row), is_valid_(true) {}

  // Sets the file name. If this method is not called (sometimes we do not know
  // the file name, e.g. streaming data) the name is set to null.
  void SetName(const std::string& name);
  void SetSize(size_t size);

  TraceProcessorContext* context_;
  tables::TraceFileTable::RowReference row_;
  bool is_valid_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_SCOPED_ACTIVE_TRACE_FILE_H_
