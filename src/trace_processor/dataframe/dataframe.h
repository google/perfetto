/*
 * Copyright (C) 2025 The Android Open Source Project
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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/cursor.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"

namespace perfetto::trace_processor::dataframe {

// Dataframe is a columnar data structure for efficient querying and filtering
// of tabular data. It provides:
//
// - Type-specialized storage and filtering optimized for common trace data
//   patterns
// - Efficient query execution with optimized bytecode generation
// - Support for serializable query plans that separate planning from execution
// - Memory-efficient storage with support for specialized column types
class Dataframe {
 public:
  // Defines the properties of a column in the dataframe.
  struct ColumnSpec {
    std::string name;
    StorageType type;
    Nullability nullability;
    SortState sort_state;
  };

  // QueryPlan encapsulates an executable, serializable representation of a
  // dataframe query operation. It contains the bytecode instructions and
  // metadata needed to execute a query.
  class QueryPlan {
   public:
    // Default constructor for an empty query plan.
    QueryPlan() = default;

    // Serializes the query plan to a string.
    std::string Serialize() const { return plan_.Serialize(); }

    // Deserializes a query plan from a string previously produced by
    // `Serialize()`.
    static QueryPlan Deserialize(std::string_view serialized) {
      return QueryPlan(impl::QueryPlan::Deserialize(serialized));
    }

    // Returns the underlying implementation for testing purposes.
    const impl::QueryPlan& GetImplForTesting() const { return plan_; }

   private:
    friend class Dataframe;
    // Constructs a QueryPlan from its implementation.
    explicit QueryPlan(impl::QueryPlan plan) : plan_(std::move(plan)) {}
    // The underlying query plan implementation.
    impl::QueryPlan plan_;
  };

  // Non-copyable
  Dataframe(const Dataframe&) = delete;
  Dataframe& operator=(const Dataframe&) = delete;

  // Movable
  Dataframe(Dataframe&&) = default;
  Dataframe& operator=(Dataframe&&) = default;

  // Creates an execution plan for querying the dataframe with specified filters
  // and column selection.
  //
  // Parameters:
  //   specs:            Filter predicates to apply to the data.
  //   cols_used_bitmap: Bitmap where each bit corresponds to a column that may
  //                     be requested. Only columns with set bits can be
  //                     fetched.
  // Returns:
  //   A StatusOr containing the QueryPlan or an error status.
  base::StatusOr<QueryPlan> PlanQuery(std::vector<FilterSpec>& specs,
                                      uint64_t cols_used_bitmap);

  // Prepares a cursor for executing the query plan. The template parameter
  // `FilterValueFetcherImpl` is a subclass of `ValueFetcher` that defines the
  // logic for fetching filter values for each filter specs specified when
  // calling `PlanQuery`.
  //
  // Parameters:
  //   plan: The query plan to execute.
  //   c:    A reference to a std::optional that will be set to the prepared
  //         cursor.
  template <typename FilterValueFetcherImpl>
  void PrepareCursor(QueryPlan plan,
                     std::optional<Cursor<FilterValueFetcherImpl>>& c) {
    c.emplace(std::move(plan.plan_), columns_.data(), string_pool_);
  }

  // Creates a vector of ColumnSpec objects that describe the columns in the
  // dataframe.
  std::vector<ColumnSpec> CreateColumnSpecs() const {
    std::vector<ColumnSpec> specs;
    specs.reserve(columns_.size());
    for (const auto& col : columns_) {
      specs.push_back({col.name, col.storage.type(), col.overlay.nullability(),
                       col.sort_state});
    }
    return specs;
  }

 private:
  friend class RuntimeDataframeBuilder;

  // TODO(lalitm): remove this once we have a proper static builder for
  // dataframe.
  friend class DataframeBytecodeTest;

  Dataframe(std::vector<impl::Column> columns,
            uint32_t row_count,
            StringPool* string_pool)
      : columns_(std::move(columns)),
        row_count_(row_count),
        string_pool_(string_pool) {}

  // Internal storage for columns in the dataframe.
  std::vector<impl::Column> columns_;

  // Number of rows in the dataframe.
  uint32_t row_count_ = 0;

  // String pool for efficient string storage and interning.
  StringPool* string_pool_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
