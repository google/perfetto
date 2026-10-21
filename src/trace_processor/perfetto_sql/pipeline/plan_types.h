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

#ifndef SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_
#define SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "src/trace_processor/core/common/schema.h"
#include "src/trace_processor/core/common/storage_types.h"

namespace perfetto::trace_processor::pipeline {

using core::ColumnSchema;
using core::Schema;

// Stable within a plan. Lowering assigns physical batch positions separately.
using ColumnId = uint32_t;

// The type of a plan column. It is a storage type, Null or unknown. A Null
// column has no values and converts to any type.
// TODO(lalitm): make Null a core::StorageType, so the executor can carry a
// column of no values without a buffer behind it.
class PlanType {
 public:
  static PlanType Unknown() { return PlanType(); }
  static PlanType Null() {
    PlanType type;
    type.null_ = true;
    return type;
  }

  template <typename T,
            typename = std::enable_if_t<
                std::is_constructible_v<core::StorageType, const T&>>>
  PlanType(const T& type) : storage_(core::StorageType(type)) {}

  bool is_unknown() const { return !null_ && !storage_; }
  bool is_null() const { return null_; }
  const std::optional<core::StorageType>& storage() const { return storage_; }

  // Whether the column is known to hold values of type T.
  template <typename T>
  bool Is() const {
    return storage_ && storage_->Is<T>();
  }

 private:
  PlanType() = default;

  std::optional<core::StorageType> storage_;
  bool null_ = false;
};

// Stable within a plan: a name's index in the plan's NameTable.
using NameId = uint32_t;

// Every name a plan uses, each spelling stored once. Names match ignoring
// case, as SQL's do: spellings which differ only in case have different ids,
// so each keeps its spelling, but match.
class NameTable {
 public:
  NameId Intern(std::string_view text);
  const std::string& text(NameId id) const { return texts_[id]; }
  bool Same(NameId a, NameId b) const { return keys_[a] == keys_[b]; }
  uint32_t size() const { return static_cast<uint32_t>(texts_.size()); }

 private:
  std::vector<std::string> texts_;
  // By name, the id of the first spelling interned which it matches.
  std::vector<NameId> keys_;
};

// A column the plan defines.
struct PlanColumn {
  NameId name;
  PlanType type;
};

// A name in a scope or result. Multiple names can refer to the same value.
struct NamedColumn {
  NameId name;
  ColumnId id;
};

// Stable within a plan.
using PlanNodeId = uint32_t;

// Which way a tree is walked: from the leaves up, or from the roots down.
enum class TreeDirection : uint8_t { kUp, kDown };

// A sort key: the first key in a list decides, each later one breaks ties.
struct SortKey {
  ColumnId column = 0;
  bool descending = false;
};

// The columns available to a stage, in the order positions number them.
using Available = std::vector<ColumnId>;

}  // namespace perfetto::trace_processor::pipeline

#endif  // SRC_TRACE_PROCESSOR_PERFETTO_SQL_PIPELINE_PLAN_TYPES_H_
