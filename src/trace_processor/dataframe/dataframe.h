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

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
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
    StorageType type;
    Nullability nullability;
    SortState sort_state;
  };
  // Defines the properties of the dataframe.
  struct Spec {
    std::vector<std::string> column_names;
    std::vector<ColumnSpec> column_specs;
  };

  // Same as ColumnSpec but for cases where the spec is known at compile time.
  template <typename T, typename N, typename S>
  struct TypedColumnSpec {
   public:
    using type = T;
    using null_storage_type = N;
    using sort_state = S;
    ColumnSpec spec;

    // Inferred properties from the above.
    using variant = std::variant<std::monostate,
                                 uint32_t,
                                 int32_t,
                                 int64_t,
                                 double,
                                 StringPool::Id>;
    using non_null_data_type = StorageType::VariantTypeAtIndex<T, variant>;
    using data_type = std::conditional_t<std::is_same_v<N, NonNull>,
                                         non_null_data_type,
                                         std::optional<non_null_data_type>>;
  };
  // Same as Spec but for cases where the spec is known at compile time.
  template <typename... C>
  struct TypedSpec {
    static constexpr uint32_t kColumnCount = sizeof...(C);
    using columns = std::tuple<C...>;
    using data_types = std::tuple<typename C::data_type...>;

    static_assert(kColumnCount > 0,
                  "TypedSpec must have at least one column type");

    std::array<const char*, kColumnCount> column_names;
    std::array<ColumnSpec, kColumnCount> column_specs;
  };
  template <typename... C>
  static constexpr TypedSpec<C...> CreateTypedSpec(
      std::array<const char*, sizeof...(C)> _column_names,
      C... _columns) {
    return TypedSpec<C...>{_column_names, {_columns.spec...}};
  }
  template <typename T, typename N, typename S>
  static constexpr TypedColumnSpec<T, N, S> CreateTypedColumnSpec(T, N, S) {
    return TypedColumnSpec<T, N, S>{ColumnSpec{T{}, N{}, S{}}};
  }

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

  // Constructs a Dataframe with the specified column names and types.
  Dataframe(StringPool* string_pool,
            uint32_t column_count,
            const char* const* column_names,
            const ColumnSpec* column_specs);

  // Creates a dataframe from a typed spec object.
  //
  // The spec specifies the column names and types of the dataframe.
  template <typename S>
  static Dataframe CreateFromTypedSpec(const S& spec, StringPool* pool) {
    static_assert(S::kColumnCount > 0,
                  "Dataframe must have at least one column type");
    return Dataframe(pool, S::kColumnCount, spec.column_names.data(),
                     spec.column_specs.data());
  }

  // Movable
  Dataframe(Dataframe&&) = default;
  Dataframe& operator=(Dataframe&&) = default;

  // Adds a new row to the dataframe with the specified values.
  //
  // Note: This function does not check the types of the values against the
  // column types. It is the caller's responsibility to ensure that the types
  // match. If the types do not match, the behavior is undefined.
  //
  // Generally, this function is only safe to call if the dataframe was
  // constructed using the public Dataframe constructor and not in other ways.
  //
  // Note: this function cannot be called on a finalized dataframe.
  //       See `MarkFinalized()` for more details.
  template <typename D, typename... Args>
  PERFETTO_ALWAYS_INLINE void InsertUnchecked(const D&, Args... ts) {
    static_assert(
        std::is_convertible_v<std::tuple<Args...>, typename D::data_types>,
        "Insert types do not match the column types");
    PERFETTO_DCHECK(!finalized_);
    InsertUncheckedInternal<D>(std::make_index_sequence<sizeof...(Args)>(),
                               ts...);
  }

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
    c.emplace(std::move(plan.plan_), uint32_t(column_ptrs_.size()),
              column_ptrs_.data(), string_pool_);
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
  //
  // Note: indexes can only be added to a finalized dataframe; it's
  // undefined behavior to call this on a non-finalized dataframe.
  void AddIndex(Index index);

  // Removes the index at the specified position.
  //
  // Note: indexes can only be removed from a finalized dataframe;it's
  // undefined behavior to call this on a non-finalized dataframe.
  void RemoveIndexAt(uint32_t);

  // Marks the dataframe as "finalized": a finalized dataframe cannot have any
  // more rows added to it (note this is different from being immutable as
  // indexes can be freely added and removed).
  //
  // If the dataframe is already finalized, this function does nothing.
  void MarkFinalized() { finalized_ = true; }

  // Makes a copy of the dataframe.
  //
  // This is a shallow copy, meaning that the contents of columns and indexes
  // are not duplicated, but the dataframe itself is a new instance.
  dataframe::Dataframe Copy() const;

  // Creates a spec object for this dataframe.
  Spec CreateSpec() const;

  // Returns the column names of the dataframe.
  const std::vector<std::string>& column_names() const { return column_names_; }

 private:
  friend class RuntimeDataframeBuilder;

  // TODO(lalitm): remove this once we have a proper static builder for
  // dataframe.
  friend class DataframeBytecodeTest;

  Dataframe(bool finalized,
            std::vector<std::string> column_names,
            std::vector<std::shared_ptr<impl::Column>> columns,
            uint32_t row_count,
            StringPool* string_pool);

  template <typename D, typename... Args, size_t... Is>
  PERFETTO_ALWAYS_INLINE void InsertUncheckedInternal(
      std::index_sequence<Is...>,
      Args... ts) {
    PERFETTO_DCHECK(column_ptrs_.size() == sizeof...(ts));
    (InsertUncheckedColumn<
         typename std::tuple_element_t<Is, typename D::columns>, Is>(ts),
     ...);
    ++row_count_;
    ++row_mutations_;
  }

  template <typename D, size_t I>
  PERFETTO_ALWAYS_INLINE void InsertUncheckedColumn(
      typename D::non_null_data_type t) {
    static_assert(std::is_same_v<typename D::null_storage_type, NonNull>);
    using type = typename D::type;
    if constexpr (std::is_same_v<type, Id>) {
      base::ignore_result(t);
      columns_[I]->storage.unchecked_get<type>().size++;
    } else {
      columns_[I]->storage.unchecked_get<type>().push_back(t);
    }
  }

  template <typename D, size_t I>
  PERFETTO_ALWAYS_INLINE void InsertUncheckedColumn(
      std::optional<typename D::non_null_data_type> t) {
    using type = typename D::type;
    using null_storage_type = typename D::null_storage_type;
    static_assert(std::is_same_v<null_storage_type, DenseNull> ||
                  std::is_same_v<null_storage_type, SparseNull>);
    auto& null_storage =
        columns_[I]->null_storage.unchecked_get<null_storage_type>();
    auto& storage = columns_[I]->storage;
    if (t.has_value()) {
      null_storage.bit_vector.push_back(true);
      if constexpr (std::is_same_v<type, Id>) {
        storage.unchecked_get<type>().size++;
      } else {
        storage.unchecked_get<type>().push_back(*t);
      }
    } else {
      null_storage.bit_vector.push_back(false);
      if constexpr (std::is_same_v<null_storage_type,
                                   impl::NullStorage::DenseNull>) {
        if constexpr (std::is_same_v<type, Id>) {
          storage.unchecked_get<type>().size++;
        } else {
          storage.unchecked_get<type>().push_back({});
        }
      }
    }
  }

  static std::vector<std::shared_ptr<impl::Column>> CreateColumnVector(
      const ColumnSpec*,
      uint32_t);

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

  // List of indexes associated with the dataframe.
  std::vector<Index> indexes_;

  // Number of rows in the dataframe.
  uint32_t row_count_ = 0;

  // String pool for efficient string storage and interning.
  StringPool* string_pool_;

  // A count of the number of mutations to rows in the dataframe. This includes
  // both row insertions and updates to existing rows. This will be used to
  // determine if a non-finalized dataframe is "dirty" and needs to be
  // re-evaluated when a query is executed.
  uint32_t row_mutations_ = 0;

  // Whether the dataframe is "finalized". See `MarkFinalized()`.
  bool finalized_ = false;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
