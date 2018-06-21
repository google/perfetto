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

#ifndef SRC_TRACE_PROCESSOR_TRACE_PARSER_H_
#define SRC_TRACE_PROCESSOR_TRACE_PARSER_H_

#include <stdint.h>
#include <memory>

#include "perfetto/protozero/proto_decoder.h"
#include "src/trace_processor/blob_reader.h"
#include "src/trace_processor/trace_storage.h"

namespace perfetto {
namespace trace_processor {

// Reads a trace in chunks from an abstract data source and parses it into a
// form which is efficient to query.
class TraceParser {
 public:
  // |reader| is the abstract method of getting chunks of size |chunk_size_b|
  // from a trace file with these chunks parsed into |trace|.
  TraceParser(BlobReader* reader, TraceStorage* storage, uint32_t chunk_size_b);

  // Parses the next chunk of TracePackets from the BlobReader. Returns true
  // if there are more chunks which can be read and false otherwise.
  bool ParseNextChunk();

 private:
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

  BlobReader* const reader_;
  TraceStorage* const storage_;
  const uint32_t chunk_size_b_;

  uint64_t offset_ = 0;
  std::unique_ptr<uint8_t[]> buffer_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_TRACE_PARSER_H_
