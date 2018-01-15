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

#include <inttypes.h>

#include <stdio.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/util/field_comparator.h>
#include <google/protobuf/util/message_differencer.h>

#include "perfetto/base/logging.h"
#include "protos/trace.pb.h"
#include "protos/trace_packet.pb.h"

namespace perfetto {
namespace {

const char kTraceHeader[] = R"({
  "traceEvents": [],
)";

const char kTraceFooter[] = R"(\n",
  "controllerTraceDataKey": "systraceController"
})";

const char kFtraceHeader[] =
    ""
    "  \"systemTraceEvents\": \""
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

using google::protobuf::Descriptor;
using google::protobuf::DynamicMessageFactory;
using google::protobuf::FileDescriptor;
using google::protobuf::Message;
using google::protobuf::TextFormat;
using google::protobuf::compiler::DiskSourceTree;
using google::protobuf::compiler::Importer;
using google::protobuf::compiler::MultiFileErrorCollector;
using google::protobuf::io::OstreamOutputStream;
using protos::FtraceEvent;
using protos::FtraceEventBundle;
using protos::PrintFtraceEvent;
using protos::SchedSwitchFtraceEvent;
using protos::Trace;
using protos::TracePacket;

class MFE : public MultiFileErrorCollector {
  virtual void AddError(const std::string& filename,
                        int line,
                        int column,
                        const std::string& message) {
    PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }

  virtual void AddWarning(const std::string& filename,
                          int line,
                          int column,
                          const std::string& message) {
    PERFETTO_ELOG("Error %s %d:%d: %s", filename.c_str(), line, column,
                  message.c_str());
  }
};

const char* GetFlag(int32_t state) {
  state &= 511;
  if (state & 1)
    return "S";
  if (state & 2)
    return "D";
  if (state & 4)
    return "T";
  if (state & 8)
    return "t";
  if (state & 16)
    return "Z";
  if (state & 32)
    return "X";
  if (state & 64)
    return "x";
  if (state & 128)
    return "W";
  return "R";
}

uint64_t TimestampToSeconds(uint64_t timestamp) {
  return timestamp / 1000000000ul;
}

uint64_t TimestampToMicroseconds(uint64_t timestamp) {
  return (timestamp / 1000) % 1000000ul;
}

std::string FormatSchedSwitch(uint64_t timestamp,
                              uint64_t cpu,
                              const SchedSwitchFtraceEvent& sched_switch) {
  char line[2048];
  uint64_t seconds = TimestampToSeconds(timestamp);
  uint64_t useconds = TimestampToMicroseconds(timestamp);
  sprintf(line,
          "<idle>-0     (-----) [%03" PRIu64 "] d..3 %" PRIu64 ".%.6" PRIu64
          ": sched_switch: prev_comm=%s "
          "prev_pid=%d prev_prio=%d prev_state=%s ==> next_comm=%s next_pid=%d "
          "next_prio=%d\\n",
          cpu, seconds, useconds, sched_switch.prev_comm().c_str(),
          sched_switch.prev_pid(), sched_switch.prev_prio(),
          GetFlag(sched_switch.prev_state()), sched_switch.next_comm().c_str(),
          sched_switch.next_pid(), sched_switch.next_prio());
  return std::string(line);
}

std::string FormatPrint(uint64_t timestamp,
                        uint64_t cpu,
                        const PrintFtraceEvent& print) {
  char line[2048];
  uint64_t seconds = TimestampToSeconds(timestamp);
  uint64_t useconds = TimestampToMicroseconds(timestamp);
  std::string msg = print.buf();
  // Remove any newlines in the message. It's not entirely clear what the right
  // behaviour is here. Maybe we should escape them instead?
  msg.erase(std::remove(msg.begin(), msg.end(), '\n'), msg.end());
  sprintf(line,
          "<idle>-0     (-----) [%03" PRIu64 "] d..3 %" PRIu64 ".%.6" PRIu64
          ": tracing_mark_write: %s\\n",
          cpu, seconds, useconds, msg.c_str());
  return std::string(line);
}

int TraceToText(std::istream* input, std::ostream* output) {
  DiskSourceTree dst;
  dst.MapPath("protos", "protos");
  MFE mfe;
  Importer importer(&dst, &mfe);
  const FileDescriptor* parsed_file = importer.Import("protos/trace.proto");

  DynamicMessageFactory dmf;
  const Descriptor* trace_descriptor = parsed_file->message_type(0);
  const Message* msg_root = dmf.GetPrototype(trace_descriptor);
  Message* msg = msg_root->New();

  if (!msg->ParseFromIstream(input)) {
    PERFETTO_ELOG("Could not parse input.");
    return 1;
  }
  OstreamOutputStream zero_copy_output(output);
  TextFormat::Print(*msg, &zero_copy_output);
  return 0;
}

int TraceToSystrace(std::istream* input, std::ostream* output) {
  std::multimap<uint64_t, std::string> sorted;

  std::string raw;
  std::istreambuf_iterator<char> begin(*input), end;
  raw.assign(begin, end);
  Trace trace;
  if (!trace.ParseFromString(raw)) {
    PERFETTO_ELOG("Could not parse input.");
    return 1;
  }

  for (const TracePacket& packet : trace.packet()) {
    if (!packet.has_ftrace_events())
      continue;

    const FtraceEventBundle& bundle = packet.ftrace_events();
    for (const FtraceEvent& event : bundle.event()) {
      if (event.has_sched_switch()) {
        const SchedSwitchFtraceEvent& sched_switch = event.sched_switch();
        sorted.emplace(
            event.timestamp(),
            FormatSchedSwitch(event.timestamp(), bundle.cpu(), sched_switch));
      } else if (event.has_print()) {
        const PrintFtraceEvent& print = event.print();
        sorted.emplace(event.timestamp(),
                       FormatPrint(event.timestamp(), bundle.cpu(), print));
      }
    }
  }

  *output << kTraceHeader;
  *output << kFtraceHeader;

  for (auto it = sorted.begin(); it != sorted.end(); it++)
    *output << it->second;

  *output << kTraceFooter;

  return 0;
}

}  // namespace
}  // namespace perfetto

namespace {

int Usage(int argc, char** argv) {
  printf("Usage: %s [systrace|text] < trace.proto > trace.txt\n", argv[0]);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return Usage(argc, argv);

  std::string format(argv[1]);

  if (format != "systrace" && format != "text")
    return Usage(argc, argv);

  bool systrace = format == "systrace";

  if (systrace) {
    return perfetto::TraceToSystrace(&std::cin, &std::cout);
  } else {
    return perfetto::TraceToText(&std::cin, &std::cout);
  }
}
