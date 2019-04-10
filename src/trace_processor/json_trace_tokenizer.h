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

#ifndef SRC_TRACE_PROCESSOR_JSON_TRACE_TOKENIZER_H_
#define SRC_TRACE_PROCESSOR_JSON_TRACE_TOKENIZER_H_

#include <stdint.h>

#include "src/trace_processor/chunked_trace_reader.h"
#include "src/trace_processor/trace_sorter.h"
#include "src/trace_processor/trace_storage.h"

namespace Json {
class Value;
}

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Visible for testing.
enum ReadDictRes { kFoundDict, kNeedsMoreData, kEndOfTrace, kFatalError };

// Visible for testing.
ReadDictRes ReadOneJsonDict(const char* start,
                            const char* end,
                            Json::Value* value,
                            const char** next);

// Reads a JSON trace in chunks and extracts top level json objects.
class JsonTraceTokenizer : public ChunkedTraceReader {
 public:
  explicit JsonTraceTokenizer(TraceProcessorContext*);
  ~JsonTraceTokenizer() override;

  // ChunkedTraceReader implementation.
  bool Parse(std::unique_ptr<uint8_t[]>, size_t) override;

 private:
  TraceProcessorContext* const context_;

  uint64_t offset_ = 0;
  // Used to glue together JSON objects that span across two (or more)
  // Parse boundaries.
  std::vector<char> buffer_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_JSON_TRACE_TOKENIZER_H_
