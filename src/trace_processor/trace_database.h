/*
 * Copyright (C) 2018 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_TRACE_DATABASE_H_
#define SRC_TRACE_PROCESSOR_TRACE_DATABASE_H_

#include <sqlite3.h>
#include <memory>

#include "perfetto/base/task_runner.h"
#include "perfetto/base/weak_ptr.h"
#include "perfetto/trace_processor/raw_query.pb.h"
#include "src/trace_processor/process_table.h"
#include "src/trace_processor/sched_slice_table.h"
#include "src/trace_processor/scoped_db.h"
#include "src/trace_processor/thread_table.h"
#include "src/trace_processor/trace_parser.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Coordinates the loading of traces from an arbitary source and allows
// execution of SQL queries on the events in these traces.
class TraceDatabase {
 public:
  TraceDatabase(base::TaskRunner* task_runner);
  ~TraceDatabase();

  // Loads a trace by reading from the given blob reader. Invokes |callback|
  // when the trace has been fully read and parsed.
  void LoadTrace(BlobReader* reader, std::function<void()> callback);

  // Executes a SQLite query on the loaded portion of the trace. |result| will
  // be invoked once after the result of the query is available.
  void ExecuteQuery(const protos::RawQueryArgs& args,
                    std::function<void(protos::RawQueryResult)> result);

 private:
  void LoadTraceChunk(std::function<void()> callback);

  ScopedDb db_;  // Keep first.

  TraceStorage storage_;
  std::unique_ptr<TraceParser> parser_;

  base::TaskRunner* const task_runner_;
  base::WeakPtrFactory<TraceDatabase> weak_factory_;  // Keep last.
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_DATABASE_H_
