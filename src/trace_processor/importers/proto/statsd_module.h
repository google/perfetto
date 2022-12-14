/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STATSD_MODULE_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STATSD_MODULE_H_

#include <cstdint>

#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/optional.h"
#include "protos/perfetto/trace/trace_packet.pbzero.h"
#include "src/trace_processor/importers/common/async_track_set_tracker.h"
#include "src/trace_processor/importers/common/trace_parser.h"
#include "src/trace_processor/importers/proto/proto_importer_module.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/slice_tables.h"
#include "src/trace_processor/tables/track_tables.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/descriptors.h"
#include "src/trace_processor/util/proto_to_args_parser.h"

namespace perfetto {
namespace trace_processor {

// Wraps a DescriptorPool and a pointer into that pool. This prevents
// common bugs where moving/changing the pool invalidates the pointer.
class PoolAndDescriptor {
 public:
  PoolAndDescriptor(const uint8_t* data, size_t size, const char* name);
  virtual ~PoolAndDescriptor();

  const DescriptorPool* pool() const { return &pool_; }

  const ProtoDescriptor* descriptor() const { return descriptor_; }

 private:
  PoolAndDescriptor(const PoolAndDescriptor&) = delete;
  PoolAndDescriptor& operator=(const PoolAndDescriptor&) = delete;
  PoolAndDescriptor(PoolAndDescriptor&&) = delete;
  PoolAndDescriptor& operator=(PoolAndDescriptor&&) = delete;

  DescriptorPool pool_;
  const ProtoDescriptor* descriptor_{};
};

class StatsdModule : public ProtoImporterModule {
 public:
  explicit StatsdModule(TraceProcessorContext* context);

  ~StatsdModule() override;

  void ParseTracePacketData(const protos::pbzero::TracePacket_Decoder& decoder,
                            int64_t ts,
                            const TracePacketData&,
                            uint32_t field_id) override;

 private:
  void ParseAtom(int64_t ts, protozero::ConstBytes bytes);
  StringId GetAtomName(uint32_t atom_field_id);
  AsyncTrackSetTracker::TrackSetId InternAsyncTrackSetId();

  TraceProcessorContext* context_;
  base::FlatHashMap<uint32_t, StringId> atom_names_;
  PoolAndDescriptor pool_;
  util::ProtoToArgsParser args_parser_;
  base::Optional<AsyncTrackSetTracker::TrackSetId> track_set_id_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_STATSD_MODULE_H_
