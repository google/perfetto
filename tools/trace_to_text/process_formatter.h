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

#ifndef TOOLS_TRACE_TO_TEXT_PROCESS_FORMATTER_H_
#define TOOLS_TRACE_TO_TEXT_PROCESS_FORMATTER_H_

#include <string>

#include "perfetto/base/string_writer.h"
#include "perfetto/trace/trace_packet.pb.h"

namespace perfetto {

inline std::string FormatProcess(const protos::ProcessTree::Process& p) {
  char line[2048];
  sprintf(line,
          "root             %d     %d   00000   000 null 0000000000 S %s       "
          "  null",
          p.pid(), p.ppid(), p.cmdline(0).c_str());
  return line;
};

inline std::string FormatThread(const protos::ProcessTree::Thread& t) {
  char line[2048];
  std::string name;
  if (t.has_name()) {
    name = t.name();
  } else {
    name = "<...>";
  }
  sprintf(line, "root         %d %d %s", t.tgid(), t.tid(), name.c_str());
  return line;
};

inline void FormatProcess(uint32_t pid,
                          uint32_t ppid,
                          const base::StringView& name,
                          base::StringWriter* writer) {
  writer->AppendLiteral("root             ");
  writer->AppendInt(pid);
  writer->AppendLiteral("     ");
  writer->AppendInt(ppid);
  writer->AppendLiteral("   00000   000 null 0000000000 S ");
  writer->AppendString(name);
  writer->AppendLiteral("         null");
}

inline void FormatThread(uint32_t tid,
                         uint32_t tgid,
                         const base::StringView& name,
                         base::StringWriter* writer) {
  writer->AppendLiteral("root         ");
  writer->AppendInt(tgid);
  writer->AppendChar(' ');
  writer->AppendInt(tid);
  writer->AppendChar(' ');
  if (name.empty()) {
    writer->AppendLiteral("<...>");
  } else {
    writer->AppendString(name);
  }
}

}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_PROCESS_FORMATTER_H_
