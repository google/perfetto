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

#include "src/trace_processor/core/tree/tree_columns_from_dataframe.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/status_macros.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/tree_types.h"
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/tree/tree_columns.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core::tree {

namespace {

// Copies a FlexVector's data into a Slab<uint8_t>.
template <typename T>
Slab<uint8_t> FlexVectorToSlab(const FlexVector<T>& vec) {
  auto byte_count = static_cast<uint64_t>(vec.size()) * sizeof(T);
  auto slab = Slab<uint8_t>::Alloc(byte_count);
  memcpy(slab.begin(), vec.data(), byte_count);
  return slab;
}

// Converts a raw column (Storage + BitVector) into a TreeColumns::Column.
// The Storage contains raw FlexVector<int64_t|double|StringPool::Id>;
// no type downcasting has been performed.
TreeColumns::Column ConvertRawColumn(
    dataframe::AdhocDataframeBuilder::RawColumn& rc,
    uint32_t row_count) {
  TreeColumns::Column tc;
  if (!rc.storage) {
    // All-null column: default to Int64 with zero data.
    tc.type = StorageType(Int64{});
    tc.elem_size = sizeof(int64_t);
    tc.data = Slab<uint8_t>::Alloc(static_cast<uint64_t>(row_count) *
                                   sizeof(int64_t));
    memset(tc.data.begin(), 0,
           static_cast<uint64_t>(row_count) * sizeof(int64_t));
  } else if (rc.storage->type().Is<Int64>()) {
    tc.type = StorageType(Int64{});
    tc.elem_size = sizeof(int64_t);
    tc.data = FlexVectorToSlab(rc.storage->unchecked_get<Int64>());
  } else if (rc.storage->type().Is<Double>()) {
    tc.type = StorageType(Double{});
    tc.elem_size = sizeof(double);
    tc.data = FlexVectorToSlab(rc.storage->unchecked_get<Double>());
  } else if (rc.storage->type().Is<String>()) {
    tc.type = StorageType(String{});
    tc.elem_size = sizeof(StringPool::Id);
    tc.data = FlexVectorToSlab(rc.storage->unchecked_get<String>());
  } else {
    PERFETTO_FATAL("Unexpected storage type in raw column");
  }
  tc.null_bv = std::move(rc.null_bv);
  return tc;
}

base::StatusOr<TreeColumns> BuildFromRawColumns(
    std::vector<dataframe::AdhocDataframeBuilder::RawColumn> raw_cols);

}  // namespace

base::StatusOr<TreeColumns> BuildTreeColumns(
    dataframe::AdhocDataframeBuilder&& builder) {
  ASSIGN_OR_RETURN(auto raw_cols, std::move(builder).BuildRaw());
  return BuildFromRawColumns(std::move(raw_cols));
}

namespace {

base::StatusOr<TreeColumns> BuildFromRawColumns(
    std::vector<dataframe::AdhocDataframeBuilder::RawColumn> raw_cols) {
  // Columns 0 and 1 are id and parent_id.
  if (raw_cols.size() < 2) {
    return base::ErrStatus("tree: need at least id and parent_id columns");
  }

  // Extract id values from column 0. A column without storage and without a
  // null bitmap means the builder has no rows; preserve the column names so
  // consumers can still expose the schema of an empty tree.
  auto& id_rc = raw_cols[0];
  uint32_t row_count = 0;
  const FlexVector<int64_t>* id_vec_ptr = nullptr;
  if (!id_rc.storage) {
    if (id_rc.null_bv.size() != 0) {
      return base::ErrStatus("tree: id column must be integer");
    }
  } else {
    if (!id_rc.storage->type().Is<Int64>()) {
      return base::ErrStatus("tree: id column must be integer");
    }
    id_vec_ptr = &id_rc.storage->unchecked_get<Int64>();
    if (id_vec_ptr->size() >= kNullParent) {
      return base::ErrStatus("tree: too many rows");
    }
    row_count = static_cast<uint32_t>(id_vec_ptr->size());
  }
  FlexVector<int64_t> empty_ids;
  const auto& id_vec = id_vec_ptr ? *id_vec_ptr : empty_ids;

  TreeColumns result;
  result.row_count = row_count;

  // Prefer indexes which avoid hashing. Identity ids need no storage; a
  // reasonably dense uint32 range uses direct indexing; arbitrary int64 ids
  // fall back to a hash map.
  bool identity_ids = true;
  bool uint32_ids = true;
  uint32_t max_id = 0;
  for (uint32_t i = 0; i < row_count; ++i) {
    int64_t id = id_vec[i];
    identity_ids = identity_ids && id == i;
    if (id < 0 ||
        static_cast<uint64_t>(id) > std::numeric_limits<uint32_t>::max()) {
      uint32_ids = false;
    } else {
      max_id = std::max(max_id, static_cast<uint32_t>(id));
    }
  }
  bool dense_ids = !identity_ids && uint32_ids &&
                   uint64_t(max_id) + 1 <= uint64_t(row_count) * 2;
  std::vector<uint32_t> dense_index;
  base::FlatHashMap<int64_t, uint32_t> hash_index;
  if (dense_ids) {
    dense_index.resize(uint64_t(max_id) + 1, kNullParent);
    for (uint32_t i = 0; i < row_count; ++i) {
      uint32_t id = static_cast<uint32_t>(id_vec[i]);
      if (dense_index[id] != kNullParent) {
        return base::ErrStatus("tree: duplicate id");
      }
      dense_index[id] = i;
    }
  } else if (!identity_ids) {
    for (uint32_t i = 0; i < row_count; ++i) {
      auto [row, inserted] = hash_index.Insert(id_vec[i], i);
      base::ignore_result(row);
      if (!inserted) {
        return base::ErrStatus("tree: duplicate id");
      }
    }
  }
  auto find_id = [&](int64_t id) -> std::optional<uint32_t> {
    if (identity_ids) {
      if (id < 0 || static_cast<uint64_t>(id) >= row_count) {
        return std::nullopt;
      }
      return static_cast<uint32_t>(id);
    }
    if (dense_ids) {
      if (id < 0 || static_cast<uint64_t>(id) >= dense_index.size()) {
        return std::nullopt;
      }
      uint32_t row = dense_index[static_cast<uint32_t>(id)];
      return row == kNullParent ? std::nullopt : std::make_optional(row);
    }
    const uint32_t* row = hash_index.Find(id);
    return row ? std::make_optional(*row) : std::nullopt;
  };

  // Normalize parent_id (column 1) to row indices.
  auto& pid_rc = raw_cols[1];
  result.parent = Slab<uint32_t>::Alloc(row_count);
  if (!pid_rc.storage) {
    // All-null parent_id: every node is a root.
    for (uint32_t i = 0; i < row_count; ++i) {
      result.parent[i] = kNullParent;
    }
  } else if (!pid_rc.storage->type().Is<Int64>()) {
    return base::ErrStatus("tree: parent_id column must be integer");
  } else {
    const auto& pid_vec = pid_rc.storage->unchecked_get<Int64>();
    for (uint32_t i = 0; i < row_count; ++i) {
      if (pid_rc.null_bv.size() > 0 && !pid_rc.null_bv.is_set(i)) {
        result.parent[i] = kNullParent;
      } else {
        std::optional<uint32_t> row = find_id(pid_vec[i]);
        if (PERFETTO_UNLIKELY(!row)) {
          return base::ErrStatus("tree: parent_id not found in id column");
        }
        result.parent[i] = *row;
      }
    }
  }

  // Convert all columns (including id/parent_id) to TreeColumns format.
  result.names.reserve(raw_cols.size());
  result.columns.reserve(raw_cols.size());
  for (auto& rc : raw_cols) {
    result.names.push_back(std::move(rc.name));
    result.columns.push_back(ConvertRawColumn(rc, row_count));
  }
  return std::move(result);
}

}  // namespace

}  // namespace perfetto::trace_processor::core::tree
