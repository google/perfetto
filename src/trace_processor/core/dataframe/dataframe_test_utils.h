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

#ifndef SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TEST_UTILS_H_
#define SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TEST_UTILS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "src/trace_processor/containers/null_term_string_view.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/core/common/null_types.h"
#include "src/trace_processor/core/common/storage_types.h"
#include "src/trace_processor/core/common/value_fetcher.h"
#include "src/trace_processor/core/dataframe/dataframe.h"
#include "src/trace_processor/core/dataframe/specs.h"
#include "src/trace_processor/core/util/bit_vector.h"
#include "test/gtest_and_gmock.h"

namespace perfetto::trace_processor::core::dataframe {

struct TestRowFetcher : ValueFetcher {
  using Value = std::variant<std::nullopt_t, int64_t, double, const char*>;
  enum Type : uint8_t {
    kNull,
    kInt64,
    kDouble,
    kString,
    kBytes,
  };

  void SetRow(const std::vector<Value>& row_data) { current_row_ = &row_data; }

  Type GetValueType(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    const auto& var = (*current_row_)[index];
    if (std::holds_alternative<std::nullopt_t>(var))
      return Type::kNull;
    if (std::holds_alternative<int64_t>(var))
      return Type::kInt64;
    if (std::holds_alternative<double>(var))
      return Type::kDouble;
    if (std::holds_alternative<const char*>(var))
      return Type::kString;
    PERFETTO_FATAL("Invalid variant state");
  }

  int64_t GetInt64Value(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<int64_t>((*current_row_)[index]);
  }

  double GetDoubleValue(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<double>((*current_row_)[index]);
  }

  const char* GetStringValue(uint32_t index) {
    PERFETTO_CHECK(current_row_ && index < current_row_->size());
    return std::get<const char*>((*current_row_)[index]);
  }
  static bool IteratorInit(uint32_t) { PERFETTO_FATAL("Unsupported"); }
  static bool IteratorNext(uint32_t) { PERFETTO_FATAL("Unsupported"); }

 private:
  const std::vector<Value>* current_row_ = nullptr;
};

// Callback for verifying cell values from the cursor.
// Stores visited values in variants for later checking.
struct ValueVerifier : CellCallback {
  using ValueVariant = std::variant<uint32_t,
                                    int32_t,
                                    int64_t,
                                    double,
                                    NullTermStringView,
                                    std::nullptr_t>;

  void Fetch(const Dataframe& df, uint32_t row, uint32_t col_count) {
    for (uint32_t i = 0; i < col_count; ++i) {
      df.GetCell(row, i, *this);
    }
  }
  void OnCell(int64_t value) { values.emplace_back(value); }
  void OnCell(double value) { values.emplace_back(value); }
  void OnCell(NullTermStringView value) { values.emplace_back(value); }
  void OnCell(std::nullptr_t) { values.emplace_back(nullptr); }
  void OnCell(int32_t value) { values.emplace_back(value); }
  void OnCell(uint32_t value) { values.emplace_back(value); }

  std::vector<ValueVariant> values;
};

template <typename Callback>
inline void VisitStorageCell(const Storage& storage,
                             uint32_t storage_idx,
                             const StringPool* string_pool,
                             Callback& cb) {
  const Storage::DataPointer ptr = storage.data();
  switch (ptr.index()) {
    case StorageType::GetTypeIndex<Id>():
      cb.OnCell(storage_idx);
      break;
    case StorageType::GetTypeIndex<Uint32>():
      cb.OnCell(Storage::CastDataPtr<Uint32>(ptr)[storage_idx]);
      break;
    case StorageType::GetTypeIndex<Int32>():
      cb.OnCell(Storage::CastDataPtr<Int32>(ptr)[storage_idx]);
      break;
    case StorageType::GetTypeIndex<Int64>():
      cb.OnCell(Storage::CastDataPtr<Int64>(ptr)[storage_idx]);
      break;
    case StorageType::GetTypeIndex<Double>():
      cb.OnCell(Storage::CastDataPtr<Double>(ptr)[storage_idx]);
      break;
    case StorageType::GetTypeIndex<String>(): {
      auto id = Storage::CastDataPtr<String>(ptr)[storage_idx];
      if (id.is_null()) {
        cb.OnCell(nullptr);
      } else {
        cb.OnCell(string_pool->Get(id));
      }
      break;
    }
    default:
      PERFETTO_FATAL("Invalid storage type");
  }
}

inline void VerifyData(
    Dataframe& df,
    uint64_t cols_bitmap,
    const std::vector<std::vector<ValueVerifier::ValueVariant>>& expected) {
  std::vector<uint32_t> selected_cols;
  for (uint32_t c = 0; c < df.column_count(); ++c) {
    if ((cols_bitmap >> std::min(c, 63u)) & 1u) {
      selected_cols.push_back(c);
    }
  }
  std::vector<uint32_t> sparse_pos(selected_cols.size(), 0);

  ASSERT_EQ(df.row_count(), expected.size())
      << "Mismatch in number of rows processed.";
  for (uint32_t row = 0; row < df.row_count(); ++row) {
    ValueVerifier verifier;
    for (size_t i = 0; i < selected_cols.size(); ++i) {
      const Column& col = df.column(selected_cols[i]);
      const BitVector* nulls = col.null_storage.MaybeGetNullBitVector();
      if (nulls && !nulls->is_set(row)) {
        verifier.OnCell(nullptr);
      } else {
        uint32_t pos =
            IsSparse(col.null_storage.nullability()) ? sparse_pos[i]++ : row;
        VisitStorageCell(col.storage, pos, df.string_pool(), verifier);
      }
    }
    EXPECT_THAT(verifier.values, testing::ElementsAreArray(expected[row]))
        << "Mismatch in data for row " << row;
  }
}

template <typename... Args>
std::vector<ValueVerifier::ValueVariant> Row(Args&&... args) {
  return std::vector<ValueVerifier::ValueVariant>{
      ValueVerifier::ValueVariant(std::forward<Args>(args))...};
}

template <typename... Args>
std::vector<std::vector<ValueVerifier::ValueVariant>> Rows(Args&&... rows) {
  return std::vector<std::vector<ValueVerifier::ValueVariant>>{
      std::forward<Args>(rows)...};
}

}  // namespace perfetto::trace_processor::core::dataframe

#endif  // SRC_TRACE_PROCESSOR_CORE_DATAFRAME_DATAFRAME_TEST_UTILS_H_
