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
  if (sequence_state.prev_index != 0 &&
      sequence_state.prev_index + 1 != index) {
    PERFETTO_ELOG("Missing packets between %" PRIu64 " and %" PRIu64,
                  sequence_state.prev_index, index);
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
         /*unique_retained_size=*/-1, /*reference_set_id=*/-1,
         /*reachable=*/0, /*type_name=*/type_name,
         /*deobfuscated_type_name=*/base::nullopt,
         /*root_type=*/base::nullopt});
    int64_t row = context_->storage->heap_graph_object_table().row_count() - 1;
    sequence_state.object_id_to_row.emplace(obj.object_id, row);
    class_to_rows_[type_name].emplace_back(row);
    sequence_state.walker.AddNode(row, obj.self_size,
                                  static_cast<int32_t>(type_name.id));
  }

  for (const SourceObject& obj : sequence_state.current_objects) {
    auto it = sequence_state.object_id_to_row.find(obj.object_id);
    if (it == sequence_state.object_id_to_row.end())
      continue;
    int64_t owner_row = it->second;

    int64_t reference_set_id =
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
      int64_t row =
          context_->storage->heap_graph_reference_table().row_count() - 1;
      field_to_rows_[field_name].emplace_back(row);
    }
    context_->storage->mutable_heap_graph_object_table()
        ->mutable_reference_set_id()
        ->Set(static_cast<uint32_t>(owner_row), reference_set_id);
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

  auto* mapping_table =
      context_->storage->mutable_stack_profile_mapping_table();

  tables::StackProfileMappingTable::Row mapping_row{};
  mapping_row.name = context_->storage->InternString("JAVA");
  MappingId mapping_id = mapping_table->Insert(mapping_row);

  uint32_t mapping_idx = *mapping_table->id().IndexOf(mapping_id);

  auto paths = sequence_state.walker.FindPathsFromRoot();
  for (const auto& p : paths.children)
    WriteFlamegraph(sequence_state, p.second, -1, 0, mapping_idx);

  sequence_state_.erase(seq_id);
}

void HeapGraphTracker::WriteFlamegraph(
    const SequenceState& sequence_state,
    const HeapGraphWalker::PathFromRoot& path,
    int32_t parent_id,
    uint32_t depth,
    uint32_t mapping_row) {
  TraceStorage::StackProfileFrames::Row row{};
  row.name_id = StringId(static_cast<uint32_t>(path.class_name));
  row.mapping_row = mapping_row;
  int32_t frame_id = static_cast<int32_t>(
      context_->storage->mutable_stack_profile_frames()->Insert(row));

  auto* callsites = context_->storage->mutable_stack_profile_callsite_table();
  auto callsite_id = callsites->Insert({depth, parent_id, frame_id});
  parent_id = static_cast<int32_t>(callsite_id.value);
  depth++;

  tables::HeapProfileAllocationTable::Row alloc_row{
      sequence_state.current_ts, sequence_state.current_upid, parent_id,
      static_cast<int64_t>(path.count), static_cast<int64_t>(path.size)};
  // TODO(fmayer): Maybe add a separate table for heap graph flamegraphs.
  context_->storage->mutable_heap_profile_allocation_table()->Insert(alloc_row);
  for (const auto& p : path.children) {
    const HeapGraphWalker::PathFromRoot& child = p.second;
    WriteFlamegraph(sequence_state, child, parent_id, depth, mapping_row);
  }
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
