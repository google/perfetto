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

#include "src/trace_processor/core/tree/tree_from_dataframe.h"

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
#include "src/trace_processor/core/dataframe/adhoc_dataframe_builder.h"
#include "src/trace_processor/core/tree/tree.h"
#include "src/trace_processor/core/util/flex_vector.h"
#include "src/trace_processor/core/util/span.h"
#include "src/trace_processor/core/util/slab.h"

namespace perfetto::trace_processor::core {

namespace {

template <typename T>
Slab<uint8_t> CopyColumn(Span<const T> values) {
  auto bytes = static_cast<uint64_t>(values.size()) * sizeof(T);
  auto slab = Slab<uint8_t>::Alloc(bytes);
  memcpy(slab.begin(), values.b, bytes);
  return slab;
}

Tree::Column ConvertRawColumn(
    dataframe::AdhocDataframeBuilder::RawColumn& rc,
    uint32_t row_count) {
  Tree::Column tc;
  if (!rc.storage) {
    // All-null column: default to Int64 with zero data.
    tc = Tree::Column::Create<int64_t>(row_count);
    memset(tc.data.begin(), 0,
           static_cast<uint64_t>(row_count) * sizeof(int64_t));
  } else if (rc.storage->type().Is<Int64>()) {
    tc.type = Tree::Column::Type(Int64{});
    tc.data = CopyColumn(rc.storage->unchecked_get<Int64>().span());
  } else if (rc.storage->type().Is<Double>()) {
    tc.type = Tree::Column::Type(Double{});
    tc.data = CopyColumn(rc.storage->unchecked_get<Double>().span());
  } else if (rc.storage->type().Is<String>()) {
    tc.type = Tree::Column::Type(String{});
    tc.data = CopyColumn(rc.storage->unchecked_get<String>().span());
  } else {
    PERFETTO_FATAL("Unexpected storage type in raw column");
  }
  tc.null_bv = std::move(rc.null_bv);
  return tc;
}

uint32_t FindComponent(uint32_t row, Slab<uint32_t>& component) {
  while (component[row] != row) {
    component[row] = component[component[row]];
    row = component[row];
  }
  return row;
}

struct IdIndex {
  std::optional<uint32_t> Find(int64_t id) const {
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
      const uint32_t row = dense_index[static_cast<uint32_t>(id)];
      return row == Tree::kNullParent ? std::nullopt : std::make_optional(row);
    }
    const uint32_t* row = hash_index->Find(id);
    return row ? std::make_optional(*row) : std::nullopt;
  }

  bool identity_ids;
  bool dense_ids;
  uint32_t row_count;
  Span<const uint32_t> dense_index;
  const base::FlatHashMap<int64_t, uint32_t>* hash_index;
};

base::StatusOr<Tree> BuildFromRawColumns(
    std::vector<dataframe::AdhocDataframeBuilder::RawColumn> raw_cols);

}  // namespace

base::StatusOr<Tree> BuildTree(dataframe::AdhocDataframeBuilder&& builder) {
  ASSIGN_OR_RETURN(auto raw_cols, std::move(builder).BuildRaw());
  return BuildFromRawColumns(std::move(raw_cols));
}

namespace {

base::StatusOr<Tree> BuildFromRawColumns(
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
    if (id_vec_ptr->size() >= Tree::kNullParent) {
      return base::ErrStatus("tree: too many rows");
    }
    row_count = static_cast<uint32_t>(id_vec_ptr->size());
  }
  FlexVector<int64_t> empty_ids;
  const auto& id_vec = id_vec_ptr ? *id_vec_ptr : empty_ids;

  Tree result;
  result.row_count = row_count;

  // Prefer indexes which avoid hashing. Identity ids need no storage; a
  // reasonably dense uint32 range uses direct indexing; arbitrary int64 ids
  // fall back to a hash map.
  bool identity_ids = true;
  bool uint32_ids = true;
  uint32_t max_id = 0;
  Slab<uint32_t> component = Slab<uint32_t>::Alloc(row_count);
  for (uint32_t i = 0; i < row_count; ++i) {
    component[i] = i;
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
    dense_index.resize(uint64_t(max_id) + 1, Tree::kNullParent);
    for (uint32_t i = 0; i < row_count; ++i) {
      uint32_t id = static_cast<uint32_t>(id_vec[i]);
      if (dense_index[id] != Tree::kNullParent) {
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
  const IdIndex id_index{identity_ids, dense_ids, row_count,
                         MakeSpan(dense_index), &hash_index};

  // Normalize parent_id to row indices and union each node with its parent.
  // Because every node has at most one parent, an edge whose endpoints are
  // already connected is exactly a directed cycle.
  auto& pid_rc = raw_cols[1];
  result.parent = Slab<uint32_t>::Alloc(row_count);
  if (!pid_rc.storage) {
    for (uint32_t row = 0; row < row_count; ++row) {
      result.parent[row] = Tree::kNullParent;
    }
  } else {
    if (!pid_rc.storage->type().Is<Int64>()) {
      return base::ErrStatus("tree: parent_id column must be integer");
    }
    const auto& pid_vec = pid_rc.storage->unchecked_get<Int64>();
    for (uint32_t row = 0; row < row_count; ++row) {
      if (pid_rc.null_bv.size() > 0 && !pid_rc.null_bv.is_set(row)) {
        result.parent[row] = Tree::kNullParent;
        continue;
      }
      std::optional<uint32_t> parent = id_index.Find(pid_vec[row]);
      if (PERFETTO_UNLIKELY(!parent)) {
        return base::ErrStatus("tree: parent_id not found in id column");
      }
      result.parent[row] = *parent;
      uint32_t row_component = FindComponent(row, component);
      uint32_t parent_component = FindComponent(*parent, component);
      if (PERFETTO_UNLIKELY(row_component == parent_component)) {
        return base::ErrStatus("tree: cycle detected");
      }
      // Prefer the component rooted at the later row. This keeps the next row
      // close to the root for both parent-first and child-first inputs.
      if (row_component < parent_component) {
        component[row_component] = parent_component;
      } else {
        component[parent_component] = row_component;
      }
    }
  }

  result.names.reserve(raw_cols.size());
  result.columns.reserve(raw_cols.size());
  for (auto& rc : raw_cols) {
    result.names.push_back(std::move(rc.name));
    result.columns.push_back(ConvertRawColumn(rc, row_count));
  }
  return std::move(result);
}

}  // namespace

}  // namespace perfetto::trace_processor::core
