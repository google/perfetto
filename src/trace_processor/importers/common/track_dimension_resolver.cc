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

#include "src/trace_processor/importers/common/track_dimension_resolver.h"

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/trace_processor/importers/common/global_stats_tracker.h"
#include "src/trace_processor/storage/stats.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_py.h"
#include "src/trace_processor/tables/track_tables_py.h"
#include "src/trace_processor/types/trace_processor_context.h"

namespace perfetto::trace_processor {
namespace {

// An effective dimension of a track.
struct EffectiveDimension {
  StringId name;
  std::optional<int64_t> int_value;
  std::optional<StringId> string_value;
  std::optional<StringId> display_name;

  bool HasSameValue(const EffectiveDimension& o) const {
    return int_value == o.int_value && string_value == o.string_value;
  }
};

using DimensionVec = std::vector<EffectiveDimension>;

// Adds |dim| to |out| unless the name is already present:
//  * the same value is simply deduplicated,
//  * a different value is invalid producer input, in which case the value
//    which is already there (i.e. the more general/inherited one) wins.
void MergeDimension(TraceProcessorContext* context,
                    const EffectiveDimension& dim,
                    DimensionVec& out) {
  for (const auto& existing : out) {
    if (existing.name != dim.name) {
      continue;
    }
    if (!existing.HasSameValue(dim)) {
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

class Resolver {
 public:
  explicit Resolver(TraceProcessorContext* context)
      : context_(context), storage_(context->storage.get()) {}

  void Run() {
    // The overwhelmingly common case: no producer declared any dimension.
    if (storage_->track_dimension_decl_table().row_count() == 0) {
      return;
    }
    ReadDeclarations();
    WriteOutput();
  }

 private:
  // Groups the raw declarations by what they are anchored to.
  void ReadDeclarations() {
    for (auto it = storage_->track_dimension_decl_table().IterateRows(); it;
         ++it) {
      EffectiveDimension dim{it.name(), it.int_value(), it.string_value(),
                             it.display_name()};
      if (auto upid = it.upid(); upid) {
        by_upid_[*upid].push_back(dim);
      } else if (auto utid = it.utid(); utid) {
        by_utid_[*utid].push_back(dim);
      } else if (auto track = it.declaring_track_id(); track) {
        by_track_[track->value].push_back(dim);
      }
    }
  }

  // Returns the effective dimensions of |id|, computing (and memoizing) them
  // if necessary.
  const DimensionVec& Resolve(TrackId id) {
    if (auto it = resolved_.find(id.value); it != resolved_.end()) {
      return it->second;
    }
    // Guard against pathological (and, in a well formed trace, impossible)
    // cycles in the parent chain.
    auto [placeholder, inserted] = resolved_.emplace(id.value, DimensionVec());
    if (!inserted) {
      return placeholder->second;
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
      if (auto d = by_upid_.find(*upid); d != by_upid_.end()) {
        MergeDimensions(context_, d->second, dims);
      }
    }
    // 2. Dimensions of the thread this track belongs to.
    if (utid) {
      if (auto d = by_utid_.find(*utid); d != by_utid_.end()) {
        MergeDimensions(context_, d->second, dims);
      }
    }
    // 3. Dimensions inherited from ancestor tracks.
    if (auto parent = track.parent_id(); parent) {
      MergeDimensions(context_, Resolve(*parent), dims);
    }
    // 4. Dimensions declared on this very track.
    if (auto d = by_track_.find(id.value); d != by_track_.end()) {
      MergeDimensions(context_, d->second, dims);
    }

    auto it = resolved_.find(id.value);
    it->second = std::move(dims);
    return it->second;
  }

  void WriteOutput() {
    auto* out = storage_->mutable_track_dimension_table();
    for (auto it = storage_->track_table().IterateRows(); it; ++it) {
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

  TraceProcessorContext* const context_;
  TraceStorage* const storage_;

  std::unordered_map<uint32_t, DimensionVec> by_upid_;
  std::unordered_map<uint32_t, DimensionVec> by_utid_;
  std::unordered_map<uint32_t, DimensionVec> by_track_;
  std::unordered_map<uint32_t, DimensionVec> resolved_;
};

}  // namespace

void ResolveTrackDimensions(TraceProcessorContext* context) {
  Resolver(context).Run();
}

}  // namespace perfetto::trace_processor
