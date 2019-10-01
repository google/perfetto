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

#include "src/trace_processor/heap_graph_tracker.h"

namespace perfetto {
namespace trace_processor {

HeapGraphTracker::HeapGraphTracker(TraceProcessorContext* context)
    : context_(context) {}

void HeapGraphTracker::AddObject(UniquePid upid, int64_t ts, SourceObject obj) {
  if (current_upid_ != 0 && current_upid_ != upid) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return;
  }
  if (current_ts_ != 0 && current_ts_ != ts) {
    context_->storage->IncrementStats(stats::heap_graph_non_finalized_graph);
    return;
  }
  current_upid_ = upid;
  current_ts_ = ts;
  current_objects_.emplace_back(std::move(obj));
}

void HeapGraphTracker::AddInternedTypeName(uint64_t intern_id,
                                           StringPool::Id strid) {
  interned_type_names_.emplace(intern_id, strid);
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
    context_->storage->mutable_heap_graph_object_table()->Insert(
        {current_upid_, current_ts_, static_cast<int64_t>(obj.object_id),
         static_cast<int64_t>(obj.self_size), it->second});
  }
  interned_type_names_.clear();
  current_objects_.clear();
  current_upid_ = 0;
  current_ts_ = 0;
}

}  // namespace trace_processor
}  // namespace perfetto
