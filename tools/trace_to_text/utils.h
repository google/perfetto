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

#ifndef TOOLS_TRACE_TO_TEXT_UTILS_H_
#define TOOLS_TRACE_TO_TEXT_UTILS_H_

#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include <zlib.h>

#include "perfetto/base/build_config.h"
#include "perfetto/ext/base/paged_memory.h"
#include "perfetto/profiling/symbolizer.h"
#include "perfetto/trace_processor/trace_processor.h"

namespace perfetto {

namespace protos {
class TracePacket;
}

namespace trace_to_text {

// When running in Web Assembly, fflush() is a no-op and the stdio buffering
// sends progress updates to JS only when a write ends with \n.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WASM)
constexpr char kProgressChar = '\n';
#else
constexpr char kProgressChar = '\r';
#endif

void ForEachPacketBlobInTrace(
    std::istream* input,
    const std::function<void(std::unique_ptr<char[]>, size_t)>&);

void ForEachPacketInTrace(
    std::istream* input,
    const std::function<void(const protos::TracePacket&)>&);

std::vector<std::string> GetPerfettoBinaryPath();

bool ReadTrace(trace_processor::TraceProcessor* tp, std::istream* input);

void SymbolizeDatabase(
    trace_processor::TraceProcessor* tp,
    Symbolizer* symbolizer,
    std::function<void(perfetto::protos::TracePacket)> callback);

class TraceWriter {
 public:
  TraceWriter(std::ostream* output);
  virtual ~TraceWriter();

  void Write(std::string s);
  virtual void Write(const char* data, size_t sz);

 private:
  std::ostream* output_;
};

class DeflateTraceWriter : public TraceWriter {
 public:
  DeflateTraceWriter(std::ostream* output);
  ~DeflateTraceWriter() override;

  void Write(const char* data, size_t sz) override;

 private:
  void Flush();
  void CheckEq(int actual_code, int expected_code);

  z_stream stream_{};
  base::PagedMemory buf_;
  uint8_t* const start_;
  uint8_t* const end_;
};

}  // namespace trace_to_text
}  // namespace perfetto

#endif  // TOOLS_TRACE_TO_TEXT_UTILS_H_
