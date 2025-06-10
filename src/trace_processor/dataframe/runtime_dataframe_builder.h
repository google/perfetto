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

#ifndef SRC_TRACE_PROCESSOR_DATAFRAME_RUNTIME_DATAFRAME_BUILDER_H_
#define SRC_TRACE_PROCESSOR_DATAFRAME_RUNTIME_DATAFRAME_BUILDER_H_

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/compiler.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/flat_hash_map.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/dataframe/dataframe.h"
#include "src/trace_processor/dataframe/impl/bit_vector.h"
#include "src/trace_processor/dataframe/impl/flex_vector.h"
#include "src/trace_processor/dataframe/impl/slab.h"
#include "src/trace_processor/dataframe/impl/types.h"
#include "src/trace_processor/dataframe/specs.h"
#include "src/trace_processor/dataframe/value_fetcher.h"
#include "src/trace_processor/util/status_macros.h"

namespace perfetto::trace_processor::dataframe {

// Builds a Dataframe instance row by row at runtime.
//
// This class allows constructing a `Dataframe` incrementally. It infers
// column types (`int64_t`, `double`, `StringPool::Id`) based on the first
// non-null value encountered in each column. Null values are tracked
// efficiently using a `BitVector` (created only if nulls exist), and the
// underlying data storage only stores non-null values (SparseNull
// representation).
//
// Upon calling `Build()`, the builder analyzes the collected data to:
// - Determine the final optimal storage type for integer columns (downcasting
//   `int64_t` to `uint32_t` or `int32_t` if possible, or using `Id` type).
// - Determine the final sort state (`IdSorted`, `SetIdSorted`, `Sorted`,
//   `Unsorted`) by analyzing the collected values. Nullable columns are always
//   `Unsorted`.
// - Construct the final `Dataframe` object.
//
// Usage Example:
// ```cpp
// // Assume MyFetcher inherits from ValueFetcher and provides data for rows.
// struct MyFetcher : ValueFetcher {
//   // ... implementation to fetch data for current row ...
// };
//
// std::vector<std::string> col_names = {"ts", "value", "name"};
// StringPool pool;
// RuntimeDataframeBuilder builder(col_names, &pool);
// for (MyFetcher fetcher; fetcher.Next();) {
//   if (!builder.AddRow(&fetcher)) {
//     // Handle error (e.g., type mismatch)
//     PERFETTO_ELOG("Failed to add row: %s", builder.status().message());
//     break;
//   }
// }
//
// base::StatusOr<Dataframe> df = std::move(builder).Build();
// if (!df.ok()) {
//   // Handle build error
//   PERFETTO_ELOG("Failed to build dataframe: %s", df.status().message());
// } else {
//   // Use the dataframe *df...
// }
// ```
class RuntimeDataframeBuilder {
 public:
  // Constructs a RuntimeDataframeBuilder.
  //
  // Args:
  //   names: A vector of strings representing the names of the columns
  //          to be built. The order determines the column order as well.
  //   pool: A pointer to a `StringPool` instance used for interning
  //         string values encountered during row addition. Must remain
  //         valid for the lifetime of the builder and the resulting
  //         Dataframe.
  RuntimeDataframeBuilder(std::vector<std::string> names, StringPool* pool)
      : string_pool_(pool) {
    for (uint32_t i = 0; i < names.size(); ++i) {
      column_states_.emplace_back();
    }
    for (auto& name : names) {
      column_names_.emplace_back(std::move(name));
    }
  }
  ~RuntimeDataframeBuilder() = default;

  // Movable but not copyable
  RuntimeDataframeBuilder(RuntimeDataframeBuilder&&) noexcept;
  RuntimeDataframeBuilder& operator=(RuntimeDataframeBuilder&&) noexcept;
  RuntimeDataframeBuilder(const RuntimeDataframeBuilder&) = delete;
  RuntimeDataframeBuilder& operator=(const RuntimeDataframeBuilder&) = delete;

  // Adds a row to the dataframe using data provided by the Fetcher.
  //
  // Template Args:
  //   ValueFetcherImpl: A concrete class derived from `ValueFetcher` that
  //                     provides methods like `GetValueType(col_idx)` and
  //                     `GetInt64Value(col_idx)`, `GetDoubleValue(col_idx)`,
  //                     `GetStringValue(col_idx)` for the current row.
  // Args:
  //   fetcher: A pointer to an instance of `ValueFetcherImpl`, configured
  //            to provide data for the row being added. The fetcher only
  //            needs to be valid for the duration of this call.
  // Returns:
  //   true: If the row was added successfully.
  //   false: If an error occurred (e.g., type mismatch). Check `status()` for
  //          details. The builder should not be used further if false is
  //          returned.
  //
  // Implementation Notes:
  // 1) Infers column types (int64_t, double, StringPool::Id) based on the first
  //    non-null value encountered. Stores integer types smaller than int64_t
  //    (i.e. Id, uint32_t, int32_t) initially as int64_t, with potential
  //    downcasting occurring during Build().
  // 2) Tracks null values sparsely: only non-null values are appended to the
  //    internal data storage vectors. A BitVector is created and maintained
  //    only if null values are encountered for a column.
  // 3) Performs strict type checking against the inferred type for subsequent
  //    rows. If a type mismatch occurs, sets an error status (retrievable via
  //    status()) and returns false.
  template <typename ValueFetcherImpl>
  bool AddRow(ValueFetcherImpl* fetcher) {
    static_assert(std::is_base_of_v<ValueFetcher, ValueFetcherImpl>,
                  "ValueFetcherImpl must inherit from ValueFetcher");
    PERFETTO_CHECK(current_status_.ok());

    for (uint32_t i = 0; i < column_names_.size(); ++i) {
      ColumnState& state = column_states_[i];
      typename ValueFetcherImpl::Type fetched_type = fetcher->GetValueType(i);
      switch (fetched_type) {
        case ValueFetcherImpl::kInt64: {
          if (!Push(i, state.data, fetcher->GetInt64Value(i))) {
            return false;
          }
          break;
        }
        case ValueFetcherImpl::kDouble: {
          if (!Push(i, state.data, fetcher->GetDoubleValue(i))) {
            return false;
          }
          break;
        }
        case ValueFetcherImpl::kString: {
          if (!Push(i, state.data,
                    string_pool_->InternString(fetcher->GetStringValue(i)))) {
            return false;
          }
          break;
        }
        case ValueFetcherImpl::kNull:
          if (PERFETTO_UNLIKELY(!state.null_overlay)) {
            state.null_overlay =
                impl::BitVector::CreateWithSize(row_count_, true);
          }
          break;
      }
      if (PERFETTO_UNLIKELY(state.null_overlay)) {
        state.null_overlay->push_back(fetched_type != ValueFetcherImpl::kNull);
      }
    }  // End column loop
    row_count_++;
    return true;
  }

  // Finalizes the builder and attempts to construct the Dataframe.
  // This method consumes the builder (note the && qualifier).
  //
  // Returns:
  //   StatusOr<Dataframe>: On success, contains the built `Dataframe`.
  //                        On failure (e.g., if `AddRow` previously failed),
  //                        contains an error status retrieved from `status()`.
  //
  // Implementation wise, the collected data for each column is analyzed to:
  // - Determine the final optimal storage type (e.g., downcasting int64_t to
  //   uint32_t/int32_t if possible, using Id type if applicable).
  // - Determine the final nullability overlay (NonNull or SparseNull).
  // - Determine the final sort state (IdSorted, SetIdSorted, Sorted, Unsorted)
  //   by analyzing the collected non-null values.
  // - Construct and return the final `Dataframe` instance.
  base::StatusOr<Dataframe> Build() && {
    RETURN_IF_ERROR(current_status_);
    std::vector<std::shared_ptr<impl::Column>> columns;
    for (uint32_t i = 0; i < column_names_.size(); ++i) {
      auto& state = column_states_[i];
      switch (state.data.index()) {
        case base::variant_index<DataVariant, std::nullopt_t>():
          columns.emplace_back(std::make_shared<impl::Column>(impl::Column{
              impl::Storage{impl::FlexVector<uint32_t>()},
              CreateNullStorageFromBitvector(std::move(state.null_overlay)),
              Unsorted{}, HasDuplicates{}}));
          break;
        case base::variant_index<DataVariant, impl::FlexVector<int64_t>>(): {
          auto& data =
              base::unchecked_get<impl::FlexVector<int64_t>>(state.data);

          IntegerColumnSummary summary;
          summary.is_id_sorted = data.empty() || data[0] == 0;
          summary.is_setid_sorted = data.empty() || data[0] == 0;
          summary.is_sorted = true;
          summary.min = data.empty() ? 0 : data[0];
          summary.max = data.empty() ? 0 : data[0];
          summary.has_duplicates = false;
          summary.is_nullable = state.null_overlay.has_value();
          if (!data.empty()) {
            seen_ints_.clear();
            seen_ints_.reserve(data.size());
            seen_ints_.insert(data[0]);
          }
          for (uint32_t j = 1; j < data.size(); ++j) {
            summary.is_id_sorted = summary.is_id_sorted && (data[j] == j);
            summary.is_setid_sorted = summary.is_setid_sorted &&
                                      (data[j] == data[j - 1] || data[j] == j);
            summary.is_sorted = summary.is_sorted && data[j - 1] <= data[j];
            summary.min = std::min(summary.min, data[j]);
            summary.max = std::max(summary.max, data[j]);
            summary.has_duplicates =
                summary.has_duplicates || !seen_ints_.insert(data[j]).second;
          }
          auto integer = CreateIntegerStorage(std::move(data), summary);
          impl::SpecializedStorage specialized_storage =
              GetSpecializedStorage(integer, summary);
          columns.emplace_back(std::make_shared<impl::Column>(impl::Column{
              std::move(integer),
              CreateNullStorageFromBitvector(std::move(state.null_overlay)),
              GetIntegerSortStateFromProperties(summary),
              summary.is_nullable || summary.has_duplicates
                  ? DuplicateState{HasDuplicates{}}
                  : DuplicateState{NoDuplicates{}},
              std::move(specialized_storage),
          }));
          break;
        }
        case base::variant_index<DataVariant, impl::FlexVector<double>>(): {
          auto& data =
              base::unchecked_get<impl::FlexVector<double>>(state.data);
          bool is_nullable = state.null_overlay.has_value();
          bool is_sorted = true;
          bool has_duplicates = false;
          if (!data.empty()) {
            seen_doubles_.clear();
            seen_doubles_.reserve(data.size());
            seen_doubles_.insert(data[0]);
          }
          for (uint32_t j = 1; j < data.size(); ++j) {
            is_sorted = is_sorted && data[j - 1] <= data[j];
            has_duplicates =
                has_duplicates || !seen_doubles_.insert(data[j]).second;
          }
          columns.emplace_back(std::make_shared<impl::Column>(impl::Column{
              impl::Storage{std::move(data)},
              CreateNullStorageFromBitvector(std::move(state.null_overlay)),
              is_sorted && !is_nullable ? SortState{Sorted{}}
                                        : SortState{Unsorted{}},
              is_nullable || has_duplicates ? DuplicateState{HasDuplicates{}}
                                            : DuplicateState{NoDuplicates{}}}));
          break;
        }
        case base::variant_index<DataVariant,
                                 impl::FlexVector<StringPool::Id>>(): {
          auto& data =
              base::unchecked_get<impl::FlexVector<StringPool::Id>>(state.data);
          bool is_nullable = state.null_overlay.has_value();
          bool is_sorted = true;
          bool has_duplicates = false;
          if (!data.empty()) {
            seen_strings_.clear();
            seen_strings_.reserve(data.size());
            seen_strings_.insert(data[0]);
            NullTermStringView prev = string_pool_->Get(data[0]);
            for (uint32_t j = 1; j < data.size(); ++j) {
              NullTermStringView curr = string_pool_->Get(data[j]);
              is_sorted = is_sorted && prev <= curr;
              has_duplicates =
                  has_duplicates || !seen_strings_.insert(data[j]).second;
              prev = curr;
            }
          }
          columns.emplace_back(std::make_shared<impl::Column>(impl::Column{
              impl::Storage{std::move(data)},
              CreateNullStorageFromBitvector(std::move(state.null_overlay)),
              is_sorted && !is_nullable ? SortState{Sorted{}}
                                        : SortState{Unsorted{}},
              is_nullable || has_duplicates ? DuplicateState{HasDuplicates{}}
                                            : DuplicateState{NoDuplicates{}}}));
          break;
        }
      }
    }
    // Create an implicit id column for acting as a primary key even if there
    // are no other id columns.
    column_names_.emplace_back("_auto_id");
    columns.emplace_back(std::make_shared<impl::Column>(impl::Column{
        impl::Storage{impl::Storage::Id{row_count_}},
        impl::NullStorage::NonNull{}, IdSorted{}, NoDuplicates{}}));
    return Dataframe(true, std::move(column_names_), std::move(columns),
                     row_count_, string_pool_);
  }

  // Returns the current status of the builder.
  //
  // If `AddRow` returned `false`, this method can be used to retrieve the
  // `base::Status` object containing the error details (e.g., type mismatch).
  //
  // Returns:
  //   const base::Status&: The current status. `ok()` will be true unless
  //                        an error occurred during a previous `AddRow` call.
  const base::Status& status() const { return current_status_; }

 private:
  using DataVariant = std::variant<std::nullopt_t,
                                   impl::FlexVector<int64_t>,
                                   impl::FlexVector<double>,
                                   impl::FlexVector<StringPool::Id>>;
  struct ColumnState {
    DataVariant data = std::nullopt;
    std::optional<impl::BitVector> null_overlay;
  };
  struct IntegerColumnSummary {
    bool is_id_sorted = true;
    bool is_setid_sorted = true;
    bool is_sorted = true;
    int64_t min = 0;
    int64_t max = 0;
    bool has_duplicates = false;
    bool is_nullable = false;
  };

  template <typename T>
  PERFETTO_ALWAYS_INLINE bool Push(uint32_t col, DataVariant& data, T value) {
    switch (data.index()) {
      case base::variant_index<DataVariant, std::nullopt_t>(): {
        data = impl::FlexVector<T>();
        auto& vec = base::unchecked_get<impl::FlexVector<T>>(data);
        vec.push_back(value);
        return true;
      }
      case base::variant_index<DataVariant, impl::FlexVector<T>>(): {
        auto& vec = base::unchecked_get<impl::FlexVector<T>>(data);
        vec.push_back(value);
        return true;
      }
      default: {
        if constexpr (std::is_same_v<T, double>) {
          if (std::holds_alternative<impl::FlexVector<int64_t>>(data)) {
            auto& vec = base::unchecked_get<impl::FlexVector<int64_t>>(data);
            auto res = impl::FlexVector<double>::CreateWithSize(vec.size());
            for (uint32_t i = 0; i < vec.size(); ++i) {
              int64_t v = vec[i];
              if (!IsPerfectlyRepresentableAsDouble(v)) {
                current_status_ =
                    base::ErrStatus("Unable to represent %" PRId64
                                    " in column '%s' at row %u as a double.",
                                    v, column_names_[col].c_str(), i);
                return false;
              }
              res[i] = static_cast<double>(v);
            }
            res.push_back(value);
            data = std::move(res);
            return true;
          }
        } else if constexpr (std::is_same_v<T, int64_t>) {
          if (std::holds_alternative<impl::FlexVector<double>>(data)) {
            if (IsPerfectlyRepresentableAsDouble(value)) {
              auto& vec = base::unchecked_get<impl::FlexVector<double>>(data);
              vec.push_back(static_cast<double>(value));
              return true;
            }
            current_status_ =
                base::ErrStatus("Inserting a too-large integer (%" PRId64
                                ") in column '%s' at row "
                                "%u. Column currently holds doubles.",
                                value, column_names_[col].c_str(), row_count_);
            return false;
          }
        }
        current_status_ = base::ErrStatus(
            "Type mismatch in column '%s' at row %u. Existing type != "
            "fetched type.",
            column_names_[col].c_str(), row_count_);
        return false;
      }
    }
  }

  static constexpr bool IsPerfectlyRepresentableAsDouble(int64_t res) {
    constexpr int64_t kMaxDoubleRepresentible = 1ull << 53;
    return res >= -kMaxDoubleRepresentible && res <= kMaxDoubleRepresentible;
  }

  static impl::Storage CreateIntegerStorage(
      impl::FlexVector<int64_t> data,
      const IntegerColumnSummary& summary) {
    if (summary.is_id_sorted) {
      return impl::Storage{
          impl::Storage::Id{static_cast<uint32_t>(data.size())}};
    }
    if (IsRangeFullyRepresentableByType<uint32_t>(summary.min, summary.max)) {
      return impl::Storage{
          impl::Storage::Uint32{DowncastFromInt64<uint32_t>(data)}};
    }
    if (IsRangeFullyRepresentableByType<int32_t>(summary.min, summary.max)) {
      return impl::Storage{
          impl::Storage::Int32{DowncastFromInt64<int32_t>(data)}};
    }
    return impl::Storage{impl::Storage::Int64{std::move(data)}};
  }

  static impl::NullStorage CreateNullStorageFromBitvector(
      std::optional<impl::BitVector> bit_vector) {
    if (bit_vector) {
      return impl::NullStorage{
          impl::NullStorage::SparseNull{*std::move(bit_vector), {}}};
    }
    return impl::NullStorage{impl::NullStorage::NonNull{}};
  }

  template <typename T>
  static bool IsRangeFullyRepresentableByType(int64_t min, int64_t max) {
    // The <= for max is intentional because we're checking representability
    // of min/max, not looping or similar.
    PERFETTO_DCHECK(min <= max);
    return min >= std::numeric_limits<T>::min() &&
           max <= std::numeric_limits<T>::max();
  }

  template <typename T>
  static impl::FlexVector<T> DowncastFromInt64(
      const impl::FlexVector<int64_t>& data) {
    auto res = impl::FlexVector<T>::CreateWithSize(data.size());
    for (uint32_t i = 0; i < data.size(); ++i) {
      PERFETTO_DCHECK(IsRangeFullyRepresentableByType<T>(data[i], data[i]));
      res[i] = static_cast<T>(data[i]);
    }
    return res;
  }

  static SortState GetIntegerSortStateFromProperties(
      const IntegerColumnSummary& summary) {
    if (summary.is_nullable) {
      return SortState{Unsorted{}};
    }
    if (summary.is_id_sorted) {
      PERFETTO_DCHECK(summary.is_setid_sorted);
      PERFETTO_DCHECK(summary.is_sorted);
      return SortState{IdSorted{}};
    }
    if (summary.is_setid_sorted) {
      PERFETTO_DCHECK(summary.is_sorted);
      return SortState{SetIdSorted{}};
    }
    if (summary.is_sorted) {
      return SortState{Sorted{}};
    }
    return SortState{Unsorted{}};
  }

  static impl::SpecializedStorage GetSpecializedStorage(
      const impl::Storage& storage,
      const IntegerColumnSummary& summary) {
    // If we're already sorted or setid_sorted, we don't need specialized
    // storage.
    if (summary.is_id_sorted || summary.is_setid_sorted) {
      return impl::SpecializedStorage{};
    }

    // Check if we meet the hard conditions for small value eq.
    if (storage.type().Is<Uint32>() && summary.is_sorted &&
        !summary.is_nullable && !summary.has_duplicates) {
      const auto& vec = storage.unchecked_get<Uint32>();

      // For memory reasons, we only use small value eq if the ratio between
      // the maximum value and the number of values is "small enough".
      if (static_cast<uint32_t>(summary.max) < 16 * vec.size()) {
        return BuildSmallValueEq(vec);
      }
    }
    // Otherwise, we cannot use specialized storage.
    return impl::SpecializedStorage{};
  }

  static impl::SpecializedStorage::SmallValueEq BuildSmallValueEq(
      const impl::FlexVector<uint32_t>& data) {
    impl::SpecializedStorage::SmallValueEq offset_bv{
        impl::BitVector::CreateWithSize(data.empty() ? 0 : data.back() + 1,
                                        false),
        {},
    };
    for (uint32_t i : data) {
      offset_bv.bit_vector.set(i);
    }
    offset_bv.prefix_popcount = offset_bv.bit_vector.PrefixPopcount();
    return offset_bv;
  }

  StringPool* string_pool_;
  uint32_t row_count_ = 0;
  std::vector<std::string> column_names_;
  std::vector<ColumnState> column_states_;
  base::Status current_status_ = base::OkStatus();

  // Class variables to avoid repeated allocations for sets across columns.
  std::unordered_set<int64_t> seen_ints_;
  std::unordered_set<double> seen_doubles_;
  std::unordered_set<StringPool::Id> seen_strings_;
};

}  // namespace perfetto::trace_processor::dataframe

#endif  // SRC_TRACE_PROCESSOR_DATAFRAME_RUNTIME_DATAFRAME_BUILDER_H_
