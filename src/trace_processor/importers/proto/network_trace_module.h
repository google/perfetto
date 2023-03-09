/*
 * Copyright (C) 2023 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_NETWORK_TRACE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_NETWORK_TRACE_MODULE_H_

#include <cstdint>

#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/parser_types.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class NetworkTraceModule : public ProtoImporterModule {
 public:
  explicit NetworkTraceModule(TraceProcessorContext* context);
  ~NetworkTraceModule() override = default;

  void ParseTracePacketData(const protos::pbzero::TracePacket::Decoder& decoder,
                            int64_t ts,
                            const TracePacketData&,
                            uint32_t field_id) override;

 private:
  void ParseNetworkPacketEvent(int64_t ts, protozero::ConstBytes blob);

  TraceProcessorContext* context_;

  const StringId net_arg_length_;
  const StringId net_arg_ip_proto_;
  const StringId net_arg_tcp_flags_;
  const StringId net_arg_tag_;
  const StringId net_arg_local_port_;
  const StringId net_arg_remote_port_;
  const StringId net_ipproto_tcp_;
  const StringId net_ipproto_udp_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_NETWORK_TRACE_MODULE_H_
