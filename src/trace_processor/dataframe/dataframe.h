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
#include <memory>
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

  // Represents an index to speed up operations on the dataframe.
  struct Index {
   public:
    Index Copy() const { return *this; }

   private:
    friend class Dataframe;

    Index(std::vector<uint32_t> _columns,
          std::shared_ptr<std::vector<uint32_t>> _permutation_vector)
        : columns(std::move(_columns)),
          permutation_vector(std::move(_permutation_vector)) {}

    std::vector<uint32_t> columns;
    std::shared_ptr<std::vector<uint32_t>> permutation_vector;
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

    // The maximum number of rows it's possible for this query plan to return.
    uint32_t max_row_count() const { return plan_.params.max_row_count; }

    // The number of rows this query plan estimates it will return.
    uint32_t estimated_row_count() const {
      return plan_.params.estimated_row_count;
    }

    // An estimate for the cost of executing the query plan.
    double estimated_cost() const { return plan_.params.estimated_cost; }

   private:
    friend class Dataframe;
    // Constructs a QueryPlan from its implementation.
    explicit QueryPlan(impl::QueryPlan plan) : plan_(std::move(plan)) {}
    // The underlying query plan implementation.
    impl::QueryPlan plan_;
  };

  // Movable
  Dataframe(Dataframe&&) = default;
  Dataframe& operator=(Dataframe&&) = default;

  // Creates an execution plan for querying the dataframe with specified filters
  // and column selection.
  //
  // Parameters:
  //   filter_specs:     Filter predicates to apply to the data.
  //   distinct_specs:   Distinct specifications to remove duplicate rows.
  //   sort_specs:       Sort specifications defining the desired row order.
  //   limit_spec:       Optional struct specifying LIMIT and OFFSET values.
  //   cols_used_bitmap: Bitmap where each bit corresponds to a column that may
  //                     be requested. Only columns with set bits can be
  //                     fetched.
  // Returns:
  //   A StatusOr containing the QueryPlan or an error status.
  base::StatusOr<QueryPlan> PlanQuery(
      std::vector<FilterSpec>& filter_specs,
      const std::vector<DistinctSpec>& distinct_specs,
      const std::vector<SortSpec>& sort_specs,
      const LimitSpec& limit_spec,
      uint64_t cols_used_bitmap) const;

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
                     std::optional<Cursor<FilterValueFetcherImpl>>& c) const {
    c.emplace(std::move(plan.plan_), column_ptrs_.data(),
              column_storage_data_ptrs_.data(), string_pool_);
  }

  // Makes an index which can speed up operations on this table. Note that
  // this function does *not* actually cause the index to be added or used, it
  // just returns it. Use `AddIndex` to add the index to the dataframe.
  //
  // Note that this index can be added to any dataframe with the same contents
  // (i.e. copies of this dataframe) not just the one it was created from.
  base::StatusOr<Index> BuildIndex(const uint32_t* columns_start,
                                   const uint32_t* columns_end) const;

  // Adds an index to the dataframe.
  void AddIndex(Index index);

  // Removes the index at the specified position.
  void RemoveIndexAt(uint32_t);

  // Makes a copy of the dataframe.
  //
  // This is a shallow copy, meaning that the contents of columns and indexes
  // are not duplicated, but the dataframe itself is a new instance.
  dataframe::Dataframe Copy() const;

  // Creates a vector of ColumnSpec objects that describe the columns in the
  // dataframe.
  std::vector<ColumnSpec> CreateColumnSpecs() const {
    std::vector<ColumnSpec> specs;
    specs.reserve(columns_.size());
    for (uint32_t i = 0; i < columns_.size(); ++i) {
      const auto& col = columns_[i];
      specs.push_back({column_names_[i], col->storage.type(),
                       col->null_storage.nullability(), col->sort_state});
    }
    return specs;
  }

  // Returns the column names of the dataframe.
  const std::vector<std::string>& column_names() const { return column_names_; }

 private:
  friend class RuntimeDataframeBuilder;

  // TODO(lalitm): remove this once we have a proper static builder for
  // dataframe.
  friend class DataframeBytecodeTest;

  Dataframe(std::vector<std::string> column_names,
            std::vector<std::shared_ptr<impl::Column>> columns,
            uint32_t row_count,
            StringPool* string_pool)
      : column_names_(std::move(column_names)),
        columns_(std::move(columns)),
        row_count_(row_count),
        string_pool_(string_pool) {
    column_ptrs_.reserve(columns_.size());
    column_storage_data_ptrs_.reserve(columns_.size());
    for (const auto& col : columns_) {
      column_ptrs_.emplace_back(col.get());
      column_storage_data_ptrs_.push_back(col->storage.data());
    }
  }

  // Private copy constructor for special methods.
  Dataframe(const Dataframe&) = default;
  Dataframe& operator=(const Dataframe&) = default;

  // The names of all columns.
  std::vector<std::string> column_names_;

  // Internal storage for columns in the dataframe.
  // Should have same size as `column_names_`.
  std::vector<std::shared_ptr<impl::Column>> columns_;

  // Simple pointers to the columns for consumption by the cursor.
  // Should have same size as `column_names_`.
  std::vector<impl::Column*> column_ptrs_;

  // Variant of pointers to the storage data for consumption by the cursor.
  // Should have same size as `column_names_`.
  std::vector<impl::Storage::DataPointer> column_storage_data_ptrs_;

  // List of indexes associated with the dataframe.
  std::vector<Index> indexes_;

  // Number of rows in the dataframe.
  uint32_t row_count_ = 0;

  // String pool for efficient string storage and interning.
  StringPool* string_pool_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
