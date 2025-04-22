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

#ifndef SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_DATA_SOURCE_H_
#define SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_DATA_SOURCE_H_

#include <functional>
#include <map>
#include <memory>

#include "perfetto/ext/base/utils.h"
#include "perfetto/ext/base/weak_ptr.h"
#include "perfetto/ext/protozero/proto_ring_buffer.h"
#include "perfetto/ext/tracing/core/basic_types.h"
#include "perfetto/ext/tracing/core/trace_writer.h"
#include "perfetto/tracing/core/forward_decls.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/frozen_ftrace_procfs.h"
#include "src/traced/probes/ftrace/ftrace_stats.h"
#include "src/traced/probes/probes_data_source.h"

#include "protos/perfetto/config/ftrace/frozen_ftrace_config.gen.h"

namespace perfetto {
struct FtraceDataSourceConfig;
class FrozenFtraceProcfs;
class ProtoTranslationTable;

namespace base {
class TaskRunner;
}

// Consumes the contents of a stopped tracefs instance, converting them to
// perfetto ftrace protos (same as FtraceDataSource). Does not reactivate the
// instance or write to any other control files within the tracefs instance (but
// the buffer contents do get consumed).
class FrozenFtraceDataSource : public ProbesDataSource {
 public:
  static const ProbesDataSource::Descriptor descriptor;

  FrozenFtraceDataSource(base::TaskRunner* task_runner,
                         const DataSourceConfig& ds_config,
                         TracingSessionID session_id,
                         std::unique_ptr<TraceWriter> writer);
  ~FrozenFtraceDataSource() override;

  // ProbeDataSource implementation.
  void Start() override;
  void Flush(FlushRequestID, std::function<void()> callback) override;

  base::WeakPtr<FrozenFtraceDataSource> GetWeakPtr() const {
    return weak_factory_.GetWeakPtr();
  }

  uint64_t* mutable_cpu_end_timestamp(size_t cpu) {
    if (cpu >= bundle_end_ts_by_cpu_.size())
      bundle_end_ts_by_cpu_.resize(cpu + 1);
    return &bundle_end_ts_by_cpu_[cpu];
  }

 private:
  void ReadTask();

  base::TaskRunner* const task_runner_;
  std::unique_ptr<TraceWriter> writer_;

  protos::gen::FrozenFtraceConfig ds_config_;

  std::unique_ptr<FrozenFtraceProcfs> tracefs_;
  std::unique_ptr<ProtoTranslationTable> translation_table_;
  std::unique_ptr<FtraceDataSourceConfig> parsing_config_;
  CpuReader::ParsingBuffers parsing_mem_;
  std::vector<CpuReader> cpu_readers_;

  std::vector<size_t> cpu_page_quota_;

  base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors_;
  std::vector<uint64_t> bundle_end_ts_by_cpu_;

  base::WeakPtrFactory<FrozenFtraceDataSource> weak_factory_;  // Keep last.
};

}  // namespace perfetto

#endif  // SRC_TRACED_PROBES_FTRACE_FROZEN_FTRACE_DATA_SOURCE_H_
