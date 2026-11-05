/*
 * Copyright (C) 2025 The Android Open Source Project
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

#include "src/protovm/test/utils.h"

#include "perfetto/base/logging.h"
#include "protos/perfetto/trace/trace.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"

namespace perfetto {
namespace protovm {
namespace test {

protozero::ConstBytes AsConstBytes(const std::string& s) {
  return protozero::ConstBytes{
      reinterpret_cast<uint8_t*>(const_cast<char*>(s.data())), s.size()};
}

protozero::ConstBytes ExtractBytes(const std::string& trace_bytes) {
  protos::pbzero::Trace::Decoder trace(trace_bytes);
  for (auto packet_it = trace.packet(); packet_it; ++packet_it) {
    protos::pbzero::TracePacket::Decoder packet(*packet_it);
    if (!packet.has_protovms()) {
      continue;
    }
    protos::pbzero::TracePacket::ProtoVms::Decoder protovms(packet.protovms());
    if (auto instance_it = protovms.instance(); instance_it) {
      protos::pbzero::TracePacket::ProtoVms::Instance::Decoder instance(
          *instance_it);
      PERFETTO_CHECK(instance.program().data != nullptr &&
                     instance.program().size > 0);
      return instance.program();
    }
  }
  PERFETTO_FATAL("Trace has no program");
}

}  // namespace test
}  // namespace protovm
}  // namespace perfetto
