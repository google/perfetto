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

#include "src/trace_processor/importers/systrace/systrace_trace_parser.h"

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/trace_sorter.h"

#include <inttypes.h>
#include <cctype>
#include <string>
#include <unordered_map>

namespace perfetto {
namespace trace_processor {

SystraceTraceParser::SystraceTraceParser(TraceProcessorContext* ctx)
    : line_parser_(ctx) {}
SystraceTraceParser::~SystraceTraceParser() = default;

util::Status SystraceTraceParser::Parse(std::unique_ptr<uint8_t[]> owned_buf,
                                        size_t size) {
  if (state_ == ParseState::kEndOfSystrace)
    return util::OkStatus();
  partial_buf_.insert(partial_buf_.end(), &owned_buf[0], &owned_buf[size]);

  if (state_ == ParseState::kBeforeParse) {
    state_ = partial_buf_[0] == '<' ? ParseState::kHtmlBeforeSystrace
                                    : ParseState::kSystrace;
  }

  // There can be multiple trace data sections in an HTML trace, we want to
  // ignore any that don't contain systrace data. In the future it would be
  // good to also parse the process dump section.
  const char kTraceDataSection[] =
      R"(<script class="trace-data" type="application/text">)";
  auto start_it = partial_buf_.begin();
  for (;;) {
    auto line_it = std::find(start_it, partial_buf_.end(), '\n');
    if (line_it == partial_buf_.end())
      break;

    std::string buffer(start_it, line_it);

    if (state_ == ParseState::kHtmlBeforeSystrace) {
      if (base::Contains(buffer, kTraceDataSection)) {
        state_ = ParseState::kTraceDataSection;
      }
    } else if (state_ == ParseState::kTraceDataSection) {
      if (base::StartsWith(buffer, "#")) {
        state_ = ParseState::kSystrace;
      } else if (base::Contains(buffer, R"(</script>)")) {
        state_ = ParseState::kHtmlBeforeSystrace;
      }
    } else if (state_ == ParseState::kSystrace) {
      if (base::Contains(buffer, R"(</script>)")) {
        state_ = ParseState::kEndOfSystrace;
        break;
      } else if (!base::StartsWith(buffer, "#") && !buffer.empty()) {
        SystraceLine line;
        util::Status status = line_tokenizer_.Tokenize(buffer, &line);
        if (!status.ok())
          return status;
        line_parser_.ParseLine(std::move(line));
      }
    }
    start_it = line_it + 1;
  }
  if (state_ == ParseState::kEndOfSystrace) {
    partial_buf_.clear();
  } else {
    partial_buf_.erase(partial_buf_.begin(), start_it);
  }
  return util::OkStatus();
}

void SystraceTraceParser::NotifyEndOfFile() {}

}  // namespace trace_processor
}  // namespace perfetto
