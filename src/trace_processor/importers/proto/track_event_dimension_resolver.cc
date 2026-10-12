/*
 * Copyright (C) 2026 The Android Open Source Project
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

#include "src/trace_processor/importers/proto/track_event_dimension_resolver.h"

#include <cstdint>
#include <optional>
#include <utility>

#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

using Dimension = TrackEventDimensionResolver::Dimension;
using DimensionVec = TrackEventDimensionResolver::DimensionVec;

bool HasSameValue(const Dimension& a, const Dimension& b) {
  return a.int_value == b.int_value && a.string_value == b.string_value;
}

// Adds |dim| to |out| unless the name is already present:
//  * the same value is simply deduplicated,
//  * a different value is invalid producer input, in which case the value
//    which is already there (i.e. the more general/inherited one) wins.
void MergeDimension(TraceProcessorContext* context,
                    const Dimension& dim,
                    DimensionVec& out) {
  for (const auto& existing : out) {
    if (existing.name != dim.name) {
      continue;
    }
    if (!HasSameValue(existing, dim)) {
      context->global_stats_tracker->IncrementGlobalStats(
          stats::track_dimension_conflicting_value);
    }
    return;
  }
  out.push_back(dim);
}

void MergeDimensions(TraceProcessorContext* context,
                     const DimensionVec& dims,
                     DimensionVec& out) {
  for (const auto& dim : dims) {
    MergeDimension(context, dim, out);
  }
}

void Append(const DimensionVec& dims, DimensionVec& out) {
  out.insert(out.end(), dims.begin(), dims.end());
}

}  // namespace

TrackEventDimensionResolver::TrackEventDimensionResolver(
    TraceProcessorContext* context)
    : context_(context), storage_(context->storage.get()) {}

TrackEventDimensionResolver::~TrackEventDimensionResolver() = default;

void TrackEventDimensionResolver::DeclareForProcess(UniquePid upid,
                                                    const DimensionVec& dims) {
  Append(dims, by_upid_[upid]);
}

void TrackEventDimensionResolver::DeclareForThread(UniqueTid utid,
                                                   const DimensionVec& dims) {
  Append(dims, by_utid_[utid]);
}

void TrackEventDimensionResolver::DeclareForTrack(TrackId track_id,
                                                  const DimensionVec& dims) {
  Append(dims, by_track_[track_id.value]);
}

void TrackEventDimensionResolver::ResolveAll() {
  // The overwhelmingly common case: no producer declared any dimension.
  if (by_upid_.size() == 0 && by_utid_.size() == 0 && by_track_.size() == 0) {
    return;
  }
  auto* out = storage_->mutable_track_dimension_table();
  for (auto it = storage_->track_table().IterateRows(); it; ++it) {
    // |dims| stays valid as nothing is inserted into |resolved_| in the loop.
    const DimensionVec& dims = Resolve(it.id());
    for (const auto& dim : dims) {
      tables::TrackDimensionTable::Row row;
      row.track_id = it.id();
      row.name = dim.name;
      row.int_value = dim.int_value;
      row.string_value = dim.string_value;
      row.display_name = dim.display_name;
      out->Insert(row);
    }
  }
}

// Returns the effective dimensions of |id|, computing (and memoizing) them if
// necessary.
//
// The returned reference is only valid until the next call to |Resolve|, as
// that can insert into |resolved_|.
const DimensionVec& TrackEventDimensionResolver::Resolve(TrackId id) {
  // Inserting an empty placeholder first also guards against pathological
  // (and, in a well formed trace, impossible) cycles in the parent chain.
  auto [placeholder, inserted] = resolved_.Insert(id.value, DimensionVec());
  if (!inserted) {
    return *placeholder;
  }

  DimensionVec dims;
  auto track = storage_->track_table()[id];

  // 1. Dimensions of the process this track belongs to. Thread tracks reach
  //    their process through the thread table.
  std::optional<uint32_t> upid = track.upid();
  std::optional<uint32_t> utid = track.utid();
  if (!upid && utid) {
    upid = storage_->thread_table()[tables::ThreadTable::Id(*utid)].upid();
  }
  if (upid) {
    if (const DimensionVec* d = by_upid_.Find(*upid); d) {
      MergeDimensions(context_, *d, dims);
    }
  }
  // 2. Dimensions of the thread this track belongs to.
  if (utid) {
    if (const DimensionVec* d = by_utid_.Find(*utid); d) {
      MergeDimensions(context_, *d, dims);
    }
  }
  // 3. Dimensions inherited from ancestor tracks. The reference returned by
  //    |Resolve| is consumed before anything else is inserted.
  if (auto parent = track.parent_id(); parent) {
    MergeDimensions(context_, Resolve(*parent), dims);
  }
  // 4. Dimensions declared on this very track.
  if (const DimensionVec* d = by_track_.Find(id.value); d) {
    MergeDimensions(context_, *d, dims);
  }

  // |placeholder| may have been invalidated by the recursion above.
  DimensionVec* slot = resolved_.Find(id.value);
  *slot = std::move(dims);
  return *slot;
}

}  // namespace perfetto::trace_processor
