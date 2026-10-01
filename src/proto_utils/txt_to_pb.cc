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

#include "src/proto_utils/txt_to_pb.h"

#include <cstdint>
#include <string>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/proto_utils/config.descriptor.h"
#include "src/proto_utils/trace_summary.descriptor.h"
#include "src/protozero/text_to_proto/text_to_proto.h"

namespace perfetto {
namespace {
constexpr char kConfigProtoName[] = ".perfetto.protos.TraceConfig";
constexpr char kTraceSummarySpecProtoName[] =
    ".perfetto.protos.TraceSummarySpec";

}  // namespace

base::StatusOr<std::vector<uint8_t>> TraceConfigTxtToPb(
    const std::string& input,
    const std::string& file_name,
    bool allow_unknown_fields) {
  const auto config_descriptor = kConfigDescriptor.Decode();

  return protozero::TextToProto(config_descriptor.data(),
                                config_descriptor.size(), kConfigProtoName,
                                file_name, input, allow_unknown_fields);
}

base::StatusOr<std::vector<uint8_t>> TraceSummarySpecTxtToPb(
    const std::string& input,
    const std::string& file_name) {
  const auto trace_summary_descriptor = kTraceSummaryDescriptor.Decode();

  return protozero::TextToProto(trace_summary_descriptor.data(),
                                trace_summary_descriptor.size(),
                                kTraceSummarySpecProtoName, file_name, input);
}

}  // namespace perfetto
