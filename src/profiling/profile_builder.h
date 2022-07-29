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

#ifndef SRC_PROFILING_PROFILE_BUILDER_H_
#define SRC_PROFILING_PROFILE_BUILDER_H_

#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/scattered_heap_buffer.h"
#include "perfetto/trace_processor/trace_processor.h"
#include "protos/third_party/pprof/profile.pbzero.h"
#include "src/trace_processor/containers/string_pool.h"

#include <cstdint>
#include <set>
#include <unordered_map>

namespace perfetto {
namespace profiling {

// Builds the |perftools.profiles.Profile| proto.
class GProfileBuilder {
 public:
  GProfileBuilder(trace_processor::TraceProcessor* tp, bool annotate_frames);
  ~GProfileBuilder();
  void WriteSampleTypes(
      const std::vector<std::pair<std::string, std::string>>& sample_types);
  bool AddSample(const protozero::PackedVarInt& values, int64_t callstack_id);
  std::string CompleteProfile();
  void Reset();

 private:
  class LocationTracker;

  // Extracts and interns the unique frames and locations (as defined by the
  // proto format) from the callstack SQL tables.
  //
  // Approach:
  //   * for each callstack (callsite ids of the leaves):
  //     * use experimental_annotated_callstack to build the full list of
  //       constituent frames
  //     * for each frame (root to leaf):
  //         * intern the location and function(s)
  //         * remember the mapping from callsite_id to the callstack so far
  //         (from
  //            the root and including the frame being considered)
  //
  // Optionally mixes in the annotations as a frame name suffix (since there's
  // no good way to attach extra info to locations in the proto format). This
  // relies on the annotations (produced by experimental_annotated_callstack) to
  // be stable for a given callsite (equivalently: dependent only on their
  // parents).
  static std::unique_ptr<LocationTracker> PreprocessLocations(
      trace_processor::TraceProcessor* tp,
      trace_processor::StringPool* interner,
      bool annotate_frames);

  // Serializes the Profile.Location entries referenced by this profile.
  bool WriteLocations(std::set<int64_t>* seen_mappings,
                      std::set<int64_t>* seen_functions);

  // Serializes the Profile.Function entries referenced by this profile.
  bool WriteFunctions(const std::set<int64_t>& seen_functions);

  // Serializes the Profile.Mapping entries referenced by this profile.
  bool WriteMappings(const std::set<int64_t>& seen_mappings);

  void WriteStringTable();

  int64_t ToStringTableId(trace_processor::StringPool::Id interned_id);

  trace_processor::TraceProcessor& trace_processor_;

  // String interner, strings referenced by LocationTracker are already
  // interned. The new internings will come from mappings, and sample types.
  trace_processor::StringPool interner_;

  // Contains all locations, lines, functions (in memory):
  std::unique_ptr<LocationTracker> locations_;

  // The profile format uses the repeated string_table field's index as an
  // implicit id, so these structures remap the interned strings into sequential
  // ids. Only the strings referenced by this GProfileBuilder instance will be
  // added to the table.
  std::unordered_map<trace_processor::StringPool::Id, int64_t>
      interning_remapper_;
  std::vector<trace_processor::StringPool::Id> string_table_;

  // Profile proto being serialized.
  protozero::HeapBuffered<third_party::perftools::profiles::pbzero::Profile>
      result_;

  // Set of locations referenced by the added samples.
  std::set<int64_t> seen_locations_;
};

}  // namespace profiling
}  // namespace perfetto

#endif  // SRC_PROFILING_PROFILE_BUILDER_H_
