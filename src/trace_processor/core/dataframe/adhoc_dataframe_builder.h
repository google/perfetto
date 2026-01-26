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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ADHOC_DATAFRAME_BUILDER_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ADHOC_DATAFRAME_BUILDER_H_

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/ext/base/variant.h"
#include "perfetto/public/compiler.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/dataframe/types.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "src/trace_processor/core/util/flex_vector.h"

namespace perfetto::trace_processor::core::dataframe {

// Builds a `Dataframe` on an adhoc basis by allowing users to append
// values column by column.
//
// This class provides a flexible way to construct a `Dataframe` when data is
// in a partially columar format but still needs to be checked for typing and
// sorting/duplicates.
//
// If the data is purely in a row-oriented format, consider using
// `RuntimeDataframeBuilder` instead, which is optimized for that use case.
//
// Usage:
// 1. Construct an `AdhocDataframeBuilder` with column names and an optional
//    `StringPool` for string interning. Column types can be provided or
//    will be inferred from the first non-null value added to each column.
// 2. Append data to columns using `PushNonNull`, `PushNonNullUnchecked`, or
//    `PushNull`. These methods add values to the end of the respective columns.
//    Conceptually, ensure that each "row" has a value (or null) for every
//    column before moving to the next "row".
// 3. Call `Build()` to finalize the `Dataframe`. This method consumes the
//    builder and returns a `StatusOr<Dataframe>`. `Build()` analyzes the
//    collected data to optimize storage types (e.g., downcasting integers),
//    determine nullability overlays, and infer sort states.
//
// Example:
// ```
// StringPool pool;
// AdhocDataframeBuilder builder({"col_a", "col_b"}, &pool);
// builder.PushNonNull(0, 10);   // col_a, row 0
// builder.PushNonNull(1, "foo"); // col_b, row 0
// builder.PushNonNull(0, 20);   // col_a, row 1
// builder.PushNull(1);        // col_b, row 1 (null)
// auto df_status = std::move(builder).Build();
// if (df_status.ok()) {
//   Dataframe df = std::move(df_status.value());
//   // ... use df
// }
// ```
//
// Note:
// - The `StringPool` instance must remain valid for the lifetime of the
//   builder and the resulting `Dataframe`.
// - If `PushNonNull` returns false (e.g. due to type mismatch), the error
//   status can be retrieved using the `status()` method. The `Build()`
//   method will also propagate this error.
// - The builder is movable but not copyable.

// Indicates the nullability type for nullable columns in the dataframe.
enum class NullabilityType : uint8_t {
  kSparseNull,
  kSparseNullWithPopcount,
  kDenseNull,
};

// Column types for AdhocDataframeBuilder.
enum class AdhocColumnType : uint8_t {
  kInt64,
  kDouble,
  kString,
};

// Options to be provided to the AdhocDataframeBuilder.
struct AdhocDataframeBuilderOptions {
  // An optional vector of `AdhocColumnType` specifying the types of the
  // columns. If empty, types are inferred from the first non-null value added
  // to each column. If provided, must match the size of `names`.
  std::vector<AdhocColumnType> types;

  // Indicates the default option for nullable columns to be converted to.
  NullabilityType nullability_type = NullabilityType::kSparseNull;
};

class AdhocDataframeBuilder {
 public:
  using ColumnType = AdhocColumnType;
  using Options = AdhocDataframeBuilderOptions;

  // Constructs a AdhocDataframeBuilder.
  //
  // Args:
  //   names: A vector of strings representing the names of the columns
  //          to be built. The order determines the column order as well.
  //   pool: A pointer to a `StringPool` instance used for interning
  //         string values encountered during row addition. Must remain
  //         valid for the lifetime of the builder and the resulting
  //         Dataframe.
  //  options: Options to configure the builder. See `Options` struct for
  //           details.
  AdhocDataframeBuilder(std::vector<std::string> names,
                        StringPool* pool,
                        const Options& options = Options{});
  ~AdhocDataframeBuilder() = default;

  // Movable but not copyable
  AdhocDataframeBuilder(AdhocDataframeBuilder&&) noexcept = default;
  AdhocDataframeBuilder& operator=(AdhocDataframeBuilder&&) noexcept = default;
  AdhocDataframeBuilder(const AdhocDataframeBuilder&) = delete;
  AdhocDataframeBuilder& operator=(const AdhocDataframeBuilder&) = delete;

  // Appends `count` copies of `value` to the specified column `col`.
  //
  // Returns true on success, false on failure (e.g., if the column type
  // does not match the type of `value`). The failure status *must* be
  // retrieved using `status()` method.
  PERFETTO_ALWAYS_INLINE bool PushNonNull(uint32_t col,
                                          uint32_t value,
                                          uint32_t count = 1) {
    return PushNonNullInternal(col, int64_t(value), count);
  }
  PERFETTO_ALWAYS_INLINE bool PushNonNull(uint32_t col,
                                          int64_t value,
                                          uint32_t count = 1) {
    return PushNonNullInternal(col, value, count);
  }
  PERFETTO_ALWAYS_INLINE bool PushNonNull(uint32_t col,
                                          double value,
                                          uint32_t count = 1) {
    return PushNonNullInternal(col, value, count);
  }
  PERFETTO_ALWAYS_INLINE bool PushNonNull(uint32_t col,
                                          StringPool::Id value,
                                          uint32_t count = 1) {
    return PushNonNullInternal(col, value, count);
  }

  // Appends `count` copies of `value` to the specified column `col`.
  // This method does not check if the column has the correct type or try to do
  // any conversions. It is intended for use when the caller is certain that the
  // column type matches the type of `value`.
  PERFETTO_ALWAYS_INLINE void PushNonNullUnchecked(uint32_t col,
                                                   uint32_t value,
                                                   uint32_t count = 1) {
    PushNonNullUncheckedInternal(col, int64_t(value), count);
  }
  PERFETTO_ALWAYS_INLINE void PushNonNullUnchecked(uint32_t col,
                                                   int64_t value,
                                                   uint32_t count = 1) {
    PushNonNullUncheckedInternal(col, value, count);
  }
  PERFETTO_ALWAYS_INLINE void PushNonNullUnchecked(uint32_t col,
                                                   double value,
                                                   uint32_t count = 1) {
    PushNonNullUncheckedInternal(col, value, count);
  }
  PERFETTO_ALWAYS_INLINE void PushNonNullUnchecked(uint32_t col,
                                                   StringPool::Id value,
                                                   uint32_t count = 1) {
    PushNonNullUncheckedInternal(col, value, count);
  }

  // Appends `count` null values to the specified column `col`.
  PERFETTO_ALWAYS_INLINE void PushNull(uint32_t col, uint32_t count = 1) {
    auto& state = column_states_[col];
    if (PERFETTO_UNLIKELY(!state.null_overlay)) {
      EnsureNullOverlayExists(state);
    }
    state.null_overlay->push_back_multiple(false, count);
    if (nullability_type_ == NullabilityType::kDenseNull) {
      // For dense null, we need to push placeholder values to the data storage
      // since DenseNull stores all values (with nulls marked by the bitvector).
      AddPlaceholderValue(col, count);
    }
  }

  // Appends `count` placeholder values to the specified column `col`.
  // This is useful for DenseNull columns where placeholder values need to be
  // pushed even for null entries.
  void AddPlaceholderValue(uint32_t col, uint32_t count = 1);

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
  base::StatusOr<Dataframe> Build() &&;

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
  struct Null {};
  using DataVariant = std::variant<std::nullopt_t,
                                   core::FlexVector<int64_t>,
                                   core::FlexVector<double>,
                                   core::FlexVector<StringPool::Id>>;
  struct ColumnState {
    DataVariant data = std::nullopt;
    std::optional<core::BitVector> null_overlay;
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

  static constexpr bool IsPerfectlyRepresentableAsDouble(int64_t res) {
    constexpr int64_t kMaxDoubleRepresentible = 1ull << 53;
    return res >= -kMaxDoubleRepresentible && res <= kMaxDoubleRepresentible;
  }

  // Appends `count` copies of `value` to the specified column `col`.
  //
  // Returns true on success, false on failure (e.g., if the column type
  // does not match the type of `value`). The failure status *must* be
  // retrieved using `status()` method.
  template <typename T>
  PERFETTO_ALWAYS_INLINE bool PushNonNullInternal(uint32_t col,
                                                  T value,
                                                  uint32_t count = 1) {
    auto& state = column_states_[col];
    auto& data = state.data;
    switch (data.index()) {
      case base::variant_index<DataVariant, std::nullopt_t>(): {
        data = core::FlexVector<T>();
        auto& vec = base::unchecked_get<core::FlexVector<T>>(data);
        vec.push_back_multiple(value, count);
        break;
      }
      case base::variant_index<DataVariant, core::FlexVector<T>>(): {
        auto& vec = base::unchecked_get<core::FlexVector<T>>(data);
        vec.push_back_multiple(value, count);
        break;
      }
      default: {
        if constexpr (std::is_same_v<T, double>) {
          if (std::holds_alternative<core::FlexVector<int64_t>>(data)) {
            auto& vec = base::unchecked_get<core::FlexVector<int64_t>>(data);
            auto res = core::FlexVector<double>::CreateWithSize(vec.size());
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
            res.push_back_multiple(value, count);
            data = std::move(res);
            break;
          }
        } else if constexpr (std::is_same_v<T, int64_t>) {
          if (std::holds_alternative<core::FlexVector<double>>(data)) {
            auto& vec = base::unchecked_get<core::FlexVector<double>>(data);
            if (!IsPerfectlyRepresentableAsDouble(value)) {
              current_status_ = base::ErrStatus(
                  "Inserting a too-large integer (%" PRId64
                  ") in column '%s' at row %" PRIu64
                  ". Column currently holds doubles.",
                  value, column_names_[col].c_str(), vec.size());
              return false;
            }
            vec.push_back_multiple(static_cast<double>(value), count);
            break;
          }
        }
        if (did_declare_types_) {
          current_status_ = base::ErrStatus(
              "column '%s' declared as %s in the schema, but %s found",
              column_names_[col].c_str(), ToString(data), ToString<T>());
        } else {
          current_status_ = base::ErrStatus(
              "column '%s' was inferred to be %s, but later received a value "
              "of type %s",
              column_names_[col].c_str(), ToString(data), ToString<T>());
        }
        return false;
      }
    }
    if (PERFETTO_UNLIKELY(state.null_overlay)) {
      state.null_overlay->push_back_multiple(true, count);
    }
    return true;
  }

  template <typename T>
  PERFETTO_ALWAYS_INLINE void PushNonNullUncheckedInternal(uint32_t col,
                                                           T value,
                                                           uint32_t count) {
    auto& state = column_states_[col];
    PERFETTO_DCHECK(std::holds_alternative<core::FlexVector<T>>(state.data));
    auto& data = base::unchecked_get<core::FlexVector<T>>(state.data);
    data.push_back_multiple(value, count);
    if (PERFETTO_UNLIKELY(state.null_overlay)) {
      state.null_overlay->push_back_multiple(true, count);
    }
  }

  static Storage CreateIntegerStorage(core::FlexVector<int64_t> data,
                                      const IntegerColumnSummary& summary);

  static NullStorage CreateNullStorageFromBitvector(
      std::optional<core::BitVector> bit_vector,
      NullabilityType nullability_type);

  template <typename T>
  static bool IsRangeFullyRepresentableByType(int64_t min, int64_t max) {
    // The <= for max is intentional because we're checking representability
    // of min/max, not looping or similar.
    PERFETTO_DCHECK(min <= max);
    return min >= std::numeric_limits<T>::min() &&
           max <= std::numeric_limits<T>::max();
  }

  template <typename T>
  PERFETTO_NO_INLINE static core::FlexVector<T> DowncastFromInt64(
      const core::FlexVector<int64_t>& data) {
    auto res = core::FlexVector<T>::CreateWithSize(data.size());
    for (uint32_t i = 0; i < data.size(); ++i) {
      PERFETTO_DCHECK(IsRangeFullyRepresentableByType<T>(data[i], data[i]));
      res[i] = static_cast<T>(data[i]);
    }
    return res;
  }

  static SortState GetIntegerSortStateFromProperties(
      const IntegerColumnSummary& summary);

  static SpecializedStorage GetSpecializedStorage(
      const Storage& storage,
      const IntegerColumnSummary& summary);

  static SpecializedStorage::SmallValueEq BuildSmallValueEq(
      const core::FlexVector<uint32_t>& data);

  static void EnsureNullOverlayExists(ColumnState& state);

  // Returns true if the value is a definite duplicate.
  PERFETTO_ALWAYS_INLINE bool CheckDuplicate(int64_t value, size_t size) {
    if (value < 0) {
      return true;
    }
    if (PERFETTO_UNLIKELY(value >=
                          static_cast<int64_t>(duplicate_bit_vector_.size()))) {
      if (value >= static_cast<int64_t>(16ll * size)) {
        return true;
      }
      duplicate_bit_vector_.push_back_multiple(
          false,
          static_cast<uint32_t>(value) - duplicate_bit_vector_.size() + 1);
    }
    if (duplicate_bit_vector_.is_set(static_cast<uint32_t>(value))) {
      return true;
    }
    duplicate_bit_vector_.set(static_cast<uint32_t>(value));
    return false;
  }

  static const char* ToString(const DataVariant&);

  template <typename T>
  static const char* ToString() {
    if constexpr (std::is_same_v<T, int64_t>) {
      return "LONG";
    } else if constexpr (std::is_same_v<T, double>) {
      return "DOUBLE";
    } else if constexpr (std::is_same_v<T, StringPool::Id>) {
      return "STRING";
    } else {
      return "unknown type";
    }
  }

  StringPool* string_pool_;
  std::vector<std::string> column_names_;
  std::vector<ColumnState> column_states_;
  bool did_declare_types_ = false;
  NullabilityType nullability_type_ = NullabilityType::kSparseNull;
  base::Status current_status_ = base::OkStatus();
  core::BitVector duplicate_bit_vector_;
};

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_ADHOC_DATAFRAME_BUILDER_H_
