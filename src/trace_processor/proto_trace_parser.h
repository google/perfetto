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

#ifndef SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_

#include <stdint.h>

#include <memory>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/trace_parser.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

// Reads a protobuf trace in chunks and parses it into a form which is
// efficient to query.
class ProtoTraceParser : public TraceParser {
 public:
  // |reader| is the abstract method of getting chunks of size |chunk_size_b|
  // from a trace file with these chunks parsed into |trace|.
  explicit ProtoTraceParser(TraceProcessorContext*);
  ~ProtoTraceParser() override;

  // TraceParser implementation.
  bool Parse(std::unique_ptr<uint8_t[]>, size_t size) override;

 private:
  void ParseInternal(std::unique_ptr<uint8_t[]>, uint8_t* data, size_t size);

  void ParsePacket(const uint8_t* data, size_t length);
  void ParseFtraceEventBundle(const uint8_t* data, size_t length);
  void ParseFtraceEvent(uint32_t cpu, const uint8_t* data, size_t length);
  void ParseSchedSwitch(uint32_t cpu,
                        uint64_t timestamp,
                        const uint8_t* data,
                        size_t length);
  void ParseProcessTree(const uint8_t* data, size_t length);
  void ParseProcess(const uint8_t* data, size_t length);
  void ParseThread(const uint8_t* data, size_t length);

  TraceProcessorContext* context_;

  // Used to glue together trace packets that span across two (or more)
  // Parse() boundaries.
  std::vector<uint8_t> partial_buf_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_PROTO_TRACE_PARSER_H_
