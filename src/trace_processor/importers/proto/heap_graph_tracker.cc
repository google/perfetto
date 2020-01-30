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

#include "src/trace_processor/importers/proto/heap_graph_tracker.h"

namespace perfetto {
namespace trace_processor {

size_t NumberOfArrays(base::StringView type) {
  if (type.size() < 2)
    return 0;

  size_t arrays = 0;
  while (type.size() >= 2 * (arrays + 1) &&
         memcmp(type.end() - 2 * (arrays + 1), "[]", 2) == 0) {
    arrays++;
  }

  return arrays;
}

base::StringView NormalizeTypeName(base::StringView type) {
  return base::StringView(type.data(), type.size() - NumberOfArrays(type) * 2);
}

HeapGraphTracker::HeapGraphTracker(TraceProcessorContext* context)
    : context_(context) {}

HeapGraphTracker::SequenceState& HeapGraphTracker::GetOrCreateSequence(
    uint32_t seq_id) {
  auto seq_it = sequence_state_.find(seq_id);
  if (seq_it == sequence_state_.end()) {
    std::tie(seq_it, std::ignore) = sequence_state_.emplace(seq_id, this);
  }
  return seq_it->second;
}

bool HeapGraphTracker::SetPidAndTimestamp(SequenceState* sequence_state,
                                          UniquePid upid,
                                          int64_t ts) {
  if (sequence_state->current_upid != 0 &&
      sequence_state->current_upid != upid) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  if (sequence_state->current_ts != 0 && sequence_state->current_ts != ts) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  sequence_state->current_upid = upid;
  sequence_state->current_ts = ts;
  return true;
}

void HeapGraphTracker::AddObject(uint32_t seq_id,
                                 UniquePid upid,
                                 int64_t ts,
                                 SourceObject obj) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);

  if (!SetPidAndTimestamp(&sequence_state, upid, ts))
    return;

  sequence_state.current_objects.emplace_back(std::move(obj));
}

void HeapGraphTracker::AddRoot(uint32_t seq_id,
                               UniquePid upid,
                               int64_t ts,
                               SourceRoot root) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  if (!SetPidAndTimestamp(&sequence_state, upid, ts))
    return;

  sequence_state.current_roots.emplace_back(std::move(root));
}

void HeapGraphTracker::AddInternedTypeName(uint32_t seq_id,
                                           uint64_t intern_id,
                                           StringPool::Id strid) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  sequence_state.interned_type_names.emplace(intern_id, strid);
}

void HeapGraphTracker::AddInternedFieldName(uint32_t seq_id,
                                            uint64_t intern_id,
                                            StringPool::Id strid) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  sequence_state.interned_field_names.emplace(intern_id, strid);
}

void HeapGraphTracker::SetPacketIndex(uint32_t seq_id, uint64_t index) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  bool dropped_packet = false;
  // perfetto_hprof starts counting at index = 0.
  if (!sequence_state.prev_index && index != 0) {
    dropped_packet = true;
  }

  if (sequence_state.prev_index && *sequence_state.prev_index + 1 != index) {
    dropped_packet = true;
  }

  if (dropped_packet) {
    if (sequence_state.prev_index) {
      PERFETTO_ELOG("Missing packets between %" PRIu64 " and %" PRIu64,
                    *sequence_state.prev_index, index);
    } else {
      PERFETTO_ELOG("Invalid first packet index %" PRIu64 " (!= 0)", index);
    }

    context_->storage->IncrementIndexedStats(
        stats::heap_graph_missing_packet,
        static_cast<int>(sequence_state.current_upid));
  }
  sequence_state.prev_index = index;
}

void HeapGraphTracker::FinalizeProfile(uint32_t seq_id) {
  SequenceState& sequence_state = GetOrCreateSequence(seq_id);
  for (const SourceObject& obj : sequence_state.current_objects) {
    auto it = sequence_state.interned_type_names.find(obj.type_id);
    if (it == sequence_state.interned_type_names.end()) {
      context_->storage->IncrementIndexedStats(
          stats::heap_graph_invalid_string_id,
          static_cast<int>(sequence_state.current_upid));
      continue;
    }
    StringPool::Id type_name = it->second;
    context_->storage->mutable_heap_graph_object_table()->Insert(
        {sequence_state.current_upid, sequence_state.current_ts,
         static_cast<int64_t>(obj.object_id),
         static_cast<int64_t>(obj.self_size), /*retained_size=*/-1,
         /*unique_retained_size=*/-1, /*reference_set_id=*/base::nullopt,
         /*reachable=*/0, /*type_name=*/type_name,
         /*deobfuscated_type_name=*/base::nullopt,
         /*root_type=*/base::nullopt});
    int64_t row = context_->storage->heap_graph_object_table().row_count() - 1;
    sequence_state.object_id_to_row.emplace(obj.object_id, row);
    base::StringView normalized_type =
        NormalizeTypeName(context_->storage->GetString(type_name));
    class_to_rows_[context_->storage->InternString(normalized_type)]
        .emplace_back(row);
    sequence_state.walker.AddNode(row, obj.self_size,
                                  static_cast<int32_t>(type_name.raw_id()));
  }

  for (const SourceObject& obj : sequence_state.current_objects) {
    auto it = sequence_state.object_id_to_row.find(obj.object_id);
    if (it == sequence_state.object_id_to_row.end())
      continue;
    uint32_t owner_row = it->second;

    uint32_t reference_set_id =
        context_->storage->heap_graph_reference_table().row_count();
    std::set<int64_t> seen_owned;
    for (const SourceObject::Reference& ref : obj.references) {
      // This is true for unset reference fields.
      if (ref.owned_object_id == 0)
        continue;

      it = sequence_state.object_id_to_row.find(ref.owned_object_id);
      // This can only happen for an invalid type string id, which is already
      // reported as an error. Silently continue here.
      if (it == sequence_state.object_id_to_row.end())
        continue;

      int64_t owned_row = it->second;
      bool inserted;
      std::tie(std::ignore, inserted) = seen_owned.emplace(owned_row);
      if (inserted)
        sequence_state.walker.AddEdge(owner_row, owned_row);

      auto field_name_it =
          sequence_state.interned_field_names.find(ref.field_name_id);
      if (field_name_it == sequence_state.interned_field_names.end()) {
        context_->storage->IncrementIndexedStats(
            stats::heap_graph_invalid_string_id,
            static_cast<int>(sequence_state.current_upid));
        continue;
      }
      StringPool::Id field_name = field_name_it->second;
      context_->storage->mutable_heap_graph_reference_table()->Insert(
          {reference_set_id, owner_row, owned_row, field_name,
           /*deobfuscated_field_name=*/base::nullopt});
      uint32_t row =
          context_->storage->heap_graph_reference_table().row_count() - 1;
      field_to_rows_[field_name].emplace_back(row);
    }
    context_->storage->mutable_heap_graph_object_table()
        ->mutable_reference_set_id()
        ->Set(owner_row, reference_set_id);
  }

  for (const SourceRoot& root : sequence_state.current_roots) {
    for (uint64_t obj_id : root.object_ids) {
      auto it = sequence_state.object_id_to_row.find(obj_id);
      // This can only happen for an invalid type string id, which is already
      // reported as an error. Silently continue here.
      if (it == sequence_state.object_id_to_row.end())
        continue;

      int64_t obj_row = it->second;
      sequence_state.walker.MarkRoot(obj_row);
      context_->storage->mutable_heap_graph_object_table()
          ->mutable_root_type()
          ->Set(static_cast<uint32_t>(obj_row), root.root_type);
    }
  }

  auto paths = sequence_state.walker.FindPathsFromRoot();
  walkers_.emplace(
      std::make_pair(sequence_state.current_upid, sequence_state.current_ts),
      std::move(sequence_state.walker));

  sequence_state_.erase(seq_id);
}

std::unique_ptr<tables::ExperimentalFlamegraphNodesTable>
HeapGraphTracker::BuildFlamegraph(const int64_t current_ts,
                                  const UniquePid current_upid) {
  auto it = walkers_.find(std::make_pair(current_upid, current_ts));
  if (it == walkers_.end())
    return nullptr;

  std::unique_ptr<tables::ExperimentalFlamegraphNodesTable> tbl(
      new tables::ExperimentalFlamegraphNodesTable(
          context_->storage->mutable_string_pool(), nullptr));

  HeapGraphWalker::PathFromRoot init_path = it->second.FindPathsFromRoot();
  auto profile_type = context_->storage->InternString("graph");
  auto java_mapping = context_->storage->InternString("JAVA");

  std::vector<int32_t> node_to_cumulative_size(init_path.nodes.size());
  std::vector<int32_t> node_to_cumulative_count(init_path.nodes.size());
  // i > 0 is to skip the artifical root node.
  for (size_t i = init_path.nodes.size() - 1; i > 0; --i) {
    const HeapGraphWalker::PathFromRoot::Node& node = init_path.nodes[i];

    node_to_cumulative_size[i] += node.size;
    node_to_cumulative_count[i] += node.count;
    node_to_cumulative_size[node.parent_id] += node_to_cumulative_size[i];
    node_to_cumulative_count[node.parent_id] += node_to_cumulative_count[i];
  }

  std::vector<FlamegraphId> node_to_id(init_path.nodes.size());
  // i = 1 is to skip the artifical root node.
  for (size_t i = 1; i < init_path.nodes.size(); ++i) {
    const HeapGraphWalker::PathFromRoot::Node& node = init_path.nodes[i];
    PERFETTO_CHECK(node.parent_id < i);
    base::Optional<FlamegraphId> parent_id;
    if (node.parent_id != 0)
      parent_id = node_to_id[node.parent_id];
    const uint32_t depth = node.depth;

    tables::ExperimentalFlamegraphNodesTable::Row alloc_row{
        current_ts, current_upid, profile_type, depth,
        StringId::Raw(static_cast<uint32_t>(node.class_name)), java_mapping,
        static_cast<int64_t>(node.count),
        static_cast<int64_t>(node_to_cumulative_count[i]),
        static_cast<int64_t>(node.size),
        static_cast<int64_t>(node_to_cumulative_size[i]),
        // For java dumps, set alloc_count == count, etc.
        static_cast<int64_t>(node.count),
        static_cast<int64_t>(node_to_cumulative_count[i]),
        static_cast<int64_t>(node.size),
        static_cast<int64_t>(node_to_cumulative_size[i]), parent_id};
    node_to_id[i] = tbl->Insert(alloc_row).id;
  }
  return tbl;
}

void HeapGraphTracker::MarkReachable(int64_t row) {
  context_->storage->mutable_heap_graph_object_table()
      ->mutable_reachable()
      ->Set(static_cast<uint32_t>(row), 1);
}

void HeapGraphTracker::SetRetained(int64_t row,
                                   int64_t retained,
                                   int64_t unique_retained) {
  context_->storage->mutable_heap_graph_object_table()
      ->mutable_retained_size()
      ->Set(static_cast<uint32_t>(row), retained);
  context_->storage->mutable_heap_graph_object_table()
      ->mutable_unique_retained_size()
      ->Set(static_cast<uint32_t>(row), unique_retained);
}

}  // namespace trace_processor
}  // namespace perfetto
