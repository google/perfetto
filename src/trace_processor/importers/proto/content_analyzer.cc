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

#include <numeric>
#include <utility>

#include "perfetto/ext/base/string_utils.h"
#include "src/trace_processor/importers/proto/content_analyzer.h"
#include "src/trace_processor/importers/trace.descriptor.h"
#include "src/trace_processor/storage/trace_storage.h"

namespace perfetto {
namespace trace_processor {

ContentAnalyzerModule::ContentAnalyzerModule(TraceProcessorContext* context)
    : context_(context) {
  base::Status status = pool_.AddFromFileDescriptorSet(kTraceDescriptor.data(),
                                                       kTraceDescriptor.size());
  if (!status.ok()) {
    PERFETTO_ELOG("Could not add TracePacket proto descriptor %s",
                  status.c_message());
  }
  RegisterForAllFields(context_);
}

ModuleResult ContentAnalyzerModule::TokenizePacket(
    const protos::pbzero::TracePacket_Decoder&,
    TraceBlobView* packet,
    int64_t /*packet_timestamp*/,
    PacketSequenceState*,
    uint32_t /*field_id*/) {
  util::SizeProfileComputer computer(&pool_);
  auto packet_samples = computer.Compute(packet->data(), packet->length(),
                                         ".perfetto.protos.TracePacket");
  for (auto it = packet_samples.GetIterator(); it; ++it) {
    auto& aggregated_samples_for_path = aggregated_samples_[it.key()];
    aggregated_samples_for_path.insert(aggregated_samples_for_path.end(),
                                       it.value().begin(), it.value().end());
  }
  return ModuleResult::Ignored();
}

void ContentAnalyzerModule::NotifyEndOfFile() {
  // TODO(kraskevich): consider generating a flamegraph-compatable table once
  // Perfetto UI supports custom flamegraphs (b/227644078).
  for (auto it = aggregated_samples_.GetIterator(); it; ++it) {
    auto field_path = base::Join(it.key(), ".");
    auto total_size = std::accumulate(it.value().begin(), it.value().end(), 0L);
    tables::ExperimentalProtoContentTable::Row row;
    row.path = context_->storage->InternString(base::StringView(field_path));
    row.total_size = total_size;
    context_->storage->mutable_experimental_proto_content_table()->Insert(row);
  }
  aggregated_samples_.Clear();
}

}  // namespace trace_processor
}  // namespace perfetto
