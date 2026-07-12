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

#include "src/trace_processor/shell/report/report_sink.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/protozero/proto_utils.h"
#include "src/trace_processor/shell/report.descriptor.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/protozero_to_json.h"

namespace perfetto::trace_processor::shell {

ReportSink::~ReportSink() = default;

base::Status BinarySink::OnPacket(protozero::ConstBytes packet) {
  // Frame the packet as field 1 (`Report.packet`, length-delimited).
  uint8_t header[16];
  uint8_t* p = header;
  p = protozero::proto_utils::WriteVarInt(
      protozero::proto_utils::MakeTagLengthDelimited(1), p);
  p = protozero::proto_utils::WriteVarInt(packet.size, p);
  auto header_size = static_cast<size_t>(p - header);
  if (fwrite(header, 1, header_size, out_) != header_size ||
      fwrite(packet.data, 1, packet.size, out_) != packet.size) {
    return base::ErrStatus("Failed to write report packet");
  }
  return base::OkStatus();
}

base::Status BinarySink::Finalize() {
  return base::OkStatus();
}

base::StatusOr<std::unique_ptr<JsonlSink>> JsonlSink::Create(FILE* out) {
  auto pool = std::make_unique<DescriptorPool>();
  RETURN_IF_ERROR(pool->AddFromFileDescriptorSet(kReportDescriptor.data(),
                                                 kReportDescriptor.size()));
  return std::unique_ptr<JsonlSink>(new JsonlSink(out, std::move(pool)));
}

JsonlSink::JsonlSink(FILE* out, std::unique_ptr<DescriptorPool> pool)
    : out_(out), pool_(std::move(pool)) {}

JsonlSink::~JsonlSink() = default;

base::Status JsonlSink::OnPacket(protozero::ConstBytes packet) {
  std::string json = protozero_to_json::ProtozeroToJson(
      *pool_, ".perfetto.protos.ReportPacket", packet,
      protozero_to_json::kNone);
  if (fwrite(json.data(), 1, json.size(), out_) != json.size() ||
      fputc('\n', out_) == EOF) {
    return base::ErrStatus("Failed to write report packet");
  }
  return base::OkStatus();
}

base::Status JsonlSink::Finalize() {
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor::shell
