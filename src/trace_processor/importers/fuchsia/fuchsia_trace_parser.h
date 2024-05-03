/*
 * Copyright (C) 2019 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_PARSER_H_

#include <functional>
#include <optional>
#include <vector>

#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_record.h"
#include "src/trace_processor/importers/fuchsia/fuchsia_trace_utils.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

class FuchsiaTraceParser : public FuchsiaRecordParser {
 public:
  explicit FuchsiaTraceParser(TraceProcessorContext*);
  ~FuchsiaTraceParser() override;

  void ParseFuchsiaRecord(int64_t timestamp, FuchsiaRecord fr) override;

  struct Arg {
    StringId name;
    fuchsia_trace_utils::ArgValue value;
  };

  // Utility to parse record arguments. Exposed here to provide consistent
  // parsing between trace parsing and tokenization.
  //
  // Returns an empty optional on error, otherwise a vector containing zero or
  // more arguments.
  static std::optional<std::vector<Arg>> ParseArgs(
      fuchsia_trace_utils::RecordCursor& cursor,
      uint32_t n_args,
      std::function<StringId(base::StringView string)> intern_string,
      std::function<StringId(uint32_t index)> get_string);

 private:
  TraceProcessorContext* const context_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_FUCHSIA_FUCHSIA_TRACE_PARSER_H_
