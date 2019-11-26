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

bool HeapGraphTracker::SetPidAndTimestamp(UniquePid upid, int64_t ts) {
  if (current_upid_ != 0 && current_upid_ != upid) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  if (current_ts_ != 0 && current_ts_ != ts) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return false;
  }
  current_upid_ = upid;
  current_ts_ = ts;
  return true;
}

void HeapGraphTracker::AddObject(UniquePid upid, int64_t ts, SourceObject obj) {
  if (!SetPidAndTimestamp(upid, ts))
    return;

  current_objects_.emplace_back(std::move(obj));
}

void HeapGraphTracker::AddRoot(UniquePid upid, int64_t ts, SourceRoot root) {
  if (!SetPidAndTimestamp(upid, ts))
    return;

  current_roots_.emplace_back(std::move(root));
}

void HeapGraphTracker::AddInternedTypeName(uint64_t intern_id,
                                           StringPool::Id strid) {
  interned_type_names_.emplace(intern_id, strid);
}

void HeapGraphTracker::AddInternedFieldName(uint64_t intern_id,
                                            StringPool::Id strid) {
  interned_field_names_.emplace(intern_id, strid);
}

void HeapGraphTracker::SetPacketIndex(uint64_t index) {
  if (prev_index_ != 0 && prev_index_ + 1 != index) {
    PERFETTO_ELOG("Missing packets between %" PRIu64 " and %" PRIu64,
                  prev_index_, index);
    context_->storage->IncrementIndexedStats(stats::heap_graph_missing_packet,
                                             static_cast<int>(current_upid_));
  }
  prev_index_ = index;
}

void HeapGraphTracker::FinalizeProfile() {
  for (const SourceObject& obj : current_objects_) {
    auto it = interned_type_names_.find(obj.type_id);
    if (it == interned_type_names_.end()) {
      context_->storage->IncrementIndexedStats(
          stats::heap_graph_invalid_string_id, static_cast<int>(current_upid_));
      continue;
    }
    StringPool::Id type_name = it->second;
    context_->storage->mutable_heap_graph_object_table()->Insert(
        {current_upid_, current_ts_, static_cast<int64_t>(obj.object_id),
         static_cast<int64_t>(obj.self_size), /*retained_size=*/-1,
         /*unique_retained_size=*/-1, /*reference_set_id=*/-1,
         /*reachable=*/0, /*type_name=*/type_name,
         /*deobfuscated_type_name=*/base::nullopt,
         /*root_type=*/base::nullopt});
    int64_t row = context_->storage->heap_graph_object_table().size() - 1;
    object_id_to_row_.emplace(obj.object_id, row);
    class_to_rows_[type_name].emplace_back(row);
    walker_.AddNode(row, obj.self_size);
  }

  for (const SourceObject& obj : current_objects_) {
    auto it = object_id_to_row_.find(obj.object_id);
    if (it == object_id_to_row_.end())
      continue;
    int64_t owner_row = it->second;

    int64_t reference_set_id =
        context_->storage->heap_graph_reference_table().size();
    std::set<int64_t> seen_owned;
    for (const SourceObject::Reference& ref : obj.references) {
      // This is true for unset reference fields.
      if (ref.owned_object_id == 0)
        continue;

      it = object_id_to_row_.find(ref.owned_object_id);
      // This can only happen for an invalid type string id, which is already
      // reported as an error. Silently continue here.
      if (it == object_id_to_row_.end())
        continue;

      int64_t owned_row = it->second;
      bool inserted;
      std::tie(std::ignore, inserted) = seen_owned.emplace(owned_row);
      if (inserted)
        walker_.AddEdge(owner_row, owned_row);

      auto field_name_it = interned_field_names_.find(ref.field_name_id);
      if (field_name_it == interned_field_names_.end()) {
        context_->storage->IncrementIndexedStats(
            stats::heap_graph_invalid_string_id,
            static_cast<int>(current_upid_));
        continue;
      }
      StringPool::Id field_name = field_name_it->second;
      context_->storage->mutable_heap_graph_reference_table()->Insert(
          {reference_set_id, owner_row, owned_row, field_name,
           /*deobfuscated_field_name=*/base::nullopt});
      int64_t row = context_->storage->heap_graph_reference_table().size() - 1;
      field_to_rows_[field_name].emplace_back(row);
    }
    context_->storage->mutable_heap_graph_object_table()
        ->mutable_reference_set_id()
        ->Set(static_cast<uint32_t>(owner_row), reference_set_id);
  }

  for (const SourceRoot& root : current_roots_) {
    for (uint64_t obj_id : root.object_ids) {
      auto it = object_id_to_row_.find(obj_id);
      // This can only happen for an invalid type string id, which is already
      // reported as an error. Silently continue here.
      if (it == object_id_to_row_.end())
        continue;

      int64_t obj_row = it->second;
      walker_.MarkRoot(obj_row);
      context_->storage->mutable_heap_graph_object_table()
          ->mutable_root_type()
          ->Set(static_cast<uint32_t>(obj_row), root.root_type);
    }
  }

  walker_.CalculateRetained();

  // TODO(fmayer): Track these fields per sequence, then delete the
  // current sequence's data here.
  current_upid_ = 0;
  current_ts_ = 0;
  current_objects_.clear();
  current_roots_.clear();
  interned_type_names_.clear();
  interned_field_names_.clear();
  object_id_to_row_.clear();
  prev_index_ = 0;
  walker_ = HeapGraphWalker(this);

  // class_to_rows_ and field_to_rows_ need to outlive this to handle
  // DeobfuscationMapping later.
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
