/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include "src/trace_processor/db/runtime_table.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "perfetto/base/logging.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/status_or.h"
#include "perfetto/trace_processor/ref_counted.h"
#include "src/trace_processor/containers/string_pool.h"
#include "src/trace_processor/db/column.h"
#include "src/trace_processor/db/column/data_layer.h"
#include "src/trace_processor/db/column/id_storage.h"
#include "src/trace_processor/db/column/null_overlay.h"
#include "src/trace_processor/db/column/numeric_storage.h"
#include "src/trace_processor/db/column/string_storage.h"
#include "src/trace_processor/db/column/types.h"
#include "src/trace_processor/db/column_storage_overlay.h"

namespace perfetto::trace_processor {
namespace {

template <typename T, typename U>
T Fill(uint32_t leading_nulls, U value) {
  T res;
  for (uint32_t i = 0; i < leading_nulls; ++i) {
    res.Append(value);
  }
  return res;
}

bool IsPerfectlyRepresentableAsDouble(int64_t res) {
  static constexpr int64_t kMaxDoubleRepresentible = 1ull << 53;
  return res >= -kMaxDoubleRepresentible && res <= kMaxDoubleRepresentible;
}

bool IsStorageNotIntNorDouble(const RuntimeTable::VariantStorage& col) {
  return std::get_if<RuntimeTable::IntStorage>(&col) == nullptr &&
         std::get_if<RuntimeTable::DoubleStorage>(&col) == nullptr;
}

}  // namespace

RuntimeTable::RuntimeTable(
    StringPool* pool,
    uint32_t row_count,
    std::vector<ColumnLegacy> columns,
    std::vector<ColumnStorageOverlay> overlays,
    std::vector<RefPtr<column::DataLayer>> storage_layers,
    std::vector<RefPtr<column::DataLayer>> null_layers,
    std::vector<RefPtr<column::DataLayer>> overlay_layers)
    : Table(pool, row_count, std::move(columns), std::move(overlays)) {
  OnConstructionCompleted(std::move(storage_layers), std::move(null_layers),
                          std::move(overlay_layers));
}

RuntimeTable::~RuntimeTable() = default;

RuntimeTable::Builder::Builder(StringPool* pool,
                               std::vector<std::string> col_names)
    : string_pool_(pool), col_names_(std::move(col_names)) {
  for (uint32_t i = 0; i < col_names_.size(); i++) {
    storage_.emplace_back(std::make_unique<VariantStorage>());
  }
}

base::Status RuntimeTable::Builder::AddNull(uint32_t idx) {
  auto* col = storage_[idx].get();
  PERFETTO_DCHECK(IsStorageNotIntNorDouble(*col));
  if (auto* leading_nulls = std::get_if<uint32_t>(col)) {
    (*leading_nulls)++;
  } else if (auto* ints = std::get_if<NullIntStorage>(col)) {
    ints->Append(std::nullopt);
  } else if (auto* strings = std::get_if<StringStorage>(col)) {
    strings->Append(StringPool::Id::Null());
  } else if (auto* doubles = std::get_if<NullDoubleStorage>(col)) {
    doubles->Append(std::nullopt);
  } else {
    PERFETTO_FATAL("Unexpected column type");
  }
  return base::OkStatus();
}

base::Status RuntimeTable::Builder::AddInteger(uint32_t idx, int64_t res) {
  auto* col = storage_[idx].get();
  PERFETTO_DCHECK(IsStorageNotIntNorDouble(*col));
  if (auto* leading_nulls_ptr = std::get_if<uint32_t>(col)) {
    *col = Fill<NullIntStorage>(*leading_nulls_ptr, std::nullopt);
  }
  if (auto* doubles = std::get_if<NullDoubleStorage>(col)) {
    if (!IsPerfectlyRepresentableAsDouble(res)) {
      return base::ErrStatus("Column %s contains %" PRId64
                             " which cannot be represented as a double",
                             col_names_[idx].c_str(), res);
    }
    doubles->Append(static_cast<double>(res));
    return base::OkStatus();
  }
  auto* ints = std::get_if<NullIntStorage>(col);
  if (!ints) {
    return base::ErrStatus("Column %s does not have consistent types",
                           col_names_[idx].c_str());
  }
  ints->Append(res);
  return base::OkStatus();
}

base::Status RuntimeTable::Builder::AddFloat(uint32_t idx, double res) {
  auto* col = storage_[idx].get();
  PERFETTO_DCHECK(IsStorageNotIntNorDouble(*col));
  if (auto* leading_nulls_ptr = std::get_if<uint32_t>(col)) {
    *col = Fill<NullDoubleStorage>(*leading_nulls_ptr, std::nullopt);
  }
  if (auto* ints = std::get_if<NullIntStorage>(col)) {
    NullDoubleStorage storage;
    for (uint32_t i = 0; i < ints->size(); ++i) {
      std::optional<int64_t> int_val = ints->Get(i);
      if (!int_val) {
        storage.Append(std::nullopt);
        continue;
      }
      if (int_val && !IsPerfectlyRepresentableAsDouble(*int_val)) {
        return base::ErrStatus("Column %s contains %" PRId64
                               " which cannot be represented as a double",
                               col_names_[idx].c_str(), *int_val);
      }
      storage.Append(static_cast<double>(*int_val));
    }
    *col = std::move(storage);
  }
  auto* doubles = std::get_if<NullDoubleStorage>(col);
  if (!doubles) {
    return base::ErrStatus("Column %s does not have consistent types",
                           col_names_[idx].c_str());
  }
  doubles->Append(res);
  return base::OkStatus();
}

base::Status RuntimeTable::Builder::AddText(uint32_t idx, const char* ptr) {
  auto* col = storage_[idx].get();
  PERFETTO_DCHECK(IsStorageNotIntNorDouble(*col));
  if (auto* leading_nulls_ptr = std::get_if<uint32_t>(col)) {
    *col = Fill<StringStorage>(*leading_nulls_ptr, StringPool::Id::Null());
  }
  auto* strings = std::get_if<StringStorage>(col);
  if (!strings) {
    return base::ErrStatus("Column %s does not have consistent types",
                           col_names_[idx].c_str());
  }
  strings->Append(string_pool_->InternString(ptr));
  return base::OkStatus();
}

base::StatusOr<std::unique_ptr<RuntimeTable>> RuntimeTable::Builder::Build(
    uint32_t rows) && {
  std::vector<RefPtr<column::DataLayer>> storage_layers(col_names_.size() + 1);
  std::vector<RefPtr<column::DataLayer>> null_layers(col_names_.size() + 1);
  std::vector<ColumnLegacy> columns;
  for (uint32_t i = 0; i < col_names_.size(); ++i) {
    auto* col = storage_[i].get();
    std::unique_ptr<column::DataLayerChain> chain;
    PERFETTO_DCHECK(IsStorageNotIntNorDouble(*col));
    if (auto* leading_nulls = std::get_if<uint32_t>(col)) {
      PERFETTO_CHECK(*leading_nulls == rows);
      *col = Fill<NullIntStorage>(*leading_nulls, std::nullopt);
    }
    if (auto* ints = std::get_if<NullIntStorage>(col)) {
      PERFETTO_CHECK(ints->size() == rows);
      // Check if the column is nullable.
      if (ints->non_null_size() == ints->size()) {
        *col = IntStorage::CreateFromAssertNonNull(std::move(*ints));
        auto* non_null_ints = std::get_if<IntStorage>(col);
        bool is_sorted = std::is_sorted(non_null_ints->vector().begin(),
                                        non_null_ints->vector().end());
        uint32_t flags = is_sorted ? ColumnLegacy::Flag::kNonNull |
                                         ColumnLegacy::Flag::kSorted
                                   : ColumnLegacy::Flag::kNonNull;
        columns.emplace_back(col_names_[i].c_str(), non_null_ints, flags, i, 0);
        storage_layers[i].reset(new column::NumericStorage<int64_t>(
            &non_null_ints->vector(), ColumnType::kInt64, is_sorted));
      } else {
        columns.emplace_back(col_names_[i].c_str(), ints,
                             ColumnLegacy::Flag::kNoFlag, i, 0);
        storage_layers[i].reset(new column::NumericStorage<int64_t>(
            &ints->non_null_vector(), ColumnType::kInt64, false));
        null_layers[i].reset(
            new column::NullOverlay(&ints->non_null_bit_vector()));
      }
    } else if (auto* strings = std::get_if<StringStorage>(col)) {
      PERFETTO_CHECK(strings->size() == rows);
      columns.emplace_back(col_names_[i].c_str(), strings,
                           ColumnLegacy::Flag::kNonNull, i, 0);
      storage_layers[i].reset(
          new column::StringStorage(string_pool_, &strings->vector()));
    } else if (auto* doubles = std::get_if<NullDoubleStorage>(col)) {
      PERFETTO_CHECK(doubles->size() == rows);
      // Check if the column is nullable.
      if (doubles->non_null_size() == doubles->size()) {
        *col = DoubleStorage::CreateFromAssertNonNull(std::move(*doubles));

        auto* non_null_doubles = std::get_if<DoubleStorage>(col);
        bool is_sorted = std::is_sorted(non_null_doubles->vector().begin(),
                                        non_null_doubles->vector().end());
        uint32_t flags = is_sorted ? ColumnLegacy::Flag::kNonNull |
                                         ColumnLegacy::Flag::kSorted
                                   : ColumnLegacy::Flag::kNonNull;
        columns.emplace_back(col_names_[i].c_str(), non_null_doubles, flags, i,
                             0);
        storage_layers[i].reset(new column::NumericStorage<double>(
            &non_null_doubles->vector(), ColumnType::kDouble, is_sorted));
      } else {
        columns.emplace_back(col_names_[i].c_str(), doubles,
                             ColumnLegacy::Flag::kNoFlag, i, 0);
        storage_layers[i].reset(new column::NumericStorage<double>(
            &doubles->non_null_vector(), ColumnType::kDouble, false));
        null_layers[i].reset(
            new column::NullOverlay(&doubles->non_null_bit_vector()));
      }
    } else {
      PERFETTO_FATAL("Unexpected column type");
    }
  }
  columns.push_back(ColumnLegacy::IdColumn(
      static_cast<uint32_t>(columns.size()), 0, "_auto_id",
      ColumnLegacy::kIdFlags | ColumnLegacy::Flag::kHidden));
  storage_layers.back().reset(new column::IdStorage());

  std::vector<ColumnStorageOverlay> overlays;
  overlays.emplace_back(rows);

  std::vector<RefPtr<column::DataLayer>> overlay_layers(1);

  auto table = std::make_unique<RuntimeTable>(
      string_pool_, rows, std::move(columns), std::move(overlays),
      std::move(storage_layers), std::move(null_layers),
      std::move(overlay_layers));
  table->storage_ = std::move(storage_);
  table->col_names_ = std::move(col_names_);

  table->schema_.columns.reserve(table->columns().size());
  for (const auto& col : table->columns()) {
    table->schema_.columns.emplace_back(
        Schema::Column{col.name(), col.type(), col.IsId(), col.IsSorted(),
                       col.IsHidden(), col.IsSetId()});
  }
  return {std::move(table)};
}

}  // namespace perfetto::trace_processor
