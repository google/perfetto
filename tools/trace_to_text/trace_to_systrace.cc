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

#include "tools/trace_to_text/trace_to_systrace.h"

#include <inttypes.h>
#include <stdio.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "perfetto/traced/sys_stats_counters.h"
#include "tools/trace_to_text/ftrace_event_formatter.h"
#include "tools/trace_to_text/process_formatter.h"
#include "tools/trace_to_text/utils.h"

#include "perfetto/trace/trace.pb.h"
#include "perfetto/trace/trace_packet.pb.h"
#include "perfetto/trace_processor/raw_query.pb.h"

// When running in Web Assembly, fflush() is a no-op and the stdio buffering
// sends progress updates to JS only when a write ends with \n.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
#define PROGRESS_CHAR "\n"
#else
#define PROGRESS_CHAR "\r"
#endif

namespace perfetto {
namespace trace_to_text {

namespace {
using protos::FtraceEvent;
using protos::FtraceEventBundle;
using protos::ProcessTree;
using protos::Trace;
using protos::TracePacket;
using protos::SysStats;

// Having an empty traceEvents object is necessary for trace viewer to
// load the json properly.
const char kTraceHeader[] = R"({
  "traceEvents": [],
)";

const char kTraceFooter[] = R"(\n",
  "controllerTraceDataKey": "systraceController"
})";

const char kProcessDumpHeader[] =
    ""
    "\"androidProcessDump\": "
    "\"PROCESS DUMP\\nUSER           PID  PPID     VSZ    RSS WCHAN  "
    "PC S NAME                        COMM                       \\n";

const char kThreadHeader[] = "USER           PID   TID CMD \\n";

const char kSystemTraceEvents[] =
    ""
    "  \"systemTraceEvents\": \"";

const char kFtraceHeader[] =
    "# tracer: nop\n"
    "#\n"
    "# entries-in-buffer/entries-written: 30624/30624   #P:4\n"
    "#\n"
    "#                                      _-----=> irqs-off\n"
    "#                                     / _----=> need-resched\n"
    "#                                    | / _---=> hardirq/softirq\n"
    "#                                    || / _--=> preempt-depth\n"
    "#                                    ||| /     delay\n"
    "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\n"
    "#              | |        |      |   ||||       |         |\n";

const char kFtraceJsonHeader[] =
    "# tracer: nop\\n"
    "#\\n"
    "# entries-in-buffer/entries-written: 30624/30624   #P:4\\n"
    "#\\n"
    "#                                      _-----=> irqs-off\\n"
    "#                                     / _----=> need-resched\\n"
    "#                                    | / _---=> hardirq/softirq\\n"
    "#                                    || / _--=> preempt-depth\\n"
    "#                                    ||| /     delay\\n"
    "#           TASK-PID    TGID   CPU#  ||||    TIMESTAMP  FUNCTION\\n"
    "#              | |        |      |   ||||       |         |\\n";

}  // namespace

int TraceToExperimentalSystrace(std::istream* input, std::ostream* output) {
  trace_processor::Config config;
  config.optimization_mode = trace_processor::OptimizationMode::kMaxBandwidth;
  std::unique_ptr<trace_processor::TraceProcessor> tp =
      trace_processor::TraceProcessor::CreateInstance(config);

  // 1MB chunk size seems the best tradeoff on a MacBook Pro 2013 - i7 2.8 GHz.
  constexpr size_t kChunkSize = 1024 * 1024;

  uint64_t file_size = 0;
  for (int i = 0;; i++) {
    if (i % 128 == 0)
      fprintf(stderr, "\x1b[2K\rLoading trace: %.2f MB\r", file_size / 1E6);

    std::unique_ptr<uint8_t[]> buf(new uint8_t[kChunkSize]);
    input->read(reinterpret_cast<char*>(buf.get()), kChunkSize);
    if (input->bad()) {
      PERFETTO_ELOG("Failed when reading trace");
      return 1;
    }

    auto rsize = input->gcount();
    if (rsize <= 0)
      break;
    file_size += static_cast<uint64_t>(rsize);
    tp->Parse(std::move(buf), static_cast<size_t>(rsize));
  }
  tp->NotifyEndOfFile();

  *output << "TRACE:\n";
  *output << kFtraceHeader;

  constexpr uint32_t kNumRowsToQuery = 100000;
  int64_t start_ts = 0;
  bool has_more = true;
  for (uint32_t i = 0; has_more; i++) {
    protos::RawQueryArgs query_args;

    char buffer[1024];
    sprintf(buffer,
            "select ts, to_ftrace(id) from raw "
            "where ts >= %" PRId64 " limit %" PRIu32,
            start_ts, kNumRowsToQuery);
    query_args.set_sql_query(buffer);

    // This query is not actually async so just pull the result up to the
    // function level so we can return on error.
    protos::RawQueryResult result;
    tp->ExecuteQuery(query_args, [&result](const protos::RawQueryResult& res) {
      result = res;
    });

    if (result.has_error()) {
      PERFETTO_ELOG("Error while writing systrace %s", result.error().c_str());
      return 1;
    }

    // The code below relies on there being at least one row so just break if
    // we don't.
    auto num_rows = result.num_records();
    if (num_rows == 0) {
      has_more = false;
      break;
    }

    // Store the end timestamp so we can start iterating from there next time.
    const auto& ts_col = result.columns(0).long_values();
    start_ts = ts_col.Get(ts_col.size() - 1);

    // Compute how many rows we should print out - this should be the first
    // index with the timestamp |start_ts|. Usually this is just |size - 1| but
    // if multiple rows have the same timestamp, this can be earlier.
    auto rit = std::find(ts_col.rbegin(), ts_col.rend(), start_ts);
    auto rdistance = std::distance(ts_col.rbegin(), rit);
    auto last_row = static_cast<uint32_t>(ts_col.size() - 1 - rdistance);

    // Print out everything until this row.
    for (uint64_t row = 0; row < last_row; row++) {
      int idx = static_cast<int>(row);
      const std::string& line = result.columns(1).string_values(idx);
      *output << line << "\n";
    }

    output->flush();

    // Update the seen count to the number of output rows and only continue if
    // we saw exactly the number of rows we asked for.
    has_more = kNumRowsToQuery == result.num_records();

    uint64_t printed_rows = i * kNumRowsToQuery + result.num_records();
    fprintf(stderr, "\x1b[2K\rWritten %" PRIu64 " rows\r", printed_rows);
  }

  return 0;
}

int TraceToSystrace(std::istream* input,
                    std::ostream* output,
                    bool wrap_in_json) {
  std::multimap<uint64_t, std::string> ftrace_sorted;
  std::vector<std::string> proc_dump;
  std::vector<std::string> thread_dump;
  std::unordered_map<uint32_t /*tid*/, uint32_t /*tgid*/> thread_map;
  std::unordered_map<uint32_t /*tid*/, std::string> thread_names;

  std::vector<const char*> meminfo_strs = BuildMeminfoCounterNames();
  std::vector<const char*> vmstat_strs = BuildVmstatCounterNames();

  std::vector<protos::TracePacket> packets_to_process;

  ForEachPacketInTrace(
      input, [&thread_map, &packets_to_process, &proc_dump, &thread_names,
              &thread_dump](const protos::TracePacket& packet) {
        if (!packet.has_process_tree()) {
          packets_to_process.emplace_back(std::move(packet));
          return;
        }
        const ProcessTree& process_tree = packet.process_tree();
        for (const auto& process : process_tree.processes()) {
          // Main threads will have the same pid as tgid.
          thread_map[static_cast<uint32_t>(process.pid())] =
              static_cast<uint32_t>(process.pid());
          std::string p = FormatProcess(process);
          proc_dump.emplace_back(p);
        }
        for (const auto& thread : process_tree.threads()) {
          // Populate thread map for matching tids to tgids.
          thread_map[static_cast<uint32_t>(thread.tid())] =
              static_cast<uint32_t>(thread.tgid());
          if (thread.has_name()) {
            thread_names[static_cast<uint32_t>(thread.tid())] = thread.name();
          }
          std::string t = FormatThread(thread);
          thread_dump.emplace_back(t);
        }
      });

  for (const auto& packet : packets_to_process) {
    if (packet.has_ftrace_events()) {
      const FtraceEventBundle& bundle = packet.ftrace_events();
      for (const FtraceEvent& event : bundle.event()) {
        std::string line = FormatFtraceEvent(event.timestamp(), bundle.cpu(),
                                             event, thread_map, thread_names);
        if (line == "")
          continue;
        ftrace_sorted.emplace(event.timestamp(), line);
      }
    }  // packet.has_ftrace_events

    if (packet.has_sys_stats()) {
      const SysStats& sys_stats = packet.sys_stats();
      for (const auto& meminfo : sys_stats.meminfo()) {
        FtraceEvent event;
        uint64_t ts = static_cast<uint64_t>(packet.timestamp());
        char str[256];
        event.set_timestamp(ts);
        event.set_pid(1);
        sprintf(str, "C|1|%s|%" PRIu64, meminfo_strs[meminfo.key()],
                static_cast<uint64_t>(meminfo.value()));
        event.mutable_print()->set_buf(str);
        ftrace_sorted.emplace(
            ts, FormatFtraceEvent(ts, 0, event, thread_map, thread_names));
      }
      for (const auto& vmstat : sys_stats.vmstat()) {
        FtraceEvent event;
        uint64_t ts = static_cast<uint64_t>(packet.timestamp());
        char str[256];
        event.set_timestamp(ts);
        event.set_pid(1);
        sprintf(str, "C|1|%s|%" PRIu64, vmstat_strs[vmstat.key()],
                static_cast<uint64_t>(vmstat.value()));
        event.mutable_print()->set_buf(str);
        ftrace_sorted.emplace(
            ts, FormatFtraceEvent(ts, 0, event, thread_map, thread_names));
      }
    }
  }

  if (wrap_in_json) {
    *output << kTraceHeader;
    *output << kProcessDumpHeader;
    for (const auto& process : proc_dump) {
      *output << process << "\\n";
    }
    *output << kThreadHeader;
    for (const auto& thread : thread_dump) {
      *output << thread << "\\n";
    }
    *output << "\",";
    *output << kSystemTraceEvents;
    *output << kFtraceJsonHeader;
  } else {
    *output << "TRACE:\n";
    *output << kFtraceHeader;
  }

  fprintf(stderr, "\n");
  size_t total_events = ftrace_sorted.size();
  size_t written_events = 0;
  std::vector<char> escaped_str;
  for (auto it = ftrace_sorted.begin(); it != ftrace_sorted.end(); it++) {
    if (wrap_in_json) {
      escaped_str.clear();
      escaped_str.reserve(it->second.size() * 101 / 100);
      for (char c : it->second) {
        if (c == '\\' || c == '"')
          escaped_str.push_back('\\');
        escaped_str.push_back(c);
      }
      escaped_str.push_back('\\');
      escaped_str.push_back('n');
      escaped_str.push_back('\0');
      *output << escaped_str.data();
    } else {
      *output << it->second;
      *output << "\n";
    }
    if (!StdoutIsTty() && (written_events++ % 1000 == 0 ||
                           written_events == ftrace_sorted.size())) {
      fprintf(stderr, "Writing trace: %.2f %%" PROGRESS_CHAR,
              written_events * 100.0 / total_events);
      fflush(stderr);
      output->flush();
    }
  }

  if (wrap_in_json)
    *output << kTraceFooter;

  return 0;
}

}  // namespace trace_to_text
}  // namespace perfetto
