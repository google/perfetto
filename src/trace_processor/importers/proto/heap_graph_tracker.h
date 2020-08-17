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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_TRACKER_H_

#include <map>
#include <set>
#include <vector>

#include "perfetto/ext/base/optional.h"
#include "perfetto/ext/base/string_view.h"

#include "protos/perfetto/trace/profiling/heap_graph.pbzero.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto {
namespace trace_processor {

class TraceProcessorContext;

struct NormalizedType {
  base::StringView name;
  bool is_static_class;
  size_t number_of_arrays;
};

struct PathFromRoot {
  static constexpr size_t kRoot = 0;
  struct Node {
    uint32_t depth = 0;
    // Invariant: parent_id < id of this node.
    size_t parent_id = 0;
    int64_t size = 0;
    int64_t count = 0;
    tables::HeapGraphClassTable::Id type_id = {};
    std::map<tables::HeapGraphClassTable::Id, size_t> children;
  };
  std::vector<Node> nodes{Node{}};
  std::set<tables::HeapGraphObjectTable::Id> visited;
};

void MarkRoot(TraceStorage* s,
              tables::HeapGraphObjectTable::Id id,
              StringPool::Id type);
void FindPathFromRoot(const TraceStorage& s,
                      tables::HeapGraphObjectTable::Id id,
                      PathFromRoot* path);

base::Optional<base::StringView> GetStaticClassTypeName(base::StringView type);
size_t NumberOfArrays(base::StringView type);
NormalizedType GetNormalizedType(base::StringView type);
base::StringView NormalizeTypeName(base::StringView type);
std::string DenormalizeTypeName(NormalizedType normalized,
                                base::StringView deobfuscated_type_name);

class HeapGraphTracker : public Destructible {
 public:
  struct SourceObject {
    // All ids in this are in the trace iid space, not in the trace processor
    // id space.
    uint64_t object_id = 0;
    uint64_t self_size = 0;
    uint64_t type_id = 0;

    std::vector<uint64_t> field_name_ids;
    std::vector<uint64_t> referred_objects;
  };

  struct SourceRoot {
    StringPool::Id root_type;
    std::vector<uint64_t> object_ids;
  };

  explicit HeapGraphTracker(TraceProcessorContext* context);

  static HeapGraphTracker* GetOrCreate(TraceProcessorContext* context) {
    if (!context->heap_graph_tracker) {
      context->heap_graph_tracker.reset(new HeapGraphTracker(context));
    }
    return static_cast<HeapGraphTracker*>(context->heap_graph_tracker.get());
  }

  void AddRoot(uint32_t seq_id, UniquePid upid, int64_t ts, SourceRoot root);
  void AddObject(uint32_t seq_id, UniquePid upid, int64_t ts, SourceObject obj);
  void AddInternedType(uint32_t seq_id,
                       uint64_t intern_id,
                       StringPool::Id strid,
                       uint64_t location_id,
                       uint64_t object_size,
                       std::vector<uint64_t> field_name_ids,
                       uint64_t superclass_id,
                       bool no_fields);
  void AddInternedFieldName(uint32_t seq_id,
                            uint64_t intern_id,
                            base::StringView str);
  void AddInternedLocationName(uint32_t seq_id,
                               uint64_t intern_id,
                               StringPool::Id str);
  void FinalizeProfile(uint32_t seq);
  void SetPacketIndex(uint32_t seq_id, uint64_t index);

  ~HeapGraphTracker() override;
  void NotifyEndOfFile();

  const std::vector<tables::HeapGraphClassTable::Id>* RowsForType(
      base::Optional<StringPool::Id> package_name,
      StringPool::Id type_name) const {
    auto it = class_to_rows_.find(std::make_pair(package_name, type_name));
    if (it == class_to_rows_.end())
      return nullptr;
    return &it->second;
  }

  const std::vector<int64_t>* RowsForField(StringPool::Id field_name) const {
    auto it = field_to_rows_.find(field_name);
    if (it == field_to_rows_.end())
      return nullptr;
    return &it->second;
  }

  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> BuildFlamegraph(
      const int64_t current_ts,
      const UniquePid current_upid);

 private:
  struct InternedField {
    StringPool::Id name;
    StringPool::Id type_name;
  };
  struct InternedType {
    StringPool::Id name;
    base::Optional<uint64_t> location_id;
    uint64_t object_size;
    std::vector<uint64_t> field_name_ids;
    uint64_t superclass_id;
    bool no_fields;
  };
  struct SequenceState {
    UniquePid current_upid = 0;
    int64_t current_ts = 0;
    std::vector<SourceRoot> current_roots;
    std::map<uint64_t, InternedType> interned_types;
    std::map<uint64_t, StringPool::Id> interned_location_names;
    std::map<uint64_t, tables::HeapGraphObjectTable::Id> object_id_to_db_id;
    std::map<uint64_t, tables::HeapGraphClassTable::Id> type_id_to_db_id;
    std::map<uint64_t, std::vector<tables::HeapGraphReferenceTable::Id>>
        references_for_field_name_id;
    std::map<uint64_t, InternedField> interned_fields;
    std::map<tables::HeapGraphClassTable::Id,
             std::vector<tables::HeapGraphObjectTable::Id>>
        deferred_reference_objects_for_type_;
    base::Optional<uint64_t> prev_index;
    // For most objects, we need not store the size in the object's message
    // itself, because all instances of the type have the same type. In this
    // case, we defer setting self_size in the table until we process the class
    // message in FinalizeProfile.
    std::map<tables::HeapGraphClassTable::Id,
             std::vector<tables::HeapGraphObjectTable::Id>>
        deferred_size_objects_for_type_;
  };

  SequenceState& GetOrCreateSequence(uint32_t seq_id);
  tables::HeapGraphObjectTable::Id GetOrInsertObject(
      SequenceState* sequence_state,
      uint64_t object_id);
  tables::HeapGraphClassTable::Id GetOrInsertType(SequenceState* sequence_state,
                                                  uint64_t type_id);
  bool SetPidAndTimestamp(SequenceState* seq, UniquePid upid, int64_t ts);
  void PopulateSuperClasses(const SequenceState& seq);
  InternedType* GetSuperClass(SequenceState* sequence_state,
                              const InternedType* current_type);

  TraceProcessorContext* const context_;
  std::map<uint32_t, SequenceState> sequence_state_;

  std::map<std::pair<base::Optional<StringPool::Id>, StringPool::Id>,
           std::vector<tables::HeapGraphClassTable::Id>>
      class_to_rows_;
  std::map<StringPool::Id, std::vector<int64_t>> field_to_rows_;

  std::map<std::pair<base::Optional<StringPool::Id>, StringPool::Id>,
           StringPool::Id>
      deobfuscation_mapping_;
  std::map<std::pair<UniquePid, int64_t>,
           std::set<tables::HeapGraphObjectTable::Id>>
      roots_;
};

}  // namespace trace_processor
}  // namespace perfetto

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_PROTO_HEAP_GRAPH_TRACKER_H_
