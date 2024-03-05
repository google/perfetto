/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/stack_profile_sequence_state.h"

#include <optional>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/string_view.h"
#include "protos/perfetto/trace/interned_data/interned_data.pbzero.h"
#include "protos/perfetto/trace/profiling/profile_common.pbzero.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/proto/packet_sequence_state.h"
#include "src/trace_processor/importers/proto/packet_sequence_state_generation.h"
#include "src/trace_processor/importers/proto/profile_packet_utils.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/build_id.h"

namespace perfetto {
namespace trace_processor {
namespace {
base::StringView ToStringView(protozero::ConstBytes bytes) {
  return base::StringView(reinterpret_cast<const char*>(bytes.data),
                          bytes.size);
}
}  // namespace

StackProfileSequenceState::StackProfileSequenceState(
    TraceProcessorContext* context)
    : context_(context) {}

StackProfileSequenceState::~StackProfileSequenceState() = default;

std::optional<MappingId> StackProfileSequenceState::FindOrInsertMapping(
    uint64_t iid) {
  if (VirtualMemoryMapping* mapping = FindOrInsertMappingImpl(iid); mapping) {
    return mapping->mapping_id();
  }
  return std::nullopt;
}

VirtualMemoryMapping* StackProfileSequenceState::FindOrInsertMappingImpl(
    uint64_t iid) {
  if (auto ptr = cached_mappings_.Find(iid); ptr) {
    return *ptr;
  }
  auto* decoder =
      LookupInternedMessage<protos::pbzero::InternedData::kMappingsFieldNumber,
                            protos::pbzero::Mapping>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_mapping_id);
    return nullptr;
  }

  std::vector<base::StringView> path_components;
  for (auto it = decoder->path_string_ids(); it; ++it) {
    std::optional<base::StringView> str = LookupInternedMappingPath(*it);
    if (!str) {
      // For backward compatibility reasons we do not return an error but
      // instead stop adding path components.
      break;
    }
    path_components.push_back(*str);
  }

  CreateMappingParams params;
  std::optional<base::StringView> build_id =
      LookupInternedBuildId(decoder->build_id());
  if (!build_id) {
    return nullptr;
  }
  params.build_id = BuildId::FromRaw(*build_id);

  params.memory_range = AddressRange(decoder->start(), decoder->end());
  params.exact_offset = decoder->exact_offset();
  params.start_offset = decoder->start_offset();
  params.load_bias = decoder->load_bias();
  params.name = ProfilePacketUtils::MakeMappingName(path_components);

  VirtualMemoryMapping& mapping =
      context_->mapping_tracker->InternMemoryMapping(std::move(params));

  cached_mappings_.Insert(iid, &mapping);
  return &mapping;
}

std::optional<base::StringView>
StackProfileSequenceState::LookupInternedBuildId(uint64_t iid) {
  // This should really be an error (value not set) or at the very least return
  // a null string, but for backward compatibility use an empty string instead.
  if (iid == 0) {
    return "";
  }
  auto* decoder =
      LookupInternedMessage<protos::pbzero::InternedData::kBuildIdsFieldNumber,
                            protos::pbzero::InternedString>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    return std::nullopt;
  }

  return ToStringView(decoder->str());
}

std::optional<base::StringView>
StackProfileSequenceState::LookupInternedMappingPath(uint64_t iid) {
  auto* decoder = LookupInternedMessage<
      protos::pbzero::InternedData::kMappingPathsFieldNumber,
      protos::pbzero::InternedString>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    return std::nullopt;
  }

  return ToStringView(decoder->str());
}

std::optional<CallsiteId> StackProfileSequenceState::FindOrInsertCallstack(
    uint64_t iid) {
  if (CallsiteId* id = cached_callstacks_.Find(iid); id) {
    return *id;
  }
  auto* decoder = LookupInternedMessage<
      protos::pbzero::InternedData::kCallstacksFieldNumber,
      protos::pbzero::Callstack>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_callstack_id);
    return std::nullopt;
  }

  std::optional<CallsiteId> parent_callsite_id;
  uint32_t depth = 0;
  for (auto it = decoder->frame_ids(); it; ++it) {
    std::optional<FrameId> frame_id = FindOrInsertFrame(*it);
    if (!frame_id) {
      return std::nullopt;
    }
    parent_callsite_id = context_->stack_profile_tracker->InternCallsite(
        parent_callsite_id, *frame_id, depth);
    ++depth;
  }

  if (!parent_callsite_id) {
    context_->storage->IncrementStats(stats::stackprofile_empty_callstack);
    return std::nullopt;
  }

  cached_callstacks_.Insert(iid, *parent_callsite_id);

  return parent_callsite_id;
}

std::optional<FrameId> StackProfileSequenceState::FindOrInsertFrame(
    uint64_t iid) {
  if (FrameId* id = cached_frames_.Find(iid); id) {
    return *id;
  }
  auto* decoder =
      LookupInternedMessage<protos::pbzero::InternedData::kFramesFieldNumber,
                            protos::pbzero::Frame>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_frame_id);
    return std::nullopt;
  }

  VirtualMemoryMapping* mapping =
      FindOrInsertMappingImpl(decoder->mapping_id());
  if (!mapping) {
    return std::nullopt;
  }

  base::StringView function_name;
  if (decoder->function_name_id() != 0) {
    std::optional<base::StringView> func =
        LookupInternedFunctionName(decoder->function_name_id());
    if (!func) {
      return std::nullopt;
    }
    function_name = *func;
  }

  FrameId frame_id = mapping->InternFrame(decoder->rel_pc(), function_name);
  cached_frames_.Insert(iid, frame_id);

  return frame_id;
}

std::optional<base::StringView>
StackProfileSequenceState::LookupInternedFunctionName(uint64_t iid) {
  // This should really be an error (value not set) or at the very least return
  // a null string, but for backward compatibility use an empty string instead.
  if (iid == 0) {
    return "";
  }
  auto* decoder = LookupInternedMessage<
      protos::pbzero::InternedData::kFunctionNamesFieldNumber,
      protos::pbzero::InternedString>(iid);
  if (!decoder) {
    context_->storage->IncrementStats(stats::stackprofile_invalid_string_id);
    return std::nullopt;
  }

  return ToStringView(decoder->str());
}

}  // namespace trace_processor
}  // namespace perfetto
