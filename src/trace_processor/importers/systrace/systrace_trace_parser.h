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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_SYSTRACE_SYSTRACE_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_SYSTRACE_SYSTRACE_TRACE_PARSER_H_

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "perfetto/base/status.h"
#include "src/trace_processor/importers/common/chunked_trace_reader.h"
#include "src/trace_processor/importers/common/clock_tracker.h"
#include "src/trace_processor/importers/systrace/systrace_line.h"
#include "src/trace_processor/importers/systrace/systrace_line_parser.h"
#include "src/trace_processor/importers/systrace/systrace_line_tokenizer.h"
#include "src/trace_processor/sorter/trace_sorter.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {

class SystraceTraceParser : public ChunkedTraceReader {
 public:
  explicit SystraceTraceParser(TraceProcessorContext*);
  ~SystraceTraceParser() override;

  // ChunkedTraceReader implementation.
  base::Status Parse(TraceBlobView) override;
  base::Status OnPushDataToSorter() override;
  void OnEventsFullyExtracted() override;

 private:
  enum ParseState {
    kBeforeParse,
    kHtmlBeforeSystrace,
    kTraceDataSection,
    kViewerData,
    kSystrace,
    kProcessDumpLong,
    kProcessDumpShort,
    kCgroupDump,
    kEndOfSystrace,
  };

  // A trace embedded in an HTML file by catapult's trace2html, as the base64
  // (usually of gzipped JSON) inside a <script id="viewer-data"> block.
  struct ViewerDataBlock {
    tables::TraceFileTable::Id file_id;
    std::string base64;
  };

  ParseState state_ = ParseState::kBeforeParse;

  // Used to glue together trace packets that span across two (or more)
  // Parse() boundaries.
  std::deque<uint8_t> partial_buf_;

  SystraceLineTokenizer line_tokenizer_;
  SystraceLineParser line_parser_;
  TraceProcessorContext* ctx_;

  std::unique_ptr<TraceSorter::Stream<SystraceLine>> stream_;

  // Like the members of a zip or tar archive, embedded traces are collected
  // while reading and only parsed at the end of the file. This way each is
  // registered as a child of this file, rather than of the previous block.
  std::vector<ViewerDataBlock> viewer_data_blocks_;
  std::vector<std::unique_ptr<ChunkedTraceReader>> viewer_data_readers_;
};

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_SYSTRACE_SYSTRACE_TRACE_PARSER_H_
