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
#include <cstdint>
#include <optional>
#include "perfetto/base/status.h"

namespace perfetto {
namespace trace_processor {
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

}  // namespace

RuntimeTable::RuntimeTable(StringPool* pool, std::vector<std::string> col_names)
    : Table(pool), col_names_(col_names), storage_(col_names_.size()) {
  for (uint32_t i = 0; i < col_names.size(); i++)
    storage_[i] = std::make_unique<VariantStorage>();
}

RuntimeTable::~RuntimeTable() = default;

base::Status RuntimeTable::AddNull(uint32_t idx) {
  auto* col = storage_[idx].get();
  if (auto* leading_nulls = std::get_if<uint32_t>(col)) {
    (*leading_nulls)++;
  } else if (auto* ints = std::get_if<IntStorage>(col)) {
    ints->Append(std::nullopt);
  } else if (auto* strings = std::get_if<StringStorage>(col)) {
    strings->Append(StringPool::Id::Null());
  } else if (auto* doubles = std::get_if<DoubleStorage>(col)) {
    doubles->Append(std::nullopt);
  } else {
    PERFETTO_FATAL("Unexpected column type");
  }
  return base::OkStatus();
}

base::Status RuntimeTable::AddInteger(uint32_t idx, int64_t res) {
  auto* col = storage_[idx].get();
  if (auto* leading_nulls_ptr = std::get_if<uint32_t>(col)) {
    *col = Fill<IntStorage>(*leading_nulls_ptr, std::nullopt);
  }
  if (auto* doubles = std::get_if<DoubleStorage>(col)) {
    if (!IsPerfectlyRepresentableAsDouble(res)) {
      return base::ErrStatus("Column %s contains %" PRId64
                             " which cannot be represented as a double",
                             col_names_[idx].c_str(), res);
    }
    doubles->Append(static_cast<double>(res));
    return base::OkStatus();
  }
  auto* ints = std::get_if<IntStorage>(col);
  if (!ints) {
    return base::ErrStatus("Column %s does not have consistent types",
                           col_names_[idx].c_str());
  }
  ints->Append(res);
  return base::OkStatus();
}

base::Status RuntimeTable::AddFloat(uint32_t idx, double res) {
  auto* col = storage_[idx].get();
  if (auto* leading_nulls_ptr = std::get_if<uint32_t>(col)) {
    *col = Fill<DoubleStorage>(*leading_nulls_ptr, std::nullopt);
  }
  if (auto* ints = std::get_if<IntStorage>(col)) {
    DoubleStorage storage;
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
  auto* doubles = std::get_if<DoubleStorage>(col);
  if (!doubles) {
    return base::ErrStatus("Column %s does not have consistent types",
                           col_names_[idx].c_str());
  }
  doubles->Append(res);
  return base::OkStatus();
}

base::Status RuntimeTable::AddText(uint32_t idx, const char* ptr) {
  auto* col = storage_[idx].get();
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

base::Status RuntimeTable::AddColumnsAndOverlays(uint32_t rows) {
  overlays_.push_back(ColumnStorageOverlay(rows));
  for (uint32_t i = 0; i < col_names_.size(); ++i) {
    auto* col = storage_[i].get();
    if (auto* leading_nulls = std::get_if<uint32_t>(col)) {
      PERFETTO_CHECK(*leading_nulls == rows);
      *col = Fill<IntStorage>(*leading_nulls, std::nullopt);
    }
    if (auto* ints = std::get_if<IntStorage>(col)) {
      PERFETTO_CHECK(ints->size() == rows);
      columns_.push_back(Column(col_names_[i].c_str(), ints,
                                Column::Flag::kNoFlag, this, i, 0));
    } else if (auto* strings = std::get_if<StringStorage>(col)) {
      PERFETTO_CHECK(strings->size() == rows);
      columns_.push_back(Column(col_names_[i].c_str(), strings,
                                Column::Flag::kNonNull, this, i, 0));
    } else if (auto* doubles = std::get_if<DoubleStorage>(col)) {
      PERFETTO_CHECK(doubles->size() == rows);
      columns_.push_back(Column(col_names_[i].c_str(), doubles,
                                Column::Flag::kNoFlag, this, i, 0));
    } else {
      PERFETTO_FATAL("Unexpected column type");
    }
  }
  columns_.push_back(
      Column::IdColumn(this, static_cast<uint32_t>(col_names_.size()), 0,
                       "_auto_id", Column::kIdFlags | Column::Flag::kHidden));
  row_count_ = rows;
  return base::OkStatus();
}

}  // namespace trace_processor
}  // namespace perfetto
