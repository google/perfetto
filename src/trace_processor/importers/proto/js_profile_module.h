// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Copyright 2025 The Lynx Authors. All rights reserved.
// Licensed under the Apache License Version 2.0 that can be found in the
// LICENSE file in the root directory of this source tree.

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_JS_PROFILE_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_JS_PROFILE_MODULE_H_

#include "perfetto/ext/base/flat_hash_map.h"
#include "protos/perfetto/trace/js_profile/js_profile.pbzero.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

struct CallFrame {
  std::string function_name;
  std::string script_id;
  std::string url;
  int32_t line_number;
  int32_t column_number;
};

struct ProfileNode {
  int32_t id;
  CallFrame call_frame;
  int64_t hit_count;
  int32_t parent;
  int32_t depth;
  std::vector<int32_t> children;
};

struct CpuProfile {
  int64_t start_timestamp;
  int64_t end_timestamp;
  std::vector<ProfileNode> nodes;
  std::vector<int32_t> samples;
  std::vector<int64_t> time_deltas;
  uint64_t track_id;
};

struct CpuProfileData {
  std::string runtime_profile;
  uint64_t track_id;
  bool is_done;
  int32_t profile_id;
};

class JSProfileModule : public ProtoImporterModule {
 public:
  using ConstBytes = protozero::ConstBytes;
  explicit JSProfileModule(TraceProcessorContext* context);

  ModuleResult TokenizePacket(
      const protos::pbzero::TracePacket::Decoder& decoder,
      TraceBlobView* packet,
      int64_t packet_timestamp,
      RefPtr<PacketSequenceStateGeneration> state,
      uint32_t field_id) override;

 private:
  ModuleResult TokenizeJsProfilePacket(
      RefPtr<PacketSequenceStateGeneration> state,
      const protos::pbzero::TracePacket_Decoder&,
      TraceBlobView* packet);

  ModuleResult DecodeJsProfilePacket(
      RefPtr<PacketSequenceStateGeneration> state,
      const protos::pbzero::JSProfilePacket_Decoder& packet);

  ModuleResult TokenizeJsProfilePacketOld(
      RefPtr<PacketSequenceStateGeneration> state,
      const protos::pbzero::TracePacket_Decoder&,
      TraceBlobView* packet);

  TraceProcessorContext* context_;
  base::FlatHashMap<int32_t, CpuProfileData> cpu_profiles_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_JS_PROFILE_MODULE_H_
