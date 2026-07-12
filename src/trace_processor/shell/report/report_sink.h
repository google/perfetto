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

#ifndef SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_SINK_H_
#define SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_SINK_H_

#include <cstdio>
#include <memory>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/protozero/field.h"

namespace perfetto::trace_processor {
class DescriptorPool;
}  // namespace perfetto::trace_processor

namespace perfetto::trace_processor::shell {

// Consumes the report stream one serialized ReportPacket at a time. View
// builders emit packets to a sink; the sink encodes them. Because every sink
// sees the identical packet bytes, the binary, JSONL and text encodings cannot
// drift from each other.
class ReportSink {
 public:
  virtual ~ReportSink();

  // Called once per packet, in emission order. |packet| is a serialized
  // ReportPacket and is only valid for the duration of the call.
  virtual base::Status OnPacket(protozero::ConstBytes packet) = 0;

  // Flushes any buffered state. Called once after the last packet.
  virtual base::Status Finalize() = 0;
};

// Writes the stream as length-delimited ReportPackets, i.e. the on-wire form of
// `Report { repeated ReportPacket packet = 1; }`. Truncating the output at any
// point still yields a parseable prefix.
class BinarySink : public ReportSink {
 public:
  explicit BinarySink(FILE* out) : out_(out) {}
  base::Status OnPacket(protozero::ConstBytes packet) override;
  base::Status Finalize() override;

 private:
  FILE* out_;
};

// Writes one JSON object per packet, newline-delimited (JSONL). The JSON is the
// proto3 JSON mapping of the same ReportPacket schema, so it is line-based,
// streamable and incrementally parseable.
class JsonlSink : public ReportSink {
 public:
  static base::StatusOr<std::unique_ptr<JsonlSink>> Create(FILE* out);
  ~JsonlSink() override;
  base::Status OnPacket(protozero::ConstBytes packet) override;
  base::Status Finalize() override;

 private:
  JsonlSink(FILE* out, std::unique_ptr<DescriptorPool> pool);
  FILE* out_;
  std::unique_ptr<DescriptorPool> pool_;
};

}  // namespace perfetto::trace_processor::shell

#endif  // SRC_TRACE_PROCESSOR_SHELL_REPORT_REPORT_SINK_H_
