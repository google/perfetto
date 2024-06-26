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

#include <stddef.h>
#include <cstdint>

#include <algorithm>

#include "perfetto/base/flat_set.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/utils.h"
#include "src/traced/probes/ftrace/cpu_reader.h"
#include "src/traced/probes/ftrace/ftrace_config_muxer.h"
#include "src/traced/probes/ftrace/test/cpu_reader_support.h"
#include "src/tracing/core/null_trace_writer.h"

#include "protos/perfetto/trace/ftrace/ftrace_event_bundle.pbzero.h"
#include "protos/perfetto/trace/ftrace/ftrace_stats.pbzero.h"

namespace perfetto {

using perfetto::protos::pbzero::FtraceEventBundle;

void FuzzCpuReaderProcessPagesForDataSource(const uint8_t* data, size_t size);

// TODO(rsavitski): make the fuzzer generate multi-page payloads.
void FuzzCpuReaderProcessPagesForDataSource(const uint8_t* data, size_t size) {
  ProtoTranslationTable* table = GetTable("synthetic");
  if (!table) {
    PERFETTO_FATAL(
        "Could not read table. "
        "This fuzzer must be run in the root directory.");
  }

  static uint8_t* g_page = new uint8_t[base::GetSysPageSize()];
  memset(g_page, 0, base::GetSysPageSize());
  memcpy(g_page, data, std::min(size_t(base::GetSysPageSize()), size));

  FtraceMetadata metadata{};
  FtraceDataSourceConfig ds_config{/*event_filter=*/EventFilter{},
                                   /*syscall_filter=*/EventFilter{},
                                   DisabledCompactSchedConfigForTesting(),
                                   /*print_filter=*/std::nullopt,
                                   /*atrace_apps=*/{},
                                   /*atrace_categories=*/{},
                                   /*atrace_categories_prefer_track_event=*/{},
                                   /*symbolize_ksyms=*/false,
                                   /*preserve_ftrace_buffer=*/false,
                                   /*syscalls_returning_fd=*/{}};
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("sched", "sched_switch")));
  ds_config.event_filter.AddEnabledEvent(
      table->EventToFtraceId(GroupAndName("ftrace", "print")));

  NullTraceWriter null_writer;
  auto compact_sched_buf = std::make_unique<CompactSchedBuffer>();
  base::FlatSet<protos::pbzero::FtraceParseStatus> parse_errors;
  uint64_t last_read_event_ts = 0;
  CpuReader::ProcessPagesForDataSource(
      &null_writer, &metadata, /*cpu=*/0, &ds_config, &parse_errors,
      &last_read_event_ts, g_page,
      /*pages_read=*/1, compact_sched_buf.get(), table, /*symbolizer*/ nullptr,
      /*ftrace_clock_snapshot=*/nullptr,
      protos::pbzero::FTRACE_CLOCK_UNSPECIFIED);
}

}  // namespace perfetto

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  perfetto::FuzzCpuReaderProcessPagesForDataSource(data, size);
  return 0;
}
