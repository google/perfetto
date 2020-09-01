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

#ifndef INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_H_
#define INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_H_

#include <memory>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/export.h"
#include "perfetto/trace_processor/basic_types.h"
#include "perfetto/trace_processor/iterator.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_processor_storage.h"

namespace perfetto {
namespace trace_processor {

// Extends TraceProcessorStorage to support execution of SQL queries on loaded
// traces. See TraceProcessorStorage for parsing of trace files.
class PERFETTO_EXPORT TraceProcessor : public TraceProcessorStorage {
 public:
  // For legacy API clients. Iterator used to be a nested class here. Many API
  // clients depends on it at this point.
  using Iterator = ::perfetto::trace_processor::Iterator;

  // Creates a new instance of TraceProcessor.
  static std::unique_ptr<TraceProcessor> CreateInstance(const Config&);

  ~TraceProcessor() override;

  // Executes a SQLite query on the loaded portion of the trace. The returned
  // iterator can be used to load rows from the result.
  virtual Iterator ExecuteQuery(const std::string& sql,
                                int64_t time_queued = 0) = 0;

  // Registers a metric at the given path which will run the specified SQL.
  virtual util::Status RegisterMetric(const std::string& path,
                                      const std::string& sql) = 0;

  // Reads the FileDescriptorSet proto message given by |data| and |size| and
  // adds any extensions to the metrics proto to allow them to be available as
  // proto builder functions when computing metrics.
  virtual util::Status ExtendMetricsProto(const uint8_t* data, size_t size) = 0;

  // Computes the given metrics on the loded portion of the trace. If
  // successful, the output argument |metrics_proto| will be filled with the
  // proto-encoded bytes for the message TraceMetrics in
  // perfetto/metrics/metrics.proto.
  virtual util::Status ComputeMetric(
      const std::vector<std::string>& metric_names,
      std::vector<uint8_t>* metrics_proto) = 0;

  // Interrupts the current query. Typically used by Ctrl-C handler.
  virtual void InterruptQuery() = 0;

  // Deletes all tables and views that have been created (by the UI or user)
  // after the trace was loaded. It preserves the built-in tables/view created
  // by the ingestion process. Returns the number of table/views deleted.
  virtual size_t RestoreInitialTables() = 0;

  // Sets/returns the name of the currently loaded trace or an empty string if
  // no trace is fully loaded yet. This has no effect on the Trace Processor
  // functionality and is used for UI purposes only.
  // The returned name is NOT a path and will contain extra text w.r.t. the
  // argument originally passed to SetCurrentTraceName(), e.g., "file (42 MB)".
  virtual std::string GetCurrentTraceName() = 0;
  virtual void SetCurrentTraceName(const std::string&) = 0;

  // Enables "meta-tracing" of trace processor.
  // Metatracing involves tracing trace processor itself to root-cause
  // performace issues in trace processor. See |DisableAndReadMetatrace| for
  // more information on the format of the metatrace.
  virtual void EnableMetatrace() = 0;

  // Disables "meta-tracing" of trace processor and writes the trace as a
  // sequence of |TracePackets| into |trace_proto| returning the status of this
  // read.
  virtual util::Status DisableAndReadMetatrace(
      std::vector<uint8_t>* trace_proto) = 0;

  // Gets all the currently loaded proto descriptors used in metric computation.
  // This includes all compiled-in binary descriptors, and all proto descriptors
  // loaded by trace processor shell at runtime. The message is encoded as
  // DescriptorSet, defined in perfetto/trace_processor/trace_processor.proto.
  virtual std::vector<uint8_t> GetMetricDescriptors() = 0;
};

// When set, logs SQLite actions on the console.
void PERFETTO_EXPORT EnableSQLiteVtableDebugging();

}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_H_
