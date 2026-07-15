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

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_TEXT_RENDERER_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_TEXT_RENDERER_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/status.h"
#include "perfetto/protozero/field.h"
#include "src/trace_processor/shell/report/report_sink.h"

namespace perfetto::trace_processor::shell {

// A ReportSink that decodes the packet stream and renders human-readable tables
// (the default `report` output). Sections are bounded by --top, so it buffers a
// section's rows to align columns, then prints on the next section boundary.
class TextSink : public ReportSink {
 public:
  // |overview| controls whether "Next:" drill-down hints are printed at the end
  // (shown for the overview, omitted for single-noun views).
  TextSink(FILE* out, bool overview);
  ~TextSink() override;

  base::Status OnPacket(protozero::ConstBytes packet) override;
  base::Status Finalize() override;

  // A raw table cell. The value is stored untyped; the section's per-column
  // format (from SectionInfo) decides how it is rendered to a string.
  struct Cell {
    enum Kind { kStr, kInt, kDbl } kind = kStr;
    std::string str;
    int64_t i = 0;
    double d = 0;
    static Cell Str(std::string v) {
      Cell c;
      c.kind = kStr;
      c.str = std::move(v);
      return c;
    }
    static Cell Int(int64_t v) {
      Cell c;
      c.kind = kInt;
      c.i = v;
      return c;
    }
    static Cell Dbl(double v) {
      Cell c;
      c.kind = kDbl;
      c.d = v;
      return c;
    }
  };

 private:
  void FlushSection();

  FILE* out_;
  bool overview_;
  std::string trace_file_;

  bool in_section_ = false;
  std::string section_title_;
  std::string row_noun_;
  int64_t total_rows_ = 0;
  int64_t shown_rows_ = 0;
  int64_t total_items_ = 0;
  std::vector<std::string> columns_;
  // Per-column format (SectionInfo::ColumnFormat), parallel to columns_.
  std::vector<int32_t> column_formats_;
  std::vector<std::vector<Cell>> rows_;
};

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_TEXT_RENDERER_H_
