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

#include "src/trace_processor/importers/pprof/pprof_trace_reader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/string_view.h"
#include "perfetto/trace_processor/trace_blob_view.h"
#include "protos/third_party/pprof/profile.pbzero.h"
#include "src/trace_processor/importers/common/address_range.h"
#include "src/trace_processor/importers/common/create_mapping_params.h"
#include "src/trace_processor/importers/common/mapping_tracker.h"
#include "src/trace_processor/importers/common/stack_profile_tracker.h"
#include "src/trace_processor/importers/common/virtual_memory_mapping.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/profiler_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/util/build_id.h"

namespace perfetto::third_party::perftools::profiles::pbzero {
using Profile = ::perfetto::third_party::perftools::profiles::pbzero::Profile;
using Sample = ::perfetto::third_party::perftools::profiles::pbzero::Sample;
using Location = ::perfetto::third_party::perftools::profiles::pbzero::Location;
using Function = ::perfetto::third_party::perftools::profiles::pbzero::Function;
using Mapping = ::perfetto::third_party::perftools::profiles::pbzero::Mapping;
using Line = ::perfetto::third_party::perftools::profiles::pbzero::Line;
using ValueType =
    ::perfetto::third_party::perftools::profiles::pbzero::ValueType;
}  // namespace perfetto::third_party::perftools::profiles::pbzero

namespace perfetto::trace_processor {

PprofTraceReader::PprofTraceReader(TraceProcessorContext* context)
    : context_(context) {}

PprofTraceReader::~PprofTraceReader() = default;

base::Status PprofTraceReader::Parse(TraceBlobView blob) {
  buffer_.insert(buffer_.end(), blob.data(), blob.data() + blob.size());
  return base::OkStatus();
}

base::Status PprofTraceReader::NotifyEndOfFile() {
  if (buffer_.empty()) {
    return base::ErrStatus("Empty pprof data");
  }

  return ParseProfile();
}

base::Status PprofTraceReader::ParseProfile() {
  using namespace perfetto::third_party::perftools::profiles::pbzero;

  TraceStorage* storage = context_->storage.get();
  Profile::Decoder profile(buffer_.data(), buffer_.size());

  // Parse string table first
  std::vector<std::string> string_table;
  for (auto it = profile.string_table(); it; ++it) {
    string_table.emplace_back(it->as_string().ToStdString());
  }

  if (string_table.empty()) {
    return base::ErrStatus("Invalid pprof: empty string table");
  }

  // Generate scope name
  std::string scope = "pprof_file";

  // Parse mappings and create VirtualMemoryMapping objects
  std::unordered_map<uint64_t, VirtualMemoryMapping*> mappings;
  for (auto it = profile.mapping(); it; ++it) {
    Mapping::Decoder mapping_decoder(*it);
    if (!mapping_decoder.has_id())
      continue;

    std::string filename =
        mapping_decoder.has_filename() &&
                static_cast<size_t>(mapping_decoder.filename()) <
                    string_table.size()
            ? string_table[static_cast<size_t>(mapping_decoder.filename())]
            : "[unknown]";

    std::string build_id_str =
        mapping_decoder.has_build_id() &&
                static_cast<size_t>(mapping_decoder.build_id()) <
                    string_table.size()
            ? string_table[static_cast<size_t>(mapping_decoder.build_id())]
            : "";

    CreateMappingParams params;
    params.memory_range = AddressRange::FromStartAndSize(
        mapping_decoder.memory_start(),
        mapping_decoder.memory_limit() - mapping_decoder.memory_start());
    params.exact_offset = mapping_decoder.file_offset();
    params.name = filename;
    if (!build_id_str.empty()) {
      params.build_id = BuildId::FromRaw(build_id_str);
    }

    VirtualMemoryMapping& mapping =
        context_->mapping_tracker->InternMemoryMapping(params);
    mappings[mapping_decoder.id()] = &mapping;
  }

  // Parse functions
  std::unordered_map<uint64_t, std::string> functions;
  for (auto it = profile.function(); it; ++it) {
    Function::Decoder func_decoder(*it);
    if (!func_decoder.has_id())
      continue;

    std::string name =
        func_decoder.has_name() &&
                static_cast<size_t>(func_decoder.name()) < string_table.size()
            ? string_table[static_cast<size_t>(func_decoder.name())]
            : "[unknown]";

    functions[func_decoder.id()] = name;
  }

  // Parse locations and create frames
  std::unordered_map<uint64_t, FrameId> location_to_frame;
  for (auto it = profile.location(); it; ++it) {
    Location::Decoder loc_decoder(*it);
    if (!loc_decoder.has_id())
      continue;

    VirtualMemoryMapping* mapping = nullptr;
    if (loc_decoder.has_mapping_id()) {
      auto mapping_it = mappings.find(loc_decoder.mapping_id());
      if (mapping_it != mappings.end()) {
        mapping = mapping_it->second;
      }
    }

    // If no mapping found, create a dummy one
    if (!mapping) {
      mapping = &context_->mapping_tracker->CreateDummyMapping("[unknown]");
    }

    std::string frame_name = "[unknown]";
    // Use the first line's function if available
    for (auto line_it = loc_decoder.line(); line_it; ++line_it) {
      Line::Decoder line_decoder(*line_it);
      if (line_decoder.has_function_id()) {
        auto func_it = functions.find(line_decoder.function_id());
        if (func_it != functions.end()) {
          frame_name = func_it->second;
          break;
        }
      }
    }

    uint64_t rel_pc = 0;
    if (loc_decoder.has_address()) {
      rel_pc = loc_decoder.address();
      if (mapping->memory_range().start() > 0 &&
          rel_pc >= mapping->memory_range().start()) {
        rel_pc -= mapping->memory_range().start();
      }
    }

    FrameId frame_id =
        mapping->InternFrame(rel_pc, base::StringView(frame_name));
    location_to_frame[loc_decoder.id()] = frame_id;
  }

  // Parse sample types and create aggregate_profile entries
  std::vector<tables::AggregateProfileTable::Id> profile_ids;
  for (auto it = profile.sample_type(); it; ++it) {
    ValueType::Decoder sample_type_decoder(*it);

    std::string type_str =
        sample_type_decoder.has_type() &&
                static_cast<size_t>(sample_type_decoder.type()) <
                    string_table.size()
            ? string_table[static_cast<size_t>(sample_type_decoder.type())]
            : "unknown";

    std::string unit_str =
        sample_type_decoder.has_unit() &&
                static_cast<size_t>(sample_type_decoder.unit()) <
                    string_table.size()
            ? string_table[static_cast<size_t>(sample_type_decoder.unit())]
            : "count";

    tables::AggregateProfileTable::Row profile_row;
    profile_row.scope = storage->InternString(scope.c_str());
    profile_row.name = storage->InternString(("pprof " + type_str).c_str());
    profile_row.sample_type_type = storage->InternString(type_str.c_str());
    profile_row.sample_type_unit = storage->InternString(unit_str.c_str());

    auto profile_id =
        storage->mutable_aggregate_profile_table()->Insert(profile_row).id;
    profile_ids.push_back(profile_id);
  }

  // Parse samples and create aggregate_sample entries
  for (auto it = profile.sample(); it; ++it) {
    Sample::Decoder sample_decoder(*it);

    // Materialize location_ids first (pprof format: leaf is at [0])
    std::vector<uint64_t> location_ids;
    bool location_parse_error = false;
    for (auto loc_it = sample_decoder.location_id(&location_parse_error);
         loc_it && !location_parse_error; ++loc_it) {
      location_ids.push_back(*loc_it);
    }

    if (location_ids.empty()) {
      continue;  // Skip samples with no locations
    }

    // Reverse to get root -> leaf order for callsite building
    std::reverse(location_ids.begin(), location_ids.end());

    // Build callsite hierarchy from root to leaf
    std::optional<CallsiteId> callsite_id;
    uint32_t depth = 0;

    for (uint64_t location_id : location_ids) {
      auto frame_it = location_to_frame.find(location_id);
      if (frame_it == location_to_frame.end()) {
        continue;  // Skip unknown locations
      }

      callsite_id = context_->stack_profile_tracker->InternCallsite(
          callsite_id, frame_it->second, depth);
      ++depth;
    }

    if (!callsite_id) {
      continue;  // Skip samples with no valid callsite
    }

    // Create aggregate_sample entries for each value
    size_t value_index = 0;
    for (auto value_it = sample_decoder.value();
         value_it && value_index < profile_ids.size();
         ++value_it, ++value_index) {
      if (*value_it == 0) {
        continue;  // Skip zero values
      }

      tables::AggregateSampleTable::Row sample_row;
      sample_row.aggregate_profile_id = profile_ids[value_index];
      sample_row.callsite_id =
          *callsite_id;  // This is now the leaf after root->leaf building
      sample_row.value = static_cast<double>(*value_it);

      storage->mutable_aggregate_sample_table()->Insert(sample_row);
    }
  }
  return base::OkStatus();
}

}  // namespace perfetto::trace_processor
