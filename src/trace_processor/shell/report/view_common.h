/*
 * Copyright (C) 2026 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_VIEW_COMMON_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_VIEW_COMMON_H_

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/trace_processor/basic_types.h"
#include "protos/perfetto/trace_processor/report.pbzero.h"
#include "src/trace_processor/shell/report/report_sink.h"

namespace perfetto::trace_processor::shell {

// Escapes a string for inclusion in a single-quoted SQL literal.
inline std::string EscapeSqlLiteral(const std::string& s) {
  return base::ReplaceAll(s, "'", "''");
}

// Declares a section's columns (name + format), in order. A view calls this on
// its SectionInfo so the renderer takes headers and formatting from the stream
// rather than from the noun.
inline void SetColumns(
    protos::pbzero::SectionInfo* s,
    std::initializer_list<
        std::pair<const char*, protos::pbzero::SectionInfo::ColumnFormat>>
        cols) {
  for (const auto& [name, format] : cols) {
    auto* c = s->add_columns();
    c->set_name(name);
    c->set_format(format);
  }
}

// Serializes |packet| and forwards the bytes to |sink|.
inline base::Status EmitPacket(
    ReportSink* sink,
    protozero::HeapBuffered<protos::pbzero::ReportPacket>* packet) {
  std::vector<uint8_t> bytes = packet->SerializeAsArray();
  return sink->OnPacket({bytes.data(), bytes.size()});
}

// Reads a SqlValue as int64, tolerating null and double-typed results.
inline int64_t AsI64(const SqlValue& v) {
  if (v.is_null())
    return 0;
  if (v.type == SqlValue::kDouble)
    return static_cast<int64_t>(v.AsDouble());
  return v.AsLong();
}

// Reads a SqlValue as double, tolerating null and long-typed results.
inline double AsF64(const SqlValue& v) {
  if (v.is_null())
    return 0.0;
  if (v.type == SqlValue::kLong)
    return static_cast<double>(v.AsLong());
  return v.AsDouble();
}

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_VIEW_COMMON_H_
