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
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/cursor.h"
#include "src/trace_processor/dataframe/impl/query_plan.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"

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

  // Creates a dataframe with the specified column specifications.
  //
  // StringPool is passed here to allow for efficient storage and lookup
  // of string column values.
  Dataframe(const std::vector<ColumnSpec>&, StringPool* string_pool);

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
  //   c:    A pointer to the storage for the cursor. The cursor will be
  //         constructed in-place at the location pointed to by `c`.
  template <typename FilterValueFetcherImpl>
  void PrepareCursor(QueryPlan plan, Cursor<FilterValueFetcherImpl>* c) {
    new (c) Cursor<FilterValueFetcherImpl>(std::move(plan.plan_),
                                           columns_.data(), string_pool_);
  }

  // Inserts a row into the dataframe.
  //
  // TODO(lalitm): this is a temporary function which exists for testing
  // purposes only. We should remove this function once we have a proper builder
  // class for dataframe construction.
  template <typename ValueFetcherImpl>
  void InsertRow() {
    static_assert(std::is_base_of_v<ValueFetcher, ValueFetcherImpl>,
                  "ValueFetcherImpl must inherit from ValueFetcher");
    for (auto& column : columns_) {
      switch (column.spec.column_type.index()) {
        case ColumnType::GetTypeIndex<Id>():
          column.storage.unchecked_get<Id>().size++;
          break;
        case ColumnType::GetTypeIndex<Uint32>():
          column.storage.unchecked_get<Uint32>().push_back(0);
          break;
        case ColumnType::GetTypeIndex<Int32>():
          column.storage.unchecked_get<Int32>().push_back(0);
          break;
        case ColumnType::GetTypeIndex<Int64>():
          column.storage.unchecked_get<Int64>().push_back(0);
          break;
        case ColumnType::GetTypeIndex<Double>():
          column.storage.unchecked_get<Double>().push_back(0);
          break;
        case ColumnType::GetTypeIndex<String>():
          column.storage.unchecked_get<String>().push_back(
              string_pool_->InternString(""));
          break;
        default:
          PERFETTO_FATAL("Invalid column type");
      }
    }
    row_count_++;
  }

 private:
  // Internal storage for columns in the dataframe.
  std::vector<impl::Column> columns_;

  // Number of rows in the dataframe.
  uint32_t row_count_ = 0;

  // String pool for efficient string storage and interning.
  StringPool* string_pool_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
