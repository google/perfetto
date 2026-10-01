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

#ifndef SRC_TRACE_PROCESSOR_TYPES_TRACE_PARSER_OPTIONS_H_
#define SRC_TRACE_PROCESSOR_TYPES_TRACE_PARSER_OPTIONS_H_

#include <functional>
#include "perfetto/trace_processor/basic_types.h"

namespace perfetto::trace_processor {
class TraceStorage;

// Internal parser retention and test/benchmark options. Not a public API.
struct TraceParserOptions {
  // Temporarily reuse existing format, clock and sorter settings. SQL/metrics
  // settings in this struct have no effect: TraceParser creates no SQL engine.
  // A dedicated common parsing-options type can replace this during migration.
  Config parsing_config;
  // Internal format detection hook, called before reader/context creation.
  std::function<void(const char*)> on_trace_type_detected;

  // Retain only generated-table columns used by C++ parsing. Intended for
  // parser embedders using table sinks; SQL over omitted columns is
  // unavailable. IDs and row counts remain unchanged. Defaults to parser
  // working columns.
  bool drop_unread_table_columns = true;

  // Experimental parser-only policy for ftrace scheduling inputs. Each unique
  // CPU keeps its open timestamp outside the table. Sched cells are sink-only;
  // storage eviction can pass pending rows, while completion waits for CPUs.
  // Thread-state timestamps/states also live in per-thread parser state, so
  // historical thread-state cells are sink-only and permit late back-patches.
  // Requires drop_unread_table_columns. Other scheduling formats and mixed
  // scheduling producers are outside this prototype's contract.
  bool experimental_ftrace_sched_frontier = false;
  uint32_t streaming_frontier_batch_rows = 1024;

  // Experimental parser-only policy for JSON and TrackEvent slices/args.
  // Active slice/flow state lives outside historical tables. Arg-set hash
  // deduplication and deferred translations retain their existing semantics.
  // Requires drop_unread_table_columns; other importers are not yet audited.
  bool experimental_slice_args_streaming = false;
  bool counter_sink_only = false;

  // Source-build embedder hook, invoked before trackers insert any rows. The
  // TraceStorage type and its generated table sinks are internal APIs.
  std::function<void(TraceStorage*)> on_trace_storage_created;
};
}  // namespace perfetto::trace_processor
#endif  // SRC_TRACE_PROCESSOR_TYPES_TRACE_PARSER_OPTIONS_H_
