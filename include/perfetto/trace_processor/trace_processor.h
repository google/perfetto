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
#include "perfetto/trace_processor/metatrace_config.h"
#include "perfetto/trace_processor/status.h"
#include "perfetto/trace_processor/trace_processor_storage.h"

namespace perfetto {
namespace trace_processor {

// Extends TraceProcessorStorage to support execution of SQL queries on loaded
// traces. See TraceProcessorStorage for parsing of trace files.
class PERFETTO_EXPORT_COMPONENT TraceProcessor : public TraceProcessorStorage {
 public:
  // For legacy API clients. Iterator used to be a nested class here. Many API
  // clients depends on it at this point.
  using Iterator = ::perfetto::trace_processor::Iterator;

  // Creates a new instance of TraceProcessor.
  static std::unique_ptr<TraceProcessor> CreateInstance(const Config&);

  ~TraceProcessor() override;

  // Executes the SQL on the loaded portion of the trace.
  //
  // More than one SQL statement can be passed to this function; all but the
  // last will be fully executed by this function before retuning. The last
  // statement will be executed and will yield rows as the caller calls Next()
  // over the returned Iterator.
  //
  // See documentation of the Iterator class for an example on how to use
  // the returned iterator.
  virtual Iterator ExecuteQuery(const std::string& sql) = 0;

  // Registers SQL files with the associated path under the package named
  // |sql_package.name|.
  //
  // For example, if you registered a package called "camera" with a file path
  // "camera/cpu/metrics.sql" you can include it (run the file) using "INCLUDE
  // PERFETTO MODULE camera.cpu.metrics". The first word of the string has to be
  // a package name and there can be only one package registered with a given
  // name.
  virtual base::Status RegisterSqlPackage(SqlPackage) = 0;

  // Registers a metric at the given path which will run the specified SQL.
  virtual base::Status RegisterMetric(const std::string& path,
                                      const std::string& sql) = 0;

  // Reads the FileDescriptorSet proto message given by |data| and |size| and
  // adds any extensions to the metrics proto to allow them to be available as
  // proto builder functions when computing metrics.
  virtual base::Status ExtendMetricsProto(const uint8_t* data, size_t size) = 0;

  // Behaves exactly as ExtendMetricsProto, except any FileDescriptor with
  // filename matching a prefix in |skip_prefixes| is skipped.
  virtual base::Status ExtendMetricsProto(
      const uint8_t* data,
      size_t size,
      const std::vector<std::string>& skip_prefixes) = 0;

  // Computes the given metrics on the loded portion of the trace. If
  // successful, the output argument |metrics_proto| will be filled with the
  // proto-encoded bytes for the message TraceMetrics in
  // perfetto/metrics/metrics.proto.
  virtual base::Status ComputeMetric(
      const std::vector<std::string>& metric_names,
      std::vector<uint8_t>* metrics_proto) = 0;

  enum MetricResultFormat {
    kProtoText = 0,
    kJson = 1,
  };

  // Computes metrics as the ComputeMetric function above, but instead of
  // producing proto encoded bytes, the output argument |metrics_string| is
  // filled with the metric formatted in the requested |format|.
  virtual base::Status ComputeMetricText(
      const std::vector<std::string>& metric_names,
      MetricResultFormat format,
      std::string* metrics_string) = 0;

  // Interrupts the current query. Typically used by Ctrl-C handler.
  virtual void InterruptQuery() = 0;

  // Restores Trace Processor to its pristine state. It preserves the built-in
  // tables/views/functions created by the ingestion process. Returns the number
  // of objects created in runtime that has been deleted.
  // NOTE: No Iterators can active when called.
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
  using MetatraceConfig = metatrace::MetatraceConfig;
  using MetatraceCategories = metatrace::MetatraceCategories;
  virtual void EnableMetatrace(MetatraceConfig config = {}) = 0;

  // Disables "meta-tracing" of trace processor and writes the trace as a
  // sequence of |TracePackets| into |trace_proto| returning the status of this
  // read.
  virtual base::Status DisableAndReadMetatrace(
      std::vector<uint8_t>* trace_proto) = 0;

  // Gets all the currently loaded proto descriptors used in metric computation.
  // This includes all compiled-in binary descriptors, and all proto descriptors
  // loaded by trace processor shell at runtime. The message is encoded as
  // DescriptorSet, defined in perfetto/trace_processor/trace_processor.proto.
  virtual std::vector<uint8_t> GetMetricDescriptors() = 0;

  // Deprecated. Use |RegisterSqlPackage()| instead, which is identical in
  // functionality to |RegisterSqlModule()| and the only difference is in
  // the argument, which is directly translatable to |SqlPackage|.
  virtual base::Status RegisterSqlModule(SqlModule) = 0;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACE_PROCESSOR_TRACE_PROCESSOR_H_
