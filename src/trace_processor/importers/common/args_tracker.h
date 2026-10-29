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

#ifndef SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRACKER_H_
#define SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRACKER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/murmur_hash.h"
#include "perfetto/ext/base/small_vector.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/storage/trace_storage.h"
#include "src/trace_processor/tables/metadata_tables_fwd.h"
#include "src/trace_processor/types/trace_processor_context.h"
#include "src/trace_processor/types/variadic.h"

namespace perfetto::trace_processor {

class ArgsInserter;

// Owns the args of a trace: interns each arg set into the args table, and hands
// out ArgsInserters, each bound to the arg_set_id cell of one table row.
class ArgsTracker {
 public:
  // How to behave if two or more args with the same key were added into the
  // same ArgSet. If |kSkipIfExists|, the arg will be ignored if another arg
  // with the same key already exists. If |kAddOrUpdate|, any existing arg with
  // the same key will be overridden.
  enum class UpdatePolicy { kSkipIfExists, kAddOrUpdate };

  struct CompactArg {
    StringId flat_key = kNullStringId;
    StringId key = kNullStringId;
    Variadic value = Variadic::Integer(0);
    UpdatePolicy update_policy = UpdatePolicy::kAddOrUpdate;
  };
  static_assert(std::is_trivially_destructible<CompactArg>::value,
                "Args must be trivially destructible");

  using CompactArgSet = base::SmallVector<CompactArg, 16>;

  explicit ArgsTracker(TraceStorage* storage);

  // Returns an inserter for the arg_set_id column of the row |id| of a table
  // in TraceStorage.
  template <typename Id>
  ArgsInserter AddArgsTo(Id id);

  // Same as above, for a table not owned by TraceStorage (e.g. a plugin's).
  template <typename T>
  ArgsInserter AddArgsTo(T* table, typename T::Id id);

  // Tables whose args live in a column other than arg_set_id, or whose rows
  // are addressed by a raw index.
  ArgsInserter AddArgsTo(MetadataId id);
  ArgsInserter AddArgsTo(TrackId id);
  ArgsInserter AddArgsToProcess(UniquePid id);
  ArgsInserter AddArgsToThread(UniqueTid id);

  // Interns the arg set formed by |args| and returns its id. Keys in |args|
  // must be unique.
  ArgSetId AddArgSet(const CompactArg* args, uint32_t count);

 private:
  friend class ArgsInserter;

  using ArgSetHash = uint64_t;

  // Reusable accumulation buffer for one ArgsInserter, recycled via the pool
  // below so short-lived inserters don't churn allocations.
  struct ArgsBuffer {
    std::vector<CompactArg> args;
    // key -> index in `args`; empty below a threshold, where a linear scan over
    // `args` is cheaper than a hash lookup.
    base::FlatHashMap<StringId, uint32_t> key_index;
    base::FlatHashMap<StringId, size_t> array_indexes;

    void Reset() {
      args.clear();
      key_index.Clear();
      array_indexes.Clear();
    }
  };

  ArgsInserter Bind(dataframe::Dataframe* df,
                    uint32_t col,
                    uint32_t row,
                    uint32_t id);

  // Buffers keep stable addresses while checked out (unique_ptr storage).
  ArgsBuffer* AcquireArgsBuffer() {
    if (!free_buffers_.empty()) {
      ArgsBuffer* buffer = free_buffers_.back();
      free_buffers_.pop_back();
      return buffer;
    }
    buffer_storage_.push_back(std::make_unique<ArgsBuffer>());
    return buffer_storage_.back().get();
  }

  void ReleaseArgsBuffer(ArgsBuffer* buffer) {
    buffer->Reset();
    free_buffers_.push_back(buffer);
  }

  base::FlatHashMap<ArgSetHash, uint32_t, base::AlreadyHashed<ArgSetHash>>
      arg_row_for_hash_;

  // Pool of ArgsInserter buffers: `buffer_storage_` owns them, `free_buffers_`
  // lists those available for reuse.
  std::vector<std::unique_ptr<ArgsBuffer>> buffer_storage_;
  std::vector<ArgsBuffer*> free_buffers_;

  TraceStorage* storage_;
};

// Accumulates the args for a single target cell (the dataframe column+row that
// holds an arg_set_id) and commits them as one arg set when it goes out of
// scope. Obtain one from ArgsTracker::AddArgsTo*; callers needing several cells
// acquire one inserter per cell. Same-key duplicates are collapsed in place
// (kSkipIfExists keeps the first value, kAddOrUpdate the last).
//
// Move-only; a moved-from inserter owns nothing and commits nothing. Its buffer
// is borrowed from a pool in ArgsTracker to avoid per-inserter allocation.
class ArgsInserter {
 public:
  using UpdatePolicy = ArgsTracker::UpdatePolicy;
  using CompactArg = ArgsTracker::CompactArg;
  using CompactArgSet = ArgsTracker::CompactArgSet;

  // Constructs an empty inserter that owns no buffer and commits nothing. Used
  // for the moved-from state and for default-constructed (empty) map entries.
  ArgsInserter() = default;

  ~ArgsInserter();

  ArgsInserter(ArgsInserter&&) noexcept;
  ArgsInserter& operator=(ArgsInserter&&) noexcept;

  ArgsInserter(const ArgsInserter&) = delete;
  ArgsInserter& operator=(const ArgsInserter&) = delete;

  // Adds an arg with the same key and flat_key.
  ArgsInserter& AddArg(
      StringId key,
      Variadic v,
      UpdatePolicy update_policy = UpdatePolicy::kAddOrUpdate) {
    return AddArg(key, key, v, update_policy);
  }

  ArgsInserter& AddArg(StringId flat_key,
                       StringId key,
                       Variadic v,
                       UpdatePolicy update_policy = UpdatePolicy::kAddOrUpdate);

  // IncrementArrayEntryIndex() and GetNextArrayEntryIndex() provide a way to
  // track the next array index for an array under a specific key.
  size_t GetNextArrayEntryIndex(StringId key) {
    // Zero-initializes |key| in the map if it doesn't exist yet.
    return buffer_->array_indexes[key];
  }

  // Returns the next available array index after increment.
  size_t IncrementArrayEntryIndex(StringId key) {
    // Zero-initializes |key| in the map if it doesn't exist yet.
    return ++buffer_->array_indexes[key];
  }

  // The id of the row these args are being added to (the value of the typed
  // Id passed to ArgsTracker::AddArgsTo).
  uint32_t id() const { return id_; }

  // The args accumulated so far, with same-key duplicates already collapsed.
  const std::vector<CompactArg>& args() const {
    PERFETTO_DCHECK(buffer_);
    return buffer_->args;
  }

  // Moves the accumulated args out into a CompactArgSet, leaving this inserter
  // empty so it commits nothing when destroyed. Used by callers that must
  // post-process the args (e.g. translation) before they reach storage.
  CompactArgSet ToCompactArgSet() &&;

 private:
  friend class ArgsTracker;

  ArgsInserter(ArgsTracker* tracker,
               dataframe::Dataframe* dataframe,
               uint32_t col,
               uint32_t row,
               uint32_t id);

  // Commits the accumulated args (if any) as a single arg set and writes the
  // resulting arg_set_id into the target cell.
  void Commit();

  // Non-null iff this inserter owns a pooled buffer; both are nulled on move.
  ArgsTracker* tracker_ = nullptr;
  ArgsTracker::ArgsBuffer* buffer_ = nullptr;
  dataframe::Dataframe* df_ = nullptr;
  uint32_t col_ = 0;
  uint32_t row_ = 0;
  uint32_t id_ = 0;
};

template <typename Id>
ArgsInserter ArgsTracker::AddArgsTo(Id id) {
  using Table = typename Id::Table;
  return AddArgsTo(storage_->mutable_table<Table>(), id);
}

template <typename T>
ArgsInserter ArgsTracker::AddArgsTo(T* table, typename T::Id id) {
  uint32_t row = (*table)[id].ToRowNumber().row_number();
  return Bind(&table->dataframe(), T::ColumnIndex::arg_set_id, row, id.value);
}

inline ArgsInserter ArgsTracker::Bind(dataframe::Dataframe* df,
                                      uint32_t col,
                                      uint32_t row,
                                      uint32_t id) {
  return ArgsInserter(this, df, col, row, id);
}

}  // namespace perfetto::trace_processor

#endif  // SRC_TRACE_PROCESSOR_IMPORTERS_COMMON_ARGS_TRACKER_H_
