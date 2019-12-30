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

#ifndef SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_

#include "src/trace_processor/trace_processor_context.h"
#include "src/trace_processor/trace_storage.h"
#include "src/trace_processor/variadic.h"

namespace perfetto {
namespace trace_processor {

// Tracks and stores args for rows until the end of the packet. This allows
// allows args to pushed as a group into storage.
class ArgsTracker {
 public:
  // Stores the table and row at creation time which args are associated with.
  // This allows callers to directly add args without repeating the row the
  // args should be associated with.
  class BoundInserter {
   public:
    BoundInserter(ArgsTracker* args_tracker, TableId table, uint32_t row)
        : args_tracker_(args_tracker), table_(table), row_(row) {}
    virtual ~BoundInserter();

    // Adds an arg with the same key and flat_key.
    void AddArg(StringId key, Variadic v) { AddArg(key, key, v); }

    // Virtual for testing.
    virtual void AddArg(StringId flat_key, StringId key, Variadic v) {
      args_tracker_->AddArg(table_, row_, flat_key, key, v);
    }

   private:
    ArgsTracker* args_tracker_ = nullptr;
    TableId table_ = TableId::kInvalid;
    uint32_t row_ = 0;
  };

  explicit ArgsTracker(TraceProcessorContext*);
  virtual ~ArgsTracker();

  // Adds a arg for this row id with the given key and value.
  // Virtual for testing.
  virtual void AddArg(TableId table,
                      uint32_t row,
                      StringId flat_key,
                      StringId key,
                      Variadic);

  // Commits the added args to storage.
  // Virtual for testing.
  virtual void Flush();

 private:
  std::vector<TraceStorage::Args::Arg> args_;
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_ARGS_TRACKER_H_
