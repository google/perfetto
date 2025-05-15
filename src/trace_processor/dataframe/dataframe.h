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
#include <limits>
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
#include "src/trace_processor/dataframe/types.h"
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

  // Base class for TypeCursor. See below for details.
  struct TypedCursorBase {
    using FilterValue =
        std::variant<int64_t, double, const char*, std::nullptr_t>;
    struct Fetcher : ValueFetcher {
      using Type = size_t;
      static const Type kInt64 = base::variant_index<FilterValue, int64_t>();
      static const Type kDouble = base::variant_index<FilterValue, double>();
      static const Type kString =
          base::variant_index<FilterValue, const char*>();
      static const Type kNull = base::variant_index<FilterValue, nullptr_t>();
      int64_t GetInt64Value(uint32_t col) const {
        return base::unchecked_get<int64_t>(filter_values_[col]);
      }
      double GetDoubleValue(uint32_t col) const {
        return base::unchecked_get<double>(filter_values_[col]);
      }
      const char* GetStringValue(uint32_t col) const {
        return base::unchecked_get<const char*>(filter_values_[col]);
      }
      Type GetValueType(uint32_t col) const {
        return filter_values_[col].index();
      }
      FilterValue* filter_values_;
    };

    PERFETTO_ALWAYS_INLINE void ExecuteUncheckedInternal() {
      if (last_execution_mutation_count_ != dataframe_->mutations_) {
        PrepareCursorInternal();
      }
      Fetcher fetcher{{}, filter_values_.data()};
      cursor_.Execute(fetcher);
    }

    PERFETTO_NO_INLINE void PrepareCursorInternal();

    const Dataframe* dataframe_;
    std::vector<FilterValue> filter_values_;
    std::vector<FilterSpec> filter_specs_;
    std::vector<SortSpec> sort_specs_;
    bool mutable_ = false;
    Cursor<Fetcher> cursor_;
    uint32_t last_execution_mutation_count_ =
        std::numeric_limits<uint32_t>::max();
  };

  // A typed version of `Cursor` which allows typed access and mutation of
  // dataframe cells while iterating over the rows of the dataframe.
  template <typename D>
  struct TypedCursor : TypedCursorBase {
   public:
    TypedCursor(const Dataframe* dataframe,
                std::vector<FilterSpec> filter_specs,
                std::vector<SortSpec> sort_specs)
        : TypedCursorBase{
              dataframe, {}, std::move(filter_specs), std::move(sort_specs),
              false,     {}} {
      filter_values_.resize(filter_specs_.size());
    }
    TypedCursor(Dataframe* dataframe,
                std::vector<FilterSpec> filter_specs,
                std::vector<SortSpec> sort_specs)
        : TypedCursorBase{
              dataframe, {}, std::move(filter_specs), std::move(sort_specs),
              true,      {}} {
      filter_values_.resize(filter_specs_.size());
    }

    // Sets the filter values for the current query plan.
    template <typename... C>
    PERFETTO_ALWAYS_INLINE void SetFilterValues(C... values) {
      PERFETTO_DCHECK(filter_values_.size() == sizeof...(values));
      filter_values_ = {values...};
    }

    // Executes the current query plan against the specified filter values and
    // populates the cursor with the results.
    //
    // See `SetFilterValues` for details on how to set the filter values.
    PERFETTO_ALWAYS_INLINE void ExecuteUnchecked() {
      ExecuteUncheckedInternal();
    }

    // Returns the current row index.
    PERFETTO_ALWAYS_INLINE uint32_t RowIndex() const {
      return cursor_.RowIndex();
    }

    // Advances the cursor to the next row of results.
    PERFETTO_ALWAYS_INLINE void Next() { cursor_.Next(); }

    // Returns true if the cursor has reached the end of the result set.
    PERFETTO_ALWAYS_INLINE bool Eof() const { return cursor_.Eof(); }

    // Calls `Dataframe:GetCellUnchecked` for the current row and specified
    // column.
    template <size_t C>
    PERFETTO_ALWAYS_INLINE auto GetCellUnchecked() {
      return dataframe_->GetCellUncheckedInternal<C, D>(cursor_.RowIndex());
    }

    // Calls `Dataframe:SetCellUnchecked` for the current row, specified column
    // and the given `value`.
    template <size_t C>
    PERFETTO_ALWAYS_INLINE void SetCellUnchecked(
        const typename D::template column_spec<C>::mutate_type& value) {
      PERFETTO_DCHECK(mutable_);
      const_cast<Dataframe*>(dataframe_)
          ->SetCellUncheckedInternal<C, D>(cursor_.RowIndex(), value);
    }
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
        std::is_convertible_v<std::tuple<Args...>, typename D::mutate_types>,
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
  void PrepareCursor(const QueryPlan& plan,
                     Cursor<FilterValueFetcherImpl>& c) const {
    c.Initialize(plan.plan_, uint32_t(column_ptrs_.size()), column_ptrs_.data(),
                 indexes_.data(), string_pool_);
  }

  // Given a typed spec, a column index and a row index, returns the value
  // stored in the dataframe at that position.
  //
  // Note: This function does not check the column type is compatible with the
  // specified spec. It is the caller's responsibility to ensure that the type
  // matches.
  //
  // Generally, this function is only safe to call if the dataframe was
  // constructed using the public Dataframe constructor and not in other ways.
  template <size_t column, typename D>
  PERFETTO_ALWAYS_INLINE auto GetCellUnchecked(const D&, uint32_t row) const {
    return GetCellUncheckedInternal<column, D>(row);
  }

  // Given a typed spec, a column index and a row index, returns the value
  // stored in the dataframe at that position.
  //
  // Note: This function does not check the column type is compatible with the
  // specified spec. It is the caller's responsibility to ensure that the type
  // matches.
  //
  // Generally, this function is only safe to call if the dataframe was
  // constructed using the public Dataframe constructor and not in other ways.
  //
  // Note: this function cannot be called on a finalized dataframe.
  //       See `MarkFinalized()` for more details.
  template <size_t column, typename D>
  PERFETTO_ALWAYS_INLINE void SetCellUnchecked(
      const D&,
      uint32_t row,
      const typename D::template column_spec<column>::mutate_type& value) {
    SetCellUncheckedInternal<column, D>(row, value);
  }

  // Creates a cursor for iterating over the rows of the dataframe.
  template <typename D>
  PERFETTO_ALWAYS_INLINE TypedCursor<D> CreateTypedCursorUnchecked(
      const D&,
      std::vector<FilterSpec> filter_specs,
      std::vector<SortSpec> sort_specs) {
    return TypedCursor<D>(this, std::move(filter_specs), std::move(sort_specs));
  }

  // Creates a cursor for iterating over the rows of the dataframe.
  template <typename D>
  PERFETTO_ALWAYS_INLINE TypedCursor<D> CreateTypedCursorUnchecked(
      const D&,
      std::vector<FilterSpec> filter_specs,
      std::vector<SortSpec> sort_specs) const {
    return TypedCursor<D>(this, std::move(filter_specs), std::move(sort_specs));
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
  void MarkFinalized();

  // Makes a copy of the dataframe.
  //
  // This is a shallow copy, meaning that the contents of columns and indexes
  // are not duplicated, but the dataframe itself is a new instance.
  dataframe::Dataframe Copy() const;

  // Creates a spec object for this dataframe.
  DataframeSpec CreateSpec() const;

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
    ++mutations_;
  }

  template <typename D, size_t I>
  PERFETTO_ALWAYS_INLINE void InsertUncheckedColumn(
      typename D::non_null_mutate_type t) {
    static_assert(std::is_same_v<typename D::null_storage_type, NonNull>);
    using type = typename D::type;
    auto& storage = columns_[I]->storage;
    if constexpr (std::is_same_v<type, Id>) {
      base::ignore_result(t);
      storage.unchecked_get<type>().size++;
    } else {
      storage.unchecked_get<type>().push_back(t);
    }
  }

  template <typename D, size_t I>
  PERFETTO_ALWAYS_INLINE void InsertUncheckedColumn(
      std::optional<typename D::non_null_mutate_type> t) {
    using type = typename D::type;
    using null_storage_type = typename D::null_storage_type;
    static_assert(!std::is_same_v<typename D::null_storage_type, NonNull>);

    auto& nulls = columns_[I]->null_storage.unchecked_get<null_storage_type>();
    auto& storage = columns_[I]->storage;

    if (t.has_value()) {
      if constexpr (std::is_same_v<type, Id>) {
        storage.unchecked_get<type>().size++;
      } else {
        storage.unchecked_get<type>().push_back(*t);
      }
    } else {
      if constexpr (std::is_same_v<null_storage_type, DenseNull>) {
        if constexpr (std::is_same_v<type, Id>) {
          storage.unchecked_get<type>().size++;
        } else {
          storage.unchecked_get<type>().push_back({});
        }
      }
    }

    static constexpr bool kIsSparseNullWithCellGet =
        std::is_same_v<null_storage_type, SparseNullSupportingCellGetAlways> ||
        std::is_same_v<null_storage_type,
                       SparseNullSupportingCellGetUntilFinalization>;
    if constexpr (kIsSparseNullWithCellGet) {
      if (nulls.bit_vector.size() % 64 == 0) {
        auto prefix_popcount =
            static_cast<uint32_t>(nulls.bit_vector.size() == 0
                                      ? 0
                                      : nulls.bit_vector.count_set_bits_in_word(
                                            nulls.bit_vector.size() - 1));
        nulls.prefix_popcount_for_cell_get.push_back(prefix_popcount);
      }
    }
    nulls.bit_vector.push_back(t.has_value());
  }

  template <size_t column, typename D>
  PERFETTO_ALWAYS_INLINE auto GetCellUncheckedInternal(uint32_t row) const {
    using ColumnSpec = std::tuple_element_t<column, typename D::columns>;
    using type = typename ColumnSpec::type;
    using null_storage_type = typename ColumnSpec::null_storage_type;
    static constexpr bool is_sparse_null_supporting_get_always =
        std::is_same_v<null_storage_type, SparseNullSupportingCellGetAlways>;
    static constexpr bool is_sparse_null_supporting_get_until_finalization =
        std::is_same_v<null_storage_type,
                       SparseNullSupportingCellGetUntilFinalization>;
    const auto& col = *column_ptrs_[column];
    const auto& storage = col.storage.unchecked_get<type>();
    const auto& nulls = col.null_storage.unchecked_get<null_storage_type>();
    if constexpr (std::is_same_v<null_storage_type, NonNull>) {
      return GetCellUncheckedFromStorage(storage, row);
    } else if constexpr (std::is_same_v<null_storage_type, DenseNull>) {
      return nulls.bit_vector.is_set(row)
                 ? std::make_optional(GetCellUncheckedFromStorage(storage, row))
                 : std::nullopt;
    } else if constexpr (is_sparse_null_supporting_get_always ||
                         is_sparse_null_supporting_get_until_finalization) {
      PERFETTO_DCHECK(is_sparse_null_supporting_get_always || !finalized_);
      return nulls.bit_vector.is_set(row)
                 ? std::make_optional(GetCellUncheckedFromStorage(
                       storage,
                       nulls.prefix_popcount_for_cell_get[row / 64] +
                           nulls.bit_vector.count_set_bits_until_in_word(row)))
                 : std::nullopt;
    } else if constexpr (std::is_same_v<null_storage_type, SparseNull>) {
      static_assert(
          !std::is_same_v<null_storage_type, null_storage_type>,
          "Trying to access a column with sparse nulls but without an approach "
          "that supports it. Please use SparseNullSupportingCellGetAlways or "
          "SparseNullSupportingCellGetUntilFinalization as appropriate.");
    } else {
      static_assert(std::is_same_v<null_storage_type, NonNull>,
                    "Unsupported null storage type");
    }
  }

  template <size_t column, typename D>
  PERFETTO_ALWAYS_INLINE auto SetCellUncheckedInternal(
      uint64_t row,
      const typename D::template column_spec<column>::mutate_type& value) {
    PERFETTO_DCHECK(!finalized_);

    using ColumnSpec = typename D::template column_spec<column>;
    using type = typename ColumnSpec::type;
    using null_storage_type = typename ColumnSpec::null_storage_type;

    // Changing the value of an Id column is not supported.
    static_assert(!std::is_same_v<type, Id>, "Cannot call set on Id column");

    // Make sure to increment the mutation count. This is important to let
    // others know that the dataframe has been modified.
    ++mutations_;

    auto& col = *column_ptrs_[column];
    auto& storage = col.storage.unchecked_get<type>();
    auto& nulls = col.null_storage.unchecked_get<null_storage_type>();
    if constexpr (std::is_same_v<null_storage_type, NonNull>) {
      storage[row] = value;
    } else if constexpr (std::is_same_v<null_storage_type, DenseNull>) {
      if (value.has_value()) {
        nulls.bit_vector.set(row);
        storage[row] = *value;
      } else {
        nulls.bit_vector.clear(row);
      }
    } else if constexpr (std::is_same_v<null_storage_type, SparseNull> ||
                         std::is_same_v<null_storage_type,
                                        SparseNullSupportingCellGetAlways> ||
                         std::is_same_v<
                             null_storage_type,
                             SparseNullSupportingCellGetUntilFinalization>) {
      static_assert(!std::is_same_v<null_storage_type, null_storage_type>,
                    "Trying to set a column with sparse nulls. This is not "
                    "supported, please use dense nulls.");
    } else {
      static_assert(std::is_same_v<null_storage_type, NonNull>,
                    "Unsupported null storage type");
    }
  }

  template <typename C>
  PERFETTO_ALWAYS_INLINE auto GetCellUncheckedFromStorage(const C& column,
                                                          uint64_t row) const {
    if constexpr (std::is_same_v<C, impl::Storage::Id>) {
      return row;
    } else {
      return column[row];
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

  // A count of the number of mutations to the dataframe. This includes adding
  // rows, setting cells to new values, adding indexes and removing indexes.
  //
  // This is used to determine if the dataframe has changed since the
  // last time an external caller looked at it. This can allow invalidation of
  // external caches of things inside this dataframe.
  uint32_t mutations_ = 0;

  // Whether the dataframe is "finalized". See `MarkFinalized()`.
  bool finalized_ = false;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_DATAFRAME_H_
